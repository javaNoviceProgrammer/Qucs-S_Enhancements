/*
 * The XML editor: a file read into its nodes, each where it is in the
 * text, and its paths; whether it is well formed, and where not; the text
 * made tidy or compact. In a tab: the text beside its tree, each following
 * the other; values edited in the tree, elements and attributes renamed,
 * added and deleted, each one step to undo; typing that closes tags; a
 * comment toggled; elements folded; the tree filtered; the error on the
 * Problems tab; the file's encoding kept.
 */
#include <QtTest>
#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeView>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "messagedock.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "syntax.h"
#include "xmldoc.h"
#include "xmlmodel.h"

using namespace qucs_s::xml;
using XNode = qucs_s::xml::Node;

namespace {

const char* const kCatalog =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!-- the books -->\n"
    "<catalog xmlns:x=\"urn:x\">\n"
    "  <book id=\"b1\" lang='en'>\n"
    "    <title>Circuits &amp; Systems</title>\n"
    "    <x:price>42</x:price>\n"
    "  </book>\n"
    "  <book id=\"b2\">\n"
    "    <title>Signals</title>\n"
    "    <note><![CDATA[a < b]]></note>\n"
    "    <empty/>\n"
    "  </book>\n"
    "  <?render fast?>\n"
    "</catalog>\n";

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The node of \a p at \a path.
int at(const Parsed& p, const QString& path)
{
    for (int n = 1; n < p.nodes.size(); ++n)
        if (pathOf(p, n) == path) return n;
    return -1;
}

// What a tree says of its nodes: kind, name and value of each, in order.
QStringList outline(const Parsed& p)
{
    QStringList out;
    for (int n = 1; n < p.nodes.size(); ++n) {
        const XNode& node = p.nodes.at(n);
        out << QStringLiteral("%1 %2=%3 @%4").arg(int(node.kind)).arg(node.name, node.value.simplified(), pathOf(p, n));
    }
    return out;
}

} // namespace

class TestXmlDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;

    XmlDoc* open(const QString& name, const QByteArray& bytes)
    {
        const QString path = dir.filePath("ws/" + name);
        if (!writeBytes(path, bytes)) return nullptr;
        if (!app->gotoPage(path)) return nullptr;
        return dynamic_cast<XmlDoc*>(app->getDoc());
    }
    void closeAll()
    {
        for (QucsDoc* d : app->allDocuments()) d->setDocChanged(false);
        app->closeAllFiles();
    }
    void moveCursor(XmlDoc* doc, int position)
    {
        QTextCursor c(doc->document());
        c.setPosition(position);
        doc->setTextCursor(c);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QDir().mkpath(dir.filePath("ws"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("ws"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("ws"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 900);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
    }

    void cleanupTestCase()
    {
        closeAll();
        delete app;
        QucsMain = nullptr;
    }

    // Its nodes, each where it is in the text: the declaration, comments,
    // elements and their attributes (either quote; a prefix as written),
    // text (an entity read), CDATA, an empty element, an instruction; their
    // lines; the node at an offset; their paths.
    void aTextIsReadIntoItsNodes()
    {
        const QString text = QString::fromUtf8(kCatalog);
        const Parsed p = parse(text);
        QVERIFY2(p.wellFormed, qPrintable(p.error));
        QCOMPARE(p.encoding, QStringLiteral("UTF-8"));
        QCOMPARE(p.elementCount(), 8);
        const QList<int>& top = p.nodes.at(0).children;
        QCOMPARE(p.nodes.at(top.at(0)).kind, XNode::Declaration);
        QCOMPARE(p.nodes.at(top.at(1)).kind, XNode::Comment);
        QCOMPARE(p.nodes.at(top.at(1)).value, QStringLiteral(" the books "));
        const int catalog = top.at(2);
        QCOMPARE(p.nodes.at(catalog).name, QStringLiteral("catalog"));
        QCOMPARE(text.mid(p.nodes.at(catalog).start, 9), QStringLiteral("<catalog "));
        QVERIFY(text.mid(p.nodes.at(catalog).start, p.nodes.at(catalog).end - p.nodes.at(catalog).start).endsWith("</catalog>"));
        QCOMPARE(p.nodes.at(catalog).line, 3);
        QCOMPARE(p.nodes.at(catalog).endLine, 14);

        const int book1 = at(p, "/catalog/book[1]");
        QVERIFY(book1 > 0);
        const int id = at(p, "/catalog/book[1]/@id"), lang = at(p, "/catalog/book[1]/@lang");
        QCOMPARE(p.nodes.at(id).value, QStringLiteral("b1"));
        QCOMPARE(text.mid(p.nodes.at(id).valueStart, p.nodes.at(id).valueEnd - p.nodes.at(id).valueStart), QStringLiteral("b1"));
        QCOMPARE(text.mid(p.nodes.at(lang).start, p.nodes.at(lang).end - p.nodes.at(lang).start), QStringLiteral("lang='en'"));
        QCOMPARE(at(p, "/catalog/@xmlns:x") > 0, true);
        const int title = at(p, "/catalog/book[1]/title/text()");
        QCOMPARE(p.nodes.at(title).value, QStringLiteral("Circuits & Systems"));
        QCOMPARE(text.mid(p.nodes.at(title).valueStart, p.nodes.at(title).valueEnd - p.nodes.at(title).valueStart),
                 QStringLiteral("Circuits &amp; Systems"));
        QVERIFY(at(p, "/catalog/book[1]/x:price") > 0);
        const int cdata = at(p, "/catalog/book[2]/note/text()");
        QCOMPARE(p.nodes.at(cdata).kind, XNode::CData);
        QCOMPARE(p.nodes.at(cdata).value, QStringLiteral("a < b"));
        const int empty = at(p, "/catalog/book[2]/empty");
        QCOMPARE(p.nodes.at(empty).tailStart, -1);
        QCOMPARE(text.mid(p.nodes.at(empty).start, p.nodes.at(empty).end - p.nodes.at(empty).start), QStringLiteral("<empty/>"));
        const int pi = at(p, "/catalog/processing-instruction()");
        QCOMPARE(p.nodes.at(pi).name, QStringLiteral("render"));
        QCOMPARE(p.nodes.at(pi).value, QStringLiteral("fast"));

        // The node at an offset: in a value, a text, a tag, between.
        QCOMPARE(nodeAt(p, p.nodes.at(id).valueStart + 1), id);
        QCOMPARE(nodeAt(p, p.nodes.at(title).valueStart + 3), title);
        QCOMPARE(nodeAt(p, p.nodes.at(book1).start + 2), book1);
        QCOMPARE(nodeAt(p, int(text.indexOf("<book id=\"b2\"")) - 1), catalog);   // (the whitespace between)
        QCOMPARE(nodeAt(p, 0), top.at(0));
        // Just after an element's end tag: in the one around it.
        const int titleElement = at(p, "/catalog/book[1]/title");
        QCOMPARE(nodeAt(p, p.nodes.at(titleElement).end), book1);
        QCOMPARE(nodeAt(p, p.nodes.at(titleElement).end - 1), titleElement);
        QCOMPARE(ancestry(p, title), QList<int>({catalog, book1, p.nodes.at(title).parent}));
        QCOMPARE(pathOf(p, 0), QStringLiteral("/"));
        // All at once, the same.
        const QStringList all = pathsOf(p);
        QCOMPARE(all.size(), p.nodes.size());
        for (int n = 0; n < p.nodes.size(); ++n) QCOMPARE(all.at(n), pathOf(p, n));

        // Characters beyond 16 bits (two in a QString) before them: where they are.
        const QString wide = QStringLiteral("<a note=\"\U0001F600\U0001F600\"><b>\U0001F680</b><c/></a>");
        const Parsed w = parse(wide);
        QVERIFY(w.wellFormed);
        const int c = at(w, "/a/c");
        QCOMPARE(wide.mid(w.nodes.at(c).start, 4), QStringLiteral("<c/>"));
        const int t = at(w, "/a/b/text()");
        QCOMPARE(wide.mid(w.nodes.at(t).valueStart, w.nodes.at(t).valueEnd - w.nodes.at(t).valueStart), QStringLiteral("\U0001F680"));
    }

    // Not well formed: the first error, where; what came before it read.
    void anErrorIsFound()
    {
        const Parsed p = parse(QStringLiteral("<a>\n  <b>one</b>\n  <c>two</d>\n</a>\n"));
        QVERIFY(!p.wellFormed);
        QCOMPARE(p.errorLine, 3);
        QVERIFY(p.errorColumn > 1);
        QVERIFY(!p.error.isEmpty());
        QVERIFY(at(p, "/a/b") > 0);
        QVERIFY(at(p, "/a/c") > 0);
        QVERIFY(!parse(QStringLiteral("<a>")).wellFormed);
        QVERIFY(!parse(QStringLiteral("<a></a><b/>")).wellFormed);
        QVERIFY(parse(QStringLiteral("  \n")).wellFormed);   // (nothing yet: nothing wrong)
        QVERIFY(!parse(QStringLiteral("<a b=1/>")).wellFormed);
    }

    // Tidy: each element on its line, indented; text, mixed content,
    // xml:space="preserve", comments, the declaration kept; done twice the
    // same. Compact: the whitespace between elements gone. Both read as the
    // original does.
    void aTextIsMadeTidyOrCompact()
    {
        const QString messy = QStringLiteral(
            "<?xml version=\"1.0\"?><root a=\"1\"   b='2'><!-- c --><item>text</item><item><sub/><sub x=\"y\"/></item>"
            "<p>mixed <b>bold</b> text</p><pre xml:space=\"preserve\"><l/>\n  <l/></pre><e></e></root>");
        QString why;
        const QString tidy = format(messy, QStringLiteral("  "), &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(tidy, QStringLiteral("<?xml version=\"1.0\"?>\n"
                                      "<root a=\"1\" b='2'>\n"
                                      "  <!-- c -->\n"
                                      "  <item>text</item>\n"
                                      "  <item>\n"
                                      "    <sub/>\n"
                                      "    <sub x=\"y\"/>\n"
                                      "  </item>\n"
                                      "  <p>mixed <b>bold</b> text</p>\n"
                                      "  <pre xml:space=\"preserve\"><l/>\n  <l/></pre>\n"
                                      "  <e></e>\n"
                                      "</root>\n"));
        QCOMPARE(format(tidy, QStringLiteral("  ")), tidy);
        QCOMPARE(format(tidy, QStringLiteral("\t")).section('\n', 2, 2), QStringLiteral("\t<!-- c -->"));
        const QString small = minify(tidy);
        QVERIFY(!small.contains(QLatin1Char('\n')) || small.contains(QStringLiteral("<l/>\n  <l/>")));
        QVERIFY(small.startsWith(QStringLiteral("<?xml version=\"1.0\"?><root a=\"1\" b='2'><!-- c --><item>text</item>")));
        QCOMPARE(outline(parse(small)), outline(parse(messy)));
        QCOMPARE(outline(parse(tidy)), outline(parse(messy)));
        QVERIFY(format(QStringLiteral("<a><b></a>"), QStringLiteral("  "), &why).isEmpty());
        QVERIFY(!why.isEmpty());
        QVERIFY(minify(QStringLiteral("<a>")).isEmpty());
        QCOMPARE(escapeText(QStringLiteral("a<b&c]]>")), QStringLiteral("a&lt;b&amp;c]]&gt;"));
        QCOMPARE(escapeAttribute(QStringLiteral("say \"hi\"")), QStringLiteral("say &quot;hi&quot;"));
        QVERIFY(isName(QStringLiteral("x:price")) && isName(QStringLiteral("_a.b-c")));
        QVERIFY(!isName(QStringLiteral("1a")) && !isName(QStringLiteral("a b")) && !isName(QString()));
        QVERIFY(isXmlFile("a.xml") && isXmlFile("b.PLIST") && isXmlFile("c.xsd") && !isXmlFile("d.svg") && !isXmlFile("e.sch"));
        QCOMPARE(qucs_s::syntax::languageFor("Info.plist"), int(LANG_XML));
    }

    // In a tab: an XML file opens as one, beside its tree; well formed and
    // its elements counted; the tree's nodes, names and values; a node
    // chosen in the tree chosen in the text, the cursor's in the tree, its
    // path above, a step clicked going there; the modes, the last kept.
    void theTextBesideItsTree()
    {
        XmlDoc* doc = open("catalog.xml", kCatalog);
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->mode(), XmlDoc::Mode::Split);
        QVERIFY(doc->tree()->isVisible());
        QVERIFY2(doc->statusLabel()->text().contains("Well formed") && doc->statusLabel()->text().contains("8 elements"),
                 qPrintable(doc->statusLabel()->text()));
        QVERIFY(doc->diagnostics().isEmpty());
        QAbstractItemModel* m = doc->treeModel();
        QCOMPARE(m->rowCount(), 3);   // the declaration, the comment, catalog
        const QModelIndex catalog = m->index(2, 0);
        QVERIFY(doc->tree()->isExpanded(doc->treeFilter()->mapFromSource(catalog)));   // (open at first)
        QCOMPARE(m->data(catalog).toString(), QStringLiteral("catalog"));
        QCOMPARE(m->rowCount(catalog), 4);   // @xmlns:x, book, book, ?render
        QCOMPARE(m->data(m->index(0, 0, catalog)).toString(), QStringLiteral("@xmlns:x"));
        QCOMPARE(m->data(m->index(0, 1, catalog)).toString(), QStringLiteral("urn:x"));
        const QModelIndex book1 = m->index(1, 0, catalog);
        QCOMPARE(m->data(book1.siblingAtColumn(1)).toString(), QStringLiteral("id=\"b1\" lang=\"en\""));
        const QModelIndex title = m->index(2, 0, book1);
        QCOMPARE(m->data(title.siblingAtColumn(1)).toString(), QStringLiteral("Circuits & Systems"));   // (its text)
        QVERIFY(m->data(title, Qt::ToolTipRole).toString().contains("/catalog/book[1]/title"));
        QVERIFY(!m->data(title, Qt::DecorationRole).value<QIcon>().isNull());

        // The tree chooses: the text follows.
        const Parsed& p = doc->parsed();
        const int b2 = at(p, "/catalog/book[2]/@id");
        doc->tree()->setCurrentIndex(doc->treeFilter()->mapFromSource(doc->treeModel()->indexOf(b2)));
        QCOMPARE(doc->textCursor().selectedText(), QStringLiteral("b2"));
        QVERIFY(doc->pathLabel()->text().contains(">book<") && doc->pathLabel()->text().contains("@id"));
        // The cursor moves: the tree follows.
        moveCursor(doc, int(doc->toPlainText().indexOf("Signals")) + 2);
        QCOMPARE(doc->treeModel()->nodeOf(doc->treeFilter()->mapToSource(doc->tree()->currentIndex())),
                 at(doc->parsed(), "/catalog/book[2]/title/text()"));
        QCOMPARE(doc->pathAtCursor(), QStringLiteral("/catalog/book[2]/title/text()"));
        QVERIFY(doc->pathLabel()->text().contains("catalog") && doc->pathLabel()->text().contains("title"));
        // A step of the path: its element chosen.
        emit doc->pathLabel()->linkActivated(QStringLiteral("node:%1").arg(at(doc->parsed(), "/catalog/book[2]")));
        QVERIFY(doc->textCursor().selectedText().startsWith("<book id=\"b2\">"));

        // The modes, and the last one kept for the next file.
        doc->findChild<QToolButton*>("xmlText")->click();
        QCOMPARE(doc->mode(), XmlDoc::Mode::Text);
        QVERIFY(!doc->tree()->isVisible());
        doc->findChild<QToolButton*>("xmlTree")->click();
        QCOMPARE(doc->mode(), XmlDoc::Mode::Tree);
        QVERIFY(doc->tree()->isVisible());
        QCOMPARE(XmlDoc::defaultMode(), XmlDoc::Mode::Tree);
        doc->findChild<QToolButton*>("xmlSplit")->click();
        QCOMPARE(XmlDoc::defaultMode(), XmlDoc::Mode::Split);
        closeAll();
    }

    // Edited in the tree: a value (escaped, its quotes kept), a text, an
    // element renamed (both tags), an attribute; added, deleted. Each one
    // step to undo. What cannot be (a name that is none, a second
    // attribute of a name): refused, nothing changed.
    void editedInTheTree()
    {
        XmlDoc* doc = open("edit.xml", kCatalog);
        QVERIFY(doc != nullptr);
        const QString original = doc->toPlainText();
        auto* m = doc->treeModel();
        const auto node = [doc](const char* path) { return at(doc->parsed(), QString::fromLatin1(path)); };

        // An attribute's value, in single quotes: an apostrophe escaped so.
        QVERIFY(m->setData(m->indexOf(node("/catalog/book[1]/@lang"), 1), QStringLiteral("it's \"fr\""), Qt::EditRole));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("lang='it&apos;s \"fr\"'")));
        QCOMPARE(doc->parsed().nodes.at(node("/catalog/book[1]/@lang")).value, QStringLiteral("it's \"fr\""));
        doc->undo();
        QCOMPARE(doc->toPlainText(), original);
        // A text, escaped.
        QVERIFY(doc->setValue(node("/catalog/book[2]/title/text()"), QStringLiteral("Noise < Signal")));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("<title>Noise &lt; Signal</title>")));
        QVERIFY(doc->parsed().wellFormed);
        // An element renamed: both its tags, one undo.
        QVERIFY(m->setData(m->indexOf(node("/catalog/book[2]/title"), 0), QStringLiteral("heading"), Qt::EditRole));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("<heading>Noise &lt; Signal</heading>")));
        doc->undo();
        QVERIFY(doc->toPlainText().contains(QStringLiteral("<title>Noise &lt; Signal</title>")));
        doc->undo();
        QCOMPARE(doc->toPlainText(), original);
        // An attribute renamed; a name taken refused.
        QVERIFY(doc->rename(node("/catalog/book[1]/@lang"), QStringLiteral("language")));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("language='en'")));
        QString why;
        QVERIFY(!doc->rename(node("/catalog/book[1]/@language"), QStringLiteral("id"), &why));
        QVERIFY(why.contains("already"));
        QVERIFY(!doc->rename(node("/catalog/book[1]"), QStringLiteral("two words"), &why));
        QVERIFY(!doc->setValue(node("/catalog/book[1]"), QStringLiteral("x"), &why));
        QVERIFY(!doc->setValue(node("/comment()"), QStringLiteral("a -- b"), &why));
        doc->undo();
        QCOMPARE(doc->toPlainText(), original);

        // Added: an attribute (an empty element's too), an element in one with
        // content (on a line of its own), in an empty one (opened).
        QVERIFY(doc->addAttribute(node("/catalog/book[2]"), QStringLiteral("year"), QStringLiteral("2026")));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("<book id=\"b2\" year=\"2026\">")));
        QVERIFY(doc->addAttribute(node("/catalog/book[2]/empty"), QStringLiteral("x"), QStringLiteral("1")));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("<empty x=\"1\"/>")));
        QVERIFY(!doc->addAttribute(node("/catalog/book[2]"), QStringLiteral("year"), QStringLiteral("2027"), &why));
        QVERIFY(doc->addElement(node("/catalog/book[1]"), QStringLiteral("isbn")));
        QVERIFY2(doc->toPlainText().contains(QStringLiteral("    <x:price>42</x:price>\n    <isbn/>\n  </book>")), qPrintable(doc->toPlainText()));
        QVERIFY(doc->addElement(node("/catalog/book[2]/empty"), QStringLiteral("inner")));
        QVERIFY2(doc->toPlainText().contains(QStringLiteral("    <empty x=\"1\">\n      <inner/>\n    </empty>")), qPrintable(doc->toPlainText()));
        QVERIFY(!doc->addElement(0, QStringLiteral("second"), &why));   // (one root)
        QVERIFY(doc->parsed().wellFormed);
        // Deleted: an attribute (its space too), an element (its line).
        QVERIFY(doc->removeNode(node("/catalog/book[2]/@year")));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("<book id=\"b2\">")));
        QVERIFY(doc->removeNode(node("/catalog/book[1]/isbn")));
        QVERIFY(doc->toPlainText().contains(QStringLiteral("    <x:price>42</x:price>\n  </book>")));
        QVERIFY(doc->parsed().wellFormed);
        for (int k = 0; k < 6; ++k) doc->undo();
        QCOMPARE(doc->toPlainText(), original);

        // The tree's menu.
        QMenu* menu = doc->treeMenuFor(node("/catalog/book[1]/@id"));
        for (const char* name : {"xmlCopyPath", "xmlCopyValue", "xmlEdit", "xmlRename", "xmlDelete"})
            QVERIFY2(menu->findChild<QAction*>(QLatin1String(name))->isEnabled(), name);
        QVERIFY(!menu->findChild<QAction*>("xmlAddElement")->isEnabled());
        menu->findChild<QAction*>("xmlCopyPath")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("/catalog/book[1]/@id"));
        menu->findChild<QAction*>("xmlCopyValue")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("b1"));
        delete menu;
        menu = doc->treeMenuFor(node("/catalog/book[1]"));
        QVERIFY(menu->findChild<QAction*>("xmlAddAttribute")->isEnabled() && menu->findChild<QAction*>("xmlAddElement")->isEnabled());
        QVERIFY(!menu->findChild<QAction*>("xmlEdit")->isEnabled());
        delete menu;
        closeAll();
    }

    // Typing: '>' after a start tag writes its end tag after the cursor;
    // "</" the end tag of the element open there; Return between the tags
    // indents; Command+/ (Ctrl+/) comments lines out and back in.
    void typingHelps()
    {
        XmlDoc* doc = open("typing.xml", "<root>\n  \n</root>\n");
        QVERIFY(doc != nullptr);
        doc->setFocus();
        moveCursor(doc, 9);   // (the empty line, indented)
        QTest::keyClicks(doc, "<item a=\"1\">");
        QCOMPARE(doc->toPlainText(), QStringLiteral("<root>\n  <item a=\"1\"></item>\n</root>\n"));
        QCOMPARE(doc->textCursor().position(), 9 + 12);
        QTest::keyClick(doc, Qt::Key_Return);
        QCOMPARE(doc->toPlainText(), QStringLiteral("<root>\n  <item a=\"1\">\n    \n  </item>\n</root>\n"));
        // ('>' writes </b> after the cursor; "</" typed before it steps over it.)
        QTest::keyClicks(doc, "<b>x</");
        QCOMPARE(doc->toPlainText(), QStringLiteral("<root>\n  <item a=\"1\">\n    <b>x</b>\n  </item>\n</root>\n"));
        QCOMPARE(doc->textCursor().position(), int(doc->toPlainText().indexOf("</b>")) + 4);
        doc->setPlainText(QStringLiteral("<root>\n  <a>\n    text\n  </a>\n</root>\n"));
        moveCursor(doc, int(doc->toPlainText().indexOf("text")) + 4);
        QTest::keyClicks(doc, "<");
        QTest::keyClick(doc, Qt::Key_Slash);
        QVERIFY2(doc->toPlainText().contains(QStringLiteral("text</a>\n  </a>")), qPrintable(doc->toPlainText()));
        for (int k = 0; k < 5 && doc->toPlainText() != QStringLiteral("<root>\n  <a>\n    text\n  </a>\n</root>\n"); ++k) doc->undo();
        QCOMPARE(doc->toPlainText(), QStringLiteral("<root>\n  <a>\n    text\n  </a>\n</root>\n"));
        // A self-closed tag, a declaration: nothing written.
        doc->setPlainText(QString());
        QTest::keyClicks(doc, "<?xml version=\"1.0\"?><br/>");
        QCOMPARE(doc->toPlainText(), QStringLiteral("<?xml version=\"1.0\"?><br/>"));

        // Command+/ (Ctrl+/): the lines commented out, then back in.
        doc->setPlainText(QStringLiteral("<root>\n  <a/>\n  <b/>\n</root>\n"));
        QTextCursor sel(doc->document());
        sel.setPosition(9);
        sel.setPosition(20, QTextCursor::KeepAnchor);
        doc->setTextCursor(sel);
        QTest::keyClick(doc, Qt::Key_Slash, Qt::ControlModifier);
        QCOMPARE(doc->toPlainText(), QStringLiteral("<root>\n  <!-- <a/>\n  <b/> -->\n</root>\n"));
        QVERIFY(doc->parsed().wellFormed);
        QCOMPARE(doc->parsed().elementCount(), 1);
        moveCursor(doc, 12);
        QTest::keyClick(doc, Qt::Key_Slash, Qt::ControlModifier);
        QCOMPARE(doc->toPlainText(), QStringLiteral("<root>\n  <a/>\n  <b/>\n</root>\n"));
        closeAll();
    }

    // Not well formed: the error in the text (underlined), the bar (a click
    // going there) and on the Problems tab; gone once mended. Format and
    // Minify refuse it, saying why.
    void anErrorIsShown()
    {
        XmlDoc* doc = open("broken.xml", "<a>\n  <b>one</b>\n  <c>two</d>\n</a>\n");
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->diagnostics().size(), 1);
        QCOMPARE(doc->diagnostics().first().line, 3);
        QVERIFY(doc->statusLabel()->text().contains("Line 3"));
        MessageDock* dock = app->messages();
        QVERIFY(dock != nullptr);
        QCOMPARE(dock->textProblemsDocument(), static_cast<TextDoc*>(doc));
        emit doc->statusLabel()->linkActivated(QStringLiteral("error"));
        QCOMPARE(doc->textCursor().blockNumber() + 1, 3);
        {
            misc::ErrorCapture said;
            QVERIFY(!doc->formatNow());
            QVERIFY(!doc->minifyNow());
            QCOMPARE(said.errors().size(), 2);
            QVERIFY(said.errors().first().contains("not well formed"));
        }
        // Mended: no error.
        QTextCursor c(doc->document());
        c.setPosition(int(doc->toPlainText().indexOf("</d>")) + 2);
        c.deleteChar();
        c.insertText(QStringLiteral("c"));
        doc->parseNow();
        QVERIFY(doc->diagnostics().isEmpty());
        QVERIFY(doc->statusLabel()->text().contains("Well formed"));
        // Format: one step to undo.
        doc->setPlainText(QStringLiteral("<a><b>x</b><c/></a>"));
        doc->parseNow();
        QVERIFY(doc->formatNow());
        QCOMPARE(doc->toPlainText(), QStringLiteral("<a>\n  <b>x</b>\n  <c/>\n</a>\n"));
        doc->undo();
        QCOMPARE(doc->toPlainText(), QStringLiteral("<a><b>x</b><c/></a>"));
        doc->findChild<QToolButton*>("xmlFormat")->click();
        QVERIFY(doc->toPlainText().contains("\n  <c/>\n"));
        doc->findChild<QToolButton*>("xmlMinify")->click();
        QCOMPARE(doc->toPlainText(), QStringLiteral("<a><b>x</b><c/></a>"));
        closeAll();
    }

    // Folding: an element on several lines folds at its start tag (its
    // lines hidden), opens again; all of the root's children at once.
    void elementsFold()
    {
        XmlDoc* doc = open("fold.xml", kCatalog);
        QVERIFY(doc != nullptr);
        const int book1 = doc->parsed().nodes.at(at(doc->parsed(), "/catalog/book[1]")).line;   // 4
        QVERIFY(doc->isFoldable(book1));
        QVERIFY(!doc->isFoldable(book1 + 1));   // (<title> on one line)
        doc->fold(book1);
        QVERIFY(doc->isFolded(book1));
        for (int l = book1 + 1; l <= book1 + 3; ++l) QVERIFY2(!doc->document()->findBlockByNumber(l - 1).isVisible(), qPrintable(QString::number(l)));
        QVERIFY(doc->document()->findBlockByNumber(book1 + 3).isVisible());   // (the next book)
        doc->unfold(book1);
        QVERIFY(doc->document()->findBlockByNumber(book1).isVisible());
        doc->foldAll();
        QVERIFY(doc->isFolded(book1));
        QVERIFY(doc->isFolded(doc->parsed().nodes.at(at(doc->parsed(), "/catalog/book[2]")).line));
        QVERIFY(!doc->isFolded(3));   // (the root open)
        doc->unfoldAll();
        for (QTextBlock b = doc->document()->begin(); b.isValid(); b = b.next()) QVERIFY(b.isVisible());
        closeAll();
        // Not well formed: an element left open folds to where the reading stopped.
        XmlDoc* broken = open("fold-broken.xml", QByteArray(kCatalog).replace("<x:price>42</x:price>", "<x:price>42</x:price>\n    <open>"));
        QVERIFY(broken != nullptr);
        QVERIFY(!broken->parsed().wellFormed);
        QVERIFY(broken->isFoldable(4));
        broken->fold(4);
        QVERIFY(broken->isFolded(4));
        // (one book read: "/catalog/book", no [1] - the message is made
        // whether or not it fails)
        QVERIFY2(!broken->document()->findBlockByNumber(4).isVisible(), qPrintable(QString("the reading stopped at line %1").arg(broken->parsed().errorLine)));
        closeAll();
    }

    // The tree's filter: the nodes whose name or value has the text, with
    // the elements they are in; cleared, all.
    void theTreeIsFiltered()
    {
        XmlDoc* doc = open("filter.xml", kCatalog);
        QVERIFY(doc != nullptr);
        doc->filterEdit()->setText(QStringLiteral("signals"));
        QAbstractItemModel* shown = doc->treeFilter();
        QCOMPARE(shown->rowCount(), 1);   // catalog
        const QModelIndex catalog = shown->index(0, 0);
        QCOMPARE(shown->rowCount(catalog), 1);   // book[2]
        const QModelIndex book = shown->index(0, 0, catalog);
        QCOMPARE(shown->rowCount(book), 1);   // title
        doc->filterEdit()->setText(QStringLiteral("b1"));
        QCOMPARE(shown->rowCount(shown->index(0, 0)), 1);
        doc->filterEdit()->clear();
        QCOMPARE(shown->rowCount(), 3);
        closeAll();
    }

    // A large file - a thousand elements wide, ten thousand in all: read,
    // its tree shown and kept open as it is edited, quickly.
    void aLargeFile()
    {
        QString text = QStringLiteral("<root>\n");
        for (int k = 0; k < 1000; ++k) {
            text += QStringLiteral("  <item n=\"%1\">\n").arg(k);
            for (int j = 0; j < 9; ++j) text += QStringLiteral("    <v>%1</v>\n").arg(j);
            text += QStringLiteral("  </item>\n");
        }
        text += QStringLiteral("</root>\n");
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer)) || !defined(NDEBUG)
        const qint64 slower = 10;   // (a build with sanitizers, or for debugging: CI's Linux edit took 3.4-3.7 s)
#else
        const qint64 slower = 1;
#endif
        QElapsedTimer clock;
        clock.start();
        XmlDoc* doc = open("large.xml", text.toUtf8());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->parsed().elementCount(), 10001);
        const qint64 readMs = clock.elapsed();
        QVERIFY2(readMs < 5000 * slower, qPrintable(QString::number(readMs)));
        // The 500th item open in the tree; an edit; read again: still open.
        const int item = at(doc->parsed(), "/root/item[500]");
        doc->tree()->expand(doc->treeFilter()->mapFromSource(doc->treeModel()->indexOf(item)));
        clock.restart();
        QVERIFY(doc->setValue(at(doc->parsed(), "/root/item[500]/@n"), QStringLiteral("five hundred")));
        const qint64 editMs = clock.elapsed();
        qInfo("A large file: opened in %lld ms, an edit in %lld ms", readMs, editMs);
        QVERIFY2(editMs < 3000 * slower, qPrintable(QString::number(editMs)));
        QVERIFY(doc->tree()->isExpanded(doc->treeFilter()->mapFromSource(doc->treeModel()->indexOf(at(doc->parsed(), "/root/item[500]")))));
        closeAll();
    }

    // Claude's xml tool: a file opened in the editor; its outline (paths,
    // attributes, text), a check, a node got, set, renamed, added, deleted -
    // each one step to undo, not saved; format and minify; what cannot be.
    void claudesXmlTool()
    {
        auto* control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const auto call = [control](const QJsonObject& args) { return control->callNow("xml", args, 30000); };
        const auto json = [](const QJsonObject& r) {
            const QString first = r.value("content").toArray().first().toObject().value("text").toString();
            return QJsonDocument::fromJson(first.toUtf8()).object();
        };
        const auto failed = [](const QJsonObject& r) { return r.value("isError").toBool(); };
        const auto text = [](const QJsonObject& r) { return QucsControl::textOf(r); };
        QVERIFY(writeBytes(dir.filePath("ws/claude.xml"), kCatalog));
        QJsonObject r = call({{"path", "claude.xml"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* doc = dynamic_cast<XmlDoc*>(app->getDoc());
        QVERIFY(doc != nullptr);
        QJsonObject o = json(r);
        QVERIFY(o.value("well formed").toBool());
        QCOMPARE(o.value("elements").toInt(), 8);
        const QJsonArray nodes = o.value("nodes").toArray();
        QStringList listed;
        for (const QJsonValue& v : nodes) listed << v.toObject().value("path").toString();
        QVERIFY2(listed.contains("/catalog/book[1]") && listed.contains("/catalog/book[2]/title") && listed.contains("/comment()"),
                 qPrintable(listed.join(' ')));
        for (const QJsonValue& v : nodes)
            if (v.toObject().value("path").toString() == "/catalog/book[1]")
                QCOMPARE(v.toObject().value("attributes").toObject().value("lang").toString(), QStringLiteral("en"));
            else if (v.toObject().value("path").toString() == "/catalog/book[1]/title")
                QCOMPARE(v.toObject().value("text").toString(), QStringLiteral("Circuits & Systems"));
        o = json(call({{"path", "claude.xml"}, {"depth", 1}, {"max", 2}}));
        QCOMPARE(o.value("nodes").toArray().size(), 2);
        QVERIFY(o.value("more").toString().contains("more"));

        o = json(call({{"path", "claude.xml"}, {"action", "get"}, {"node", "/catalog/book[2]/@id"}}));
        QCOMPARE(o.value("value").toString(), QStringLiteral("b2"));
        QCOMPARE(o.value("text in the file").toString(), QStringLiteral("id=\"b2\""));
        r = call({{"path", "claude.xml"}, {"action", "set"}, {"node", "/catalog/book[2]/title/text()"}, {"value", "A & B"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("unsaved").toBool());
        QVERIFY(doc->toPlainText().contains("<title>A &amp; B</title>"));
        QVERIFY(!failed(call({{"path", "claude.xml"}, {"action", "rename"}, {"node", "/catalog/book[2]/note"}, {"value", "remark"}})));
        QVERIFY(doc->toPlainText().contains("<remark><![CDATA[a < b]]></remark>"));
        QVERIFY(!failed(call({{"path", "claude.xml"}, {"action", "add_attribute"}, {"node", "/catalog/book[2]"}, {"name", "year"}, {"value", "2026"}})));
        QVERIFY(!failed(call({{"path", "claude.xml"}, {"action", "add_element"}, {"node", "/catalog/book[2]"}, {"name", "isbn"}})));
        QVERIFY(doc->toPlainText().contains("<book id=\"b2\" year=\"2026\">") && doc->toPlainText().contains("    <isbn/>\n  </book>"));
        QVERIFY(!failed(call({{"path", "claude.xml"}, {"action", "delete"}, {"node", "/catalog/book[2]/empty"}})));
        QVERIFY(!doc->toPlainText().contains("<empty/>"));
        QVERIFY(json(call({{"path", "claude.xml"}, {"action", "check"}})).value("well formed").toBool());
        // Each a step to undo; nothing written to the file.
        for (int k = 0; k < 5; ++k) doc->undo();
        QCOMPARE(doc->toPlainText(), QString::fromUtf8(kCatalog));
        QCOMPARE(readBytes(dir.filePath("ws/claude.xml")), QByteArray(kCatalog));
        // Minify, format.
        QVERIFY(!failed(call({{"path", "claude.xml"}, {"action", "minify"}})));
        QVERIFY(!doc->toPlainText().contains("\n  <book"));
        QVERIFY(!failed(call({{"path", "claude.xml"}, {"action", "format"}})));
        QVERIFY(doc->toPlainText().contains("\n  <book id=\"b1\" lang='en'>\n"));
        // What cannot be.
        QVERIFY(failed(call({{"path", "claude.xml"}, {"action", "get"}, {"node", "/catalog/nothing"}})));
        QVERIFY(failed(call({{"path", "claude.xml"}, {"action", "set"}})));
        QVERIFY(failed(call({{"path", "claude.xml"}, {"action", "rename"}, {"node", "/catalog"}, {"value", "two words"}})));
        QVERIFY(failed(call({{"path", "claude.xml"}, {"action", "fly"}})));
        QVERIFY(failed(call({{"path", "none.xml"}})));
        QVERIFY(writeBytes(dir.filePath("ws/notxml.txt"), "hello"));
        QVERIFY(failed(call({{"path", "notxml.txt"}})));
        // Not well formed: said; format refused.
        doc->setPlainText(QStringLiteral("<a><b></a>"));
        o = json(call({{"path", "claude.xml"}, {"action", "check"}}));
        QVERIFY(!o.value("well formed").toBool());
        QCOMPARE(o.value("error").toObject().value("line").toInt(), 1);
        r = call({{"path", "claude.xml"}, {"action", "format"}});
        QVERIFY(failed(r));
        QVERIFY(text(r).contains("not well formed"));
        QVERIFY(!control->readOnlyTools().contains("xml"));
        closeAll();
    }

    // Its encoding: a Latin-1 file read as one, written back so; other
    // suffixes (.plist, .xsd) open as XML.
    void itsEncodingAndKinds()
    {
        const QByteArray latin = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<r v=\"caf\xe9\">\xb5V</r>\n";
        XmlDoc* doc = open("latin.xml", latin);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->toPlainText().contains(QStringLiteral("café")));
        QVERIFY(doc->parsed().wellFormed);
        QVERIFY(doc->setValue(at(doc->parsed(), "/r/text()"), QStringLiteral("µA")));
        QVERIFY(app->saveFile(doc));
        QCOMPARE(readBytes(dir.filePath("ws/latin.xml")), QByteArray("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<r v=\"caf\xe9\">\xb5" "A</r>\n"));
        closeAll();
        for (const char* name : {"Info.plist", "schema.xsd"}) {
            XmlDoc* other = open(QString::fromLatin1(name), "<a><b/></a>\n");
            QVERIFY2(other != nullptr, name);
            QCOMPARE(other->parsed().elementCount(), 2);
            closeAll();
        }
    }
};

QTEST_MAIN(TestXmlDoc)
#include "test_xml_doc.moc"
