/***************************************************************************
                               id_dialog.h
                              -------------
    begin                : Sat Oct 16 2004
    copyright            : (C) 2004 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

/*! \file id_dialog.h
  * \brief Dialog to edit parameters in a subcircuit symbol.
  */

#ifndef ID_DIALOG_H
#define ID_DIALOG_H

#include <QDialog>

class ID_Text;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

/*!
 * \brief The properties of a subcircuit: the prefix of its instances'
 *        names and its parameters (upstream #1285).
 *
 * One table, edited in its cells as the component dialog is: whether the
 * parameter is shown on the instances, its name, default value, type and
 * description. Rows are added, removed and moved (their order is that of
 * the symbol and the netlist). OK and Apply check the table - a name
 * given, not twice, not "File", nothing the file format cannot hold -
 * and write it into the symbol; Apply keeps the dialog open.
 */
class ID_Dialog : public QDialog
{
  Q_OBJECT
public:
  enum Column { ShowColumn, NameColumn, DefaultColumn, TypeColumn, DescriptionColumn, ColumnCount };

  explicit ID_Dialog(ID_Text* idText, QWidget* parent = nullptr);

  /// Whether Apply or OK wrote something into the symbol.
  bool applied() const { return m_applied; }

  /// What is wrong with the table, or an empty string; \a row and
  /// \a column get the cell (-1 when it is the prefix).
  QString problem(int* row = nullptr, int* column = nullptr) const;

  QLineEdit* prefixEdit() const { return m_prefix; }
  QTableWidget* table() const { return m_table; }
  QLabel* messageLabel() const { return m_message; }

  /// The value types offered in the Type column.
  static QStringList types();

public slots:
  void addParameter();
  void removeParameters();
  void moveUp();
  void moveDown();
  bool apply();   ///< false (and the problem shown) when the table is not right

private:
  void appendRow(bool display, const QString& name, const QString& value, const QString& type,
                 const QString& description);
  void moveCurrent(int by);
  void updateButtons();

  ID_Text* m_idText;
  QLineEdit* m_prefix;
  QTableWidget* m_table;
  QLabel* m_message;
  QPushButton* m_remove;
  QPushButton* m_up;
  QPushButton* m_down;
  bool m_applied = false;
};

#endif
