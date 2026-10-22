/*
 * A Markdown file in a tab of its own: its text - a text document, edited
 * and saved as any other - and the text rendered (headings, emphasis,
 * tables, code on a shade, math typeset), beside it or instead of it; the
 * rendering follows the edits; its links lead to their heading, or to the
 * document they name; the mode chosen is kept for the next file.
 */
#include <QtTest>
#include <QElapsedTimer>
#include <QClipboard>
#include <QGuiApplication>
#include <QAbstractButton>
#include <QDesktopServices>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QRandomGenerator>
#include <QSet>
#include <QPainter>
#include <QPicture>
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextList>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextTable>
#include <QWindow>

#include "gpurendering.h"
#ifdef QUCS_GPU_VIEW
#include "rhicanvas.h"
#endif
#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "apptheme.h"
#include "markdowndoc.h"
#include "mathtypeset.h"
#include "settings.h"
#include "syntax.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

void pump()
{
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
}

// The time a viewport's paint takes, its painting sent on and timed.
class PaintTimer : public QObject
{
public:
    explicit PaintTimer(QWidget* viewport) : QObject(viewport) { viewport->installEventFilter(this); }
    qint64 worst = 0;   // µs
    int paints = 0;
    QList<qint64> times;   // µs, each paint's
    qint64 median() const
    {
        QList<qint64> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        return sorted.isEmpty() ? 0 : sorted.at(sorted.size() / 2);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::Paint || a_inside) return false;
        a_inside = true;
        QElapsedTimer t;
        t.start();
        QCoreApplication::sendEvent(watched, event);
        times << t.nsecsElapsed() / 1000;
        worst = std::max(worst, times.last());
        ++paints;
        a_inside = false;
        return true;
    }

private:
    bool a_inside = false;
};

// The blocks of a rendering whose text is \a text.
QTextBlock blockWith(QTextDocument* doc, const QString& text)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.text().contains(text)) return b;
    return {};
}

const char* kReadme =
    "# The Amplifier\n"
    "\n"
    "It has a **gain** of 20 dB, see [the other](other.md) and [below](#section-two).\n"
    "\n"
    "| Part | Value |\n"
    "|------|-------|\n"
    "| R1   | 1k    |\n"
    "\n"
    "```\n"
    "plot v(out)\n"
    "```\n"
    "\n"
    "The gain is $A = \\frac{R_2}{R_1}$.\n";
// Collects what is handed to the system (QDesktopServices).
class Handed : public QObject
{
    Q_OBJECT
public slots:
    void open(const QUrl& url) { urls << url; }

public:
    QList<QUrl> urls;
};

// Answers the link question with the button named \a button (empty:
// Cancel); what it said, and whether it was asked.
struct LinkAnswer {
    bool asked = false;
    QString said;
};
LinkAnswer answeringLink(const std::function<void()>& fn, const QString& button = QString())
{
    LinkAnswer a;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr || !box->isVisible()) return;
        a.asked = true;
        a.said = box->text() + QLatin1Char('\n') + box->informativeText();
        if (box->objectName() == "linkQuestion" && !button.isEmpty()) box->findChild<QPushButton*>(button)->click();
        else box->button(QMessageBox::Cancel) != nullptr ? box->button(QMessageBox::Cancel)->click() : box->reject();
    });
    timer.start(20);
    fn();
    return a;
}

} // namespace

class TestMarkdownDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QByteArray& bytes)
    {
        QFile f(dir.filePath(name));
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        return f.fileName();
    }

    MarkdownDoc* current(QucsApp& app)
    {
        return qobject_cast<MarkdownDoc*>(app.DocumentTab->currentWidget());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void anchorsAreGitHubs()
    {
        QCOMPARE(qucs_s::markdown::anchorOf("Getting Started!"), QString("getting-started"));
        QCOMPARE(qucs_s::markdown::anchorOf("Section Two"), QString("section-two"));
        QCOMPARE(qucs_s::markdown::anchorOf(" R_1 and R-2 "), QString("r_1-and-r-2"));
    }

    // Headings, emphasis, a table, code on a shade, math typeset.
    void theRenderingHasTheMarkdown()
    {
        QTextDocument doc;
        qucs_s::math::MathObject::install(&doc);
        qucs_s::markdown::render(&doc, kReadme, QFont(), {});
        const QTextBlock title = blockWith(&doc, "The Amplifier");
        QVERIFY(title.isValid());
        QCOMPARE(title.blockFormat().headingLevel(), 1);
        bool bold = false;
        const QTextBlock line = blockWith(&doc, "gain");
        for (QTextBlock::iterator it = line.begin(); !it.atEnd(); ++it)
            if (it.fragment().text() == "gain") bold = it.fragment().charFormat().fontWeight() >= QFont::Bold;
        QVERIFY(bold);
        bool table = false;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            if (QTextCursor(b).currentTable() != nullptr) table = true;
        QVERIFY(table);
        const QTextBlock code = blockWith(&doc, "plot v(out)");
        QVERIFY(code.isValid());
        QVERIFY(code.blockFormat().background().style() != Qt::NoBrush);
        const QTextBlock math = blockWith(&doc, "The gain is");
        bool typeset = false;
        for (QTextBlock::iterator it = math.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().objectType() == qucs_s::math::MathObject::Type) typeset = true;
        QVERIFY(typeset);
        QVERIFY(!doc.toPlainText().contains("frac"));
    }

    // A Markdown file with HTML Qt's importer saw opened and never closed -
    // a <br> in a table's cell, an <hr>, an <img>, a placeholder <name>, a
    // vector<int>, a lone <b>, a "/>" in an HTML block, a tag in CDATA, an
    // alt="a>b" - is rendered whole: the importer dropped everything after
    // the first (the review of 5 October, its re-checks). An HTML block is a
    // block of its own: centred badges after a heading were in it; one in a
    // list's item is under its bullet.
    void anUnclosedTagKeepsTheRest()
    {
        QTextDocument doc;
        qucs_s::math::MathObject::install(&doc);
        qucs_s::markdown::render(&doc, "| A | B |\n|---|---|\n| one<br>two | `x` |\n| row2 | y |\n\nAfter the table:\n\n"
                                       "- **Bold item** with text\n- plain item and `code`\n\nA rule<HR>and <img src=\"a.png\"> more, `<br>` as code.\n\n"
                                       "1. The dataset is <name>.dat.ngspice beside the schematic.\n2. still here?\n\nA vector<int> and a lone <b>.\n\n"
                                       "<!-- <img src=\"badge.svg\"> a README's badge, commented out -->\n\n"
                                       "<div>a</div>/>\n\nAfter a stray close.\n\n<![CDATA[\nx <b>\n]]>\n\n"
                                       "After CDATA, <img alt=\"a>b\" src=\"n.png\"> inline.\n\n## Badges\n\n<p align=\"center\">\n<img src=\"b.png\"/>\n</p>\n\n"
                                       "- item\n\n  <div>Under its bullet.</div>\n\n"
                                       "1. Build it:\n\n       make <target> && ls <dir>\n\n> Note: <b>a < b</b>, see [the docs](<docs/a b.md>).\n\n"
                                       "THE END\n",
                                 QFont(), {});
        const QString text = doc.toPlainText();
        // The rule between the paragraph's parts, its block empty.
        bool ruleBetween = false;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            if (b.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth))
                ruleBetween = b.text().isEmpty() && b.previous().text() == "A rule" && b.next().text().startsWith("and");
        QVERIFY(ruleBetween);
        for (const char* kept : {"one", "two", "row2", "After the table:", "Bold item with text", "plain item and code", "more, <br> as code.",
                                 "The dataset is <name>.dat.ngspice beside the schematic.", "still here?", "A vector<int> and a lone <b>.",
                                 "After a stray close.", "After CDATA,", "inline.", "make <target> && ls <dir>", "Note: a < b, see the docs.",
                                 "THE END"})
            QVERIFY2(text.contains(QString::fromUtf8(kept)), qPrintable(QString(kept) + "\n---\n" + text));
        const QTextBlock badges = blockWith(&doc, "Badges");
        QCOMPARE(badges.text(), QString("Badges"));
        QVERIFY(badges.next().text().startsWith(QChar::ObjectReplacementCharacter));
        QVERIFY(badges.next().blockFormat().alignment() & Qt::AlignHCenter);
        QCOMPARE(blockWith(&doc, "Under its bullet.").blockFormat().indent(), 1);
    }

    // A .md file opens in a Markdown document: a text document (highlighted
    // as Markdown), shown rendered at first.
    void aMarkdownFileOpensInItsOwnTab()
    {
        const QString readme = write("README.md", kReadme);
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(readme));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        QVERIFY(QucsApp::isTextDocument(md));
        QCOMPARE(md->language, int(LANG_MARKDOWN));
        QCOMPARE(md->mode(), MarkdownDoc::Mode::Preview);   // the default
        QVERIFY(md->isReadOnly());                            // not edited unseen
        QVERIFY(blockWith(md->preview()->document(), "The Amplifier").isValid());
        QCOMPARE(md->toPlainText(), QString(kReadme));
        app.closeAllFiles();
    }

    // Edit, Split, Preview: the text, the rendering, or both side by side;
    // the one chosen is kept for the next file.
    void theModesShowTheTextTheRenderingOrBoth()
    {
        const QString readme = write("modes.md", kReadme);
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(readme));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        pump();

        auto* split = md->findChild<QAbstractButton*>("markdownSplit");
        QVERIFY(split != nullptr);
        split->click();
        pump();
        QCOMPARE(md->mode(), MarkdownDoc::Mode::Split);
        QVERIFY(!md->isReadOnly());
        QVERIFY(md->preview()->isVisible());
        QVERIFY(md->findChild<QWidget*>("markdownHandle")->isVisible());
        QVERIFY(md->findChild<QScrollBar*>("markdownTextScroll")->isVisible());
        // The text on the left, the rendering on the right, side by side.
        const QRect text = md->viewport()->geometry();
        const QRect rendering = md->preview()->geometry();
        QVERIFY(text.width() > 100);
        QVERIFY(rendering.left() > text.right());
        QVERIFY(qAbs(text.top() - rendering.top()) <= 1);
        QCOMPARE(MarkdownDoc::defaultMode(), MarkdownDoc::Mode::Split);   // kept

        // The handle shares the width.
        const int before = text.width();
        md->setShare(0.3);
        pump();
        QVERIFY(md->viewport()->geometry().width() < before);
        QVERIFY(md->preview()->geometry().width() > rendering.width());

        md->findChild<QAbstractButton*>("markdownEdit")->click();
        pump();
        QCOMPARE(md->mode(), MarkdownDoc::Mode::Edit);
        QVERIFY(!md->preview()->isVisible());
        QVERIFY(md->viewport()->geometry().width() > rendering.width());

        md->findChild<QAbstractButton*>("markdownPreview")->click();
        pump();
        QVERIFY(md->preview()->isVisible());
        QVERIFY(md->isReadOnly());
        QCOMPARE(MarkdownDoc::defaultMode(), MarkdownDoc::Mode::Preview);
        app.closeAllFiles();
    }

    // Images no wider than the rendering: a wide one scaled down to its
    // room, its proportions kept, and up again as the tab widens - never
    // past its own size; a small one as it is; one in a list within the
    // list's indent; an HTML <img>'s width the most. Nothing past the tab's
    // edge (the user's report: they drew at their own size, past it).
    void imagesFitTheRendering()
    {
        QImage wide(1600, 400, QImage::Format_RGB32), small(60, 20, QImage::Format_RGB32);
        wide.fill(Qt::darkCyan);
        small.fill(Qt::darkRed);
        QVERIFY(wide.save(dir.filePath("wide.png")) && small.save(dir.filePath("small.png")));
        const QString file = write("images.md", "# Pictures\n\n![wide](wide.png)\n\n![small](small.png)\n\n"
                                                "<img src=\"wide.png\" width=\"500\">\n\n- an item\n\n  ![listed](wide.png)\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        pump();
        // The images in order: name, size drawn, the room of their block.
        struct Drawn {
            QString name;
            QSizeF size;
            qreal room;
        };
        auto drawn = [md] {
            QTextDocument* doc = md->preview()->document();
            QList<Drawn> out;
            for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
                const QTextBlockFormat bf = b.blockFormat();
                qreal room = md->preview()->viewport()->width() - 2 * doc->documentMargin() - bf.leftMargin() - bf.rightMargin()
                           - bf.indent() * doc->indentWidth();
                if (b.textList() != nullptr) room -= b.textList()->format().indent() * doc->indentWidth();
                for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
                    if (it.fragment().charFormat().isImageFormat()) {
                        const QTextImageFormat f = it.fragment().charFormat().toImageFormat();
                        out.append({f.name(), QSizeF(f.width(), f.height()), room});
                    }
            }
            return out;
        };
        QList<Drawn> images = drawn();
        QCOMPARE(images.size(), 4);
        // (Drawn smoothly, its math at the screen's pixels: SmoothImages.)
        QVERIFY(dynamic_cast<qucs_s::markdown::SmoothImages*>(
                    md->preview()->document()->documentLayout()->handlerForObject(QTextFormat::ImageObject)) != nullptr);
        const qreal roomWide = images[0].room;
        QVERIFY2(images[0].name == "wide.png" && images[0].size.width() <= roomWide && images[0].size.width() > roomWide - 2,
                 qPrintable(QString("%1 in %2").arg(images[0].size.width()).arg(roomWide)));
        QVERIFY(qAbs(images[0].size.height() - images[0].size.width() / 4) < 0.01);
        QCOMPARE(images[1].size, QSizeF(60, 20));
        QCOMPARE(images[2].size, QSizeF(500, 125));
        QVERIFY2(images[3].size.width() <= images[3].room && images[3].room < roomWide,
                 qPrintable(QString("%1 in %2").arg(images[3].size.width()).arg(images[3].room)));
        QVERIFY(md->preview()->document()->idealWidth() <= md->preview()->viewport()->width() + 1);
        // (No horizontal bar at the tab's foot: the rendering's nor the hidden
        // text's.)
        QVERIFY(!md->preview()->horizontalScrollBar()->isVisible() && !md->horizontalScrollBar()->isVisible());
        // Narrower: the HTML one's 500 too wide now, all within.
        app.resize(500, 800);
        pump();
        images = drawn();
        QVERIFY(images[0].size.width() < roomWide && images[0].size.width() <= images[0].room);
        QVERIFY(images[2].size.width() < 500 && images[2].size.width() <= images[2].room);
        QVERIFY(qAbs(images[2].size.height() - images[2].size.width() / 4) < 0.01);
        QCOMPARE(images[1].size, QSizeF(60, 20));
        QVERIFY2(md->preview()->document()->idealWidth() <= md->preview()->viewport()->width() + 1,
                 qPrintable(QString("%1 in %2").arg(md->preview()->document()->idealWidth()).arg(md->preview()->viewport()->width())));
        // Wider again: back up, the HTML one at its 500.
        app.resize(1200, 800);
        pump();
        images = drawn();
        QVERIFY(images[0].size.width() > roomWide - 2);
        QCOMPARE(images[2].size, QSizeF(500, 125));
        app.closeAllFiles();
    }

    // Images in a table's cells fit their cells (they were fitted to the
    // whole width: a table with one ran past the rendering's edge, its
    // pictures not shrinking with it): a column of text its words' width,
    // the image's column the rest; two images alike; at any width, the
    // table within the rendering, each image within its cell.
    void imagesInATableFitTheirCells()
    {
        QImage wide(1600, 400, QImage::Format_RGB32);
        wide.fill(Qt::darkCyan);
        QVERIFY(wide.save(dir.filePath("tablewide.png")));
        const QString file = write("tables.md", "| Plot | Notes |\n|------|-------|\n| ![one](tablewide.png) | a few words |\n\n"
                                                "| ![two](tablewide.png) | ![three](tablewide.png) |\n|---|---|\n| x | y |\n\n"
                                                "| ![four](tablewide.png) | A longer note on the plot beside it, which wraps in its cell "
                                                "as the table narrows: it is text, and has the room the picture leaves. |\n|---|---|\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        pump();
        // Each image: its size, where it starts and where the next cell of
        // its row starts (the rendering's right margin for the last).
        struct Drawn {
            QSizeF size;
            qreal left, next;
        };
        const auto drawn = [md] {
            QTextDocument* doc = md->preview()->document();
            QList<Drawn> out;
            for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
                for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
                    if (it.fragment().charFormat().isImageFormat()) {
                        const QTextImageFormat f = it.fragment().charFormat().toImageFormat();
                        qreal next = md->preview()->viewport()->width() - doc->documentMargin();
                        if (QTextTable* t = QTextCursor(b).currentTable()) {
                            const QTextTableCell cell = t->cellAt(b.position());
                            const QTextTableCell after = t->cellAt(cell.row(), cell.column() + cell.columnSpan());
                            if (after.isValid()) next = doc->documentLayout()->blockBoundingRect(doc->findBlock(after.firstPosition())).left();
                        }
                        out.append({QSizeF(f.width(), f.height()), doc->documentLayout()->blockBoundingRect(b).left(), next});
                    }
            return out;
        };
        for (const int width : {1400, 900, 700, 450, 1400}) {
            app.resize(width, 800);
            pump();
            const QString at = QStringLiteral("at %1: ").arg(width);
            const QList<Drawn> images = drawn();
            QCOMPARE(images.size(), 4);
            const qreal room = md->preview()->viewport()->width() - 2 * md->preview()->document()->documentMargin();
            QVERIFY2(md->preview()->document()->idealWidth() <= md->preview()->viewport()->width() + 1,
                     qPrintable(at + QString("%1 in %2").arg(md->preview()->document()->idealWidth()).arg(md->preview()->viewport()->width())));
            QVERIFY(!md->preview()->horizontalScrollBar()->isVisible());
            // (Each in its cell: Qt's layout keeps a table to the width and
            // squeezes its columns - an image wider than its cell ran over
            // the next one, past the edge.)
            for (const Drawn& d : images) {
                QVERIFY2(d.left + d.size.width() <= d.next + 0.5,
                         qPrintable(at + QString("%1 from %2, the next cell at %3").arg(d.size.width()).arg(d.left).arg(d.next)));
                QVERIFY(qAbs(d.size.height() - d.size.width() / 4) < 0.01);
            }
            // The picture beside a few words has most of the room; two side
            // by side the same; beside a long note, room still. (At 450 the
            // rendering is a hundred pixels: a picture beside words there
            // has what the words leave.)
            qInfo("%s room %.0f: %.0f, %.0f and %.0f, %.0f", qPrintable(at), room, images[0].size.width(), images[1].size.width(),
                  images[2].size.width(), images[3].size.width());
            if (room < 300) continue;
            QVERIFY2(images[0].size.width() > 0.6 * room, qPrintable(at + QString("%1 of %2").arg(images[0].size.width()).arg(room)));
            QVERIFY(qAbs(images[1].size.width() - images[2].size.width()) < 1);
            QVERIFY2(images[1].size.width() > 0.4 * room, qPrintable(at + QString("%1 of %2").arg(images[1].size.width()).arg(room)));
            QVERIFY2(images[3].size.width() > 0.3 * room, qPrintable(at + QString("%1 of %2").arg(images[3].size.width()).arg(room)));
        }
        app.closeAllFiles();
    }

    // Images are drawn smoothly scaled - an average of the pixels each
    // takes in - with the device's pixels (Qt's own took the nearest pixel
    // of the picture for each: a plot's lines and letters lost, scaled
    // down); one at its own size pixel for pixel.
    void imagesAreDrawnSmoothly()
    {
        QImage stripes(1200, 40, QImage::Format_RGB32);
        for (int x = 0; x < stripes.width(); ++x)
            for (int y = 0; y < stripes.height(); ++y) stripes.setPixel(x, y, x % 2 == 0 ? 0xff000000 : 0xffffffff);
        QImage dot(60, 20, QImage::Format_RGB32);
        for (int x = 0; x < dot.width(); ++x)
            for (int y = 0; y < dot.height(); ++y) dot.setPixel(x, y, qRgb(4 * x, 12 * y, 200));
        // A document of the stripes (fitted to 400: a third) and the dot,
        // drawn at a device pixel ratio; the share of grey pixels across the
        // stripes, and whether the dot is its own pixels.
        struct Seen {
            double grey = 0;
            bool dotExact = false;
        };
        const auto draw = [&stripes, &dot](bool smooth, qreal dpr) {
            QTextDocument doc;
            doc.setDocumentMargin(10);
            if (smooth) qucs_s::markdown::SmoothImages::install(&doc);
            qucs_s::markdown::render(&doc, "![s](stripes.png)\n\n![d](dot.png)\n", QFont(), {});
            doc.addResource(QTextDocument::ImageResource, QUrl("stripes.png"), stripes);
            doc.addResource(QTextDocument::ImageResource, QUrl("dot.png"), dot);
            qucs_s::markdown::fitImages(&doc, 420);
            doc.setTextWidth(420);
            QImage out(QSize(420, int(doc.size().height()) + 2) * dpr, QImage::Format_RGB32);
            out.setDevicePixelRatio(dpr);
            out.fill(Qt::white);
            QPainter p(&out);
            p.translate(0, 0.37);   // (a fraction of a pixel down: on whole pixels all the same)
            doc.drawContents(&p);
            p.end();
            Seen seen;
            const QRectF first = doc.documentLayout()->blockBoundingRect(doc.begin());
            const int y = int((first.top() + 10) * dpr);
            int grey = 0, all = 0;
            for (int x = int(15 * dpr); x < int(395 * dpr); ++x, ++all) {
                const int v = qGray(out.pixel(x, y));
                grey += v > 25 && v < 230;
            }
            seen.grey = double(grey) / all;
            // The dot: at its own size where the device is 1:1.
            const QRectF second = doc.documentLayout()->blockBoundingRect(doc.begin().next());
            const QPoint at(int(std::round((second.left() + 0) * dpr)), int(std::round((second.top() + 0.37) * dpr)));
            int exact = 0;
            for (int x = 0; x < 60; ++x)
                for (int y2 = 0; y2 < 20; ++y2) exact += out.pixel(at.x() + x, at.y() + y2) == dot.pixel(x, y2);
            seen.dotExact = exact == 60 * 20;
            return seen;
        };
        const Seen qts = draw(false, 1.0), ours = draw(true, 1.0), retina = draw(true, 2.0);
        QVERIFY2(qts.grey < 0.05, qPrintable(QString::number(qts.grey)));   // (Qt's: black or white, a stripe in three)
        QVERIFY2(ours.grey > 0.95, qPrintable(QString::number(ours.grey)));
        QVERIFY2(retina.grey > 0.9, qPrintable(QString::number(retina.grey)));
        QVERIFY(ours.dotExact);
        // A thin picture made 20000 wide by its author, fitted: millions of
        // pixels tall, drawn where it is seen (it was scaled whole - 12 GB,
        // nothing drawn, seconds a frame: the fuzzing of 2026-10-16).
        {
            QImage thin(1, 20000, QImage::Format_RGB32);
            thin.fill(qRgb(200, 0, 0));
            QTextDocument doc;
            doc.setDocumentMargin(10);
            qucs_s::markdown::SmoothImages::install(&doc);
            qucs_s::markdown::render(&doc, "<img src=\"thin.png\" width=\"20000\">", QFont(), {});
            doc.addResource(QTextDocument::ImageResource, QUrl("thin.png"), thin);
            qucs_s::markdown::fitImages(&doc, 420);
            doc.setTextWidth(420);
            QVERIFY(doc.size().height() > 1e6);
            QImage out(400, 400, QImage::Format_RGB32);
            out.fill(Qt::white);
            QElapsedTimer drawing;
            drawing.start();
            QPainter p(&out);
            doc.drawContents(&p, QRectF(0, 0, 400, 400));
            p.end();
            // (Whole, 2 to 4 s here; seen, some milliseconds.)
            QVERIFY2(drawing.elapsed() < 1000, qPrintable(QString::number(drawing.elapsed())));
            QCOMPARE(QColor(out.pixel(200, 200)), QColor(200, 0, 0));
        }
        // On paper (no pixels of its own, a picture): Qt's, the image whole.
        QTextDocument doc;
        qucs_s::markdown::SmoothImages::install(&doc);
        qucs_s::markdown::render(&doc, "![s](stripes.png)", QFont(), {});
        doc.addResource(QTextDocument::ImageResource, QUrl("stripes.png"), stripes);
        qucs_s::markdown::fitImages(&doc, 420);
        QPicture picture;
        QPainter p(&picture);
        doc.drawContents(&p);
        p.end();
        QVERIFY(picture.size() > 0);
    }

    // The rendering's paper and ink: black on white under the platform's
    // themes, light or dark, as the editor's; a designed theme's own,
    // followed as the theme changes with the preview open (its document
    // drawn again in them). Set as a style sheet, as the editor's: the main
    // window's style sheet put the application's palette back on the
    // preview it polished - in a dark theme its viewport the dark base
    // under the black ink. The GPU's preview the same, where there is one.
    void thePaperIsTheThemesNotThePlatforms()
    {
        struct ThemeGuard {
            int theme = QucsSettings.Theme;
            ~ThemeGuard()
            {
                QucsSettings.Theme = theme;
                qucs_s::apptheme::apply(theme);
            }
        } themeGuard;
        const QString file = write("paper.md", "# Paper\n\nSome text, then a quote.\n\n> Quoted.\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        app.applyTheme(qucs_s::apptheme::System);
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        QTextBrowser* view = md->preview();
        const auto check = [&](const QString& what) {
            pump();
            QTest::qWait(30);
            const auto [paper, ink] = TextDoc::paperAndInk();
            const QImage shot = view->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const qreal r = shot.devicePixelRatio();
            // Below the document: the paper.
            const QColor below = shot.pixelColor(int((view->viewport()->width() - 6) * r), int((view->viewport()->height() - 6) * r));
            QVERIFY2(below.name() == paper.name(), qPrintable(QStringLiteral("%1: %2 under the document, the paper %3").arg(what, below.name(), paper.name())));
            // A line of text: its glyphs' cores the ink.
            const QTextBlock line = blockWith(view->document(), "Some text");
            QVERIFY(line.isValid());
            const QRectF box = view->document()->documentLayout()->blockBoundingRect(line).translated(0, -view->verticalScrollBar()->value());
            int nearest = 1000;
            for (int y = int(box.top() * r); y < int(box.bottom() * r) && y < shot.height(); ++y)
                for (int x = int(box.left() * r); x < int(box.right() * r) && x < shot.width(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    nearest = std::min(nearest, std::abs(c.red() - ink.red()) + std::abs(c.green() - ink.green()) + std::abs(c.blue() - ink.blue()));
                }
            QVERIFY2(nearest < 60, qPrintable(QStringLiteral("%1: the text %2 from its ink %3").arg(what).arg(nearest).arg(ink.name())));
            // What the bars and the selections are drawn in.
            QCOMPARE(view->palette().color(QPalette::Base).name(), paper.name());
            QCOMPARE(view->palette().color(QPalette::Text).name(), ink.name());
        };
        check(QStringLiteral("System"));
        if (QTest::currentTestFailed()) return;
        app.applyTheme(qucs_s::apptheme::Dark);
        QVERIFY(qucs_s::apptheme::isDark());
        check(QStringLiteral("Dark"));
        if (QTest::currentTestFailed()) return;
        app.applyTheme(qucs_s::apptheme::Nord);
        QVERIFY(qucs_s::apptheme::designedInUse());
        check(QStringLiteral("Nord"));
        if (QTest::currentTestFailed()) return;
        app.applyTheme(qucs_s::apptheme::Daylight);
        check(QStringLiteral("Daylight"));
        if (QTest::currentTestFailed()) return;
        app.applyTheme(qucs_s::apptheme::Dark);
        check(QStringLiteral("Dark again"));
        if (QTest::currentTestFailed()) return;
        // Opened in a dark designed theme.
        app.applyTheme(qucs_s::apptheme::Dracula);
        const QString other = write("paper2.md", "# Again\n\nSome text in Dracula.\n");
        QVERIFY(app.gotoPage(other));
        md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        view = md->preview();
        check(QStringLiteral("Dracula"));
        if (QTest::currentTestFailed()) return;
        // The GPU's preview.
        if (qucs_s::gpu::hindrance().isEmpty()) {
            md->setPreviewGpu(true);
            QVERIFY(md->previewGpu());
            check(QStringLiteral("Dracula on the GPU"));
            if (QTest::currentTestFailed()) return;
            app.applyTheme(qucs_s::apptheme::Dark);
            check(QStringLiteral("Dark on the GPU"));
            if (QTest::currentTestFailed()) return;
            md->setPreviewGpu(false);
            check(QStringLiteral("Dark back on the CPU"));
            if (QTest::currentTestFailed()) return;
        }
        app.applyTheme(qucs_s::apptheme::System);
        check(QStringLiteral("System again"));
        if (QTest::currentTestFailed()) return;
        app.closeAllFiles();
    }

    // A block quote marked by a bar beside it, a level's each - Qt's
    // Markdown sets it in by its margin alone (the update check of
    // 7451b1c): in the viewer, drawn over what the view paints (under it, the
    // document's background covered it).
    void quotesHaveTheirBar()
    {
        const QString file = write("quotes.md", "Plain.\n\n> Quoted,\n>\n> two paragraphs.\n\n>> Nested.\n\nAfter.\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        pump();
        QTextBrowser* view = md->preview();
        const QList<QRectF> bars = qucs_s::markdown::QuoteBars::bars(view);
        QCOMPARE(int(bars.size()), 4);   // (two paragraphs a bar each, the nested two)
        // One bar through the quote's paragraphs: the first down to the second.
        QVERIFY2(std::abs(bars[0].bottom() - bars[1].top()) < 1, qPrintable(QStringLiteral("%1 %2").arg(bars[0].bottom()).arg(bars[1].top())));
        const QImage shot = view->viewport()->grab().toImage();
        const QColor background = view->palette().color(QPalette::Base);
        for (const QRectF& bar : bars) {
            const QPoint at = (bar.center() * shot.devicePixelRatio()).toPoint(), beside = ((bar.center() + QPointF(8, 0)) * shot.devicePixelRatio()).toPoint();
            QVERIFY2(bar.left() > 0 && qAbs(qGray(shot.pixel(at)) - qGray(background.rgb())) > 20,
                     qPrintable(QStringLiteral("%1 at %2: %3").arg(bar.left()).arg(bar.top()).arg(qGray(shot.pixel(at)))));
            QVERIFY(qAbs(qGray(shot.pixel(beside)) - qGray(background.rgb())) < 5);
            // (The quote's text drawn beside it: the view's own painting
            // sent on, the bars over it.)
            bool text = false;
            const qreal r = shot.devicePixelRatio();
            for (int y = int(bar.top() * r); y < int(bar.bottom() * r) && !text; ++y)
                for (int x = int((bar.right() + 4) * r); x < shot.width() && !text; ++x)
                    text = qAbs(qGray(shot.pixel(x, y)) - qGray(background.rgb())) > 100;
            QVERIFY2(text, qPrintable(QStringLiteral("no text beside the bar at %1").arg(bar.top())));
        }
        app.closeAllFiles();
    }

    // Drawn by the GPU as by the CPU (gpurendering.h: an RHI viewport, the
    // document drawn as QTextEdit draws it in tiles, PreviewTiles): headings, a
    // table, code, math, a quote's bars, an image, a selection, scrolled; a
    // document shorter than the view; what the screen shows the grab's
    // picture; the setting switching the preview open; a scroll's paint
    // timed each way.
    void theGpuDrawsAsTheCpu()
    {
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        // (The GPU's viewport its own: never painted by QPainter - the
        // area's painting, as it is switched in and shown.)
        QTest::failOnWarning(QRegularExpression(QStringLiteral("paintEngine|Paint device returned engine")));
        QImage picture(240, 80, QImage::Format_RGB32);
        picture.fill(qRgb(30, 120, 200));
        QVERIFY(picture.save(dir.filePath("picture.png")));
        QByteArray text = kReadme;
        text += "\n> A quote,\n>\n>> nested.\n\n![a picture](picture.png)\n\n- one\n- two\n";
        for (int i = 0; i < 40; ++i) text += "\nLine " + QByteArray::number(i) + " of the tail, long enough to wrap at the preview's width or near it.\n";
        const QString file = write("gpu.md", text);
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        QTextBrowser* view = md->preview();
        // (An odd width: at a ratio of 1.5 a fractional number of device
        // pixels - the GPU's mapping to its framebuffer's pixels tried.)
        if (view->viewport()->width() % 2 == 0) {
            app.resize(app.width() + 1, app.height());
            QTRY_VERIFY(view->viewport()->width() % 2 == 1);
        }
        QVERIFY(!md->previewGpu());   // (the CPU's unless chosen)
        QVERIFY(!qucs_s::gpu::wanted(qucs_s::gpu::Viewer::Markdown));
        // (Two grabs alike: what a timer renders again - math, images -
        // done.)
        const auto shownOf = [](QTextBrowser* v) {
            QImage last;
            for (int k = 0; k < 100; ++k) {
                QTest::qWait(40);
                const QImage now = v->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
                if (!last.isNull() && now == last) return now;
                last = now;
            }
            return last;
        };
        const auto shown = [&shownOf, view] { return shownOf(view); };
        // What the screen shows, read before a grab paints the framebuffer
        // again: alike twice, the window having painted it.
        const auto onScreen = [](QTextBrowser* v) {
            QImage last;
            for (int k = 0; k < 100; ++k) {
                QTest::qWait(40);
                const QImage now = qucs_s::gpu::shownFrame(v);
                if (k >= 3 && !last.isNull() && now == last) return now;
                last = now;
            }
            return last;
        };
        const QString shots = qEnvironmentVariable("QUCS_TEST_SHOTS");
        for (const QString& kind : {QStringLiteral("plain"), QStringLiteral("selected"), QStringLiteral("scrolled")}) {
            if (kind == "selected") {
                QTextCursor c(view->document());
                c.setPosition(4);
                c.setPosition(60, QTextCursor::KeepAnchor);
                view->setTextCursor(c);
            }
            if (kind == "scrolled") {
                QVERIFY(view->verticalScrollBar()->maximum() > 300);
                view->verticalScrollBar()->setValue(237);
            }
            qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Markdown, false);
            QVERIFY(!md->previewGpu());
            QCOMPARE(QucsSettingsFile().value("Viewers/markdownGpu").toBool(), false);
            const QImage cpu = shown();
            qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Markdown, true);
            QVERIFY(md->previewGpu());
            // What the screen shows - the framebuffer as the window painted
            // it, not a grab's paint - is the grab's picture.
            const QImage screen = onScreen(view);
            const QImage gpu = shown();
            if (!shots.isEmpty()) {
                cpu.save(shots + QStringLiteral("/markdown_cpu_%1.png").arg(kind));
                gpu.save(shots + QStringLiteral("/markdown_gpu_%1.png").arg(kind));
                screen.save(shots + QStringLiteral("/markdown_screen_%1.png").arg(kind));
            }
            QCOMPARE(gpu.size(), cpu.size());
            QCOMPARE(screen.size(), gpu.size());
            // The quotes' bars, each its colour in each.
            const qreal ratio = cpu.devicePixelRatio();
            int bars = 0;
            for (const QRectF& bar : qucs_s::markdown::QuoteBars::bars(view)) {
                const QPoint at = (bar.center() * ratio).toPoint();
                if (!cpu.rect().contains(at)) continue;
                ++bars;
                QVERIFY2(cpu.pixel(at) != cpu.pixel(at + QPoint(int(6 * ratio), 0)), "a bar drawn by the CPU");
                QCOMPARE(QColor(gpu.pixel(at)), QColor(cpu.pixel(at)));
                QCOMPARE(QColor(screen.pixel(at)), QColor(cpu.pixel(at)));
            }
            if (kind != "scrolled") QVERIFY(bars >= 2);
            // Pixel by pixel: alike where a pixel of one has its colour in
            // the other within a pixel of it - the CPU's glyphs on both, at
            // most one inked pixel in 200 apart. (At a ratio of 1.5 a tile
            // half a device pixel off, through the linear sampler: a quarter
            // of them, the text softer, the blocks below alike.)
            const QColor paper = cpu.pixelColor(cpu.width() - 3, cpu.height() - 3);
            const auto near = [](const QImage& in, int x, int y, QColor want) {
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!in.rect().contains(x + dx, y + dy)) continue;
                        const QColor k = in.pixelColor(x + dx, y + dy);
                        if (std::abs(k.red() - want.red()) + std::abs(k.green() - want.green()) + std::abs(k.blue() - want.blue()) <= 60) return true;
                    }
                return false;
            };
            int inked = 0, differ = 0, blue = 0, off = 0;
            for (int y = 0; y < cpu.height(); ++y)
                for (int x = 0; x < cpu.width(); ++x) {
                    const QColor a = cpu.pixelColor(x, y), b = gpu.pixelColor(x, y);
                    blue += b == QColor(30, 120, 200);
                    off += screen.pixel(x, y) != gpu.pixel(x, y);
                    if (a == paper && b == paper) continue;
                    ++inked;
                    if (!near(gpu, x, y, a) || !near(cpu, x, y, b)) ++differ;
                }
            QVERIFY2(off == 0, qPrintable(QStringLiteral("the screen's frame and the grab: %1 pixels apart").arg(off)));
            // Alike block by block: the mean colour of each 32 by 32 pixels
            // within 30 (the channels' sum) - a selection or a scroll
            // missing scores of them.
            constexpr int n = 32;
            int blocks = 0, apart = 0, worst = 0;
            for (int by = 0; by + n <= cpu.height(); by += n)
                for (int bx = 0; bx + n <= cpu.width(); bx += n) {
                    qint64 sum[2][3] = {};
                    for (int y = by; y < by + n; ++y)
                        for (int x = bx; x < bx + n; ++x)
                            for (int k = 0; k < 2; ++k) {
                                const QRgb c = (k == 0 ? cpu : gpu).pixel(x, y);
                                sum[k][0] += qRed(c);
                                sum[k][1] += qGreen(c);
                                sum[k][2] += qBlue(c);
                            }
                    int d = 0;
                    for (int c = 0; c < 3; ++c) d += int(std::abs(sum[0][c] - sum[1][c]) / (n * n));
                    ++blocks;
                    apart += d > 30;
                    worst = std::max(worst, d);
                }
            qInfo("%s: %d of %d pixels inked, %d differ (%.2f %%); %d of %d blocks apart, at most %d", qPrintable(kind), inked,
                  cpu.width() * cpu.height(), differ, 100.0 * differ / std::max(1, inked), apart, blocks, worst);
            QVERIFY(inked > 5000);
            QVERIFY2(differ <= inked / 200, qPrintable(QStringLiteral("%1 of %2 inked pixels apart").arg(differ).arg(inked)));
            if (kind != "scrolled") QVERIFY(blue > 1000);   // (the picture, drawn by the GPU)
            QCOMPARE(view->verticalScrollBar()->value(), kind == "scrolled" ? 237 : 0);
            QVERIFY2(apart < blocks / 100, qPrintable(QStringLiteral("%1 of %2 blocks").arg(apart).arg(blocks)));
        }
        // A document shorter than the view: the paper under it too - the
        // GPU's viewport fills its background as Qt fills the CPU's, and a
        // viewport back from the GPU still fills its own.
        {
            QVERIFY(app.gotoPage(write("short.md", "# Short\n\nOne line.\n")));
            MarkdownDoc* small = current(app);
            QVERIFY(small != nullptr && small != md);
            small->setMode(MarkdownDoc::Mode::Preview);
            QTextBrowser* preview = small->preview();
            small->setPreviewGpu(false);
            const QImage before = shownOf(preview);
            small->setPreviewGpu(true);
            QVERIFY(small->previewGpu());
            const QImage screen = onScreen(preview), gpu = shownOf(preview);
            small->setPreviewGpu(false);
            const QImage after = shownOf(preview);
            const QPoint low = QPoint(preview->viewport()->width() / 2, preview->viewport()->height() - 8) * before.devicePixelRatio();
            // (the CPU's background: its viewport's, filled by Qt)
            QCOMPARE(QColor(before.pixel(low)).name(), preview->viewport()->palette().color(preview->viewport()->backgroundRole()).name());
            QCOMPARE(QColor(gpu.pixel(low)).name(), QColor(before.pixel(low)).name());
            QCOMPARE(QColor(screen.pixel(low)).name(), QColor(before.pixel(low)).name());
            QCOMPARE(QColor(after.pixel(low)).name(), QColor(before.pixel(low)).name());
            QVERIFY(app.gotoPage(file));
        }
        // A scroll's paint, each way.
        qint64 worst[2] = {0, 0}, median[2] = {0, 0}, whole[2] = {0, 0};
        for (const bool onGpu : {false, true}) {
            md->setPreviewGpu(onGpu);
            (void)shown();
            auto* timer = new PaintTimer(view->viewport());
            for (int k = 0; k < 20; ++k) {
                view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() + 31);
                QTest::qWait(20);
            }
            QTRY_VERIFY(timer->paints > 5);
            worst[onGpu] = timer->worst;
            median[onGpu] = timer->median();
            // (all of it painted again: the drawing's own cost)
            timer->times.clear();
            for (int k = 0; k < 10; ++k) {
                view->viewport()->update();
                QTest::qWait(20);
            }
            QTRY_VERIFY(timer->times.size() >= 5);
            whole[onGpu] = timer->median();
            delete timer;
            view->verticalScrollBar()->setValue(0);
        }
        qInfo("all of it painted: the CPU %lld µs, the GPU %lld µs", whole[0], whole[1]);
        // (Back to the default for the tests after.)
        QucsSettingsFile().remove(qucs_s::gpu::settingKey(qucs_s::gpu::Viewer::Markdown));
        md->setPreviewGpu(qucs_s::gpu::used(qucs_s::gpu::Viewer::Markdown));
        QVERIFY(!md->previewGpu());
        qInfo("a scroll's paint: the CPU %lld µs (%lld at most), the GPU %lld µs (%lld)", median[0], worst[0], median[1], worst[1]);
        app.closeAllFiles();
    }

    // The preview on the GPU draws its tiles once (PreviewTiles: the CPU's
    // drawing kept by the GPU): a frame with nothing changed draws none; a
    // scroll only the row it brings into sight, back none; a selection, an
    // edit, a new width all of those in sight again; the rows far out of
    // sight let go.
    // A GPU viewport that gives up leaves the CPU drawing the preview, not
    // a blank one; the GPU not tried again there.
    void aGpuPreviewThatGivesUpLeavesTheCpu()
    {
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
#ifdef QUCS_GPU_VIEW
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(700, 500);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(write("gives-up.md", "# Given up\n\nThe CPU draws this.\n")));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        md->setPreviewGpu(true);
        QVERIFY(md->previewGpu());
        auto* viewport = dynamic_cast<qucs_s::gpu::Viewport2D*>(md->preview()->viewport());
        QVERIFY(viewport != nullptr);
        viewport->giveUp();
        QTRY_VERIFY(!md->previewGpu());
        QVERIFY(md->previewTiles() == nullptr);
        QVERIFY(qucs_s::gpu::gpuViewportBroken(md->preview()));
        // Drawn: ink in the preview.
        pump();
        const QImage shot = md->preview()->viewport()->grab().toImage();
        int inked = 0;
        for (int y = 0; y < shot.height(); y += 2)
            for (int x = 0; x < shot.width(); x += 2) inked += std::abs(qGray(shot.pixel(x, y)) - qGray(shot.pixel(0, 0))) > 80;
        QVERIFY2(inked > 20, qPrintable(QString::number(inked)));
        md->setPreviewGpu(true);
        QVERIFY(!md->previewGpu());
        app.closeAllFiles();
#else
        QSKIP("a build without Qt's RHI");
#endif
    }

    // A viewer on the GPU chosen (Settings… > Viewers), the window is made
    // for it before it is first shown: a preview on the GPU opened in it
    // leaves its native window as it was. It was made again, the whole
    // window gone and back - a flash as a model opened (the user's report
    // of 18 October); without the choice, a preview on the GPU still has
    // Qt make it again.
    void aGpuViewerLeavesTheWindowAsItWas()
    {
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
#ifdef QUCS_GPU_VIEW
        const QString file = write("window.md", "# The window\n\nAs it was.\n");
        const struct Back {
            ~Back() { QucsSettingsFile().remove(qucs_s::gpu::settingKey(qucs_s::gpu::Viewer::Markdown)); }
        } back;
        for (const bool chosen : {true, false}) {
            qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Markdown, chosen);
            QucsApp app(false);
            MainGuard guard(&app);
            app.resize(700, 500);
            app.show();
            QVERIFY(QTest::qWaitForWindowExposed(&app));
            // (Its RHI widget hidden: nothing of it over the window.)
            const QRhiWidget* ready = app.findChild<QRhiWidget*>(QStringLiteral("qucsGpuWindowReady"));
            QCOMPARE(ready != nullptr, chosen);
            QVERIFY(ready == nullptr || !ready->isVisible());
            const QPointer<QWindow> shown = app.windowHandle();
            QVERIFY(shown);
            QVERIFY(app.gotoPage(file));
            MarkdownDoc* md = current(app);
            QVERIFY(md != nullptr);
            md->setMode(MarkdownDoc::Mode::Preview);
            md->setPreviewGpu(true);
            QVERIFY(md->previewGpu());
            pump();
            QVERIFY2(!shown.isNull() == chosen, chosen ? "made again" : "kept: Qt no longer makes it again?");
            if (chosen) QCOMPARE(app.windowHandle(), shown.data());
            app.closeAllFiles();
        }
#else
        QSKIP("a build without Qt's RHI");
#endif
    }

    void theGpuPreviewDrawsWhatChanged()
    {
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        using qucs_s::markdown::PreviewTiles;
        QByteArray text = "# Tiles\n\n";
        for (int i = 0; i < 300; ++i) text += "Line " + QByteArray::number(i) + " of a long document, drawn in tiles.\n\n";
        const QString file = write("tiles.md", text);
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        md->setPreviewGpu(true);
        QVERIFY(md->previewGpu());
        const PreviewTiles* tiles = md->previewTiles();
        QVERIFY(tiles != nullptr);
        QTextBrowser* view = md->preview();
        pump();
        // (A frame drawn now - grabbed -: the window's paints not waited for.)
        const auto frame = [view] { return qucs_s::gpu::shownFrame(view); };
        const int rows = view->viewport()->height() / PreviewTiles::TileHeight + 2;   // in sight, at most
        (void)frame();
        int drawn = tiles->drawn();
        QVERIFY(drawn >= 1);
        (void)frame();
        (void)frame();
        QCOMPARE(tiles->drawn(), drawn);
        // A scroll of half a row: one row new at most; back: none.
        QVERIFY(view->verticalScrollBar()->maximum() > 10 * PreviewTiles::TileHeight);
        view->verticalScrollBar()->setValue(PreviewTiles::TileHeight / 2);
        (void)frame();
        QVERIFY2(tiles->drawn() - drawn <= 1, qPrintable(QString::number(tiles->drawn() - drawn)));
        drawn = tiles->drawn();
        view->verticalScrollBar()->setValue(0);
        (void)frame();
        QCOMPARE(tiles->drawn(), drawn);
        // A selection: drawn over the tiles, an image to each block in it
        // (the heading's and the first line's) - no tile again.
        const QImage plain = frame();
        QTextCursor c(view->document());
        c.setPosition(2);
        c.setPosition(40, QTextCursor::KeepAnchor);
        view->setTextCursor(c);
        const QImage selected = frame();
        QCOMPARE(tiles->drawn(), drawn);
        QCOMPARE(tiles->overlaysDrawn(), 2);
        QVERIFY(selected != plain);
        // A drag's step: the block it ends in alone; a frame unchanged: none.
        c.setPosition(45, QTextCursor::KeepAnchor);
        view->setTextCursor(c);
        const QImage dragged = frame();
        QCOMPARE(tiles->overlaysDrawn(), 3);
        QVERIFY(dragged != selected);
        (void)frame();
        QCOMPARE(tiles->overlaysDrawn(), 3);
        QCOMPARE(tiles->drawn(), drawn);
        // Taken away: the tiles as they were.
        c.clearSelection();
        view->setTextCursor(c);
        QCOMPARE(frame(), plain);
        QCOMPARE(tiles->overlaysDrawn(), 3);
        c.setPosition(2);
        c.setPosition(40, QTextCursor::KeepAnchor);
        view->setTextCursor(c);
        QCOMPARE(frame(), selected);
        // An edit of the rendering: again.
        QTextCursor(view->document()).insertText(QStringLiteral("Edited. "));
        const QImage edited = frame();
        QVERIFY(tiles->drawn() - drawn >= 2 && tiles->drawn() - drawn <= rows);
        QVERIFY(tiles->overlaysDrawn() > 4);
        QVERIFY(edited != selected);
        drawn = tiles->drawn();
#ifdef QUCS_GPU_VIEW
        // The textures of tiles let go dropped with them (at the next
        // frame): those in sight alone kept.
        (void)frame();
        auto* gpuViewport = dynamic_cast<qucs_s::gpu::Viewport2D*>(view->viewport());
        QVERIFY(gpuViewport != nullptr);
        QVERIFY2(gpuViewport->textures() <= rows + 2, qPrintable(QString::number(gpuViewport->textures())));   // (and the selection's two)
#endif
        // A new width: again.
        app.resize(800, 700);
        pump();
        (void)frame();
        QVERIFY(tiles->drawn() > drawn);
        // Through the whole document: a row beyond those in sight kept each
        // side, no more.
        for (int y = 0; y <= view->verticalScrollBar()->maximum(); y += PreviewTiles::TileHeight) {
            view->verticalScrollBar()->setValue(y);
            (void)frame();
            QVERIFY2(tiles->kept() <= rows + 2, qPrintable(QString::number(tiles->kept())));
        }
        md->setPreviewGpu(false);
        QVERIFY(md->previewTiles() == nullptr);
        app.closeAllFiles();
    }

    // In a window - a browser placed in it, drawn where it is - an image
    // and a formula are drawn where the layout put them (taken off the
    // painter's transform, each was moved twice: the chat's pictures below
    // its prompt's frame). Read in the picture's own pixels: two to each on
    // a Retina screen.
    void drawnWhereLaidOutInAWindow()
    {
        QWidget window;
        window.resize(520, 420);
        auto* browser = new QTextBrowser(&window);
        browser->setGeometry(30, 70, 440, 320);
        browser->setFrameShape(QFrame::NoFrame);
        QTextDocument* doc = browser->document();
        qucs_s::math::MathObject::install(doc);
        qucs_s::markdown::SmoothImages::install(doc);
        QImage solid(300, 60, QImage::Format_RGB32);
        solid.fill(qRgb(0, 160, 0));
        qucs_s::markdown::render(doc, "![s](solid.png)\n\nThe math: $\\sum_{k=1}^{n} k^2$ here.\n", QFont(), {});
        doc->addResource(QTextDocument::ImageResource, QUrl("solid.png"), solid);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        qucs_s::markdown::fitImages(doc, browser->viewport()->width());
        pump();
        const QImage shot = window.grab().toImage();
        const qreal r = shot.devicePixelRatio();
        const auto pixel = [&shot, r](QPoint at) { return QColor(shot.pixel((QPointF(at) * r).toPoint())); };
        const QPoint origin = browser->viewport()->mapTo(&window, QPoint(0, 0));
        // The image: green at its middle, not above it.
        const QRectF block = doc->documentLayout()->blockBoundingRect(doc->begin());
        const QPoint middle = origin + QPoint(int(block.left() + 150), int(block.top() + 30));
        QVERIFY2(block.height() > 55, qPrintable(QString::number(block.height())));   // (the picture 300 by 60)
        QCOMPARE(pixel(middle).green(), 160);
        QVERIFY(pixel(middle - QPoint(0, 33)).green() != 160 || pixel(middle - QPoint(0, 33)).red() != 0);
        // The formula: ink within its place in its line.
        QTextBlock line = doc->begin().next();
        int at = -1;
        for (QTextBlock::iterator it = line.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().objectType() == qucs_s::math::MathObject::Type) at = it.fragment().position() - line.position();
        QVERIFY(at >= 0);
        const QTextLine l = line.layout()->lineForTextPosition(at);
        const QRectF lineRect = doc->documentLayout()->blockBoundingRect(line);
        const QRect place(origin + QPoint(int(lineRect.left() + l.cursorToX(at)), int(lineRect.top() + l.y())), QSize(20, int(l.height())));
        int darkest = 255;
        for (int x = int(place.left() * r); x <= int(place.right() * r); ++x)
            for (int y = int(place.top() * r); y <= int(place.bottom() * r); ++y) darkest = std::min(darkest, qGray(shot.pixel(x, y)));
        QVERIFY2(darkest < 100, qPrintable(QString::number(darkest)));
    }

    // Math is typeset at the screen's pixels and drawn on whole ones, a
    // pixel of it to each (the rendering's was typeset at one to a point,
    // scaled up on a Retina screen; and drawn at a fraction of a pixel,
    // blended with its neighbours: blurred). GitHub's math blocks and bare
    // equation environments are typeset; \\( written for MathJax too.
    void mathIsSharpAndFound()
    {
        QTextDocument doc;
        qucs_s::math::MathObject::install(&doc);
        qucs_s::markdown::render(&doc, "Inline $x^2$.", QFont(), {}, 2.0);
        QImage image;
        QTextCharFormat math;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().objectType() == qucs_s::math::MathObject::Type) {
                    math = it.fragment().charFormat();
                    image = qvariant_cast<QImage>(math.property(qucs_s::math::MathObject::ImageProperty));
                }
        QVERIFY(!image.isNull());
        QCOMPARE(image.devicePixelRatio(), 2.0);
        // Drawn at a fraction of a pixel: on the nearest whole one, its own
        // pixels.
        const QSizeF size = math.property(qucs_s::math::MathObject::SizeProperty).toSizeF();
        QImage drawn(QSize(80, 60) * 2, QImage::Format_ARGB32_Premultiplied), expected = drawn;
        drawn.setDevicePixelRatio(2);
        expected.setDevicePixelRatio(2);
        drawn.fill(Qt::white);
        expected.fill(Qt::white);
        {
            QPainter p(&drawn);
            qucs_s::math::MathObject object;
            object.drawObject(&p, QRectF(QPointF(10.3, 5.7), size), &doc, 0, math);
        }
        {
            QPainter p(&expected);
            p.drawImage(QPointF(21, 11) / 2, image);   // (20.6 and 11.4 device pixels, rounded)
        }
        QVERIFY(drawn == expected);

        using qucs_s::math::findMath;
        const auto texts = [](const QString& md) {
            QStringList t;
            for (const auto& sp : findMath(md)) t << (sp.display ? "D:" : "T:") + sp.tex;
            return t;
        };
        QCOMPARE(texts("```math\nE = mc^2\n```"), QStringList{"D:E = mc^2"});
        QCOMPARE(texts("~~~ Math\nx\n~~~\n"), QStringList{"D:x"});
        QCOMPARE(texts("- item\n\n  ```math\n  a^2 + b^2\n  ```\n"), QStringList{"D:a^2 + b^2"});
        QCOMPARE(texts("$a$ then\n```math\nb\n```\nthen $c$"), (QStringList{"T:a", "D:b", "T:c"}));
        QCOMPARE(texts("```python\nx = $a$\n```"), QStringList());
        QCOMPARE(texts("````\n```math\nx\n```\n````"), QStringList());   // (in another block)
        QCOMPARE(texts("```a``` and ```math``` inline"), QStringList());
        QCOMPARE(texts("```a``` x\n\n```math\nb\n```\n"), QStringList{"D:b"});   // (inline code first: no fence)
        QCOMPARE(texts("\\begin{equation}\nE = mc^2\n\\end{equation}"), QStringList{"D:\\begin{equation}\nE = mc^2\n\\end{equation}"});
        QCOMPARE(texts("\\begin{align*}\na &= b \\\\\nc &= d\n\\end{align*}"), QStringList{"D:\\begin{align*}\na &= b \\\\\nc &= d\n\\end{align*}"});
        QCOMPARE(texts("$$\\begin{equation}x\\end{equation}$$"), QStringList{"D:\\begin{equation}x\\end{equation}"});
        QCOMPARE(texts("\\begin{itemize}x\\end{itemize}"), QStringList());
        QCOMPARE(texts("Inline \\\\(a^2\\\\) and \\\\[b\\\\]"), (QStringList{"T:a^2", "D:b"}));
        for (const char* tex : {"\\begin{equation}\nE = mc^2\n\\end{equation}", "\\begin{align*}\na &= b \\\\\nc &= d\n\\end{align*}"})
            QVERIFY2(qucs_s::math::typeset(QString::fromLatin1(tex), QFont(), Qt::black, true).ok, tex);
        // Rendered: each typeset, nothing of the fences left; a block centred.
        QTextDocument blocks;
        qucs_s::math::MathObject::install(&blocks);
        qucs_s::markdown::render(&blocks, "Before.\n\n```math\nZ = R + jX\n```\n\n\\begin{equation}\nY = 1/Z\n\\end{equation}\n\nAfter \\\\(w\\\\).\n",
                                 QFont(), {});
        QStringList typeset;
        bool centred = false;
        for (QTextBlock b = blocks.begin(); b.isValid(); b = b.next())
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().objectType() == qucs_s::math::MathObject::Type) {
                    typeset << it.fragment().charFormat().toolTip();
                    centred = centred || (b.blockFormat().alignment() & Qt::AlignHCenter);
                }
        QCOMPARE(typeset.size(), 3);
        QCOMPARE(typeset.at(0), QStringLiteral("Z = R + jX"));
        QCOMPARE(typeset.at(2), QStringLiteral("w"));
        // (An environment LaTeX would number: said in its tip that it is not.)
        QVERIFY2(typeset.at(1).startsWith("\\begin{equation}\nY = 1/Z\n\\end{equation}\n\n") && typeset.at(1).contains("Not numbered"),
                 qPrintable(typeset.at(1)));
        QVERIFY(centred);
        // In a list item: set in under its text, not centred on the page
        // (GUI check of 2026-10-16, 6); on its own: centred.
        QTextDocument listed;
        qucs_s::math::MathObject::install(&listed);
        qucs_s::markdown::render(&listed, "$$a = b$$\n\n- an item\n\n    ```math\n    y = mx + c\n    ```\n\n> quoted\n>\n> $$q$$\n", QFont(), {});
        QList<QTextBlock> shown;
        for (QTextBlock b = listed.begin(); b.isValid(); b = b.next())
            if (b.text().trimmed() == QString(QChar::ObjectReplacementCharacter)) shown << b;
        QCOMPARE(shown.size(), 3);
        QVERIFY(shown.at(0).blockFormat().alignment() & Qt::AlignHCenter);
        for (const QTextBlock& b : {shown.at(1), shown.at(2)}) {
            QVERIFY(!(b.blockFormat().alignment() & Qt::AlignHCenter));
            QVERIFY(b.blockFormat().indent() > 0 || b.blockFormat().leftMargin() > 24 || b.textList() != nullptr);
        }
        // align's columns: the left sides to the right of one column, the
        // right sides from it - wider than any one row set alone.
        const qreal aligned = qucs_s::math::measure("\\begin{aligned} x &= 1111111 \\\\ aaaaaaa &= 2 \\end{aligned}", QFont(), true).width;
        const qreal rowOne = qucs_s::math::measure("x = 1111111", QFont(), true).width;
        const qreal rowTwo = qucs_s::math::measure("aaaaaaa = 2", QFont(), true).width;
        QVERIFY2(aligned > std::max(rowOne, rowTwo) + 0.5 * std::min(rowOne, rowTwo),
                 qPrintable(QString("%1 against %2 and %3").arg(aligned).arg(rowOne).arg(rowTwo)));
        QVERIFY2(!blocks.toPlainText().contains("```") && !blocks.toPlainText().contains("begin"), qPrintable(blocks.toPlainText()));
    }

    // QUCS_MARKDOWN_FUZZ=<seconds> (QUCS_MARKDOWN_FUZZ_SEED): Markdown
    // made of fragments - tables (images, math and HTML in cells, rows of
    // other widths), lists and quotes nested, fences (math ones, ones not
    // closed), HTML, links, images of hostile sizes (none, 1 by 20000,
    // missing), math in every form, the private use area the renderer
    // marks math with - rendered at random widths and pixel ratios, its
    // images fitted and drawn under random transforms; random TeX typeset
    // alone. Nothing may crash, hang or trip a sanitizer.
    void fuzz()
    {
        const int seconds = qEnvironmentVariableIntValue("QUCS_MARKDOWN_FUZZ");
        if (seconds <= 0) QSKIP("set QUCS_MARKDOWN_FUZZ to a number of seconds");
        QRandomGenerator rng(quint32(qEnvironmentVariableIntValue("QUCS_MARKDOWN_FUZZ_SEED") + 1));
        const auto pick = [&rng](const QStringList& from) { return from.at(int(rng.bounded(quint32(from.size())))); };
        const QStringList tex{"x^2", "\\frac{a}{b}", "\\sqrt[3]{x}", "\\sum_{n=0}^{\\infty}", "\\int_0^1 f\\,dx", "\\left(\\frac{1}{2}\\right)",
                              "\\begin{pmatrix}1&2\\\\3&4\\end{pmatrix}", "\\begin{cases}a&b\\\\c\\end{cases}", "\\mathbb{R}", "\\text{SNR}",
                              "\\hat{x}\\vec{v}", "\\overline{AB}", "\\underbrace{a}_{b}", "\\color{red}{x}", "\\nosuch", "{", "}", "^", "_",
                              "\\\\", "&", "\\left(", "\\right)", "\\begin{align}a&=b\\\\c&=d\\end{align}", "\\begin{equation}", "\\end{equation}",
                              "\\frac{", "\\sqrt{", "\\mathrm{V}", "\\,", "\\!", "\\qquad", "%", "$", "\\$", "\\tag{1}", "\\begin{array}{cc}1&2\\end{array}",
                              QString(300, QLatin1Char('{')), QString(300, QLatin1Char('}')), "\\frac" + QString(40, QLatin1Char('{'))};
        const std::function<QString(int)> mathText = [&rng, &pick, &tex, &mathText](int depth) -> QString {
            QString t;
            for (int k = int(rng.bounded(6u)); k-- > 0;) t += depth < 8 && rng.bounded(3u) == 0 ? "{" + mathText(depth + 1) + "}" : pick(tex);
            return t;
        };
        const auto math = [&rng, &mathText]() -> QString {
            const QString t = mathText(0);
            switch (rng.bounded(9u)) {
            case 0: return "$" + t + "$";
            case 1: return "$$" + t + "$$";
            case 2: return "\\(" + t + "\\)";
            case 3: return "\\[" + t + "\\]";
            case 4: return "\n```math\n" + t + "\n```\n";
            case 5: return "\\begin{equation}" + t + "\\end{equation}";
            case 6: return "\\\\(" + t + "\\\\)";
            case 7: return "$`" + t + "`$";
            default: return "$" + t;   // (not closed)
            }
        };
        const QStringList images{"![a](big.png)", "![b](tall.png)", "![c](tiny.png)", "![d](missing.png)", "<img src=\"big.png\" width=\"0\">",
                                 "<img src=\"big.png\" width=\"-5\" height=\"99999\">", "<img src=\"tall.png\" width=\"20000\">", "![e](big.png \"t\")",
                                 "[![f](tiny.png)](link)", "![g]()", "![h](data:image/png;base64,AAAA)"};
        const QStringList inline_{"word", "**bold**", "*em*", "`code $x$`", "~~del~~", "<b>b</b>", "<br>", "<span>", "</div>", "&amp;", "&#xE000;",
                                  QString(QChar(0xE000)), QString(QChar(0xE001)) + "x", QString(QChar(0xF7FF)), QString(QChar(0xFFFC)), "\\", "|", "[link](#h)",
                                  "<!-- c -->", "<code>$y$</code>", "😀", QString(QChar(0xD800)), "\t"};
        const std::function<QString(int)> block = [&rng, &pick, &math, &images, &inline_, &block](int depth) -> QString {
            const auto line = [&rng, &pick, &math, &images, &inline_]() {
                QString l;
                for (int k = 1 + int(rng.bounded(6u)); k-- > 0;)
                    l += (rng.bounded(4u) == 0 ? math() : rng.bounded(5u) == 0 ? pick(images) : pick(inline_)) + " ";
                return l;
            };
            switch (rng.bounded(depth > 4 ? 3u : 9u)) {
            case 0: return line() + "\n\n";
            case 1: return pick({"# ", "## ", "###### ", "####### "}) + line() + "\n";
            case 2: return "```" + pick({"", "math", "cpp", "`"}) + "\n" + line() + (rng.bounded(4u) ? "\n```\n" : "\n");
            case 3: {   // a table
                const int columns = 1 + int(rng.bounded(5u));
                QString t = "|";
                for (int c = 0; c < columns; ++c) t += " h" + QString::number(c) + " |";
                t += "\n|";
                for (int c = 0; c < columns; ++c) t += pick({"---|", ":--|", "--:|", ":-:|", "-|"});
                t += "\n";
                for (int r = int(rng.bounded(5u)); r-- > 0;) {
                    t += "|";
                    for (int c = int(rng.bounded(quint32(columns + 2))); c-- > 0;) t += " " + line() + " |";
                    t += "\n";
                }
                return t + "\n";
            }
            case 4: return "- " + line() + "\n" + (rng.bounded(2u) ? "  " + block(depth + 1).replace("\n", "\n  ") : QString()) + "\n";
            case 5: return "1. " + line() + "\n\n    " + block(depth + 1).replace("\n", "\n    ") + "\n";
            case 6: return "> " + block(depth + 1).replace("\n", "\n> ") + "\n";
            case 7: return "<details>\n<summary>s</summary>\n\n" + block(depth + 1) + (rng.bounded(2u) ? "</details>\n" : "\n");
            default: return math() + "\n\n";
            }
        };
        // The images: big, tall and thin, tiny.
        QImage big(1600, 400, QImage::Format_RGB32), tall(1, 20000, QImage::Format_RGB32), tiny(1, 1, QImage::Format_ARGB32);
        big.fill(Qt::darkCyan);
        tall.fill(Qt::red);
        tiny.fill(Qt::transparent);
        QElapsedTimer timer;
        timer.start();
        int documents = 0, typeset = 0;
        int slow = 0;
        while (timer.elapsed() < seconds * 1000LL) {
            QElapsedTimer one;
            one.start();
            QString md;
            for (int k = 1 + int(rng.bounded(8u)); k-- > 0;) md += block(0);
            QTextDocument doc;
            doc.setDocumentMargin(rng.bounded(3u) * 6);
            qucs_s::math::MathObject::install(&doc);
            qucs_s::markdown::SmoothImages::install(&doc);
            QFont font;
            font.setPointSizeF(pick({"1", "6", "13", "40", "200"}).toDouble());
            const qreal dpr = pick({"0.5", "1", "1.5", "2", "3", "4"}).toDouble();
            qucs_s::markdown::render(&doc, md, font, {}, dpr);
            doc.addResource(QTextDocument::ImageResource, QUrl("big.png"), big);
            doc.addResource(QTextDocument::ImageResource, QUrl("tall.png"), tall);
            doc.addResource(QTextDocument::ImageResource, QUrl("tiny.png"), tiny);
            const qreal width = pick({"0", "1", "50", "300", "800", "5000", "-20"}).toDouble();
            qucs_s::markdown::fitImages(&doc, width);
            qucs_s::markdown::fitImages(&doc, width * 0.5);
            doc.setTextWidth(std::max<qreal>(1.0, width));
            const QSizeF size = doc.size();
            QImage out(QSize(int(std::min<qreal>(1200, std::max<qreal>(1, size.width()))), int(std::min<qreal>(1200, std::max<qreal>(1, size.height())))),
                       QImage::Format_ARGB32_Premultiplied);
            out.setDevicePixelRatio(rng.bounded(2u) ? 1.0 : 2.0);
            out.fill(Qt::white);
            {
                QPainter p(&out);
                switch (rng.bounded(4u)) {
                case 0: p.translate(rng.generateDouble() * 3, rng.generateDouble() * 3); break;
                case 1: p.scale(0.37, 0.37); break;
                case 2: p.rotate(13); break;
                default: break;
                }
                doc.drawContents(&p, QRectF(0, 0, out.width(), out.height()));
            }
            (void)qucs_s::math::findMath(md);
            (void)qucs_s::markdown::htmlBalanced(md);
            ++documents;
            // (A document of more than 3 s kept, with the width it had: a hang's start.)
            if (one.elapsed() > 3000) {
                const QString keep = qEnvironmentVariable("QUCS_FUZZ_OUT", dir.path());
                QDir().mkpath(keep);
                QFile f(keep + QStringLiteral("/slow-%1.md").arg(++slow));
                if (f.open(QIODevice::WriteOnly)) f.write(md.toUtf8());
                qWarning("slow document %d: %lld ms at width %g, font %g pt, dpr %g (%s)", slow, one.elapsed(), width, font.pointSizeF(), dpr,
                         qPrintable(f.fileName()));
            }
            for (int k = 0; k < 3; ++k) {
                QFont f;
                f.setPointSizeF(pick({"0.5", "9", "13", "72"}).toDouble());
                const qucs_s::math::Typeset t = qucs_s::math::typeset(mathText(0), f, Qt::black, rng.bounded(2u) == 0, dpr);
                (void)t;
                ++typeset;
            }
        }
        qInfo("%d documents, %d formulas typeset alone", documents, typeset);
    }

    // The rendering follows the edits; the text is saved as it is.
    void theRenderingFollowsTheText()
    {
        const QString file = write("edit.md", "# First\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Split);
        QTextCursor c(md->document());
        c.movePosition(QTextCursor::End);
        c.insertText("\n## Added *later*\n");
        QTRY_VERIFY(blockWith(md->preview()->document(), "Added later").isValid());
        QCOMPARE(blockWith(md->preview()->document(), "Added later").blockFormat().headingLevel(), 2);
        QVERIFY(md->getDocChanged());
        QCOMPARE(md->save(), 0);
        QFile saved(file);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains("## Added *later*"));
        app.closeAllFiles();
    }

    // The images it shows, written again, made or put in place by a
    // rename: the rendering is made again. (The Markdown's own file the
    // window watches; its pictures were no one's - a plot made again stayed
    // as it was until the file itself changed.) A file beside them changed
    // alone, or an image of the web or a data URL, is not followed.
    void imagesWrittenAgainRenderAgain()
    {
        const auto picture = [this](const QString& name, QColor colour, bool rename) {
            QImage image(240, 160, QImage::Format_RGB32);
            image.fill(colour);
            if (!rename) return image.save(dir.filePath(name), "PNG");
            const QString part = dir.filePath(name + QStringLiteral(".part"));
            if (!image.save(part, "PNG")) return false;
            QFile::remove(dir.filePath(name));
            return QFile::rename(part, dir.filePath(name));
        };
        QVERIFY(QDir().mkpath(dir.filePath("pictures")));
        QVERIFY(picture("pictures/plot.png", Qt::red, false));
        QFile::remove(dir.filePath("pictures/later.png"));
        // (And links of no file: an empty one, a part, the folder - each
        // the folder resolved, watched as an image: every file made beside
        // the document rendered it again - the hunt of 21 October, B2.)
        const QString file = write("pictures.md", "# Pictures\n\n![plot](pictures/plot.png)\n\n![later](pictures/later.png)\n\n"
                                                  "![web](https://example.com/badge.png)\n\n![data](data:image/png;base64,iVBORw0KGgo=)\n\n"
                                                  "![a figure to come]()\n\n![part](#part)\n\n![folder](.)\n\n![up](pictures/..)\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        pump();
        // (The pixels of the rendering in \a colour.)
        const auto shown = [md](QColor colour) {
            const QImage grab = md->preview()->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
            int n = 0;
            for (int y = 0; y < grab.height(); ++y)
                for (int x = 0; x < grab.width(); ++x) n += (grab.pixel(x, y) & 0xffffff) == (colour.rgb() & 0xffffff);
            return n;
        };
        QTRY_VERIFY(shown(Qt::red) > 10000);
        QStringList watched;
        for (const QString& image : md->watchedImages()) watched << QFileInfo(image).fileName();
        watched.sort();
        QCOMPARE(watched, QStringList({"later.png", "plot.png"}));

        int renderings = 0;
        connect(md->preview()->document(), &QTextDocument::contentsChange, this, [&renderings] { ++renderings; });
        write("pictures/notes.txt", "beside them\n");
        write("notes.txt", "beside the document\n");
        QTest::qWait(1500);
        QCOMPARE(renderings, 0);

        // Written again where it is; renamed over it; one made after.
        QVERIFY(picture("pictures/plot.png", Qt::blue, false));
        QTRY_VERIFY_WITH_TIMEOUT(shown(Qt::blue) > 10000, 10000);
        QCOMPARE(shown(Qt::red), 0);
        QVERIFY(renderings > 0);
        QVERIFY(picture("pictures/plot.png", Qt::green, true));
        QTRY_VERIFY_WITH_TIMEOUT(shown(Qt::green) > 10000, 10000);
        // (Before it is rendered again, Qt draws one that came at its own
        // size in the place of the missing one's mark, over the one above.)
        QVERIFY(picture("pictures/later.png", Qt::magenta, false));
        QTRY_VERIFY_WITH_TIMEOUT(shown(Qt::magenta) > 30000 && shown(Qt::green) > 30000, 10000);
        disconnect(md->preview()->document(), nullptr, this, nullptr);
        app.closeAllFiles();
    }

    // An image is read once while its file is as it was: each render
    // cleared the document's resources and decoded every image again (eight
    // photos of 4000x3000, 0.53 s a render, 250 ms after each pause in
    // typing - the hunt of 21 October, C1). Its file changed, it is read
    // again.
    void imagesDecodedOnce()
    {
        QImage picture(400, 300, QImage::Format_RGB32);
        picture.fill(Qt::darkRed);
        QVERIFY(picture.save(dir.filePath("once.png"), "PNG"));
        const QString file = write("once.md", "# Once\n\n![p](once.png)\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        pump();
        const auto shown = [md] {
            const QVariant v = md->preview()->document()->resource(QTextDocument::ImageResource, QUrl(QStringLiteral("once.png")));
            return v.typeId() == QMetaType::QImage ? v.value<QImage>() : QImage();
        };
        const QImage first = shown();
        QVERIFY(!first.isNull());
        md->renderNow();
        QCOMPARE(shown().cacheKey(), first.cacheKey());
        md->renderNow();
        QCOMPARE(shown().cacheKey(), first.cacheKey());
        picture.fill(Qt::darkBlue);
        QVERIFY(picture.save(dir.filePath("once.png"), "PNG"));
        QTRY_VERIFY_WITH_TIMEOUT(shown().cacheKey() != first.cacheKey() && shown().pixelColor(5, 5) == QColor(Qt::darkBlue), 10000);
        app.closeAllFiles();
    }

    // A file dated in the future (unpacked from a time zone ahead, a fast
    // clock), written again by another program - dated now, before it -: read
    // again. (Only a newer date was a change: every later one was missed
    // until the clock passed its date - the hunt of 21 October, B5.) Not
    // changed, it is not read again.
    void aFileDatedInTheFutureIsReadAgain()
    {
        const QString file = write("future.md", "# Future\n\nMark zero\n");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::currentDateTime().addSecs(3 * 3600), QFileDevice::FileModificationTime));
        }
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        QTRY_VERIFY(blockWith(md->preview()->document(), "Mark zero").isValid());
        QVERIFY(md->getLastSaved() > QDateTime::currentDateTime());
        QVERIFY(!md->changedOnDisk(QFileInfo(file).lastModified()));
        int loads = 0;
        connect(md->document(), &QTextDocument::contentsChanged, this, [&loads] { ++loads; });
        QTest::qWait(2000);   // (the watch set; nothing changed: not read again)
        QCOMPARE(loads, 0);
        write("future.md", "# Future\n\nMark one\n");
        QTRY_VERIFY_WITH_TIMEOUT(blockWith(md->preview()->document(), "Mark one").isValid(), 10000);
        disconnect(md->document(), nullptr, this, nullptr);
        app.closeAllFiles();
    }

    // A change waited for 400 ms after the last, as a file is written in
    // pieces - but not past 1.5 s after the first: where each change is told
    // at once (Linux's inotify), a file written to all the time held it off
    // for good (the hunt of 21 October, N5).
    void changesPutOffAtMostOneAndAHalfSeconds()
    {
        QTimer timer;
        timer.setSingleShot(true);
        timer.setInterval(400);
        QElapsedTimer first, clock;
        qint64 fired = -1;
        connect(&timer, &QTimer::timeout, this, [&] {
            if (fired < 0) fired = clock.elapsed();
        });
        clock.start();
        while (clock.elapsed() < 3500 && fired < 0) {
            misc::restartUpTo(&timer, first, 1500);
            QTest::qWait(100);
        }
        QVERIFY2(fired >= 1500 && fired < 2400, qPrintable(QString::number(fired)));
        // Changes apart: each waited for 400 ms, as before (a coarse timer's
        // 5 % early at most).
        fired = -1;
        clock.start();
        misc::restartUpTo(&timer, first, 1500);
        QTRY_VERIFY(fired >= 0);
        QVERIFY2(fired >= 370 && fired < 900, qPrintable(QString::number(fired)));
    }

    // A long run with no break - letters, braces -, where quotes narrow the
    // column, is laid out in time linear in its length: read into a document
    // of no view and given a break every 64 letters (a zero-width space). Qt
    // laid it out as it read it, and broke each line searching the run to its
    // end: a letter a line, 32 kB three quotes deep 8.9 s, a list and 105 kB
    // of display math's braces 24 s (the hunt of 22 October, C1 and C2).
    // Copied, the run is whole, without the breaks. A hundred images in one
    // long block are fitted in one edit (each laid the block out again: 10 s).
    // (Its times' ratios judged in Release builds: sanitized, cache effects.)
    void longRunsRenderInLinearTime()
    {
        const auto runOf = [](int letters, const QByteArray& before) {
            QByteArray t = before;
            for (int i = 0; i < letters; ++i) t += char('a' + i % 26);
            return t + "\n";
        };
        const QString file = write("runs.md", runOf(8000, "> > > "));
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(528, 479);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Split);
        pump();
        const auto best = [md] {
            qint64 least = std::numeric_limits<qint64>::max();
            for (int i = 0; i < 2; ++i) {
                QElapsedTimer t;
                t.start();
                md->renderNow();
                least = std::min(least, t.nsecsElapsed());
            }
            return least;
        };
        const qint64 shorter = best();
        write("runs.md", runOf(32000, "> > > "));
        QVERIFY(md->reload());
        const qint64 longer = best();
        qInfo("a run three quotes deep: 8000 letters %.1f ms, 32000 %.1f ms", shorter / 1e6, longer / 1e6);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
        QVERIFY2(longer < 8 * shorter, qPrintable(QStringLiteral("%1 ms, a quarter of it %2 ms").arg(longer / 1e6).arg(shorter / 1e6)));
#endif
        QTextDocument* shown = md->preview()->document();
        QVERIFY2(shown->toPlainText().count(QChar(0x200B)) >= 32000 / 64 - 2, qPrintable(QString::number(shown->toPlainText().count(QChar(0x200B)))));
        // Copied whole, without them.
        md->preview()->selectAll();
        md->preview()->copy();
        const QString copied = QGuiApplication::clipboard()->text();
        QVERIFY2(!copied.contains(QChar(0x200B)) && copied.contains(QString::fromLatin1(runOf(32000, "").chopped(1))), qPrintable(copied.left(80)));
        // A hundred, then four hundred images in one block: fitted in one edit.
        QImage dot(4, 4, QImage::Format_RGB32);
        dot.fill(Qt::red);
        QVERIFY(dot.save(dir.filePath("dot.png")));
        const auto images = [](int n) {
            QByteArray t = "x";
            for (int i = 0; i < n; ++i) t += " ![d](dot.png)`";
            return t + "\n";
        };
        write("runs.md", images(100));
        QVERIFY(md->reload());
        const qint64 few = best();
        write("runs.md", images(400));
        QVERIFY(md->reload());
        const qint64 many = best();
        qInfo("images in one block: 100 %.1f ms, 400 %.1f ms", few / 1e6, many / 1e6);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
        QVERIFY2(many < 8 * few, qPrintable(QStringLiteral("%1 ms, a quarter of them %2 ms").arg(many / 1e6).arg(few / 1e6)));
#endif
        app.closeAllFiles();
    }

    // A long document - a formula on each line, code blocks - is laid out
    // once a rendering, its formulas found in one pass. (Laid out again
    // after each formula from it to the end, each searched for from the
    // start: 3000 lines took 4 s - its rendering after a change on disk
    // seemed never to come.)
    void longDocumentsRenderAtOnce()
    {
        const auto text = [](int lines) {
            QByteArray t = "# Long\n\n";
            for (int i = 0; i < lines; ++i) {
                t += "Line " + QByteArray::number(i) + " of some text, *emphasis* and $x_{" + QByteArray::number(i) + "}^2$.\n\n";
                if (i % 100 == 0) t += "```\ncode " + QByteArray::number(i) + "\n```\n\n";
            }
            return t;
        };
        const QString file = write("long.md", text(1000));
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        pump();
        QTextDocument* shown = md->preview()->document();
        int changes = 0;
        connect(shown, &QTextDocument::contentsChange, this, [&changes] { ++changes; });
        md->renderNow();
        disconnect(shown, nullptr, this, nullptr);
        // (Qt's reading of it, its HTML apart, the shading and the math: a
        // change each.)
        QVERIFY2(changes < 20, qPrintable(QString::number(changes)));
        QVERIFY(blockWith(shown, "Line 999 of some text").isValid());
        QCOMPARE(shown->toPlainText().count(QChar::ObjectReplacementCharacter), 1000);

        // Four times the length, some four times the time (searched from
        // the start, ten times and more).
        const auto best = [md] {
            qint64 least = std::numeric_limits<qint64>::max();
            for (int i = 0; i < 2; ++i) {
                QElapsedTimer t;
                t.start();
                md->renderNow();
                least = std::min(least, t.nsecsElapsed());
            }
            return least;
        };
        const qint64 shorter = best();
        write("long.md", text(4000));
        QVERIFY(md->reload());
        QCOMPARE(md->preview()->document()->toPlainText().count(QChar::ObjectReplacementCharacter), 4000);
        const qint64 longer = best();
        QVERIFY2(longer < 7 * shorter, qPrintable(QStringLiteral("%1 ms, a quarter of it %2 ms").arg(longer / 1e6).arg(shorter / 1e6)));
        app.closeAllFiles();
    }

    // The file written again while it is read (a long one's text set,
    // highlighted and rendered): read again. Its date was taken after the
    // reading, the writing's - taken for the one read, and never loaded.
    void aFileWrittenWhileReadIsReadAgain()
    {
        const QString file = write("race.md", "# Race\n\nMark zero\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        QTRY_VERIFY(blockWith(md->preview()->document(), "Mark zero").isValid());
        // As the text read is set, the file written again (dated a second
        // on: the same millisecond is no change).
        bool written = false;
        connect(md->document(), &QTextDocument::contentsChanged, this, [&] {
            if (written || !md->toPlainText().contains(QLatin1String("Mark one"))) return;
            written = true;
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("# Race\n\nMark two\n");
            QVERIFY(f.setFileTime(QDateTime::currentDateTime().addSecs(1), QFileDevice::FileModificationTime));
        });
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("# Race\n\nMark one\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(written, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(blockWith(md->preview()->document(), "Mark two").isValid(), 10000);
        QVERIFY(md->toPlainText().contains(QLatin1String("Mark two")));
        QVERIFY(!md->getDocChanged());
        disconnect(md->document(), nullptr, this, nullptr);
        app.closeAllFiles();
    }

    // A link to a heading scrolls to it; one to another document opens it.
    void linksLeadWhereTheyPoint()
    {
        QByteArray longText = kReadme;
        for (int i = 0; i < 80; ++i) longText += "\nFiller line " + QByteArray::number(i) + ".\n";
        longText += "\n## Section Two\n\nThe end.\n";
        const QString readme = write("links.md", longText);
        write("other.md", "# The Other One\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(readme));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);
        md->setMode(MarkdownDoc::Mode::Preview);
        pump();
        QScrollBar* bar = md->preview()->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(0);
        md->followLink(QUrl("#section-two"));
        QVERIFY(bar->value() > 0);

        md->followLink(QUrl("other.md"));
        MarkdownDoc* other = current(app);
        QVERIFY(other != nullptr);
        QCOMPARE(QFileInfo(other->getDocName()).fileName(), QString("other.md"));
        app.closeAllFiles();
    }

    // A link never hands a program to the system (it ran: a .command in
    // Terminal, an .app, an .exe - bug hunt 2026-09-26, B2): another file
    // is shown in the file manager once asked, a URL of another scheme goes
    // to the system once asked; web pages and mail go straight there; a
    // document Qucs-S opens opens in a tab.
    void linksNeverRunPrograms()
    {
        QVERIFY(QDir().mkpath(dir.filePath("project")));
        const QString readme = write("project/README.md", "# Project\n\n[Open the schematic](evil.command)\n");
        const QString evil = write("project/evil.command", "#!/bin/sh\necho ran > ran.txt\n");
        QFile::setPermissions(evil, QFile::permissions(evil) | QFileDevice::ExeOwner);
        write("project/notes.txt", "notes\n");
        Handed handed;
        for (const char* scheme : {"file", "x-custom", "https", "mailto"})
            QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), &handed, "open");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(readme));
        MarkdownDoc* md = current(app);
        QVERIFY(md != nullptr);

        LinkAnswer a = answeringLink([&] { md->followLink(QUrl("evil.command")); });
        QVERIFY(a.asked);
        QVERIFY2(a.said.contains("evil.command") && a.said.contains("not open"), qPrintable(a.said));
        a = answeringLink([&] { md->followLink(QUrl::fromLocalFile(evil)); });   // as a file: URL
        QVERIFY(a.asked);
        QVERIFY2(handed.urls.isEmpty(), qPrintable(handed.urls.value(0).toString()));

        a = answeringLink([&] { md->followLink(QUrl("x-custom://whatever/x")); });
        QVERIFY(a.asked);
        QVERIFY2(a.said.contains("x-custom://whatever/x"), qPrintable(a.said));
        QVERIFY(handed.urls.isEmpty());
        a = answeringLink([&] { md->followLink(QUrl("x-custom://whatever/x")); }, "linkOpen");
        QCOMPARE(handed.urls, QList<QUrl>{QUrl("x-custom://whatever/x")});
        handed.urls.clear();

        a = answeringLink([&] {
            md->followLink(QUrl("https://example.org/datasheet"));
            md->followLink(QUrl("mailto:someone@example.org"));
            md->followLink(QUrl("missing.command"));   // not there: nothing
        });
        QVERIFY(!a.asked);
        QCOMPARE(handed.urls, QList<QUrl>({QUrl("https://example.org/datasheet"), QUrl("mailto:someone@example.org")}));

        md->followLink(QUrl("notes.txt"));
        QCOMPARE(QFileInfo(app.getDoc()->getDocName()).fileName(), QString("notes.txt"));
        QVERIFY(!QFileInfo::exists(dir.filePath("project/ran.txt")));
        for (const char* scheme : {"file", "x-custom", "https", "mailto"}) QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
        app.closeAllFiles();
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestMarkdownDoc test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_markdown_doc.moc"
