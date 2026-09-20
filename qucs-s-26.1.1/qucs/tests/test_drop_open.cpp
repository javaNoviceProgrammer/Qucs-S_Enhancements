/*
 * Drag and drop from the Content panel into the document area: the drag
 * carries the selected files as URLs, and a drop on the tab widget, on a
 * schematic or on a text document opens each file in its viewer
 * (schematic/data display/symbol view, the text editor for text) once the
 * drop event is over.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "textdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "projectView.h"
#include "extsimkernels/spicecompat.h"

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

class TestDropOpen : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString project;

    QString write(const QString& path, const QByteArray& bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(bytes);
        return path;
    }

    static QMimeData* filesMime(const QStringList& files)
    {
        auto* data = new QMimeData;
        QList<QUrl> urls;
        for (const QString& f : files) urls << QUrl::fromLocalFile(f);
        data->setUrls(urls);
        return data;
    }

    // A drag entering and dropping on the widget, as the window system
    // would deliver it.
    static void drop(QWidget* target, const QStringList& files)
    {
        QScopedPointer<QMimeData> data(filesMime(files));
        const QPoint pos(10, 10);
        QDragEnterEvent enter(pos, Qt::CopyAction | Qt::MoveAction, data.data(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QVERIFY2(enter.isAccepted(), "the drag was not accepted");
        QDropEvent dropEv(pos, Qt::CopyAction | Qt::MoveAction, data.data(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &dropEv);
        QVERIFY2(dropEv.isAccepted(), "the drop was not accepted");
        QCOMPARE(dropEv.dropAction(), Qt::CopyAction);   // never a move: the source keeps its file
    }

    static QStringList openDocs(QucsApp& app)
    {
        QStringList names;
        for (int i = 0; i < app.DocumentTab->count(); ++i)
            names << QFileInfo(app.getDoc(i)->getDocName()).fileName();
        return names;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.maxUndo = 20;
        QucsSettings.Editor = "qucs";          // the built-in text editor
        QucsSettings.ContentTreeView = false;
        QucsSettings.FileTypes = {"osdi/true"}; // a harmless handler for the binary file
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();

        project = dir.filePath("drop_prj");
        QVERIFY(QDir().mkpath(project + "/models"));
        write(project + "/models/model.va", "module model(p, n);\nendmodule\n");
        write(project + "/notes.txt", "some notes\n");
        write(project + "/circuit.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n</Components>\n");
        write(project + "/shape.sym", "<Qucs Schematic " PACKAGE_VERSION ">\n<Symbol>\n</Symbol>\n");
        write(project + "/model.osdi", QByteArray("\x7f" "ELF\0\0\0\0binary", 14));
        QucsSettings.QucsWorkDir.setPath(project);
    }

    void theDragCarriesTheSelectedFilesAsUrls()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QStandardItemModel* m = view->model();
        const QModelIndex va = m->index(ProjectView::VerilogA, 0);
        const QModelIndex others = m->index(ProjectView::Others, 0);
        QVERIFY(!(m->flags(va) & Qt::ItemIsDragEnabled));                 // category rows cannot be dragged
        QVERIFY(m->flags(m->index(0, 0, va)) & Qt::ItemIsDragEnabled);    // files can

        view->selectionModel()->select(m->index(0, 0, va), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        view->selectionModel()->select(m->index(0, 0, others), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QList<QUrl> urls = view->selectedFileUrls();
        std::sort(urls.begin(), urls.end());
        QCOMPARE(urls, QList<QUrl>({QUrl::fromLocalFile(project + "/models/model.va"),
                                    QUrl::fromLocalFile(project + "/notes.txt")}));
        QVERIFY(view->dragEnabled());
        QCOMPARE(view->dragDropMode(), QAbstractItemView::DragOnly);
    }

    // A drop on the tab widget: every kind of file lands in its viewer.
    void eachFileOpensInItsViewer()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QCOMPARE(app.DocumentTab->count(), 1);   // the untitled schematic

        drop(app.DocumentTab, {project + "/models/model.va", project + "/notes.txt",
                               project + "/circuit.sch", project + "/shape.sym", project + "/model.osdi"});
        QCOMPARE(app.DocumentTab->count(), 1);   // nothing yet: opening is deferred past the event
        QTRY_COMPARE(app.DocumentTab->count(), 4);
        QTest::qWait(50);
        QCOMPARE(app.DocumentTab->count(), 4);   // the binary file did not become a text tab
        QCOMPARE(openDocs(app), QStringList({"model.va", "notes.txt", "circuit.sch", "shape.sym"}));

        QVERIFY(QucsApp::isTextDocument(app.DocumentTab->widget(0)));    // Verilog-A: the text editor
        QVERIFY(QucsApp::isTextDocument(app.DocumentTab->widget(1)));    // plain text: the text editor
        QVERIFY(!QucsApp::isTextDocument(app.DocumentTab->widget(2)));   // schematic: the schematic view
        Schematic* sym = dynamic_cast<Schematic*>(app.DocumentTab->widget(3));
        QVERIFY(sym != nullptr);
        QVERIFY(sym->getIsSymbolOnly());                                // symbol: the symbol editor
        QVERIFY(sym->getSymbolMode());
        // The untitled, unchanged document the files were dropped on is gone.
        for (int i = 0; i < app.DocumentTab->count(); ++i)
            QVERIFY(!app.getDoc(i)->getDocName().isEmpty());
        app.closeAllFiles();
    }

    // Dropping on the schematic itself (the classic target): the untitled
    // schematic under the cursor is the one that gets closed.
    void dropOnTheUntitledSchematic()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* untitled = app.currentSchematic();
        QVERIFY(untitled != nullptr);
        QVERIFY(untitled->getDocName().isEmpty());

        drop(untitled->viewport(), {project + "/circuit.sch"});
        QTRY_COMPARE(openDocs(app), QStringList({"circuit.sch"}));
        app.closeAllFiles();
    }

    // Dropping a file on a text document opens it instead of pasting its
    // path into the text.
    void dropOnATextDocumentOpensRatherThanPastes()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(project + "/notes.txt"));
        TextDoc* notes = app.findTextDoc(project + "/notes.txt");
        QVERIFY(notes != nullptr);
        const QString before = notes->toPlainText();

        drop(notes->viewport(), {project + "/models/model.va"});
        QTRY_VERIFY(app.findTextDoc(project + "/models/model.va") != nullptr);
        QCOMPARE(notes->toPlainText(), before);
        QVERIFY(!notes->getDocChanged());
        // Dropping a file that is already open just switches to it.
        drop(app.DocumentTab, {project + "/notes.txt"});
        QTRY_COMPARE(app.DocumentTab->currentWidget(), static_cast<QWidget*>(notes));
        QCOMPARE(app.DocumentTab->count(), 2);
        app.closeAllFiles();
    }

    void textDrag_isStillPastedIntoTheEditor()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(project + "/notes.txt"));
        TextDoc* notes = app.findTextDoc(project + "/notes.txt");
        QVERIFY(notes != nullptr);
        QMimeData text;
        text.setText("dropped words");
        QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(notes->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent dropEv(QPoint(5, 5), Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(notes->viewport(), &dropEv);
        QVERIFY(notes->toPlainText().contains("dropped words"));
        notes->setDocChanged(false);
        app.closeAllFiles();
    }

    void helpers()
    {
        QVERIFY(misc::isTextFile(project + "/notes.txt"));
        QVERIFY(misc::isTextFile(project + "/circuit.sch"));
        QVERIFY(!misc::isTextFile(project + "/model.osdi"));
        QVERIFY(!misc::isTextFile(project + "/missing"));
        QScopedPointer<QMimeData> data(filesMime({project + "/notes.txt"}));
        QCOMPARE(misc::localFiles(data.data()), QStringList({QDir::toNativeSeparators(project + "/notes.txt")}));
        QMimeData remote;
        remote.setUrls({QUrl("https://example.org/x.sch")});
        QVERIFY(misc::localFiles(&remote).isEmpty());
        QVERIFY(misc::localFiles(nullptr).isEmpty());
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestDropOpen test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_drop_open.moc"
