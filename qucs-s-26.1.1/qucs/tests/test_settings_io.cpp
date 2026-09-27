/*
 * File > Export Settings and File > Import Settings (settingsio.h): the
 * file (JSON, who made it, every kind of value), what stays out of it (the
 * session's state), what an import leaves as this computer has it (paths
 * not here, Claude allowed to do anything), the settings replaced and a
 * backup of them kept, and the running application brought in line - the
 * theme, the toolbars, the shortcuts, the workspace, Claude Code, what
 * waits for the next start - and the shortcuts kept, loaded at the start.
 */
#include <QtTest>
#include <QAction>
#include <QColor>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "settingsio.h"
#include "qucsshortcutmanager.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace settingsio = qucs_s::settingsio;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QJsonObject readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeJson(const QString& path, const QJsonObject& doc)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(doc).toJson());
}

void writeText(const QString& path, const QByteArray& text)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}

// A settings file holding \a settings.
QJsonObject fileOf(const QJsonObject& settings)
{
    return {{"Qucs-S settings", 1}, {"exported by", "Qucs-S 26.1.3"}, {"exported", "2026-09-27T08:50:12Z"},
            {"platform", "Elsewhere (x86_64)"}, {"settings", settings}};
}

// The store emptied, as a new user's.
void clearStore()
{
    QucsSettingsFile store;
    store.clear();
    store.sync();
}

} // namespace

class TestSettingsIO : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString path(const QString& name) const { return dir.filePath(name); }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        settingsio::setBackupDirectory(dir.filePath("backups"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void init()
    {
        clearStore();
        // As at the start of the test (loadSettings() read an empty store):
        // a simulator a QucsApp finds, not the first start (its welcome is
        // modal), a workspace of the test's - never the user's.
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QDir().mkpath(path("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(path("workspace"));
        QucsSettings.QucsWorkDir.setPath(path("workspace"));
    }

    // ---- the file ------------------------------------------------------
    void theFileIsJsonWithWhoMadeItAndEveryValue()
    {
        {
            QucsSettingsFile store;
            store.setValue("IgnoreVersion", true);
            store.setValue("maxUndo", 33);
            store.setValue("Export/Scale", 2.5);
            store.setValue("Editor", "vim");
            store.setValue("FileTypes", QStringList{"pdf/open", "txt/qucs-editor"});
            store.setValue("Blob", QByteArray("\x01\x02\xff", 3));
            store.setValue("GridColor", QColor(10, 20, 30));
            store.setValue("SomeFont", QFont("Courier", 11));
            store.setValue("SyntaxFormats/python/Keyword", "#00007f bold");
            store.setValue("ClaudeCode/model", "claude-opus-5-5");
            // The session's state: not in it.
            store.setValue("MainWindowGeometry", QByteArray("geometry"));
            store.setValue("ComponentDialog/geometry", QByteArray("geometry"));
            store.setValue("RecentDocs", "/a.sch*/b.sch");
            store.setValue("RecentProjects", "/p_prj");
            store.setValue("FileBrowser/location", "/somewhere");
            store.setValue("ClaudeCode/models", "[]");
            store.setValue("ClaudeCode/slashCommands", QStringList{"x"});
            store.setValue("firstRun", false);
        }
        const QString file = path("exported.json");
        QString error;
        QVERIFY2(settingsio::exportTo(file, &error), qPrintable(error));
        const QJsonObject doc = readJson(file);
        QCOMPARE(doc.value("Qucs-S settings").toInt(), 1);
        QCOMPARE(doc.value("exported by").toString(), QString("Qucs-S " PACKAGE_VERSION));
        QVERIFY(!doc.value("platform").toString().isEmpty());
        QVERIFY(QDateTime::fromString(doc.value("exported").toString(), Qt::ISODate).isValid());
        const QJsonObject s = doc.value("settings").toObject();
        for (const char* key : {"IgnoreVersion", "maxUndo", "Export/Scale", "Editor", "FileTypes", "Blob",
                                "GridColor", "SomeFont", "SyntaxFormats/python/Keyword", "ClaudeCode/model"})
            QVERIFY2(s.contains(key), key);
        for (const char* key : {"MainWindowGeometry", "ComponentDialog/geometry", "RecentDocs", "RecentProjects",
                                "FileBrowser/location", "ClaudeCode/models", "ClaudeCode/slashCommands",
                                "firstRun"})
            QVERIFY2(!s.contains(key), key);
        QCOMPARE(s.value("Editor").toString(), QString("vim"));
        QCOMPARE(s.value("FileTypes").toArray().size(), 2);
        QCOMPARE(s.value("Blob").toObject().value("bytes").toString(), QString("AQL/"));
        QCOMPARE(s.value("GridColor").toObject().value("color").toString(), QString("#ff0a141e"));
        QVERIFY(s.value("SomeFont").toObject().contains("variant"));

        // Taken in by a new user: every value as it was.
        clearStore();
        settingsio::Import import;
        QVERIFY2(settingsio::read(file, import, &error), qPrintable(error));
        QCOMPARE(import.count, 10);
        QVERIFY(import.source.startsWith("Qucs-S " PACKAGE_VERSION ", "));
        QVERIFY(import.kept.isEmpty());
        settingsio::apply(import);
        QucsSettingsFile store;
        QVERIFY(store.value("IgnoreVersion").toBool());
        QCOMPARE(store.value("maxUndo").toInt(), 33);
        QCOMPARE(store.value("Export/Scale").toDouble(), 2.5);
        QCOMPARE(store.value("Editor").toString(), QString("vim"));
        QCOMPARE(store.value("FileTypes").toStringList(), (QStringList{"pdf/open", "txt/qucs-editor"}));
        QCOMPARE(store.value("Blob").toByteArray(), QByteArray("\x01\x02\xff", 3));
        QCOMPARE(store.value("GridColor").value<QColor>(), QColor(10, 20, 30));
        QCOMPARE(store.value("SomeFont").value<QFont>().family(), QString("Courier"));
        QCOMPARE(store.value("SyntaxFormats/python/Keyword").toString(), QString("#00007f bold"));
        QVERIFY(!store.contains("MainWindowGeometry"));
    }

    void filesThatAreNotSettingsAreRefused()
    {
        const auto refused = [&](const QByteArray& text, const char* expected) {
            writeText(path("bad.json"), text);
            settingsio::Import import;
            QString error;
            QVERIFY(!settingsio::read(path("bad.json"), import, &error));
            QVERIFY2(error.contains(expected), qPrintable(error));
        };
        refused("not json at all", "not JSON");
        refused("{\"shortcuts\": []}", "not a Qucs-S settings file");
        refused("{\"Qucs-S settings\": 2, \"exported by\": \"Qucs-S 30.1\", \"settings\": {}}", "newer Qucs-S (Qucs-S 30.1)");
        settingsio::Import import;
        QString error;
        QVERIFY(!settingsio::read(path("none.json"), import, &error));
        QVERIFY(!error.isEmpty());
    }

    // ---- what stays as this computer has it ----------------------------
    void pathsNotOnThisComputerStayAsTheyAre()
    {
        QDir().mkpath(path("lib1"));
        QDir().mkpath(path("lib2"));
        writeText(path("ngspice"), "#!/bin/sh\n");
        {
            QucsSettingsFile store;
            store.setValue("XyceExecutable", "/mine/xyce");
            store.setValue("Paths/size", 1);
            store.setValue("Paths/1/path", "/old/lib");
        }
        writeJson(path("paths.json"), fileOf({
            {"NgspiceExecutable", path("ngspice")},               // here
            {"XyceExecutable", "/nowhere/Xyce"},                 // not here: mine stays
            {"SpiceOpusExecutable", "C:\\Program Files\\spiceopus\\spiceopus.exe"},   // a Windows one
            {"OctaveExecutable", "octave"},                       // a name, found on PATH
            {"Paths/size", 3},
            {"Paths/1/path", path("lib1")},
            {"Paths/2/path", "/nowhere/lib"},
            {"Paths/3/path", path("lib2")},
        }));
        settingsio::Import import;
        QString error;
        QVERIFY2(settingsio::read(path("paths.json"), import, &error), qPrintable(error));
        QCOMPARE(import.kept.size(), 3);
        QVERIFY(import.kept.join('\n').contains("XyceExecutable: " + QDir::toNativeSeparators("/nowhere/Xyce")));
        QVERIFY(import.kept.join('\n').contains("SpiceOpusExecutable"));
        QVERIFY(import.kept.join('\n').contains("left out"));
        QVERIFY(!import.remove.contains("XyceExecutable"));   // mine is kept, not removed
        settingsio::apply(import);
        QucsSettingsFile store;
        QCOMPARE(store.value("NgspiceExecutable").toString(), path("ngspice"));
        QCOMPARE(store.value("XyceExecutable").toString(), QString("/mine/xyce"));
        QVERIFY(!store.contains("SpiceOpusExecutable"));
        QCOMPARE(store.value("OctaveExecutable").toString(), QString("octave"));
        // The folders here, in their order.
        QCOMPARE(store.value("Paths/size").toInt(), 2);
        QCOMPARE(store.value("Paths/1/path").toString(), path("lib1"));
        QCOMPARE(store.value("Paths/2/path").toString(), path("lib2"));
        QVERIFY(!store.contains("Paths/3/path"));
    }

    void claudeIsNotAllowedEverythingByAFile()
    {
        QucsSettingsFile().setValue("ClaudeCode/permissionMode", "plan");
        writeJson(path("bypass.json"), fileOf({{"ClaudeCode/permissionMode", "bypassPermissions"},
                                                {"ClaudeCode/model", "claude-sonnet-5"}}));
        settingsio::Import import;
        QVERIFY(settingsio::read(path("bypass.json"), import));
        QCOMPARE(import.kept.size(), 1);
        QVERIFY(import.kept.first().contains("Bypass Permissions"));
        settingsio::apply(import);
        QCOMPARE(QucsSettingsFile().value("ClaudeCode/permissionMode").toString(), QString("plan"));
        QCOMPARE(QucsSettingsFile().value("ClaudeCode/model").toString(), QString("claude-sonnet-5"));
        // Any other mode is taken.
        writeJson(path("edits.json"), fileOf({{"ClaudeCode/permissionMode", "acceptEdits"}}));
        QVERIFY(settingsio::read(path("edits.json"), import));
        QVERIFY(import.kept.isEmpty());
        settingsio::apply(import);
        QCOMPARE(QucsSettingsFile().value("ClaudeCode/permissionMode").toString(), QString("acceptEdits"));
    }

    void theSettingsAreReplacedTheStateIsNot()
    {
        {
            QucsSettingsFile store;
            store.setValue("SyntaxFormats/python/Keyword", "#ff0000");   // mine, not in the file
            store.setValue("Shortcuts/File.New", "Ctrl+Alt+N");         // mine, not in the file
            store.setValue("maxUndo", 50);
            store.setValue("RecentDocs", "/mine.sch");                  // state
            store.setValue("MainWindowGeometry", QByteArray("mine"));   // state
        }
        writeJson(path("replace.json"), fileOf({{"maxUndo", 10}, {"Editor", "nano"},
                                                 {"RecentDocs", "/theirs.sch"}}));   // state in a file: not taken
        settingsio::Import import;
        QVERIFY(settingsio::read(path("replace.json"), import));
        QCOMPARE(import.count, 2);
        QVERIFY(import.remove.contains("SyntaxFormats/python/Keyword"));
        QVERIFY(import.remove.contains("Shortcuts/File.New"));
        QVERIFY(!import.remove.contains("RecentDocs"));
        QVERIFY(!import.remove.contains("MainWindowGeometry"));
        settingsio::apply(import);
        QucsSettingsFile store;
        QVERIFY(!store.contains("SyntaxFormats/python/Keyword"));
        QVERIFY(!store.contains("Shortcuts/File.New"));
        QCOMPARE(store.value("maxUndo").toInt(), 10);
        QCOMPARE(store.value("Editor").toString(), QString("nano"));
        QCOMPARE(store.value("RecentDocs").toString(), QString("/mine.sch"));
        QCOMPARE(store.value("MainWindowGeometry").toByteArray(), QByteArray("mine"));
    }

    void theProgramsItChangesAreTold()
    {
        writeText(path("my-ngspice"), "#!/bin/sh\n");
        {
            QucsSettingsFile store;
            store.setValue("Editor", "vim");
            store.setValue("OctaveExecutable", "octave");
        }
        writeJson(path("programs.json"), fileOf({{"NgspiceExecutable", path("my-ngspice")},
                                                  {"Editor", "emacs"},
                                                  {"OctaveExecutable", "octave"},   // the same
                                                  {"maxUndo", 5}}));
        settingsio::Import import;
        QVERIFY(settingsio::read(path("programs.json"), import));
        QCOMPARE(import.programs.size(), 2);
        QVERIFY(import.programs.contains("ngspice: " + path("my-ngspice")));
        QVERIFY(import.programs.contains("Text editor: emacs"));
    }

    void tenBackupsAreKept()
    {
        QDir(settingsio::backupDirectory()).removeRecursively();
        QucsSettingsFile().setValue("maxUndo", 1);
        QString first, last;
        for (int i = 0; i < 12; ++i) {
            QucsSettingsFile().setValue("maxUndo", i + 1);
            QString error;
            last = settingsio::backup(&error);
            QVERIFY2(!last.isEmpty(), qPrintable(error));
            if (i == 0) first = last;
        }
        const QStringList all = QDir(settingsio::backupDirectory()).entryList({"settings-*.json"}, QDir::Files, QDir::Name);
        QCOMPARE(all.size(), 10);
        QVERIFY(!QFileInfo::exists(first));
        QVERIFY(QFileInfo::exists(last));
        QCOMPARE(QFileInfo(last).fileName(), all.last());
        // The newest holds the settings as they were last.
        QCOMPARE(readJson(last).value("settings").toObject().value("maxUndo").toVariant().toInt(), 12);
        // Each a settings file.
        settingsio::Import import;
        QVERIFY(settingsio::read(last, import));
    }

    void readingTheSettingsAgainAddsNothing()
    {
        {
            QucsSettingsFile store;
            store.setValue("Paths/size", 2);
            store.setValue("Paths/1/path", "/a");
            store.setValue("Paths/2/path", "/b");
        }
        QVERIFY(loadSettings());
        QVERIFY(loadSettings());
        QCOMPARE(qucsPathList, (QStringList{"/a", "/b"}));
        QCOMPARE(QucsSettings.spiceExtensions.size(), 4);
        qucsPathList.clear();
    }

    // ---- the application -------------------------------------------------
    void theFileMenuHasBoth()
    {
        QucsApp app(false);
        MainGuard main(&app);
        QList<QAction*> actions;
        for (QAction* top : app.menuBar()->actions())
            if (top->menu() != nullptr && top->menu()->actions().contains(app.applSettings))
                actions = top->menu()->actions();
        QVERIFY(!actions.isEmpty());
        const int settings = int(actions.indexOf(app.applSettings));
        QVERIFY(settings >= 0);
        QCOMPARE(actions.value(settings + 1), app.exportSettings);
        QCOMPARE(actions.value(settings + 2), app.importSettings);
        QCOMPARE(app.exportSettings->text(), QString("Export Settings..."));
        QCOMPARE(app.importSettings->text(), QString("Import Settings..."));
        // Left in the File menu on macOS.
        QCOMPARE(app.exportSettings->menuRole(), QAction::NoRole);
        QCOMPARE(app.importSettings->menuRole(), QAction::NoRole);
        QVERIFY(QucsShortcutManager::instance().command("File.ExportSettings") != nullptr);
        QVERIFY(QucsShortcutManager::instance().command("File.ImportSettings") != nullptr);
    }

    void theApplicationExportsWhatItHolds()
    {
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettings.maxUndo = 77;   // changed, not saved yet
        QString error;
        QVERIFY2(app.exportSettingsTo(path("app.json"), &error), qPrintable(error));
        const QJsonObject s = readJson(path("app.json")).value("settings").toObject();
        QCOMPARE(s.value("maxUndo").toVariant().toInt(), 77);
        QVERIFY(s.contains("Theme"));
        QVERIFY(s.contains("NgspiceExecutable"));
        QVERIFY(!s.contains("MainWindowGeometry"));
        QucsSettings.maxUndo = 20;
    }

    void anImportIsAppliedAndTheBackupTakesItBack()
    {
        QDir().mkpath(path("their-workspace"));
        QucsApp app(false);
        MainGuard main(&app);
        const QString workspaceBefore = QucsSettings.qucsWorkspaceDir.absolutePath();
        const int themeBefore = QucsSettings.Theme;
        const int theirTheme = themeBefore == 2 ? 3 : 2;
        const QKeySequence newBefore = app.fileNew->shortcut();
        QVERIFY(!app.toolbarsLocked());
        QVERIFY(QucsSettings.WriteTextDocSettings);
        app.exportSettingsTo(path("mine.json"));

        writeJson(path("theirs.json"), fileOf({
            {"Theme", theirTheme},
            {"LockToolbars", true},
            {"WriteTextDocSettings", false},
            {"ContentRefreshSeconds", 7},
            {"GridMode", 1},
            {"Shortcuts/File.New", "Ctrl+Alt+Shift+N"},
            {"ClaudeCode/model", "claude-haiku-4-5-20251001"},
            {"Language", "de"},
            {"QucsHomeDir", path("their-workspace")},
            {"NgspiceExecutable", QucsSettings.NgspiceExecutable},
            {"XyceExecutable", "/nowhere/Xyce"},
        }));
        QString report;
        QVERIFY2(app.importSettingsFrom(path("theirs.json"), false, &report), qPrintable(report));
        QCOMPARE(QucsSettings.Theme, theirTheme);
        QVERIFY(app.toolbarsLocked());
        QVERIFY(!QucsSettings.WriteTextDocSettings);
        QCOMPARE(QucsSettings.ContentRefreshSeconds, 7);
        QCOMPARE(QucsSettings.GridMode, 1);
        QCOMPARE(app.fileNew->shortcut(), QKeySequence("Ctrl+Alt+Shift+N"));
        QCOMPARE(QDir(QucsSettings.qucsWorkspaceDir).canonicalPath(), QDir(path("their-workspace")).canonicalPath());
        QCOMPARE(QucsSettingsFile().value("ClaudeCode/model").toString(), QString("claude-haiku-4-5-20251001"));
        QVERIFY2(report.contains("At the next start: the language."), qPrintable(report));
        QVERIFY2(report.contains("XyceExecutable"), qPrintable(report));
        // The backup: the settings as they were.
        const QRegularExpression backupLine("Your previous settings: (.*)$");
        const QString backup = QDir::fromNativeSeparators(backupLine.match(report).captured(1));
        QVERIFY2(QFileInfo::exists(backup), qPrintable(report));
        QCOMPARE(readJson(backup).value("settings").toObject().value("Theme").toVariant().toInt(), themeBefore);

        // Taken back.
        QVERIFY2(app.importSettingsFrom(backup, false, &report), qPrintable(report));
        QCOMPARE(QucsSettings.Theme, themeBefore);
        QVERIFY(!app.toolbarsLocked());
        QVERIFY(QucsSettings.WriteTextDocSettings);
        QCOMPARE(QucsSettings.GridMode, 0);
        QCOMPARE(app.fileNew->shortcut(), newBefore);
        QCOMPARE(QDir(QucsSettings.qucsWorkspaceDir).canonicalPath(), QDir(workspaceBefore).canonicalPath());
        QVERIFY(!QucsSettingsFile().contains("Shortcuts/File.New"));
        app.closeAllFiles();
    }

    void aBadFileChangesNothing()
    {
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettingsFile().setValue("maxUndo", 42);
        const int backups = int(QDir(settingsio::backupDirectory()).entryList({"settings-*.json"}).size());
        writeText(path("broken.json"), "{\"Qucs-S settings\": 1, \"settings\": ");
        QString report;
        QVERIFY(!app.importSettingsFrom(path("broken.json"), false, &report));
        QVERIFY2(report.contains("not JSON"), qPrintable(report));
        QCOMPARE(QucsSettingsFile().value("maxUndo").toInt(), 42);
        QCOMPARE(int(QDir(settingsio::backupDirectory()).entryList({"settings-*.json"}).size()), backups);
    }

    void theShortcutsKeptAreLoadedAtTheStart()
    {
        QucsSettingsFile().setValue("Shortcuts/File.New", "Ctrl+Alt+Shift+M");
        {
            QucsApp app(false);
            MainGuard main(&app);
            QCOMPARE(app.fileNew->shortcut(), QKeySequence("Ctrl+Alt+Shift+M"));
            // Set back to its default: no longer kept.
            QucsShortcutManager::instance().resetToDefaults();
            QucsShortcutManager::instance().saveToSettings();
            QVERIFY(!QucsSettingsFile().contains("Shortcuts/File.New"));
        }
        QucsApp app(false);
        MainGuard main(&app);
        QCOMPARE(app.fileNew->shortcut(), QKeySequence(QKeySequence::New));
    }
};

QTEST_MAIN(TestSettingsIO)
#include "test_settings_io.moc"
