/***************************************************************************
                              diagramdialog.cpp
                             -------------------
    begin                : Sun Oct 5 2003
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

/*!
  \class DiagramDialog
  \brief The DiagramDialog is used to setup and edit diagrams.
*/
#include "diagramdialog.h"
#include "dataimport.h"
#include "dataexportpanel.h"
#include "dataimportpanel.h"
#include "extsimkernels/spicecompat.h"
#include "ink.h"
#include "main.h"
#include "markerdialog.h"
#include "misc.h"
#include "qucs.h"
#include "rect3ddiagram.h"
#include "histogramdiagram.h"
#include "eyediagram.h"
#include "stackeddiagram.h"
#include "polezerodiagram.h"
#include "bodediagram.h"
#include "nicholsdiagram.h"
#include "spectrumdiagram.h"
#include "bathtubdiagram.h"
#include "contourdiagram.h"
#include "spectrogramdiagram.h"
#include "tornadodiagram.h"
#include "boxplotdiagram.h"
#include "constellationdiagram.h"
#include "smithdiagram.h"
#include "polardiagram.h"
#include "valuereading.h"
#include "schematic.h"
#include "settings.h"

#include <assert.h>
#include <cmath>
#include <functional>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QStandardItemModel>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtAlgorithms>

// Variable completion
#include <QCompleter>
#include <QStringListModel>
#include "qucs_assert.h"

#define CROSS3D_SIZE 30
#define WIDGET3D_SIZE 2 * CROSS3D_SIZE
// This widget class paints a small 3-dimensional coordinate cross.
class Cross3D : public QWidget {
public:
  Cross3D(float rx_, float ry_, float rz_, QWidget *parent = 0)
      : QWidget(parent) {
    rotX = rx_;
    rotY = ry_;
    rotZ = rz_;
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    setMinimumSize(WIDGET3D_SIZE, WIDGET3D_SIZE);
    resize(WIDGET3D_SIZE, WIDGET3D_SIZE);
  };
  ~Cross3D(){};

  double rotX, rotY, rotZ; // in radians !!!!

private:
  void paintEvent(QPaintEvent *) {
    QPainter Painter(this);
    float cxx = cos(rotZ);
    float cxy = sin(rotZ);
    float cxz = sin(rotY);
    float cyz = sin(rotX);
    float cyy = cos(rotX);
    float cyx = cyy * cxy + cyz * cxz * cxx;
    cyy = cyy * cxx - cyz * cxz * cxy;
    cyz *= cos(rotY);
    cxx *= cos(rotY);
    cxy *= cos(rotY);

    Painter.setPen(QPen(Qt::red, 2));
    Painter.drawLine(CROSS3D_SIZE, CROSS3D_SIZE,
                     int(CROSS3D_SIZE * (1.0 + cxx)),
                     int(CROSS3D_SIZE * (1.0 - cyx)));
    Painter.setPen(QPen(Qt::green, 2));
    Painter.drawLine(CROSS3D_SIZE, CROSS3D_SIZE,
                     int(CROSS3D_SIZE * (1.0 - cxy)),
                     int(CROSS3D_SIZE * (1.0 - cyy)));
    Painter.setPen(QPen(Qt::blue, 2));
    Painter.drawLine(CROSS3D_SIZE, CROSS3D_SIZE,
                     int(CROSS3D_SIZE * (1.0 + cxz)),
                     int(CROSS3D_SIZE * (1.0 + cyz)));
  };
};

namespace theme = qucs_s::diagramtheme;

// The diagram as the Theme tab's colours draw it, on the paper of its
// schematic: the diagram itself, painted in those colours for the time of
// a paint and put back.
class ThemePreview : public QWidget {
public:
  ThemePreview(Diagram *d, const QColor &canvas, QWidget *parent = nullptr)
      : QWidget(parent), m_diagram(d), m_canvas(canvas) {
    setObjectName(QStringLiteral("diagramThemePreview"));
    setMinimumSize(240, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  void show(const theme::Theme &t) {
    m_theme = t;
    update();
  }

private:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), m_canvas);
    p.setFont(QucsSettings.font);
    // All it draws, with its background's margin, in the middle.
    const QRectF drawn = (m_diagram->paintedRect(QFontMetricsF(p.font())) | QRectF(m_diagram->boundingRect()))
                             .adjusted(-12, -12, 12, 12);
    if (drawn.width() <= 0 || drawn.height() <= 0) return;
    const qreal scale = std::min({width() / drawn.width(), height() / drawn.height(), 1.5});
    p.translate(width() / 2.0, height() / 2.0);
    p.scale(scale, scale);
    p.translate(-drawn.center());
    const theme::Theme kept = m_diagram->theme();
    const bool selected = m_diagram->isSelected;
    m_diagram->setTheme(m_theme);
    m_diagram->isSelected = false;
    {
      const qucs_s::ink::Paper paper(m_canvas);
      m_diagram->paint(&p);
    }
    m_diagram->setTheme(kept);
    m_diagram->isSelected = selected;
  }

  Diagram *m_diagram;
  QColor m_canvas;
  theme::Theme m_theme;
};

// standard colors: blue, red, magenta, green, cyan, yellow, grey, black
static const QRgb DefaultColors[] = {0x0000ff, 0xff0000, 0xff00ff, 0x00ff00,
                                     0x00ffff, 0xffff00, 0x777777, 0x000000};

static const int NumDefaultColors = 8;

DiagramDialog::DiagramDialog(Diagram *d, QWidget *parent, Graph *currentGraph)
    : QDialog(parent) {
  setAttribute(Qt::WA_DeleteOnClose);
  Diag = d;
  copyDiagramGraphs(); // make a copy of all graphs
  if (parent) {
    const Schematic *s = dynamic_cast<const Schematic *>(parent);
    QUCS_ASSERT(s);
    QFileInfo Info(s->getDocName());
    defaultDataSet = Info.absolutePath() + QDir::separator() + s->getDataSet();
  } else {
    defaultDataSet = "unknown";
  }
  setWindowTitle(tr("Edit Diagram Properties"));
  changed = false;
  transfer = false; // have changes be applied ? (used by "Cancel")
  toTake = false;   // double-clicked variable be inserted into graph list ?

  Expr.setPattern("[^\"]+");
  Validator = new QRegularExpressionValidator(Expr, this);
  ValInteger = new QIntValidator(0, 360, this);
  ValDouble = new QDoubleValidator(-1e200, 1e200, 6, this);
  ValDouble->setLocale(QLocale::C);

  QString NameY, NameZ;
  if ((Diag->Name == "Rect") || (Diag->Name == "Curve")) {
    NameY = tr("left Axis");
    NameZ = tr("right Axis");
  } else if (Diag->Name == "Polar") {
    NameY = tr("y-Axis");
  } else if ((Diag->Name == "Smith") || (Diag->Name == "ySmith")) {
    NameY = tr("y-Axis");
  } else if (Diag->Name == "PS") {
    NameY = tr("smith Axis");
    NameZ = tr("polar Axis");
  } else if (Diag->Name == "SP") {
    NameY = tr("polar Axis");
    NameZ = tr("smith Axis");
  } else if (Diag->Name == "Rect3D") {
    NameY = tr("y-Axis");
    NameZ = tr("z-Axis");
  } else if (Diag->Name == "Histogram" || Diag->Name == "Eye" || Diag->Name == "Bathtub") {
    NameY = tr("y-Axis");
  } else if (Diag->Name == "Contour") {
    NameY = tr("y-Axis");
    NameZ = tr("Colour bar");
  }

  all = new QVBoxLayout(this); // to provide necessary size
  QTabWidget *t = new QTabWidget();
  all->addWidget(t);

  // Tab #1 - Data ...........................................................
  QWidget *Tab1 = new QWidget();
  QVBoxLayout *Tab1Layout = new QVBoxLayout();
  Tab1Layout->setSpacing(0);
  Tab1->setLayout(Tab1Layout);

  Label4 = 0; // different types with same content
  yrLabel = 0;
  yAxisBox = 0;
  thicknessSpin = 0;
  thicknessLabel = 0;
  precisionSpin = 0;
  precisionLabel = 0;
  ColorButt = 0;
  hideInvisible = 0;
  rotationX = rotationY = rotationZ = 0;

  QGroupBox *InputGroup = new QGroupBox(tr("Graph Input"));
  QVBoxLayout *InputGroupLayout = new QVBoxLayout();
  InputGroup->setLayout(InputGroupLayout);
  Tab1Layout->addWidget(InputGroup);
  GraphInput = new QLineEdit();
  lblPlotVs = new QLabel(tr("Plot Vs."));
  ChooseXVar = new QComboBox();
  connect(ChooseXVar, SIGNAL(currentIndexChanged(int)), this,
          SLOT(slotPlotVs(int)));
  QHBoxLayout *InpSubHL = new QHBoxLayout();
  InpSubHL->addWidget(GraphInput);
  InpSubHL->addWidget(lblPlotVs);
  InpSubHL->addWidget(ChooseXVar);
  InputGroupLayout->addLayout(InpSubHL);
  GraphInput->setValidator(Validator);
  connect(GraphInput, SIGNAL(textChanged(const QString &)),
          SLOT(slotResetToTake(const QString &)));
  QWidget *Box2 = new QWidget();
  QHBoxLayout *Box2Layout = new QHBoxLayout();
  Box2->setLayout(Box2Layout);
  InputGroupLayout->addWidget(Box2);
  Box2Layout->setSpacing(5);

  // Variable completion for "GraphInput"
  graphCompleter = new QCompleter(this);
  graphCompleter->setCaseSensitivity(Qt::CaseInsensitive);
  graphCompleter->setCompletionMode(QCompleter::PopupCompletion);
  GraphInput->setCompleter(graphCompleter);

  // What of each value a graph shows - a complex one's magnitude (auto),
  // dB, phase, ... - on a Cartesian diagram or in a table.
  if (Graph::valuePartApplies(Diag->Name)) {
    PartLabel = new QLabel(tr("Shows:"));
    Box2Layout->addWidget(PartLabel);
    PartBox = new QComboBox();
    PartBox->setObjectName(QStringLiteral("graphValuePart"));
    PartBox->addItems({tr("auto"), tr("magnitude"), tr("dB"), tr("phase (deg)"), tr("real part"), tr("imaginary part")});
    PartBox->setToolTip(tr("What of each value the graph shows: auto is a complex value's magnitude, and a real one as it is"));
    Box2Layout->addWidget(PartBox);
    connect(PartBox, QOverload<int>::of(&QComboBox::activated), this, &DiagramDialog::slotSetValuePart);
  }

  if (Diag->Name == "Tab") {
    Label1 = new QLabel(tr("Number Notation: "));
    Box2Layout->addWidget(Label1);
    PropertyBox = new QComboBox();
    Box2Layout->addWidget(PropertyBox);
    PropertyBox->addItem(tr("real/imaginary"));
    PropertyBox->addItem(tr("magnitude/angle (degree)"));
    PropertyBox->addItem(tr("magnitude/angle (radian)"));
    PropertyBox->setCurrentIndex(1);
    connect(PropertyBox, QOverload<int>::of(&QComboBox::activated), this,
            &DiagramDialog::slotSetNumMode);
    Box2Layout->setStretchFactor(new QWidget(Box2), 5);

    precisionLabel = new QLabel(tr("Precision:"));
    Box2Layout->addWidget(precisionLabel);
    precisionSpin = new QSpinBox();
    Box2Layout->addWidget(precisionSpin);
    precisionSpin->setMinimum(0);
    precisionSpin->setMaximum(99);
    precisionSpin->setValue(3);
    precisionSpin->setMaximumWidth(60);
    connect(precisionSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &DiagramDialog::slotSetPrecision);

  } else if (Diag->Name != "Truth") {
    Label1 = new QLabel(tr("Color:"));
    Box2Layout->addWidget(Label1);
    ColorButt = new QPushButton("   ");
    Box2Layout->addWidget(ColorButt);
    ColorButt->setMinimumWidth(50);
    ColorButt->setEnabled(false);
    connect(ColorButt, &QPushButton::clicked, this,
            &DiagramDialog::slotSetColor);
    // Auto: a color for each curve of the graph - each value of a
    // parameter swept - instead of one for all.
    if (Graph::autoColorApplies(Diag->Name)) {
      AutoColorBox = new QCheckBox(tr("auto"));
      AutoColorBox->setToolTip(
          tr("Each curve of the graph - every value of a swept parameter - in a color of its own, "
             "named with its values in the legend"));
      AutoColorBox->setEnabled(false);
      Box2Layout->addWidget(AutoColorBox);
      connect(AutoColorBox, &QCheckBox::toggled, this, &DiagramDialog::slotSetAutoColor);
      GhostBox = new QCheckBox(tr("ghost"));
      GhostBox->setObjectName(QStringLiteral("traceGhost"));
      GhostBox->setToolTip(tr("Drawn faint, behind the others: a kept run's beside this one's - a before for an after "
                              "(the variable of a run kept with simulate's keep_as: ngspice/before:tran.v(out))"));
      GhostBox->setEnabled(false);
      Box2Layout->addWidget(GhostBox);
      connect(GhostBox, &QCheckBox::toggled, this, &DiagramDialog::slotSetGhost);
    }

    Box2Layout->setStretchFactor(new QWidget(Box2),
                                 5); // stretchable placeholder

    Label3 = new QLabel(tr("Style:"));
    Box2Layout->addWidget(Label3);
    Label3->setEnabled(false);
    PropertyBox = new QComboBox();
    Box2Layout->addWidget(PropertyBox);
    PropertyBox->addItem(tr("solid line"));
    PropertyBox->addItem(tr("dash line"));
    PropertyBox->addItem(tr("dot line"));
    if (Diag->Name != "Time") {
      PropertyBox->addItem(tr("long dash line"));
      PropertyBox->addItem(tr("stars"));
      PropertyBox->addItem(tr("circles"));
      PropertyBox->addItem(tr("arrows"));
    }
    connect(PropertyBox, QOverload<int>::of(&QComboBox::activated), this,
            &DiagramDialog::slotSetGraphStyle);
    // Point markers on a line graph: none, auto (a shape for each curve),
    // or one shape for all.
    if (Graph::autoColorApplies(Diag->Name)) {
      MarkerLabel = new QLabel(tr("Marker:"));
      Box2Layout->addWidget(MarkerLabel);
      MarkerBox = new QComboBox();
      MarkerBox->addItems({tr("none"), tr("auto"), tr("circle"), tr("square"), tr("triangle"),
                           tr("diamond"), tr("triangle down"), tr("cross"), tr("plus")});
      MarkerBox->setToolTip(tr("A symbol on the data points; auto gives each curve a shape of its own"));
      Box2Layout->addWidget(MarkerBox);
      MarkerLabel->setEnabled(false);
      MarkerBox->setEnabled(false);
      connect(MarkerBox, QOverload<int>::of(&QComboBox::activated), this,
              &DiagramDialog::slotSetPointMarker);
    }
    Box2Layout->setStretchFactor(new QWidget(Box2),
                                 5); // stretchable placeholder

    Box2Layout->setStretchFactor(new QWidget(Box2), 5);

    thicknessLabel = new QLabel(tr("Thickness:"));
    Box2Layout->addWidget(thicknessLabel);
    thicknessSpin = new QSpinBox();
    Box2Layout->addWidget(thicknessSpin);
    thicknessSpin->setMinimum(0);
    thicknessSpin->setMaximum(99);
    thicknessSpin->setValue(
        _settings::Get().item<QString>("DefaultGraphLineWidth").toInt());
    thicknessSpin->setMaximumWidth(60);
    connect(thicknessSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &DiagramDialog::slotSetThickness);

    if ((Diag->Name == "Rect") || (Diag->Name == "PS") ||
        (Diag->Name == "SP") || (Diag->Name == "Curve") || (Diag->Name == "Stacked")) {
      Label4 = new QLabel(Diag->Name == "Stacked" ? tr("Pane:") : tr("y-Axis:"));
      Box2Layout->addWidget(Label4);
      Label4->setEnabled(false);
      yAxisBox = new QComboBox();
      yAxisBox->setObjectName(QStringLiteral("yAxisBox"));
      Box2Layout->addWidget(yAxisBox);
      if (Diag->Name == "Stacked") {
        fillAxisBox();
      } else {
        yAxisBox->addItem(NameY);
        yAxisBox->addItem(NameZ);
      }
      yAxisBox->setEnabled(false);
      connect(yAxisBox, QOverload<int>::of(&QComboBox::activated), this,
              &DiagramDialog::slotSetYAxis);
    }
  }

  if (thicknessSpin) {
    Label1->setEnabled(false);
    PropertyBox->setEnabled(false);
    thicknessLabel->setEnabled(false);
    thicknessSpin->setEnabled(false);
  }

  if (precisionSpin) {
    Label1->setEnabled(false);
    PropertyBox->setEnabled(false);
    precisionLabel->setEnabled(false);
    precisionSpin->setEnabled(false);
  }

  QWidget *Box1 = new QWidget();
  Tab1Layout->addWidget(Box1);
  QHBoxLayout *Box1Layout = new QHBoxLayout();
  Box1->setLayout(Box1Layout);
  Box1Layout->setSpacing(5);
  Box1Layout->setContentsMargins(0, 0, 0, 0);

  QGroupBox *DataGroup = new QGroupBox(tr("Dataset"));
  Box1Layout->addWidget(DataGroup);
  QVBoxLayout *DataGroupLayout = new QVBoxLayout();
  DataGroup->setLayout(DataGroupLayout);
  ChooseData = new QComboBox();
  ChooseData->setObjectName(QStringLiteral("diagramDataset"));
  DataGroupLayout->addWidget(ChooseData);
  ChooseData->setMinimumWidth(300); // will force also min width of table below
  connect(ChooseData, QOverload<int>::of(&QComboBox::activated), this,
          &DiagramDialog::slotReadVarsAndSetSimulator);

  // connect(ChooseData,
  // SIGNAL(currentIndexChanged(int)),this,SLOT(slotSetSimulator())); todo:
  // replace by QTableWidget see https://gist.github.com/ClemensFMN/8955411

  QHBoxLayout *hb1 = new QHBoxLayout;
  ChooseSimulator = new QComboBox;
  ChooseSimulator->setObjectName(QStringLiteral("diagramSimulator"));
  QStringList lst_sim;
  lst_sim << "Qucsator" << "Ngspice" << "Xyce" << "SpiceOpus";
  ChooseSimulator->addItems(lst_sim);
  connect(ChooseSimulator, &QComboBox::currentIndexChanged, this,
          &DiagramDialog::slotReadVars);
  lblSim = new QLabel(tr("Data from simulator:"));
  hb1->addWidget(lblSim);
  hb1->addWidget(ChooseSimulator);
  DataGroupLayout->addLayout(hb1);

  ChooseVars = new QTableWidget(1, 3);
  ChooseVars->setObjectName(QStringLiteral("diagramVariables"));
  ChooseVars->verticalHeader()->setVisible(false);
  ChooseVars->horizontalHeader()->setStretchLastSection(true);
  ChooseVars->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  // make sure sorting is disabled before inserting items
  ChooseVars->setSortingEnabled(false);
  ChooseVars->horizontalHeader()->setSortIndicatorShown(true);
  ChooseVars->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);

  ChooseVars->setSelectionBehavior(QAbstractItemView::SelectRows);
  // ChooseVars->selectRow(0);
  DataGroupLayout->addWidget(ChooseVars);
  // ChooseVars->addColumn(tr("Name"));
  // ChooseVars->addColumn(tr("Type"));
  // ChooseVars->addColumn(tr("Size"));
  QStringList headers;
  headers << tr("Name") << tr("Type") << tr("Size");
  ChooseVars->setHorizontalHeaderLabels(headers);

  connect(ChooseVars, &QTableWidget::itemDoubleClicked, this,
          &DiagramDialog::slotTakeVar);

  QGroupBox *GraphGroup = new QGroupBox(tr("Graph"));
  Box1Layout->addWidget(GraphGroup);
  QVBoxLayout *GraphGroupLayout = new QVBoxLayout();
  GraphGroup->setLayout(GraphGroupLayout);

  // GraphList displays the trace properties along the trace name. This helps
  // the user to identify the visual properties of the trace such as color,
  // thickness, style and the y-axis
  GraphList = new QTableWidget();
  GraphList->setObjectName(QStringLiteral("diagramGraphs"));

  // Determine which columns to show based on diagram type
  // Tabular data and truth tables doesn't contain traces, so it makes no sense
  // to show properties like the width and color, etc.
  bool showTraceProperties = (Diag->Name != "Tab" && Diag->Name != "Truth");

  if (showTraceProperties) {
    GraphList->setColumnCount(5);
    QStringList graphHeaders;
    graphHeaders << tr("Variable") << tr("Color") << tr("Style") << tr("Thick")
                 << tr("y-Axis");
    GraphList->setHorizontalHeaderLabels(graphHeaders);
  } else {
    // Tabular data and truth tables
    GraphList->setColumnCount(1);
    QStringList graphHeaders;
    graphHeaders << tr("Variable");
    GraphList->setHorizontalHeaderLabels(graphHeaders);
  }
  GraphList->verticalHeader()->setVisible(false);
  GraphList->setSelectionBehavior(QAbstractItemView::SelectRows);
  GraphList->setSelectionMode(QAbstractItemView::SingleSelection);
  GraphList->setSortingEnabled(false);
  GraphList->setEditTriggers(QAbstractItemView::NoEditTriggers);

  // Set column resize modes for compact display
  GraphList->horizontalHeader()->setSectionResizeMode(
      0, QHeaderView::ResizeToContents); // Variable column stretches

  if (showTraceProperties) {
    // Plots with traces (not tabular)
    GraphList->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch); // Color
    GraphList->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch); // Style
    GraphList->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Stretch); // Thick
    GraphList->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Stretch);     // y-Axis
    GraphList->setColumnWidth(1, 40); // Fixed width for color swatch
  }

  // Set resize policy
  GraphList->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  // Set size policy to allow it to shrink
  GraphList->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);

  GraphGroupLayout->addWidget(GraphList);

  Box1Layout->addWidget(DataGroup, 0);  // No stretch
  Box1Layout->addWidget(GraphGroup, 1); // Stretch factor 1 (takes extra space)

  connect(GraphList, &QTableWidget::itemClicked, this,
          &DiagramDialog::slotSelectGraph);

  connect(GraphList, &QTableWidget::itemDoubleClicked, this,
          &DiagramDialog::slotDeleteGraph);

  QPushButton *NewButt = new QPushButton(tr("New Graph"));
  GraphGroupLayout->addWidget(NewButt);
  connect(NewButt, &QPushButton::clicked, this, &DiagramDialog::slotNewGraph);

  QPushButton *DelButt = new QPushButton(tr("Delete Graph"));
  GraphGroupLayout->addWidget(DelButt);
  connect(DelButt, &QPushButton::clicked, this,
          &DiagramDialog::slotDeleteGraph);

  t->addTab(Tab1, tr("Data"));

  // Tab #2...........................................................
  int Row = 0;
  if (Diag->Name.at(0) != 'T') { // not tabular or timing diagram
    QWidget *Tab2 = new QWidget(t);
    // QGridLayout *gp = new QGridLayout(Tab2,13,3,5,5);
    QGridLayout *gp = new QGridLayout(Tab2);

    gp->addWidget(new QLabel(tr("Title:"), Tab2), Row, 0);
    titleEdit = new QLineEdit(Tab2);
    titleEdit->setObjectName(QStringLiteral("diagramTitle"));
    titleEdit->setValidator(Validator);
    titleEdit->setToolTip(tr("Drawn above the diagram, and moved and exported with it"));
    gp->addWidget(titleEdit, Row, 1);
    Row++;

    gp->addWidget(new QLabel(tr("x-Axis Label:"), Tab2), Row, 0);
    xLabel = new QLineEdit(Tab2);
    xLabel->setValidator(Validator);
    gp->addWidget(xLabel, Row, 1);
    Row++;

    ylLabelName = new QLabel(NameY + " " + tr("Label:"), Tab2);
    gp->addWidget(ylLabelName, Row, 0);
    ylLabel = new QLineEdit(Tab2);
    ylLabel->setValidator(Validator);
    gp->addWidget(ylLabel, Row, 1);
    Row++;

    if ((Diag->Name != "Smith") && (Diag->Name != "Polar") && (Diag->Name != "Histogram") && (Diag->Name != "Eye")
        && (Diag->Name != "Bathtub")) {
      yrLabelName = new QLabel(NameZ + " " + tr("Label:"), Tab2);
      gp->addWidget(yrLabelName, Row, 0);
      yrLabel = new QLineEdit(Tab2);
      yrLabel->setValidator(Validator);
      gp->addWidget(yrLabel, Row, 1);
      Row++;
    }

    gp->addWidget(new QLabel(tr("<b>Label text</b>: Use LaTeX style for "
                                "special characters, e.g. \\tau"),
                             Tab2),
                  Row, 0, 1, 2);
    Row++;

    if (Diag->Name != "Rect3D") {
      GridOn = new QCheckBox(tr("show Grid"), Tab2);
      gp->addWidget(GridOn, Row, 0);
      Row++;

      // (Its colour: on the Theme tab, with the other parts'.)
      GridLabel2 = new QLabel(tr("Grid Style: "), Tab2);
      gp->addWidget(GridLabel2, Row, 0);
      GridStyleBox = new QComboBox(Tab2);
      GridStyleBox->addItem(tr("solid line"));
      GridStyleBox->addItem(tr("dash line"));
      GridStyleBox->addItem(tr("dot line"));
      GridStyleBox->addItem(tr("dash dot line"));
      GridStyleBox->addItem(tr("dash dot dot line"));
      gp->addWidget(GridStyleBox, Row, 1);
      Row++;
      GridStyleBox->setCurrentIndex(Diag->GridPen.style() - 1);

      GridOn->setChecked(Diag->xAxis.GridOn);
      if (!Diag->xAxis.GridOn)
        slotSetGridBox(0);
      connect(GridOn, &QCheckBox::stateChanged, this,
              &DiagramDialog::slotSetGridBox);
    } else {
      GridOn = 0;
      GridStyleBox = 0;
      NotationBox = 0;
    }

    NotationLabel = new QLabel(tr("Number notation: "), Tab2);
    gp->addWidget(NotationLabel, Row, 0);
    // The notations, each with examples; the item's data is the value the
    // diagram keeps (numberformat::Notation).
    NotationBox = new QComboBox(Tab2);
    for (const auto& [notation, text] : qucs_s::numberformat::choices())
      NotationBox->addItem(text, int(notation));
    NotationBox->setCurrentIndex(std::max(0, NotationBox->findData(int(Diag->notation))));
    gp->addWidget(NotationBox, Row, 1);
    Row++;

    gp->addWidget(new QLabel(tr("Decimal places: "), Tab2), Row, 0);
    DecimalsBox = new QSpinBox(Tab2);
    DecimalsBox->setRange(-1, 15);
    DecimalsBox->setSpecialValueText(tr("auto"));   // -1: as many as each number needs
    DecimalsBox->setValue(Diag->notationDecimals);
    DecimalsBox->setToolTip(tr("Places after the point of the numbers on the axes and in the cursor "
                               "readout (of the mantissa, with an exponent or a prefix). Auto: as many "
                               "as they need; decimal labels as many as the grid step needs."));
    gp->addWidget(DecimalsBox, Row, 1);
    Row++;

    // The legend: off, or in one of the corners (the order of the entries
    // is that of Diagram::LegendPosition).
    gp->addWidget(new QLabel(tr("Legend: "), Tab2), Row, 0);
    LegendBox = new QComboBox(Tab2);
    LegendBox->addItem(tr("none"));
    LegendBox->addItem(tr("top left"));
    LegendBox->addItem(tr("top right"));
    LegendBox->addItem(tr("bottom left"));
    LegendBox->addItem(tr("bottom right"));
    LegendBox->setCurrentIndex(Diag->legendPos);
    gp->addWidget(LegendBox, Row, 1);
    Row++;

    // A histogram: its bins and what goes with them.
    if (auto *hist = dynamic_cast<HistogramDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Histogram"), Tab2);
      QGridLayout *hl = new QGridLayout(box);
      hl->addWidget(new QLabel(tr("Bins:")), 0, 0);
      HistBins = new QSpinBox();
      HistBins->setRange(0, 1000);
      HistBins->setSpecialValueText(tr("automatic"));   // 0: Freedman-Diaconis
      HistBins->setValue(hist->bins);
      HistBins->setToolTip(tr("The number of bins over the values (or the x-axis' manual limits); "
                              "automatic: by the spread of the values (Freedman-Diaconis)"));
      hl->addWidget(HistBins, 0, 1);
      hl->addWidget(new QLabel(tr("Height:")), 1, 0);
      HistHeight = new QComboBox();
      HistHeight->addItem(tr("count"));
      HistHeight->addItem(tr("percent"));
      HistHeight->addItem(tr("probability density"));
      HistHeight->setCurrentIndex(hist->height);
      hl->addWidget(HistHeight, 1, 1);
      HistFit = new QCheckBox(tr("normal distribution of the same mean and deviation"));
      HistFit->setChecked(hist->normalFit);
      hl->addWidget(HistFit, 2, 0, 1, 2);
      HistStats = new QCheckBox(tr("statistics: number, mean and deviation"));
      HistStats->setChecked(hist->statistics);
      hl->addWidget(HistStats, 3, 0, 1, 2);
      auto limitEdit = [](double value) {
        auto *e = new QLineEdit(std::isfinite(value) ? misc::num2str(value, -1, QString()) : QString());
        e->setPlaceholderText(tr("none"));
        return e;
      };
      hl->addWidget(new QLabel(tr("Lower limit:")), 4, 0);
      HistLower = limitEdit(hist->lowerLimit);
      hl->addWidget(HistLower, 4, 1);
      hl->addWidget(new QLabel(tr("Upper limit:")), 5, 0);
      HistUpper = limitEdit(hist->upperLimit);
      hl->addWidget(HistUpper, 5, 1);
      HistLower->setToolTip(tr("A line at a spec limit, and the share of the values between the two "
                               "in the statistics (1.432k, 2e-3)"));
      HistUpper->setToolTip(HistLower->toolTip());
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // An eye diagram: the unit interval, the windows, the levels, the mask.
    if (auto *eyeDiagram = dynamic_cast<EyeDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Eye"), Tab2);
      QGridLayout *el = new QGridLayout(box);
      // (What a field showed: read again only when it was changed, as one
      // written back would lose digits.)
      auto valueEdit = [](double value, const QString &unit, const QString &empty) {
        // ("333.333 ps"; a value without a unit "450m".)
        auto *e = new QLineEdit(!std::isfinite(value) ? QString()
                                : unit.isEmpty()      ? misc::num2str(value, -1, QString())
                                                      : qucs_s::units::engineering(value, unit));
        e->setPlaceholderText(empty);
        e->setProperty("qucsShown", e->text());
        return e;
      };
      int r = 0;
      el->addWidget(new QLabel(tr("Unit interval:")), r, 0);
      EyeUi = valueEdit(eyeDiagram->ui, "s", tr("the PRBS source's Tbit, or from the crossings"));
      EyeUi->setToolTip(tr("A bit's length (a PAM4 symbol's): 100 ps, 1n. Empty: the Tbit of the V(PRBS) "
                           "source the first graph comes from - the nearest to its node, never through ground "
                           "- or, with none, told from where it crosses its threshold"));
      el->addWidget(EyeUi, r++, 1);
      el->addWidget(new QLabel(tr("Across it:")), r, 0);
      EyeSpan = new QSpinBox();
      EyeSpan->setRange(1, EyeDiagram::MaxSpan);
      EyeSpan->setSuffix(tr(" UI"));
      EyeSpan->setValue(eyeDiagram->span);
      EyeSpan->setToolTip(tr("How many unit intervals each trace is long: 2 shows an eye in the middle, "
                             "half an eye either side"));
      el->addWidget(EyeSpan, r++, 1);
      el->addWidget(new QLabel(tr("From:")), r, 0);
      EyeStart = valueEdit(eyeDiagram->start, "s", tr("the start"));
      EyeStart->setToolTip(tr("The eye from this time on, the settling before it left out (2 ns)"));
      el->addWidget(EyeStart, r++, 1);
      el->addWidget(new QLabel(tr("Levels:")), r, 0);
      EyeLevels = new QComboBox();
      EyeLevels->addItem(tr("as the PRBS source is coded"));
      EyeLevels->addItem(tr("2 (NRZ)"));
      EyeLevels->addItem(tr("4 (PAM4)"));
      EyeLevels->setCurrentIndex(eyeDiagram->levels == 4 ? 2 : eyeDiagram->levels == 2 ? 1 : 0);
      EyeLevels->setToolTip(tr("As coded: 4 for a graph whose V(PRBS) source is coded PAM4, else 2"));
      el->addWidget(EyeLevels, r++, 1);
      el->addWidget(new QLabel(tr("Threshold:")), r, 0);
      EyeThreshold = valueEdit(eyeDiagram->threshold, "", tr("halfway between the levels"));
      EyeThreshold->setToolTip(tr("Where a bit is told 0 or 1 (0.5, 450m). PAM4's thresholds are halfway "
                                  "between its levels"));
      EyeThreshold->setEnabled(eyeDiagram->levels != 4);
      connect(EyeLevels, &QComboBox::currentIndexChanged, EyeThreshold,
              [this](int index) { EyeThreshold->setEnabled(index != 2); });
      el->addWidget(EyeThreshold, r++, 1);
      el->addWidget(new QLabel(tr("Drawn as:")), r, 0);
      EyeDrawn = new QComboBox();
      EyeDrawn->addItem(tr("density: how many traces pass"));
      EyeDrawn->addItem(tr("traces"));
      EyeDrawn->setCurrentIndex(eyeDiagram->drawn);
      el->addWidget(EyeDrawn, r++, 1);
      EyeMeasure = new QCheckBox(tr("measurements beside it; its height and width marked"));
      EyeMeasure->setChecked(eyeDiagram->measurements);
      el->addWidget(EyeMeasure, r++, 0, 1, 2);
      el->addWidget(new QLabel(tr("Mask width:")), r, 0);
      EyeMaskWidth = valueEdit(eyeDiagram->maskWidth, "", tr("no mask"));
      EyeMaskWidth->setToolTip(tr("A hexagon at the eye's centre that no trace should enter: its width "
                                  "in UI, 0 to 1 (0.5)"));
      el->addWidget(EyeMaskWidth, r++, 1);
      el->addWidget(new QLabel(tr("Mask height:")), r, 0);
      EyeMaskHeight = valueEdit(eyeDiagram->maskHeight, "", tr("no mask"));
      EyeMaskHeight->setToolTip(tr("... and its height, in the unit of the signal (0.2, 200m)"));
      el->addWidget(EyeMaskHeight, r++, 1);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A stacked diagram: its panes, each with its own y axes; the x axis'
    // scale.
    if (auto *stacked = dynamic_cast<StackedDiagram *>(Diag)) {
      for (QWidget *w : {static_cast<QWidget *>(ylLabelName), static_cast<QWidget *>(ylLabel),
                         static_cast<QWidget *>(yrLabelName), static_cast<QWidget *>(yrLabel)})
        if (w) w->setVisible(false);
      QGroupBox *box = new QGroupBox(tr("Panes"), Tab2);
      QGridLayout *pl = new QGridLayout(box);
      pl->addWidget(new QLabel(tr("Panes:")), 0, 0);
      PaneCount = new QSpinBox();
      PaneCount->setObjectName(QStringLiteral("paneCount"));
      PaneCount->setRange(1, StackedDiagram::MaxPanes);
      PaneCount->setValue(stacked->paneCount());
      PaneCount->setToolTip(tr("How many panes, one above the other on the one x axis: each trace in its own "
                               "(the y-Axis box beside its colour), each pane with its own y axes"));
      PaneCount->setEnabled(!dynamic_cast<BodeDiagram *>(Diag));   // (a Bode diagram's: the magnitude and the phase)
      pl->addWidget(PaneCount, 0, 1);
      GridLogX = new QCheckBox(tr("logarithmic X Axis Grid"));
      GridLogX->setChecked(Diag->xAxis.log);
      pl->addWidget(GridLogX, 0, 2);
      PaneTable = new QTableWidget(0, 8);
      PaneTable->setObjectName(QStringLiteral("paneTable"));
      PaneTable->setHorizontalHeaderLabels({tr("left label"), tr("from"), tr("to"), tr("log"), tr("right label"),
                                            tr("from"), tr("to"), tr("log")});
      PaneTable->setToolTip(tr("Each pane's axes, the top one first: a label (empty: its traces' names), "
                               "its limits (both empty: automatic) and whether it is logarithmic"));
      // The labels as wide as there is room, the limits room for a number.
      for (int c : {0, 4}) PaneTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
      for (int c : {1, 2, 5, 6}) PaneTable->setColumnWidth(c, 70);
      for (int c : {3, 7}) PaneTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
      PaneTable->setMinimumHeight(120);
      setPaneRows(stacked->paneCount());
      for (int i = 0; i < stacked->paneCount(); ++i) {
        int c = 0;
        for (const Axis *a : {&stacked->pane(i).left, &stacked->pane(i).right}) {
          PaneTable->item(i, c)->setText(a->Label);
          PaneTable->item(i, c + 1)->setText(a->autoScale ? QString() : QString::number(a->limit_min));
          PaneTable->item(i, c + 2)->setText(a->autoScale ? QString() : QString::number(a->limit_max));
          PaneTable->item(i, c + 3)->setCheckState(a->log ? Qt::Checked : Qt::Unchecked);
          c += 4;
        }
      }
      pl->addWidget(PaneTable, 1, 0, 1, 3);
      connect(PaneCount, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int n) {
        setPaneRows(n);
        // The traces in a pane no longer there go to the last.
        for (int i = 0; i < int(Graphs.size()); ++i)
          if (Graphs.at(i)->pane >= n) {
            Graphs.at(i)->pane = n - 1;
            updateGraphListItem(i);
          }
        const int at = yAxisBox ? yAxisBox->currentIndex() : -1;
        fillAxisBox();
        if (yAxisBox) yAxisBox->setCurrentIndex(std::min(at, yAxisBox->count() - 1));
        changed = true;
      });
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A Bode diagram: its crossovers and margins.
    if (auto *bode = dynamic_cast<BodeDiagram *>(Diag)) {
      BodeMargins = new QCheckBox(tr("crossovers and margins marked"), Tab2);
      BodeMargins->setObjectName(QStringLiteral("bodeMargins"));
      BodeMargins->setToolTip(tr("Where each loop gain falls through 0 dB and its phase through -180\u00B0, with the phase margin "
                                 "and the gain margin"));
      BodeMargins->setChecked(bode->margins);
      gp->addWidget(BodeMargins, Row, 0, 1, 2);
      Row++;
    }

    // A Nichols chart: its contours. A polar diagram: a Nyquist plot's marks.
    if (auto *nichols = dynamic_cast<NicholsDiagram *>(Diag)) {
      NicholsGrid = new QCheckBox(tr("M and N contours of the closed loop"), Tab2);
      NicholsGrid->setObjectName(QStringLiteral("nicholsGrid"));
      NicholsGrid->setChecked(nichols->grid);
      gp->addWidget(NicholsGrid, Row, 0, 1, 2);
      Row++;
    }
    if (auto *polar = dynamic_cast<PolarDiagram *>(Diag)) {
      NyquistMarks = new QCheckBox(tr("Nyquist: the critical point -1 and the unit circle"), Tab2);
      NyquistMarks->setObjectName(QStringLiteral("nyquistMarks"));
      NyquistMarks->setChecked(polar->nyquist);
      gp->addWidget(NyquistMarks, Row, 0, 1, 2);
      Row++;
      NyquistMirror = new QCheckBox(tr("Nyquist: the negative frequencies too (mirrored, dashed)"), Tab2);
      NyquistMirror->setObjectName(QStringLiteral("nyquistMirror"));
      NyquistMirror->setChecked(polar->mirror);
      gp->addWidget(NyquistMirror, Row, 0, 1, 2);
      Row++;
    }

    // A spectrum view: its window, harmonics, units and lines.
    if (auto *spectrum = dynamic_cast<SpectrumDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Spectrum"), Tab2);
      QGridLayout *sl = new QGridLayout(box);
      int r = 0;
      sl->addWidget(new QLabel(tr("Window:")), r, 0);
      SpecWindow = new QComboBox();
      SpecWindow->setObjectName(QStringLiteral("spectrumWindow"));
      for (const QString &w : qucs_s::spectrum::windowNames()) SpecWindow->addItem(QString(w).replace('_', ' '));
      SpecWindow->setCurrentIndex(int(spectrum->window));
      SpecWindow->setToolTip(tr("Hann for most; flat top reads a line's amplitude best; Blackman-Harris shows the "
                                "lowest spurs; rectangular for a signal of whole periods"));
      sl->addWidget(SpecWindow, r++, 1);
      sl->addWidget(new QLabel(tr("Harmonics:")), r, 0);
      SpecHarmonics = new QSpinBox();
      SpecHarmonics->setRange(2, 50);
      SpecHarmonics->setValue(spectrum->harmonics);
      SpecHarmonics->setToolTip(tr("The highest harmonic numbered and counted in THD"));
      sl->addWidget(SpecHarmonics, r++, 1);
      SpecDbc = new QCheckBox(tr("in dBc (the fundamental at 0)"));
      SpecDbc->setChecked(spectrum->dbc);
      sl->addWidget(SpecDbc, r++, 0, 1, 2);
      SpecStems = new QCheckBox(tr("a stem for each line"));
      SpecStems->setChecked(spectrum->stems);
      sl->addWidget(SpecStems, r++, 0, 1, 2);
      const auto edit = [](double v, const QString &empty) {
        auto *e = new QLineEdit(std::isfinite(v) ? misc::num2str(v, -1, QString()) : QString());
        e->setPlaceholderText(empty);
        return e;
      };
      sl->addWidget(new QLabel(tr("From:")), r, 0);
      SpecFrom = edit(spectrum->from, tr("the start"));
      SpecFrom->setToolTip(tr("The signal from this time on, its settling left out (1m)"));
      sl->addWidget(SpecFrom, r++, 1);
      sl->addWidget(new QLabel(tr("Fundamental:")), r, 0);
      SpecFundamental = edit(spectrum->fundamental, tr("the strongest line"));
      SpecFundamental->setToolTip(tr("Its frequency in Hz (1k), or empty: the strongest line"));
      sl->addWidget(SpecFundamental, r++, 1);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A bathtub curve: its traces folded as an eye's, the rate its
    // opening is measured at, the axis' floor.
    if (auto *tub = dynamic_cast<BathtubDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Bathtub"), Tab2);
      QGridLayout *bl = new QGridLayout(box);
      int r = 0;
      const auto edit = [](double v, const QString &empty, const char *name) {
        auto *e = new QLineEdit(std::isfinite(v) ? misc::num2str(v, -1, QString()) : QString());
        e->setPlaceholderText(empty);
        e->setObjectName(QLatin1String(name));
        return e;
      };
      bl->addWidget(new QLabel(tr("Unit interval:")), r, 0);
      TubUi = edit(tub->ui, tr("the PRBS source's Tbit, else the crossings'"), "bathtubUi");
      TubUi->setToolTip(tr("A bit's length (100p), as the eye diagram takes it"));
      bl->addWidget(TubUi, r++, 1);
      bl->addWidget(new QLabel(tr("From:")), r, 0);
      TubFrom = edit(tub->start, tr("the start"), "bathtubFrom");
      TubFrom->setToolTip(tr("The signal from this time on, its settling left out (2n)"));
      bl->addWidget(TubFrom, r++, 1);
      bl->addWidget(new QLabel(tr("Levels:")), r, 0);
      TubLevels = new QComboBox();
      TubLevels->setObjectName(QStringLiteral("bathtubLevels"));
      TubLevels->addItems({tr("as the source is coded"), tr("2 (NRZ)"), tr("4 (PAM4)")});
      TubLevels->setCurrentIndex(tub->levels == 2 ? 1 : tub->levels == 4 ? 2 : 0);
      bl->addWidget(TubLevels, r++, 1);
      bl->addWidget(new QLabel(tr("Threshold:")), r, 0);
      TubThreshold = edit(tub->threshold, tr("halfway between the levels"), "bathtubThreshold");
      bl->addWidget(TubThreshold, r++, 1);
      bl->addWidget(new QLabel(tr("Bit error rate:")), r, 0);
      TubBer = edit(tub->ber, QStringLiteral("1e-12"), "bathtubBer");
      TubBer->setToolTip(tr("The rate the opening and the total jitter are measured at, at most 0.01"));
      bl->addWidget(TubBer, r++, 1);
      bl->addWidget(new QLabel(tr("Down to:")), r, 0);
      TubFloor = edit(tub->floor, tr("the rate over 10000"), "bathtubFloor");
      TubFloor->setToolTip(tr("The rate the axis goes down to, below the one measured at"));
      bl->addWidget(TubFloor, r++, 1);
      TubMeasured = new QCheckBox(tr("the crossings counted, too"));
      TubMeasured->setObjectName(QStringLiteral("bathtubMeasured"));
      TubMeasured->setToolTip(tr("The rate the crossings give, down to one in their number, beside the model's"));
      TubMeasured->setChecked(tub->measured);
      bl->addWidget(TubMeasured, r++, 0, 1, 2);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A Smith chart: its two-port's circles.
    if (auto *smith = dynamic_cast<SmithDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Circles"), Tab2);
      QGridLayout *sl = new QGridLayout(box);
      sl->addWidget(new QLabel(tr("Circles:")), 0, 0);
      SmithCircles = new QLineEdit(SmithDiagram::circlesText(smith->circles));
      SmithCircles->setObjectName(QStringLiteral("smithCircles"));
      SmithCircles->setPlaceholderText(tr("in, out, gain 12, noise 2"));
      SmithCircles->setToolTip(tr("Of the two-port the traces' run is of (its S-parameters): in and out, its stability circles; "
                                  "gain <dB>, available gain; noise <dB>, noise figure (the run's Fmin, Sopt, Rn)"));
      sl->addWidget(SmithCircles, 0, 1);
      sl->addWidget(new QLabel(tr("At:")), 1, 0);
      SmithFrequency = new QLineEdit(std::isfinite(smith->circleFrequency) ? misc::num2str(smith->circleFrequency, -1, QString()) : QString());
      SmithFrequency->setObjectName(QStringLiteral("smithFrequency"));
      SmithFrequency->setPlaceholderText(tr("the middle of the sweep"));
      sl->addWidget(SmithFrequency, 1, 1);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A constellation: its symbols' sampling and modulation.
    if (auto *iq = dynamic_cast<ConstellationDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Constellation"), Tab2);
      QGridLayout *cl = new QGridLayout(box);
      const auto edit = [](double v, const QString &empty, const char *name) {
        auto *e = new QLineEdit(std::isfinite(v) ? misc::num2str(v, -1, QString()) : QString());
        e->setPlaceholderText(empty);
        e->setObjectName(QLatin1String(name));
        return e;
      };
      int r = 0;
      cl->addWidget(new QLabel(tr("Symbol period:")), r, 0);
      IqPeriod = edit(iq->period, tr("every sample"), "constellationPeriod");
      cl->addWidget(IqPeriod, r++, 1);
      cl->addWidget(new QLabel(tr("First sample:")), r, 0);
      IqOffset = edit(iq->offset, tr("half a period in"), "constellationOffset");
      cl->addWidget(IqOffset, r++, 1);
      cl->addWidget(new QLabel(tr("From:")), r, 0);
      IqFrom = edit(iq->from, tr("the start"), "constellationFrom");
      cl->addWidget(IqFrom, r++, 1);
      cl->addWidget(new QLabel(tr("Modulation:")), r, 0);
      IqModulation = new QComboBox();
      IqModulation->setObjectName(QStringLiteral("constellationModulation"));
      IqModulation->addItems({tr("none"), QStringLiteral("BPSK"), QStringLiteral("QPSK"), QStringLiteral("8-PSK"), QStringLiteral("16-QAM"), QStringLiteral("64-QAM")});
      IqModulation->setCurrentIndex(iq->modulation);
      cl->addWidget(IqModulation, r++, 1);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A box plot: where each curve's value is taken, its whiskers.
    if (auto *boxes = dynamic_cast<BoxPlotDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Box plot"), Tab2);
      QGridLayout *bl = new QGridLayout(box);
      bl->addWidget(new QLabel(tr("Curves at x:")), 0, 0);
      BoxAt = new QLineEdit(std::isfinite(boxes->at) ? misc::num2str(boxes->at, -1, QString()) : QString());
      BoxAt->setObjectName(QStringLiteral("boxPlotAt"));
      BoxAt->setPlaceholderText(tr("their last (the final values)"));
      BoxAt->setToolTip(tr("Of a trace of several curves (a sweep, Monte Carlo runs): each curve's value at this x"));
      bl->addWidget(BoxAt, 0, 1);
      BoxRange = new QCheckBox(tr("whiskers to the lowest and highest (no outliers)"));
      BoxRange->setObjectName(QStringLiteral("boxPlotRange"));
      BoxRange->setChecked(boxes->range);
      bl->addWidget(BoxRange, 1, 0, 1, 2);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A tornado chart: its bars' values, how many, a sensitivity run's parts.
    if (auto *tornado = dynamic_cast<TornadoDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Tornado chart"), Tab2);
      QGridLayout *tl = new QGridLayout(box);
      int r = 0;
      tl->addWidget(new QLabel(tr("Bars:")), r, 0);
      TornadoMode = new QComboBox();
      TornadoMode->setObjectName(QStringLiteral("tornadoMode"));
      TornadoMode->addItems({tr("each trace's value at a point (a sensitivity)"), tr("each trace's spread (corners, Monte Carlo)")});
      TornadoMode->setCurrentIndex(tornado->mode);
      tl->addWidget(TornadoMode, r++, 1);
      tl->addWidget(new QLabel(tr("At:")), r, 0);
      TornadoAt = new QLineEdit(std::isfinite(tornado->at) ? misc::num2str(tornado->at, -1, QString()) : QString());
      TornadoAt->setObjectName(QStringLiteral("tornadoAt"));
      TornadoAt->setPlaceholderText(tr("the sweep's first point"));
      tl->addWidget(TornadoAt, r++, 1);
      tl->addWidget(new QLabel(tr("At most:")), r, 0);
      TornadoBars = new QSpinBox();
      TornadoBars->setObjectName(QStringLiteral("tornadoBars"));
      TornadoBars->setRange(1, TornadoDiagram::MaxBars);
      TornadoBars->setValue(tornado->bars);
      tl->addWidget(TornadoBars, r++, 1);
      auto *parts = new QPushButton(tr("Add the parts of a sensitivity run"));
      parts->setObjectName(QStringLiteral("tornadoParts"));
      parts->setToolTip(tr("Of the run chosen on the Data tab (ngspice's .SENS): a trace for each part - its _scale "
                           "(the output's change per 100 % of its value) where it has one, else its derivative"));
      connect(parts, &QPushButton::clicked, this, &DiagramDialog::addSensitivityParts);
      tl->addWidget(parts, r++, 0, 1, 2);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A spectrogram: its window, segments and range.
    if (auto *gram = dynamic_cast<SpectrogramDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Spectrogram"), Tab2);
      QGridLayout *gl = new QGridLayout(box);
      int r = 0;
      gl->addWidget(new QLabel(tr("Window:")), r, 0);
      GramWindow = new QComboBox();
      GramWindow->setObjectName(QStringLiteral("spectrogramWindow"));
      for (const QString &w : qucs_s::spectrum::windowNames()) GramWindow->addItem(QString(w).replace('_', ' '));
      GramWindow->setCurrentIndex(int(gram->window));
      gl->addWidget(GramWindow, r++, 1);
      gl->addWidget(new QLabel(tr("Segment:")), r, 0);
      GramSegment = new QLineEdit(std::isfinite(gram->segment) ? misc::num2str(gram->segment, -1, QString()) : QString());
      GramSegment->setObjectName(QStringLiteral("spectrogramSegment"));
      GramSegment->setPlaceholderText(tr("a sixteenth of the transient"));
      GramSegment->setToolTip(tr("The seconds each column is the spectrum of (1m)"));
      gl->addWidget(GramSegment, r++, 1);
      gl->addWidget(new QLabel(tr("Overlap:")), r, 0);
      GramOverlap = new QDoubleSpinBox();
      GramOverlap->setObjectName(QStringLiteral("spectrogramOverlap"));
      GramOverlap->setRange(0, 0.9);
      GramOverlap->setSingleStep(0.25);
      GramOverlap->setValue(gram->overlap);
      gl->addWidget(GramOverlap, r++, 1);
      gl->addWidget(new QLabel(tr("Range (dB):")), r, 0);
      GramRange = new QDoubleSpinBox();
      GramRange->setObjectName(QStringLiteral("spectrogramRange"));
      GramRange->setRange(1, 400);
      GramRange->setValue(gram->range);
      GramRange->setToolTip(tr("The colours over this many dB below the loudest (when the colour bar's range is automatic)"));
      gl->addWidget(GramRange, r++, 1);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A contour map: its iso-lines, colours, labels and pass band.
    if (auto *contour = dynamic_cast<ContourDiagram *>(Diag)) {
      QGroupBox *box = new QGroupBox(tr("Contour map"), Tab2);
      QGridLayout *cl = new QGridLayout(box);
      int r = 0;
      cl->addWidget(new QLabel(tr("Iso-lines:")), r, 0);
      MapLevels = new QSpinBox();
      MapLevels->setObjectName(QStringLiteral("contourLevels"));
      MapLevels->setRange(0, ContourDiagram::MaxLevels);
      MapLevels->setValue(contour->levels);
      MapLevels->setToolTip(tr("About how many, at round values across the colour range (0: none)"));
      cl->addWidget(MapLevels, r++, 1);
      cl->addWidget(new QLabel(tr("Colours:")), r, 0);
      MapColours = new QComboBox();
      MapColours->setObjectName(QStringLiteral("contourMap"));
      MapColours->addItems({tr("viridis"), tr("turbo"), tr("grey")});
      MapColours->setCurrentIndex(contour->map);
      cl->addWidget(MapColours, r++, 1);
      MapFilled = new QCheckBox(tr("the first trace in colour"));
      MapFilled->setObjectName(QStringLiteral("contourFilled"));
      MapFilled->setChecked(contour->filled);
      cl->addWidget(MapFilled, r++, 0, 1, 2);
      MapLabels = new QCheckBox(tr("the iso-lines' values on them"));
      MapLabels->setChecked(contour->labels);
      cl->addWidget(MapLabels, r++, 0, 1, 2);
      const auto edit = [](double v, const char *name) {
        auto *e = new QLineEdit(std::isfinite(v) ? misc::num2str(v, -1, QString()) : QString());
        e->setPlaceholderText(tr("none"));
        e->setObjectName(QLatin1String(name));
        return e;
      };
      cl->addWidget(new QLabel(tr("Passes from:")), r, 0);
      MapPassMin = edit(contour->passMin, "contourPassMin");
      MapPassMin->setToolTip(tr("The lowest value that passes (20, 1.5m); the rest hatched"));
      cl->addWidget(MapPassMin, r++, 1);
      cl->addWidget(new QLabel(tr("to:")), r, 0);
      MapPassMax = edit(contour->passMax, "contourPassMax");
      MapPassMax->setToolTip(tr("The highest value that passes"));
      cl->addWidget(MapPassMax, r++, 1);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // A pole-zero map: its guides.
    if (auto *pz = dynamic_cast<PoleZeroDiagram *>(Diag)) {
      PzGuides = new QCheckBox(tr("\u03B6 lines and \u03C9n circles"), Tab2);
      PzGuides->setObjectName(QStringLiteral("poleZeroGuides"));
      PzGuides->setToolTip(tr("Lines of constant damping and circles of constant natural frequency in the left half-plane"));
      PzGuides->setChecked(pz->guides);
      gp->addWidget(PzGuides, Row, 0, 1, 2);
      Row++;
    }

    // Its spec limits: lines and masks its traces keep within.
    if (Diag->takesLimits()) {
      QGroupBox *box = new QGroupBox(tr("Limits"), Tab2);
      QGridLayout *ll = new QGridLayout(box);
      const bool panes = dynamic_cast<StackedDiagram *>(Diag) != nullptr;
      LimitTable = new QTableWidget(0, panes ? 5 : 4);
      LimitTable->setObjectName(QStringLiteral("limitTable"));
      QStringList heads{tr("limit"), tr("points: x, y; x, y; ... or a level"), tr("label"), tr("axis")};
      if (panes) heads << tr("pane");
      LimitTable->setHorizontalHeaderLabels(heads);
      LimitTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
      LimitTable->setToolTip(tr("What the traces keep within: below an upper limit, above a lower one. Its points run "
                                "straight from one to the next, x rising (1k, -3; 10k, -3); two of one x are a "
                                "mask's step; a number alone is a level over all x. Its y in the units the axis "
                                "shows (dB on an axis of dB). Drawn dashed, the traces red where beyond, PASS or "
                                "FAIL in the corner."));
      LimitTable->setMinimumHeight(110);
      for (const qucs_s::limits::Limit &limit : Diag->limits) addLimitRow(limit);
      ll->addWidget(LimitTable, 0, 0, 1, 3);
      QPushButton *add = new QPushButton(tr("Add Limit"));
      add->setObjectName(QStringLiteral("addLimit"));
      connect(add, &QPushButton::clicked, this, [this] {
        qucs_s::limits::Limit l;
        l.points << QPointF(0, 1);
        addLimitRow(l);
        LimitTable->item(LimitTable->rowCount() - 1, 1)->setText(QString());
        LimitTable->editItem(LimitTable->item(LimitTable->rowCount() - 1, 1));
      });
      QPushButton *remove = new QPushButton(tr("Remove Limit"));
      connect(remove, &QPushButton::clicked, this, [this] {
        if (LimitTable->currentRow() >= 0) LimitTable->removeRow(LimitTable->currentRow());
      });
      ll->addWidget(add, 1, 0);
      ll->addWidget(remove, 1, 1);
      gp->addWidget(box, Row, 0, 1, 2);
      Row++;
    }

    // ...........................................................
    xLabel->setText(Diag->xAxis.Label);
    ylLabel->setText(Diag->yAxis.Label);
    if (titleEdit) titleEdit->setText(Diag->title);
    if (yrLabel)
      yrLabel->setText(Diag->zAxis.Label);

    if ((Diag->Name.left(4) == "Rect") || (Diag->Name == "Curve")) {
      GridLogX = new QCheckBox(tr("logarithmic X Axis Grid"), Tab2);
      gp->addWidget(GridLogX, Row, 0);
      Row++;

      QStringList units_name;
      units_name << "No units" << "dB" << "dBuV" << "dBm";

      GridLogY = new QCheckBox(
          tr("logarithmic") + " " + NameY + " " + tr("Grid"), Tab2);
      gp->addWidget(GridLogY, Row, 0);
      LogUnitsY = new QComboBox(Tab2);
      LogUnitsY->addItems(units_name);
      LogUnitsY->setCurrentIndex(Diag->yAxis.Units);
      LogUnitsY->setEnabled(Diag->yAxis.log);
      connect(GridLogY, &QCheckBox::toggled, LogUnitsY, &QComboBox::setEnabled);

      connect(LogUnitsY, &QComboBox::currentIndexChanged, this,
              &DiagramDialog::slotRecalcDbLimitsY);

      gp->addWidget(LogUnitsY, Row, 1);
      Row++;

      GridLogZ = new QCheckBox(
          tr("logarithmic") + " " + NameZ + " " + tr("Grid"), Tab2);
      gp->addWidget(GridLogZ, Row, 0);
      LogUnitsZ = new QComboBox(Tab2);
      LogUnitsZ->addItems(units_name);
      LogUnitsZ->setCurrentIndex(Diag->zAxis.Units);
      LogUnitsZ->setEnabled(Diag->zAxis.log);
      connect(GridLogZ, &QCheckBox::toggled, LogUnitsZ, &QComboBox::setEnabled);

      connect(LogUnitsZ, &QComboBox::currentIndexChanged, this,
              &DiagramDialog::slotRecalcDbLimitsZ);

      gp->addWidget(LogUnitsZ, Row, 1);
      Row++;

      // ...........................................................
      // transfer the diagram properties to the dialog
      GridLogX->setChecked(Diag->xAxis.log);
      GridLogY->setChecked(Diag->yAxis.log);
      GridLogZ->setChecked(Diag->zAxis.log);

      if (Diag->Name == "Rect3D") {
        hideInvisible = new QCheckBox(tr("hide invisible lines"), Tab2);
        gp->addWidget(hideInvisible, Row, 0);
        Row++;

        QLabel *LabelRotX = new QLabel(tr("Rotation around x-Axis:"), Tab2);
        // LabelRotX->setPaletteForegroundColor(Qt::red);
        gp->addWidget(LabelRotX, Row, 0);
        SliderRotX = new QSlider(Tab2);
        SliderRotX->setMinimum(0);
        SliderRotX->setMaximum(360);
        SliderRotX->setSingleStep(20);
        SliderRotX->setValue(((Rect3DDiagram *)Diag)->rotX);
        SliderRotX->setOrientation(Qt::Horizontal);
        gp->addWidget(SliderRotX, Row, 1);
        connect(SliderRotX, QOverload<int>::of(&QSlider::valueChanged), this,
                &DiagramDialog::slotNewRotX);
        rotationX = new QLineEdit(Tab2);
        rotationX->setValidator(ValInteger);
        rotationX->setMaxLength(3);
        rotationX->setMaximumWidth(40);
        gp->addWidget(rotationX, Row, 2);
        connect(rotationX, &QLineEdit::textChanged, this,
                &DiagramDialog::slotEditRotX);

        Row++;

        QLabel *LabelRotY = new QLabel(tr("Rotation around y-Axis:"), Tab2);
        // LabelRotY->setPaletteForegroundColor(Qt::green);
        gp->addWidget(LabelRotY, Row, 0);
        // SliderRotY = new QSlider(0,360,20, ((Rect3DDiagram*)Diag)->rotY,
        //		 Qt::Horizontal, Tab2);
        SliderRotY = new QSlider(Tab2);
        SliderRotY->setMinimum(0);
        SliderRotY->setMaximum(360);
        SliderRotY->setSingleStep(20);
        SliderRotY->setValue(((Rect3DDiagram *)Diag)->rotY);
        SliderRotY->setOrientation(Qt::Horizontal);
        gp->addWidget(SliderRotY, Row, 1);
        connect(SliderRotY, QOverload<int>::of(&QSlider::valueChanged), this,
                &DiagramDialog::slotNewRotY);

        rotationY = new QLineEdit(Tab2);
        rotationY->setValidator(ValInteger);
        rotationY->setMaxLength(3);
        rotationY->setMaximumWidth(40);
        gp->addWidget(rotationY, Row, 2);
        connect(rotationY, &QLineEdit::textChanged, this,
                &DiagramDialog::slotEditRotY);

        Row++;

        QLabel *LabelRotZ = new QLabel(tr("Rotation around z-Axis:"), Tab2);
        // LabelRotZ->setPaletteForegroundColor(Qt::blue);
        gp->addWidget(LabelRotZ, Row, 0);
        // SliderRotZ = new QSlider(0,360,20, ((Rect3DDiagram*)Diag)->rotZ,
        //		 Qt::Horizontal, Tab2);
        SliderRotZ = new QSlider(Tab2);
        SliderRotZ->setMinimum(0);
        SliderRotZ->setMaximum(360);
        SliderRotZ->setSingleStep(20);
        SliderRotZ->setValue(((Rect3DDiagram *)Diag)->rotZ);
        SliderRotZ->setOrientation(Qt::Horizontal);
        gp->addWidget(SliderRotZ, Row, 1);
        connect(SliderRotZ, QOverload<int>::of(&QSlider::valueChanged), this,
                &DiagramDialog::slotNewRotZ);
        rotationZ = new QLineEdit(Tab2);
        rotationZ->setValidator(ValInteger);
        rotationZ->setMaxLength(3);
        rotationZ->setMaximumWidth(40);
        gp->addWidget(rotationZ, Row, 2);
        connect(rotationZ, &QLineEdit::textChanged, this,
                &DiagramDialog::slotEditRotZ);

        Row++;

        gp->addWidget(new QLabel(tr("2D-projection:"), Tab2), Row, 0);
        DiagCross = new Cross3D(((Rect3DDiagram *)Diag)->rotX,
                                ((Rect3DDiagram *)Diag)->rotY,
                                ((Rect3DDiagram *)Diag)->rotZ, Tab2);
        gp->addWidget(DiagCross, Row, 1);

        // transfer the diagram properties to the dialog
        hideInvisible->setChecked(Diag->hideLines);
        rotationX->setText(QString::number(((Rect3DDiagram *)Diag)->rotX));
        rotationY->setText(QString::number(((Rect3DDiagram *)Diag)->rotY));
        rotationZ->setText(QString::number(((Rect3DDiagram *)Diag)->rotZ));
      }
    } else
      GridLogX = GridLogY = GridLogZ = 0;

    t->addTab(Tab2, tr("Properties"));

    // Tab #3 - Limits
    // ...........................................................
    QWidget *Tab3 = new QWidget();
    QVBoxLayout *Tab3Layout = new QVBoxLayout();

    QGroupBox *axisX = new QGroupBox(tr("x-Axis"));
    QHBoxLayout *axisXLayout = new QHBoxLayout();

    QWidget *VBox1 = new QWidget();
    axisXLayout->addWidget(VBox1);
    QVBoxLayout *VBox1Layout = new QVBoxLayout();
    VBox1Layout->addStretch();
    manualX = new QCheckBox(tr("manual")); //, VBox1);
    VBox1Layout->addWidget(manualX);
    VBox1->setLayout(VBox1Layout);
    connect(manualX, QOverload<int>::of(&QCheckBox::stateChanged), this,
            &DiagramDialog::slotManualX);

    QWidget *VBox2 = new QWidget();
    axisXLayout->addWidget(VBox2);
    QVBoxLayout *VBox2Layout = new QVBoxLayout();
    VBox2Layout->addWidget(new QLabel(tr("start")));
    startX = new QLineEdit();
    VBox2Layout->addWidget(startX);
    startX->setValidator(ValDouble);
    VBox2->setLayout(VBox2Layout);

    QWidget *VBox3 = new QWidget();
    axisXLayout->addWidget(VBox3);
    QVBoxLayout *VBox3Layout = new QVBoxLayout();
    VBox3Layout->addWidget(new QLabel(tr("step")));
    stepX = new QLineEdit(); // VBox3);
    VBox3Layout->addWidget(stepX);
    stepX->setValidator(ValDouble);
    VBox3->setLayout(VBox3Layout);

    QWidget *VBox4 = new QWidget();
    axisXLayout->addWidget(VBox4);
    QVBoxLayout *VBox4Layout = new QVBoxLayout();
    VBox4Layout->addWidget(new QLabel(tr("stop")));
    stopX = new QLineEdit();
    VBox4Layout->addWidget(stopX);
    stopX->setValidator(ValDouble);
    VBox4->setLayout(VBox4Layout);

    axisX->setLayout(axisXLayout);
    Tab3Layout->addWidget(axisX);
    Tab3Layout->addStretch();

    QGroupBox *axisY = new QGroupBox(NameY);
    QHBoxLayout *axisYLayout = new QHBoxLayout();

    QWidget *VBox5 = new QWidget();
    axisYLayout->addWidget(VBox5);
    QVBoxLayout *VBox5Layout = new QVBoxLayout();
    VBox5Layout->addStretch();
    manualY = new QCheckBox(tr("manual"));
    VBox5Layout->addWidget(manualY);
    connect(manualY, QOverload<int>::of(&QCheckBox::stateChanged), this,
            &DiagramDialog::slotManualY);

    VBox5->setLayout(VBox5Layout);

    QWidget *VBox6 = new QWidget();
    axisYLayout->addWidget(VBox6);
    QVBoxLayout *VBox6Layout = new QVBoxLayout();
    VBox6Layout->addWidget(new QLabel(tr("start")));
    startY = new QLineEdit();
    VBox6Layout->addWidget(startY);
    startY->setValidator(ValDouble);
    VBox6->setLayout(VBox6Layout);

    QWidget *VBox7 = new QWidget();
    axisYLayout->addWidget(VBox7);
    QVBoxLayout *VBox7Layout = new QVBoxLayout();
    if ((Diag->Name == "Smith") || (Diag->Name == "ySmith") ||
        (Diag->Name == "PS"))
      VBox7Layout->addWidget(new QLabel(tr("number")));
    else
      VBox7Layout->addWidget(new QLabel(tr("step")));
    stepY = new QLineEdit();
    VBox7Layout->addWidget(stepY);
    stepY->setValidator(ValDouble);
    VBox7->setLayout(VBox7Layout);

    QWidget *VBox8 = new QWidget();
    axisYLayout->addWidget(VBox8);
    QVBoxLayout *VBox8Layout = new QVBoxLayout();
    VBox8Layout->addWidget(new QLabel(tr("stop")));
    stopY = new QLineEdit();
    VBox8Layout->addWidget(stopY);
    stopY->setValidator(ValDouble);
    VBox8->setLayout(VBox8Layout);

    axisY->setLayout(axisYLayout);
    Tab3Layout->addWidget(axisY);
    Tab3Layout->addStretch();

    QGroupBox *axisZ = new QGroupBox(NameZ);
    QHBoxLayout *axisZLayout = new QHBoxLayout();

    QWidget *VBox9 = new QWidget();
    axisZLayout->addWidget(VBox9);
    QVBoxLayout *VBox9Layout = new QVBoxLayout();
    VBox9Layout->addStretch();
    manualZ = new QCheckBox(tr("manual"));
    VBox9Layout->addWidget(manualZ);
    connect(manualZ, QOverload<int>::of(&QCheckBox::stateChanged), this,
            &DiagramDialog::slotManualZ);

    VBox9->setLayout(VBox9Layout);

    QWidget *VBox10 = new QWidget();
    axisZLayout->addWidget(VBox10);
    QVBoxLayout *VBox10Layout = new QVBoxLayout();
    VBox10Layout->addWidget(new QLabel(tr("start")));
    startZ = new QLineEdit();
    VBox10Layout->addWidget(startZ);
    startZ->setValidator(ValDouble);
    VBox10->setLayout(VBox10Layout);

    QWidget *VBox11 = new QWidget();
    axisZLayout->addWidget(VBox11);
    QVBoxLayout *VBox11Layout = new QVBoxLayout();
    if (Diag->Name == "SP")
      VBox11Layout->addWidget(new QLabel(tr("number")));
    else
      VBox11Layout->addWidget(new QLabel(tr("step")));
    stepZ = new QLineEdit();
    VBox11Layout->addWidget(stepZ);
    stepZ->setValidator(ValDouble);
    VBox11->setLayout(VBox11Layout);

    QWidget *VBox12 = new QWidget();
    axisZLayout->addWidget(VBox12);
    QVBoxLayout *VBox12Layout = new QVBoxLayout();
    VBox12Layout->addWidget(new QLabel(tr("stop")));
    stopZ = new QLineEdit();
    VBox12Layout->addWidget(stopZ);
    stopZ->setValidator(ValDouble);
    VBox12->setLayout(VBox12Layout);

    Tab3Layout->setStretchFactor(new QWidget(Tab3),
                                 5); // stretchable placeholder

    axisZ->setLayout(axisZLayout);
    Tab3Layout->addWidget(axisZ);

    Tab3->setLayout(Tab3Layout);
    t->addTab(Tab3, tr("Limits"));

    // ...........................................................
    // transfer the diagram properties to the dialog
    if (Diag->xAxis.autoScale)
      slotManualX(0);
    else
      manualX->setChecked(true);
    if (Diag->yAxis.autoScale)
      slotManualY(0);
    else
      manualY->setChecked(true);
    if (Diag->zAxis.autoScale)
      slotManualZ(0);
    else
      manualZ->setChecked(true);

    Diag->calcLimits(); // inserts auto-scale values if not manual

    startX->setText(QString::number(Diag->xAxis.limit_min));
    stepX->setText(QString::number(Diag->xAxis.step));
    stopX->setText(QString::number(Diag->xAxis.limit_max));

    double val_startY = qucs::num2db(Diag->yAxis.limit_min, Diag->yAxis.Units);
    double val_stepY = qucs::num2db(Diag->yAxis.step, Diag->yAxis.Units);
    double val_stopY = qucs::num2db(Diag->yAxis.limit_max, Diag->yAxis.Units);

    double val_startZ = qucs::num2db(Diag->zAxis.limit_min, Diag->zAxis.Units);
    double val_stepZ = qucs::num2db(Diag->zAxis.step, Diag->zAxis.Units);
    double val_stopZ = qucs::num2db(Diag->zAxis.limit_max, Diag->zAxis.Units);

    startY->setText(QString::number(val_startY));
    stepY->setText(QString::number(val_stepY));
    stopY->setText(QString::number(val_stopY));

    startZ->setText(QString::number(val_startZ));
    stepZ->setText(QString::number(val_stepZ));
    stopZ->setText(QString::number(val_stopZ));

    if ((Diag->Name == "Smith") || (Diag->Name == "ySmith") ||
        (Diag->Name == "Polar") || (Diag->Name == "Histogram") || (Diag->Name == "Eye") || (Diag->Name == "PoleZero")
        || (Diag->Name == "Bathtub")) {
      axisZ->setEnabled(false);
    }
    if (Diag->Name == "Stacked") {   // (its panes' axes: on the Properties tab)
      axisY->setVisible(false);
      axisZ->setVisible(false);
    }
    if (Diag->Name.left(4) != "Rect") // cartesian 2D and 3D
      if (Diag->Name != "Curve" && Diag->Name != "Histogram" && Diag->Name != "Eye" && Diag->Name != "Stacked"
          && Diag->Name != "PoleZero" && Diag->Name != "Nichols" && Diag->Name != "Bode" && Diag->Name != "Spectrum"
          && Diag->Name != "Bathtub" && Diag->Name != "Contour") {
        axisX->setEnabled(false);
        startY->setEnabled(false);
        startZ->setEnabled(false);
      }
  } else
    stepX = 0;

  // Tab #4 - Theme: the colours of its parts.
  if (auto *doc = qobject_cast<Schematic *>(parent))
    a_canvas = doc->viewport()->palette().color(doc->viewport()->backgroundRole());
  if (!a_canvas.isValid()) a_canvas = Qt::white;
  t->addTab(makeThemeTab(NameY, NameZ), tr("Theme"));

  // The Import tab: data files read into datasets beside the schematic,
  // for the Data tab to list next to the simulations'. (A schematic not
  // saved has no folder for them: the tab says so.)
  QString importFolder;
  if (const auto *s = dynamic_cast<const Schematic *>(parent); s != nullptr && !s->getDocName().isEmpty())
    importFolder = QFileInfo(s->getDocName()).absolutePath();
  a_import = new DataImportPanel(importFolder, t);
  t->addTab(a_import, tr("Import"));
  connect(a_import, &DataImportPanel::datasetsChanged, this, &DiagramDialog::slotDatasetsChanged);

  // The Export tab: a dataset's variables written to a file for another
  // program (CSV, Excel, text, NumPy, ...); the Data tab's dataset shown
  // until one is chosen there.
  a_export = new DataExportPanel(importFolder, t);
  a_export->setTraces([this] { return traceFiles(); });
  connect(a_export, &DataExportPanel::datasetsChanged, this, &DiagramDialog::slotDatasetsChanged);
  t->addTab(a_export, tr("Export"));

  connect(t, &QTabWidget::currentChanged, this, &DiagramDialog::slotChangeTab);
  // ...........................................................
  QWidget *Butts = new QWidget();
  QHBoxLayout *ButtsLayout = new QHBoxLayout();
  ButtsLayout->setSpacing(5);
  ButtsLayout->setContentsMargins(5, 5, 5, 5);
  Butts->setLayout(ButtsLayout);
  all->addWidget(Butts);

  QPushButton *OkButt = new QPushButton(tr("OK"));
  ButtsLayout->addWidget(OkButt);
  connect(OkButt, &QPushButton::clicked, this, &DiagramDialog::slotOK);
  QPushButton *ApplyButt = new QPushButton(tr("Apply"));
  ButtsLayout->addWidget(ApplyButt);
  connect(ApplyButt, &QPushButton::clicked, this, &DiagramDialog::slotApply);
  QPushButton *CancelButt = new QPushButton(tr("Cancel"));
  ButtsLayout->addWidget(CancelButt);
  connect(CancelButt, &QPushButton::clicked, this, &DiagramDialog::slotCancel);

  OkButt->setDefault(true);

  // ...........................................................
  // put all data files into ComboBox
  fillDatasets(QString());
  slotReadVarsAndSetSimulator(0); // put variables into the ListView

  // ...........................................................
  // put all graphs into the ListBox
  Row = 0;
  for (Graph *pg : Diag->Graphs) {
    GraphList->setRowCount(Row + 1);

    // Populate the table row with graph properties
    // Note: Graphs vector is already populated by copyDiagramGraphs()
    updateGraphListItem(Row);

    if (pg == currentGraph) {
      GraphList->selectRow(Row); // select current graph
      SelectGraph(currentGraph);
    }
    Row++;
  }

  if (ColorButt) {
    if (!currentGraph) {
      QColor selectedColor(
          DefaultColors[GraphList->rowCount() % NumDefaultColors]);
      QString stylesheet = QStringLiteral("QPushButton {background-color: %1};")
                               .arg(selectedColor.name());
      ColorButt->setStyleSheet(stylesheet);
      misc::setPickerColor(ColorButt, selectedColor);
    }
  }
}

void DiagramDialog::fillDatasets(const QString &select) {
  const QString before = select.isEmpty() ? chosenDataset() : select;
  QFileInfo Info(defaultDataSet);
  QDir ProjDir(Info.absolutePath());
  a_importedNames.clear();
  if (a_import != nullptr && !a_import->folder().isEmpty())
    for (const qucs_s::dataimport::Imported &i : qucs_s::dataimport::importedIn(a_import->folder()))
      a_importedNames.insert(i.name);
  ChooseData->clear();
  // Each dataset once, by its name (its data), an imported one said so.
  const auto add = [this](const QString &name) {
    if (ChooseData->findData(name) >= 0)
      return;
    ChooseData->addItem(a_importedNames.contains(name) ? tr("%1  (imported)").arg(name) : name, name);
  };
  QStringList entries;
  entries << "*.dat" << "*.dat.ngspice" << "*.dat.xyce" << "*.dat.spopus";
  QStringList Elements = ProjDir.entryList(entries, QDir::Files, QDir::Name);
  QStringList::iterator it;
  int own = -1;   // the schematic's own
  for (it = Elements.begin(); it != Elements.end(); ++it) {
    if (it->endsWith(".dat")) {
      add((*it).left((*it).length() - 4));
      if ((*it) == Info.fileName())
        own = ChooseData->findData((*it).left((*it).length() - 4));
    } else {
      QString ext = (*it).section('.', -2, -1); // double extension
      int extl = ext.length() + 1;              // full extension length
      int shextl = extl - 4; // extension length without ".dat"
      add((*it).left((*it).length() - extl));
      if ((*it).left((*it).length() - shextl) == Info.fileName()) // default dataset should be the current
        own = ChooseData->findData((*it).left((*it).length() - extl));
    }
  }
  const int chosen = ChooseData->findData(before);
  if (chosen >= 0)
    ChooseData->setCurrentIndex(chosen);
  else if (own >= 0)
    ChooseData->setCurrentIndex(own);
}

QList<QPair<QString, QString>> DiagramDialog::traceFiles() const {
  // As Graph::loadDatFile finds them: a simulator's prefix is the file's
  // suffix (ngspice/tran.v(out): name.dat.ngspice), name:variable another
  // dataset beside the schematic.
  QList<QPair<QString, QString>> list;
  const QString dir = QFileInfo(defaultDataSet).absolutePath();
  for (const auto &g : Graphs) {
    QString var = g->Var, tail;
    const qsizetype slash = var.indexOf('/');
    if (slash > 0) {
      tail = '.' + var.left(slash);
      var = var.mid(slash + 1);
    }
    const qsizetype colon = var.indexOf(':');
    QString file = defaultDataSet + tail;
    if (colon > 0) {
      file = dir + QDir::separator() + var.left(colon) + ".dat" + tail;
      var = var.mid(colon + 1);
    }
    const int at = Graph::plotVsSeparator(var);
    list.append({file, at > 0 ? var.left(at) : var});
  }
  return list;
}

QString DiagramDialog::chosenDataset() const {
  const QVariant name = ChooseData->currentData();
  return name.isValid() ? name.toString() : ChooseData->currentText();
}

void DiagramDialog::slotDatasetsChanged(const QString &select) {
  if (a_export != nullptr)
    a_export->refresh();
  fillDatasets(select);
  slotReadVarsAndSetSimulator(0);
}

DiagramDialog::~DiagramDialog() {
  delete all; // delete all widgets from heap
  delete ValInteger;
  delete ValDouble;
  delete Validator;
}

/*!
 * \brief Reads variables from the selected dataset file and populates the
 *        ChooseVars table, also sets the appropriate simulator.
 *
 * Called when the dataset selection changes. Determines which simulator
 * datasets exist (.dat, .dat.ngspice, .dat.xyce, .dat.spopus) and populates
 * the ChooseSimulator combo box accordingly.
 *
 * \param index The index of the selected dataset (unused, kept for signal
 * compatibility) \see slotReadVars(), updateCompleter()
 */
void DiagramDialog::slotReadVarsAndSetSimulator(int) {
  QFileInfo Info(defaultDataSet);
  QString DocName = chosenDataset() + ".dat";
  // Imported: from its file, no simulator's.
  if (a_importedNames.contains(chosenDataset())) {
    lblSim->setText(tr("Data from:"));
    ChooseSimulator->blockSignals(true);
    ChooseSimulator->clear();
    qucs_s::dataimport::Origin origin;
    qucs_s::dataimport::originOf(Info.absolutePath() + QDir::separator() + DocName, &origin);
    ChooseSimulator->addItem(QFileInfo(origin.source).fileName());
    ChooseSimulator->setToolTip(QDir::toNativeSeparators(origin.source));
    ChooseSimulator->blockSignals(false);
    slotReadVars(0);
    updateCompleter();
    return;
  }
  lblSim->setText(tr("Data from simulator:"));
  ChooseSimulator->setToolTip(QString());

  QString curr_sim;
  switch (QucsSettings.DefaultSimulator) {
  case spicecompat::simQucsator:
    curr_sim = "Qucsator";
    break;
  case spicecompat::simNgspice:
    curr_sim = "Ngspice";
    break;
  case spicecompat::simXyce:
    curr_sim = "Xyce";
    break;
  case spicecompat::simSpiceOpus:
    curr_sim = "SpiceOpus";
    break;
  default:
    curr_sim = ChooseSimulator->currentText();
  }

  // Recreate items of ChooseSimulator. Only existing datasets
  // should be shown
  ChooseSimulator->blockSignals(true); // Lock signals firing
  ChooseSimulator->clear();
  Info.setFile(Info.absolutePath() + QDir::separator() + DocName);
  if (Info.exists())
    ChooseSimulator->addItem("Qucsator");
  Info.setFile(Info.absolutePath() + QDir::separator() + DocName + ".ngspice");
  if (Info.exists())
    ChooseSimulator->addItem("Ngspice");
  Info.setFile(Info.absolutePath() + QDir::separator() + DocName + ".xyce");
  if (Info.exists())
    ChooseSimulator->addItem("Xyce");
  Info.setFile(Info.absolutePath() + QDir::separator() + DocName + ".spopus");
  if (Info.exists())
    ChooseSimulator->addItem("SpiceOpus");
  int sim_pos =
      ChooseSimulator->findText(curr_sim); // set default simulator if possible
  if (sim_pos >= 0)
    ChooseSimulator->setCurrentIndex(sim_pos);
  ChooseSimulator->blockSignals(false); // Unlock signals

  slotReadVars(0);

  updateCompleter(); // Variable completion
}

/*!
 * \brief Reads variables from the current dataset file and populates the
 *        ChooseVars table.
 *
 * Parses the dataset file to extract variable information (dependent and
 * independent variables) and displays them in the ChooseVars table with
 * their type and size.
 *
 * \param index The simulator index (unused, kept for signal compatibility)
 * \see slotReadVarsAndSetSimulator(), updateCompleter()
 */
void DiagramDialog::slotReadVars(int) {
  QFileInfo Info(defaultDataSet);
  QString DocName = chosenDataset() + ".dat";

  if (ChooseSimulator->currentText() == "Ngspice") {
    DocName += ".ngspice";
  } else if (ChooseSimulator->currentText() == "Xyce") {
    DocName += ".xyce";
  } else if (ChooseSimulator->currentText() == "SpiceOpus") {
    DocName += ".spopus";
  }
  if (a_export != nullptr)
    a_export->follow(Info.absolutePath() + QDir::separator() + DocName);

  QFile file(Info.absolutePath() + QDir::separator() + DocName);
  if (!file.open(QIODevice::ReadOnly)) {
    return;
  }

  QString Line, tmp, Var;
  int varNumber = 0;
  // reading the file as a whole improves speed very much, also using
  // a QByteArray rather than a QString
  QByteArray FileString = file.readAll();
  file.close();

  // make sure sorting is disabled before inserting items
  ChooseVars->setSortingEnabled(false);
  ChooseVars->clearContents();
  ChooseXVar->clear();
  ChooseXVar->addItem("default");

  int i = 0, j = 0;
  i = FileString.indexOf('<') + 1;
  if (i > 0)
    do {
      j = FileString.indexOf('>', i);
      Line.resize(j - i);
      for (int k = 0; k < j - i; k++)
        Line[k] = (FileString[k + i]);
      Line.truncate(j - i);
      i = FileString.indexOf('<', j) + 1;

      Var = Line.section(' ', 1, 1).remove('>');
      if (Var.length() > 0)
        if (Var.at(0) == '_')
          continue;

      if (Line.left(3) == "dep") {
        tmp = Line.section(' ', 2);
        // new Q3ListViewItem(ChooseVars, Var, "dep", tmp.remove('>'));
        qDebug() << varNumber << Var << tmp.remove('>');
        ChooseVars->setRowCount(varNumber + 1);
        QTableWidgetItem *cell = new QTableWidgetItem(Var);
        ChooseXVar->addItem(Var);
        cell->setFlags(cell->flags() ^ Qt::ItemIsEditable);
        ChooseVars->setItem(varNumber, 0, cell);
        cell = new QTableWidgetItem("dep");
        cell->setFlags(cell->flags() ^ Qt::ItemIsEditable);
        ChooseVars->setItem(varNumber, 1, cell);
        cell = new QTableWidgetItem(tmp.remove('>'));
        cell->setFlags(cell->flags() ^ Qt::ItemIsEditable);
        ChooseVars->setItem(varNumber, 2, cell);
        varNumber++;
      } else if (Line.left(5) == "indep") {
        tmp = Line.section(' ', 2, 2);
        // new Q3ListViewItem(ChooseVars, Var, "indep", tmp.remove('>'));
        qDebug() << varNumber << Var << tmp.remove('>');
        ChooseVars->setRowCount(varNumber + 1);
        QTableWidgetItem *cell = new QTableWidgetItem(Var);
        ChooseXVar->addItem(Var);
        cell->setFlags(cell->flags() ^ Qt::ItemIsEditable);
        ChooseVars->setItem(varNumber, 0, cell);
        cell = new QTableWidgetItem("indep");
        cell->setFlags(cell->flags() ^ Qt::ItemIsEditable);
        ChooseVars->setItem(varNumber, 1, cell);
        cell = new QTableWidgetItem(tmp.remove('>'));
        cell->setFlags(cell->flags() ^ Qt::ItemIsEditable);
        ChooseVars->setItem(varNumber, 2, cell);
        varNumber++;
      }
    } while (i > 0);
  // sorting should be enabled only after adding items
  ChooseVars->setSortingEnabled(true);

  // Update completer with new variables
  updateCompleter();
}

/*!
 * \brief Inserts a variable from the dataset into the graph input and creates
 *        a new graph entry.
 *
 * This slot is triggered when the user double-clicks a variable in the
 * ChooseVars table. It performs the following operations:
 * - Constructs the full variable name from the dataset and the simulator
 * prefixes
 * - Sets the variable name in the GraphInput field
 * - Creates a new Graph object with appropriate properties
 * - Adds the graph to the internal Graphs vector
 * - Updates the GraphList table display
 * - Selects the newly created graph
 *
 * Variable name construction:
 * - If the selected dataset differs from the default, prefixes with
 *   "dataset_name:"
 * - If a non-Qucsator simulator is selected, prefixes with "ngspice/",
 *   "xyce/", or "spopus/"
 * - Example: "ngspice/mydataset:voltage.V1"
 *
 *
 * \param Item The table widget item that was double-clicked
 *
 * \see slotNewGraph(), slotSelectGraph(), updateGraphListItem()
 */
void DiagramDialog::slotTakeVar(QTableWidgetItem *Item) {
  GraphInput->blockSignals(true);
  if (toTake)
    GraphInput->setText("");

  int row = Item->row();
  QString s1 = ChooseVars->item(row, 0)->text();
  QFileInfo Info(defaultDataSet);
  if (chosenDataset() != Info.baseName())
    s1 = chosenDataset() + ":" + s1;
  if (ChooseSimulator->currentText() == "Ngspice") {
    s1 = "ngspice/" + s1;
  } else if (ChooseSimulator->currentText() == "Xyce") {
    s1 = "xyce/" + s1;
  } else if (ChooseSimulator->currentText() == "SpiceOpus") {
    s1 = "spopus/" + s1;
  }
  GraphInput->setText(s1);
  updateXVar();

  // Add new row to table
  int newRow = GraphList->rowCount();
  GraphList->setRowCount(newRow + 1);

  Graph *g = new Graph(Diag, GraphInput->text());
  if (PartBox != nullptr) g->valuePart = Graph::ValuePart(PartBox->currentIndex());

  if (Diag->Name != "Tab" && Diag->Name != "Truth") {
    g->Color = misc::getWidgetBackgroundColor(ColorButt);
    g->autoColor = AutoColorBox != nullptr && AutoColorBox->isChecked();
    if (MarkerBox != nullptr) g->pointMarker = Graph::PointMarker(MarkerBox->currentIndex());
    g->Thick = thicknessSpin->value();
    QColor selectedColor(
        DefaultColors[GraphList->rowCount() % NumDefaultColors]);
    QString stylesheet = QStringLiteral("QPushButton {background-color: %1};")
                             .arg(selectedColor.name());
    ColorButt->setStyleSheet(stylesheet);
    misc::setPickerColor(ColorButt, selectedColor);
    if (g->Var.right(3) == ".Vb")
      if (PropertyBox->count() >= GRAPHSTYLE_ARROW)
        PropertyBox->setCurrentIndex(GRAPHSTYLE_ARROW);
    g->Style = toGraphStyle(PropertyBox->currentIndex());
    QUCS_ASSERT(g->Style != GRAPHSTYLE_INVALID);
    if (yAxisBox) {
      setAxisIndex(g, yAxisBox->currentIndex());
      yAxisBox->setEnabled(true);
      Label4->setEnabled(true);
    } else if (Diag->Name == "Rect3D") {
      g->yAxisNo = 1;
    }
    Label3->setEnabled(true);
    ColorButt->setEnabled(!g->autoColor);
    if (AutoColorBox) AutoColorBox->setEnabled(true);
    enableMarkerBox(g);
  } else if (Diag->Name == "Tab") { // Changed from 'else' to 'else if'
    if (precisionSpin) {            // Add null check
      g->Precision = precisionSpin->value();
      g->numMode = PropertyBox->currentIndex();
    }
  }
  // For "Truth" diagrams, we don't set any special properties

  Graphs.emplace_back(g);
  updateGraphListItem(newRow);
  GraphList->selectRow(newRow);

  changed = true;
  toTake = true;

  GraphInput->blockSignals(false);

  if (thicknessSpin) {
    Label1->setEnabled(true);
    PropertyBox->setEnabled(true);
    thicknessLabel->setEnabled(true);
    thicknessSpin->setEnabled(true);
  }
  if (precisionSpin) {
    Label1->setEnabled(true);
    PropertyBox->setEnabled(true);
    precisionLabel->setEnabled(true);
    precisionSpin->setEnabled(true);
  }
}

/*!
 * \brief Handles selection of a graph row in the GraphList table.
 *
 * Extracts the Graph pointer from the selected row
 * and calls SelectGraph() to populate the UI controls.
 *
 * \param item The table item that was clicked
 * \see SelectGraph()
 */
void DiagramDialog::slotSelectGraph(QTableWidgetItem *item) {
  if (item == 0) {
    GraphList->clearSelection();
    return;
  }

  int row = item->row();
  SelectGraph(Graphs.at(row).get());
}

/*!
 * \brief Displays the properties of the selected graph in the UI controls.
 *
 * This function is called when a graph is selected from the GraphList table.
 * It populates all relevant UI controls with the selected graph's properties,
 * allowing the user to view and edit them.
 *
 * For plot-type diagrams (Rectangular, Smith, Polar, etc.):
 * - Sets thicknessSpin
 * - Sets ColorButt
 * - Sets PropertyBox
 * - Sets yAxisBox
 *
 * For tabular diagrams ("Tab" or "Truth"):
 * - Sets precisionSpin
 *
 * \see slotSelectGraph(), slotTakeVar(), updateXVar()
 */
void DiagramDialog::SelectGraph(Graph *g) {
  GraphInput->blockSignals(true);
  GraphInput->setText(g->Var);
  GraphInput->blockSignals(false);
  updateXVar();
  if (PartBox) {
    const QSignalBlocker block(PartBox);
    PartBox->setCurrentIndex(int(g->valuePart));
  }

  if (Diag->Name != "Tab") {
    if (Diag->Name != "Truth") {
      thicknessSpin->setValue(g->Thick);
      QString stylesheet = QStringLiteral("QPushButton {background-color: %1};")
                               .arg(g->Color.name());
      ColorButt->setStyleSheet(stylesheet);
      misc::setPickerColor(ColorButt, g->Color);
      PropertyBox->setCurrentIndex(g->Style);
      if (yAxisBox) {
        yAxisBox->setCurrentIndex(axisIndexOf(g));
        yAxisBox->setEnabled(true);
        Label4->setEnabled(true);
      }

      Label3->setEnabled(true);
      ColorButt->setEnabled(!g->autoColor);
      if (AutoColorBox) {
        const QSignalBlocker block(AutoColorBox);
        AutoColorBox->setChecked(g->autoColor);
        AutoColorBox->setEnabled(true);
      }
      if (GhostBox) {
        const QSignalBlocker block(GhostBox);
        GhostBox->setChecked(g->ghost);
        GhostBox->setEnabled(true);
      }
      if (MarkerBox) {
        const QSignalBlocker block(MarkerBox);
        MarkerBox->setCurrentIndex(int(g->pointMarker));
      }
      enableMarkerBox(g);
    }
  } else {
    precisionSpin->setValue(g->Precision);
    PropertyBox->setCurrentIndex(g->numMode);
  }
  toTake = false;

  if (thicknessSpin) {
    Label1->setEnabled(true);
    PropertyBox->setEnabled(true);
    thicknessLabel->setEnabled(true);
    thicknessSpin->setEnabled(true);
  }
  if (precisionSpin) {
    Label1->setEnabled(true);
    PropertyBox->setEnabled(true);
    precisionLabel->setEnabled(true);
    precisionSpin->setEnabled(true);
  }
}

/*!
 * \brief Removes the currently selected graph from the diagram.
 *
 * This slot is called when the user presses the "Delete Graph" button or
 * double-clicks a graph in the GraphList. It performs the following cleanup:
 * - Removes the selected row from the GraphList table
 * - Erases the corresponding Graph from the Graphs vector
 * - Selects an adjacent graph if available
 * - Resets UI controls to default values
 * - Updates the default color for new graphs
 *
 * \see slotNewGraph(), SelectGraph(), slotSelectGraph()
 */
void DiagramDialog::slotDeleteGraph() {
  int i = GraphList->currentRow();
  if (i < 0)
    return;

  GraphList->removeRow(i);
  Graphs.erase(std::next(Graphs.begin(), i));

  int k = 0;
  if (GraphList->rowCount() != 0) {
    if (i > (GraphList->rowCount() - 1)) {
      k = GraphList->rowCount() - 1;
    } else {
      k = i;
    }
    GraphList->selectRow(k);
    SelectGraph(Graphs.at(k).get());
  } else {
    GraphInput->setText("");
  }

  if (Diag->Name != "Tab" && Diag->Name != "Truth") {
    QColor selectedColor(
        DefaultColors[GraphList->rowCount() % NumDefaultColors]);
    QString stylesheet = QStringLiteral("QPushButton {background-color: %1};")
                             .arg(selectedColor.name());
    ColorButt->setStyleSheet(stylesheet);
    misc::setPickerColor(ColorButt, selectedColor);
    if (thicknessSpin)
      thicknessSpin->setValue(0);
    if (yAxisBox) {
      yAxisBox->setCurrentIndex(0);
      yAxisBox->setEnabled(false);
      Label4->setEnabled(false);
    }
    Label3->setEnabled(false);
    ColorButt->setEnabled(false);
    if (AutoColorBox) AutoColorBox->setEnabled(GraphList->rowCount() != 0);
    if (GhostBox) GhostBox->setEnabled(GraphList->rowCount() != 0);
    if (GraphList->rowCount() == 0) enableMarkerBox(nullptr);
  } else {
    if (precisionSpin)
      precisionSpin->setValue(3);
  }

  changed = true;
  toTake = false;

  if (thicknessSpin) {
    PropertyBox->setCurrentIndex(0);
    Label1->setEnabled(false);
    PropertyBox->setEnabled(false);
    thicknessLabel->setEnabled(false);
    thicknessSpin->setEnabled(false);
  }
  if (precisionSpin) {
    PropertyBox->setCurrentIndex(0);
    Label1->setEnabled(false);
    PropertyBox->setEnabled(false);
    precisionLabel->setEnabled(false);
    precisionSpin->setEnabled(false);
  }
}

/*!
 * \brief Creates a new graph from the current GraphInput text.
 *
 * This slot is triggered when the user presses the "New Graph" button or
 * presses Enter in the GraphInput field (when no graph is selected). It
 * creates a new graph entry with the variable expression from GraphInput.
 *
 * The function performs the following operations:
 * - Validates that GraphInput is not empty
 * - Creates a new Graph object with the input text as the variable expression
 * - Sets graph properties based on current UI control values
 * - Adds the graph to the Graphs vector
 * - Adds a new row to the GraphList table
 * - Selects the newly created graph
 *
 *
 * \see slotTakeVar(), SelectGraph(), updateGraphListItem()
 */
void DiagramDialog::slotNewGraph() {
  QUCS_ASSERT(Diag);
  if (GraphInput->text().isEmpty())
    return;

  int newRow = GraphList->rowCount();
  GraphList->setRowCount(newRow + 1);

  Graph *g = new Graph(Diag, GraphInput->text());
  if (PartBox != nullptr) g->valuePart = Graph::ValuePart(PartBox->currentIndex());

  if (Diag->Name != "Tab" && Diag->Name != "Truth") {
    g->Color = misc::getWidgetBackgroundColor(ColorButt);
    g->autoColor = AutoColorBox != nullptr && AutoColorBox->isChecked();
    if (MarkerBox != nullptr) g->pointMarker = Graph::PointMarker(MarkerBox->currentIndex());
    g->Thick = thicknessSpin->value();
    g->Style = toGraphStyle(PropertyBox->currentIndex());
    QUCS_ASSERT(g->Style != GRAPHSTYLE_INVALID);
    if (yAxisBox) {
      setAxisIndex(g, yAxisBox->currentIndex());
    } else if (Diag->Name == "Rect3D") {
      g->yAxisNo = 1;
    }
  } else {
    g->Precision = precisionSpin->value();
    g->numMode = PropertyBox->currentIndex();
  }

  Graphs.emplace_back(g);
  updateGraphListItem(newRow);
  GraphList->selectRow(newRow);

  changed = true;
  toTake = false;
}

/*!
 * \brief Applies all changes and closes the dialog.
 *
 * Calls slotApply() to commit changes, then slotCancel() to close.
 *
 * \see slotApply(), slotCancel()
 */
void DiagramDialog::slotOK() {
  if (!valuesTaken()) return;   // (left open, the field to correct in focus)
  slotApply();
  slotCancel();
}

// Whether the values typed can be taken: one that cannot is said, and its
// field given the focus (an eye diagram's: they were dropped without a
// word, and a mask set before went with them).
bool DiagramDialog::valuesTaken() {
  if (LimitTable) {
    QList<qucs_s::limits::Limit> limits;
    if (!readLimits(&limits, true)) return false;
  }
  auto *eyeDiagram = dynamic_cast<EyeDiagram *>(Diag);
  if (eyeDiagram == nullptr || EyeUi == nullptr) return true;
  struct Check {
    QLineEdit *edit;
    std::function<bool(double)> fits;
    QString what;
  };
  const QList<Check> checks = {
      {EyeUi, [](double v) { return v > 0; }, tr("The unit interval is a bit's length above 0 (100 ps, 1n), or empty: the PRBS source's Tbit or the crossings.")},
      {EyeStart, [](double) { return true; }, tr("From is a time (2 ns), or empty: from the start.")},
      {EyeThreshold, [](double) { return true; }, tr("The threshold is a value of the signal (0.5, 450m), or empty: halfway between the levels.")},
      {EyeMaskWidth, [](double v) { return v > 0 && v <= 1; }, tr("The mask's width is in UI, above 0 and at most 1 (0.5), or empty: no mask.")},
      {EyeMaskHeight, [](double v) { return v > 0; }, tr("The mask's height is above 0, in the signal's unit (0.2), or empty: no mask.")},
  };
  for (const Check &c : checks) {
    const QString text = c.edit->text().trimmed();
    if (text.isEmpty() || c.edit->text() == c.edit->property("qucsShown").toString()) continue;
    const qucs_s::units::Reading r = qucs_s::units::read(text);
    if (r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) && c.fits(r.value)) continue;
    QMessageBox::warning(this, tr("Eye Diagram"), tr("%1 cannot be taken. %2").arg(text, c.what));
    c.edit->setFocus();
    c.edit->selectAll();
    return false;
  }
  // A mask: both, or neither.
  if (EyeMaskWidth->text().trimmed().isEmpty() != EyeMaskHeight->text().trimmed().isEmpty()) {
    QLineEdit *empty = EyeMaskWidth->text().trimmed().isEmpty() ? EyeMaskWidth : EyeMaskHeight;
    QMessageBox::warning(this, tr("Eye Diagram"), tr("A mask has a width and a height: give both, or neither for no mask."));
    empty->setFocus();
    return false;
  }
  return true;
}

/*!
 * \brief Applies all diagram property changes without closing the dialog.
 *
 * Transfers all UI control values to the Diagram object
 *
 * \see slotOK(), slotCancel()
 */
void DiagramDialog::slotApply() {
  if (!valuesTaken()) return;
  if (Diag->Name.at(0) != 'T') { // not tabular or timing
    if (titleEdit && Diag->title != titleEdit->text().trimmed()) {
      Diag->title = titleEdit->text().trimmed();
      changed = true;
    }
    if (Diag->xAxis.Label.isEmpty())
      Diag->xAxis.Label = ""; // can be not 0 and empty!
    if (xLabel->text().isEmpty())
      xLabel->setText("");
    if (Diag->xAxis.Label != xLabel->text()) {
      Diag->xAxis.Label = xLabel->text();
      changed = true;
    }
    if (Diag->yAxis.Label.isEmpty())
      Diag->yAxis.Label = ""; // can be not 0 and empty!
    if (ylLabel->text().isEmpty())
      ylLabel->setText("");
    if (Diag->yAxis.Label != ylLabel->text()) {
      Diag->yAxis.Label = ylLabel->text();
      changed = true;
    }

    if (NotationBox) {
      const auto notation = qucs_s::numberformat::fromInt(NotationBox->currentData().toInt());
      if (Diag->notation != notation)
        changed = true;
      Diag->notation = notation;
    }
    if (DecimalsBox && Diag->notationDecimals != DecimalsBox->value()) {
      Diag->notationDecimals = DecimalsBox->value();
      changed = true;
    }

    if (LegendBox && Diag->legendPos != LegendBox->currentIndex()) {
      Diag->legendPos = LegendBox->currentIndex();
      changed = true;
    }

    if (auto *hist = dynamic_cast<HistogramDiagram *>(Diag); hist && HistBins) {
      auto limitOf = [](const QLineEdit *e) {
        const qucs_s::units::Reading r = qucs_s::units::read(e->text());
        return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      };
      auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
      const double lower = limitOf(HistLower), upper = limitOf(HistUpper);
      if (hist->bins != HistBins->value() || hist->height != HistHeight->currentIndex()
          || hist->normalFit != HistFit->isChecked() || hist->statistics != HistStats->isChecked()
          || !same(hist->lowerLimit, lower) || !same(hist->upperLimit, upper)) {
        hist->bins = HistBins->value();
        hist->height = HistHeight->currentIndex();
        hist->normalFit = HistFit->isChecked();
        hist->statistics = HistStats->isChecked();
        hist->lowerLimit = lower;
        hist->upperLimit = upper;
        changed = true;
      }
    }

    if (auto *eyeDiagram = dynamic_cast<EyeDiagram *>(Diag); eyeDiagram && EyeUi) {
      // A field as it was shown keeps the value it showed.
      auto valueOf = [](const QLineEdit *e, double was) {
        if (e->text() == e->property("qucsShown").toString()) return was;
        const qucs_s::units::Reading r = qucs_s::units::read(e->text());
        return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      };
      auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
      double ui = valueOf(EyeUi, eyeDiagram->ui);
      if (!(ui > 0)) ui = std::nan("");
      const double start = valueOf(EyeStart, eyeDiagram->start);
      const double threshold = valueOf(EyeThreshold, eyeDiagram->threshold);
      double maskWidth = valueOf(EyeMaskWidth, eyeDiagram->maskWidth);
      double maskHeight = valueOf(EyeMaskHeight, eyeDiagram->maskHeight);
      if (!(maskWidth > 0 && maskWidth <= 1) || !(maskHeight > 0)) maskWidth = maskHeight = std::nan("");
      const int levels = EyeLevels->currentIndex() == 2 ? 4 : EyeLevels->currentIndex() == 1 ? 2 : 0;
      if (!same(eyeDiagram->ui, ui) || eyeDiagram->span != EyeSpan->value() || !same(eyeDiagram->start, start)
          || eyeDiagram->levels != levels || !same(eyeDiagram->threshold, threshold)
          || eyeDiagram->drawn != EyeDrawn->currentIndex() || eyeDiagram->measurements != EyeMeasure->isChecked()
          || !same(eyeDiagram->maskWidth, maskWidth) || !same(eyeDiagram->maskHeight, maskHeight)) {
        eyeDiagram->ui = ui;
        eyeDiagram->span = EyeSpan->value();
        eyeDiagram->start = start;
        eyeDiagram->levels = levels;
        eyeDiagram->threshold = threshold;
        eyeDiagram->drawn = EyeDrawn->currentIndex();
        eyeDiagram->measurements = EyeMeasure->isChecked();
        eyeDiagram->maskWidth = maskWidth;
        eyeDiagram->maskHeight = maskHeight;
        changed = true;
      }
      // Shown as they are now.
      for (QLineEdit *e : {EyeUi, EyeStart, EyeThreshold, EyeMaskWidth, EyeMaskHeight}) e->setProperty("qucsShown", e->text());
    }

    if (auto *stacked = dynamic_cast<StackedDiagram *>(Diag); stacked && PaneCount) {
      if (stacked->paneCount() != PaneCount->value()) {
        stacked->setPaneCount(PaneCount->value());
        changed = true;
      }
      if (GridLogX && Diag->xAxis.log != GridLogX->isChecked()) {
        Diag->xAxis.log = GridLogX->isChecked();
        changed = true;
      }
      // A step of 1, 2 or 5 times a power of ten, some five across.
      const auto niceStep = [](double from, double to) {
        const double span = std::abs(to - from);
        if (!(span > 0) || !std::isfinite(span)) return 1.0;
        const double power = std::pow(10.0, std::floor(std::log10(span / 5)));
        const double f = span / 5 / power;
        return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * power;
      };
      for (int i = 0; i < stacked->paneCount() && i < PaneTable->rowCount(); ++i) {
        int c = 0;
        for (Axis *a : {&stacked->pane(i).left, &stacked->pane(i).right}) {
          const QString label = PaneTable->item(i, c)->text();
          bool okFrom = false, okTo = false;
          const double from = PaneTable->item(i, c + 1)->text().toDouble(&okFrom);
          const double to = PaneTable->item(i, c + 2)->text().toDouble(&okTo);
          const bool log = PaneTable->item(i, c + 3)->checkState() == Qt::Checked;
          // Limits given both, and of use (above 0 on a log axis), else automatic.
          const bool manual = okFrom && okTo && from < to && (!log || from > 0);
          if (a->Label != label || a->log != log || a->autoScale == manual
              || (manual && (a->limit_min != from || a->limit_max != to))) {
            a->Label = label;
            a->log = log;
            a->autoScale = !manual;
            if (manual) {
              a->limit_min = from;
              a->limit_max = to;
              a->step = niceStep(from, to);
            }
            changed = true;
          }
          c += 4;
        }
      }
    }

    if (auto *bode = dynamic_cast<BodeDiagram *>(Diag); bode && BodeMargins && bode->margins != BodeMargins->isChecked()) {
      bode->margins = BodeMargins->isChecked();
      changed = true;
    }
    if (auto *spectrum = dynamic_cast<SpectrumDiagram *>(Diag); spectrum && SpecWindow) {
      const auto read = [](const QLineEdit *e) {
        const qucs_s::units::Reading r = qucs_s::units::read(e->text().trimmed());
        return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      };
      const auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
      const double from = read(SpecFrom);
      double fundamental = read(SpecFundamental);
      if (!(fundamental > 0)) fundamental = std::nan("");
      if (int(spectrum->window) != SpecWindow->currentIndex() || spectrum->harmonics != SpecHarmonics->value()
          || spectrum->dbc != SpecDbc->isChecked() || spectrum->stems != SpecStems->isChecked() || !same(spectrum->from, from)
          || !same(spectrum->fundamental, fundamental)) {
        spectrum->window = qucs_s::spectrum::Window(SpecWindow->currentIndex());
        spectrum->harmonics = SpecHarmonics->value();
        spectrum->dbc = SpecDbc->isChecked();
        spectrum->stems = SpecStems->isChecked();
        spectrum->from = from;
        spectrum->fundamental = fundamental;
        changed = true;
      }
    }
    if (auto *smith = dynamic_cast<SmithDiagram *>(Diag); smith && SmithCircles) {
      QList<SmithDiagram::CircleSpec> circles;
      QString why;
      if (!SmithDiagram::circlesFromText(SmithCircles->text(), &circles, &why)) {
        QMessageBox::warning(this, tr("Circles"), tr("The circles are kept as they were: %1.").arg(why));
        SmithCircles->setText(SmithDiagram::circlesText(smith->circles));
      } else {
        const qucs_s::units::Reading r = qucs_s::units::read(SmithFrequency->text().trimmed());
        const double frequency = r.kind == qucs_s::units::Reading::Number && r.value > 0 ? r.value : std::nan("");
        const bool same = (std::isnan(frequency) && std::isnan(smith->circleFrequency)) || frequency == smith->circleFrequency;
        if (circles != smith->circles || !same) {
          smith->circles = circles;
          smith->circleFrequency = frequency;
          changed = true;
        }
      }
    }
    if (auto *iq = dynamic_cast<ConstellationDiagram *>(Diag); iq && IqPeriod) {
      const auto read = [](const QLineEdit *e) {
        const qucs_s::units::Reading r = qucs_s::units::read(e->text().trimmed());
        return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      };
      const auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
      double period = read(IqPeriod);
      if (!(period > 0)) period = std::nan("");
      const double offset = read(IqOffset), from = read(IqFrom);
      if (!same(iq->period, period) || !same(iq->offset, offset) || !same(iq->from, from) || iq->modulation != IqModulation->currentIndex()) {
        iq->period = period;
        iq->offset = offset;
        iq->from = from;
        iq->modulation = IqModulation->currentIndex();
        changed = true;
      }
    }
    if (auto *boxes = dynamic_cast<BoxPlotDiagram *>(Diag); boxes && BoxAt) {
      const qucs_s::units::Reading r = qucs_s::units::read(BoxAt->text().trimmed());
      const double at = r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      const bool same = (std::isnan(at) && std::isnan(boxes->at)) || at == boxes->at;
      if (!same || boxes->range != BoxRange->isChecked()) {
        boxes->at = at;
        boxes->range = BoxRange->isChecked();
        changed = true;
      }
    }
    if (auto *tornado = dynamic_cast<TornadoDiagram *>(Diag); tornado && TornadoMode) {
      const qucs_s::units::Reading r = qucs_s::units::read(TornadoAt->text().trimmed());
      const double at = r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      const bool same = (std::isnan(at) && std::isnan(tornado->at)) || at == tornado->at;
      if (tornado->mode != TornadoMode->currentIndex() || !same || tornado->bars != TornadoBars->value()) {
        tornado->mode = TornadoMode->currentIndex();
        tornado->at = at;
        tornado->bars = TornadoBars->value();
        changed = true;
      }
    }
    if (auto *gram = dynamic_cast<SpectrogramDiagram *>(Diag); gram && GramWindow) {
      const qucs_s::units::Reading r = qucs_s::units::read(GramSegment->text().trimmed());
      double segment = r.kind == qucs_s::units::Reading::Number && r.value > 0 ? r.value : std::nan("");
      const bool same = (std::isnan(segment) && std::isnan(gram->segment)) || segment == gram->segment;
      if (int(gram->window) != GramWindow->currentIndex() || !same || gram->overlap != GramOverlap->value() || gram->range != GramRange->value()) {
        gram->window = qucs_s::spectrum::Window(GramWindow->currentIndex());
        gram->segment = segment;
        gram->overlap = GramOverlap->value();
        gram->range = GramRange->value();
        changed = true;
      }
    }
    if (auto *contour = dynamic_cast<ContourDiagram *>(Diag); contour && MapLevels) {
      const auto read = [](const QLineEdit *e) {
        const qucs_s::units::Reading r = qucs_s::units::read(e->text().trimmed());
        return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      };
      const auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
      double lo = read(MapPassMin), hi = read(MapPassMax);
      if (std::isfinite(lo) && std::isfinite(hi) && !(lo < hi)) {
        QMessageBox::warning(this, tr("Contour map"), tr("What passes goes from a value below the one it goes to: %1 to %2 is kept as it was.")
                                                          .arg(MapPassMin->text(), MapPassMax->text()));
        lo = contour->passMin;
        hi = contour->passMax;
      }
      if (contour->levels != MapLevels->value() || contour->map != MapColours->currentIndex() || contour->filled != MapFilled->isChecked()
          || contour->labels != MapLabels->isChecked() || !same(contour->passMin, lo) || !same(contour->passMax, hi)) {
        contour->levels = MapLevels->value();
        contour->map = MapColours->currentIndex();
        contour->filled = MapFilled->isChecked();
        contour->labels = MapLabels->isChecked();
        contour->passMin = lo;
        contour->passMax = hi;
        changed = true;
      }
    }
    if (auto *tub = dynamic_cast<BathtubDiagram *>(Diag); tub && TubUi) {
      const auto read = [](const QLineEdit *e) {
        const qucs_s::units::Reading r = qucs_s::units::read(e->text().trimmed());
        return r.kind == qucs_s::units::Reading::Number && std::isfinite(r.value) ? r.value : std::nan("");
      };
      const auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
      double ui = read(TubUi), ber = read(TubBer), floor = read(TubFloor);
      if (!(ui > 0)) ui = std::nan("");
      // (A rate out of range: the one it had, said.)
      if (!(ber > 0 && ber <= 0.01)) {
        QMessageBox::warning(this, tr("Bathtub"), tr("The bit error rate is above 0 and at most 0.01: \"%1\" is not one; it stays %2.")
                                                       .arg(TubBer->text(), QString::number(tub->ber, 'g', 3)));
        TubBer->setText(misc::num2str(tub->ber, -1, QString()));
        ber = tub->ber;
      }
      if (!(floor > 0 && floor < ber)) floor = std::nan("");
      const int levels = TubLevels->currentIndex() == 1 ? 2 : TubLevels->currentIndex() == 2 ? 4 : 0;
      const double from = read(TubFrom), threshold = read(TubThreshold);
      if (!same(tub->ui, ui) || !same(tub->start, from) || tub->levels != levels || !same(tub->threshold, threshold)
          || tub->ber != ber || !same(tub->floor, floor) || tub->measured != TubMeasured->isChecked()) {
        tub->ui = ui;
        tub->start = from;
        tub->levels = levels;
        tub->threshold = threshold;
        tub->ber = ber;
        tub->floor = floor;
        tub->measured = TubMeasured->isChecked();
        changed = true;
      }
    }
    if (auto *nichols = dynamic_cast<NicholsDiagram *>(Diag); nichols && NicholsGrid && nichols->grid != NicholsGrid->isChecked()) {
      nichols->grid = NicholsGrid->isChecked();
      changed = true;
    }
    if (auto *polar = dynamic_cast<PolarDiagram *>(Diag); polar && NyquistMarks
        && (polar->nyquist != NyquistMarks->isChecked() || polar->mirror != NyquistMirror->isChecked())) {
      polar->nyquist = NyquistMarks->isChecked();
      polar->mirror = NyquistMirror->isChecked();
      changed = true;
    }
    if (auto *pz = dynamic_cast<PoleZeroDiagram *>(Diag); pz && PzGuides && pz->guides != PzGuides->isChecked()) {
      pz->guides = PzGuides->isChecked();
      changed = true;
    }
    if (LimitTable) {
      QList<qucs_s::limits::Limit> limits;
      if (readLimits(&limits, false) && limits != Diag->limits) {
        Diag->limits = limits;
        changed = true;
      }
    }

    if ((Diag->Name.left(4) == "Rect") || (Diag->Name == "Curve")) {
      auto yUnit = Diag->yAxis.Units;
      if (yUnit != LogUnitsY->currentIndex()) {
        Diag->yAxis.Units = LogUnitsY->currentIndex();
        changed = true;
      }

      auto zUnit = Diag->zAxis.Units;
      if (zUnit != LogUnitsZ->currentIndex()) {
        Diag->zAxis.Units = LogUnitsZ->currentIndex();
        changed = true;
      }
    }

    if (GridOn)
      if (Diag->xAxis.GridOn != GridOn->isChecked()) {
        Diag->xAxis.GridOn = GridOn->isChecked();
        Diag->yAxis.GridOn = GridOn->isChecked();
        changed = true;
      }
    if (GridStyleBox)
      if (Diag->GridPen.style() !=
          (Qt::PenStyle)(GridStyleBox->currentIndex() + 1)) {
        Diag->GridPen.setStyle(
            (Qt::PenStyle)(GridStyleBox->currentIndex() + 1));
        changed = true;
      }
    if ((Diag->Name != "Smith") && (Diag->Name != "Polar") && (Diag->Name != "Histogram") && (Diag->Name != "Eye")
        && (Diag->Name != "Bathtub")) {
      if (Diag->zAxis.Label.isEmpty())
        Diag->zAxis.Label = ""; // can be not 0 and empty!
      if (yrLabel->text().isEmpty())
        yrLabel->setText("");
      if (Diag->zAxis.Label != yrLabel->text()) {
        Diag->zAxis.Label = yrLabel->text();
        changed = true;
      }
    }

    if (Diag->Name.left(4) == "Rect") {
      if (Diag->xAxis.log != GridLogX->isChecked()) {
        Diag->xAxis.log = GridLogX->isChecked();
        changed = true;
      }
      if (Diag->yAxis.log != GridLogY->isChecked()) {
        Diag->yAxis.log = GridLogY->isChecked();
        changed = true;
      }
      if (Diag->zAxis.log != GridLogZ->isChecked()) {
        Diag->zAxis.log = GridLogZ->isChecked();
        changed = true;
      }
    }

    if ((Diag->Name == "Smith") || (Diag->Name == "ySmith") ||
        (Diag->Name == "PS"))
      if (stopY->text().toDouble() < 1.0)
        stopY->setText("1");

    if (Diag->Name == "SP")
      if (stopZ->text().toDouble() < 1.0)
        stopZ->setText("1");

    if (Diag->xAxis.autoScale == manualX->isChecked()) {
      Diag->xAxis.autoScale = !(manualX->isChecked());
      changed = true;
    }

    // Use string compares for all floating point numbers, in
    // order to avoid rounding problems.
    if (QString::number(Diag->xAxis.limit_min) != startX->text()) {
      Diag->xAxis.limit_min = startX->text().toDouble();
      changed = true;
    }
    if (QString::number(Diag->xAxis.step) != stepX->text()) {
      Diag->xAxis.step = stepX->text().toDouble();
      changed = true;
    }
    if (QString::number(Diag->xAxis.limit_max) != stopX->text()) {
      Diag->xAxis.limit_max = stopX->text().toDouble();
      changed = true;
    }
    if (Diag->yAxis.autoScale == manualY->isChecked()) {
      Diag->yAxis.autoScale = !(manualY->isChecked());
      changed = true;
    }
    if (QString::number(Diag->yAxis.limit_min) != startY->text()) {
      Diag->yAxis.limit_min =
          qucs::db2num(startY->text().toDouble(), Diag->yAxis.Units);
      changed = true;
    }
    if (QString::number(Diag->yAxis.step) != stepY->text()) {
      Diag->yAxis.step =
          qucs::db2num(stepY->text().toDouble(), Diag->yAxis.Units);
      changed = true;
    }
    if (QString::number(Diag->yAxis.limit_max) != stopY->text()) {
      Diag->yAxis.limit_max =
          qucs::db2num(stopY->text().toDouble(), Diag->yAxis.Units);
      changed = true;
    }
    if (Diag->zAxis.autoScale == manualZ->isChecked()) {
      Diag->zAxis.autoScale = !(manualZ->isChecked());
      changed = true;
    }
    if (QString::number(Diag->zAxis.limit_min) != startZ->text()) {
      Diag->zAxis.limit_min =
          qucs::db2num(startZ->text().toDouble(), Diag->zAxis.Units);
      changed = true;
    }
    if (QString::number(Diag->zAxis.step) != stepZ->text()) {
      Diag->zAxis.step =
          qucs::db2num(stepZ->text().toDouble(), Diag->zAxis.Units);
      changed = true;
    }
    if (QString::number(Diag->zAxis.limit_max) != stopZ->text()) {
      Diag->zAxis.limit_max =
          qucs::db2num(stopZ->text().toDouble(), Diag->zAxis.Units);
      changed = true;
    }

    // for "rect3D"
    if (hideInvisible)
      if (((Rect3DDiagram *)Diag)->hideLines != hideInvisible->isChecked()) {
        ((Rect3DDiagram *)Diag)->hideLines = hideInvisible->isChecked();
        changed = true;
      }

    if (rotationX)
      if (((Rect3DDiagram *)Diag)->rotX != rotationX->text().toInt()) {
        ((Rect3DDiagram *)Diag)->rotX = rotationX->text().toInt();
        changed = true;
      }

    if (rotationY)
      if (((Rect3DDiagram *)Diag)->rotY != rotationY->text().toInt()) {
        ((Rect3DDiagram *)Diag)->rotY = rotationY->text().toInt();
        changed = true;
      }

    if (rotationZ)
      if (((Rect3DDiagram *)Diag)->rotZ != rotationZ->text().toInt()) {
        ((Rect3DDiagram *)Diag)->rotZ = rotationZ->text().toInt();
        changed = true;
      }

  } // of "if(Diag->Name != "Tab")"

  if (Diag->theme() != ofThisDiagram(a_theme)) {
    Diag->setTheme(a_theme);
    changed = true;
  }

  qDeleteAll(Diag->Graphs);
  Diag->Graphs.clear(); // delete the graphs

  for (std::unique_ptr<Graph> &graph : Graphs) {
    Diag->Graphs.append(graph.release()); // transfer the new graphs to diagram
  }
  Graphs.clear();

  Diag->loadGraphData(defaultDataSet);
  if (auto *doc = qobject_cast<Schematic *>(parent()))   // none when used standalone
    doc->viewport()->repaint();
  copyDiagramGraphs();
  if (changed)
    transfer = true; // changes have been applied ?
}

/*!
 * \brief Closes the dialog, accepting or rejecting changes based on transfer
 * flag.
 *
 * \post Dialog is closed with Accepted status if transfer=true, Rejected
 * otherwise \see slotOK(), slotApply(), reject()
 */
void DiagramDialog::slotCancel() {
  if (transfer) {
    done(QDialog::Accepted);
  } else {
    done(QDialog::Rejected);
  }
}

//-----------------------------------------------------------------
// To get really all close events (even <Escape> key).
void DiagramDialog::reject() { slotCancel(); }

/*!
 * \brief Opens a color picker dialog and sets the selected graph's color.
 *
 */
void DiagramDialog::slotSetColor() {
  QColor c =
      QColorDialog::getColor(misc::getWidgetBackgroundColor(ColorButt), this);
  if (!c.isValid())
    return;
  QString stylesheet =
      QStringLiteral("QPushButton {background-color: %1};").arg(c.name());
  ColorButt->setStyleSheet(stylesheet);
  misc::setPickerColor(ColorButt, c);

  int i = GraphList->currentRow();
  if (i < 0)
    return;

  Graphs.at(i)->Color = c;
  updateGraphListItem(i); // Update table display
  changed = true;
  toTake = false;
}

void DiagramDialog::slotSetAutoColor(bool on) {
  ColorButt->setEnabled(!on);
  const int i = GraphList->currentRow();
  if (i < 0)
    return;
  Graphs.at(i)->autoColor = on;
  updateGraphListItem(i);
  // The colors mean nothing without the values they stand for.
  if (on && LegendBox != nullptr && LegendBox->currentIndex() == Diagram::LegendOff)
    LegendBox->setCurrentIndex(Diagram::LegendTopRight);
  changed = true;
  toTake = false;
}

void DiagramDialog::slotSetGhost(bool on) {
  const int i = GraphList->currentRow();
  if (i < 0)
    return;
  Graphs.at(i)->ghost = on;
  changed = true;
  toTake = false;
}

void DiagramDialog::enableMarkerBox(const Graph *g) {
  if (MarkerBox == nullptr) return;
  const bool line = g != nullptr && g->Style >= GRAPHSTYLE_SOLID && g->Style <= GRAPHSTYLE_LONGDASH;
  MarkerBox->setEnabled(line);
  MarkerLabel->setEnabled(line);
}

void DiagramDialog::slotSetValuePart(int part) {
  const int i = GraphList->currentRow();
  if (i < 0)
    return;
  Graphs.at(i)->valuePart = Graph::ValuePart(part);
  Graphs.at(i)->lastLoaded = QDateTime();   // (read again for it)
  updateGraphListItem(i);
  changed = true;
  toTake = false;
}

void DiagramDialog::slotSetPointMarker(int marker) {
  const int i = GraphList->currentRow();
  if (i < 0)
    return;
  Graphs.at(i)->pointMarker = Graph::PointMarker(marker);
  updateGraphListItem(i);
  // The shapes mean nothing without the values they stand for.
  if (Graphs.at(i)->pointMarker == Graph::PointMarker::Auto && LegendBox != nullptr &&
      LegendBox->currentIndex() == Diagram::LegendOff)
    LegendBox->setCurrentIndex(Diagram::LegendTopRight);
  changed = true;
  toTake = false;
}

/*!
 * \brief Opens a color picker dialog and sets the grid color.
 *
 */

/*!
 * \brief Updates the selected graph's variable expression when GraphInput
 * changes.
 *
 * \param s Expression's text
 */
void DiagramDialog::slotResetToTake(const QString &s) {
  int i = GraphList->currentRow();
  if (i < 0)
    return;

  Graphs.at(i)->Var = s;
  updateGraphListItem(i);
  changed = true;
  toTake = false;
  updateXVar();
}

/*!
 * \brief Sets the thickness of the currently selected graph trace.
 *
 * This function updates the line thickness for the selected graph in the
 * graph list. The thickness value is stored in the Graph object and the
 * graph list item is updated to reflect the change visually.
 *
 * The function performs the following operations:
 * - Get the index of the selected row from the graph list
 * - Updates the thickness property of the corresponding Graph object
 * - Calls updateGraphListItem() to refresh the display
 * - Sets the changed flag to indicate unsaved modifications
 * - Resets the toTake flag to prevent unwanted insertions
 *
 * \param value The new thickness value in pixels
 *
 * Implementation details:
 * - Returns immediately if no graph is selected (row < 0)
 * - Changes are not applied to the diagram until slotApply() or slotOK()
 *   is called
 *
 * \see slotSetPrecision(), slotApply(), updateGraphListItem()
 */
void DiagramDialog::slotSetThickness(int value) {
  int i = GraphList->currentRow();
  if (i < 0)
    return;

  Graph *g = Graphs.at(i).get();
  g->Thick = value;

  updateGraphListItem(i);
  changed = true;
  toTake = false;
}

/*!
 * \brief Sets the decimal precision for the currently selected tabular graph.
 *
 * This function updates the number of decimal places displayed for numerical
 * values in tabular diagrams. The precision value is stored in the Graph
 * object and affects how numbers are formatted in the table output.
 *
 * The function performs the following operations:
 * - Gets the index of the selected row from the graph list
 * - Updates the Precision property of the corresponding Graph object
 * - Calls updateGraphListItem() to refresh the display
 * - Sets the changed flag to indicate unsaved modifications
 * - Resets the toTake flag to prevent unwanted insertions
 *
 * \see slotSetThickness(), slotSetNumMode(), slotApply()
 */
void DiagramDialog::slotSetPrecision(int value) {
  int i = GraphList->currentRow();
  if (i < 0)
    return;

  Graph *g = Graphs.at(i).get();
  g->Precision = value;

  updateGraphListItem(i);
  changed = true;
  toTake = false;
}

/*!
 * \brief Sets the number notation mode for tabular diagrams.
 *
 * \param Mode The number notation mode index
 */
void DiagramDialog::slotSetNumMode(int Mode) {
  int i = GraphList->currentRow();
  if (i < 0)
    return; // return, if no item selected

  Graphs.at(i)->numMode = Mode;
  changed = true;
  toTake = false;
}

/*!
 * \brief Enables or disables grid-related UI controls based on checkbox state.
 *
 * \param state The checkbox state (2=checked, Otherwise: unchecked)
 */
void DiagramDialog::slotSetGridBox(int state) {
  if (state == 2) {
    GridStyleBox->setEnabled(true);
    GridLabel2->setEnabled(true);
  } else {
    GridStyleBox->setEnabled(false);
    GridLabel2->setEnabled(false);
  }
}

/*!
 * \brief Sets the line style for the selected graph.
 *
 * \param style The style index from PropertyBox
 */
void DiagramDialog::slotSetGraphStyle(int style) {
  int i = GraphList->currentRow();
  if (i < 0)
    return;

  Graph *g = Graphs.at(i).get();
  g->Style = toGraphStyle(style);
  QUCS_ASSERT(g->Style != GRAPHSTYLE_INVALID);
  enableMarkerBox(g);   // markers go on lines, not on symbols

  updateGraphListItem(i); // Update table display
  changed = true;
  toTake = false;
}

/*!
 * \brief Makes a copy of all graphs from the diagram.
 *
 * Creates new Graph objects using sameNewOne() for editing without modifying
 * the original diagram until Apply is pressed.
 *
 */
void DiagramDialog::copyDiagramGraphs() {
  for (Graph *pg : Diag->Graphs)
    Graphs.emplace_back(pg->sameNewOne());
}

/*!
 * \brief Sets which y-axis the selected graph uses (left/right).
 *
 * \param axis The axis index (0 or 1)
 */
void DiagramDialog::slotSetYAxis(int axis) {
  int i = GraphList->currentRow();
  if (i < 0)
    return;

  setAxisIndex(Graphs.at(i).get(), axis);
  updateGraphListItem(i); // Update table display
  changed = true;
  toTake = false;
}

/*!
 * \brief Enables or disables X-axis limit controls based on manual mode
 * checkbox.
 *
 * \param state Checkbox state (2=manual, otherwise: auto)
 * \post X-axis start, step, stop controls are enabled/disabled
 */
void DiagramDialog::slotManualX(int state) {
  if (state == 2) {
    if ((Diag->Name.left(4) == "Rect") || (Diag->Name == "Curve"))
      startX->setEnabled(true);
    stopX->setEnabled(true);
    if (GridLogX)
      if (GridLogX->isChecked())
        return;
    stepX->setEnabled(true);
  } else {
    startX->setEnabled(false);
    stepX->setEnabled(false);
    stopX->setEnabled(false);
  }
}

/*!
 * \brief Enables or disables Y-axis limit controls based on manual mode
 * checkbox.
 *
 * \param state Checkbox state (2=manual, Otherwise: auto)
 * \post Y-axis start, step, stop controls are enabled/disabled
 */
void DiagramDialog::slotManualY(int state) {
  if (state == 2) {
    if ((Diag->Name.left(4) == "Rect") || (Diag->Name == "Curve"))
      startY->setEnabled(true);
    stopY->setEnabled(true);
    if (GridLogY)
      if (GridLogY->isChecked())
        return;
    stepY->setEnabled(true);
  } else {
    startY->setEnabled(false);
    stepY->setEnabled(false);
    stopY->setEnabled(false);
  }
}

/*!
 * \brief Enables or disables Z-axis limit controls based on manual mode
 * checkbox.
 *
 * \param state Checkbox state (2=manual, Otherwise: auto)
 * \post Z-axis start, step, stop controls are enabled/disabled
 */
void DiagramDialog::slotManualZ(int state) {
  if (state == 2) {
    if ((Diag->Name.left(4) == "Rect") || (Diag->Name == "Curve"))
      startZ->setEnabled(true);
    stopZ->setEnabled(true);
    if (GridLogZ)
      if (GridLogZ->isChecked())
        return;
    stepZ->setEnabled(true);
  } else {
    startZ->setEnabled(false);
    stepZ->setEnabled(false);
    stopZ->setEnabled(false);
  }
}

/*!
 Is called if the current tab of the QTabWidget changes.
*/
void DiagramDialog::slotChangeTab(int) {
  if (stepX == 0)
    return; // defined ?
  if (GridLogX) {
    if (GridLogX->isChecked())
      stepX->setEnabled(false);
    else if (manualX->isChecked())
      stepX->setEnabled(true);
  }
  if (GridLogY) {
    if (GridLogY->isChecked())
      stepY->setEnabled(false);
    else if (manualY->isChecked())
      stepY->setEnabled(true);
  }
  if (GridLogZ) {
    if (GridLogZ->isChecked())
      stepZ->setEnabled(false);
    else if (manualZ->isChecked())
      stepZ->setEnabled(true);
  }
}

/*!
 Is called when the slider for rotation angle is changed.
*/
void DiagramDialog::slotNewRotX(int Value) {
  rotationX->setText(QString::number(Value));
  DiagCross->rotX = float(Value) * pi / 180.0;
  DiagCross->update();
}

/*!
 Is called when the slider for rotation angle is changed.
*/
void DiagramDialog::slotNewRotY(int Value) {
  rotationY->setText(QString::number(Value));
  DiagCross->rotY = float(Value) * pi / 180.0;
  DiagCross->update();
}

/*!
 Is called when the slider for rotation angle is changed.
*/
void DiagramDialog::slotNewRotZ(int Value) {
  rotationZ->setText(QString::number(Value));
  DiagCross->rotZ = float(Value) * pi / 180.0;
  DiagCross->update();
}

/*!
 Is called when the number (text) for rotation angle is changed.
*/
void DiagramDialog::slotEditRotX(const QString &Text) {
  SliderRotX->setValue(Text.toInt());
  DiagCross->rotX = Text.toFloat() * pi / 180.0;
  DiagCross->update();
}

/*!
 Is called when the number (text) for rotation angle is changed.
*/
void DiagramDialog::slotEditRotY(const QString &Text) {
  SliderRotY->setValue(Text.toInt());
  DiagCross->rotY = Text.toFloat() * pi / 180.0;
  DiagCross->update();
}

/*!
 Is called when the number (text) for rotation angle is changed.
*/
void DiagramDialog::slotEditRotZ(const QString &Text) {
  SliderRotZ->setValue(Text.toInt());
  DiagCross->rotZ = Text.toFloat() * pi / 180.0;
  DiagCross->update();
}

void DiagramDialog::slotPlotVs(int) {
  QString s = GraphInput->text();
  // (All after the "@" of "plotted against", not that of a device's vector.)
  if (const int at = Graph::plotVsSeparator(s); at > 0) s.truncate(at);
  if (ChooseXVar->currentIndex() != 0) {
    s += "@" + ChooseXVar->currentText();
  }
  GraphInput->setText(s);
}

void DiagramDialog::updateXVar() {
  ChooseXVar->blockSignals(true);
  QString s = GraphInput->text();
  if (const int at = Graph::plotVsSeparator(s); at > 0) {
    QString xvar = s.mid(at + 1);
    int n = ChooseXVar->findText(xvar);
    if (n != -1)
      ChooseXVar->setCurrentIndex(n);
    else
      ChooseXVar->setCurrentIndex(0);
  } else {
    ChooseXVar->setCurrentIndex(0);
  }
  ChooseXVar->blockSignals(false);
}

void DiagramDialog::slotRecalcDbLimitsY() {
  int Units = LogUnitsY->currentIndex();
  startY->setText(QString::number(qucs::num2db(Diag->yAxis.limit_min, Units)));
  stepY->setText(QString::number(qucs::num2db(Diag->yAxis.step, Units)));
  stopY->setText(QString::number(qucs::num2db(Diag->yAxis.limit_max, Units)));
}

void DiagramDialog::slotRecalcDbLimitsZ() {
  int Units = LogUnitsZ->currentIndex();
  startZ->setText(QString::number(qucs::num2db(Diag->zAxis.limit_min, Units)));
  stepZ->setText(QString::number(qucs::num2db(Diag->zAxis.step, Units)));
  stopZ->setText(QString::number(qucs::num2db(Diag->zAxis.limit_max, Units)));
}

/*!
 * \brief Updates the autocomplete model for the GraphInput line edit.
 *
 * Populates the completer with all available variables from the ChooseVars
 * table, prefixing each variable name with the appropriate simulator identifier
 * (ngspice/, xyce/, or spopus/) and dataset name if different from the default
 * dataset.
 *
 * This method should be called whenever:
 * - A new dataset is selected (variables list changes)
 * - The simulator selection changes (prefix changes)
 * - Variables are loaded or refreshed from a data file
 *
 * The completer provides case-insensitive popup suggestions as the user types
 * in the GraphInput field, making it easier to select valid variable names.
 */

void DiagramDialog::updateCompleter() {
  QStringList varList;

  // Get current dataset and simulator prefix
  QFileInfo Info(defaultDataSet);
  QString datasetPrefix = "";
  if (chosenDataset() != Info.baseName())
    datasetPrefix = chosenDataset() + ":";

  QString simPrefix = "";
  if (ChooseSimulator->currentText() == "Ngspice") {
    simPrefix = "ngspice/";
  } else if (ChooseSimulator->currentText() == "Xyce") {
    simPrefix = "xyce/";
  } else if (ChooseSimulator->currentText() == "SpiceOpus") {
    simPrefix = "spopus/";
  }

  // Extract all variable names from ChooseVars table
  for (int i = 0; i < ChooseVars->rowCount(); i++) {
    QTableWidgetItem *item = ChooseVars->item(i, 0);
    if (item) {
      QString varName = item->text();
      varList << simPrefix + datasetPrefix + varName;
    }
  }

  // Update completer model
  QStringListModel *model = new QStringListModel(varList, graphCompleter);
  graphCompleter->setModel(model);
}

/*!
 * \brief Handles key press events
 *
 * Behavior:
 * - If a graph is selected in GraphList: Updates the selected graph with
 *   the current variable from GraphInput
 * - If no graph is selected: Creates a new graph with the variable from
 *   GraphInput
 * - If the Delete Key is pressed, it removes the graph selected in "GraphList"
 *
 * This avoids using the mouse to press "New Graph" or "Apply" to create graphs
 */
void DiagramDialog::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Delete) {
    if (GraphList->hasFocus() && GraphList->currentRow() >= 0) {
      slotDeleteGraph();
      event->accept();
      return;
    }
  }

  if (GraphInput->hasFocus() &&
      (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
    event->accept();

    QTimer::singleShot(0, this, [this]() {
      if (!GraphInput->text().isEmpty()) {
        int selectedRow = GraphList->currentRow();

        if (selectedRow >= 0) {
          GraphList->item(selectedRow, 0)->setText(GraphInput->text());
          Graphs.at(selectedRow)->Var = GraphInput->text();
          updateGraphListItem(selectedRow);
        } else {
          slotNewGraph();
        }

        GraphInput->setFocus();
      }
    });
    return;
  }

  QDialog::keyPressEvent(event);
}

/*!
 * \brief Updates a single row in the GraphList table to display current
 *        graph properties.
 *
 * This function synchronizes the visual representation in the GraphList table
 * with the actual Graph object properties. It updates all columns in the
 * specified row to reflect the current state of the corresponding graph.
 *
 * Column layout for plot-type diagrams (5 columns):
 * - Column 0: Variable name (always shown, stretches to fill space)
 * - Column 1: Color swatch (40px fixed width, displays graph color)
 * - Column 2: Style name (e.g., "solid", "dash", "stars")
 * - Column 3: Thickness value (numeric)
 * - Column 4: y-Axis assignment (e.g., "left", "right", "smith", "polar")
 *
 * Column layout for tabular/truth diagrams (1 column):
 * - Column 0: Variable name only
 *
 * The function handles:
 * - Creating table items if they don't exist
 * - Updating existing table items with new values
 * - Setting appropriate display flags (non-editable)
 * - Translating graph style enum to readable text
 * - Translating y-axis number to readable text
 *
 *
 * \param row The row index in GraphList and Graphs vector to update
 *
 * \see SelectGraph(), slotSetThickness(), slotSetPrecision()
 */
void DiagramDialog::updateGraphListItem(int row) {
  if (row < 0 || row >= (int)Graphs.size())
    return;
  if (row >= GraphList->rowCount())
    return; // Safety check

  Graph *g = Graphs.at(row).get();
  if (!g)
    return; // Safety check

  // Column 0: Variable name (always shown)
  QTableWidgetItem *varItem = GraphList->item(row, 0);
  if (!varItem) {
    varItem = new QTableWidgetItem(g->withValuePart(g->Var));
    varItem->setFlags(varItem->flags() ^ Qt::ItemIsEditable);
    GraphList->setItem(row, 0, varItem);
  } else {
    varItem->setText(g->withValuePart(g->Var));
  }

  // Only show trace properties if we have more than 1 column
  if (GraphList->columnCount() > 1 && Diag->Name != "Tab" &&
      Diag->Name != "Truth") {
    // Column 1: Color swatch
    QTableWidgetItem *colorItem = GraphList->item(row, 1);
    if (!colorItem) {
      colorItem = new QTableWidgetItem();
      colorItem->setFlags(colorItem->flags() ^ Qt::ItemIsEditable);
      GraphList->setItem(row, 1, colorItem);
    }
    if (g->autoColor && Graph::autoColorApplies(Diag->Name)) {
      // The first colors of the palette, in stripes.
      QPixmap stripes(16, 12);
      QPainter p(&stripes);
      const QList<QColor> &palette = Graph::autoPalette();
      for (int k = 0; k < 4; ++k)
        p.fillRect(k * 4, 0, 4, 12, palette.at(k));
      p.end();
      colorItem->setBackground(QBrush());
      colorItem->setIcon(QIcon(stripes));
      colorItem->setText(tr("auto"));
    } else {
      colorItem->setIcon(QIcon());
      colorItem->setText(QString());
      colorItem->setBackground(QBrush(g->Color));
    }

    // Column 2: Style
    QString styleName;
    switch (g->Style) {
    case GRAPHSTYLE_SOLID:
      styleName = tr("solid");
      break;
    case GRAPHSTYLE_DASH:
      styleName = tr("dash");
      break;
    case GRAPHSTYLE_DOT:
      styleName = tr("dot");
      break;
    case GRAPHSTYLE_LONGDASH:
      styleName = tr("long dash");
      break;
    case GRAPHSTYLE_STAR:
      styleName = tr("stars");
      break;
    case GRAPHSTYLE_CIRCLE:
      styleName = tr("circles");
      break;
    case GRAPHSTYLE_ARROW:
      styleName = tr("arrows");
      break;
    default:
      styleName = "";
      break;
    }
    if (g->pointMarker != Graph::PointMarker::None && MarkerBox != nullptr &&
        g->Style >= GRAPHSTYLE_SOLID && g->Style <= GRAPHSTYLE_LONGDASH)
      styleName += " + " + MarkerBox->itemText(int(g->pointMarker));
    QTableWidgetItem *styleItem = GraphList->item(row, 2);
    if (!styleItem) {
      styleItem = new QTableWidgetItem(styleName);
      styleItem->setFlags(styleItem->flags() ^ Qt::ItemIsEditable);
      GraphList->setItem(row, 2, styleItem);
    } else {
      styleItem->setText(styleName);
    }

    // Column 3: Thickness
    QTableWidgetItem *thickItem = GraphList->item(row, 3);
    if (!thickItem) {
      thickItem = new QTableWidgetItem(QString::number(g->Thick));
      thickItem->setFlags(thickItem->flags() ^ Qt::ItemIsEditable);
      GraphList->setItem(row, 3, thickItem);
    } else {
      thickItem->setText(QString::number(g->Thick));
    }

    // Column 4: y-Axis (if applicable)
    const QString axisName = axisNameOf(g);
    QTableWidgetItem *axisItem = GraphList->item(row, 4);
    if (!axisItem) {
      axisItem = new QTableWidgetItem(axisName);
      axisItem->setFlags(axisItem->flags() ^ Qt::ItemIsEditable);
      GraphList->setItem(row, 4, axisItem);
    } else {
      axisItem->setText(axisName);
    }
  }
}

// ---------------------------------------------------------------------------
// The Theme tab.

theme::Theme DiagramDialog::ofThisDiagram(const theme::Theme &t) const {
  theme::Theme mine;
  for (Part p : Diag->themeParts())
    mine.choose(p, t.chosen(p));
  return mine;
}

QWidget *DiagramDialog::makeThemeTab(const QString &nameY, const QString &nameZ) {
  auto *tab = new QWidget();
  tab->setObjectName(QStringLiteral("diagramTheme"));
  auto *columns = new QHBoxLayout(tab);
  auto *left = new QVBoxLayout();
  columns->addLayout(left);

  // A ready-made theme to start from; "Custom" when the colours are no
  // one's (the item data: a Preset, -1 the default, -2 custom).
  auto *presetRow = new QHBoxLayout();
  presetRow->addWidget(new QLabel(tr("Theme:")));
  a_themePreset = new QComboBox();
  a_themePreset->setObjectName(QStringLiteral("diagramThemePreset"));
  for (const auto &[preset, name] : theme::presets())
    a_themePreset->addItem(name, int(preset));
  a_themePreset->addItem(tr("My default for new diagrams"), -1);
  a_themePreset->addItem(tr("Custom"), -2);
  presetRow->addWidget(a_themePreset, 1);
  left->addLayout(presetRow);
  connect(a_themePreset, &QComboBox::activated, this, [this](int index) {
    const int which = a_themePreset->itemData(index).toInt();
    if (which == -2) return;
    setTheme(which == -1 ? theme::defaultForNewDiagrams() : theme::preset(theme::Preset(which)));
  });

  // A row for each part it has, in sections: a button showing its colour
  // (and "Automatic" when it is), and a reset back to automatic.
  const QList<Part> parts = Diag->themeParts();
  const bool table = parts.contains(Part::Text);
  const QString axisTip = tr("Its ticks, numbers and label (the names of the traces keep their colours)");
  struct Entry {
    Part part;
    QString label, tip;
  };
  const QList<QPair<QString, QList<Entry>>> sections = {
      {tr("Areas"),
       {{Part::Background, tr("Background:"),
         tr("Under all of it: its frame, numbers, labels and title. Automatic: none on light paper, white on dark "
            "paper")},
        {Part::PlotArea, tr("Plot area:"), tr("Inside its frame. Automatic: the background")}}},
      {tr("Lines"),
       {{Part::Frame, table ? tr("Rules:") : tr("Frame:"),
         table ? tr("The table's frame and rules") : tr("The frame around the plot area")},
        {Part::Grid, tr("Grid:"), tr("The grid lines (shown or not on the Properties tab)")}}},
      {tr("Axes"),
       {{Part::XAxis, tr("x-Axis:"), axisTip},
        {Part::YAxis, (nameY.isEmpty() ? tr("y-Axis") : nameY) + QLatin1Char(':'), axisTip},
        {Part::RightAxis, (nameZ.isEmpty() ? tr("right Axis") : nameZ) + QLatin1Char(':'), axisTip}}},
      {tr("Texts"),
       {{Part::Title, tr("Title:"), tr("The title above it (Properties tab)")},
        {Part::Text, tr("Text:"), tr("The table's texts")}}},
      {tr("Legend"),
       {{Part::LegendBackground, tr("Background:"), tr("The legend's box (and a histogram's statistics)")},
        {Part::LegendBorder, tr("Border:"), tr("The line around the legend")},
        {Part::LegendText, tr("Text:"), tr("The names of the traces in the legend")}}},
  };
  auto *grid = new QGridLayout();
  grid->setColumnMinimumWidth(0, 12);
  int row = 0;
  for (const auto &[section, entries] : sections) {
    if (std::none_of(entries.begin(), entries.end(), [&parts](const Entry &e) { return parts.contains(e.part); }))
      continue;
    grid->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(section.toHtmlEscaped())), row++, 0, 1, 4);
    for (const Entry &e : entries) {
      if (!parts.contains(e.part)) continue;
      auto *label = new QLabel(e.label);
      label->setToolTip(e.tip);
      auto *button = new QPushButton();
      button->setObjectName(QStringLiteral("theme_") + theme::keyOf(e.part));
      button->setIconSize(QSize(28, 16));
      button->setToolTip(e.tip);
      auto *reset = new QToolButton();
      reset->setObjectName(QStringLiteral("theme_%1_auto").arg(theme::keyOf(e.part)));
      reset->setText(tr("Reset"));
      reset->setToolTip(tr("Back to automatic"));
      grid->addWidget(label, row, 1);
      grid->addWidget(button, row, 2);
      grid->addWidget(reset, row, 3);
      ++row;
      const Part part = e.part;
      const QString name = QString(e.label).remove(QLatin1Char(':'));
      connect(button, &QPushButton::clicked, this, [this, part, name] {
        const QColor c = QColorDialog::getColor(shownColor(part), this, tr("%1 Color").arg(name),
                                                QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        theme::Theme t = a_theme;
        t.choose(part, c);
        setTheme(t);
      });
      connect(reset, &QToolButton::clicked, this, [this, part] {
        theme::Theme t = a_theme;
        t.choose(part, QColor());
        setTheme(t);
      });
      a_themeRows.append({part, button, reset});
    }
  }
  grid->setColumnStretch(2, 1);
  left->addLayout(grid);

  auto *save = new QPushButton(tr("Save as Default for New Diagrams"));
  save->setObjectName(QStringLiteral("diagramThemeSaveDefault"));
  save->setToolTip(tr("The diagrams you place from now on start with these colours"));
  connect(save, &QPushButton::clicked, this, [this] {
    theme::setDefaultForNewDiagrams(a_theme);
    showTheme();
  });
  left->addWidget(save);
  auto *note = new QLabel(tr("Automatic: as Qucs-S has always drawn it, in colours that show on the background "
                             "chosen. The traces keep their colours (Data tab), and the markers theirs."));
  note->setWordWrap(true);
  left->addWidget(note);
  left->addStretch(1);

  a_themePreview = new ThemePreview(Diag, a_canvas);
  columns->addWidget(a_themePreview, 1);

  a_theme = Diag->theme();
  showTheme();
  return tab;
}

void DiagramDialog::setTheme(const theme::Theme &t) {
  a_theme = t;
  showTheme();
}

QColor DiagramDialog::shownColor(Part part) const {
  const QColor chosen = a_theme.chosen(part);
  if (chosen.isValid()) return chosen;
  // An automatic one: as it is drawn now (the areas: what shows there).
  const theme::Colors shown = theme::colorsOn(ofThisDiagram(a_theme), a_canvas);
  return part == Part::Background ? shown.outside : part == Part::PlotArea ? shown.inside : shown.of(part);
}

void DiagramDialog::showTheme() {
  for (const ThemeRow &r : std::as_const(a_themeRows)) {
    const QColor chosen = a_theme.chosen(r.part);
    r.button->setIcon(colorSwatch(shownColor(r.part)));
    r.button->setText(chosen.isValid() ? theme::colorText(chosen).toUpper() : tr("Automatic"));
    r.reset->setEnabled(chosen.isValid());
  }

  // The ready-made theme the colours are, or Custom; the default only
  // when one was saved.
  const theme::Theme mine = ofThisDiagram(a_theme);
  const theme::Theme saved = theme::defaultForNewDiagrams();
  auto *model = qobject_cast<QStandardItemModel *>(a_themePreset->model());
  int match = -1, custom = -1;
  for (int i = 0; i < a_themePreset->count(); ++i) {
    const int which = a_themePreset->itemData(i).toInt();
    if (which == -2) {
      custom = i;
      continue;
    }
    if (which == -1 && model != nullptr) model->item(i)->setEnabled(!saved.isAutomatic());
    if (which == -1 && saved.isAutomatic()) continue;
    const theme::Theme t = which == -1 ? saved : theme::preset(theme::Preset(which));
    if (match < 0 && ofThisDiagram(t) == mine) match = i;
  }
  if (model != nullptr && custom >= 0) model->item(custom)->setEnabled(match < 0);
  a_themePreset->setCurrentIndex(match >= 0 ? match : custom);
  if (a_themePreview != nullptr) a_themePreview->show(ofThisDiagram(a_theme));
}

void DiagramDialog::fillAxisBox() {
  auto *stacked = dynamic_cast<StackedDiagram *>(Diag);
  if (!yAxisBox || !stacked) return;
  yAxisBox->clear();
  const int n = PaneCount ? PaneCount->value() : stacked->paneCount();
  for (int i = 1; i <= n; ++i) {
    yAxisBox->addItem(tr("pane %1, left").arg(i));
    yAxisBox->addItem(tr("pane %1, right").arg(i));
  }
}

int DiagramDialog::axisIndexOf(const Graph *g) const {
  if (Diag->Name == "Stacked") return 2 * g->pane + (g->yAxisNo == 0 ? 0 : 1);
  return g->yAxisNo;
}

void DiagramDialog::setAxisIndex(Graph *g, int index) const {
  if (index < 0) index = 0;
  if (Diag->Name == "Stacked") {
    g->pane = index / 2;
    g->yAxisNo = index % 2;
    return;
  }
  g->yAxisNo = index;
}

QString DiagramDialog::axisNameOf(const Graph *g) const {
  if (!yAxisBox) return QString();
  if (Diag->Name == "Stacked")
    return tr("pane %1, %2").arg(g->pane + 1).arg(g->yAxisNo == 0 ? tr("left") : tr("right"));
  if ((Diag->Name == "Rect") || (Diag->Name == "Curve")) return (g->yAxisNo == 0) ? tr("left") : tr("right");
  if (Diag->Name == "PS" || Diag->Name == "SP") return (g->yAxisNo == 0) ? tr("smith") : tr("polar");
  return (g->yAxisNo == 0) ? tr("y") : tr("z");
}

void DiagramDialog::setPaneRows(int n) {
  if (!PaneTable) return;
  const int was = PaneTable->rowCount();
  PaneTable->setRowCount(n);
  for (int i = was; i < n; ++i) {
    PaneTable->setVerticalHeaderItem(i, new QTableWidgetItem(tr("pane %1").arg(i + 1)));
    for (int c = 0; c < 8; ++c) {
      auto *item = new QTableWidgetItem();
      if (c == 3 || c == 7) {
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        item->setCheckState(Qt::Unchecked);
      }
      PaneTable->setItem(i, c, item);
    }
  }
}

void DiagramDialog::addLimitRow(const qucs_s::limits::Limit &limit) {
  const int row = LimitTable->rowCount();
  LimitTable->setRowCount(row + 1);
  auto *side = new QComboBox();
  side->addItem(tr("upper"));
  side->addItem(tr("lower"));
  side->setCurrentIndex(limit.side == qucs_s::limits::Limit::Lower ? 1 : 0);
  LimitTable->setCellWidget(row, 0, side);
  QStringList points;
  for (const QPointF &p : limit.points)
    points << (limit.isLevel() ? misc::num2str(p.y(), -1, QString()) : misc::num2str(p.x(), -1, QString()) + ", " + misc::num2str(p.y(), -1, QString()));
  LimitTable->setItem(row, 1, new QTableWidgetItem(points.join(QStringLiteral("; "))));
  LimitTable->setItem(row, 2, new QTableWidgetItem(limit.label));
  auto *axis = new QComboBox();
  axis->addItem(tr("left"));
  axis->addItem(tr("right"));
  axis->setCurrentIndex(limit.axis == 1 ? 1 : 0);
  LimitTable->setCellWidget(row, 3, axis);
  if (LimitTable->columnCount() > 4) {
    auto *pane = new QSpinBox();
    auto *stacked = dynamic_cast<StackedDiagram *>(Diag);
    pane->setRange(1, stacked ? StackedDiagram::MaxPanes : 1);
    pane->setValue(limit.pane + 1);
    LimitTable->setCellWidget(row, 4, pane);
  }
}

bool DiagramDialog::readLimits(QList<qucs_s::limits::Limit> *limits, bool warn) {
  limits->clear();
  const auto number = [](const QString &text, double *v) {
    const qucs_s::units::Reading r = qucs_s::units::read(text.trimmed());
    if (r.kind != qucs_s::units::Reading::Number || !std::isfinite(r.value)) return false;
    *v = r.value;
    return true;
  };
  for (int row = 0; row < LimitTable->rowCount(); ++row) {
    qucs_s::limits::Limit l;
    auto *side = qobject_cast<QComboBox *>(LimitTable->cellWidget(row, 0));
    l.side = side && side->currentIndex() == 1 ? qucs_s::limits::Limit::Lower : qucs_s::limits::Limit::Upper;
    const QString text = LimitTable->item(row, 1) ? LimitTable->item(row, 1)->text().trimmed() : QString();
    QString why;
    const QStringList pairs = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (pairs.size() == 1 && !pairs.first().contains(QLatin1Char(','))) {
      double y = 0;
      if (!number(pairs.first(), &y)) why = tr("%1 is no number.").arg(pairs.first().trimmed());
      else l.points << QPointF(0, y);
    } else {
      for (const QString &pair : pairs) {
        double x = 0, y = 0;
        if (!number(pair.section(QLatin1Char(','), 0, 0), &x) || !number(pair.section(QLatin1Char(','), 1, 1), &y)) {
          why = tr("%1 is no point: x, y.").arg(pair.trimmed());
          break;
        }
        if (!l.points.isEmpty() && x < l.points.last().x()) {
          why = tr("The points go with x rising.");
          break;
        }
        l.points << QPointF(x, y);
      }
      if (why.isEmpty() && l.points.size() < 2) why = tr("A limit is a level (a number) or two points or more.");
    }
    l.label = LimitTable->item(row, 2) ? LimitTable->item(row, 2)->text().trimmed() : QString();
    if (l.label.contains(QLatin1Char('"'))) why = tr("A label has no double quotes.");
    auto *axis = qobject_cast<QComboBox *>(LimitTable->cellWidget(row, 3));
    l.axis = axis && axis->currentIndex() == 1 ? 1 : 0;
    if (auto *pane = qobject_cast<QSpinBox *>(LimitTable->cellWidget(row, 4))) l.pane = pane->value() - 1;
    if (!why.isEmpty()) {
      if (warn) {
        QMessageBox::warning(this, tr("Limits"), tr("Limit %1 cannot be taken. %2").arg(row + 1).arg(why));
        LimitTable->setCurrentCell(row, 1);
      }
      return false;
    }
    *limits << l;
  }
  return true;
}

void DiagramDialog::addSensitivityParts()
{
  // The Data tab's variables: a part's chosen as a sensitivity run's are,
  // taken as a double click on it takes it (those shown already left).
  QStringList names;
  QHash<QString, QTableWidgetItem *> items;
  for (int row = 0; row < ChooseVars->rowCount(); ++row)
    if (QTableWidgetItem *item = ChooseVars->item(row, 0)) {
      names << item->text();
      items.insert(item->text(), item);
    }
  QStringList shown;
  for (int row = 0; row < GraphList->rowCount(); ++row)
    if (QTableWidgetItem *item = GraphList->item(row, 0)) shown << item->text().section('/', -1);
  int added = 0;
  for (const QString &part : TornadoDiagram::sensitivityParts(names)) {
    if (shown.contains(part)) continue;
    slotTakeVar(items.value(part));
    ++added;
  }
  if (added == 0)
    QMessageBox::information(this, tr("Tornado chart"),
                             tr("The run chosen on the Data tab has no part of a sensitivity run not shown already "
                                "(ngspice's .SENS: r1, r1_scale, v1, ...)."));
}
