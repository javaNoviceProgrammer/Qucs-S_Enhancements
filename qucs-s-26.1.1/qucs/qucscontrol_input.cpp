/*
 * qucscontrol_input.cpp - Claude's last resort: raw input - a click, a
 *                         double click, a drag, keys and typed text - on a
 *                         schematic's canvas or a part of the window, as
 *                         the user's mouse and keyboard would give it, and
 *                         a picture of it afterwards. Keys that would set
 *                         off an action Claude does not use (File > Exit,
 *                         the Claude Code panel's) are refused, and the
 *                         consoles and the Claude Code panel are not
 *                         reached.
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

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>

#include <memory>

using namespace qucs_s::control;

namespace {

QJsonObject picture(const QImage& image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return {{QStringLiteral("type"), QStringLiteral("image")},
            {QStringLiteral("data"), QString::fromLatin1(png.toBase64())},
            {QStringLiteral("mimeType"), QStringLiteral("image/png")}};
}

// \a w as it looks, one pixel a point (as the coordinates are given).
QImage grabbed(QWidget* w)
{
    QImage image = w->grab().toImage();
    if (!image.isNull() && image.size() != w->size()) image = image.scaled(w->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return image;
}

// Shown, as the user would show it: its dock, and its panel in front.
void inView(QWidget* root)
{
    for (QWidget* w = root; w != nullptr; w = w->parentWidget()) {
        if (auto* dock = qobject_cast<QDockWidget*>(w)) {
            dock->show();
            dock->raise();
        }
        if (auto* stack = qobject_cast<QStackedWidget*>(w->parentWidget()))
            if (auto* tabs = qobject_cast<QTabWidget*>(stack->parentWidget())) tabs->setCurrentWidget(w);
    }
}

// An action's text as the menu shows it.
QString named(QString text)
{
    text.remove(QLatin1Char('&'));
    return text.trimmed();
}

bool xyOf(const QJsonValue& v, QPoint* p)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 2 || !a.at(0).isDouble() || !a.at(1).isDouble()) return false;
    *p = QPoint(qRound(a.at(0).toDouble()), qRound(a.at(1).toDouble()));
    return true;
}

void mouse(QWidget* w, QEvent::Type type, const QPoint& local, Qt::MouseButton button, Qt::MouseButtons held, Qt::KeyboardModifiers modifiers)
{
    QMouseEvent e(type, QPointF(local), QPointF(w->mapToGlobal(local)), button, held, modifiers);
    QApplication::sendEvent(w, &e);
}

// The text a key types (a letter, a digit, a sign), with no modifier but
// Shift; empty for the others.
QString keyText(QKeyCombination k)
{
    const Qt::KeyboardModifiers m = k.keyboardModifiers();
    if ((m & ~Qt::ShiftModifier) != Qt::NoModifier || int(k.key()) >= 0x01000000) return {};
    const QChar c(int(k.key()));
    return (m & Qt::ShiftModifier) ? QString(c) : QString(c.toLower());
}

void keyPress(QWidget* w, QKeyCombination k, const QString& text)
{
    QKeyEvent down(QEvent::KeyPress, int(k.key()), k.keyboardModifiers(), text);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, int(k.key()), k.keyboardModifiers(), text);
    QApplication::sendEvent(w, &up);
}

} // namespace

void QucsControl::sendInput(const QJsonObject& args, const Done& done)
{
    const QString target = args.value(QLatin1String("target")).toString(QStringLiteral("canvas")).trimmed();
    const bool hasClick = args.contains(QLatin1String("click"));
    const QString keys = args.value(QLatin1String("keys")).toString().trimmed();
    const QString typed = args.value(QLatin1String("text")).toString();
    if (!hasClick && keys.isEmpty() && typed.isEmpty()) {
        done(errorResult(tr("Give 'click' (and 'drag_to', 'double'), 'keys' or 'text' - what the mouse and keyboard do.")));
        return;
    }
    QPoint click, dragTo;
    const bool drag = args.contains(QLatin1String("drag_to"));
    if ((hasClick && !xyOf(args.value(QLatin1String("click")), &click)) || (drag && !xyOf(args.value(QLatin1String("drag_to")), &dragTo))) {
        done(errorResult(tr("'click' and 'drag_to' are [x, y].")));
        return;
    }
    if (drag && !hasClick) {
        done(errorResult(tr("'drag_to' is where a drag from 'click' ends: give 'click' too.")));
        return;
    }
    const QString buttonName = args.value(QLatin1String("button")).toString(QStringLiteral("left")).trimmed().toLower();
    const Qt::MouseButton button = buttonName == QLatin1String("right")    ? Qt::RightButton
                                   : buttonName == QLatin1String("middle") ? Qt::MiddleButton
                                   : buttonName == QLatin1String("left")   ? Qt::LeftButton
                                                                           : Qt::NoButton;
    if (button == Qt::NoButton) {
        done(errorResult(tr("'button' is left, right or middle.")));
        return;
    }
    Qt::KeyboardModifiers modifiers;
    for (const QJsonValue& v : args.value(QLatin1String("modifiers")).toArray()) {
        const QString m = v.toString().trimmed().toLower();
        if (m == QLatin1String("shift")) modifiers |= Qt::ShiftModifier;
        else if (m == QLatin1String("ctrl") || m == QLatin1String("cmd") || m == QLatin1String("command")) modifiers |= Qt::ControlModifier;
        else if (m == QLatin1String("alt") || m == QLatin1String("option")) modifiers |= Qt::AltModifier;
        else if (m == QLatin1String("meta")) modifiers |= Qt::MetaModifier;
        else {
            done(errorResult(tr("'modifiers' are shift, ctrl (Command on a Mac), alt and meta.")));
            return;
        }
    }
    const bool doubleClick = args.value(QLatin1String("double")).toBool();
    const bool pixels = args.value(QLatin1String("pixels")).toBool();

    // Where: the canvas of a schematic, or a part of the window (not a
    // console, not the Claude Code panel).
    QPointer<Schematic> sch;
    QPointer<QWidget> root;      // what the picture shows, what the points are in
    QString where;
    if (target.compare(QLatin1String("canvas"), Qt::CaseInsensitive) == 0) {
        QString error;
        sch = schematic(args, &error, false);
        if (!sch) {
            done(errorResult(error));
            return;
        }
        a_app->showDocument(QucsApp::documentWidget(sch));
        root = sch->viewport();
        where = tr("the canvas of %1").arg(titleOf(sch));
    } else if (target.compare(QLatin1String("tabs"), Qt::CaseInsensitive) == 0) {
        done(errorResult(tr("send_input reaches a schematic's canvas, a dock or panel, a toolbar or the status bar: set_ui brings a tab "
                            "forward, context_menu opens its menu.")));
        return;
    } else {
        QString name, error;
        root = uiArea(target, &name, &error, true);
        if (!root) {
            done(errorResult(error));
            return;
        }
        inView(root);
        // Laid out as shown now, from the window down (a page brought
        // forward kept the size it had when it was hidden).
        QList<QWidget*> up;
        for (QWidget* w = root; w != nullptr; w = w->parentWidget()) up.prepend(w);
        for (QWidget* w : std::as_const(up))
            if (w->layout() != nullptr) w->layout()->activate();
        where = name;
    }

    // The points, in the picture's pixels: from the schematic's
    // coordinates on the canvas (brought into view when they are not).
    QPoint at = click, to = dragTo;
    if (sch && hasClick && !pixels) {
        const QRect shown = root->rect().adjusted(4, 4, -4, -4);
        if (!shown.contains(sch->modelToViewport(click)) || (drag && !shown.contains(sch->modelToViewport(dragTo)))) {
            sch->centerOn(drag ? (click + dragTo) / 2 : click);
            if (!shown.contains(sch->modelToViewport(click)) || (drag && !shown.contains(sch->modelToViewport(dragTo)))) {
                done(errorResult(tr("The drag from %1, %2 to %3, %4 does not fit in the canvas at this zoom: zoom out first (zoom).")
                                     .arg(click.x()).arg(click.y()).arg(dragTo.x()).arg(dragTo.y())));
                return;
            }
        }
        at = sch->modelToViewport(click);
        to = sch->modelToViewport(dragTo);
    } else if (hasClick && (!root->rect().contains(at) || (drag && !root->rect().contains(to)))) {
        done(errorResult(tr("%1, %2 is outside %3, which is %4 x %5 pixels (its picture's).")
                             .arg(root->rect().contains(at) ? to.x() : at.x()).arg(root->rect().contains(at) ? to.y() : at.y())
                             .arg(where).arg(root->width()).arg(root->height())));
        return;
    }

    // The keys: each as the user's keyboard gives it - the action whose
    // shortcut it is set off (refused, with nothing sent, when it is one
    // Claude does not use), else pressed on what has the focus there. (A
    // key sent goes through the window's shortcuts all the same: every
    // action that has it is looked at first - the window's, and any
    // widget's own, the Claude Code panel's among them.)
    QKeySequence sequence;
    if (!keys.isEmpty()) {
        sequence = QKeySequence::fromString(keys, QKeySequence::PortableText);
        bool known = !sequence.isEmpty();
        for (int i = 0; i < sequence.count(); ++i) known = known && sequence[i].key() != Qt::Key_unknown;
        if (!known) {
            sequence = QKeySequence::fromString(keys, QKeySequence::NativeText);
            known = !sequence.isEmpty();
            for (int i = 0; i < sequence.count(); ++i) known = known && sequence[i].key() != Qt::Key_unknown;
        }
        if (!known) {
            done(errorResult(tr("'keys' %1 are not keys: \"Ctrl+Z\", \"Delete\", \"Escape, Return\" (at most four, as list_actions "
                                "writes shortcuts; Ctrl is Command on a Mac).").arg(keys)));
            return;
        }
    }
    QDockWidget* claude = a_app->claudeDockWidget();
    const auto inClaude = [claude](QObject* o) {
        for (; o != nullptr; o = o->parent())
            if (o == claude) return true;
        return false;
    };
    // (An action is the panel's when it is in it, or shown on a widget of it.)
    const auto claudes = [&inClaude](QAction* a) {
        if (inClaude(a)) return true;
        for (QObject* o : a->associatedObjects())
            if (inClaude(o)) return true;
        return false;
    };
    QList<QAction*> actions = a_app->findChildren<QAction*>();
    for (QWidget* w : QApplication::allWidgets())
        for (QAction* a : w->actions())
            if (!actions.contains(a)) actions << a;
    // (A widget's own shortcut is live only on it.)
    const auto live = [&root](QObject* owner, Qt::ShortcutContext context) {
        if (context == Qt::WindowShortcut || context == Qt::ApplicationShortcut) return true;
        auto* w = qobject_cast<QWidget*>(owner);
        return w != nullptr && root != nullptr && (w == root || w->isAncestorOf(root) || root->isAncestorOf(w));
    };
    struct Key {
        QKeyCombination combination;
        QPointer<QAction> action;
        QPointer<QShortcut> shortcut;
        QString name;
    };
    QList<Key> pressed;
    QStringList sets;   // what each key sets off
    for (int i = 0; i < sequence.count(); ++i) {
        Key k{sequence[i], nullptr, nullptr, QKeySequence(sequence[i]).toString(QKeySequence::NativeText)};
        for (QAction* a : std::as_const(actions)) {
            if (!a->shortcuts().contains(QKeySequence(sequence[i]))) continue;
            if (!live(a->parent(), a->shortcutContext()) && !claudes(a)) continue;
            if (QString why = refusedAction(a); !why.isEmpty() || claudes(a)) {
                if (why.isEmpty()) why = tr("it is the Claude Code panel's, which is the user's.");
                done(errorResult(tr("'keys' %1 would set off %2: %3 Nothing was sent.").arg(k.name, named(a->text()), why)));
                return;
            }
            if (k.action == nullptr && a->isEnabled()) k.action = a;
        }
        if (k.action == nullptr)
            for (QShortcut* s : a_app->findChildren<QShortcut*>()) {
                if (s->key() != QKeySequence(sequence[i]) || !s->isEnabled()) continue;
                if (inClaude(s)) {
                    done(errorResult(tr("'keys' %1 is a key of the Claude Code panel, which is the user's. Nothing was sent.").arg(k.name)));
                    return;
                }
                if (live(s->parent(), s->context())) k.shortcut = s;
            }
        sets << (k.action ? tr("%1: %2").arg(k.name, named(k.action->text())) : tr("%1: pressed").arg(k.name));
        pressed << k;
    }

    // Given from the event loop (a click may open a dialog or a menu, which
    // runs a loop of its own until it closes); answered once it has
    // settled, or what it opened is up.
    struct Run {
        bool given = false;     // all of it given
        bool keysSkipped = false;
        bool answered = false;
        QElapsedTimer clock, since;
        QPointer<QWidget> dialogBefore;
        QPointer<QWidget> clicked;   // what the click focused: the keys go there
    };
    auto run = std::make_shared<Run>();
    run->clock.start();
    run->dialogBefore = openDialog();
    QPointer<QWidget> receiver = root;
    if (hasClick) {
        // On the widget under the point (a view's viewport, a button).
        if (QWidget* under = sch ? nullptr : root->childAt(at)) receiver = under;
    }
    const QPoint local = receiver && receiver != root ? receiver->mapFrom(root, at) : at;
    const QPoint localTo = receiver && receiver != root ? receiver->mapFrom(root, to) : to;
    if (hasClick)
        QTimer::singleShot(0, a_app, [run, receiver, local, localTo, drag, doubleClick, button, modifiers] {
            const QtFileDialogs qt;
            if (!receiver) return;
            mouse(receiver, QEvent::MouseMove, local, Qt::NoButton, Qt::NoButton, modifiers);
            mouse(receiver, QEvent::MouseButtonPress, local, button, button, modifiers);
            if (drag) {
                // In steps, as a hand drags - the first where it was
                // pressed (a canvas takes the first to set the move up,
                // and moves nothing with it).
                if (receiver) mouse(receiver, QEvent::MouseMove, local, Qt::NoButton, button, modifiers);
                for (int i = 1; i <= 8 && receiver; ++i)
                    mouse(receiver, QEvent::MouseMove, local + (localTo - local) * i / 8, Qt::NoButton, button, modifiers);
                if (receiver) mouse(receiver, QEvent::MouseButtonRelease, localTo, button, Qt::NoButton, modifiers);
                return;
            }
            if (receiver) mouse(receiver, QEvent::MouseButtonRelease, local, button, Qt::NoButton, modifiers);
            // (Focused, as a click focuses what takes it: the keys after go there.)
            if (receiver && (receiver->focusPolicy() & Qt::ClickFocus) == Qt::ClickFocus) {
                receiver->setFocus(Qt::MouseFocusReason);
                run->clicked = receiver;
            }
            if (doubleClick && receiver) {
                mouse(receiver, QEvent::MouseButtonDblClick, local, button, button, modifiers);
                if (receiver) mouse(receiver, QEvent::MouseButtonRelease, local, button, Qt::NoButton, modifiers);
            }
        });
    QPointer<Schematic> canvas = sch;
    QTimer::singleShot(0, a_app, [this, run, pressed, typed, root, canvas] {
        const QtFileDialogs qt;
        // (Not into a dialog the click opened: the keys were for here.)
        if (QWidget* d = openDialog(); d != nullptr && d != run->dialogBefore && (!pressed.isEmpty() || !typed.isEmpty())) {
            run->keysSkipped = true;
        } else {
            // What has the focus there takes the keys (the canvas: the
            // schematic; a panel: its field).
            const auto focused = [root, canvas, run]() -> QWidget* {
                if (run->clicked) return run->clicked;
                QWidget* f = QApplication::focusWidget();
                if (canvas) return f != nullptr && canvas->isAncestorOf(f) ? f : static_cast<QWidget*>(canvas.data());
                return f != nullptr && root && (f == root || root->isAncestorOf(f)) ? f : root.data();
            };
            for (const Key& k : pressed) {
                if (k.action) k.action->trigger();
                else if (k.shortcut) emit k.shortcut->activated();
                else if (QWidget* w = focused()) keyPress(w, k.combination, keyText(k.combination));
            }
            for (const QChar c : typed)
                if (QWidget* w = focused()) keyPress(w, QKeyCombination(Qt::Key(c.toUpper().unicode())), QString(c));
        }
        run->given = true;
        run->since.start();
    });

    // Answered: what it opened, or a moment after it was all given - with a
    // picture of it.
    auto* watch = new QTimer(this);
    watch->setInterval(50);
    const QString report = QStringLiteral("%1%2%3")
                               .arg(hasClick ? tr("%1%2 %3 at %4, %5%6")
                                                   .arg(doubleClick ? tr("double ") : QString(), buttonName, tr("click"))
                                                   .arg(click.x()).arg(click.y())
                                                   .arg(drag ? tr(", dragged to %1, %2").arg(dragTo.x()).arg(dragTo.y()) : QString())
                                             : QString(),
                                    sets.isEmpty() ? QString() : (hasClick ? QStringLiteral("; ") : QString()) + tr("keys %1").arg(sets.join(QStringLiteral(", "))),
                                    typed.isEmpty() ? QString() : (hasClick || !sets.isEmpty() ? QStringLiteral("; ") : QString()) + tr("typed \"%1\"").arg(typed.left(80)));
    connect(watch, &QTimer::timeout, this, [this, watch, run, done, root, canvas, where, report] {
        if (run->answered) return;
        QJsonObject o{{QStringLiteral("on"), where}, {QStringLiteral("given"), report}};
        QWidget* dialog = openDialog();
        const bool opened = dialog != nullptr && dialog != run->dialogBefore;
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr && !opened && !(run->given && run->since.elapsed() > 400) && run->clock.elapsed() < 10000) return;
        run->answered = true;
        watch->stop();
        watch->deleteLater();
        if (menu != nullptr) {
            // (Its loop waits: closed, and read - context_menu chooses.)
            QStringList entries;
            for (QAction* a : menu->actions())
                if (!a->isSeparator() && a->isVisible() && !a->text().isEmpty()) entries << named(a->text());
            o.insert(QStringLiteral("menu"), tr("A menu came up and was closed: %1. context_menu opens it and chooses from it.").arg(entries.join(QStringLiteral("; "))));
            for (int n = 0; n < 5 && QApplication::activePopupWidget() != nullptr; ++n) QApplication::activePopupWidget()->close();
        }
        if (opened)
            o.insert(QStringLiteral("opened"), tr("“%1”, which waits for an answer: get_dialog reads it, set_dialog answers it.")
                                                   .arg(dialog->windowTitle().isEmpty() ? QString::fromLatin1(dialog->metaObject()->className()) : dialog->windowTitle()));
        if (run->keysSkipped) o.insert(QStringLiteral("keys"), tr("not sent: the click opened a dialog first"));
        if (!run->given && !opened && menu == nullptr) o.insert(QStringLiteral("note"), tr("Not all of it was given within 10 s."));
        if (const QString said = a_app->statusBar()->currentMessage(); !said.isEmpty()) o.insert(QStringLiteral("status bar"), said);
        QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                       {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))}}};
        if (root) {
            content.append(picture(grabbed(root)));
            QString how = tr("The picture: %1 as it is now, %2 x %3 pixels").arg(where).arg(root->width()).arg(root->height());
            if (canvas) {
                const QPoint topLeft = canvas->viewportToModel(QPoint(0, 0)), bottomRight = canvas->viewportToModel(QPoint(root->width(), root->height()));
                how += tr(" - the schematic from %1, %2 to %3, %4 (a point there is 'click' as it is; with 'pixels', its pixel)")
                           .arg(topLeft.x()).arg(topLeft.y()).arg(bottomRight.x()).arg(bottomRight.y());
            } else {
                how += tr(" (a point of it is 'click' as it is)");
            }
            content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), how + QLatin1Char('.')}});
        }
        done(QJsonObject{{QStringLiteral("content"), content}, {QStringLiteral("isError"), false}});
    });
    watch->start();
}
