/*
 * pinorderdialog.h - which pin of a subcircuit comes first
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PINORDERDIALOG_H
#define PINORDERDIALOG_H

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class Component;
class PortSymbol;
class Schematic;
class QListWidget;
class QPushButton;

/*!
 * \brief The order of a subcircuit's pins.
 *
 * The number of a port decides where it sits on the .SUBCKT line and
 * therefore which pin of an instance it is. This shows them in that
 * order and lets them be moved, writing the new numbers back into the
 * schematic's port components and into the symbol.
 */
class PinOrderDialog : public QDialog {
    Q_OBJECT
public:
    explicit PinOrderDialog(Schematic* document, QWidget* parent = nullptr);

    //! Whether the numbers were actually changed when the dialog closed.
    bool changedAnything() const { return m_changed; }

    //! How many pins the subcircuit has.
    int pinCount() const { return int(m_pins.size()); }
    //! Their names, in the order they are shown, which is their order.
    QStringList pinNames() const;
    //! Moves the pin in \a row up (-1) or down (1).
    void movePin(int row, int by);
    //! Writes the order shown into the ports and the symbol.
    void applyOrder();

private slots:
    void moveUp();
    void moveDown();
    void apply();

private:
    struct Pin {
        Component* port = nullptr;     //!< the port component on the schematic
        PortSymbol* symbol = nullptr;  //!< the pin on the symbol, if drawn
        QString name;
        QString direction;
        int number = 0;
    };

    void fill();
    QString describe(const Pin& pin, int position) const;

    Schematic* m_document;
    QList<Pin> m_pins;
    QListWidget* m_list;
    QPushButton* m_up;
    QPushButton* m_down;
    bool m_changed = false;
};

#endif // PINORDERDIALOG_H
