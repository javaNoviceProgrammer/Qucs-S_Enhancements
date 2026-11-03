/*
 * qucs_multiphysics3d.cpp - Multiphysics 3D in the window: the left dock's
 *                           Multiphysics 3D tab, the Multiphysics 3D menu, a
 *                           new 3D model, a 3D model's file
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

#include "main.h"

#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabWidget>

extern QString lastDirOpenSave;   // (qucs.cpp: the last folder a file was opened from)

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
