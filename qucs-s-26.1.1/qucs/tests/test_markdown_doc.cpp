/*
 * A Markdown file in a tab of its own: its text - a text document, edited
 * and saved as any other - and the text rendered (headings, emphasis,
 * tables, code on a shade, math typeset), beside it or instead of it; the
 * rendering follows the edits; its links lead to their heading, or to the
 * document they name; the mode chosen is kept for the next file.
 */
#include <QtTest>
#include <QAbstractButton>
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
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestMarkdownDoc test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_markdown_doc.moc"
