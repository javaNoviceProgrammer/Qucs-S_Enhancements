/*
 * test_theme_switch.cpp - the theme changed with the main window open
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

// The main window has a style sheet (qucs.cpp), so every widget under it has
// Qt::WA_StyleSheet, and Qt passes a new palette from a window to children
// without that attribute only. Picking a theme left the Claude Code dock in
// the old colours but for the text, which follows the palette of its class:
// black on the dark transcript, in Light on a Mac whose system is dark.
//
// CTest runs this offscreen, where a platform theme is a palette of ours; run
// the program itself, with no QT_QPA_PLATFORM, for the platform's own switch
// (Cocoa, Windows). QUCS_THEME_PICTURES=<folder> keeps a picture of each step.
// docs/feature_gaps/2026-11-07-theme-switch-dock.md

#include <QtTest>
#include <QActionGroup>
#include <QDockWidget>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyle>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QToolBar>

#include "apptheme.h"
#include "claudecode.h"
#include "claudecodepanel.h"
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "ink.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"

using namespace qucs_s::apptheme;

namespace {

// WCAG's relative luminance and contrast of two colours.
double luminance(const QColor& c)
{
    const auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

double contrast(const QColor& a, const QColor& b)
{
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

} // namespace

class TestThemeSwitch : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QucsApp* app = nullptr;
    ClaudeCodePanel* panel = nullptr;
    int picture = 0;

    static void settle(int ms = 100)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    QAction* themeAction(int theme) const
    {
        for (QActionGroup* group : app->findChildren<QActionGroup*>())
            for (QAction* a : group->actions())
                if (a->isCheckable() && a->data().isValid() && a->data().toInt() == theme && a->text() == name(theme)) return a;
        return nullptr;
    }

    // What the window shows that is not in the application's colours.
    QStringList whatIsOff() const
    {
        QStringList off;
        const QPalette now = QApplication::palette();
        const auto colour = [&off](const QString& what, const QColor& shown, const QColor& meant) {
            if (shown.rgb() != meant.rgb()) off << QStringLiteral("%1 is %2, not %3").arg(what, shown.name(), meant.name());
        };
        colour(QStringLiteral("the main window"), app->palette().color(QPalette::Window), now.color(QPalette::Window));
        colour(QStringLiteral("the dock"), app->claudeDockWidget()->palette().color(QPalette::Window), now.color(QPalette::Window));
        colour(QStringLiteral("the panel's window"), panel->palette().color(QPalette::Window), now.color(QPalette::Window));
        colour(QStringLiteral("the panel's base"), panel->palette().color(QPalette::Base), now.color(QPalette::Base));
        colour(QStringLiteral("the panel's text"), panel->palette().color(QPalette::Text), now.color(QPalette::Text));
        for (QToolBar* bar : app->findChildren<QToolBar*>())
            if (bar->isVisible()) colour(bar->objectName(), bar->palette().color(QPalette::WindowText), now.color(QPalette::WindowText));
        // The pixels: the transcript's background the theme's, its text to be read on it.
        const QImage image = panel->transcript()->viewport()->grab().toImage();
        colour(QStringLiteral("the transcript's background"), image.pixelColor(image.width() - 3, image.height() - 3), now.color(QPalette::Base));
        double best = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) best = std::max(best, contrast(image.pixelColor(x, y), now.color(QPalette::Base)));
        if (best < 4.5) off << QStringLiteral("the transcript's text has a contrast of %1 at best").arg(best);
        // The reply made again in them: its code block on the theme's shade
        // of the base (what the panel mixes), not the last theme's.
        const QColor base = now.color(QPalette::Base);
        const QColor code = mix(base, now.color(QPalette::Text), qucs_s::ink::isDark(base) ? 0.10 : 0.05);
        qsizetype shaded = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (std::abs(c.red() - code.red()) <= 3 && std::abs(c.green() - code.green()) <= 3 && std::abs(c.blue() - code.blue()) <= 3) ++shaded;
            }
        if (shaded * 200 < qsizetype(image.width()) * image.height()) off << QStringLiteral("no code block in %1 in the transcript").arg(code.name());
        return off;
    }

    void shows(const QString& step)
    {
        const QString folder = qEnvironmentVariable("QUCS_THEME_PICTURES");
        settle();
        if (!folder.isEmpty())
            app->grab().save(QDir(folder).filePath(QStringLiteral("%1-%2.png").arg(++picture, 2, 10, QLatin1Char('0')).arg(step)));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsSettings.ExamplesDir = QStringLiteral(QUCS_EXAMPLES_DIR "/");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        Module::registerModules();
        // As main.cpp has it: the theme set, then the window.
        apply(System);
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1300, 900);
        app->show();
        app->claudeDockWidget()->show();
        panel = app->claudeDockWidget()->findChild<ClaudeCodePanel*>();
        QVERIFY(panel != nullptr);
        const QString reply = QStringLiteral("Some words of a reply, **bold** and `code`.\n\n```\nsome code\n```\n\nMore words after it.");
        const QJsonObject text{{"type", "text"}, {"text", reply}};
        const QJsonObject message{{"content", QJsonArray{text}}};
        panel->session()->handleLine(QJsonDocument(QJsonObject{{"type", "assistant"}, {"message", message}}).toJson(QJsonDocument::Compact));
        panel->renderNow();
        settle();
        qInfo("platform %s, the system's scheme %d", qPrintable(QApplication::platformName()), int(QGuiApplication::styleHints()->colorScheme()));
    }

    void cleanupTestCase()
    {
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
        apply(System);
    }

    // View > Theme, between the platform's themes and the designed ones.
    void theDockFollowsTheMenu_data()
    {
        QTest::addColumn<int>("theme");
        const QList<int> steps{Light, Dark, Light, System, Light, Daylight, Light, Nord, Dark, System, Nord, System};
        int n = 0;
        for (int theme : steps) QTest::addRow("%d %s", ++n, qPrintable(name(theme))) << theme;
    }

    void theDockFollowsTheMenu()
    {
        QFETCH(int, theme);
        QAction* action = themeAction(theme);
        QVERIFY(action != nullptr);
        action->trigger();
        (void)QTest::qWaitFor([this] { return whatIsOff().isEmpty(); }, 1500);
        const QStringList off = whatIsOff();
        QVERIFY2(off.isEmpty(), qPrintable(off.join(QStringLiteral("; "))));
        shows(QStringLiteral("menu-%1").arg(name(theme)));
    }

    // The theme put on directly, with no main window's applyLook() after it.
    void theDockFollowsApply_data()
    {
        QTest::addColumn<int>("theme");
        const QList<int> steps{Light, Dark, Nord, Light, System};
        int n = 0;
        for (int theme : steps) QTest::addRow("%d %s", ++n, qPrintable(name(theme))) << theme;
    }

    void theDockFollowsApply()
    {
        QFETCH(int, theme);
        apply(theme);
        (void)QTest::qWaitFor([this] { return whatIsOff().isEmpty(); }, 1500);
        const QStringList off = whatIsOff();
        QVERIFY2(off.isEmpty(), qPrintable(off.join(QStringLiteral("; "))));
        shows(QStringLiteral("apply-%1").arg(name(theme)));
    }

    // A widget's palette of its own stays its own.
    void aPaletteOfItsOwnIsKept()
    {
        QWidget own(panel);
        QPalette p = own.palette();
        p.setColor(QPalette::Window, QColor(0x12, 0x34, 0x56));
        own.setPalette(p);
        apply(Nord);
        QTRY_COMPARE_WITH_TIMEOUT(panel->palette().color(QPalette::Window), designedTheme(Nord)->colours.window, 1500);
        QCOMPARE(own.palette().color(QPalette::Window), QColor(0x12, 0x34, 0x56));
        QCOMPARE(own.palette().color(QPalette::WindowText), designedTheme(Nord)->colours.text);
        apply(System);
        QTRY_VERIFY_WITH_TIMEOUT(whatIsOff().isEmpty(), 1500);
    }
};

QTEST_MAIN(TestThemeSwitch)
#include "test_theme_switch.moc"
