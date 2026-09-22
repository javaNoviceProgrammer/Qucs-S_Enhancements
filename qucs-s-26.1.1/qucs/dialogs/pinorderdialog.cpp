/*
 * pinorderdialog.cpp - which pin of a subcircuit comes first
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pinorderdialog.h"

#include "components/component.h"
#include "paintings/portsymbol.h"
#include "schematic.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

PinOrderDialog::PinOrderDialog(Schematic* document, QWidget* parent)
    : QDialog(parent), m_document(document)
{
    setWindowTitle(tr("Pin Order"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("The first pin here is the first argument of the subcircuit, and the\n"
           "first pin of every instance of it. Moving a pin rewires the instances."),
        this));

    auto* middle = new QHBoxLayout;
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    middle->addWidget(m_list);

    auto* buttons = new QVBoxLayout;
    m_up = new QPushButton(tr("Move &Up"), this);
    m_down = new QPushButton(tr("Move &Down"), this);
    buttons->addWidget(m_up);
    buttons->addWidget(m_down);
    buttons->addStretch();
    middle->addLayout(buttons);
    layout->addLayout(middle);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(box);

    connect(m_up, &QPushButton::clicked, this, &PinOrderDialog::moveUp);
    connect(m_down, &QPushButton::clicked, this, &PinOrderDialog::moveDown);
    connect(box, &QDialogButtonBox::accepted, this, &PinOrderDialog::apply);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    fill();
}

// The ports of the schematic, in the order their numbers put them, each
// with the pin the symbol draws for it.
void PinOrderDialog::fill()
{
    m_pins.clear();
    if (m_document == nullptr) return;

    for (Component* c : m_document->a_DocComps) {
        if (c->Model != QLatin1String("Port")) continue;
        if (c->Props.isEmpty()) continue;

        Pin pin;
        pin.port = c;
        pin.number = c->Props.first()->Value.toInt();
        pin.direction = c->Props.count() > 1 ? c->Props.at(1)->Value : QString();
        pin.name = m_document->portPinName(c);

        for (Painting* painting : m_document->a_SymbolPaints)
            if (painting->Name == QLatin1String(".PortSym ")
                && static_cast<PortSymbol*>(painting)->numberStr == c->Props.first()->Value) {
                pin.symbol = static_cast<PortSymbol*>(painting);
                break;
            }

        m_pins.append(pin);
    }

    std::sort(m_pins.begin(), m_pins.end(),
              [](const Pin& a, const Pin& b) { return a.number < b.number; });

    m_list->clear();
    for (int i = 0; i < m_pins.size(); ++i) m_list->addItem(describe(m_pins.at(i), i + 1));
    if (!m_pins.isEmpty()) m_list->setCurrentRow(0);

    const bool movable = m_pins.size() > 1;
    m_up->setEnabled(movable);
    m_down->setEnabled(movable);
}

QString PinOrderDialog::describe(const Pin& pin, int position) const
{
    QString text = QStringLiteral("%1.  %2").arg(position).arg(pin.name);
    const QString dir = pin.direction.toLower();
    if (dir == QLatin1String("in") || dir == QLatin1String("out") || dir == QLatin1String("inout"))
        text += QStringLiteral("  (%1)").arg(dir);
    if (pin.port != nullptr && pin.port->Name != pin.name)
        text += QStringLiteral("  [%1]").arg(pin.port->Name);
    return text;
}

void PinOrderDialog::moveUp() { movePin(m_list->currentRow(), -1); }
void PinOrderDialog::moveDown() { movePin(m_list->currentRow(), 1); }

void PinOrderDialog::movePin(int row, int by)
{
    if (row < 0 || row >= m_pins.size()) return;

    const int to = row + by;
    if (to < 0 || to >= m_pins.size()) return;

    m_pins.swapItemsAt(row, to);
    for (int i = 0; i < m_pins.size(); ++i) m_list->item(i)->setText(describe(m_pins.at(i), i + 1));
    m_list->setCurrentRow(to);
}

QStringList PinOrderDialog::pinNames() const
{
    QStringList names;
    for (const Pin& pin : m_pins) names << pin.name;
    return names;
}

// The order on screen becomes the numbers, on the port components and on
// the pins of the symbol alike.
void PinOrderDialog::apply()
{
    applyOrder();
    accept();
}

void PinOrderDialog::applyOrder()
{
    m_changed = false;
    for (int i = 0; i < m_pins.size(); ++i) {
        const QString wanted = QString::number(i + 1);
        const Pin& pin = m_pins.at(i);
        if (pin.port == nullptr || pin.port->Props.isEmpty()) continue;

        if (pin.port->Props.first()->Value != wanted) {
            pin.port->Props.first()->Value = wanted;
            m_changed = true;
        }
        if (pin.symbol != nullptr && pin.symbol->numberStr != wanted) {
            pin.symbol->numberStr = wanted;
            m_changed = true;
        }
    }
}
