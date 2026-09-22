/*
 * findbar.h - the find bar under a pane's schematics: a component by its
 * name, a net by its label, or a property value
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FINDBAR_H
#define QUCS_FINDBAR_H

#include "componentsearch.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QTabWidget;
class QToolButton;
class Schematic;

/*!
 * \brief A bar of the kind a browser has: what is typed is looked for in
 *        the schematic in front of the pane (component names first, then
 *        net labels, then property values), the first match is selected
 *        and centred, Enter and Shift+Enter step through the rest, and
 *        Escape puts the bar away. It follows the pane to another
 *        schematic and searches that one.
 */
class FindBar : public QWidget
{
    Q_OBJECT
public:
    explicit FindBar(QTabWidget* tabs, QWidget* parent = nullptr);

    /// Shows the bar with its text selected and the focus in it, and
    /// searches the schematic in front again.
    void open();

    QLineEdit* edit() const { return m_edit; }
    QLabel* countLabel() const { return m_count; }
    const QList<qucs_s::search::Match>& matches() const { return m_matches; }
    int current() const { return m_current; }   ///< -1: none

public slots:
    void next();
    void previous();
    void dismiss();   ///< hides the bar and gives the focus back to the schematic

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Schematic* schematic() const;
    void search();          // the matches of the text, the current one kept when still there
    void step(int by);
    void showCurrent();
    void updateState();

    QTabWidget* m_tabs;
    QLineEdit* m_edit;
    QLabel* m_count;
    QToolButton* m_previous;
    QToolButton* m_next;
    QList<qucs_s::search::Match> m_matches;
    int m_current = -1;
};

#endif
