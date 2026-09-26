/*
 * A spreadsheet in a tab: a CSV file or an Excel workbook in a table, the
 * cell in front edited above it or in it, edits undone and redone, cells
 * cut, copied and pasted as tab-separated text and cleared, a CSV file's
 * rows and columns inserted and deleted; saved in its own manner, saved as
 * another; a workbook's sheets in tabs, its merged cells and widths; the
 * Content panel opening them (and Markdown) here whatever text editor the
 * settings name.
 */
#include <QtTest>
#include <QClipboard>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "markdowndoc.h"
#include "sheetdoc.h"
#include "spreadsheet.h"
#include "extsimkernels/spicecompat.h"
#include "excel_fixture.h"
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

QString shown(SheetDoc* doc, int row, int column)
{
    return doc->view()->model()->data(doc->view()->model()->index(row, column), Qt::DisplayRole).toString();
}

QByteArray contents(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void select(SheetDoc* doc, int top, int left, int bottom, int right)
{
    QAbstractItemModel* m = doc->view()->model();
    doc->view()->setCurrentIndex(m->index(top, left));
    doc->view()->selectionModel()->select(QItemSelection(m->index(top, left), m->index(bottom, right)),
                                         QItemSelectionModel::ClearAndSelect);
}
} // namespace

class TestSheetDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QByteArray& bytes)
    {
        QFile f(dir.filePath(name));
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        return f.fileName();
    }

    SheetDoc* current(QucsApp& app)
    {
        return qobject_cast<SheetDoc*>(app.DocumentTab->currentWidget());
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

    // A CSV file in a table: its cells, its columns' letters, the cell in
    // front above it; no sheet tabs; rows and columns may be changed.
    void aCsvFileOpensInATable()
    {
        const QString file = write("parts.csv", "part;value;note\nR1;1000;\"a;b\"\nC1;1e-9;\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        SheetDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QVERIFY(QucsApp::isSheetDocument(doc));
        QVERIFY(!doc->isWorkbook());
        QCOMPARE(shown(doc, 0, 0), QString("part"));
        QCOMPARE(shown(doc, 1, 2), QString("a;b"));
        QCOMPARE(shown(doc, 2, 1), QString("1e-9"));
        QCOMPARE(doc->view()->model()->headerData(1, Qt::Horizontal).toString(), QString("B"));
        QVERIFY(doc->view()->model()->rowCount() >= 100);   // rows to type into
        QCOMPARE(doc->findChild<QLabel*>("sheetCellName")->text(), QString("A1"));
        QCOMPARE(doc->cellEditor()->text(), QString("part"));
        QVERIFY(doc->sheetTabs()->isHidden());
        QVERIFY(doc->findChild<QToolButton*>("sheetInsertRow")->isEnabled());
        QCOMPARE(int(doc->view()->model()->data(doc->view()->model()->index(1, 1), Qt::TextAlignmentRole).toInt()
                     & Qt::AlignRight), int(Qt::AlignRight));   // a number
        app.closeAllFiles();
    }

    // Edited in the table and above it; undone and redone from the Edit
    // menu; saved as it was read (its delimiter, its quotes).
    void editsAreUndoneAndSaved()
    {
        const QString file = write("edit.csv", "part;value\nR1;1000\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        SheetDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QAbstractItemModel* m = doc->view()->model();

        QVERIFY(m->setData(m->index(1, 1), "2200", Qt::EditRole));
        QCOMPARE(shown(doc, 1, 1), QString("2200"));
        QVERIFY(doc->getDocChanged());
        doc->view()->setCurrentIndex(m->index(2, 0));
        QCOMPARE(doc->findChild<QLabel*>("sheetCellName")->text(), QString("A3"));
        doc->cellEditor()->setText("C1; big");
        doc->cellEditor()->setModified(true);
        QMetaObject::invokeMethod(doc->cellEditor(), "returnPressed");
        QCOMPARE(shown(doc, 2, 0), QString("C1; big"));
        QCOMPARE(doc->view()->currentIndex().row(), 3);   // down to the next row

        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditUndo"));
        QCOMPARE(shown(doc, 2, 0), QString());
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditUndo"));
        QCOMPARE(shown(doc, 1, 1), QString("1000"));
        QVERIFY(!doc->getDocChanged());                     // back as it was saved
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditRedo"));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditRedo"));
        QCOMPARE(shown(doc, 2, 0), QString("C1; big"));

        QCOMPARE(doc->save(), 0);
        QCOMPARE(contents(file), QByteArray("part;value\nR1;2200\n\"C1; big\"\n"));
        QVERIFY(!doc->getDocChanged());
        app.closeAllFiles();
    }

    // Copied as tab-separated text, pasted, one value into a range, cut,
    // cleared with Delete.
    void cellsAreCopiedPastedAndCleared()
    {
        const QString file = write("clip.csv", "a,b,c\n1,2,3\n4,5,6\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QVERIFY(app.gotoPage(file));
        SheetDoc* doc = current(app);
        QVERIFY(doc != nullptr);

        select(doc, 1, 0, 2, 1);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditCopy"));
        QCOMPARE(QApplication::clipboard()->text(), QString("1\t2\n4\t5\n"));
        doc->view()->setCurrentIndex(doc->view()->model()->index(4, 1));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditPaste", Q_ARG(bool, true)));
        QCOMPARE(shown(doc, 4, 1), QString("1"));
        QCOMPARE(shown(doc, 5, 2), QString("5"));

        QApplication::clipboard()->setText("x");
        select(doc, 0, 0, 0, 2);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditPaste", Q_ARG(bool, true)));
        QCOMPARE(shown(doc, 0, 0) + shown(doc, 0, 1) + shown(doc, 0, 2), QString("xxx"));

        select(doc, 2, 2, 2, 2);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEditCut"));
        QCOMPARE(QApplication::clipboard()->text(), QString("6\n"));
        QCOMPARE(shown(doc, 2, 2), QString());

        select(doc, 1, 0, 1, 1);
        doc->view()->setFocus();
        QTest::keyClick(doc->view(), Qt::Key_Delete);
        QCOMPARE(shown(doc, 1, 0) + shown(doc, 1, 1), QString());
        QCOMPARE(shown(doc, 1, 2), QString("3"));
        doc->undoStack()->setClean();
        app.closeAllFiles();
    }

    // A CSV file's rows and columns inserted and deleted, and undone.
    void rowsAndColumnsOfACsvFile()
    {
        const QString file = write("shape.csv", "a,b,c\n1,2,3\n4,5,6\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        SheetDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QAbstractItemModel* m = doc->view()->model();

        doc->view()->setCurrentIndex(m->index(1, 1));
        doc->insertRow();
        QCOMPARE(shown(doc, 1, 0), QString());
        QCOMPARE(shown(doc, 2, 0), QString("1"));
        doc->insertColumn();
        QCOMPARE(shown(doc, 0, 1), QString());
        QCOMPARE(shown(doc, 0, 2), QString("b"));
        select(doc, 2, 0, 3, 0);
        doc->deleteRows();
        QCOMPARE(shown(doc, 2, 0), QString());   // past the rows there were
        select(doc, 0, 2, 0, 3);
        doc->deleteColumns();
        QCOMPARE(shown(doc, 0, 1), QString());   // the inserted one
        QCOMPARE(shown(doc, 0, 2), QString());
        for (int i = 0; i < 4; ++i) doc->undo();
        QCOMPARE(qucs_s::sheet::writeCsv(doc->sheet(), doc->workbook()), QByteArray("a,b,c\n1,2,3\n4,5,6\n"));
        app.closeAllFiles();
    }

    // A workbook: its sheets in tabs, its merged cells and widths, its
    // formulas shown by their values; rows and columns not changed; a cell
    // changed and saved, the rest as it was.
    void aWorkbookOpensWithItsSheets()
    {
        const QString file = write("book.xlsx", excelLike());
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        SheetDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->isWorkbook());
        QVERIFY(!doc->sheetTabs()->isHidden());
        QCOMPARE(doc->sheetTabs()->count(), 2);
        QCOMPARE(doc->sheetTabs()->tabText(1), QString("Parts && Values"));   // shows "Parts & Values"
        QVERIFY(!doc->findChild<QToolButton*>("sheetInsertRow")->isEnabled());

        QCOMPARE(shown(doc, 1, 2), QString("300"));
        QAbstractItemModel* m = doc->view()->model();
        QCOMPARE(m->data(m->index(1, 2), Qt::EditRole).toString(), QString("=A2*B2"));
        QCOMPARE(shown(doc, 2, 0), QString("2024-05-01"));
        QCOMPARE(doc->view()->columnSpan(4, 1), 2);
        QVERIFY(doc->view()->columnWidth(0) > doc->view()->columnWidth(1));

        doc->sheetTabs()->setCurrentIndex(1);
        QCOMPARE(doc->currentSheet(), 1);
        QCOMPARE(shown(doc, 1, 0), QString("C1"));
        doc->sheetTabs()->setCurrentIndex(0);

        doc->view()->setCurrentIndex(m->index(1, 1));
        doc->cellEditor()->setText("0.5");
        doc->cellEditor()->setModified(true);
        QMetaObject::invokeMethod(doc->cellEditor(), "returnPressed");
        QCOMPARE(doc->save(), 0);
        QVERIFY(doc->isWorkbook());
        qucs_s::sheet::Workbook saved;
        QVERIFY(qucs_s::sheet::readXlsx(contents(file), saved));
        QCOMPARE(saved.sheets[0].at(1, 1).text, QString("0.5"));
        QCOMPARE(saved.sheets[0].at(1, 2).formula, QString("A2*B2"));
        QCOMPARE(saved.sheets[1].at(0, 0).text, QString("R1"));
        QCOMPARE(shown(doc, 1, 1), QString("0.5"));   // read again, still shown
        app.closeAllFiles();
    }

    // Save As another format: a CSV file as a workbook, and back; the
    // autosave copy in the document's own format.
    void savedAsAnotherFormat()
    {
        const QString file = write("convert.csv", "part,value\nR1,1000\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        SheetDoc* doc = current(app);
        QVERIFY(doc != nullptr);
        const QString xlsx = dir.filePath("convert.xlsx");
        QVERIFY(app.saveDocumentAs(doc, xlsx));
        QVERIFY(doc->isWorkbook());
        qucs_s::sheet::Workbook book;
        QVERIFY(qucs_s::sheet::readXlsx(contents(xlsx), book));
        QCOMPARE(book.sheets[0].at(1, 1).kind, qucs_s::sheet::Cell::Kind::Number);

        const QString copy = dir.filePath("copy.xlsx.part");
        QVERIFY(doc->writeTo(copy));
        QVERIFY(qucs_s::sheet::readXlsx(contents(copy), book));   // a workbook, whatever its name

        const QString tsv = dir.filePath("convert.tsv");
        QVERIFY(app.saveDocumentAs(doc, tsv));
        QCOMPARE(contents(tsv), QByteArray("part\tvalue\nR1\t1000\n"));
        QVERIFY(!doc->isWorkbook());
        app.closeAllFiles();
    }

    // The Content panel opens spreadsheets and Markdown in their tabs,
    // though the settings name another text editor.
    void theContentPanelOpensThemHere()
    {
        const QString csv = write("panel.csv", "a,b\n");
        const QString md = write("panel.md", "# Title\n");
        const QString editor = QucsSettings.Editor;
        QucsSettings.Editor = "/nonexistent/editor";
        QucsApp app(false);
        MainGuard guard(&app);
        app.openFileFromProjectView(QFileInfo(csv), QString());
        QVERIFY(current(app) != nullptr);
        app.openFileFromProjectView(QFileInfo(md), QString());
        QVERIFY(qobject_cast<MarkdownDoc*>(app.DocumentTab->currentWidget()) != nullptr);
        QucsSettings.Editor = editor;
        QVERIFY(QucsApp::isSheetFile("a/b.XLSX"));
        QVERIFY(!QucsApp::isSheetFile("a/b.txt"));
        app.closeAllFiles();
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestSheetDoc test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_sheet_doc.moc"
