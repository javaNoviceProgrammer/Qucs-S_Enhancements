/*
 * test_long_file_names.cpp - a long file name, cut as Application Settings
 *                            > Appearance says (50 characters unless set,
 *                            "…" for the rest, its extension whole), in a
 *                            document's tab and in the Claude Code panel;
 *                            the whole path in their tooltips; what reads
 *                            a tab's title reading the file's name instead
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include "config.h"
#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "mouseactions.h"
#include "projectView.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "settings.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

bool copy(const QString& from, const QString& to)
{
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    return QFile::copy(from, to);
}

const QString kLong = QStringLiteral("a_very_long_schematic_name_that_goes_on_and_on_past_any_tabs");   // 60 characters

} // namespace

class TestLongFileNames : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    // The tab of the document of \a path, and its pane.
    static int tabOf(QucsApp& app, const QString& path, QTabWidget** pane)
    {
        for (QucsDoc* doc : app.allDocuments())
            if (QFileInfo(doc->getDocName()).absoluteFilePath() == QFileInfo(path).absoluteFilePath()) {
                QWidget* w = QucsApp::documentWidget(doc);
                *pane = app.paneOf(w);
                return *pane != nullptr ? (*pane)->indexOf(w) : -1;
            }
        return -1;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        // (A simulator there is: the window asks for none at its start.)
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QCOMPARE(kLong.size(), 60);
    }

    // The name cut: after the cap's characters, "…", the extension whole.
    void theNameIsCutKeepingItsExtension()
    {
        const QChar dots(0x2026);
        QCOMPARE(QucsSettings.FileNameCap, 50);   // the default
        QCOMPARE(_settings::Get().itemDefault<int>("FileNameCap"), 50);
        QCOMPARE(misc::shownFileName("/x/" + kLong + ".sch", 50), kLong.left(50) + dots + ".sch");
        QCOMPARE(misc::shownFileName(kLong + ".sch"), kLong.left(50) + dots + ".sch");
        QCOMPARE(misc::shownFileName("/x/amp.sch", 50), QStringLiteral("amp.sch"));
        // Not longer than the cap: whole (the extension does not count).
        QCOMPARE(misc::shownFileName(kLong.left(50) + ".sch", 50), kLong.left(50) + ".sch");
        QCOMPARE(misc::shownFileName(kLong.left(51) + ".sch", 50), kLong.left(50) + dots + ".sch");
        // A long extension, whole; the extension is from the last dot.
        QCOMPARE(misc::shownFileName("abcdefghij.dat.ngspice", 4), QStringLiteral("abcd") + dots + ".ngspice");
        // No extension: a name with no dot, one that starts with it, one that ends with it.
        QCOMPARE(misc::shownFileName("abcdefghij", 4), QStringLiteral("abcd") + dots);
        QCOMPARE(misc::shownFileName(".abcdefghij", 4), QStringLiteral(".abc") + dots);
        QCOMPARE(misc::shownFileName("abcdefghij.", 4), QStringLiteral("abcd") + dots);
        // 0: never cut.
        QCOMPARE(misc::shownFileName("/x/" + kLong + ".sch", 0), kLong + ".sch");
        // Characters as read: an accented letter of two code points, an emoji
        // of two halves, one each - and never cut in two.
        const QString accented = QStringLiteral("éééé");   // éééé, decomposed
        QCOMPARE(misc::shownFileName(accented + ".sch", 2), QStringLiteral("éé") + dots + ".sch");
        const QString faces = QStringLiteral("\U0001F600\U0001F600\U0001F600");
        QCOMPARE(misc::shownFileName(faces + ".sch", 2), QStringLiteral("\U0001F600\U0001F600") + dots + ".sch");
        QCOMPARE(misc::shownFileName(faces + ".sch", 3), faces + ".sch");
        // Cut twice: as cut once.
        const QString once = misc::shownFileName(kLong + ".sch", 50);
        QCOMPARE(misc::shownFileName(once, 50), once);
    }

    // A document's tab: the name cut, the whole path its tooltip - after
    // opening, Save As, a move, a move to another pane; again when the
    // setting changes, through the settings dialog's OK; whole at 0.
    void theTabShowsTheNameCut()
    {
        const QString file = dir.filePath("work/" + kLong + ".sch");
        QVERIFY(copy(QStringLiteral(QUCS_EXAMPLES_DIR "/external_interface/probe_and_subcircuit/example_sub_subcircuit.sch"), file));
        QucsApp app(false);
        MainGuard main(&app);
        QVERIFY(app.gotoPage(file, false, false));
        QTabWidget* pane = nullptr;
        int i = tabOf(app, file, &pane);
        QVERIFY(i >= 0);
        const QString cut = kLong.left(50) + QChar(0x2026) + ".sch";
        QCOMPARE(pane->tabText(i), cut);
        QCOMPARE(pane->tabToolTip(i), QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()));
        // Saved under another long name: the new one cut.
        QucsDoc* doc = app.getDoc();
        const QString other = dir.filePath("work/" + kLong + "_and_then_some_more.sch");
        doc->setDocChanged(true);
        QVERIFY(app.saveFile(doc));   // (its own name first: Save as follows)
        app.documentsMoved({file}, {other});
        i = tabOf(app, other, &pane);
        QCOMPARE(pane->tabText(i), cut);   // (the first 50 the same)
        QCOMPARE(pane->tabToolTip(i), QDir::toNativeSeparators(QFileInfo(other).absoluteFilePath()));
        // Moved to another pane: still so.
        QTabWidget* before = pane;
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSplitPaneRight"));
        QVERIFY(app.panes().size() > 1);
        ContextMenuTabWidget* target = nullptr;
        for (ContextMenuTabWidget* p : app.panes())
            if (p != before) target = p;
        QVERIFY(target != nullptr);
        app.moveDocument(QucsApp::documentWidget(doc), target);
        i = tabOf(app, other, &pane);
        QCOMPARE(pane, static_cast<QTabWidget*>(target));   // (the pane it left, empty, closed)
        QCOMPARE(pane->tabText(i), cut);
        QCOMPARE(pane->tabToolTip(i), QDir::toNativeSeparators(QFileInfo(other).absoluteFilePath()));
        // The setting changed in Application Settings > Appearance, OK: at
        // once; "Never": whole.
        for (const int cap : {12, 0, 50}) {
            QucsSettingsDialog dialog(&app);
            auto* spin = dialog.findChild<QSpinBox*>("fileNameCapSpin");
            QVERIFY(spin != nullptr);
            QCOMPARE(spin->value(), QucsSettings.FileNameCap);
            spin->setValue(cap);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotOK"));
            QCOMPARE(QucsSettings.FileNameCap, cap);
            QCOMPARE(QucsSettingsFile().value("FileNameCap").toInt(), cap);
            i = tabOf(app, other, &pane);
            QCOMPARE(pane->tabText(i), cap == 0 ? QFileInfo(other).fileName() : misc::shownFileName(other, cap));
        }
        // Imported (File > Import Settings): at once too.
        {
            const QString json = dir.filePath("cap.json");
            QFile f(json);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(QJsonObject{{"Qucs-S settings", 1}, {"exported by", "Qucs-S 26.1.5"},
                                              {"settings", QJsonObject{{"FileNameCap", 9}}}}).toJson());
            f.close();
            QString report;
            QVERIFY2(app.importSettingsFrom(json, false, &report), qPrintable(report));
            QCOMPARE(QucsSettings.FileNameCap, 9);
            // (What the file has not goes back to its default - ngspice's
            // path too: a machine without ngspice, CI's, said so in a box
            // over the import, and at each window's start after it. The
            // report says it now; the path back for the tests after.)
            if (!QFileInfo::exists(QStandardPaths::findExecutable("ngspice")))
                QVERIFY2(report.contains("No simulator was found"), qPrintable(report));
            QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
            _settings::Get().setItem<QString>("NgspiceExecutable", QucsSettings.NgspiceExecutable);
            i = tabOf(app, other, &pane);
            QVERIFY(i >= 0);
            QCOMPARE(pane->tabText(i), misc::shownFileName(other, 9));
            QucsSettings.FileNameCap = 50;
            app.titleFileNames();
        }
        // "Set to default": 50 again.
        {
            QucsSettingsDialog dialog(&app);
            auto* spin = dialog.findChild<QSpinBox*>("fileNameCapSpin");
            spin->setValue(7);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotDefaultValues"));
            QCOMPARE(spin->value(), 50);
            QCOMPARE(spin->specialValueText(), QStringLiteral("Never"));
            QCOMPARE(spin->minimum(), 0);
        }
        // An & of a name is the name's: written && on the tab.
        const QString ampersand = dir.filePath("work/R&D_" + kLong + ".sch");
        QVERIFY(copy(file, ampersand));
        QVERIFY(app.gotoPage(ampersand, false, false));
        i = tabOf(app, ampersand, &pane);
        QCOMPARE(pane->tabText(i), QStringLiteral("R&&D_") + kLong.left(46) + QChar(0x2026) + ".sch");
        for (QucsDoc* d : app.allDocuments()) d->setDocChanged(false);
    }

    // The Claude Code panel: the schematic pinned, and the one in front
    // its chip names, cut; the whole path in their tooltips.
    void thePanelShowsTheNameCut()
    {
        const QString file = dir.filePath("panel/" + kLong + ".sch");
        QVERIFY(copy(QStringLiteral(QUCS_EXAMPLES_DIR "/external_interface/probe_and_subcircuit/example_sub_subcircuit.sch"), file));
        QucsApp app(false);
        MainGuard main(&app);
        QVERIFY(app.gotoPage(file, false, false));
        ClaudeCodePanel* panel = app.claudeCode() != nullptr ? app.claudeCode()->current() : nullptr;
        QVERIFY(panel != nullptr);
        const QString cut = kLong.left(50) + QChar(0x2026) + ".sch";
        panel->refreshDocument();
        QCOMPARE(panel->attachButton()->text(), cut);
        QVERIFY(panel->attachButton()->toolTip().contains(QDir::toNativeSeparators(file)));
        panel->pinDocument(file);
        QCOMPARE(panel->pinButton()->text(), cut);
        QVERIFY(panel->pinButton()->toolTip().contains(QDir::toNativeSeparators(file)));
        // The setting changed: the panel too.
        QucsSettings.FileNameCap = 10;
        app.titleFileNames();
        QCOMPARE(panel->pinButton()->text(), kLong.left(10) + QChar(0x2026) + ".sch");
        // (The chip of the document in front, shown again when unpinned.)
        panel->pinDocument(QString());
        QCOMPARE(panel->attachButton()->text(), kLong.left(10) + QChar(0x2026) + ".sch");
        QucsSettings.FileNameCap = 50;
        app.titleFileNames();
        QCOMPARE(panel->attachButton()->text(), cut);
    }

    // A subcircuit is not placed into itself: told by its file's name, not
    // its tab's (which is cut now).
    void aSubcircuitIsNotPlacedIntoItself()
    {
        const QString project = dir.filePath("long_prj");
        const QString file = project + "/" + kLong + ".sch";
        QVERIFY(copy(QStringLiteral(QUCS_EXAMPLES_DIR "/external_interface/probe_and_subcircuit/example_sub_subcircuit.sch"), file));
        QVERIFY(copy(file, project + "/short_sub.sch"));
        QucsApp app(false);
        MainGuard main(&app);
        app.projectView()->setProjPath(project);
        QVERIFY(app.gotoPage(file, false, false));
        // The Content panel's row of a file, once it has read the folder.
        const auto rowOf = [&app](const QString& name) {
            QAbstractItemModel* model = app.projectView()->model();
            QList<QModelIndex> stack{QModelIndex()};
            while (!stack.isEmpty()) {
                const QModelIndex parent = stack.takeFirst();
                for (int r = 0; r < model->rowCount(parent); ++r) {
                    const QModelIndex index = model->index(r, 0, parent);
                    if (QFileInfo(index.data(ProjectView::FilePathRole).toString()).fileName() == name) return index;
                    stack.append(index);
                }
            }
            return QModelIndex();
        };
        QTRY_VERIFY(rowOf(kLong + ".sch").isValid() && rowOf("short_sub.sch").isValid());
        QTRY_VERIFY(!rowOf(kLong + ".sch").sibling(rowOf(kLong + ".sch").row(), 1).data().toString().isEmpty());   // (a subcircuit)
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSelectSubcircuit", Q_ARG(QModelIndex, rowOf(kLong + ".sch"))));
        QVERIFY(app.view->selElem == nullptr);   // not into itself
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSelectSubcircuit", Q_ARG(QModelIndex, rowOf("short_sub.sch"))));
        QVERIFY(app.view->selElem != nullptr);   // another one: to place
        for (QucsDoc* d : app.allDocuments()) d->setDocChanged(false);
    }
};

QTEST_MAIN(TestLongFileNames)
#include "test_long_file_names.moc"
