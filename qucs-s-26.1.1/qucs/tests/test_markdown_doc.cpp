/*
 * A Markdown file in a tab of its own: its text - a text document, edited
 * and saved as any other - and the text rendered (headings, emphasis,
 * tables, code on a shade, math typeset), beside it or instead of it; the
 * rendering follows the edits; its links lead to their heading, or to the
 * document they name; the mode chosen is kept for the next file.
 */
#include <QtTest>
#include <QAbstractButton>
#include <QDesktopServices>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
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

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
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

    // In a window - a browser placed in it, drawn where it is - an image
    // and a formula are drawn where the layout put them (taken off the
    // painter's transform, each was moved twice: the chat's pictures below
    // its prompt's frame).
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
        const QPoint origin = browser->viewport()->mapTo(&window, QPoint(0, 0));
        // The image: green at its middle, not above it.
        const QRectF block = doc->documentLayout()->blockBoundingRect(doc->begin());
        const QPoint middle = origin + QPoint(int(block.left() + 150), int(block.top() + 30));
        QVERIFY2(block.height() > 55, qPrintable(QString::number(block.height())));   // (the picture 300 by 60)
        QCOMPARE(QColor(shot.pixel(middle)).green(), 160);
        QVERIFY(QColor(shot.pixel(middle - QPoint(0, 33))).green() != 160 || QColor(shot.pixel(middle - QPoint(0, 33))).red() != 0);
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
        for (int x = place.left(); x <= place.right(); ++x)
            for (int y = place.top(); y <= place.bottom(); ++y) darkest = std::min(darkest, qGray(shot.pixel(x, y)));
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
        QCOMPARE(typeset, (QStringList{"Z = R + jX", "\\begin{equation}\nY = 1/Z\n\\end{equation}", "w"}));
        QVERIFY(centred);
        QVERIFY2(!blocks.toPlainText().contains("```") && !blocks.toPlainText().contains("begin"), qPrintable(blocks.toPlainText()));
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
