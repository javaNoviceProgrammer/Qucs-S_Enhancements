/*
 * qucs_multiphysics.cpp - the multiphysics solver in the window: the left
 *                         dock's Multiphysics tab, the Multiphysics menu,
 *                         a new model, a model's file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucs.h"

#include "multiphysicsdoc.h"
#include "multiphysicspanel.h"

#include "main.h"

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabWidget>

extern QString lastDirOpenSave;   // (qucs.cpp: the last folder a file was opened from)

bool QucsApp::isMultiphysicsDocument(QWidget *w)
{
  return w != nullptr && w->inherits("MultiphysicsDoc");
}

bool QucsApp::isMultiphysicsFile(const QString &name)
{
  return QFileInfo(name).suffix().compare(QLatin1String("qfem"), Qt::CaseInsensitive) == 0;
}

void QucsApp::initMultiphysics()
{
  // The sixth tab of the left dock: the tree of the model in front.
  a_multiphysicsPanel = new MultiphysicsPanel;
  TabView->addTab(a_multiphysicsPanel, tr("Multiphysics"));
  TabView->setTabToolTip(TabView->indexOf(a_multiphysicsPanel),
                         tr("a 2D multiphysics model's tree: geometry, materials, physics, mesh, studies, results"));
  connect(a_multiphysicsPanel, &MultiphysicsPanel::newModelRequested, this, [this] { newMultiphysicsModel(); });
  connect(a_multiphysicsPanel, &MultiphysicsPanel::openModelRequested, this, [this](const QString &given) {
    QString path = given;
    if (path.isEmpty())
      path = QFileDialog::getOpenFileName(this, tr("Open Multiphysics Model"),
                                          lastDirOpenSave.isEmpty() ? QucsSettings.qucsWorkspaceDir.absolutePath() : lastDirOpenSave,
                                          tr("Multiphysics models") + QStringLiteral(" (*.qfem)"));
    if (!path.isEmpty()) openFileFromProjectView(QFileInfo(path), QString());
  });

  // The Multiphysics menu, before View.
  a_multiphysicsMenu = new QMenu(tr("&Multiphysics"), this);
  a_multiphysicsMenu->setObjectName(QStringLiteral("multiphysicsMenu"));
  a_multiphysicsMenu->setToolTipsVisible(true);
  auto model = [this] { return qobject_cast<MultiphysicsDoc *>(DocumentTab->currentWidget()); };
  QAction *newModel = a_multiphysicsMenu->addAction(tr("New Model"), this, [this] { newMultiphysicsModel(); });
  newModel->setObjectName(QStringLiteral("mpNewModelAction"));
  newModel->setStatusTip(tr("A new 2D multiphysics model in a tab of its own"));
  a_multiphysicsMenu->addAction(tr("Open Model…"), this, [this] { emit a_multiphysicsPanel->openModelRequested(QString()); });
  a_multiphysicsMenu->addSeparator();
  QAction *geometry = a_multiphysicsMenu->addAction(tr("Build Geometry"), this, [model] {
    if (MultiphysicsDoc *doc = model()) {
      QString why;
      if (doc->buildGeometry(&why)) doc->showGeometry();
    }
  });
  geometry->setObjectName(QStringLiteral("mpBuildGeometryAction"));
  QAction *mesh = a_multiphysicsMenu->addAction(tr("Build Mesh"), this, [model] {
    if (MultiphysicsDoc *doc = model()) doc->buildMesh();
  });
  mesh->setObjectName(QStringLiteral("mpBuildMeshAction"));
  QAction *compute = a_multiphysicsMenu->addAction(tr("Compute"), this, [model] {
    if (MultiphysicsDoc *doc = model()) doc->compute();
  });
  compute->setObjectName(QStringLiteral("mpComputeAction"));
  compute->setStatusTip(tr("Solve the first study (Simulate, F2, does too)"));
  QAction *clear = a_multiphysicsMenu->addAction(tr("Clear Solutions"), this, [model] {
    if (MultiphysicsDoc *doc = model()) doc->clearSolutions();
  });
  clear->setObjectName(QStringLiteral("mpClearAction"));
  a_multiphysicsMenu->addSeparator();
  a_multiphysicsMenu->addAction(tr("Show the Multiphysics Panel"), this, [this] { showMultiphysicsPanel(); });
  connect(a_multiphysicsMenu, &QMenu::aboutToShow, this, &QucsApp::updateMultiphysicsMenu);
  const QList<QAction *> bar = menuBar()->actions();
  const int view = int(bar.indexOf(viewMenu->menuAction()));
  if (view >= 0) menuBar()->insertMenu(bar.at(view), a_multiphysicsMenu);
  else menuBar()->addMenu(a_multiphysicsMenu);
}

void QucsApp::updateMultiphysicsMenu()
{
  auto *doc = qobject_cast<MultiphysicsDoc *>(DocumentTab->currentWidget());
  for (QAction *a : a_multiphysicsMenu->actions()) {
    const QString name = a->objectName();
    if (name == QLatin1String("mpBuildGeometryAction") || name == QLatin1String("mpBuildMeshAction")
        || name == QLatin1String("mpComputeAction"))
      a->setEnabled(doc != nullptr && !doc->isBusy());
    else if (name == QLatin1String("mpClearAction"))
      a->setEnabled(doc != nullptr && doc->solution() != nullptr);
  }
}

MultiphysicsDoc *QucsApp::newMultiphysicsModel()
{
  slotHideEdit(); // disable text edit of component property
  auto *doc = new MultiphysicsDoc(this, QString());
  const int i = addDocumentTab(doc);
  DocumentTab->setCurrentIndex(i);
  showMultiphysicsPanel();
  statusBar()->showMessage(tr("A new multiphysics model: its geometry first (the panel's Geometry node, its menu)."), 6000);
  return doc;
}

void QucsApp::showMultiphysicsPanel()
{
  if (a_multiphysicsPanel == nullptr) return;
  if (dock != nullptr && !dock->isVisible()) dock->show();
  TabView->setCurrentWidget(a_multiphysicsPanel);
}
