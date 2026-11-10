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
#include "datadisplay.h"
#include "layoutdoc.h"
#include "pptx.h"
#include "pptxdoc.h"
#include "qucs.h"
#include "schematic.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCompleter>
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

#include <cmath>
#include <functional>
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

// Laid out as shown now, from the window down - a page brought forward
// kept the size it had when it was hidden, a document just opened has its
// layout still to come - so that the points are where its pixels will be
// when the mouse's events come.
void laidOut(QWidget* root)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QList<QWidget*> up;
    for (QWidget* w = root; w != nullptr; w = w->parentWidget()) up.prepend(w);
    for (QWidget* w : std::as_const(up))
        if (w->layout() != nullptr) w->layout()->activate();
}

// An action's text as the menu shows it.
QString named(QString text)
{
    text.remove(QLatin1Char('&'));
    return text.trimmed();
}

bool xyOf(const QJsonValue& v, QPointF* p)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 2 || !a.at(0).isDouble() || !a.at(1).isDouble()) return false;
    // (Within reach of an int: 1e308 rounded into one was undefined.)
    if (!(std::abs(a.at(0).toDouble()) <= 1e7) || !(std::abs(a.at(1).toDouble()) <= 1e7)) return false;
    *p = QPointF(a.at(0).toDouble(), a.at(1).toDouble());
    return true;
}

// The most points a drag goes through ('drag_through').
constexpr int MostDragged = 1000;

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
    // Where the mouse goes, in the target's measures: 'click', then a drag
    // to 'drag_to', or through each of 'drag_through' in turn - a stroke's
    // shape, on a layout or a slide with the pen taken up.
    QPointF click, dragTo;
    QList<QPointF> dragged;
    const bool through = args.contains(QLatin1String("drag_through"));
    const bool drag = args.contains(QLatin1String("drag_to")) || through;
    if ((hasClick && !xyOf(args.value(QLatin1String("click")), &click))
        || (args.contains(QLatin1String("drag_to")) && !xyOf(args.value(QLatin1String("drag_to")), &dragTo))) {
        done(errorResult(tr("'click' and 'drag_to' are [x, y], each within 10,000,000 of 0.")));
        return;
    }
    if (through && args.contains(QLatin1String("drag_to"))) {
        done(errorResult(tr("'drag_to' or 'drag_through', not both: a drag through points ends at the last of them.")));
        return;
    }
    if (through) {
        const QJsonArray points = args.value(QLatin1String("drag_through")).toArray();
        bool good = !points.isEmpty() && points.size() <= MostDragged;
        for (const QJsonValue& v : points) {
            QPointF p;
            good = good && xyOf(v, &p);
            dragged << p;
        }
        if (!good) {
            done(errorResult(tr("'drag_through' is a list of [x, y] - one to %1 of them, each within 10,000,000 of 0 - that a drag "
                                "from 'click' goes through in turn.").arg(MostDragged)));
            return;
        }
        dragTo = dragged.last();
    } else if (drag) {
        dragged << dragTo;
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

    // Where: the canvas of a schematic, a layout's view or a presentation's
    // slides, or a part of the window (not a console, not the Claude Code
    // panel).
    QPointer<Schematic> sch;
    QPointer<qucs_s::layout::LayoutView> layoutView;
    QPointer<qucs_s::pptx::SlideView> slideView;
    QPointer<PptxDoc> slides;    // whose they are
    int slide = -1;              // the slide the points are on
    QPointer<QWidget> root;      // what the picture shows, what the points are in
    QString where;
    const bool intoDialog = target.compare(QLatin1String("dialog"), Qt::CaseInsensitive) == 0;
    if (intoDialog) {
        // The dialog that waits for an answer: its picture's pixels (get_dialog
        // gives each control's 'at'); keys go to its field with the focus,
        // never to the window's actions, which wait while it is open.
        root = openDialog();
        if (!root) {
            done(errorResult(tr("No dialog is open: 'target' dialog is for one that waits for an answer (get_dialog reads it).")));
            return;
        }
        where = tr("the dialog “%1”").arg(root->windowTitle());
    } else if (target.compare(QLatin1String("canvas"), Qt::CaseInsensitive) == 0) {
        QString error;
        sch = schematic(args, &error, false);
        if (!sch) {
            done(errorResult(error));
            return;
        }
        a_app->showDocument(QucsApp::documentWidget(sch));
        root = sch->viewport();
        where = tr("the canvas of %1").arg(titleOf(sch));
    } else if (target.compare(QLatin1String("layout"), Qt::CaseInsensitive) == 0) {
        // An open layout's view: the points µm on the cell it shows, y up,
        // as its pen's strokes and read_drawings have them.
        LayoutDoc* doc = nullptr;
        const QString file = args.value(QLatin1String("path")).toString().trimmed();
        if (file.isEmpty()) {
            doc = qobject_cast<LayoutDoc*>(a_app->DocumentTab->count() > 0 ? a_app->DocumentTab->currentWidget() : nullptr);
        } else {
            for (QucsDoc* d : a_app->allDocuments())
                if (auto* l = qobject_cast<LayoutDoc*>(QucsApp::documentWidget(d)); l != nullptr && sameFile(l->getDocName(), absolute(file)))
                    doc = l;
        }
        if (doc == nullptr) {
            done(errorResult(file.isEmpty() ? tr("Which layout? 'path' (the document in front is none): show_layout opens one.")
                                            : tr("%1 is not open as a layout: show_layout opens it.").arg(file)));
            return;
        }
        if (doc->isLoading() || !doc->layout() || doc->view()->drawingPart().isEmpty()) {
            done(errorResult(doc->isLoading() ? tr("%1 is still being read: show_layout waits for it.").arg(titleOf(doc))
                             : doc->problem().isEmpty() ? tr("%1 shows no cell.").arg(titleOf(doc))
                                                        : doc->problem()));
            return;
        }
        a_app->showDocument(doc);
        layoutView = doc->view();
        root = layoutView.data();
        laidOut(root);
        where = tr("the cell %1 of %2").arg(layoutView->drawingPart(), titleOf(doc));
    } else if (target.compare(QLatin1String("slide"), Qt::CaseInsensitive) == 0) {
        // An open presentation's slides: the points from the top left of
        // 'slide' (the one in sight), as its pen's strokes and read_drawings
        // have them.
        QString error;
        PptxDoc* deck = presentationFound(args, &error);
        if (deck == nullptr || deck->presentation() == nullptr) {
            done(errorResult(!error.isEmpty() ? error
                             : deck != nullptr ? (deck->problem().isEmpty() ? tr("%1 shows nothing.").arg(titleOf(deck)) : deck->problem())
                                               : tr("%1 is not open: show_pptx opens it.").arg(args.value(QLatin1String("path")).toString())));
            return;
        }
        const int count = deck->presentation()->slideCount();
        if (count == 0) {
            done(errorResult(tr("%1 has no slides.").arg(titleOf(deck))));
            return;
        }
        slide = deck->currentSlide();
        if (args.contains(QLatin1String("slide"))) {
            // (Judged first: toInt hands back its default for 2.5 or "2".)
            const QJsonValue v = args.value(QLatin1String("slide"));
            const double n = v.toDouble(-1);
            if (!v.isDouble() || n != std::floor(n) || n < 1 || n > count) {
                done(errorResult(tr("'slide' is a slide's number, from 1 to %1.").arg(count)));
                return;
            }
            slide = int(n) - 1;
        }
        a_app->showDocument(deck);
        slides = deck;
        slideView = deck->view();
        root = slideView->viewport();
        laidOut(root);
        where = tr("slide %1 of %2").arg(slide + 1).arg(titleOf(deck));
    } else if (target.compare(QLatin1String("tabs"), Qt::CaseInsensitive) == 0) {
        done(errorResult(tr("send_input reaches a schematic's canvas, a layout's view, a presentation's slides, a dock or panel, a "
                            "toolbar or the status bar: set_ui brings a tab forward, context_menu opens its menu.")));
        return;
    } else {
        QString name, error;
        root = uiArea(target, &name, &error, true);
        if (!root) {
            done(errorResult(error));
            return;
        }
        inView(root);
        laidOut(root);
        where = name;
    }

    // The points, in the picture's pixels: from the schematic's coordinates
    // on the canvas, a cell's µm on a layout, a slide's points on a slide -
    // brought into view when they are not, the zoom as it is.
    QList<QPointF> asked{click};
    asked << dragged;
    QList<QPoint> path;   // the click, then where the drag goes
    // (Mapped now, to bring them into view, and again as the events are
    // given: what comes between moves them - Qt shows a scroll bar by a
    // queued call, and slides fitted to the width are fitted again to the
    // narrower viewport it leaves: a stroke drawn 23 points off its click,
    // on CI's Linux.)
    std::function<QList<QPoint>()> mapped;
    if (hasClick && !pixels && (sch || layoutView || slideView)) {
        const auto toPixel = [sch, layoutView, slideView, slide](QPointF p) {
            if (sch) return sch->modelToViewport(p.toPoint());
            if (layoutView) return layoutView->toPixel(p).toPoint();
            return slideView ? slideView->toViewport(slide, p).toPoint() : p.toPoint();
        };
        const QRect shown = root->rect().adjusted(4, 4, -4, -4);
        const auto inSight = [&asked, &shown, &toPixel] {
            for (const QPointF& p : std::as_const(asked))
                if (!shown.contains(toPixel(p))) return false;
            return true;
        };
        double x0 = click.x(), x1 = x0, y0 = click.y(), y1 = y0;
        for (const QPointF& p : std::as_const(asked)) {
            x0 = std::min(x0, p.x());
            x1 = std::max(x1, p.x());
            y0 = std::min(y0, p.y());
            y1 = std::max(y1, p.y());
        }
        if (!inSight()) {
            const QPointF middle((x0 + x1) / 2, (y0 + y1) / 2);
            if (sch) sch->centerOn(middle.toPoint());
            else if (layoutView) layoutView->setView(middle, layoutView->scale());
            else slideView->centerOn(slide, middle);
            if (!inSight()) {
                done(errorResult(tr("%1 does not fit in %2 at this zoom: zoom out first (%3).")
                                     .arg(drag ? tr("The drag from %1, %2 to %3, %4").arg(x0).arg(y0).arg(x1).arg(y1)
                                               : tr("%1, %2").arg(click.x()).arg(click.y()),
                                          where,
                                          sch ? tr("zoom") : layoutView ? tr("show_layout's 'region'") : tr("show_pptx's 'zoom'"))));
                return;
            }
        }
        mapped = [toPixel, asked] {
            QList<QPoint> out;
            for (const QPointF& p : asked) out << toPixel(p);
            return out;
        };
        path = mapped();
    } else if (hasClick) {
        for (const QPointF& p : std::as_const(asked)) {
            if (!root->rect().contains(p.toPoint())) {
                done(errorResult(tr("%1, %2 is outside %3, which is %4 x %5 pixels (its picture's).")
                                     .arg(p.x()).arg(p.y()).arg(where).arg(root->width()).arg(root->height())));
                return;
            }
            path << p.toPoint();
        }
    }
    const QPoint at = path.value(0);

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
        if (intoDialog) {   // (pressed in the dialog: no action of the window)
            sets << tr("%1: pressed").arg(k.name);
            pressed << k;
            continue;
        }
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
        QStringList keys;            // what each key did, as given
    };
    auto run = std::make_shared<Run>();
    run->clock.start();
    run->dialogBefore = openDialog();
    QPointer<QWidget> receiver = root;
    if (hasClick) {
        // On the widget under the point (a view's viewport, a button) - a
        // canvas, a layout's view and slides taking their own.
        if (QWidget* under = sch || layoutView || slideView ? nullptr : root->childAt(at)) receiver = under;
    }
    // (The status bar's Claude chip shows and hides the Claude Code panel:
    // the user's, as the panel is.)
    if (receiver && usersOnly(receiver)) {
        done(errorResult(tr("What is at %1, %2 is the Claude Code panel's (the Claude chip, which shows and hides it): the "
                            "user's. Nothing was sent.").arg(click.x()).arg(click.y())));
        return;
    }
    QList<QPoint> asGiven;   // the path in the receiver's pixels
    for (const QPoint& p : std::as_const(path)) asGiven << (receiver && receiver != root ? receiver->mapFrom(root, p) : p);
    if (hasClick)
        QTimer::singleShot(0, a_app, [run, receiver, asGiven, mapped, through, drag, doubleClick, button, modifiers] {
            const QtFileDialogs qt;
            if (!receiver) return;
            // (A view's points where it shows them now: it is the receiver.)
            const QList<QPoint> local = mapped ? mapped() : asGiven;
            mouse(receiver, QEvent::MouseMove, local.first(), Qt::NoButton, Qt::NoButton, modifiers);
            mouse(receiver, QEvent::MouseButtonPress, local.first(), button, button, modifiers);
            if (drag) {
                // In steps, as a hand drags - the first where it was
                // pressed (a canvas takes the first to set the move up,
                // and moves nothing with it); through points, a step every
                // four pixels or so of each leg, as a pen's stroke has them.
                if (receiver) mouse(receiver, QEvent::MouseMove, local.first(), Qt::NoButton, button, modifiers);
                for (int k = 1; k < local.size() && receiver; ++k) {
                    const QPoint from = local.at(k - 1), next = local.at(k);
                    const int steps = through ? std::max(1, int(std::ceil(QLineF(from, next).length() / 4))) : 8;
                    for (int i = 1; i <= steps && receiver; ++i)
                        mouse(receiver, QEvent::MouseMove, from + (next - from) * i / steps, Qt::NoButton, button, modifiers);
                }
                if (receiver) mouse(receiver, QEvent::MouseButtonRelease, local.last(), button, Qt::NoButton, modifiers);
                return;
            }
            if (receiver) mouse(receiver, QEvent::MouseButtonRelease, local.first(), button, Qt::NoButton, modifiers);
            // (Focused, as a click focuses what takes it: the keys after go there.)
            if (receiver && (receiver->focusPolicy() & Qt::ClickFocus) == Qt::ClickFocus) {
                receiver->setFocus(Qt::MouseFocusReason);
                run->clicked = receiver;
            }
            if (doubleClick && receiver) {
                mouse(receiver, QEvent::MouseButtonDblClick, local.first(), button, button, modifiers);
                if (receiver) mouse(receiver, QEvent::MouseButtonRelease, local.first(), button, Qt::NoButton, modifiers);
            }
        });
    QPointer<Schematic> canvas = sch;
    // (Keys want their window active, as a user's typing has it: a field of
    // one not active has not Qt's focus - a list it opens below closes at
    // the first key; offscreen none is once a list has closed.)
    QPointer<QWidget> activating;
    if ((!pressed.isEmpty() || !typed.isEmpty()) && root && !intoDialog && openList() == nullptr)
        if (QWidget* window = root->window(); window != nullptr && QApplication::activeWindow() != window) {
            window->activateWindow();
            activating = window;
        }
    const auto give = [this, run, pressed, typed, root, canvas] {
        const QtFileDialogs qt;
        // (Not into a dialog the click opened: the keys were for here.)
        if (QWidget* d = openDialog(); d != nullptr && d != run->dialogBefore && (!pressed.isEmpty() || !typed.isEmpty())) {
            run->keysSkipped = true;
        } else {
            // What has the focus there takes the keys (the canvas: the
            // schematic; a panel: its field) - a list open below a field
            // (names offered as typed: a popup with the keyboard) before it,
            // as a keyboard's keys go to it: Return takes a name, an arrow
            // moves, a letter goes on to the field.
            const auto focused = [this, root, canvas, run]() -> QWidget* {
                if (QWidget* list = openList()) return list;
                if (run->clicked) return run->clicked;
                QWidget* f = QApplication::focusWidget();
                // (A window not active - a list it opened gone, offscreen, or
                // another window's field with Qt's focus, its activation not
                // come yet: a loaded runner's half second past - has Qt's
                // focus with none of its own: the field the window keeps it
                // on, as a user's keys go there once it is in front.)
                if (root && (f == nullptr || f->window() != root->window())) f = root->window()->focusWidget();
                if (canvas) return f != nullptr && canvas->isAncestorOf(f) ? f : static_cast<QWidget*>(canvas.data());
                return f != nullptr && root && (f == root || root->isAncestorOf(f)) ? f : root.data();
            };
            // (A key into the list: the field it is of keeps the focus when
            // it closes, as the keyboard's - its window active again: a
            // popup's going leaves none offscreen, and Qt's focus with no
            // widget.)
            const auto press = [this, run](QWidget* w, QKeyCombination key, const QString& text) {
                QCompleter* completer = nullptr;
                QPointer<QWidget> field = openList(&completer) == w && completer != nullptr ? completer->widget() : nullptr;
                keyPress(w, key, text);
                if (field && openList() == nullptr) {
                    if (QApplication::focusWidget() != field) {
                        if (QApplication::activeWindow() != field->window()) field->activateWindow();
                        field->setFocus(Qt::PopupFocusReason);
                    }
                    run->clicked = field;
                }
            };
            for (const Key& k : pressed) {
                // A key is the field's first when it claims it (a field's
                // Ctrl+A selects its text, not the window's all), as the
                // keyboard offers it - not a list's, whose keys are its own.
                QWidget* w = focused();
                bool claimed = w != nullptr && openList() != nullptr;
                if (w != nullptr && !claimed && (k.action || k.shortcut)) {
                    QKeyEvent over(QEvent::ShortcutOverride, int(k.combination.key()), k.combination.keyboardModifiers(), keyText(k.combination));
                    over.ignore();
                    QApplication::sendEvent(w, &over);
                    claimed = over.isAccepted();
                }
                if (claimed || (!k.action && !k.shortcut)) {
                    const bool toList = w != nullptr && w == openList();
                    if (w != nullptr) press(w, k.combination, keyText(k.combination));
                    run->keys << (toList ? tr("%1: in the list").arg(k.name) : claimed ? tr("%1: the field's").arg(k.name) : tr("%1: pressed").arg(k.name));
                } else if (k.action) {
                    k.action->trigger();
                    run->keys << tr("%1: %2").arg(k.name, named(k.action->text()));
                } else {
                    emit k.shortcut->activated();
                    run->keys << tr("%1: pressed").arg(k.name);
                }
            }
            // (A character typed with a list open: its field's, as the list
            // hands a letter on to it - not through the list, which closed at
            // it when the field had not Qt's focus: its window's activation
            // not come yet, as on CI's macOS runner, or behind another
            // program. Its field's completer narrows the list.)
            for (const QChar c : typed) {
                QCompleter* completer = nullptr;
                QWidget* w = openList(&completer) != nullptr && completer != nullptr && completer->widget() != nullptr ? completer->widget() : focused();
                if (w != nullptr) press(w, QKeyCombination(Qt::Key(c.toUpper().unicode())), QString(c));
            }
        }
        run->given = true;
        run->since.start();
    };
    // (Given as the window is active - its activation is an event to come
    // -, or half a second on without it (setActivationWait): then to the
    // field the window keeps its focus on, as focused() finds it.)
    if (activating) {
        auto* wait = new QTimer(a_app);
        wait->setInterval(10);
        QElapsedTimer asked;
        asked.start();
        connect(wait, &QTimer::timeout, a_app, [wait, activating, asked, give, most = a_activationWait] {
            if (activating && QApplication::activeWindow() != activating && asked.elapsed() < most) return;
            wait->stop();
            wait->deleteLater();
            give();
        });
        wait->start();
    } else {
        QTimer::singleShot(0, a_app, give);
    }

    // Answered: what it opened, or a moment after it was all given - with a
    // picture of it.
    // What the pen had drawn before, on a layout or slides - each part's
    // strokes - to say after what the input drew, or took away.
    QHash<QString, int> drawnBefore;
    int strokesBefore = 0;
    quint64 revisionBefore = 0;
    // (A data display's page: its own pen, on its canvas.)
    QPointer<Schematic> page = sch && sch->isDataDisplayPage() ? sch : QPointer<Schematic>();
    if (layoutView || slideView || page) {
        const qucs_s::markup::Drawing& drawing = layoutView ? layoutView->drawing() : slideView ? slideView->drawing() : page->penDrawing();
        for (const qucs_s::markup::Stroke& s : drawing.strokes()) drawnBefore[s.on] = std::max(drawnBefore.value(s.on), s.id);   // (the last number each part gave)
        strokesBefore = int(drawing.strokes().size());
        revisionBefore = drawing.revision();
    }
    auto* watch = new QTimer(this);
    watch->setInterval(50);
    const QString clicked = hasClick ? tr("%1%2 %3 at %4, %5%6")
                                           .arg(doubleClick ? tr("double ") : QString(), buttonName, tr("click"))
                                           .arg(click.x()).arg(click.y())
                                           .arg(through  ? tr(", dragged through %1 point(s) to %2, %3").arg(dragged.size()).arg(dragTo.x()).arg(dragTo.y())
                                                : drag ? tr(", dragged to %1, %2").arg(dragTo.x()).arg(dragTo.y())
                                                       : QString())
                                     : QString();
    connect(watch, &QTimer::timeout, this, [this, watch, run, done, root, canvas, layoutView, slideView, slides, slide, page, drawnBefore,
                                            strokesBefore, revisionBefore, where, clicked, sets, typed, intoDialog] {
        if (run->answered) return;
        // (Each key as it went: an action, the field's, the list's.)
        const QStringList keysSaid = run->keys.isEmpty() ? sets : run->keys;
        const QString report = QStringLiteral("%1%2%3").arg(
            clicked, keysSaid.isEmpty() ? QString() : (clicked.isEmpty() ? QString() : QStringLiteral("; ")) + tr("keys %1").arg(keysSaid.join(QStringLiteral(", "))),
            typed.isEmpty() ? QString() : (clicked.isEmpty() && keysSaid.isEmpty() ? QString() : QStringLiteral("; ")) + tr("typed \"%1\"").arg(typed.left(80)));
        QJsonObject o{{QStringLiteral("on"), where}, {QStringLiteral("given"), report}};
        // (The dialog answered: closed - its picture no longer shown.)
        const bool closed = intoDialog && (!root || !root->isVisible());
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
        if (opened) {
            o.insert(QStringLiteral("opened"), tr("“%1”, which waits for an answer: set_dialog answers it")
                                                   .arg(dialog->windowTitle().isEmpty() ? QString::fromLatin1(dialog->metaObject()->className()) : dialog->windowTitle()));
            o.insert(QStringLiteral("dialog"), dialogRead(dialog));
        }
        if (run->keysSkipped) o.insert(QStringLiteral("keys"), tr("not sent: the click opened a dialog first"));
        if (!run->given && !opened && menu == nullptr) o.insert(QStringLiteral("note"), tr("Not all of it was given within 10 s."));
        // (A dialog answered and another opened: 'dialog' the open one's.)
        if (closed) o.insert(opened ? QStringLiteral("closed") : QStringLiteral("dialog"), tr("closed: it was answered"));
        if (const QString said = a_app->statusBar()->currentMessage(); !said.isEmpty()) o.insert(QStringLiteral("status bar"), said);
        // (A list it opened below a field: what it offers - it is a window
        // of its own, drawn into the picture where it is.)
        QAbstractItemView* list = openList();
        if (list != nullptr) o.insert(QStringLiteral("list"), listRead(list));
        // (On a layout or slides: each stroke the pen drew, read as
        //  read_drawings reads it - what it marks -, or how many went.)
        if (layoutView || (slideView && slides) || page) {
            const qucs_s::markup::Drawing& drawing = layoutView ? layoutView->drawing() : slideView ? slideView->drawing() : page->penDrawing();
            if (drawing.revision() != revisionBefore) {
                QJsonArray drawn;
                const QJsonArray read = layoutView ? layoutView->readDrawing(false)
                                        : slides   ? slides->readDrawing(false)
                                                   : qucs_s::datadisplay::readDrawing(page, false);
                for (const QJsonValue& v : read) {
                    const QJsonObject r = v.toObject();
                    const QString part = layoutView ? r.value(QLatin1String("cell")).toString()
                                       : slides     ? QString::number(r.value(QLatin1String("slide")).toInt())
                                                    : Schematic::penPart();
                    if (r.value(QLatin1String("stroke")).toInt() > drawnBefore.value(part)) drawn.append(r);
                }
                if (!drawn.isEmpty()) o.insert(QStringLiteral("drawn"), drawn);
                if (drawing.strokes().size() < strokesBefore)
                    o.insert(QStringLiteral("taken away"), tr("%1 of the pen's stroke(s)").arg(strokesBefore - drawing.strokes().size()));
            }
        }
        QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                       {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))}}};
        if (root && !closed) {
            content.append(picture(list != nullptr ? withList(root, grabbed(root)) : grabbed(root)));
            QString how = tr("The picture: %1 as it is now, %2 x %3 pixels").arg(where).arg(root->width()).arg(root->height());
            if (canvas) {
                const QPoint topLeft = canvas->viewportToModel(QPoint(0, 0)), bottomRight = canvas->viewportToModel(QPoint(root->width(), root->height()));
                how += tr(" - the schematic from %1, %2 to %3, %4 (a point there is 'click' as it is; with 'pixels', its pixel)")
                           .arg(topLeft.x()).arg(topLeft.y()).arg(bottomRight.x()).arg(bottomRight.y());
            } else if (layoutView) {
                const QPointF low = layoutView->toLayout(QPointF(0, root->height())), high = layoutView->toLayout(QPointF(root->width(), 0));
                how += tr(" - the cell from %1, %2 to %3, %4 µm, y up (a point there is 'click' as it is; with 'pixels', its pixel)")
                           .arg(low.x(), 0, 'g', 6).arg(low.y(), 0, 'g', 6).arg(high.x(), 0, 'g', 6).arg(high.y(), 0, 'g', 6);
            } else if (slideView) {
                const QPointF topLeft = slideView->toSlide(slide, QPointF(0, 0)),
                              bottomRight = slideView->toSlide(slide, QPointF(root->width(), root->height()));
                how += tr(" - slide %1 from %2, %3 to %4, %5 points of it, from its top left (a point there is 'click' as it is; with "
                          "'pixels', its pixel)")
                           .arg(slide + 1)
                           .arg(topLeft.x(), 0, 'f', 1).arg(topLeft.y(), 0, 'f', 1).arg(bottomRight.x(), 0, 'f', 1).arg(bottomRight.y(), 0, 'f', 1);
            } else {
                how += tr(" (a point of it is 'click' as it is)");
            }
            content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), how + QLatin1Char('.')}});
        }
        done(QJsonObject{{QStringLiteral("content"), content}, {QStringLiteral("isError"), false}});
    });
    watch->start();
}
