/*
 * The text editor's syntax highlighting: the language of a file by its
 * suffix; what each language picks out (keywords, strings, comments over
 * several lines, a # in a string that is not a comment...); the formats,
 * as the settings have them, saved when changed; the status bar's language
 * button, whose choice is kept for the files of a suffix; and the Source
 * Code Editor tab of the settings, a radio button per language and its
 * styles, every language's changes saved by Apply.
 */
#include <QtTest>
#include <QCheckBox>
#include <QColorDialog>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "statusbar.h"
#include "syntax.h"
#include "textdoc.h"
#include "dialogs/qucssettingsdialog.h"
#include "dialogs/syntaxsettings.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using qucs_s::syntax::Format;
using qucs_s::syntax::Style;

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The settings of the highlighting as they were before a test.
struct SyntaxGuard {
    QMap<QString, QString> formats = QucsSettings.SyntaxFormats, chosen = QucsSettings.SyntaxForSuffix;
    ~SyntaxGuard()
    {
        QucsSettings.SyntaxFormats = formats;
        QucsSettings.SyntaxForSuffix = chosen;
    }
};

// A colour of its own for each style: what a character was highlighted
// as is told by its colour.
QColor colourOf(Style style)
{
    return QColor(10 + 12 * int(style), 50, 90);
}

// \a text highlighted as \a language.
class Highlighted
{
public:
    Highlighted(int language, const QString& text) : a_highlighter(&a_doc)
    {
        a_doc.setPlainText(text);
        QHash<int, Format> formats;
        for (int s = 0; s < int(Style::Count); ++s) formats.insert(s, Format{colourOf(Style(s)), false, false});
        a_highlighter.setFormats(formats);
        a_highlighter.setLanguage(language);
    }

    // The style of the \a nth \a needle on \a line (the style of its
    // first character; and its last must be the same); -1 when plain.
    int styleAt(int line, const QString& needle, int nth = 0) const
    {
        const QTextBlock block = a_doc.findBlockByNumber(line);
        int column = -1;
        for (int i = 0; i <= nth; ++i) column = int(block.text().indexOf(needle, column + 1));
        if (column < 0) return -2;   // not there
        const int first = styleOfColumn(block, column);
        const int last = styleOfColumn(block, column + int(needle.size()) - 1);
        return first == last ? first : -3;
    }

private:
    QTextDocument a_doc;
    SyntaxHighlighter a_highlighter;

    static int styleOfColumn(const QTextBlock& block, int column)
    {
        for (const QTextLayout::FormatRange& range : block.layout()->formats())
            if (column >= range.start && column < range.start + range.length) {
                const QColor colour = range.format.foreground().color();
                for (int s = 0; s < int(Style::Count); ++s)
                    if (colourOf(Style(s)).rgb() == colour.rgb()) return s;
            }
        return -1;
    }
};

int as(Style style)
{
    return int(style);
}

QToolButton* chip(QucsApp& app, const char* name)
{
    return app.statusBar()->findChild<QToolButton*>(QLatin1String(name));
}

TextDoc* current(QucsApp& app)
{
    return qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
}

// The status bar's language menu, open, and the action named \a text in it.
QAction* languageAction(QucsApp& app, const QString& text)
{
    QToolButton* button = chip(app, "statusLanguage");
    if (button == nullptr) return nullptr;
    button->click();
    auto* menu = button->findChild<QMenu*>("statusLanguageMenu");
    if (menu == nullptr) return nullptr;
    for (QAction* a : menu->actions())
        if (a->text() == text) return a;
    return nullptr;
}

void closeMenus(QucsApp& app)
{
    if (QToolButton* button = chip(app, "statusLanguage"))
        for (QMenu* menu : button->findChildren<QMenu*>()) menu->close();
    QCoreApplication::processEvents();
}
} // namespace

class TestSyntaxHighlighting : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QByteArray& bytes)
    {
        QFile f(dir.filePath(name));
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        return f.fileName();
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
        QucsSettings.SyntaxFormats.clear();
        QucsSettings.SyntaxForSuffix.clear();
    }

    // A file is highlighted as the language of its suffix, whatever its
    // case; plain text for one no language has.
    void theLanguageOfAFileIsItsSuffixs()
    {
        using namespace qucs_s::syntax;
        const struct { const char* file; int language; } files[] = {
            {"a.py", LANG_PYTHON}, {"b.CPP", LANG_CPP}, {"c.h", LANG_CPP}, {"d.va", LANG_VERILOGA},
            {"e.vhd", LANG_VHDL}, {"f.v", LANG_VERILOG}, {"g.sv", LANG_VERILOG}, {"h.m", LANG_OCTAVE},
            {"i.cir", LANG_SPICE}, {"j.lib", LANG_SPICE}, {"k.net", LANG_QUCS_NETLIST}, {"l.sh", LANG_SHELL},
            {"m.md", LANG_MARKDOWN}, {"n.json", LANG_JSON}, {"o.xml", LANG_XML}, {"p.txt", LANG_NONE},
            {"Makefile", LANG_NONE}, {"/some/dir.py/q.vhdl", LANG_VHDL},
        };
        for (const auto& f : files) QCOMPARE(languageFor(f.file), f.language);
        // Each language named once, plain text first, the others by name.
        const QList<int> list = languages();
        QCOMPARE(list.size(), int(LANG_COUNT));
        QCOMPARE(list.first(), int(LANG_NONE));
        QCOMPARE(name(LANG_NONE), QString("Plain Text"));
        for (int i = 2; i < list.size(); ++i)
            QVERIFY(QString::compare(name(list[i - 1]), name(list[i]), Qt::CaseInsensitive) < 0);
        for (int language : list) QCOMPARE(byKey(key(language)), language);
    }

    void pythonIsHighlighted()
    {
        const Highlighted h(LANG_PYTHON,
                            "def gain(x):  # say \"hi\"\n"
                            "s = \"a # b\" + 'it''s'\n"
                            "\"\"\"doc\n"
                            "still doc\"\"\" + x\n"
                            "x = 0x1F + 2.5\n"
                            "@staticmethod\n"
                            "print(len(s), None)\n");
        QCOMPARE(h.styleAt(0, "def"), as(Style::Keyword));
        QCOMPARE(h.styleAt(0, "gain"), as(Style::Element));
        QCOMPARE(h.styleAt(0, "# say"), as(Style::Comment));
        QCOMPARE(h.styleAt(0, "\"hi\""), as(Style::Comment));   // a string in a comment
        QCOMPARE(h.styleAt(1, "\"a # b\""), as(Style::String));   // a # in a string
        QCOMPARE(h.styleAt(1, "s"), -1);
        QCOMPARE(h.styleAt(2, "doc"), as(Style::String));
        QCOMPARE(h.styleAt(3, "still doc\"\"\""), as(Style::String));   // over two lines
        QCOMPARE(h.styleAt(3, "x"), -1);                               // and closed
        QCOMPARE(h.styleAt(4, "0x1F"), as(Style::Number));
        QCOMPARE(h.styleAt(4, "2.5"), as(Style::Number));
        QCOMPARE(h.styleAt(5, "@staticmethod"), as(Style::Directive));
        QCOMPARE(h.styleAt(6, "print"), as(Style::Builtin));
        QCOMPARE(h.styleAt(6, "len"), as(Style::Builtin));
        QCOMPARE(h.styleAt(6, "None"), as(Style::Keyword));
    }

    void cppIsHighlighted()
    {
        const Highlighted h(LANG_CPP,
                            "#include <vector>\n"
                            "/* a comment\n"
                            "   over lines */ int n = 42;\n"
                            "const char* s = \"// not \\\" a comment\"; // but this\n"
                            "char c = '\\'';\n");
        QCOMPARE(h.styleAt(0, "#include"), as(Style::Directive));
        QCOMPARE(h.styleAt(0, "<vector>"), as(Style::String));
        QCOMPARE(h.styleAt(1, "a comment"), as(Style::Comment));
        QCOMPARE(h.styleAt(2, "over lines */"), as(Style::Comment));
        QCOMPARE(h.styleAt(2, "int"), as(Style::Type));
        QCOMPARE(h.styleAt(2, "42"), as(Style::Number));
        QCOMPARE(h.styleAt(3, "const"), as(Style::Keyword));
        QCOMPARE(h.styleAt(3, "\"// not \\\" a comment\""), as(Style::String));   // an escaped quote
        QCOMPARE(h.styleAt(3, "// but this"), as(Style::Comment));
        QCOMPARE(h.styleAt(4, "'\\''"), as(Style::String));
    }

    void verilogAIsHighlighted()
    {
        const Highlighted h(LANG_VERILOGA,
                            "`include \"disciplines.vams\"\n"
                            "analog begin\n"
                            "  I(p,n) <+ V(p,n) / 1k;  // flow\n"
                            "  x = $temperature + ln(2);\n");
        QCOMPARE(h.styleAt(0, "`include"), as(Style::Directive));
        QCOMPARE(h.styleAt(0, "\"disciplines.vams\""), as(Style::String));
        QCOMPARE(h.styleAt(1, "analog"), as(Style::Keyword));
        QCOMPARE(h.styleAt(2, "I"), as(Style::Builtin));
        QCOMPARE(h.styleAt(2, "V"), as(Style::Builtin));
        QCOMPARE(h.styleAt(2, "1"), as(Style::Number));
        QCOMPARE(h.styleAt(2, "k"), as(Style::Unit));
        QCOMPARE(h.styleAt(2, "p"), -1);
        QCOMPARE(h.styleAt(2, "// flow"), as(Style::Comment));
        QCOMPARE(h.styleAt(3, "$temperature"), as(Style::Builtin));
        QCOMPARE(h.styleAt(3, "ln"), as(Style::Builtin));
    }

    void vhdlAndVerilogAreHighlighted()
    {
        const Highlighted vhdl(LANG_VHDL,
                               "ENTITY Foo IS -- the entity\n"
                               "  wait for 10 ns;\n"
                               "  if clk'event and clk = '1' then\n");
        QCOMPARE(vhdl.styleAt(0, "ENTITY"), as(Style::Keyword));   // in any case
        QCOMPARE(vhdl.styleAt(0, "Foo"), -1);
        QCOMPARE(vhdl.styleAt(0, "-- the entity"), as(Style::Comment));
        QCOMPARE(vhdl.styleAt(1, "ns"), as(Style::Unit));
        QCOMPARE(vhdl.styleAt(1, "10"), as(Style::Number));
        QCOMPARE(vhdl.styleAt(2, "'event"), as(Style::Attribute));
        QCOMPARE(vhdl.styleAt(2, "'1'"), as(Style::String));

        const Highlighted verilog(LANG_VERILOG,
                                  "module m (input clk); reg [7:0] q;\n"
                                  "  always @(posedge clk) q <= 8'hFF; $display(\"q\");\n");
        QCOMPARE(verilog.styleAt(0, "module"), as(Style::Keyword));
        QCOMPARE(verilog.styleAt(0, "reg"), as(Style::Type));
        QCOMPARE(verilog.styleAt(1, "8'hFF"), as(Style::Number));
        QCOMPARE(verilog.styleAt(1, "$display"), as(Style::Builtin));
    }

    void spiceAndQucsNetlistsAreHighlighted()
    {
        const Highlighted spice(LANG_SPICE,
                                "* a title comment\n"
                                "R1 in out 1k ; the resistor\n"
                                ".tran 1n 1u\n"
                                "V1 in 0 DC 5 AC 1\n"
                                "C1 out 0 {cval}\n"
                                "plot v(out)\n");
        QCOMPARE(spice.styleAt(0, "* a title comment"), as(Style::Comment));
        QCOMPARE(spice.styleAt(1, "R1"), as(Style::Element));
        QCOMPARE(spice.styleAt(1, "1k"), as(Style::Number));
        QCOMPARE(spice.styleAt(1, "; the resistor"), as(Style::Comment));
        QCOMPARE(spice.styleAt(2, ".tran"), as(Style::Directive));
        QCOMPARE(spice.styleAt(2, "1n"), as(Style::Number));
        QCOMPARE(spice.styleAt(3, "DC"), as(Style::Keyword));
        QCOMPARE(spice.styleAt(4, "{cval}"), as(Style::Variable));
        QCOMPARE(spice.styleAt(5, "v"), as(Style::Builtin));

        const Highlighted net(LANG_QUCS_NETLIST,
                              "# a comment\n"
                              "R:R1 _net0 gnd R=\"1 kOhm\"\n"
                              ".AC:AC1 Type=\"lin\"\n");
        QCOMPARE(net.styleAt(0, "# a comment"), as(Style::Comment));
        QCOMPARE(net.styleAt(1, "R"), as(Style::Keyword));
        QCOMPARE(net.styleAt(1, "R1"), as(Style::Element));
        QCOMPARE(net.styleAt(1, "R", 2), as(Style::Key));
        QCOMPARE(net.styleAt(1, "\"1 kOhm\""), as(Style::String));
        QCOMPARE(net.styleAt(2, ".AC"), as(Style::Directive));
    }

    void octaveAndShellAreHighlighted()
    {
        const Highlighted octave(LANG_OCTAVE,
                                 "a = b'; % transpose\n"
                                 "s = 'text';\n"
                                 "%{\n"
                                 "inside\n"
                                 "%}\n"
                                 "plot(x); # done\n");
        QCOMPARE(octave.styleAt(0, "'"), -1);   // a transpose, not a string
        QCOMPARE(octave.styleAt(0, "% transpose"), as(Style::Comment));
        QCOMPARE(octave.styleAt(1, "'text'"), as(Style::String));
        QCOMPARE(octave.styleAt(3, "inside"), as(Style::Comment));
        QCOMPARE(octave.styleAt(5, "plot"), as(Style::Builtin));
        QCOMPARE(octave.styleAt(5, "# done"), as(Style::Comment));

        const Highlighted shell(LANG_SHELL,
                                "#!/bin/sh\n"
                                "echo \"$HOME\" ${#list} $n # note\n"
                                "for f in *.cir; do done\n");
        QCOMPARE(shell.styleAt(0, "#!/bin/sh"), as(Style::Directive));
        QCOMPARE(shell.styleAt(1, "echo"), as(Style::Builtin));
        QCOMPARE(shell.styleAt(1, "\"$HOME\""), as(Style::String));
        QCOMPARE(shell.styleAt(1, "${#list}"), as(Style::Variable));   // not a comment
        QCOMPARE(shell.styleAt(1, "$n"), as(Style::Variable));
        QCOMPARE(shell.styleAt(1, "# note"), as(Style::Comment));
        QCOMPARE(shell.styleAt(2, "for"), as(Style::Keyword));
        QCOMPARE(shell.styleAt(2, "done"), as(Style::Keyword));
    }

    void markupAndDataAreHighlighted()
    {
        const Highlighted json(LANG_JSON, "{\"key\": \"value\", \"n\": -1.5, \"ok\": true}\n");
        QCOMPARE(json.styleAt(0, "\"key\""), as(Style::Key));
        QCOMPARE(json.styleAt(0, "\"value\""), as(Style::String));
        QCOMPARE(json.styleAt(0, "-1.5"), as(Style::Number));
        QCOMPARE(json.styleAt(0, "true"), as(Style::Keyword));

        const Highlighted markdown(LANG_MARKDOWN,
                                   "# Title\n"
                                   "Some **bold** and `code` and [a link](x.md)\n"
                                   "- item\n"
                                   "```\n"
                                   "# not a heading\n"
                                   "```\n"
                                   "> quoted\n");
        QCOMPARE(markdown.styleAt(0, "# Title"), as(Style::Heading));
        QCOMPARE(markdown.styleAt(1, "**bold**"), as(Style::Emphasis));
        QCOMPARE(markdown.styleAt(1, "`code`"), as(Style::Code));
        QCOMPARE(markdown.styleAt(1, "[a link](x.md)"), as(Style::Link));
        QCOMPARE(markdown.styleAt(1, "Some"), -1);
        QCOMPARE(markdown.styleAt(2, "-"), as(Style::Keyword));
        QCOMPARE(markdown.styleAt(4, "# not a heading"), as(Style::Code));
        QCOMPARE(markdown.styleAt(6, "> quoted"), as(Style::Comment));

        const Highlighted xml(LANG_XML,
                              "<?xml version=\"1.0\"?>\n"
                              "<!-- a\n"
                              "note -->\n"
                              "<a href=\"x\">don't &amp; stop</a>\n");
        QCOMPARE(xml.styleAt(0, "<?xml version=\"1.0\"?>"), as(Style::Directive));
        QCOMPARE(xml.styleAt(2, "note -->"), as(Style::Comment));
        QCOMPARE(xml.styleAt(3, "<a"), as(Style::Tag));
        QCOMPARE(xml.styleAt(3, "href"), as(Style::Attribute));
        QCOMPARE(xml.styleAt(3, "\"x\""), as(Style::String));
        QCOMPARE(xml.styleAt(3, "don't"), -1);   // text, not a string
        QCOMPARE(xml.styleAt(3, "&amp;"), as(Style::Builtin));
        QCOMPARE(xml.styleAt(3, "</a"), as(Style::Tag));
    }

    // Every language that has styles has a sample that shows each of them.
    void eachSampleShowsEveryStyle()
    {
        for (int language : qucs_s::syntax::languages()) {
            const QList<Style> styles = qucs_s::syntax::styles(language);
            if (styles.isEmpty()) continue;
            const QString text = qucs_s::syntax::sample(language);
            QTextDocument doc;
            doc.setPlainText(text);
            SyntaxHighlighter h(&doc);
            QHash<int, Format> formats;
            for (int s = 0; s < int(Style::Count); ++s) formats.insert(s, Format{colourOf(Style(s)), false, false});
            h.setFormats(formats);
            h.setLanguage(language);
            QSet<int> seen;
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
                for (const QTextLayout::FormatRange& r : b.layout()->formats())
                    for (int s = 0; s < int(Style::Count); ++s)
                        if (colourOf(Style(s)).rgb() == r.format.foreground().color().rgb()) seen.insert(s);
            for (Style style : styles)
                QVERIFY2(seen.contains(int(style)),
                         qPrintable(qucs_s::syntax::name(language) + ": " + qucs_s::syntax::styleKey(style)));
        }
    }

    // A format is as the settings have it; kept when changed, forgotten
    // when set back; saved, and read again as Qucs-S starts.
    void formatsAreSetSavedAndRead()
    {
        using namespace qucs_s::syntax;
        SyntaxGuard guard;
        QucsSettings.SyntaxFormats.clear();
        const Format keyword = defaultFormat(LANG_PYTHON, Style::Keyword);
        QVERIFY(keyword.bold);
        QCOMPARE(format(LANG_PYTHON, Style::Keyword), keyword);
        QCOMPARE(parseFormat(formatText(Format{QColor("#123456"), true, true}), keyword),
                 (Format{QColor("#123456"), true, true}));
        QCOMPARE(parseFormat("nonsense italic", keyword), (Format{keyword.colour, false, true}));

        setFormat(LANG_PYTHON, Style::Keyword, Format{QColor(Qt::red), false, true});
        setFormat(LANG_CPP, Style::Comment, Format{QColor(Qt::darkGreen), false, true});
        setFormat(LANG_SPICE, Style::Number, defaultFormat(LANG_SPICE, Style::Number));   // not a change
        QCOMPARE(QucsSettings.SyntaxFormats.value("python/Keyword"), QString("#ff0000 italic"));
        QCOMPARE(QucsSettings.SyntaxFormats.size(), 2);
        QCOMPARE(format(LANG_PYTHON, Style::Keyword), (Format{QColor(Qt::red), false, true}));
        QCOMPARE(format(LANG_CPP, Style::Keyword), defaultFormat(LANG_CPP, Style::Keyword));   // each language its own

        QVERIFY(saveApplSettings());
        {
            QucsSettingsFile file;
            file.beginGroup("SyntaxFormats");
            QCOMPARE(file.allKeys().size(), 2);
            QCOMPARE(file.value("cpp/Comment").toString(), QString("#008000 italic"));
        }
        QucsSettings.SyntaxFormats.clear();
        QVERIFY(loadSettings());   // as at the next start
        QCOMPARE(format(LANG_PYTHON, Style::Keyword), (Format{QColor(Qt::red), false, true}));
        QCOMPARE(format(LANG_CPP, Style::Comment), (Format{QColor(Qt::darkGreen), false, true}));

        setFormat(LANG_PYTHON, Style::Keyword, keyword);
        QVERIFY(!QucsSettings.SyntaxFormats.contains("python/Keyword"));
        QucsSettings.SyntaxFormats.clear();
        QVERIFY(saveApplSettings());
    }

    // A file opened in the editor is highlighted as its language, in the
    // formats of the settings.
    void aTextTabIsHighlighted()
    {
        SyntaxGuard guard;
        QucsSettings.SyntaxFormats.clear();
        const QString py = write("script.py", "import os\nprint(os.name)  # the name\n");
        QucsApp app(false);
        MainGuard mainGuard(&app);
        QVERIFY(app.gotoPage(py));
        TextDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->language, int(LANG_PYTHON));
        const QTextBlock first = doc->document()->firstBlock();
        QTextCharFormat import;
        for (const QTextLayout::FormatRange& r : first.layout()->formats())
            if (r.start == 0) import = r.format;
        QCOMPARE(import.foreground().color().rgb(), qucs_s::syntax::format(LANG_PYTHON, Style::Keyword).colour.rgb());
        QCOMPARE(import.fontWeight(), int(QFont::Bold));
        app.closeAllFiles();
    }

    // The status bar names the language of the text document in front; its
    // menu chooses another, for every file of the suffix - those open, those
    // opened later, and at the next start - and goes back to the default.
    void theStatusBarChoosesTheLanguageOfASuffix()
    {
        SyntaxGuard guard;
        QucsSettings.SyntaxForSuffix.clear();
        const QString first = write("model.foo", "def f():\n    return 1\n");
        const QString second = write("other.foo", "x = 1\n");
        const QString third = write("later.foo", "y = 2\n");
        QucsApp app(false);
        MainGuard mainGuard(&app);
        app.setAttribute(Qt::WA_DontShowOnScreen);
        app.resize(1600, 800);
        app.show();
        QVERIFY(app.gotoPage(second));
        TextDoc* otherDoc = current(app);
        QVERIFY(app.gotoPage(first));
        TextDoc* doc = current(app);
        QVERIFY(doc != nullptr && otherDoc != nullptr && doc != otherDoc);
        app.statusPanel()->refresh();
        QToolButton* button = chip(app, "statusLanguage");
        QVERIFY(button != nullptr);
        QTRY_VERIFY(button->isVisibleTo(&app));
        QCOMPARE(button->text(), QString("Plain Text"));

        QAction* python = languageAction(app, "Python");
        QVERIFY(python != nullptr);
        QVERIFY(python->isCheckable() && !python->isChecked());
        QVERIFY(languageAction(app, "Plain Text")->isChecked());
        python->trigger();
        closeMenus(app);
        QCOMPARE(doc->language, int(LANG_PYTHON));
        QCOMPARE(otherDoc->language, int(LANG_PYTHON));   // the open one too
        QCOMPARE(QucsSettings.SyntaxForSuffix.value("foo"), QString("python"));
        {
            QucsSettingsFile file;   // saved at once
            QCOMPARE(file.value("SyntaxForSuffix/foo").toString(), QString("python"));
        }
        app.statusPanel()->refresh();
        QCOMPARE(button->text(), QString("Python"));
        QVERIFY(qucs_s::syntax::suffixesOf(LANG_PYTHON).contains("foo"));
        QVERIFY(app.gotoPage(third));
        QCOMPARE(current(app)->language, int(LANG_PYTHON));   // opened later

        QucsSettings.SyntaxForSuffix.clear();
        QVERIFY(loadSettings());   // at the next start
        QCOMPARE(qucs_s::syntax::languageFor("x.foo"), int(LANG_PYTHON));

        // Back to the default.
        QVERIFY(app.gotoPage(first));
        languageAction(app, "Python");
        auto* back = button->findChild<QAction*>("statusLanguageDefault");
        QVERIFY(back != nullptr);
        back->trigger();
        closeMenus(app);
        QCOMPARE(doc->language, int(LANG_NONE));
        QVERIFY(QucsSettings.SyntaxForSuffix.isEmpty());
        QucsSettingsFile file;
        QVERIFY(!file.contains("SyntaxForSuffix/foo"));
        app.closeAllFiles();
    }

    // A document with no suffix: the language is its own (nothing to keep
    // it for).
    void aDocumentWithoutASuffixHasItsOwnLanguage()
    {
        SyntaxGuard guard;
        const QString makefile = write("Makefile", "all:\n\techo done\n");
        QucsApp app(false);
        MainGuard mainGuard(&app);
        QVERIFY(app.gotoPage(makefile));
        TextDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->language, int(LANG_NONE));
        doc->chooseLanguage(LANG_SHELL);
        QCOMPARE(doc->language, int(LANG_SHELL));
        doc->refreshLanguage();   // (as after a save)
        QCOMPARE(doc->language, int(LANG_SHELL));
        QVERIFY(QucsSettings.SyntaxForSuffix.isEmpty());
        app.closeAllFiles();
    }

    // The Source Code Editor tab: a radio button per language; the one
    // chosen shows its styles, a colour, bold and italic each, and a
    // preview. Changes of several languages are all saved by Apply, and
    // the open documents highlighted in them; Default Values puts the
    // defaults back.
    void theSettingsTabSetsEveryLanguage()
    {
        using namespace qucs_s::syntax;
        SyntaxGuard guard;
        QucsSettings.SyntaxFormats.clear();
        const QString py = write("tab.py", "import os\n");
        QucsApp app(false);
        MainGuard mainGuard(&app);
        QVERIFY(app.gotoPage(py));
        TextDoc* doc = current(app);
        QVERIFY(doc != nullptr);

        QucsSettingsDialog dlg(&app);
        auto* tabs = dlg.findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        SyntaxSettingsPage* page = nullptr;
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Source Code Editor") page = qobject_cast<SyntaxSettingsPage*>(tabs->widget(i));
        QVERIFY(page != nullptr);
        QCOMPARE(page->selected(), int(LANG_PYTHON));   // the language of the document in front

        // A radio button for each language that is highlighted, by name.
        QStringList radios;
        for (QRadioButton* radio : page->findChildren<QRadioButton*>()) radios << radio->text();
        QStringList expected;
        for (int language : languages())
            if (language != LANG_NONE) expected << name(language);
        QCOMPARE(radios, expected);

        // Choosing one shows its page, with a row per style.
        auto* pages = page->findChild<QStackedWidget*>("syntaxPages");
        QVERIFY(pages != nullptr);
        page->findChild<QRadioButton*>("syntaxLanguage_spice")->click();
        QCOMPARE(page->selected(), int(LANG_SPICE));
        QCOMPARE(pages->currentWidget()->objectName(), QString("syntaxPage_spice"));
        for (Style style : styles(LANG_SPICE)) {
            const QString suffix = "_spice_" + styleKey(style);
            auto* colour = pages->currentWidget()->findChild<QPushButton*>("syntaxColour" + suffix);
            QVERIFY(colour != nullptr);
            QCOMPARE(colour->text(), defaultFormat(LANG_SPICE, style).colour.name());
            QVERIFY(pages->currentWidget()->findChild<QCheckBox*>("syntaxBold" + suffix) != nullptr);
            QVERIFY(pages->currentWidget()->findChild<QCheckBox*>("syntaxItalic" + suffix) != nullptr);
        }
        QVERIFY(page->findChild<QLabel*>("syntaxSuffixes_spice")->text().contains(".cir"));

        // SPICE: comments bold, the preview at once.
        auto* bold = page->findChild<QCheckBox*>("syntaxBold_spice_Comment");
        QVERIFY(!bold->isChecked());
        bold->setChecked(true);
        auto* preview = page->findChild<QPlainTextEdit*>("syntaxPreview_spice");
        QVERIFY(preview != nullptr);
        QTextCharFormat comment;
        for (const QTextLayout::FormatRange& r : preview->document()->firstBlock().layout()->formats())
            if (r.start == 0) comment = r.format;
        QCOMPARE(comment.fontWeight(), int(QFont::Bold));

        // Python: keywords in another colour, picked in the colour dialog.
        page->findChild<QRadioButton*>("syntaxLanguage_python")->click();
        QTimer::singleShot(0, [] {
            auto* picker = qobject_cast<QColorDialog*>(QApplication::activeModalWidget());
            if (picker == nullptr) return;
            picker->setCurrentColor(QColor("#aa5500"));
            picker->accept();
        });
        page->findChild<QPushButton*>("syntaxColour_python_Keyword")->click();
        QCOMPARE(page->findChild<QPushButton*>("syntaxColour_python_Keyword")->text(), QString("#aa5500"));
        QVERIFY(QucsSettings.SyntaxFormats.isEmpty());   // not before Apply

        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QCOMPARE(format(LANG_SPICE, Style::Comment).bold, true);
        QCOMPARE(format(LANG_PYTHON, Style::Keyword).colour, QColor("#aa5500"));
        {
            QucsSettingsFile file;
            QCOMPARE(file.value("SyntaxFormats/python/Keyword").toString(), QString("#aa5500 bold"));
            QCOMPARE(file.value("SyntaxFormats/spice/Comment").toString(), QString("#808080 bold italic"));
        }
        // The open document, in the new colour.
        QTextCharFormat import;
        for (const QTextLayout::FormatRange& r : doc->document()->firstBlock().layout()->formats())
            if (r.start == 0) import = r.format;
        QCOMPARE(import.foreground().color(), QColor("#aa5500"));

        // Default Values, then Apply: nothing of the user's left.
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotDefaultValues"));
        QVERIFY(!page->findChild<QCheckBox*>("syntaxBold_spice_Comment")->isChecked());
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(QucsSettings.SyntaxFormats.isEmpty());

        // A language's own button, for it alone.
        page->findChild<QCheckBox*>("syntaxItalic_cpp_Keyword")->setChecked(true);
        page->findChild<QCheckBox*>("syntaxItalic_json_Key")->setChecked(true);
        page->findChild<QPushButton*>("syntaxRestore_cpp")->click();
        QVERIFY(!page->findChild<QCheckBox*>("syntaxItalic_cpp_Keyword")->isChecked());
        QVERIFY(page->findChild<QCheckBox*>("syntaxItalic_json_Key")->isChecked());
        app.closeAllFiles();
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestSyntaxHighlighting test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_syntax_highlighting.moc"
