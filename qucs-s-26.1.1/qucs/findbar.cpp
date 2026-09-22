/*
 * findbar.cpp - the find bar under a pane's schematics: a component by its
 * name, a net by its label, or a property value
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "findbar.h"

#include "schematic.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>

#include <algorithm>

using qucs_s::search::Match;

FindBar::FindBar(QTabWidget* tabs, QWidget* parent)
    : QWidget(parent), m_tabs(tabs)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(4);

    layout->addWidget(new QLabel(tr("Find:"), this));
    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(tr("Component name, net label or value"));
    m_edit->setClearButtonEnabled(true);
    m_edit->setToolTip(tr("Looks for component names first, then net labels, then property values.\n"
                          "Enter: next match, Shift+Enter: previous one, Escape: close."));
    m_edit->installEventFilter(this);
    layout->addWidget(m_edit, 1);

    m_count = new QLabel(this);
    m_count->setMinimumWidth(m_count->fontMetrics().horizontalAdvance(tr("No matches")) + 8);
    layout->addWidget(m_count);

    m_previous = new QToolButton(this);
    m_previous->setArrowType(Qt::UpArrow);
    m_previous->setToolTip(tr("Previous match (Shift+Enter)"));
    m_previous->setAutoRaise(true);
    layout->addWidget(m_previous);
    m_next = new QToolButton(this);
    m_next->setArrowType(Qt::DownArrow);
    m_next->setToolTip(tr("Next match (Enter)"));
    m_next->setAutoRaise(true);
    layout->addWidget(m_next);
    auto* close = new QToolButton(this);
    close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    close->setToolTip(tr("Close (Escape)"));
    close->setAutoRaise(true);
    layout->addWidget(close);

    connect(m_edit, &QLineEdit::textChanged, this, [this] {
        m_current = -1;
        search();
        if (!m_matches.isEmpty()) m_current = 0;
        showCurrent();
    });
    connect(m_previous, &QToolButton::clicked, this, &FindBar::previous);
    connect(m_next, &QToolButton::clicked, this, &FindBar::next);
    connect(close, &QToolButton::clicked, this, &FindBar::dismiss);
    // Another document in front of the pane: its matches, none selected yet.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (isHidden()) return;   // put away (not merely in a window not shown yet)
        m_current = -1;
        search();
        updateState();
    });

    hide();
    updateState();
}

Schematic* FindBar::schematic() const
{
    return qobject_cast<Schematic*>(m_tabs->currentWidget());
}

void FindBar::open()
{
    show();
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
    search();
    updateState();
}

void FindBar::search()
{
    const Match was = m_current >= 0 && m_current < m_matches.size() ? m_matches.at(m_current) : Match();
    const bool had = m_current >= 0 && m_current < m_matches.size();
    m_matches.clear();
    m_current = -1;

    qucs_s::search::Query query;
    query.text = m_edit->text().trimmed();
    query.names = true;
    query.values = true;
    Schematic* doc = schematic();
    if (doc != nullptr && !query.text.isEmpty()) {
        // One stop per component: a name that matches and a value of the
        // same component that matches too are one place to look at.
        for (const Match& m : qucs_s::search::find(doc, query)) {
            const bool seen = m.kind != Match::Label
                && std::any_of(m_matches.cbegin(), m_matches.cend(), [&m](const Match& o) {
                       return o.kind != Match::Label && o.component == m.component && o.where == m.where;
                   });
            if (!seen) m_matches << m;
        }
    }
    if (had) m_current = m_matches.indexOf(was);
}

void FindBar::step(int by)
{
    search();   // the schematic may have changed since
    if (m_matches.isEmpty()) {
        updateState();
        return;
    }
    const int n = m_matches.size();
    m_current = m_current < 0 ? (by > 0 ? 0 : n - 1) : ((m_current + by) % n + n) % n;
    showCurrent();
}

void FindBar::next() { step(1); }
void FindBar::previous() { step(-1); }

void FindBar::showCurrent()
{
    Schematic* doc = schematic();
    if (doc != nullptr && m_current >= 0 && m_current < m_matches.size())
        qucs_s::search::reveal(doc, m_matches.at(m_current));
    updateState();
}

void FindBar::dismiss()
{
    hide();
    if (QWidget* doc = m_tabs->currentWidget()) doc->setFocus(Qt::OtherFocusReason);
}

void FindBar::updateState()
{
    Schematic* doc = schematic();
    const bool usable = doc != nullptr && !doc->getSymbolMode();
    m_edit->setEnabled(usable);
    if (!usable) m_count->setText(doc == nullptr ? tr("Not a schematic") : tr("Symbol editing"));
    else if (m_edit->text().trimmed().isEmpty()) m_count->clear();
    else if (m_matches.isEmpty()) m_count->setText(tr("No matches"));
    else if (m_current < 0)
        m_count->setText(m_matches.size() == 1 ? tr("1 match") : tr("%1 matches").arg(m_matches.size()));
    else m_count->setText(tr("%1 of %2").arg(m_current + 1).arg(m_matches.size()));
    m_previous->setEnabled(usable && !m_matches.isEmpty());
    m_next->setEnabled(usable && !m_matches.isEmpty());
}

bool FindBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            dismiss();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (key->modifiers() & Qt::ShiftModifier) previous();
            else next();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
