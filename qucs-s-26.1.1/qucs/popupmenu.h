/*
 * A context menu shown and left to itself.
 *
 * QMenu::exec() runs an event loop inside the slot that asked for the menu,
 * and in that loop the menu's owner can go: a document's tab closed by its
 * close button (a click outside a menu goes on to what it lands on, on
 * macOS), or by a tool of Claude's (popups are not modal: the calls wait
 * for no menu). The tab then frees its children under the slot - a QMenu on
 * the slot's stack freed as if allocated ("pointer being freed was not
 * allocated", the crash of 7 November in the layout viewer's layers), a
 * menu of the heap freed again once exec() returns - and the slot goes on
 * with a `this` that is gone.
 *
 * popUp() shows \a menu without a loop of its own and frees it once it
 * closes: a child like any other, freed with its owner if that goes first.
 * \a menu is allocated with new, its parent what it belongs to; what an
 * entry does is in its slot, not in what exec() would have returned.
 */
#ifndef QUCS_POPUPMENU_H
#define QUCS_POPUPMENU_H

#include <QMenu>
#include <QPoint>

namespace qucs_s {

inline void popUp(QMenu* menu, const QPoint& globalPos)
{
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(globalPos);
}

} // namespace qucs_s

#endif
