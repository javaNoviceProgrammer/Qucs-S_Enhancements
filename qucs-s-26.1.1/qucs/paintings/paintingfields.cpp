/*
 * paintingfields.cpp - a painting's properties as fields, and the dialog
 *                      made of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "paintingfields.h"

#include "ink.h"
#include "main.h"
#include "misc.h"
#include "painting.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

namespace {

QString tr(const char* text)
{
  return QCoreApplication::translate("PaintingDialog", text);
}

// The painting as it would be, drawn to fit.
class Preview : public QWidget {
public:
  explicit Preview(Painting* painting, QWidget* parent) : QWidget(parent), a_painting(painting)
  {
    setObjectName(QStringLiteral("paintingPreview"));
    setMinimumSize(240, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(rect(), qucs_s::ink::paper());
    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    const QRectF shape = QRectF(a_painting->boundingRect()).adjusted(-4, -4, 4, 4);
    if (shape.width() <= 0 || shape.height() <= 0) return;
    const QRectF room = QRectF(rect()).adjusted(12, 12, -12, -12);
    const qreal scale = std::min({room.width() / shape.width(), room.height() / shape.height(), 3.0});
    p.translate(room.center());
    p.scale(scale, scale);
    p.translate(-shape.center());
    a_painting->paint(&p);
  }

private:
  Painting* a_painting;
};

const char* const kPenStyles[] = {QT_TRANSLATE_NOOP("PaintingDialog", "solid line"),
                                  QT_TRANSLATE_NOOP("PaintingDialog", "dash line"),
                                  QT_TRANSLATE_NOOP("PaintingDialog", "dot line"),
                                  QT_TRANSLATE_NOOP("PaintingDialog", "dash dot line"),
                                  QT_TRANSLATE_NOOP("PaintingDialog", "dash dot dot line")};

const char* const kBrushStyles[] = {
    QT_TRANSLATE_NOOP("PaintingDialog", "no filling"),       QT_TRANSLATE_NOOP("PaintingDialog", "solid"),
    QT_TRANSLATE_NOOP("PaintingDialog", "dense 1 (densest)"), QT_TRANSLATE_NOOP("PaintingDialog", "dense 2"),
    QT_TRANSLATE_NOOP("PaintingDialog", "dense 3"),          QT_TRANSLATE_NOOP("PaintingDialog", "dense 4"),
    QT_TRANSLATE_NOOP("PaintingDialog", "dense 5"),          QT_TRANSLATE_NOOP("PaintingDialog", "dense 6"),
    QT_TRANSLATE_NOOP("PaintingDialog", "dense 7 (least dense)"),
    QT_TRANSLATE_NOOP("PaintingDialog", "horizontal line"),  QT_TRANSLATE_NOOP("PaintingDialog", "vertical line"),
    QT_TRANSLATE_NOOP("PaintingDialog", "crossed lines"),    QT_TRANSLATE_NOOP("PaintingDialog", "hatched backwards"),
    QT_TRANSLATE_NOOP("PaintingDialog", "hatched forwards"), QT_TRANSLATE_NOOP("PaintingDialog", "diagonal crossed")};

} // namespace

namespace qucs_s::paintings {

QString encodeText(const QString& text)
{
  return QString::fromLatin1(QUrl::toPercentEncoding(text, QByteArray(), QByteArray(" \"<>%\\")));
}

QString decodeText(const QString& text)
{
  return QUrl::fromPercentEncoding(text.toLatin1());
}

QFont textFont(int pointSize, bool bold)
{
  QFont font = QucsSettings.font;
  font.setPointSize(std::max(1, pointSize));
  QFont drawn = misc::canvasFont(font);
  drawn.setPixelSize(std::max(1, QFontInfo(font).pixelSize()));
  drawn.setBold(bold);
  return drawn;
}

bool editFields(Painting* painting, const QString& title, QWidget* parent, bool* accepted)
{
  if (accepted != nullptr) *accepted = false;
  auto* editable = dynamic_cast<FieldEditable*>(painting);
  if (editable == nullptr) return false;
  const QString before = painting->save();
  std::unique_ptr<Painting> shown(painting->newOne());
  shown->load(before);
  auto* shownFields = dynamic_cast<FieldEditable*>(shown.get());

  QDialog dialog(parent);
  dialog.setObjectName(QStringLiteral("paintingDialog"));
  dialog.setWindowTitle(title);
  auto* top = new QVBoxLayout(&dialog);
  auto* body = new QHBoxLayout;
  auto* column = new QVBoxLayout;
  body->addLayout(column);
  auto* preview = new Preview(shown.get(), &dialog);
  body->addWidget(preview, 1);
  top->addLayout(body, 1);

  const QList<PaintingField> fields = editable->fields();
  std::map<QString, std::function<QVariant()>> values;
  std::map<QString, QWidget*> widgets;
  std::map<QString, QFormLayout*> groups;
  QList<std::function<void()>> afterChange;
  const auto changed = [&] {
    for (const PaintingField& f : fields) shownFields->setField(f.key, values[f.key]());
    for (const auto& also : afterChange) also();
    preview->update();
  };

  for (const PaintingField& f : fields) {
    const QString group = f.group.isEmpty() ? tr("Properties") : f.group;
    QFormLayout*& form = groups[group];
    if (form == nullptr) {
      auto* box = new QGroupBox(group, &dialog);
      form = new QFormLayout(box);
      column->addWidget(box);
    }
    QWidget* w = nullptr;
    switch (f.kind) {
    case PaintingField::Colour: {
      auto* b = new QPushButton(QStringLiteral("        "), &dialog);
      misc::setPickerColor(b, f.value.value<QColor>());
      QObject::connect(b, &QPushButton::clicked, &dialog, [&, b, label = f.label] {
        const QColor c = QColorDialog::getColor(misc::getWidgetBackgroundColor(b), &dialog, label);
        if (!c.isValid()) return;
        misc::setPickerColor(b, c);
        changed();
      });
      values[f.key] = [b] { return QVariant(misc::getWidgetBackgroundColor(b)); };
      w = b;
      break;
    }
    case PaintingField::Int: {
      auto* s = new QSpinBox(&dialog);
      s->setRange(int(f.min), int(f.max));
      s->setValue(f.value.toInt());
      QObject::connect(s, &QSpinBox::valueChanged, &dialog, changed);
      values[f.key] = [s] { return QVariant(s->value()); };
      w = s;
      break;
    }
    case PaintingField::Real: {
      auto* s = new QDoubleSpinBox(&dialog);
      s->setDecimals(f.decimals);
      s->setRange(f.min, f.max);
      s->setSingleStep(std::pow(10.0, -std::min(f.decimals, 2)));
      s->setValue(f.value.toDouble());
      QObject::connect(s, &QDoubleSpinBox::valueChanged, &dialog, changed);
      values[f.key] = [s] { return QVariant(s->value()); };
      w = s;
      break;
    }
    case PaintingField::Choice:
    case PaintingField::PenStyle:
    case PaintingField::BrushStyle: {
      auto* c = new QComboBox(&dialog);
      int offset = 0;
      if (f.kind == PaintingField::Choice) {
        c->addItems(f.choices);
      } else if (f.kind == PaintingField::PenStyle) {
        for (const char* s : kPenStyles) c->addItem(tr(s));
        offset = 1;   // Qt::SolidLine is 1
      } else {
        for (const char* s : kBrushStyles) c->addItem(tr(s));
      }
      c->setCurrentIndex(std::clamp(f.value.toInt() - offset, 0, c->count() - 1));
      QObject::connect(c, &QComboBox::currentIndexChanged, &dialog, changed);
      values[f.key] = [c, offset] { return QVariant(c->currentIndex() + offset); };
      w = c;
      break;
    }
    case PaintingField::Text: {
      auto* e = new QLineEdit(f.value.toString(), &dialog);
      QObject::connect(e, &QLineEdit::textChanged, &dialog, changed);
      values[f.key] = [e] { return QVariant(e->text()); };
      w = e;
      break;
    }
    case PaintingField::LongText: {
      auto* e = new QPlainTextEdit(f.value.toString(), &dialog);
      e->setTabChangesFocus(true);
      e->setMinimumHeight(90);
      QObject::connect(e, &QPlainTextEdit::textChanged, &dialog, changed);
      values[f.key] = [e] { return QVariant(e->toPlainText()); };
      w = e;
      break;
    }
    case PaintingField::Check: {
      auto* c = new QCheckBox(f.label, &dialog);
      c->setChecked(f.value.toBool());
      QObject::connect(c, &QCheckBox::toggled, &dialog, changed);
      values[f.key] = [c] { return QVariant(c->isChecked()); };
      w = c;
      break;
    }
    case PaintingField::Cells: {
      auto* t = new QTableWidget(&dialog);
      t->setMinimumSize(260, 140);
      t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
      const QVariantList rows = f.value.toList();
      t->setRowCount(int(rows.size()));
      t->setColumnCount(rows.isEmpty() ? 0 : int(rows.first().toStringList().size()));
      for (int r = 0; r < rows.size(); ++r) {
        const QStringList cells = rows.at(r).toStringList();
        for (int c = 0; c < cells.size() && c < t->columnCount(); ++c) t->setItem(r, c, new QTableWidgetItem(cells.at(c)));
      }
      QObject::connect(t, &QTableWidget::itemChanged, &dialog, changed);
      values[f.key] = [t] {
        QVariantList rows;
        for (int r = 0; r < t->rowCount(); ++r) {
          QStringList cells;
          for (int c = 0; c < t->columnCount(); ++c) cells << (t->item(r, c) != nullptr ? t->item(r, c)->text() : QString());
          rows << cells;
        }
        return QVariant(rows);
      };
      // Sized by the rows and columns fields.
      afterChange << [&widgets, t] {
        const auto rows = widgets.find(QStringLiteral("rows")), cols = widgets.find(QStringLiteral("columns"));
        if (rows == widgets.end() || cols == widgets.end()) return;
        const QSignalBlocker quiet(t);
        t->setRowCount(static_cast<QSpinBox*>(rows->second)->value());
        t->setColumnCount(static_cast<QSpinBox*>(cols->second)->value());
      };
      w = t;
      break;
    }
    }
    w->setObjectName(QStringLiteral("field_") + f.key);
    if (!f.tip.isEmpty()) w->setToolTip(f.tip);
    widgets[f.key] = w;
    if (f.kind == PaintingField::Check) form->addRow(w);
    else if (f.kind == PaintingField::Cells || f.kind == PaintingField::LongText) {
      form->addRow(new QLabel(f.label, &dialog));
      form->addRow(w);
    } else {
      form->addRow(f.label, w);
    }
  }
  // Fields editable while a check is on.
  for (const PaintingField& f : fields) {
    if (f.enabledBy.isEmpty()) continue;
    auto* check = qobject_cast<QCheckBox*>(widgets[f.enabledBy]);
    QWidget* w = widgets[f.key];
    if (check == nullptr || w == nullptr) continue;
    w->setEnabled(check->isChecked());
    QObject::connect(check, &QCheckBox::toggled, w, &QWidget::setEnabled);
  }
  column->addStretch(1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  top->addWidget(buttons);
  changed();   // (the Cells sized)

  if (dialog.exec() != QDialog::Accepted) return false;
  if (accepted != nullptr) *accepted = true;
  for (const PaintingField& f : fields) editable->setField(f.key, values[f.key]());
  return painting->save() != before;
}

} // namespace qucs_s::paintings
