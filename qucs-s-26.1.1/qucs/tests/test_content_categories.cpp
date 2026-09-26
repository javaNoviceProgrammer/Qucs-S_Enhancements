/*
 * The Content panel's categories: which of the project's files each lists,
 * by patterns of their names that Application Settings > Contents sets. A
 * file is listed under the first category, from the top, whose patterns
 * match its name; Others takes by default whatever no other category took,
 * and Text (*.txt) comes right before it. Only the patterns that differ
 * from the defaults are saved.
 */
#include <QtTest>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
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
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestContentCategories test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_content_categories.moc"
