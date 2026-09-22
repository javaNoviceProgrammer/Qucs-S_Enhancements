/***************************************************************************
                               id_dialog.cpp
                              ---------------
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
#include "id_dialog.h"
#include "id_text.h"
#include "schematic.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

// What each field may hold: the file keeps a parameter as
// "1=name=default=description=type" between double quotes, and the
// component dialog reads "[a,b]" in a description as a list of choices.
const QRegularExpression& prefixPattern()
{
  static const QRegularExpression re("^[A-Za-z][A-Za-z0-9_]*$");
  return re;
}
const QRegularExpression& namePattern()
{
  static const QRegularExpression re("^[A-Za-z0-9_]+$");
  return re;
}
const QRegularExpression& typePattern()
{
  static const QRegularExpression re("^[A-Za-z0-9_]*$");
  return re;
}
const QRegularExpression& valuePattern()
{
  static const QRegularExpression re("^[^\"=]*$");
  return re;
}
const QRegularExpression& descriptionPattern()
{
  static const QRegularExpression re("^[^\"=\\[\\]]*$");
  return re;
}

// The editors of the table's cells: each lets through only what its
// field may hold; the type is chosen from a list or typed.
class ParameterDelegate : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                        const QModelIndex& index) const override
  {
    if (index.column() == ID_Dialog::TypeColumn) {
      auto* combo = new QComboBox(parent);
      combo->setEditable(true);
      combo->addItems(ID_Dialog::types());
      combo->setValidator(new QRegularExpressionValidator(typePattern(), combo));
      return combo;
    }
    QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
    if (auto* line = qobject_cast<QLineEdit*>(editor)) {
      const QRegularExpression& re = index.column() == ID_Dialog::NameColumn      ? namePattern()
                                   : index.column() == ID_Dialog::DefaultColumn   ? valuePattern()
                                                                                  : descriptionPattern();
      line->setValidator(new QRegularExpressionValidator(re, line));
    }
    return editor;
  }

  void setEditorData(QWidget* editor, const QModelIndex& index) const override
  {
    if (auto* combo = qobject_cast<QComboBox*>(editor)) {
      combo->setCurrentText(index.data(Qt::EditRole).toString());
      return;
    }
    QStyledItemDelegate::setEditorData(editor, index);
  }

  void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
  {
    if (auto* combo = qobject_cast<QComboBox*>(editor)) {
      model->setData(index, combo->currentText().trimmed(), Qt::EditRole);
      return;
    }
    QStyledItemDelegate::setModelData(editor, model, index);
  }
};

} // namespace

QStringList ID_Dialog::types()
{
  return {QStringLiteral("real"), QStringLiteral("integer"), QStringLiteral("string")};
}

ID_Dialog::ID_Dialog(ID_Text* idText, QWidget* parent)
    : QDialog(parent), m_idText(idText)
{
  setWindowTitle(tr("Edit Subcircuit Properties"));
  auto* all = new QVBoxLayout(this);

  auto* top = new QHBoxLayout;
  top->addWidget(new QLabel(tr("Prefix:"), this));
  m_prefix = new QLineEdit(idText->prefix, this);
  m_prefix->setValidator(new QRegularExpressionValidator(prefixPattern(), m_prefix));
  m_prefix->setToolTip(tr("What the names of this subcircuit's instances start with (SUB1, SUB2, ...)"));
  top->addWidget(m_prefix, 1);
  all->addLayout(top);

  auto* box = new QGroupBox(tr("Parameters"), this);
  auto* boxLayout = new QVBoxLayout(box);
  m_table = new QTableWidget(0, ColumnCount, box);
  m_table->setHorizontalHeaderLabels({tr("Show"), tr("Name"), tr("Default"), tr("Type"), tr("Description")});
  m_table->horizontalHeaderItem(ShowColumn)->setToolTip(tr("Shown beside the instances on the schematic"));
  m_table->horizontalHeaderItem(TypeColumn)->setToolTip(tr("The parameter's type where one is needed "
                                                           "(Verilog-A): real when none is given"));
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->horizontalHeader()->setSectionsClickable(false);
  m_table->verticalHeader()->setVisible(false);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
                           QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
  m_table->setItemDelegate(new ParameterDelegate(m_table));
  m_table->setMinimumSize(520, 180);
  boxLayout->addWidget(m_table, 1);

  for (const auto& p : idText->subParameters)
    appendRow(p->display, p->name.section('=', 0, 0), p->name.section('=', 1, 1), p->type, p->description);
  m_table->resizeColumnsToContents();
  m_table->setColumnWidth(NameColumn, std::max(m_table->columnWidth(NameColumn), 100));
  m_table->setColumnWidth(DefaultColumn, std::max(m_table->columnWidth(DefaultColumn), 100));
  m_table->setColumnWidth(TypeColumn, std::max(m_table->columnWidth(TypeColumn), 80));

  auto* rows = new QHBoxLayout;
  auto* add = new QPushButton(tr("Add"), box);
  m_remove = new QPushButton(tr("Remove"), box);
  m_up = new QPushButton(tr("Move Up"), box);
  m_down = new QPushButton(tr("Move Down"), box);
  for (QPushButton* b : {add, m_remove, m_up, m_down}) rows->addWidget(b);
  rows->addStretch(1);
  boxLayout->addLayout(rows);
  all->addWidget(box, 1);

  m_message = new QLabel(this);
  m_message->setWordWrap(true);
  QPalette red = m_message->palette();
  red.setColor(QPalette::WindowText, QColor(0xd0, 0x30, 0x30));
  m_message->setPalette(red);
  all->addWidget(m_message);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
  all->addWidget(buttons);

  connect(add, &QPushButton::clicked, this, &ID_Dialog::addParameter);
  connect(m_remove, &QPushButton::clicked, this, &ID_Dialog::removeParameters);
  connect(m_up, &QPushButton::clicked, this, &ID_Dialog::moveUp);
  connect(m_down, &QPushButton::clicked, this, &ID_Dialog::moveDown);
  connect(m_table, &QTableWidget::itemSelectionChanged, this, &ID_Dialog::updateButtons);
  connect(m_table, &QTableWidget::itemChanged, this, [this] { m_message->clear(); });
  connect(m_prefix, &QLineEdit::textEdited, this, [this] { m_message->clear(); });
  connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &ID_Dialog::apply);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    if (!apply()) return;   // the problem is on show
    m_applied ? accept() : reject();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  updateButtons();
}

void ID_Dialog::appendRow(bool display, const QString& name, const QString& value, const QString& type,
                          const QString& description)
{
  const int row = m_table->rowCount();
  m_table->insertRow(row);
  auto* show = new QTableWidgetItem;
  show->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
  show->setCheckState(display ? Qt::Checked : Qt::Unchecked);
  m_table->setItem(row, ShowColumn, show);
  m_table->setItem(row, NameColumn, new QTableWidgetItem(name));
  m_table->setItem(row, DefaultColumn, new QTableWidgetItem(value));
  m_table->setItem(row, TypeColumn, new QTableWidgetItem(type));
  m_table->setItem(row, DescriptionColumn, new QTableWidgetItem(description));
}

void ID_Dialog::addParameter()
{
  // A name of its own to begin with, ready to be typed over.
  QSet<QString> taken;
  for (int row = 0; row < m_table->rowCount(); ++row) taken.insert(m_table->item(row, NameColumn)->text());
  int n = 1;
  while (taken.contains(QStringLiteral("P%1").arg(n))) ++n;
  appendRow(true, QStringLiteral("P%1").arg(n), QString(), QString(), QString());
  const int row = m_table->rowCount() - 1;
  m_table->setCurrentCell(row, NameColumn);
  m_table->editItem(m_table->item(row, NameColumn));
}

void ID_Dialog::removeParameters()
{
  QList<int> rows;
  for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) rows << index.row();
  if (rows.isEmpty() && m_table->currentRow() >= 0) rows << m_table->currentRow();
  std::sort(rows.begin(), rows.end(), std::greater<int>());
  for (int row : rows) m_table->removeRow(row);
  if (!rows.isEmpty() && m_table->rowCount() > 0)
    m_table->selectRow(std::min(rows.last(), m_table->rowCount() - 1));
  updateButtons();
}

void ID_Dialog::moveCurrent(int by)
{
  const int row = m_table->currentRow(), other = row + by;
  if (row < 0 || other < 0 || other >= m_table->rowCount()) return;
  for (int column = 0; column < ColumnCount; ++column) {
    QTableWidgetItem* a = m_table->takeItem(row, column);
    QTableWidgetItem* b = m_table->takeItem(other, column);
    m_table->setItem(row, column, b);
    m_table->setItem(other, column, a);
  }
  m_table->selectRow(other);
  m_table->setCurrentCell(other, NameColumn);
  updateButtons();
}

void ID_Dialog::moveUp() { moveCurrent(-1); }
void ID_Dialog::moveDown() { moveCurrent(1); }

void ID_Dialog::updateButtons()
{
  const int row = m_table->currentRow();
  const bool any = !m_table->selectionModel()->selectedRows().isEmpty() || row >= 0;
  m_remove->setEnabled(any && m_table->rowCount() > 0);
  m_up->setEnabled(row > 0);
  m_down->setEnabled(row >= 0 && row < m_table->rowCount() - 1);
}

QString ID_Dialog::problem(int* row, int* column) const
{
  auto at = [&](int r, int c, const QString& text) {
    if (row) *row = r;
    if (column) *column = c;
    return text;
  };
  const QString prefix = m_prefix->text().trimmed();
  if (!prefixPattern().match(prefix).hasMatch())
    return at(-1, -1, tr("The prefix must start with a letter, and hold only letters, digits and _."));

  QSet<QString> names;
  for (int r = 0; r < m_table->rowCount(); ++r) {
    const QString name = m_table->item(r, NameColumn)->text().trimmed();
    if (name.isEmpty()) return at(r, NameColumn, tr("Parameter %1 has no name.").arg(r + 1));
    if (!namePattern().match(name).hasMatch())
      return at(r, NameColumn, tr("\"%1\": a name may hold only letters, digits and _.").arg(name));
    if (name == QLatin1String("File"))
      return at(r, NameColumn, tr("\"File\" is the subcircuit's own property; the parameter needs another name."));
    if (names.contains(name)) return at(r, NameColumn, tr("\"%1\" is there twice.").arg(name));
    names.insert(name);
    if (!valuePattern().match(m_table->item(r, DefaultColumn)->text()).hasMatch())
      return at(r, DefaultColumn, tr("The default of \"%1\" cannot hold \" or =.").arg(name));
    if (!typePattern().match(m_table->item(r, TypeColumn)->text().trimmed()).hasMatch())
      return at(r, TypeColumn, tr("The type of \"%1\" may hold only letters, digits and _.").arg(name));
    if (!descriptionPattern().match(m_table->item(r, DescriptionColumn)->text()).hasMatch())
      return at(r, DescriptionColumn, tr("The description of \"%1\" cannot hold \", =, [ or ].").arg(name));
  }
  return at(-1, -1, QString());
}

bool ID_Dialog::apply()
{
  int row = -1, column = -1;
  const QString wrong = problem(&row, &column);
  if (!wrong.isEmpty()) {
    m_message->setText(wrong);
    if (row < 0) {
      m_prefix->setFocus();
    } else {
      m_table->setCurrentCell(row, column);
      m_table->setFocus();
    }
    return false;
  }
  m_message->clear();

  bool changed = false;
  const QString prefix = m_prefix->text().trimmed();
  if (m_idText->prefix != prefix) {
    m_idText->prefix = prefix;
    changed = true;
  }

  std::vector<std::unique_ptr<SubParameter>> table;
  for (int r = 0; r < m_table->rowCount(); ++r)
    table.push_back(std::make_unique<SubParameter>(
        m_table->item(r, ShowColumn)->checkState() == Qt::Checked,
        m_table->item(r, NameColumn)->text().trimmed() + "=" + m_table->item(r, DefaultColumn)->text(),
        m_table->item(r, DescriptionColumn)->text(),
        m_table->item(r, TypeColumn)->text().trimmed()));

  bool same = table.size() == m_idText->subParameters.size();
  for (std::size_t i = 0; same && i < table.size(); ++i) {
    const SubParameter& a = *table[i];
    const SubParameter& b = *m_idText->subParameters[i];
    same = a.display == b.display && a.name == b.name && a.description == b.description && a.type == b.type;
  }
  if (!same) {
    m_idText->subParameters.swap(table);
    changed = true;
  }

  if (changed) {
    m_applied = true;
    if (auto* doc = qobject_cast<Schematic*>(parentWidget())) doc->viewport()->update();
  }
  return true;
}
