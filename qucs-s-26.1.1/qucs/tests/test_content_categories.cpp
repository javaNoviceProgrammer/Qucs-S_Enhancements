/*
 * The Content panel's categories: which of the project's files each lists,
 * by patterns of their names that Application Settings > Contents sets. A
 * file is listed under the first category, from the top, whose patterns
 * match its name; Others takes by default whatever no other category took,
 * and Text (*.txt) comes right before it. Only the patterns that differ
 * from the defaults are saved.
 */
#include <QtTest>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "projectView.h"
#include "settings.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The patterns set here are the defaults again when a test ends.
struct PatternsGuard {
    QMap<QString, QString> saved = QucsSettings.ContentPatterns;
    ~PatternsGuard() { QucsSettings.ContentPatterns = saved; }
};

QStringList childrenOf(QStandardItem* parent)
{
    QStringList names;
    for (int i = 0; parent && i < parent->rowCount(); ++i)
        names << parent->child(i, 0)->text();
    return names;
}

// The files the panel shows - their rows and every row above them shown -
// as their rows name them, in the panel's order.
QStringList visibleFiles(ProjectView* view, const QModelIndex& parent = QModelIndex())
{
    QStringList names;
    for (int row = 0; row < view->model()->rowCount(parent); ++row) {
        const QModelIndex idx = view->model()->index(row, 0, parent);
        if (view->isRowHidden(row, parent)) continue;
        if (view->isFile(idx)) names << idx.data().toString();
        else names << visibleFiles(view, idx);
    }
    return names;
}

QLineEdit* patternsEdit(QucsSettingsDialog& dlg, int category)
{
    return dlg.findChild<QLineEdit*>(QStringLiteral("contentPatterns") + ProjectView::categoryKey(category));
}
} // namespace

class TestContentCategories : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString project;

    void write(const QString& name, const QByteArray& bytes = "\n")
    {
        const QString path = project + "/" + name;
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
    }

    // What each category lists, the project's files as they are now.
    QMap<int, QStringList> listing(QucsApp& app)
    {
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QMap<int, QStringList> shown;
        for (int c = 0; c < ProjectView::CategoryCount; ++c)
            shown[c] = childrenOf(view->model()->item(c, 0));
        return shown;
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
        QucsSettings.ContentTreeView = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();

        const QString workspace = dir.filePath("workspace");
        project = workspace + "/categories_prj";
        QVERIFY(QDir().mkpath(project));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.projsDir.setPath(workspace);
        QucsSettings.QucsWorkDir.setPath(workspace);
        write("circuit.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n</Components>\n");
        write("broken.sch", "not a schematic\n");
        write("notes.txt");
        write("docs/README.TXT");
        write("readme.md");
        write("run.log");
        write("sheet.pdf");
        write("logo.png");
        write("analyse.py");
        write("Scratch/spice4qucs.cir");
        write("Scratch/log.txt");
    }

    // Typed as one likes: separated by commas, semicolons or spaces; an
    // extension alone (txt, .txt) is taken for *.txt; names with
    // wildcards, or whole, as they are.
    void patternsAreReadAsTyped()
    {
        QCOMPARE(ProjectView::parsePatterns("*.txt, *.png .pdf;log  notes*.md,Makefile*, data.csv,, *.TXT"),
                 QStringList({"*.txt", "*.png", "*.pdf", "*.log", "notes*.md", "Makefile*", "data.csv"}));
        QCOMPARE(ProjectView::parsePatterns("  "), QStringList());
        QCOMPARE(ProjectView::normalizedPatterns(".txt;md"), QString("*.txt, *.md"));
    }

    // Text lists .txt files, right before Others; a .txt in Scratch stays
    // there. Each category's name and default patterns, in order.
    void textFilesHaveACategoryBeforeOthers()
    {
        QCOMPARE(ProjectView::Text, ProjectView::Images + 1);
        QCOMPARE(ProjectView::Others, ProjectView::Text + 1);
        QCOMPARE(ProjectView::Scratch, ProjectView::Others + 1);
        QCOMPARE(ProjectView::CategoryCount, ProjectView::Scratch + 1);
        QCOMPARE(ProjectView::categoryName(ProjectView::Text), QString("Text"));
        QCOMPARE(ProjectView::defaultPatterns(ProjectView::Text), QString("*.txt"));
        QCOMPARE(ProjectView::defaultPatterns(ProjectView::Others), QString("*"));
        QCOMPARE(ProjectView::defaultPatterns(ProjectView::Datasets), QString("*.dat, *.dat.ngspice, *.dat.xyce, *.dat.spopus"));
        QVERIFY(ProjectView::defaultPatterns(ProjectView::Images).startsWith("*.png, *.jpg, "));

        PatternsGuard guard;
        QucsSettings.ContentPatterns.clear();
        QucsApp app(false);
        MainGuard mainGuard(&app);
        const auto shown = listing(app);
        QCOMPARE(app.projectView()->model()->item(ProjectView::Text, 0)->text(), QString("Text"));
        QCOMPARE(shown[ProjectView::Text], QStringList({"notes.txt", "docs/README.TXT"}));
        QCOMPARE(shown[ProjectView::Schematics], QStringList({"circuit.sch"}));
        QCOMPARE(shown[ProjectView::Images], QStringList({"logo.png"}));
        QCOMPARE(shown[ProjectView::Python], QStringList({"analyse.py"}));
        // Not a schematic: whatever takes it after Schematics (it was not
        // listed at all).
        QCOMPARE(shown[ProjectView::Others], QStringList({"broken.sch", "readme.md", "run.log", "sheet.pdf"}));
        QCOMPARE(shown[ProjectView::Scratch], QStringList({"log.txt", "spice4qucs.cir"}));
    }

    // A big folder as the project: other programs' folders (node_modules,
    // __pycache__, venv, a CMake build tree) are not walked, at most
    // misc::MaxProjectEntries files and folders are looked at - the header
    // says so -, and the look every few seconds is taken aside: a home
    // folder froze the window for half a minute, every 3 s (bug hunt
    // 2026-09-26, C3).
    void aBigFolderIsListedInPart()
    {
        const QString big = dir.filePath("big");
        const auto put = [&](const QString& name) {
            const QString path = big + "/" + name;
            QDir().mkpath(QFileInfo(path).path());
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
        };
        put("amp.sch");
        put("node_modules/pkg/index.js");
        put("__pycache__/x.pyc");
        put("venv/lib/site.py");
        put("build/CMakeCache.txt");
        put("build/objects/amp.o");
        put("models/deep.va");
        QStringList few = misc::projectFiles(QDir(big));
        few.sort();
        QCOMPARE(few, QStringList({"amp.sch", "models/deep.va"}));
        bool complete = false;
        misc::projectFiles(QDir(big), {}, &complete);
        QVERIFY(complete);

        for (int i = 0; i < misc::MaxProjectEntries + 100; ++i) put(QStringLiteral("data/f%1.txt").arg(i));
        QElapsedTimer clock;
        clock.start();
        const QStringList many = misc::projectFiles(QDir(big), {}, &complete);
        QVERIFY(!complete);
        QVERIFY(many.size() < misc::MaxProjectEntries);
        qInfo() << "a walk of the big folder:" << clock.elapsed() << "ms";

        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(big);
        const QString header = view->model()->horizontalHeaderItem(0)->text();
        QVERIFY2(header.contains("the first 20,000") || header.contains("the first 20000"), qPrintable(header));
        clock.restart();
        view->refreshIfChanged();   // looked at aside
        QVERIFY2(clock.elapsed() < 150, qPrintable(QString::number(clock.elapsed())));
        QTest::qWait(1500);          // (its look ends)
        view->setProjPath(project);
    }

    // Categories of the user's own: added, named, given patterns and put in
    // order in the Contents tab; listed after Text, before Others, taking
    // the files no category above took; saved, and read back.
    void categoriesOfYourOwn()
    {
        PatternsGuard guard;
        struct UserGuard {
            QList<ContentCategory> saved = QucsSettings.ContentUserCategories;
            ~UserGuard() { QucsSettings.ContentUserCategories = saved; }
        } userGuard;
        QucsSettings.ContentPatterns.clear();
        QucsSettings.ContentUserCategories.clear();
        write("measured/amp.s2p");
        write("filter.S4P");
        write("report_final.pdf");
        write("report.txt");

        QucsApp app(false);
        MainGuard mainGuard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QCOMPARE(ProjectView::categories().size(), int(ProjectView::CategoryCount));

        QucsSettingsDialog dlg(&app);
        auto* table = dlg.findChild<QTableWidget*>("contentUserCategories");
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 0);
        auto* add = dlg.findChild<QPushButton*>("contentAddCategory");
        QVERIFY(add != nullptr);
        add->click();
        add->click();
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(0, 0)->text(), QString("New Category"));
        QCOMPARE(table->item(1, 0)->text(), QString("New Category 2"));
        table->item(0, 0)->setText("Reports");
        table->item(0, 1)->setText("report*");
        table->item(1, 0)->setText(" Touchstone ");
        table->item(1, 1)->setText("s2p .s4p");
        table->setCurrentCell(1, 0);
        dlg.findChild<QPushButton*>("contentCategoryUp")->click();   // Touchstone first
        add->click();                                                // one left without a name: not kept
        table->item(2, 0)->setText("  ");
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));

        QCOMPARE(QucsSettings.ContentUserCategories,
                 QList<ContentCategory>({{"Touchstone", "*.s2p, *.s4p"}, {"Reports", "report*"}}));
        QCOMPARE(table->rowCount(), 2);   // as read
        QCOMPARE(table->item(1, 1)->text(), QString("report*"));
        {
            QucsSettingsFile file;
            QCOMPARE(file.value("ContentUserCategories/size").toInt(), 2);
            QCOMPARE(file.value("ContentUserCategories/1/name").toString(), QString("Touchstone"));
        }
        // In the panel, at once: after Text, before Others; the first that
        // matches takes a file (report.txt stays under Text).
        const QList<int> order = ProjectView::categories();
        QCOMPARE(order.indexOf(ProjectView::UserCategory), int(ProjectView::Text) + 1);
        QCOMPARE(order.indexOf(ProjectView::UserCategory + 1), int(ProjectView::Text) + 2);
        QCOMPARE(order.last(), int(ProjectView::Scratch));
        QStandardItemModel* m = view->model();
        QStandardItem* touchstone = m->item(ProjectView::rowOf(ProjectView::UserCategory), 0);
        QCOMPARE(touchstone->text(), QString("Touchstone"));
        QCOMPARE(childrenOf(touchstone), QStringList({"filter.S4P", "measured/amp.s2p"}));
        QCOMPARE(childrenOf(m->item(ProjectView::rowOf(ProjectView::UserCategory + 1), 0)), QStringList({"report_final.pdf"}));
        QVERIFY(childrenOf(m->item(ProjectView::rowOf(ProjectView::Text), 0)).contains("report.txt"));
        const QStringList others = childrenOf(m->item(ProjectView::rowOf(ProjectView::Others), 0));
        QVERIFY(!others.contains("filter.S4P") && !others.contains("report_final.pdf"));
        QCOMPARE(m->item(ProjectView::rowOf(ProjectView::Others), 0)->text(), QString("Others"));
        QCOMPARE(view->categoryOf(touchstone->child(0)->index()), int(ProjectView::UserCategory));
        QCOMPARE(view->categoryOf(m->index(ProjectView::rowOf(ProjectView::Others), 0)), int(ProjectView::Others));

        // Read again (a restart): the same.
        QucsSettings.ContentUserCategories.clear();
        QVERIFY(loadSettings());
        QCOMPARE(QucsSettings.ContentUserCategories.size(), 2);
        QCOMPARE(QucsSettings.ContentUserCategories.at(1).name, QString("Reports"));

        // Removed: gone from the panel, their files back where they were.
        table->setCurrentCell(0, 0);
        dlg.findChild<QPushButton*>("contentRemoveCategory")->click();
        table->setCurrentCell(0, 0);
        dlg.findChild<QPushButton*>("contentRemoveCategory")->click();
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(QucsSettings.ContentUserCategories.isEmpty());
        QCOMPARE(m->rowCount(), int(ProjectView::CategoryCount));
        QVERIFY(childrenOf(m->item(ProjectView::Others, 0)).contains("filter.S4P"));
        {
            QucsSettingsFile file;
            QVERIFY(!file.contains("ContentUserCategories/size"));
        }
        for (const char* f : {"measured/amp.s2p", "filter.S4P", "report_final.pdf", "report.txt"}) QFile::remove(project + "/" + f);
        QDir(project + "/measured").removeRecursively();
    }

    // Patterns set: a category lists what they match, the first from the
    // top wins, and a file no category takes is not listed.
    void eachCategoryListsWhatItsPatternsMatch()
    {
        PatternsGuard guard;
        ProjectView::setPatterns(ProjectView::Text, "*.txt, .md; log");
        ProjectView::setPatterns(ProjectView::Images, "*.png *.pdf");
        ProjectView::setPatterns(ProjectView::Python, "*.py, notes.*");   // above Text: its notes.txt
        ProjectView::setPatterns(ProjectView::Others, "");                // nothing else
        ProjectView::setPatterns(ProjectView::Scratch, "*.cir");
        QCOMPARE(ProjectView::patterns(ProjectView::Text), QString("*.txt, *.md, *.log"));

        QucsApp app(false);
        MainGuard mainGuard(&app);
        const auto shown = listing(app);
        QCOMPARE(shown[ProjectView::Python], QStringList({"analyse.py", "notes.txt"}));
        QCOMPARE(shown[ProjectView::Text], QStringList({"readme.md", "run.log", "docs/README.TXT"}));
        QCOMPARE(shown[ProjectView::Images], QStringList({"logo.png", "sheet.pdf"}));
        QCOMPARE(shown[ProjectView::Others], QStringList());
        QCOMPARE(shown[ProjectView::Scratch], QStringList({"spice4qucs.cir"}));
        QCOMPARE(shown[ProjectView::Schematics], QStringList({"circuit.sch"}));   // broken.sch: nowhere now

        // Set back to its default: no longer a change of the user's.
        ProjectView::setPatterns(ProjectView::Text, " .txt ");
        QVERIFY(!QucsSettings.ContentPatterns.contains("Text"));
        QCOMPARE(ProjectView::patterns(ProjectView::Text), QString("*.txt"));
    }

    // Saved and read back: only the categories whose patterns were changed
    // (a later version's new defaults reach the others).
    void onlyChangedPatternsAreSaved()
    {
        PatternsGuard guard;
        QucsSettings.ContentPatterns.clear();
        ProjectView::setPatterns(ProjectView::Text, "*.txt, *.md");
        ProjectView::setPatterns(ProjectView::Datasets, ProjectView::defaultPatterns(ProjectView::Datasets));
        QVERIFY(saveApplSettings());
        {
            QucsSettingsFile file;
            file.beginGroup("ContentPatterns");
            QCOMPARE(file.childKeys(), QStringList({"Text"}));
            QCOMPARE(file.value("Text").toString(), QString("*.txt, *.md"));
        }
        QucsSettings.ContentPatterns.clear();
        QVERIFY(loadSettings());
        QCOMPARE(ProjectView::patterns(ProjectView::Text), QString("*.txt, *.md"));
        QCOMPARE(ProjectView::patterns(ProjectView::Datasets), ProjectView::defaultPatterns(ProjectView::Datasets));

        ProjectView::setPatterns(ProjectView::Text, "*.txt");
        QVERIFY(saveApplSettings());
        QucsSettingsFile file;
        file.beginGroup("ContentPatterns");
        QCOMPARE(file.childKeys(), QStringList());
    }

    // Application Settings has a Contents tab: a row per category, in the
    // panel's order, its name and its patterns. Apply sets them (as typed,
    // tidied), saves them and lists the files again at once; the tab's
    // button, and Default Values, put the defaults back.
    void theSettingsHaveAContentsTab()
    {
        PatternsGuard guard;
        QucsSettings.ContentPatterns.clear();
        QucsApp app(false);
        MainGuard mainGuard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);

        QucsSettingsDialog dlg(&app);
        auto* tabs = dlg.findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        int contents = -1;
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Contents") contents = i;
        QVERIFY(contents >= 0);
        QWidget* tab = tabs->widget(contents);
        for (int c = 0; c < ProjectView::CategoryCount; ++c) {
            QLineEdit* edit = patternsEdit(dlg, c);
            QVERIFY2(edit != nullptr, qPrintable(ProjectView::categoryKey(c)));
            QVERIFY(tab->isAncestorOf(edit));
            QCOMPARE(edit->text(), ProjectView::defaultPatterns(c));
            bool named = false;
            for (QLabel* label : tab->findChildren<QLabel*>())
                if (label->buddy() == edit) named = label->text().startsWith(ProjectView::categoryName(c));
            QVERIFY2(named, qPrintable(ProjectView::categoryKey(c)));
            // In order, one under the other.
            if (c > 0) QVERIFY(edit->mapTo(tab, QPoint()).y() > patternsEdit(dlg, c - 1)->mapTo(tab, QPoint()).y());
        }

        // (An Apply first, so the patterns are all the next one changes:
        // a change of some settings rebuilds the panel anyway.)
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        patternsEdit(dlg, ProjectView::Text)->setText("*.txt, .md");
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QCOMPARE(ProjectView::patterns(ProjectView::Text), QString("*.txt, *.md"));
        QCOMPARE(patternsEdit(dlg, ProjectView::Text)->text(), QString("*.txt, *.md"));   // tidied
        {
            QucsSettingsFile file;
            QCOMPARE(file.value("ContentPatterns/Text").toString(), QString("*.txt, *.md"));   // saved
        }
        QVERIFY(childrenOf(view->model()->item(ProjectView::Text, 0)).contains("readme.md"));   // at once

        patternsEdit(dlg, ProjectView::Others)->setText("*.pdf");
        QPushButton* restore = nullptr;
        for (QPushButton* b : tab->findChildren<QPushButton*>())
            if (b->text().contains("Default")) restore = b;
        QVERIFY(restore != nullptr);
        restore->click();
        for (int c = 0; c < ProjectView::CategoryCount; ++c)
            QCOMPARE(patternsEdit(dlg, c)->text(), ProjectView::defaultPatterns(c));

        patternsEdit(dlg, ProjectView::Images)->setText("*.png");
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotDefaultValues"));
        QCOMPARE(patternsEdit(dlg, ProjectView::Images)->text(), ProjectView::defaultPatterns(ProjectView::Images));
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(QucsSettings.ContentPatterns.isEmpty());
        QVERIFY(!childrenOf(view->model()->item(ProjectView::Text, 0)).contains("readme.md"));
    }

    // Above the panel, the File Browser's filter: the files whose names
    // hold what is typed, whatever its case - a folder's name finds what
    // is in it -, in their categories and folders, open; the header says
    // how many. A row it hides is no longer selected. It stays through a
    // refresh, and cleared, every row is back, open as it was.
    void aFilterByNameAsTheFileBrowsers()
    {
        PatternsGuard guard;
        QucsSettings.ContentPatterns.clear();
        QucsApp app(false);
        MainGuard mainGuard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        auto* box = app.findChild<QLineEdit*>("contentFilter");
        QVERIFY(box != nullptr);
        auto* browsers = app.findChild<QLineEdit*>("fbFilter");
        QVERIFY(browsers != nullptr);
        QCOMPARE(box->placeholderText(), browsers->placeholderText());
        QCOMPARE(box->placeholderText(), QString("Filter by name"));
        QVERIFY(box->isClearButtonEnabled());
        QCOMPARE(box->actions().size(), browsers->actions().size());   // the magnifier
        // On the Content tab, above the panel.
        QWidget* page = nullptr;
        for (auto* tabs : app.findChildren<QTabWidget*>())
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == "Content") page = tabs->widget(i);
        QVERIFY(page != nullptr && page->isAncestorOf(box) && page->isAncestorOf(view));
        QVERIFY(page->layout()->indexOf(view) > 0);
        const QModelIndex python = view->model()->index(ProjectView::Python, 0);
        view->setExpanded(python, true);
        const QString header = view->model()->headerData(0, Qt::Horizontal).toString();
        QCOMPARE(header, QString("Content of categories"));
        const QStringList all = visibleFiles(view);
        QCOMPARE(all.size(), 11);

        box->setText("log");
        QCOMPARE(view->filterText(), QString("log"));
        QCOMPARE(visibleFiles(view), QStringList({"logo.png", "run.log", "log.txt"}));
        QVERIFY(view->isRowHidden(ProjectView::Schematics, QModelIndex()));
        for (int c : {ProjectView::Images, ProjectView::Others, ProjectView::Scratch}) {
            QVERIFY(!view->isRowHidden(c, QModelIndex()));
            QVERIFY(view->isExpanded(view->model()->index(c, 0)));
        }
        QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toString(), QString("Content of categories: 3 found"));
        QVERIFY2(view->columnWidth(0) >= view->header()->sectionSizeHint(0),
                 qPrintable(QString("%1 < %2").arg(view->columnWidth(0)).arg(view->header()->sectionSizeHint(0))));
        box->setText("  LOG ");   // case and spaces aside
        QCOMPARE(visibleFiles(view), QStringList({"logo.png", "run.log", "log.txt"}));
        box->setText("readme");
        QCOMPARE(visibleFiles(view), QStringList({"docs/README.TXT", "readme.md"}));
        box->setText("docs");   // a folder's name
        QCOMPARE(visibleFiles(view), QStringList({"docs/README.TXT"}));
        view->setTreeView(true);
        QCOMPARE(visibleFiles(view), QStringList({"README.TXT"}));
        const QModelIndex text = view->model()->index(ProjectView::Text, 0);
        QModelIndex docs;
        for (int row = 0; row < view->model()->rowCount(text); ++row)
            if (view->model()->index(row, 0, text).data().toString() == "docs") docs = view->model()->index(row, 0, text);
        QVERIFY(docs.isValid() && !view->isFile(docs));
        QVERIFY(view->isExpanded(docs));
        view->setTreeView(false);
        box->setText("Scratch");   // named within it under Scratch
        QCOMPARE(visibleFiles(view), QStringList());
        QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toString(), QString("Content of categories: 0 found"));
        for (int c = 0; c < ProjectView::CategoryCount; ++c) QVERIFY(view->isRowHidden(c, QModelIndex()));

        // What it hides is not selected.
        box->clear();
        const QModelIndex others = view->model()->index(ProjectView::Others, 0);
        const QModelIndex images = view->model()->index(ProjectView::Images, 0);
        QModelIndex runLog, logo;
        for (int row = 0; row < view->model()->rowCount(others); ++row)
            if (view->model()->index(row, 0, others).data().toString() == "run.log") runLog = view->model()->index(row, 0, others);
        logo = view->model()->index(0, 0, images);
        QCOMPARE(logo.data().toString(), QString("logo.png"));
        view->selectionModel()->select(runLog, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        view->selectionModel()->select(logo, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(view->selectedFileUrls().size(), 2);
        box->setText("logo");
        QCOMPARE(view->selectedFileUrls(), QList<QUrl>{QUrl::fromLocalFile(project + "/logo.png")});

        // Through a refresh: what came is filtered too.
        box->setText("log");
        {
            write("logbook.txt");
            const QString added = project + "/logbook.txt";
            const auto gone = qScopeGuard([&] { QFile::remove(added); });
            view->refresh();
            QCOMPARE(visibleFiles(view), QStringList({"logo.png", "logbook.txt", "run.log", "log.txt"}));
            QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toString(), QString("Content of categories: 4 found"));
        }
        view->refresh();

        // Cleared: all of them, the categories open as they were.
        box->clear();
        QCOMPARE(view->filterText(), QString());
        QCOMPARE(visibleFiles(view), all);
        QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toString(), header);
        for (int c = 0; c < ProjectView::CategoryCount; ++c)
            QCOMPARE(view->isExpanded(view->model()->index(c, 0)), c == ProjectView::Schematics || c == ProjectView::Python);
        // A project opened while it filters: cleared, it shows as a project
        // opened does - its schematics.
        box->setText("log");
        view->setProjPath(project);
        QCOMPARE(visibleFiles(view), QStringList({"logo.png", "run.log", "log.txt"}));
        box->clear();
        for (int c = 0; c < ProjectView::CategoryCount; ++c)
            QCOMPARE(view->isExpanded(view->model()->index(c, 0)), c == ProjectView::Schematics);
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestContentCategories test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_content_categories.moc"
