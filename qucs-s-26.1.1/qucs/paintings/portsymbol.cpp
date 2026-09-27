/***************************************************************************
                        portsymbol.cpp  -  description
                             -------------------
    begin                : Sun Sep 5 2004
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
#include "main.h"
#include "one_point.h"
#include "portsymbol.h"
#include "schematic.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QMargins>
#include <QPainter>
#include "misc.h"
#include "ink.h"

namespace helper {

inline QSize textSize(const QString& text) {
  return QFontMetrics(QucsSettings.font, nullptr).size(0, text);
}


class TextHelper {
  static constexpr int c = 8; // Some inherited magic number
  int m_angle;
  QSize m_textSize;

public:
  TextHelper(int a, const QString& s) : m_angle{a} {
    m_textSize = QFontMetrics(QucsSettings.font, nullptr).size(0, s);
  }

  QPoint offset() const {
    const int half_height = m_textSize.height() / 2;

    switch (m_angle) {
    case 180:
    case 270:
      return {c, -half_height};
    default:
      return {-c - m_textSize.width(), -half_height};
    }
  }

  QRect bounds() const {
    auto to = offset();
    switch (m_angle) {
    case 90:
      return QRect{to.y(), to.x() + m_textSize.width() + 2 * c, m_textSize.height(),
                   m_textSize.width()};
    case 270:
      return QRect{to.x() - m_textSize.height(), to.y() - m_textSize.width(), m_textSize.height(),
                   m_textSize.width()};
    default:
      return QRect{to, m_textSize};
    }
  }
};


// Calculates the bounding of port symbol and offset of port name text
std::pair<QRect, QPoint> boundingAndTextOffset(int angle, const QString& portName, int circleRadius) {
  const QRect circle_br{
    -QPoint{circleRadius, circleRadius},
     QPoint{circleRadius, circleRadius}};

  const TextHelper th{angle, portName};

  const auto total_br = circle_br
    .united(th.bounds())
    .normalized()
    .marginsAdded(QMargins{2, 2, 2, 2});

  return {total_br, th.offset()};
}
} // namespace helper


namespace qucs_s::portsym {

Name read(const QString& rest)
{
  Name n;
  const QString s = rest.trimmed();
  // A label is the line's end, in quotes (it has none inside).
  const qsizetype quote = s.indexOf(QLatin1Char('"'));
  if (quote >= 0 && s.size() - quote >= 2 && s.endsWith(QLatin1Char('"'))) {
    n.name = s.left(quote).trimmed();
    n.label = s.mid(quote + 1, s.size() - quote - 2);
    n.labelSet = !n.label.contains(QLatin1Char('"'));
    if (n.labelSet) return n;
    n.label.clear();
  }
  n.name = s;
  return n;
}

QString write(const Name& n)
{
  if (!n.labelSet) return n.name;
  QString label = n.label;
  label.remove(QLatin1Char('"'));
  return n.name + QStringLiteral(" \"") + label + QLatin1Char('"');
}

} // namespace qucs_s::portsym

constexpr int portCircleRadius = 4;
constexpr int portCircleDiameter = 2 * portCircleRadius;


PortSymbol::PortSymbol(int cx_, int cy_, const QString& numberStr_,
                                         const QString& nameStr_)
    : numberStr(numberStr_)
    , nameStr(nameStr_)
    , angle(0)
{
  Name = ".PortSym ";
  isSelected = false;
  cx = cx_;
  cy = cy_;

  updateBounds();
}

void PortSymbol::paint(QPainter *painter)
{
  // Little circle and port name
  {
    painter->save();

    painter->translate(center());

    // Little circle
    const QRect circle_br{ -portCircleRadius, -portCircleRadius, portCircleDiameter, portCircleDiameter };
    painter->setPen(qucs_s::ink::on(QPen(Qt::red,1)));  // like open node
    painter->drawEllipse(circle_br);

    // The name is laid out to one side of the circle; the mark for the
    // port's direction goes to the other, which is the symbol's side.
    if (angle == 90 || angle == 270) painter->rotate(-90.0);

    // Which way the port points, when the setting asks for it
    const QString dir = dirStr.toLower();
    if (QucsSettings.ShowPinDirections
        && (dir == QLatin1String("in") || dir == QLatin1String("out")
            || dir == QLatin1String("inout"))) {
      const int away = (angle == 180 || angle == 270) ? -1 : 1;
      const QPoint tail(away * (portCircleRadius + 3), 0);
      const QPoint head(away * (portCircleRadius + 10), 0);
      const QPoint across(0, 3);

      painter->setPen(qucs_s::ink::on(QPen(Qt::darkGreen, 1)));
      painter->setBrush(qucs_s::ink::on(QBrush(Qt::darkGreen)));
      if (dir == QLatin1String("inout")) {
        const QPoint middle((tail + head) / 2);
        painter->drawPolygon(QPolygon() << tail << (middle + across) << head << (middle - across));
      } else if (dir == QLatin1String("in")) {
        painter->drawPolygon(QPolygon() << tail << (head + across) << (head - across));
      } else {
        painter->drawPolygon(QPolygon() << head << (tail + across) << (tail - across));
      }
    }

    // Port name
    painter->setPen(qucs_s::ink::on(Qt::black));
    painter->drawText(m_textOrigin.x(), m_textOrigin.y(), 1, 1, Qt::TextDontClip, editorText());
    painter->restore();
  }


  // Rectangle and selection box
  {
    painter->save();

    // Rectangle around the text and the circle.
    painter->setPen(qucs_s::ink::on(Qt::lightGray));
    painter->drawRect(boundingRect());

    // Selection box
    if (isSelected) {
      painter->setPen(qucs_s::ink::on(QPen(Qt::darkGray,3)));
      painter->drawRoundedRect(boundingRect().marginsAdded(QMargins{3, 3, 3, 3}), 4, 4);
    }

    painter->restore();
  }
}

void PortSymbol::paintScheme(Schematic *p)
{
  p->PostPaintEvent(_Ellipse, cx - portCircleRadius, cy - portCircleRadius, portCircleDiameter, portCircleDiameter);
  p->PostPaintEvent(_Rect, x1, y1, x2 - x1, y2 - y1);
}

bool PortSymbol::load(const QString& s)
{
  bool ok;

  QString n;
  n  = s.section(' ',1,1);    // cx
  cx = misc::clampCoordinate(n.toInt(&ok));
  if(!ok) return false;

  n  = s.section(' ',2,2);    // cy
  cy = misc::clampCoordinate(n.toInt(&ok));
  if(!ok) return false;

  numberStr  = s.section(' ',3,3);    // number
  if(numberStr.isEmpty()) return false;

  n  = s.section(' ',4,4);      // Angel
  if(n.isEmpty()) return true;  // be backward-compatible
  angle = n.toInt(&ok);
  if(!ok) return false;

  // name string, and the label its instances show instead (in quotes)
  n = s.section(' ', 5);
  if (n.isEmpty()) return true;
  const qucs_s::portsym::Name named = qucs_s::portsym::read(n);
  nameStr = named.name;
  labelStr = named.label;
  labelSet = named.labelSet;

  updateBounds();
  return true;
}

QString PortSymbol::save()
{
  QString s = Name+QString::number(cx)+" "+QString::number(cy)+" ";
  s += numberStr+" "+QString::number(angle) + " " + qucs_s::portsym::write({nameStr, labelStr, labelSet});
  return s;
}

QString PortSymbol::editorText() const
{
  const QString name = nameStr.isEmpty() ? numberStr : nameStr;
  if (!labelSet) return name;
  return labelStr.isEmpty() ? QObject::tr("%1 (not shown)").arg(name) : QObject::tr("%1 (shown as %2)").arg(name, labelStr);
}

QString PortSymbol::saveCpp()
{
  QString s =
    QString ("new Port (%1, %2)").
    arg(cx).arg(cy);
  s = "Ports.append (" + s + "); /* " + nameStr + " */";
  return s;
}

QString PortSymbol::saveJSON()
{
  QString s = QString ("{\"type\" : \"portsymbol\", "
                       "\"x\" : %1, \"y\" : %2},").arg(cx).arg(cy);
  return s;
}

// Checks if the coordinates x/y point to the painting.
bool PortSymbol::getSelected(const QPoint& click, int /*tolerance*/)
{
  return QRect{QPoint{x1, y1}, QPoint{x2, y2}}.contains(click);
}

// Rotates around the center.
inline bool PortSymbol::rotate() noexcept
{
  if (angle < 270) {
    angle += 90;
  } else {
    angle = 0;
  }
  updateBounds();
  return true;
}

// Rotates around the center.
inline bool PortSymbol::rotate(int x, int y) noexcept
{
  qucs_s::geom::rotate_point_ccw(cx, cy, x, y);
  rotate();
  return true;
}

// Mirrors about connection node (not center line !).
bool PortSymbol::mirrorX() noexcept
{
  switch (angle) {
    case 90:
      angle = 270;
      break;
    case 270:
      angle = 90;
      break;
    default:
      break;
  };
  updateBounds();
  return true;
}

// Mirrors about connection node (not center line !).
bool PortSymbol::mirrorY() noexcept
{
  switch (angle) {
    case 0:
      angle = 180;
      break;
    case 180:
      angle = 0;
      break;
    default:
      break;
  };
  updateBounds();
  return true;
}

bool PortSymbol::MousePressing(Schematic *sch) {
  if (!sch->getIsSymbolOnly()) {
    return false;
  }
  QString text = QInputDialog::getText(nullptr, QObject::tr("Port name"),
                                        QObject::tr("Input port name:"));
  if (!text.isNull() && !text.isEmpty()) {
    nameStr = text;
    numberStr = "0"; // 0 indicates no number assigned
    updateBounds();
    return true;
  }

  return false;
}

void PortSymbol::MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& /*cursor*/) {
  moveCenterTo(onGrid.x(), onGrid.y());
  paintScheme(sch);
}

Painting* PortSymbol::newOne() {
  return new PortSymbol();
}

// This function is called from double click handler, see mouseactions.cpp
// Returned bool signal whether the object has changed as a result of
// the invocation.
bool PortSymbol::Dialog(QWidget* /*parent*/Doc) {
  // The name follows the schematic's Port component (it is the net's), so
  // it is typed only in a symbol file (*.sym), which has none. What the
  // instances show beside the pin can be set in either.
  Schematic *sch = (Schematic *) Doc;
  const bool symbolOnly = sch->getIsSymbolOnly();

  QDialog dialog(sch);
  dialog.setObjectName(QStringLiteral("portSymbolDialog"));
  dialog.setWindowTitle(QObject::tr("Port %1").arg(numberStr));
  auto* form = new QFormLayout;
  auto* name = new QLineEdit(nameStr, &dialog);
  name->setObjectName(QStringLiteral("portName"));
  name->setReadOnly(!symbolOnly);
  if (!symbolOnly) name->setToolTip(QObject::tr("The name of the schematic's Port component: change it there"));
  form->addRow(QObject::tr("Name:"), name);

  auto* shown = new QGroupBox(QObject::tr("Beside the pin, the instances show"), &dialog);
  auto* asName = new QRadioButton(QObject::tr("Its name"), shown);
  auto* nothing = new QRadioButton(QObject::tr("Nothing"), shown);
  auto* asText = new QRadioButton(QObject::tr("This text:"), shown);
  auto* label = new QLineEdit(labelStr, shown);
  asName->setObjectName(QStringLiteral("portShowName"));
  nothing->setObjectName(QStringLiteral("portShowNothing"));
  asText->setObjectName(QStringLiteral("portShowText"));
  label->setObjectName(QStringLiteral("portLabel"));
  label->setPlaceholderText(QObject::tr("+, -, CLK, ..."));
  (labelSet ? (labelStr.isEmpty() ? nothing : asText) : asName)->setChecked(true);
  label->setEnabled(asText->isChecked());
  QObject::connect(asText, &QRadioButton::toggled, label, &QLineEdit::setEnabled);
  QObject::connect(label, &QLineEdit::textEdited, asText, [asText] { asText->setChecked(true); });
  auto* choices = new QGridLayout(shown);
  choices->addWidget(asName, 0, 0, 1, 2);
  choices->addWidget(nothing, 1, 0, 1, 2);
  choices->addWidget(asText, 2, 0);
  choices->addWidget(label, 2, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  auto* layout = new QVBoxLayout(&dialog);
  layout->addLayout(form);
  layout->addWidget(shown);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) return false;

  const QString newName = symbolOnly && !name->text().trimmed().isEmpty() ? name->text().trimmed() : nameStr;
  const bool newSet = !asName->isChecked();
  QString newLabel = asText->isChecked() ? label->text().trimmed() : QString();
  newLabel.remove(QLatin1Char('"'));
  if (newName == nameStr && newSet == labelSet && newLabel == labelStr) return false;
  nameStr = newName;
  labelSet = newSet;
  labelStr = newLabel;
  updateBounds();
  return true;
}

void PortSymbol::updateBounds()
{
  const QString text = editorText();
  auto [br, to] = helper::boundingAndTextOffset(angle, text, portCircleRadius);

  m_textOrigin = to;
  x1 = cx + br.left();
  y1 = cy + br.top();
  x2 = cx + br.right();
  y2 = cy + br.bottom();
}

void PortSymbol::placeLike(const PortSymbol& other)
{
  cx = other.cx;
  cy = other.cy;
  angle = other.angle;
  updateBounds();
}

void PortSymbol::setPortName(const QString& newName)
{
  nameStr = newName;
  updateBounds();
}
