/*
 * qucscontrol_settings.cpp - Claude's settings tools: Application
 *                            Settings, Simulators Settings, a document's
 *                            settings and CDL Settings read and set by
 *                            typed keys, through their own dialogs opened
 *                            as their menu actions open them (so what the
 *                            window does after them is done), each change
 *                            told with its old value
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"
#include "qucs.h"
#include "schematic.h"
#include "textdoc.h"
#include "dialogs/digisettingsdialog.h"
#include "dialogs/qucssettingsdialog.h"
#include "dialogs/settingsdialog.h"
#include "dialogs/vasettingsdialog.h"
#include "extsimkernels/CdlSettingsDialog.h"
#include "extsimkernels/simsettingsdialog.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include <memory>

using namespace qucs_s::control;

namespace {

// A button of \a dialog by what it says.
QPushButton* buttonOf(QWidget* dialog, const QStringList& texts)
{
    for (const QString& text : texts)
        for (QPushButton* b : dialog->findChildren<QPushButton*>())
            if (b->isVisibleTo(dialog) && b->text().remove(QLatin1Char('&')).compare(text, Qt::CaseInsensitive) == 0) return b;
    return nullptr;
}

// Closed without its changes.
void cancel(QWidget* dialog)
{
    if (QPushButton* b = buttonOf(dialog, {QStringLiteral("Cancel")})) b->click();
    else if (auto* d = qobject_cast<QDialog*>(dialog)) d->reject();
    else dialog->close();
}

} // namespace

QWidget* QucsControl::settingsDialogFor(const QJsonObject& args, QString* error)
{
    // Made as its action makes it, not shown: read, then deleted.
    const QString scope = args.value(QLatin1String("scope")).toString().trimmed().toLower();
    if (scope == QLatin1String("app")) return new QucsSettingsDialog(a_app);
    if (scope == QLatin1String("simulators")) return new SimSettingsDialog(a_app);
    if (scope == QLatin1String("cdl")) return new CdlSettingsDialog(a_app);
    if (scope != QLatin1String("document")) {
        *error = tr("'scope' is app (Application Settings), simulators, document (the document's own settings) or cdl.");
        return nullptr;
    }
    QucsDoc* doc = document(args, error);
    if (doc == nullptr) return nullptr;
    if (auto* sch = dynamic_cast<Schematic*>(doc)) return new SettingsDialog(sch);
    if (auto* text = dynamic_cast<TextDoc*>(doc)) {
        const QString suffix = text->fileSuffix();
        if (suffix == QLatin1String("va")) return new VASettingsDialog(text);
        if (suffix != QLatin1String("m") && suffix != QLatin1String("oct")) return new DigiSettingsDialog(text);
    }
    *error = tr("%1 has no settings of its own.").arg(titleOf(doc));
    return nullptr;
}

void QucsControl::withSettingsDialog(const QJsonObject& args, std::function<void(QWidget* dialog)> with, const Done& done)
{
    const QString scope = args.value(QLatin1String("scope")).toString().trimmed().toLower();
    QAction* action = nullptr;
    if (scope == QLatin1String("app")) action = a_app->applSettings;
    else if (scope == QLatin1String("simulators")) action = a_app->simSettings;
    else if (scope == QLatin1String("cdl")) action = a_app->cdlSettings;
    else if (scope == QLatin1String("document")) {
        // The document's own: the one 'path' names, brought to the front.
        QString error;
        QucsDoc* doc = document(args, &error);
        if (doc == nullptr) {
            done(errorResult(error));
            return;
        }
        a_app->showDocument(QucsApp::documentWidget(doc));
        action = a_app->fileSettings;
    } else {
        done(errorResult(tr("'scope' is app (Application Settings), simulators, document (the document's own settings) or cdl.")));
        return;
    }
    if (action == nullptr || !action->isEnabled()) {
        done(errorResult(tr("Those settings cannot be opened now.")));
        return;
    }
    if (QApplication::activeModalWidget() != nullptr) {
        done(errorResult(tr("A dialog waits for an answer: get_dialog reads it.")));
        return;
    }
    // Opened from the event loop (its own loop runs until it closes); handed
    // over once it is up.
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto handed = std::make_shared<bool>(false);
    auto* watch = new QTimer(this);
    watch->setInterval(20);
    connect(watch, &QTimer::timeout, this, [this, watch, clock, handed, with, done] {
        if (*handed) return;
        QWidget* dialog = openDialog();
        if (dialog != nullptr && qobject_cast<QMessageBox*>(dialog) == nullptr) {
            *handed = true;
            watch->stop();
            watch->deleteLater();
            with(dialog);
            return;
        }
        if (clock->elapsed() > 5000) {
            *handed = true;
            watch->stop();
            watch->deleteLater();
            done(errorResult(dialog != nullptr ? tr("“%1” came up instead: get_dialog reads it.").arg(dialog->windowTitle())
                                               : tr("Those settings have no dialog for the document in front.")));
        }
    });
    watch->start();
    QPointer<QAction> target(action);
    QTimer::singleShot(0, a_app, [target] {
        const QtFileDialogs qt;
        if (target) target->trigger();
    });
}

void QucsControl::getSettings(const QJsonObject& args, const Done& done)
{
    const QString scope = args.value(QLatin1String("scope")).toString().trimmed().toLower();
    QString error;
    std::unique_ptr<QWidget> dialog(settingsDialogFor(args, &error));
    if (dialog == nullptr) {
        done(errorResult(error));
        return;
    }
    done(jsonResult(QJsonObject{{QStringLiteral("scope"), scope},
                                {QStringLiteral("dialog"), dialog->windowTitle()},
                                {QStringLiteral("settings"), typedSettings(dialog.get(), nullptr)}}));
}

void QucsControl::setSettings(const QJsonObject& args, const Done& done)
{
    const QString scope = args.value(QLatin1String("scope")).toString().trimmed().toLower();
    const QJsonObject values = args.value(QLatin1String("values")).toObject();
    if (values.isEmpty()) {
        done(errorResult(tr("'values' names the settings and their new values: {\"Settings/Language\": \"English\"} - get_settings "
                            "lists the keys.")));
        return;
    }
    withSettingsDialog(args, [this, args, scope, values, done](QWidget* dialog) {
        QHash<QString, QWidget*> byKey;
        typedSettings(dialog, &byKey);
        // A key as get_settings gives it, or its label alone when that is
        // one setting's.
        const auto find = [&byKey](const QString& key) -> std::pair<QString, QWidget*> {
            for (auto it = byKey.cbegin(); it != byKey.cend(); ++it)
                if (it.key().compare(key, Qt::CaseInsensitive) == 0) return {it.key(), it.value()};
            std::pair<QString, QWidget*> only{QString(), nullptr};
            int found = 0;
            for (auto it = byKey.cbegin(); it != byKey.cend(); ++it)
                if (it.key().section(QLatin1Char('/'), -1).compare(key, Qt::CaseInsensitive) == 0) {
                    only = {it.key(), it.value()};
                    ++found;
                }
            return found == 1 ? only : std::pair<QString, QWidget*>{QString(), nullptr};
        };
        QJsonArray changed, notDone;
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            const auto [key, w] = find(it.key());
            if (w == nullptr) {
                notDone.append(QJsonObject{{QStringLiteral("key"), it.key()},
                                           {QStringLiteral("why"), tr("no such setting (get_settings lists them; a label shared by several "
                                                                      "needs its tab: \"Tab/Label\")")}});
                continue;
            }
            // (Claude Code's own settings are the user's.)
            if (key.contains(QLatin1String("Claude"), Qt::CaseInsensitive)) {
                notDone.append(QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("why"), tr("Claude Code's settings are the user's")}});
                continue;
            }
            const QJsonValue was = typedValue(w);
            QString why;
            if (!setTyped(dialog, w, it.value(), &why)) {
                notDone.append(QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("why"), why}});
                continue;
            }
            if (typedValue(w) != was) changed.append(QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("was"), was}});
        }
        QPointer<QWidget> open(dialog);
        if (changed.isEmpty()) {
            QTimer::singleShot(0, a_app, [open] {
                if (open) cancel(open);
            });
            QJsonObject result{{QStringLiteral("scope"), scope}, {QStringLiteral("changed"), changed}};
            if (!notDone.isEmpty()) result.insert(QStringLiteral("not done"), notDone);
            done(notDone.isEmpty() ? jsonResult(result) : textResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)), true));
            return;
        }
        // Applied as its own button applies them; a message it shows on the
        // way (a restart needed, a value refused) answered and told.
        QPushButton* commit = buttonOf(dialog, {QStringLiteral("OK"), QStringLiteral("Apply changes"), QStringLiteral("Apply")});
        if (commit == nullptr) {
            QTimer::singleShot(0, a_app, [open] {
                if (open) cancel(open);
            });
            done(errorResult(tr("“%1” has no OK or Apply: nothing was changed.").arg(dialog->windowTitle())));
            return;
        }
        QPointer<QPushButton> press(commit);
        QTimer::singleShot(0, a_app, [press] {
            const QtFileDialogs qt;
            if (press) press->click();
        });
        auto said = std::make_shared<QStringList>();
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto* watch = new QTimer(this);
        watch->setInterval(50);
        connect(watch, &QTimer::timeout, this, [this, watch, clock, said, open, args, scope, changed, notDone, done] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                *said << box->text() + (box->informativeText().isEmpty() ? QString() : QLatin1Char(' ') + box->informativeText());
                QAbstractButton* answer = box->defaultButton() != nullptr ? static_cast<QAbstractButton*>(box->defaultButton())
                                                                          : box->buttons().value(0);
                if (answer != nullptr) answer->click();
                else box->close();
                return;
            }
            if (open && open->isVisible() && clock->elapsed() < 3000) return;
            watch->stop();
            watch->deleteLater();
            if (open && open->isVisible()) {
                // Not taken: the dialog stayed (a value it refused).
                QTimer::singleShot(0, a_app, [open] {
                    if (open) cancel(open);
                });
                done(errorResult(tr("The settings were not taken: %1").arg(said->isEmpty() ? tr("the dialog stayed open.") : said->join(QLatin1Char(' ')))));
                return;
            }
            // Read again from the dialog as it is made now: what was kept.
            QString error;
            std::unique_ptr<QWidget> again(settingsDialogFor(args, &error));
            QHash<QString, QWidget*> now;
            if (again != nullptr) typedSettings(again.get(), &now);
            QJsonArray told;
            for (const QJsonValue& c : changed) {
                QJsonObject o = c.toObject();
                if (QWidget* w = now.value(o.value(QLatin1String("key")).toString())) o.insert(QStringLiteral("now"), typedValue(w));
                told.append(o);
            }
            QJsonObject result{{QStringLiteral("scope"), scope}, {QStringLiteral("changed"), told}};
            if (!notDone.isEmpty()) result.insert(QStringLiteral("not done"), notDone);
            if (!said->isEmpty()) result.insert(QStringLiteral("said"), QJsonArray::fromStringList(*said));
            result.insert(QStringLiteral("note"), tr("Each change's 'was' is its value before: set_settings with it puts it back."));
            done(jsonResult(result));
        });
        watch->start();
    }, done);
}
