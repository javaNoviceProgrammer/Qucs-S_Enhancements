/***************************************************************************
                          markerdialog.cpp  -  description
                             -------------------
    begin                : Wed April 21 2004
    copyright            : (C) 2003 by Michael Margraf
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
#include "markerdialog.h"
#include "diagram.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QValidator>
#include <QGridLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QPainter>
#include <QToolButton>
#include "qucs_assert.h"

// A swatch of \a color for a button, a checkerboard under it when it is
// see-through.
QIcon colorSwatch(const QColor& color)
{
  QPixmap pixmap(28, 16);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  const QRect box(0, 0, 27, 15);
  if (color.alpha() < 255)
    for (int y = 0; y < 16; y += 4)
      for (int x = 0; x < 28; x += 4)
        p.fillRect(x, y, 4, 4, ((x + y) / 4) % 2 ? QColor(0xcc, 0xcc, 0xcc) : Qt::white);
  p.fillRect(box, color);
  p.setPen(QColor(0x80, 0x80, 0x80));
  p.drawRect(box);
  return QIcon(pixmap);
}


MarkerDialog::MarkerDialog(Marker *pm_, QWidget *parent)
                     : QDialog(parent)
{
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Edit Marker Properties"));
  pMarker = pm_;

  QGridLayout *g = new QGridLayout;

  Precision = new QLineEdit();
  Precision->setText(QString::number(pMarker->Precision));
  Precision->setValidator(new QIntValidator(0, 12, this));

  XPosition = new QLineEdit();
  if (pMarker->varPos().size() > 0) {
      XPosition->setText(QString::number(pMarker->varPos().at(0),
                         'g', pMarker->precision()));
  } else {
      XPosition->setText("0");
      XPosition->setEnabled(false);
  }
  QDoubleValidator *dblVal = new QDoubleValidator(this);
  dblVal->setLocale(QLocale::C);
  XPosition->setValidator(dblVal);

  Precision->setToolTip(tr("Significant digits in the automatic notation; places after the point in the "
                           "others (of the mantissa, with an exponent or a prefix)."));
  g->addWidget(new QLabel(tr("Precision: ")), 0, 0);
  g->addWidget(Precision, 0, 1);

  // How its numbers are written: as the diagram writes those of its axes,
  // or a notation of its own - each with examples; the item's data is
  // what the marker keeps (-1, or a numberformat::Notation).
  NotationBox = new QComboBox();
  NotationBox->setObjectName(QStringLiteral("markerNotation"));
  QString diagramsNotation;
  for (const auto& [notation, text] : qucs_s::numberformat::choices())
    if (pMarker->diag() && notation == pMarker->diag()->notation) diagramsNotation = text.section(QLatin1String(" ("), 0, 0);
  NotationBox->addItem(diagramsNotation.isEmpty() ? tr("as the diagram's axes")
                                                  : tr("as the diagram's axes (now %1)").arg(diagramsNotation), -1);
  for (const auto& [notation, text] : qucs_s::numberformat::choices())
    NotationBox->addItem(text, int(notation));
  NotationBox->setCurrentIndex(std::max(0, NotationBox->findData(pMarker->notation)));
  NotationBox->setToolTip(tr("How the marker writes its numbers - its position and its value"));
  g->addWidget(new QLabel(tr("Number format: ")), 1, 0);
  g->addWidget(NotationBox, 1, 1);

  NumberBox = new QComboBox();
  NumberBox->addItem(tr("real/imaginary"));
  NumberBox->addItem(tr("magnitude/angle (degree)"));
  NumberBox->addItem(tr("magnitude/angle (radian)"));
  NumberBox->setCurrentIndex(pMarker->numMode);

  g->addWidget(new QLabel(tr("Complex values: ")), 2, 0);
  g->addWidget(NumberBox, 2, 1);

  QLabel *lblXpos = new QLabel(tr("X-axis position:"));
  g->addWidget(lblXpos, 3, 0);
  g->addWidget(XPosition, 3, 1);

  IndicatorBox = new QComboBox();
  IndicatorBox->addItem(tr("Off"));
  IndicatorBox->addItem(tr("Square"));
  IndicatorBox->addItem(tr("Triangle"));
  IndicatorBox->setCurrentIndex(pMarker->indicatorMode);

  QLabel *lblIndicator = new QLabel(tr("Marker Indicator"));
  g->addWidget(lblIndicator, 4, 0);
  g->addWidget(IndicatorBox, 4, 1);

  QUCS_ASSERT(pMarker->diag());
  if(pMarker->diag()->Name=="Smith") // BUG
  {
      //S parameter also displayed as Z, need Z0 here
      lblXpos->setText("Frequency:");
      SourceImpedance = new QLineEdit();
      SourceImpedance->setText(QString::number(pMarker->Z0));

      g->addWidget(new QLabel(tr("Z0: ")), 5, 0);
      g->addWidget(SourceImpedance, 5, 1);
  }
  
  // The colours of its text and its background: automatic (the paper's
  // ink and the paper - a diagram's light card in the dark theme) or
  // chosen.
  const auto colorRow = [this, g](int row, const QString& label, QPushButton*& button, QToolButton*& automatic,
                                  const char* name) {
    button = new QPushButton();
    button->setObjectName(QLatin1String(name));
    button->setIconSize(QSize(28, 16));
    automatic = new QToolButton();
    automatic->setObjectName(QLatin1String(name) + QStringLiteral("Auto"));
    automatic->setText(tr("Reset"));
    automatic->setToolTip(tr("Back to automatic: the paper's colours, dark text on the light background the "
                             "diagram is drawn on"));
    auto* both = new QHBoxLayout();
    both->setSpacing(4);
    both->addWidget(button, 1);
    both->addWidget(automatic);
    g->addWidget(new QLabel(label), row, 0);
    g->addLayout(both, row, 1);
  };
  colorRow(6, tr("Text color:"), TextColorButton, TextColorAuto, "markerTextColor");
  colorRow(7, tr("Background color:"), FillColorButton, FillColorAuto, "markerFillColor");
  connect(TextColorButton, &QPushButton::clicked, this, [this] {
    const QColor c = QColorDialog::getColor(a_textColor.isValid() ? a_textColor : pMarker->shownTextColor(), this,
                                            tr("Marker Text Color"));
    if (c.isValid()) setTextColor(c);
  });
  connect(FillColorButton, &QPushButton::clicked, this, [this] {
    const QColor c = QColorDialog::getColor(a_fillColor.isValid() ? a_fillColor : pMarker->shownFillColor(), this,
                                            tr("Marker Background Color"), QColorDialog::ShowAlphaChannel);
    if (c.isValid()) setFillColor(c);
  });
  connect(TextColorAuto, &QToolButton::clicked, this, [this] { setTextColor(QColor()); });
  connect(FillColorAuto, &QToolButton::clicked, this, [this] { setFillColor(QColor()); });

  TransBox = new QCheckBox(tr("Transparent background"));
  TransBox->setObjectName(QStringLiteral("markerTransparent"));
  TransBox->setChecked(pMarker->transparent);
  g->addWidget(TransBox, 8, 0, 1, 2);
  connect(TransBox, &QCheckBox::toggled, this, &MarkerDialog::showColors);
  a_textColor = pMarker->textColor;
  a_fillColor = pMarker->fillColor;
  showColors();

  // A delta marker: Δx, Δy and 1/Δx from another marker of the diagram.
  g->addWidget(new QLabel(tr("Relative to:")), 9, 0);
  RelativeBox = new QComboBox();
  RelativeBox->setObjectName(QStringLiteral("markerRelativeTo"));
  RelativeBox->addItem(tr("none"), -1);
  RelativeBox->setToolTip(tr("Its text adds \u0394x, \u0394y and 1/\u0394x from that marker, and a dashed line joins the two: "
                             "the time between two edges, the band between two -3 dB points"));
  if (const Diagram *d = pMarker->diag()) {
    const QList<Marker *> all = d->markers();
    for (int i = 0; i < all.size(); ++i) {
      if (all.at(i) == pMarker) continue;
      const QString first = all.at(i)->Text.section('\n', 0, 0);
      RelativeBox->addItem(tr("marker %1 (%2)").arg(i + 1).arg(first), i);
      if (all.at(i) == pMarker->reference()) RelativeBox->setCurrentIndex(RelativeBox->count() - 1);
    }
  }
  g->addWidget(RelativeBox, 9, 1);

  // first => activated by pressing RETURN
  QPushButton *ButtOK = new QPushButton(tr("OK"));
  connect(ButtOK, SIGNAL(clicked()), SLOT(slotAcceptValues()));

  QPushButton *ButtCancel = new QPushButton(tr("Cancel"));
  connect(ButtCancel, SIGNAL(clicked()), SLOT(reject()));

  QHBoxLayout *b = new QHBoxLayout();
  b->setSpacing(5);
  b->addWidget(ButtOK);
  b->addWidget(ButtCancel);
  g->addLayout(b,10,0,1,2);   // (under the rest: it covered the check box once)

  this->setLayout(g);
}

MarkerDialog::~MarkerDialog()
{
}

void MarkerDialog::setTextColor(const QColor& color)
{
  a_textColor = color;
  showColors();
}

void MarkerDialog::setFillColor(const QColor& color)
{
  a_fillColor = color;
  showColors();
}

void MarkerDialog::showColors()
{
  const auto show = [this](QPushButton* button, QToolButton* automatic, const QColor& chosen, const QColor& shown) {
    button->setIcon(colorSwatch(chosen.isValid() ? chosen : shown));
    button->setText(chosen.isValid() ? chosen.name(chosen.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb).toUpper()
                                     : tr("Automatic"));
    automatic->setEnabled(chosen.isValid());
  };
  // (Automatic: as it is drawn - black on the light background.)
  show(TextColorButton, TextColorAuto, a_textColor, QColor(Qt::black));
  show(FillColorButton, FillColorAuto, a_fillColor, QColor(Qt::white));
  const bool filled = !TransBox->isChecked();
  FillColorButton->setEnabled(filled);
  FillColorAuto->setEnabled(filled && a_fillColor.isValid());
}

// ----------------------------------------------------------
void MarkerDialog::slotAcceptValues()
{
  bool changed = false;
  int tmp = Precision->text().toInt();
  if(tmp != pMarker->Precision) {
    pMarker->Precision = tmp;
    changed = true;
  }
  QUCS_ASSERT(pMarker->diag());
  if(pMarker->diag()->Name=="Smith") // BUG: need generic MarkerDialog.
	{
			double SrcImp = SourceImpedance->text().toDouble();
			if(SrcImp != pMarker->Z0)
			{
					pMarker->Z0 = SrcImp;
					changed = true;
			}
	}
  if(NumberBox->currentIndex() != pMarker->numMode) {
    pMarker->numMode = NumberBox->currentIndex();
    changed = true;
  }
  if (NotationBox->currentData().toInt() != pMarker->notation) {
    pMarker->notation = NotationBox->currentData().toInt();
    changed = true;
  }
  if (IndicatorBox->currentIndex() != pMarker->indicatorMode) {
    pMarker->indicatorMode = static_cast<indicatorMode_t>(IndicatorBox->currentIndex());
    changed = true;
  }
  if(TransBox->isChecked() != pMarker->transparent) {
    pMarker->transparent = TransBox->isChecked();
    changed = true;
  }
  if (a_textColor != pMarker->textColor) {
    pMarker->textColor = a_textColor;
    changed = true;
  }
  if (a_fillColor != pMarker->fillColor) {
    pMarker->fillColor = a_fillColor;
    changed = true;
  }

  {
    const Diagram *d = pMarker->diag();
    const int at = RelativeBox->currentData().toInt();
    const Marker *ref = d && at >= 0 && at < d->markers().size() ? d->markers().at(at) : nullptr;
    if (ref != pMarker->reference()) {
      pMarker->setReference(ref);
      changed = true;
    }
  }

  double xpos = XPosition->text().toDouble();
  if ((xpos != pMarker->powFreq()) &&
      (pMarker->varPos().size() > 0)) {
      pMarker->setPos(XPosition->text().toDouble());
      changed = true;
  }

  if(changed) {
    pMarker->createText();
    pMarker->refreshDependents();
    done(2);
  }
  else done(1);
}

// vim:ts=8:sw=2:noet
