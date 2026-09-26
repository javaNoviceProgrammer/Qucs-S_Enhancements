/*
 * paintingfields.h - a painting's properties as fields, and the dialog
 *                    made of them (with a preview)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef PAINTINGFIELDS_H
#define PAINTINGFIELDS_H

#include <QFont>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <utility>

class Painting;
class QWidget;

/// A property of a painting, as its dialog edits it.
struct PaintingField {
  enum Kind {
    Colour,       ///< a QColor
    Int,          ///< an int in [min, max]
    Real,         ///< a double in [min, max], decimals
    Choice,       ///< the index of one of choices
    Text,         ///< one line
    LongText,     ///< lines
    Check,        ///< a bool
    PenStyle,     ///< a Qt::PenStyle (solid to dash-dot-dot)
    BrushStyle,   ///< a Qt::BrushStyle (none to diagonal crossed)
    Cells         ///< rows of texts (a QVariantList of QStringList), sized by the fields "rows" and "columns"
  };
  PaintingField() = default;
  PaintingField(QString key_, QString label_, Kind kind_, QVariant value_, double min_ = 0, double max_ = 100,
                int decimals_ = 2, QStringList choices_ = {}, QString group_ = {}, QString tip_ = {},
                QString enabledBy_ = {})
      : key(std::move(key_)), label(std::move(label_)), kind(kind_), value(std::move(value_)), min(min_), max(max_),
        decimals(decimals_), choices(std::move(choices_)), group(std::move(group_)), tip(std::move(tip_)),
        enabledBy(std::move(enabledBy_))
  {
  }

  QString key;
  QString label;
  Kind kind = Int;
  QVariant value;
  double min = 0;
  double max = 100;
  int decimals = 2;
  QStringList choices;
  QString group;       ///< the box it goes in ("Line", "Fill", "Shape", "Text"...)
  QString tip;
  QString enabledBy;   ///< a Check field's key: editable while that is on
};

/// A painting whose properties dialog is made of its fields.
class FieldEditable {
public:
  virtual ~FieldEditable() = default;
  virtual QList<PaintingField> fields() const = 0;
  virtual void setField(const QString& key, const QVariant& value) = 0;
};

namespace qucs_s::paintings {

/// Edits \a painting (FieldEditable): its fields in boxes beside a preview
/// that follows them. True when it changed; \a accepted: whether OK was
/// pressed, changed or not.
bool editFields(Painting* painting, const QString& title, QWidget* parent, bool* accepted = nullptr);

/// Text for a painting's line: without spaces, quotes, angle brackets,
/// percent signs or line breaks (percent-encoded), and back.
QString encodeText(const QString& text);
QString decodeText(const QString& text);

/// The font of a painting's text of \a pointSize, as the canvas draws
/// text (GraphicText's pixel size).
QFont textFont(int pointSize, bool bold = false);

} // namespace qucs_s::paintings

#endif
