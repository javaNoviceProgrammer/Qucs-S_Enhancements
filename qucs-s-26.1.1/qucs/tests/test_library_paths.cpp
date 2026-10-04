/*
 * test_library_paths.cpp - the library search paths (Settings > Locations):
 * kept in the settings and set in their dialog; a section of the Libraries
 * panel for each folder, its libraries read when opened or searched, one
 * that cannot be read marked; a placed part's library found there when it
 * is not where it was; Create Library saving into one; the schematic
 * check's word for them.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "schematic.h"
#include "settings.h"
#include "components/component.h"
#include "dialogs/librarydialog.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QString write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(bytes);
    return path;
}

// A Qucs library of one three-pin part, \a part, under the title \a title.
QByteArray library(const QByteArray& title, const QByteArray& part)
{
    return "<Qucs Library 2.1.0 \"" + title + "\">\n\n<Component " + part + ">\n  <Description>\nAn adder\n  </Description>\n"
           "  <Model>\n.Def:Lib_" + part + " _net0 _net3 _net1\nVCVS:SRC1 _net0 _net1 _net2 gnd G=\"1\" T=\"0\"\n"
           "VCVS:SRC2 _net3 _net2 gnd gnd G=\"1\" T=\"0\"\n.Def:End\n  </Model>\n"
           "  <Spice>\n.SUBCKT Lib_" + part + " 0 _net0 _net3 _net1\nESRC1 _net1 _net2 _net0 0 1\nESRC2 _net2 0 _net3 0 1\n.ENDS\n"
           "  </Spice>\n  <Symbol>\n    <.ID 10 14 ADD>\n    <.PortSym 0 -30 1 0>\n    <.PortSym 0 30 2 0>\n"
           "    <.PortSym 30 0 3 180>\n    <Ellipse -20 -20 40 40 #000080 2 1 #c0c0c0 1 0>\n  </Symbol>\n</Component>\n";
}

// Every message box shown while \a during runs closed, and what it said.
QStringList boxesDuring(const std::function<void()>& during)
{
    QStringList said;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&said] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            said << box->text();
            box->reject();
        }
    });
    timer.start(50);
    during();
    return said;
}

QStringList topTexts(QTreeWidget* tree)
{
    QStringList texts;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) texts << tree->topLevelItem(i)->text(0);
    return texts;
}

QTreeWidgetItem* topItem(QTreeWidget* tree, const QString& text)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->text(0) == text) return tree->topLevelItem(i);
    return nullptr;
}

QString schematicWith(const QString& lines)
{
    return QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n") + lines
           + QStringLiteral("</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
}

Component* find(Schematic* doc, const QString& name)
{
    for (Component* c : doc->a_DocComps)
        if (c->Name == name) return c;
    return nullptr;
}

} // namespace

class TestLibraryPaths : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace, project, userLib, team, otherTeam, missing;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        // (Fonts with a family: the settings saved here are read back.)
        QucsSettings.font = QucsSettings.appFont = QucsSettings.textFont = QApplication::font();
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QDir().mkpath(dir.filePath("kernel"));
        QucsSettings.S4Qworkdir = dir.filePath("kernel");

        // An installed library of one part; a workspace whose user_lib has
        // a library that cannot be read before one that can.
        QucsSettings.LibDir = write(dir.filePath("system/Basic.lib"), library("Basic", "Add")).section('/', 0, -2) + "/";
        workspace = dir.filePath("workspace");
        project = workspace + "/proj_prj";
        userLib = workspace + "/user_lib";
        QVERIFY(QDir().mkpath(project));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.QucsWorkDir.setPath(project);
        write(userLib + "/Mine.lib", library("Mine", "Sum"));
        write(userLib + "/Bad.lib", library("Bad", "Nope"));
        QFile::setPermissions(userLib + "/Bad.lib", QFileDevice::Permissions());

        // The folders of the search paths: a team's, with a Qucs library, a
        // SPICE library and one that cannot be read; another of the same
        // name; one that is not there.
        team = dir.filePath("shared/team");
        write(team + "/TeamLib.lib", library("Team Library", "Amp"));
        write(team + "/Models.lib", "* models\n.subckt opamp inp inn out\nR1 inp out 1k\n.ends\n");
        write(team + "/Broken.lib", library("Broken", "Gone"));
        QFile::setPermissions(team + "/Broken.lib", QFileDevice::Permissions());
        write(team + "/sub.sch", schematicWith(QString()).toUtf8());
        otherTeam = dir.filePath("other/team");
        write(otherTeam + "/Other.lib", library("Other", "Mul"));
        missing = dir.filePath("gone/libs");
    }

    void cleanupTestCase()
    {
        // (Readable again, for the temporary folder to go.)
        QFile::setPermissions(userLib + "/Bad.lib", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile::setPermissions(team + "/Broken.lib", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    // Saved with the other settings, and read back.
    void theSettingIsKept()
    {
        QucsSettings.LibraryPaths = {team, missing};
        QVERIFY(saveApplSettings());
        QucsSettings.LibraryPaths.clear();
        QVERIFY(loadSettings());
        QCOMPARE(QucsSettings.LibraryPaths, QStringList({team, missing}));
        QucsSettings.LibraryPaths.clear();
        QVERIFY(saveApplSettings());
    }

    // A section of the Libraries panel for each folder, after the user
    // libraries: its libraries named, their parts read when one is opened
    // or searched; one that cannot be read marked, here and in user_lib,
    // and no box (a box stopped the listing at it).
    void thePanelHasASectionForEachFolder()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettings.LibraryPaths = {team, team + "/", userLib, otherTeam, missing};
        const QStringList said = boxesDuring([&app] { app.fillLibrariesTreeView(); });
        QVERIFY2(said.isEmpty(), qPrintable(said.join(" | ")));
        QTreeWidget* tree = app.librariesTree();
        const QStringList texts = topTexts(tree);
        // (A folder listed twice, or user_lib, has no section of its own.)
        QCOMPARE(texts, QStringList({"System Libraries", "Basic", "User Libraries", "Bad", "Mine", "team", "Broken", "Models",
                                     "Team Library", "team (other)", "Other", "libs (not found)", "Project Libraries"}));
        QCOMPARE(topItem(tree, "team")->toolTip(0), QDir::toNativeSeparators(team));
        QCOMPARE(topItem(tree, "libs (not found)")->toolTip(0), QDir::toNativeSeparators(missing));

        // user_lib's: read at once; the one that cannot be opened greyed,
        // and the one after it listed with its part.
        QTreeWidgetItem* bad = topItem(tree, "Bad");
        QCOMPARE(bad->childCount(), 0);
        QVERIFY2(bad->toolTip(0).contains("It cannot be opened."), qPrintable(bad->toolTip(0)));
        QCOMPARE(topItem(tree, "Mine")->childCount(), 1);

        // The team's: not read until opened.
        QTreeWidgetItem* teamLib = topItem(tree, "Team Library");
        QCOMPARE(teamLib->childCount(), 0);
        QCOMPARE(teamLib->childIndicatorPolicy(), QTreeWidgetItem::ShowIndicator);
        teamLib->setExpanded(true);
        QCOMPARE(teamLib->childCount(), 1);
        QCOMPARE(teamLib->child(0)->text(0), QStringLiteral("Amp"));
        QCOMPARE(teamLib->child(0)->text(3), team + "/TeamLib");   // placed with its path
        QVERIFY(teamLib->child(0)->text(1).contains("\"" + team + "/TeamLib\""));
        QTreeWidgetItem* broken = topItem(tree, "Broken");
        QVERIFY(!app.readLibraryParts(broken));
        QVERIFY2(broken->toolTip(0).contains("It cannot be opened."), qPrintable(broken->toolTip(0)));
        QCOMPARE(broken->childIndicatorPolicy(), QTreeWidgetItem::DontShowIndicator);

        // A search reads those not read yet: the SPICE library's subcircuit.
        QTreeWidgetItem* models = topItem(tree, "Models");
        QCOMPARE(models->childCount(), 0);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSearchLibComponent", Q_ARG(QString, "opamp")));
        QCOMPARE(models->childCount(), 1);
        QVERIFY(!models->isHidden() && models->isExpanded());
        QVERIFY(topItem(tree, "Other")->isHidden());
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSearchLibComponent", Q_ARG(QString, "Mul")));
        QVERIFY(!topItem(tree, "Other")->isHidden());
        QCOMPARE(topItem(tree, "Other")->childCount(), 1);

        // None: no section.
        QucsSettings.LibraryPaths.clear();
        app.fillLibrariesTreeView();
        QCOMPARE(topTexts(app.librariesTree()),
                 QStringList({"System Libraries", "Basic", "User Libraries", "Bad", "Mine", "Project Libraries"}));
    }

    // A library copied in by anyone (the Finder, Claude Code's own tools)
    // shows without a restart, and one taken away goes; the libraries open
    // stay open and a search stays searched. A file that is no library
    // changes nothing.
    void thePanelFollowsTheFolders()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettings.LibraryPaths = {otherTeam};
        app.fillLibrariesTreeView();
        QTreeWidget* tree = app.librariesTree();
        topItem(tree, "Other")->setExpanded(true);
        QCOMPARE(topItem(tree, "Other")->childCount(), 1);
        write(otherTeam + "/Fresh.lib", library("Fresh", "New"));
        QTRY_VERIFY_WITH_TIMEOUT(topItem(app.librariesTree(), "Fresh") != nullptr, 5000);
        QVERIFY(topItem(app.librariesTree(), "Other")->isExpanded());   // (as it was)
        // A search typed stays applied.
        auto* search = app.findChild<QLineEdit*>();
        for (QLineEdit* e : app.findChildren<QLineEdit*>())
            if (e->placeholderText() == "Search Lib Components") search = e;
        QVERIFY(search != nullptr && search->placeholderText() == "Search Lib Components");
        search->setText("New");
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSearchLibComponent", Q_ARG(QString, "New")));
        QVERIFY(topItem(app.librariesTree(), "Other")->isHidden());
        QVERIFY(QFile::remove(otherTeam + "/Fresh.lib"));
        write(otherTeam + "/Later.lib", library("Later", "New2"));
        QTRY_VERIFY_WITH_TIMEOUT(topItem(app.librariesTree(), "Later") != nullptr, 5000);
        QVERIFY(topItem(app.librariesTree(), "Fresh") == nullptr);
        QVERIFY(topItem(app.librariesTree(), "Other")->isHidden() && !topItem(app.librariesTree(), "Later")->isHidden());
        search->clear();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSearchLibComponent", Q_ARG(QString, QString())));
        QFile::remove(otherTeam + "/Later.lib");
        QTRY_VERIFY_WITH_TIMEOUT(topItem(app.librariesTree(), "Later") == nullptr, 5000);
        // Not a library: the panel is not filled anew (an item kept is the
        // same item).
        QTreeWidgetItem* before = topItem(app.librariesTree(), "Other");
        write(otherTeam + "/notes.txt", "not a library\n");
        QTest::qWait(1000);
        QCOMPARE(topItem(app.librariesTree(), "Other"), before);
        QFile::remove(otherTeam + "/notes.txt");
        QucsSettings.LibraryPaths.clear();

        // A workspace with no user_lib yet: the first library made in one
        // shows too.
        const QString fresh = dir.filePath("fresh-workspace");
        QVERIFY(QDir().mkpath(fresh));
        QucsSettings.qucsWorkspaceDir.setPath(fresh);
        app.fillLibrariesTreeView();
        write(fresh + "/user_lib/First.lib", library("First", "One"));
        QTRY_VERIFY_WITH_TIMEOUT(topItem(app.librariesTree(), "First") != nullptr, 5000);
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
    }

    // Set in Settings > Locations: kept at once (the subcircuit paths
    // too, which were saved only when Qucs-S closed), and the panel shows
    // them.
    void theDialogSetsThem()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettings.LibraryPaths = {missing};
        const QStringList subcircuitsBefore = qucsPathList;
        QucsSettingsDialog dialog(&app);
        auto* table = dialog.findChild<QTableWidget*>("libraryPaths");
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), missing);
        QCOMPARE(table->accessibleName(), QStringLiteral("Library search paths"));
        QVERIFY(dialog.findChild<QTableWidget*>("subcircuitPaths") != nullptr);
        dialog.setPathList("libraryPaths", {team, team, otherTeam});
        QCOMPARE(table->rowCount(), 2);   // (once each)
        dialog.setPathList("subcircuitPaths", {otherTeam});
        QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        QCOMPARE(QucsSettings.LibraryPaths, QStringList({team, otherTeam}));
        QVERIFY(topItem(app.librariesTree(), "team") != nullptr);
        QVERIFY(topItem(app.librariesTree(), "team (other)") != nullptr);
        // Saved: read back from the store.
        QucsSettings.LibraryPaths.clear();
        qucsPathList.clear();
        QVERIFY(loadSettings());
        QCOMPARE(QucsSettings.LibraryPaths, QStringList({team, otherTeam}));
        QCOMPARE(qucsPathList, QStringList({otherTeam}));
        // Removed by its row's button.
        QucsSettingsDialog again(&app);
        auto* rows = again.findChild<QTableWidget*>("libraryPaths");
        QCOMPARE(rows->rowCount(), 2);
        qobject_cast<QPushButton*>(rows->cellWidget(0, 1))->click();
        QCOMPARE(rows->rowCount(), 1);
        QVERIFY(QMetaObject::invokeMethod(&again, "slotApply"));
        QCOMPARE(QucsSettings.LibraryPaths, QStringList({otherTeam}));
        qucsPathList = subcircuitsBefore;
        QucsSettings.LibraryPaths.clear();
        saveApplSettings();
    }

    // A placed part whose library is not where it was (another computer,
    // a folder moved) - or named by its name alone, as Claude places it -
    // is found in a folder of the search paths; without them, it is not,
    // and the schematic check says where it was looked for.
    void aPlacedPartFindsItsLibrary()
    {
        const QString file = write(dir.filePath("placed.sch"),
                                   schematicWith("  <Lib X1 1 100 100 13 10 0 0 \"/elsewhere/team/TeamLib\" 0 \"Amp\" 0>\n"
                                                 "  <Lib X2 1 300 100 13 10 0 0 \"TeamLib\" 0 \"Amp\" 0>\n")
                                       .toUtf8());
        // (ngspice's parts, as a settings read before may have chosen another.)
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        Module::unregisterModules();
        Module::registerModules();
        QucsSettings.LibraryPaths = {missing, team};
        {
            Schematic doc(nullptr, file);
            QVERIFY(doc.loadDocument());
            QCOMPARE(find(&doc, "X1")->Ports.size(), 3);
            QCOMPARE(find(&doc, "X2")->Ports.size(), 3);
        }
        QCOMPARE(misc::properAbsFileName("/elsewhere/team/TeamLib.lib"), QFileInfo(team + "/TeamLib.lib").canonicalFilePath());
        // (Only a library: a subcircuit is looked for in the subcircuit paths.)
        QCOMPARE(misc::properAbsFileName("/elsewhere/team/sub.sch"), QStringLiteral("/elsewhere/team/sub.sch"));

        QucsSettings.LibraryPaths.clear();
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        QCOMPARE(find(&doc, "X1")->Ports.size(), 0);
        QStringList messages;
        for (const auto& issue : qucs_s::erc::check(&doc))
            if (issue.component == "X1") messages << issue.message;
        QVERIFY2(messages.join('\n').contains("nor a folder of the library search paths"), qPrintable(messages.join('\n')));

        // A SPICE library part's library, the same: found there, and when
        // it is nowhere, the check says the search paths were looked in.
        const QString spice = write(dir.filePath("spice.sch"),
                                    schematicWith("  <SpLib X3 1 100 100 -29 -164 0 0 \"/elsewhere/team/Models.lib\" 0 \"opamp\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n"
                                                  "  <SpLib X4 1 300 100 -29 -164 0 0 \"/elsewhere/team/NoSuch.lib\" 0 \"opamp\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n")
                                        .toUtf8());
        QucsSettings.LibraryPaths = {team};
        {
            Schematic spiceDoc(nullptr, spice);
            misc::ErrorCapture errors;
            QVERIFY2(spiceDoc.load(), qPrintable(errors.errors().join('\n')));
            QCOMPARE(find(&spiceDoc, "X3")->Ports.size(), 3);
            QStringList said;
            for (const auto& issue : qucs_s::erc::check(&spiceDoc)) said << issue.component + ": " + issue.message;
            QVERIFY2(!said.join('\n').contains("X3: X3: its SPICE library"), qPrintable(said.join('\n')));
            QVERIFY2(said.join('\n').contains("X4: X4: its SPICE library /elsewhere/team/NoSuch.lib is not found (beside the schematic, in "
                                              "the project or its user_lib, in a folder of the library search paths, nor in the library "
                                              "of Qucs-S)"),
                     qPrintable(said.join('\n')));
        }

        // Claude's list of library folders: in the order a name is found.
        QucsSettings.LibraryPaths = {team, missing};
        QCOMPARE(misc::libraryFolders(false),
                 QStringList({QFileInfo(QucsSettings.LibDir).canonicalFilePath(), QFileInfo(userLib).canonicalFilePath(),
                              QFileInfo(team).canonicalFilePath()}));
        QucsSettings.LibraryPaths.clear();
    }

    // Create Library: into user_lib as before, or into the project or a
    // folder of the search paths, as chosen.
    void createLibrarySavesWhereChosen()
    {
        QucsSettings.QucsWorkDir.setPath(project);   // (a settings read before moved it)
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "proj";
        QucsSettings.LibraryPaths = {missing, team};
        write(project + "/amp.sch",
              schematicWith("  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                            "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n"
                            "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
                  .toUtf8());
        const auto create = [&app](const QString& name, int destination) {
            LibraryDialog dialog(&app);
            dialog.fillSchematicList({"amp.sch"});
            auto* combo = dialog.findChild<QComboBox*>("destination");
            if (combo == nullptr) return QStringLiteral("no destination");
            QStringList items;
            for (int i = 0; i < combo->count(); ++i) items << combo->itemText(i);
            combo->setCurrentIndex(destination);
            dialog.findChild<QLineEdit*>()->setText(name);
            for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
                if (box->text() == "Add subcircuit description") box->setChecked(false);
            QMetaObject::invokeMethod(&dialog, "slotCreateNext");
            return items.join(" | ");
        };
        // (The folder not there is not offered.)
        QCOMPARE(create("Shared", 2), QStringLiteral("User libraries (user_lib) | The project proj | ")
                                          + QDir::toNativeSeparators(team));
        QVERIFY(QFileInfo::exists(team + "/Shared.lib"));
        QVERIFY(QFile(team + "/Shared.lib").open(QIODevice::ReadOnly));
        create("Personal", 0);
        QVERIFY(QFileInfo::exists(userLib + "/Personal.lib"));
        create("Local", 1);
        QVERIFY(QFileInfo::exists(project + "/Local.lib"));
        QFile::remove(team + "/Shared.lib");
        QucsSettings.LibraryPaths.clear();
        app.ProjName.clear();
    }
};

QTEST_MAIN(TestLibraryPaths)
#include "test_library_paths.moc"
