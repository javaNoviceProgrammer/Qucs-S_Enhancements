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
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
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
                                 "After a stray close.", "After CDATA,", "inline.", "THE END"})
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
