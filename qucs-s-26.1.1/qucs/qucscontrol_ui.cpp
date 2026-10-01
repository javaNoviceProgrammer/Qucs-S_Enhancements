/*
 * qucscontrol_ui.cpp - Claude's tools for any part of the window: the
 *                      docks and their panels, the toolbars, the status
 *                      bar and the documents' tabs read and used as the
 *                      dialogs are (get_ui, set_ui); the right-click menus
 *                      opened, read and chosen from (context_menu). Not
 *                      the Claude Code panel, which is the user's.
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
#include "filebrowser.h"
#include "projectView.h"
#include "qucs.h"
#include "schematic.h"

#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>

#include <memory>

using namespace qucs_s::control;

namespace {

// A label as the user reads it: no mnemonic, no "...", no count ("Problems
// (3)" is Problems).
QString plain(QString text)
{
    text.remove(QLatin1Char('&'));
    text.replace(QChar(0x2026), QStringLiteral("..."));
    text.remove(QStringLiteral("..."));
    text.remove(QRegularExpression(QStringLiteral("\\s*\\(\\d+\\)$")));
    return text.simplified();
}

bool same(const QString& a, const QString& b)
{
    return plain(a).compare(plain(b), Qt::CaseInsensitive) == 0;
}

// The tab widget of a dock's panels: the dock's own widget, or the first
// one in it.
QTabWidget* panelsOf(QDockWidget* dock)
{
    if (dock->widget() == nullptr) return nullptr;
    if (auto* tabs = qobject_cast<QTabWidget*>(dock->widget())) return tabs;
    return dock->widget()->findChild<QTabWidget*>();
}

// A right-click menu's entries, each by its path ("Toggle hierarchy
// search view > Flat"), with whether it can be chosen and is checked.
void menuEntries(const QMenu* menu, const QString& prefix, QJsonArray* entries, QList<QAction*>* actions, QStringList* paths)
{
    for (QAction* a : menu->actions()) {
        if (a->isSeparator() || !a->isVisible() || plain(a->text()).isEmpty()) continue;
        const QString path = prefix.isEmpty() ? plain(a->text()) : prefix + QStringLiteral(" > ") + plain(a->text());
        if (a->menu() != nullptr) {
            menuEntries(a->menu(), path, entries, actions, paths);
            continue;
        }
        QJsonObject o{{QStringLiteral("entry"), path}};
        if (!a->isEnabled()) o.insert(QStringLiteral("enabled"), false);
        if (a->isCheckable()) o.insert(QStringLiteral("checked"), a->isChecked());
        if (!a->shortcut().isEmpty()) o.insert(QStringLiteral("shortcut"), a->shortcut().toString(QKeySequence::NativeText));
        entries->append(o);
        actions->append(a);
        paths->append(path);
    }
}

void closeMenus()
{
    for (int n = 0; n < 10; ++n) {
        QWidget* popup = QApplication::activePopupWidget();
        if (popup == nullptr) return;
        popup->close();
    }
}

void key(QWidget* w, int k)
{
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

// A row of \a view named by \a name: its text, or its path in the model
// (FilePathRole and the like), among all its rows.
QModelIndex rowNamed(const QAbstractItemView* view, const QString& name, int pathRole)
{
    const QAbstractItemModel* model = view->model();
    if (model == nullptr) return {};
    QList<QModelIndex> stack{view->rootIndex()};
    QModelIndex byText;
    while (!stack.isEmpty()) {
        const QModelIndex parent = stack.takeFirst();
        for (int r = 0; r < model->rowCount(parent); ++r) {
            const QModelIndex index = model->index(r, 0, parent);
            if (pathRole >= 0 && index.data(pathRole).toString() == name) return index;
            if (!byText.isValid() && index.data().toString().compare(name, Qt::CaseInsensitive) == 0) byText = index;
            stack.append(index);
        }
    }
    return byText;
}

} // namespace

// ----------------------------------------------------------------------
// The parts of the window

QJsonArray QucsControl::uiAreas() const
{
    QJsonArray list;
    QDockWidget* claude = a_app->claudeDockWidget();
    for (QDockWidget* dock : a_app->findChildren<QDockWidget*>()) {
        if (dock == claude || dock->windowTitle().isEmpty()) continue;
        QJsonObject o{{QStringLiteral("area"), QStringLiteral("dock:") + plain(dock->windowTitle())},
                      {QStringLiteral("shown"), !dock->isHidden()}};
        if (QTabWidget* tabs = panelsOf(dock)) {
            QJsonArray pages;
            for (int i = 0; i < tabs->count(); ++i) pages.append(QStringLiteral("dock:") + plain(tabs->tabText(i)));
            o.insert(QStringLiteral("panels"), pages);
        }
        if (dock == a_app->terminalDockWidget() || dock == a_app->pythonDockWidget() || same(dock->windowTitle(), QStringLiteral("Octave Dock")))
            o.insert(QStringLiteral("note"), tr("a console: read here; typed into with the console tool"));
        list.append(o);
    }
    for (QToolBar* bar : a_app->findChildren<QToolBar*>())
        if (bar->window() == a_app && !bar->windowTitle().isEmpty())
            list.append(QJsonObject{{QStringLiteral("area"), QStringLiteral("toolbar:") + plain(bar->windowTitle())},
                                    {QStringLiteral("shown"), !bar->isHidden()}});
    list.append(QJsonObject{{QStringLiteral("area"), QStringLiteral("statusbar")}, {QStringLiteral("shown"), a_app->statusBar()->isVisible()}});
    list.append(QJsonObject{{QStringLiteral("area"), QStringLiteral("tabs")}, {QStringLiteral("note"), tr("the documents' tabs, in each pane")}});
    return list;
}

QWidget* QucsControl::uiArea(const QString& area, QString* name, QString* error, bool forChange) const
{
    const QString a = area.trimmed();
    QDockWidget* claude = a_app->claudeDockWidget();
    const auto refuse = [&](QWidget* w) {
        if (claude != nullptr && w != nullptr && (w == claude || claude->isAncestorOf(w))) {
            *error = tr("The Claude Code panel is the user's: its prompts, permissions and settings are not Claude's to use.");
            return true;
        }
        if (!forChange || w == nullptr) return false;
        for (QWidget* console : {static_cast<QWidget*>(a_app->terminalDockWidget()), static_cast<QWidget*>(a_app->pythonDockWidget())})
            if (console != nullptr && (w == console || console->isAncestorOf(w))) {
                *error = tr("%1 is a console: what is typed there runs. The console tool types into it, asked each time.")
                             .arg(plain(static_cast<QDockWidget*>(console)->windowTitle()));
                return true;
            }
        for (QDockWidget* dock : a_app->findChildren<QDockWidget*>())
            if (same(dock->windowTitle(), QStringLiteral("Octave Dock")) && (w == dock || dock->isAncestorOf(w))) {
                *error = tr("The Octave dock is a console: what is typed there runs. The console tool types into it, asked each time.");
                return true;
            }
        return false;
    };
    if (a.compare(QLatin1String("statusbar"), Qt::CaseInsensitive) == 0) {
        *name = QStringLiteral("statusbar");
        return a_app->statusBar();
    }
    if (a.startsWith(QLatin1String("toolbar:"), Qt::CaseInsensitive)) {
        const QString title = a.mid(8);
        for (QToolBar* bar : a_app->findChildren<QToolBar*>())
            if (bar->window() == a_app && same(bar->windowTitle(), title)) {
                *name = QStringLiteral("toolbar:") + plain(bar->windowTitle());
                return bar;
            }
        *error = tr("There is no toolbar %1 (get_ui without 'area' lists the parts of the window).").arg(title);
        return nullptr;
    }
    if (a.startsWith(QLatin1String("dock:"), Qt::CaseInsensitive)) {
        const QString wanted = a.mid(5);
        const QString dockTitle = wanted.section(QLatin1Char('/'), 0, 0), panel = wanted.section(QLatin1Char('/'), 1);
        const QList<QDockWidget*> docks = a_app->findChildren<QDockWidget*>();
        // A dock by its title; a panel of one by "dock/panel".
        for (QDockWidget* dock : docks) {
            if (!same(dock->windowTitle(), dockTitle)) continue;
            if (refuse(dock)) return nullptr;
            if (panel.isEmpty()) {
                *name = QStringLiteral("dock:") + plain(dock->windowTitle());
                return dock->widget();
            }
            if (QTabWidget* tabs = panelsOf(dock))
                for (int i = 0; i < tabs->count(); ++i)
                    if (same(tabs->tabText(i), panel)) {
                        *name = QStringLiteral("dock:") + plain(dock->windowTitle()) + QLatin1Char('/') + plain(tabs->tabText(i));
                        return tabs->widget(i);
                    }
        }
        // A panel by its name alone: Problems, Content, Projects.
        if (panel.isEmpty())
            for (QDockWidget* dock : docks)
                if (QTabWidget* tabs = panelsOf(dock); tabs != nullptr && dock != claude)
                    for (int i = 0; i < tabs->count(); ++i)
                        if (same(tabs->tabText(i), dockTitle)) {
                            if (refuse(tabs->widget(i))) return nullptr;
                            *name = QStringLiteral("dock:") + plain(tabs->tabText(i));
                            return tabs->widget(i);
                        }
        if (claude != nullptr && same(claude->windowTitle(), dockTitle)) {
            refuse(claude);
            return nullptr;
        }
        *error = tr("There is no dock or panel %1 (get_ui without 'area' lists them).").arg(wanted);
        return nullptr;
    }
    *error = tr("'area' is dock:<title> (or a panel of one, dock:Content), toolbar:<title>, statusbar or tabs; get_ui without it "
                "lists them.");
    return nullptr;
}

namespace {

// Shown, as the user would show it: its dock, and its panel in front.
void bringForward(QWidget* root)
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

} // namespace

QJsonObject QucsControl::getUi(const QJsonObject& args)
{
    const QString area = args.value(QLatin1String("area")).toString().trimmed();
    if (area.isEmpty())
        return jsonResult(QJsonObject{{QStringLiteral("areas"), uiAreas()},
                                      {QStringLiteral("note"), tr("get_ui with 'area' reads one; the Claude Code panel is the user's and "
                                                                  "is not among them.")}});
    if (area.compare(QLatin1String("tabs"), Qt::CaseInsensitive) == 0) {
        // Each pane's tabs: the documents, the one in front, which have
        // unsaved changes.
        QJsonArray controls;
        const QList<ContextMenuTabWidget*> panes = a_app->panes();
        for (int p = 0; p < panes.size(); ++p) {
            QJsonArray items, unsaved;
            for (int i = 0; i < panes.at(p)->count(); ++i) {
                items.append(panes.at(p)->tabText(i));
                if (QucsDoc* doc = QucsApp::docIn(panes.at(p)->widget(i)); doc != nullptr && doc->getDocChanged())
                    unsaved.append(panes.at(p)->tabText(i));
            }
            QJsonObject o{{QStringLiteral("id"), QStringLiteral("c%1").arg(p + 1)},
                          {QStringLiteral("label"), panes.size() == 1 ? tr("documents") : tr("pane %1").arg(p + 1)},
                          {QStringLiteral("kind"), QStringLiteral("tabs")},
                          {QStringLiteral("value"), panes.at(p)->tabText(panes.at(p)->currentIndex())},
                          {QStringLiteral("items"), items}};
            if (!unsaved.isEmpty()) o.insert(QStringLiteral("unsaved"), unsaved);
            if (panes.at(p) == a_app->DocumentTab) o.insert(QStringLiteral("active"), true);
            controls.append(o);
        }
        return jsonResult(QJsonObject{{QStringLiteral("area"), QStringLiteral("tabs")}, {QStringLiteral("controls"), controls}});
    }
    QString name, error;
    QWidget* root = uiArea(area, &name, &error, false);
    if (root == nullptr) return errorResult(error);
    QJsonObject o = describeControls(root, true);
    o.insert(QStringLiteral("area"), name);
    bool hidden = false;
    for (QWidget* w = root; w != nullptr; w = w->parentWidget())
        if (auto* dock = qobject_cast<QDockWidget*>(w); dock != nullptr && dock->isHidden()) hidden = true;
    if (hidden) o.insert(QStringLiteral("shown"), false);
    return jsonResult(o);
}

void QucsControl::setUi(const QJsonObject& args, const Done& done)
{
    const QString area = args.value(QLatin1String("area")).toString().trimmed();
    if (area.isEmpty()) {
        done(errorResult(tr("'area' says which part of the window (get_ui without it lists them).")));
        return;
    }
    if (area.compare(QLatin1String("tabs"), Qt::CaseInsensitive) == 0) {
        // A document's tab brought to the front: 'value' its title.
        const QList<ContextMenuTabWidget*> panes = a_app->panes();
        QStringList changed, problems;
        for (const QJsonValue& v : args.value(QLatin1String("set")).toArray()) {
            const QJsonObject change = v.toObject();
            const QString id = change.value(QLatin1String("control")).toString().trimmed();
            const QString title = change.value(QLatin1String("value")).toString().trimmed();
            int pane = -1;
            static const QRegularExpression byId(QStringLiteral("^c(\\d+)$"));
            if (const auto m = byId.match(id); m.hasMatch()) pane = m.captured(1).toInt() - 1;
            bool found = false;
            for (int p = 0; p < panes.size() && !found; ++p) {
                if (pane >= 0 && p != pane) continue;
                for (int i = 0; i < panes.at(p)->count() && !found; ++i)
                    if (panes.at(p)->tabText(i).compare(title, Qt::CaseInsensitive) == 0) {
                        a_app->showDocument(panes.at(p)->widget(i));
                        changed << title;
                        found = true;
                    }
            }
            if (!found) problems << tr("no tab %1").arg(title);
        }
        QString report = changed.isEmpty() ? tr("Nothing changed.") : tr("In front: %1.").arg(changed.join(QStringLiteral(", ")));
        if (!problems.isEmpty()) report += QLatin1Char(' ') + tr("Not done: %1.").arg(problems.join(QStringLiteral("; ")));
        done(textResult(report, changed.isEmpty() && !problems.isEmpty()));
        return;
    }
    QString name, error;
    QWidget* root = uiArea(area, &name, &error, true);
    if (root == nullptr) {
        done(errorResult(error));
        return;
    }
    bringForward(root);
    fillControls(root, args, done, true);
}

// ----------------------------------------------------------------------
// Right-click menus

void QucsControl::contextMenu(const QJsonObject& args, const Done& done)
{
    const QJsonObject on = args.value(QLatin1String("on")).toObject();
    const QString choose = args.value(QLatin1String("choose")).toString().trimmed();
    QString what, error;
    // What opens the menu as a right-click there would. (A panel's needs
    // its dock shown, its row on screen.) Tried again until the row is
    // there - a panel reads its folder aside -, and why it is not, when it
    // never comes.
    std::function<bool()> open;
    auto missing = std::make_shared<QString>();
    if (on.contains(QLatin1String("part")) || on.contains(QLatin1String("diagram")) || on.contains(QLatin1String("canvas"))) {
        Schematic* sch = schematic(args, &error, false);
        if (sch == nullptr) {
            done(errorResult(error));
            return;
        }
        QPoint point;
        if (!canvasPoint(sch, on, &point, &what, &error)) {
            done(errorResult(error));
            return;
        }
        a_app->showDocument(QucsApp::documentWidget(sch));
        // In select mode: an insert mode's right-click ends the mode instead.
        if (a_app->select != nullptr && !a_app->select->isChecked()) a_app->select->trigger();
        sch->centerOn(point);
        QPointer<Schematic> target(sch);
        open = [target, point] {
            if (!target) return false;
            const QPoint at = target->modelToViewport(point);
            QWidget* viewport = target->viewport();
            const QPointF local(at), global(viewport->mapToGlobal(at));
            QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &release);
            return true;
        };
    } else if (on.contains(QLatin1String("project_item")) || on.contains(QLatin1String("project"))) {
        const bool content = on.contains(QLatin1String("project_item"));
        QAbstractItemView* view = content ? static_cast<QAbstractItemView*>(a_app->projectView())
                                          : static_cast<QAbstractItemView*>(a_app->projectsView());
        const QString item = on.value(content ? QLatin1String("project_item") : QLatin1String("project")).toString().trimmed();
        if (view == nullptr) {
            done(errorResult(tr("There is no such panel.")));
            return;
        }
        bringForward(view);
        what = content ? tr("%1 in the Content panel").arg(item) : tr("%1 in the Projects panel").arg(item);
        const QString notThere = content ? tr("%1 is not in the Content panel (a file of the open project, as the panel names it: "
                                              "amp.sch, models/bjt.va).").arg(item)
                                         : tr("%1 is not in the Projects panel.").arg(item);
        QPointer<QAbstractItemView> target(view);
        open = [target, item, content, notThere, missing] {
            if (!target) return false;
            const QModelIndex index = rowNamed(target, item, content ? int(ProjectView::FilePathRole) : -1);
            if (!index.isValid()) {
                *missing = notThere;
                return false;
            }
            missing->clear();
            if (auto* tree = qobject_cast<QTreeView*>(target.data()))
                for (QModelIndex up = index.parent(); up.isValid(); up = up.parent()) tree->expand(up);
            target->scrollTo(index);
            target->setCurrentIndex(index);
            emit target->customContextMenuRequested(target->visualRect(index).center());
            return true;
        };
    } else if (on.contains(QLatin1String("tab"))) {
        const QString title = on.value(QLatin1String("tab")).toString().trimmed();
        for (ContextMenuTabWidget* pane : a_app->panes())
            for (int i = 0; i < pane->count() && !open; ++i)
                if (pane->tabText(i).compare(title, Qt::CaseInsensitive) == 0) {
                    QPointer<ContextMenuTabWidget> target(pane);
                    const QPoint at = pane->tabBar()->tabRect(i).center();
                    open = [target, at] {
                        if (!target) return false;
                        target->showContextMenu(at);
                        return true;
                    };
                }
        if (!open) {
            done(errorResult(tr("There is no tab %1 (get_ui with 'area': \"tabs\" lists them).").arg(title)));
            return;
        }
        what = tr("the tab %1").arg(title);
    } else if (on.contains(QLatin1String("file"))) {
        FileBrowser* browser = a_app->fileBrowserPanel();
        const QString file = absolute(on.value(QLatin1String("file")).toString().trimmed());
        if (browser == nullptr || !QFileInfo::exists(file)) {
            done(errorResult(tr("There is no %1.").arg(QDir::toNativeSeparators(file))));
            return;
        }
        bringForward(browser);
        browser->setLocation(QFileInfo(file).absolutePath());
        what = tr("%1 in the File Browser").arg(QFileInfo(file).fileName());
        QPointer<FileBrowser> target(browser);
        open = [target, file] {
            if (!target) return false;
            target->selectPath(file);
            QAbstractItemView* view = target->currentView();
            const QModelIndex index = view != nullptr ? view->currentIndex() : QModelIndex();
            if (!index.isValid()) return false;
            emit view->customContextMenuRequested(view->visualRect(index).center());
            return true;
        };
    } else {
        done(errorResult(tr("'on' is {\"part\": name}, {\"diagram\": n}, {\"canvas\": [x, y]}, {\"project_item\": file}, "
                            "{\"project\": name}, {\"tab\": title} or {\"file\": path}.")));
        return;
    }

    // Opened from the event loop; looked at, and chosen from, while it is
    // up (a menu's own loop runs until it closes). What the choice opens -
    // a dialog - is told as trigger_action tells it.
    struct Run {
        bool answered = false;
        bool chosen = false;
        int tries = 0;
        QElapsedTimer clock;
        QString path;
    };
    auto run = std::make_shared<Run>();
    run->clock.start();
    auto* watch = new QTimer(this);
    watch->setInterval(10);
    const auto answer = [run, watch, done](const QJsonObject& result) {
        if (run->answered) return;
        run->answered = true;
        watch->stop();
        watch->deleteLater();
        done(result);
    };
    connect(watch, &QTimer::timeout, this, [this, run, answer, choose, what, open, missing] {
        if (run->answered) return;
        if (!run->chosen) {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                // (A file the browser lists once it has read its folder.)
                if (run->clock.elapsed() > 300 && run->tries < 20 && run->clock.elapsed() < 3000 && run->tries * 150 < run->clock.elapsed()) {
                    ++run->tries;
                    // From a timer of its own: a menu that waits runs its
                    // loop there, and this one goes on looking meanwhile
                    // (a timer does not fire within its own slot).
                    QTimer::singleShot(0, a_app, [open] {
                        const QtFileDialogs qt;
                        open();
                    });
                }
                if (run->clock.elapsed() > 3000)
                    answer(errorResult(missing->isEmpty() ? tr("No menu came up on %1.").arg(what) : *missing));
                return;
            }
            QJsonArray entries;
            QList<QAction*> actions;
            QStringList paths;
            menuEntries(menu, QString(), &entries, &actions, &paths);
            if (choose.isEmpty()) {
                closeMenus();
                answer(jsonResult(QJsonObject{{QStringLiteral("on"), what}, {QStringLiteral("menu"), entries}}));
                return;
            }
            int found = -1;
            for (int i = 0; i < paths.size() && found < 0; ++i)
                if (same(paths.at(i), choose)) found = i;
            for (int i = 0; i < paths.size() && found < 0; ++i)
                if (same(paths.at(i).section(QStringLiteral(" > "), -1), choose)) found = i;
            if (found < 0 || !actions.at(found)->isEnabled()) {
                closeMenus();
                answer(errorResult((found < 0 ? tr("The menu on %1 has no %2; it has: %3.") : tr("%2 cannot be chosen now on %1; the menu has: %3."))
                                       .arg(what, choose, paths.join(QStringLiteral("; ")))));
                return;
            }
            run->chosen = true;
            run->path = paths.at(found);
            run->clock.restart();
            // As the keyboard would: each step of its path, then Return (so
            // that a menu waited on returns what was chosen).
            const QStringList steps = run->path.split(QStringLiteral(" > "));
            QPointer<QMenu> top(menu);
            QPointer<QAction> leaf(actions.at(found));
            QTimer::singleShot(0, a_app, [top, steps, leaf] {
                const QtFileDialogs qt;
                QMenu* at = top;
                for (int i = 0; at != nullptr && i < steps.size(); ++i) {
                    QAction* step = nullptr;
                    for (QAction* a : at->actions())
                        if (!a->isSeparator() && same(a->text(), steps.at(i))) step = a;
                    if (step == nullptr) break;
                    at->setActiveAction(step);
                    if (i + 1 < steps.size()) {
                        key(at, Qt::Key_Right);
                        at = step->menu();
                        if (at != nullptr && !at->isVisible()) at->popup(QCursor::pos());
                    } else {
                        key(at, Qt::Key_Return);
                    }
                }
                // (A menu that took no keys: the action, and the menus closed.)
                if (QApplication::activePopupWidget() != nullptr && leaf) {
                    closeMenus();
                    leaf->trigger();
                }
            });
            return;
        }
        // Chosen: what it opened, or done.
        if (QWidget* dialog = openDialog()) {
            answer(textResult(tr("%1 on %2: it opened “%3”, which waits for an answer (get_dialog reads it, set_dialog answers it).")
                                  .arg(run->path, what, dialog->windowTitle())));
            return;
        }
        if (QApplication::activePopupWidget() == nullptr && run->clock.elapsed() > 300)
            answer(textResult(tr("%1 on %2: done.").arg(run->path, what)));
    });
    watch->start();
    QTimer::singleShot(0, a_app, [open, run] {
        // (A menu waited on runs its own loop here, and what is chosen in
        // it after: the file dialogs it opens are Qt's too.)
        const QtFileDialogs qt;
        if (open()) run->tries = 1;
    });
}
