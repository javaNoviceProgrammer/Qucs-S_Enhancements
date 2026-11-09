/*
 * qucs_multiphysics3d.cpp - Multiphysics 3D in the window: the left dock's
 *                           Multiphysics 3D tab, the Multiphysics 3D menu and
 *                           its Settings, a new 3D model, a 3D model's file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucs.h"

#include "multiphysics3ddoc.h"
#include "multiphysics3dpanel.h"
#include "multiphysicspanel.h"
#include "multiphysicssettings.h"
#include "settings.h"

#include "main.h"

#include "fem3_estimate.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

extern QString lastDirOpenSave;   // (qucs.cpp: the last folder a file was opened from)

namespace {

/// Multiphysics 3D > Settings: the memory a solve may take, past which it is
/// not solved (half of this machine's by default; 0, no limit), and the
/// sparse direct solver - the Multiphysics menu's own setting, the same for
/// both (Multiphysics/solver). Opened, not run: the window goes on behind it.
void showMultiphysics3DSettings(QWidget *parent)
{
  using qucs_s::fem::bytesText;
  using qucs_s::fem::physicalMemory;
  using qucs_s::fem::MultiphysicsSettingsDialog;
  auto *dialog = new QDialog(parent);
  dialog->setObjectName(QStringLiteral("multiphysics3DSettings"));
  dialog->setWindowTitle(QObject::tr("Multiphysics 3D Settings"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto *layout = new QVBoxLayout(dialog);
  auto *memory = new QGroupBox(QObject::tr("Memory"), dialog);
  auto *memoryRows = new QVBoxLayout(memory);
  auto *line = new QHBoxLayout();
  line->addWidget(new QLabel(QObject::tr("Memory a solve may take:"), dialog));
  auto *limit = new QDoubleSpinBox(dialog);
  limit->setObjectName(QStringLiteral("mp3MemoryLimit"));
  limit->setRange(0, 1e5);
  limit->setDecimals(1);
  limit->setSuffix(QObject::tr(" GB"));
  limit->setSpecialValueText(QObject::tr("No limit"));
  limit->setValue(Multiphysics3DDoc::memoryLimit() / (1024.0 * 1048576.0));
  line->addWidget(limit, 1);
  const double half = physicalMemory() / 2;
  auto *byDefault = new QPushButton(QObject::tr("Half of this machine's"), dialog);
  byDefault->setObjectName(QStringLiteral("mp3MemoryDefault"));
  byDefault->setToolTip(QObject::tr("%1 of its %2").arg(bytesText(half), bytesText(2 * half)));
  QObject::connect(byDefault, &QPushButton::clicked, limit, [limit, half] { limit->setValue(half / (1024.0 * 1048576.0)); });
  line->addWidget(byDefault);
  memoryRows->addLayout(line);
  auto *about = new QLabel(QObject::tr("Each solve is estimated before it starts - each physics' unknowns, its matrix's nonzeros, "
                                       "the memory and time of an iterative and of a direct solve - and one past this is not "
                                       "solved: the messages say what would bring it under (the iterative solver, a coarser mesh, "
                                       "fewer divisions in z). Build Mesh says the estimate too. Claude's fem_solve may go past it "
                                       "(force)."),
                           memory);
  about->setWordWrap(true);
  memoryRows->addWidget(about);
  layout->addWidget(memory);
  // The sparse direct solver: the Multiphysics menu's setting, shown and set
  // here too.
  auto *solver = new QGroupBox(QObject::tr("Solver"), dialog);
  auto *solverRows = new QVBoxLayout(solver);
  auto *solverLine = new QHBoxLayout();
  solverLine->addWidget(new QLabel(QObject::tr("Sparse direct solver:"), solver));
  auto *choice = new QComboBox(solver);
  choice->setObjectName(QStringLiteral("mp3SolverBox"));
  MultiphysicsSettingsDialog::addSolverChoices(choice);
  choice->setCurrentIndex(MultiphysicsSettingsDialog::solverWanted());
  solverLine->addWidget(choice, 1);
  solverRows->addLayout(solverLine);
  auto *how = new QLabel(QObject::tr("The same setting as the Multiphysics menu's: Apple's Accelerate, MUMPS and STRUMPACK "
                                     "factorize on many threads, Eigen's on one (the quicker below some 20,000 unknowns); "
                                     "Accelerate's LU needs macOS 26 or later, its Cholesky any macOS; MUMPS and STRUMPACK are "
                                     "there where this build has them (Linux's and Windows' releases do). In 3D it is what "
                                     "factorizes where a solve factorizes: heat, "
                                     "electrostatics and currents by Cholesky where a matrix has many right-hand sides (a "
                                     "capacitance matrix's) or the step asks for the direct solver - else they iterate "
                                     "(multigrid) -, an eigenmode expansion's cuts, each mode solve an LU, and a full-wave solve's system "
                                     "(Electromagnetic Waves, Frequency Domain): Accelerate's LDLT of a complex symmetric matrix, "
                                     "macOS 26 or later, or MUMPS's, else an LU; a time-domain one's (Time Domain) a Cholesky, "
                                     "factorized once for all its steps. Where the Cholesky is a multithreaded one, heat and "
                                     "electrostatics of many right-hand sides factorize by themselves. QUCS_FEM_SOLVER=eigen, "
                                     "accelerate, mumps, strumpack or auto in the environment chooses, whatever this says."),
                         solver);
  how->setWordWrap(true);
  solverRows->addWidget(how);
  auto *status = new QLabel(solver);
  status->setObjectName(QStringLiteral("mp3SolverStatus"));
  status->setWordWrap(true);
  status->setText(MultiphysicsSettingsDialog::solverStatus(choice->currentIndex()));
  QObject::connect(choice, &QComboBox::currentIndexChanged, status,
                   [status](int index) { status->setText(MultiphysicsSettingsDialog::solverStatus(index)); });
  solverRows->addWidget(status);
  layout->addWidget(solver);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, limit, choice] {
    Multiphysics3DDoc::setMemoryLimitGB(limit->value());
    QucsSettingsFile().setValue(QStringLiteral("Multiphysics/solver"), choice->currentData().toString());
    MultiphysicsSettingsDialog::applySolverSetting();
    dialog->accept();
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog->open();
}

} // namespace

bool QucsApp::isMultiphysics3DDocument(QWidget *w)
{
  return w != nullptr && w->inherits("Multiphysics3DDoc");
}

bool QucsApp::isMultiphysics3DFile(const QString &name)
{
  return QFileInfo(name).suffix().compare(QLatin1String("qfem3d"), Qt::CaseInsensitive) == 0;
}

void QucsApp::initMultiphysics3D()
{
  // A tab of the left dock, after Multiphysics: the tree of the 3D model in
  // front. Its page is SidePage::Multiphysics3D, never a number.
  a_multiphysics3DPanel = new Multiphysics3DPanel;
  const int after = TabView->indexOf(a_multiphysicsPanel);
  const int at = after >= 0 ? after + 1 : TabView->count();
  TabView->insertTab(at, a_multiphysics3DPanel, tr("Multiphysics 3D"));
  a_sidePages.insert(int(SidePage::Multiphysics3D), a_multiphysics3DPanel);
  TabView->setTabToolTip(TabView->indexOf(a_multiphysics3DPanel),
                         tr("a 3D model's tree: its solids, materials and mesh"));
  connect(a_multiphysics3DPanel, &Multiphysics3DPanel::newModelRequested, this, [this] { newMultiphysics3DModel(); });
  connect(a_multiphysics3DPanel, &Multiphysics3DPanel::openModelRequested, this, [this](const QString &given) {
    QString path = given;
    if (path.isEmpty())
      path = QFileDialog::getOpenFileName(this, tr("Open 3D Model"),
                                          lastDirOpenSave.isEmpty() ? QucsSettings.qucsWorkspaceDir.absolutePath() : lastDirOpenSave,
                                          tr("3D multiphysics models") + QStringLiteral(" (*.qfem3d)"));
    if (!path.isEmpty()) openFileFromProjectView(QFileInfo(path), QString());
  });

  // The Multiphysics 3D menu, after Multiphysics.
  a_multiphysics3DMenu = new QMenu(tr("Multiphysics &3D"), this);
  a_multiphysics3DMenu->setObjectName(QStringLiteral("multiphysics3DMenu"));
  a_multiphysics3DMenu->setToolTipsVisible(true);
  auto model = [this] { return qobject_cast<Multiphysics3DDoc *>(DocumentTab->currentWidget()); };
  QAction *newModel = a_multiphysics3DMenu->addAction(tr("New 3D Model"), this, [this] { newMultiphysics3DModel(); });
  newModel->setObjectName(QStringLiteral("mp3NewModelAction"));
  newModel->setStatusTip(tr("A new 3D multiphysics model in a tab of its own"));
  a_multiphysics3DMenu->addAction(tr("Open Model…"), this, [this] { emit a_multiphysics3DPanel->openModelRequested(QString()); });
  a_multiphysics3DMenu->addSeparator();
  QAction *geometry = a_multiphysics3DMenu->addAction(tr("Build Geometry"), this, [model] {
    if (Multiphysics3DDoc *doc = model()) {
      doc->showGeometry();
      doc->requestGeometry();
    }
  });
  geometry->setObjectName(QStringLiteral("mp3BuildGeometryAction"));
  QAction *mesh = a_multiphysics3DMenu->addAction(tr("Build Mesh"), this, [model] {
    if (Multiphysics3DDoc *doc = model()) doc->buildMesh();
  });
  mesh->setObjectName(QStringLiteral("mp3BuildMeshAction"));
  QAction *compute = a_multiphysics3DMenu->addAction(tr("Compute"), this, [model] {
    if (Multiphysics3DDoc *doc = model()) doc->compute();
  });
  compute->setObjectName(QStringLiteral("mp3ComputeAction"));
  compute->setStatusTip(tr("Solve the 3D model's study: heat, electrostatics, electric currents"));
  a_multiphysics3DMenu->addSeparator();
  // (No role, as the Multiphysics menu's: on macOS a "Settings…" of the
  // text-heuristic role is taken into the application's menu as its
  // Preferences - and this one was, out of the Multiphysics 3D menu.)
  QAction *settings = a_multiphysics3DMenu->addAction(tr("Settings…"), this, [this] { showMultiphysics3DSettings(this); });
  settings->setObjectName(QStringLiteral("mp3SettingsAction"));
  settings->setMenuRole(QAction::NoRole);
  settings->setStatusTip(tr("The memory a 3D solve may take, and the sparse direct solver"));
  a_multiphysics3DMenu->addAction(tr("Show the Multiphysics 3D Panel"), this, [this] { showMultiphysics3DPanel(); });
  connect(a_multiphysics3DMenu, &QMenu::aboutToShow, this, &QucsApp::updateMultiphysics3DMenu);
  const QList<QAction *> bar = menuBar()->actions();
  int place = a_multiphysicsMenu != nullptr ? int(bar.indexOf(a_multiphysicsMenu->menuAction())) : -1;
  if (place >= 0 && place + 1 < bar.size()) menuBar()->insertMenu(bar.at(place + 1), a_multiphysics3DMenu);
  else {
    const int view = int(bar.indexOf(viewMenu->menuAction()));
    if (view >= 0) menuBar()->insertMenu(bar.at(view), a_multiphysics3DMenu);
    else menuBar()->addMenu(a_multiphysics3DMenu);
  }
}

void QucsApp::updateMultiphysics3DMenu()
{
  auto *doc = qobject_cast<Multiphysics3DDoc *>(DocumentTab->currentWidget());
  for (QAction *a : a_multiphysics3DMenu->actions())
    if (a->objectName() == QLatin1String("mp3BuildGeometryAction") || a->objectName() == QLatin1String("mp3BuildMeshAction")
        || a->objectName() == QLatin1String("mp3ComputeAction"))
      a->setEnabled(doc != nullptr);
}

Multiphysics3DDoc *QucsApp::newMultiphysics3DModel()
{
  slotHideEdit(); // disable text edit of component property
  auto *doc = new Multiphysics3DDoc(this, QString());
  const int i = addDocumentTab(doc);
  DocumentTab->setCurrentIndex(i);
  showMultiphysics3DPanel();
  statusBar()->showMessage(tr("A new 3D model: its solids first (right-click the panel's Geometry: Add > Box, Cylinder, Extrude, "
                              "Layer Stack), then its physics (right-click the Component) and Compute."),
                           6000);
  return doc;
}

void QucsApp::showMultiphysics3DPanel()
{
  if (a_multiphysics3DPanel == nullptr) return;
  if (dock != nullptr && !dock->isVisible()) dock->show();
  TabView->setCurrentWidget(a_multiphysics3DPanel);
}
