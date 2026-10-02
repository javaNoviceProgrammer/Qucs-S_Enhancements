/*
 * Keeps a test's settings out of the user's own preferences.
 *
 * The application opens its store through QucsSettingsFile (settings.h),
 * in QSettings::defaultFormat(): once this has been called, the settings
 * manager, the recent-file lists a QucsApp writes as documents open, the
 * dialog geometries, all go to an INI file under the given directory, and
 * so do the Claude Code dock's conversations. Call it before the first
 * QucsApp is created.
 */
#ifndef ISOLATED_SETTINGS_H
#define ISOLATED_SETTINGS_H

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QString>
#include <QTimer>

#include <memory>

#include "autosave.h"
#include "settingsio.h"

/*
 * A dialog or a menu left open more than \a seconds - one the test did not
 * expect, as a box of a machine without what this one has ("No simulation
 * backend found" on CI's runner, which has no ngspice) - is said with its
 * words and closed: the test fails on what came of it, rather than at
 * ctest's timeout with nothing said (two suites timed out on CI so, for
 * days). Called by useIsolatedSettings; once per process.
 */
inline void watchForDialogsLeftOpen(int seconds = 60)
{
    static bool started = false;
    if (started || qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr) return;
    started = true;
    auto* timer = new QTimer(QCoreApplication::instance());
    auto since = std::make_shared<QHash<QWidget*, qint64>>();
    QObject::connect(timer, &QTimer::timeout, [since, seconds] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // (Rebuilt from the windows there are: a pointer kept is only
        // compared, never followed once its window is gone.)
        QHash<QWidget*, qint64> open;
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->isVisible() && (w->isModal() || qobject_cast<QMenu*>(w) != nullptr)) open.insert(w, since->value(w, now));
        *since = open;
        for (auto it = open.cbegin(); it != open.cend(); ++it) {
            if (now - it.value() < seconds * 1000LL) continue;
            QWidget* w = it.key();
            QStringList words;
            if (auto* box = qobject_cast<QMessageBox*>(w)) words << box->text() << box->informativeText();
            else
                for (QLabel* l : w->findChildren<QLabel*>()) words << l->text();
            qCritical().noquote() << QStringLiteral("A %1 left open %2 s, closed: \"%3\" %4")
                                         .arg(QString::fromLatin1(w->metaObject()->className()))
                                         .arg(seconds)
                                         .arg(w->windowTitle(), words.join(QStringLiteral(" | ")).left(600));
            since->remove(w);
            if (auto* dialog = qobject_cast<QDialog*>(w)) dialog->reject();
            else w->close();
            break;   // (the others at the next look: closing one may close them)
        }
    });
    timer->start(1000);
}

inline void useIsolatedSettings(const QString& dir)
{
    watchForDialogsLeftOpen();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
    // The Claude Code dock's conversations, and Claude Code's own sessions
    // it lists: there too, not the user's.
    qputenv("QUCS_CLAUDE_HISTORY", (dir + "/claude-conversations").toUtf8());
    qputenv("CLAUDE_CONFIG_DIR", (dir + "/claude-config").toUtf8());
    // The caches - project scratch folders, the netlists runs are kept
    // with: there too (misc::cacheDir()).
    qputenv("QUCS_CACHE_DIR", (dir + "/cache").toUtf8());
    // What Claude's tools move to the trash (clean_scratch, trash_file):
    // there too, not into the user's trash (QUCS_TRASH_DIR).
    qputenv("QUCS_TRASH_DIR", (dir + "/trash").toUtf8());
    // The documents' autosaves and the backups an import of settings makes:
    // there too - they went into ~/Library/Application Support/<the test>
    // (a test sets its own after this, as test_autosave does).
    qucs_s::autosave::setDirectory(dir + "/autosave");
    qucs_s::settingsio::setBackupDirectory(dir + "/settings-backups");
}

#endif // ISOLATED_SETTINGS_H
