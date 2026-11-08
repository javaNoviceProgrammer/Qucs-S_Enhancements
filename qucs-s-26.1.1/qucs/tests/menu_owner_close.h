/*
 * A document closed while one of its menus is up (docs/bug_hunts/
 * 2026-11-07-layout-close-crash.md): the menu goes with it and nothing is
 * freed twice.
 *
 * The menus were QMenus on the stack of the slot that asked for them, or
 * of the heap and freed after it, shown with exec(): the document closed in
 * exec()'s loop - its tab's close button clicked through the menu, a tool's
 * close_document - freed its child as if allocated ("pointer being freed
 * was not allocated", the abort of 27.0.7 in the layout viewer), or the
 * slot freed again what the document had freed.
 */
#ifndef QUCS_TESTS_MENU_OWNER_CLOSE_H
#define QUCS_TESTS_MENU_OWNER_CLOSE_H

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QTimer>

#include <functional>

namespace menu_owner_close {

/// The close button of \a page's tab in \a tabs clicked, as a user does
/// (its close asked of the tab bar where it has none).
inline void clickClose(QTabWidget* tabs, QWidget* page)
{
    QTabBar* bar = tabs->tabBar();
    const int index = tabs->indexOf(page);
    if (index < 0) return;
    auto* close = qobject_cast<QAbstractButton*>(bar->tabButton(index, QTabBar::RightSide));
    if (close == nullptr) close = qobject_cast<QAbstractButton*>(bar->tabButton(index, QTabBar::LeftSide));
    if (close != nullptr) close->click();
    else emit bar->tabCloseRequested(index);
}

/// \a ask asks for a menu of \a owner; once it is up, \a close closes
/// \a owner. Empty if \a owner and the menu went and no menu is left up;
/// else what went wrong.
inline QString closeWithMenuUp(QObject* owner, const std::function<void()>& ask, const std::function<void()>& close,
                               int timeoutMs = 30000)
{
    QPointer<QObject> watched(owner);
    QPointer<QMenu> menu;
    bool seen = false;
    QTimer look;
    look.setInterval(5);
    QObject::connect(&look, &QTimer::timeout, &look, [&] {
        auto* up = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (up == nullptr) return;
        look.stop();
        seen = true;
        menu = up;
        close();
    });
    look.start();
    ask();
    QElapsedTimer clock;
    clock.start();
    while ((!seen || !watched.isNull()) && clock.elapsed() < timeoutMs) QTest::qWait(10);
    look.stop();
    if (!seen) return QStringLiteral("no menu came up");
    if (!watched.isNull()) return QStringLiteral("the menu came up, but its owner was not closed");
    if (!menu.isNull()) return QStringLiteral("the owner closed, its menu left");
    if (QApplication::activePopupWidget() != nullptr) return QStringLiteral("a menu is still up");
    return {};
}

/// The menu \a ask asks for, once it is up (null: none came).
inline QMenu* menuUp(const std::function<void()>& ask, int timeoutMs = 5000)
{
    ask();
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (auto* up = qobject_cast<QMenu*>(QApplication::activePopupWidget())) return up;
        QTest::qWait(10);
    }
    return nullptr;
}

} // namespace menu_owner_close

#endif
