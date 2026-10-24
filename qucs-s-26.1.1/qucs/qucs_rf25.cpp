/*
 * qucs_rf25.cpp - the RF 2.5D solver in the window: the left dock's RF 2.5D
 *                 tab, the RF 2.5D menu, a new model, a model's file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucs.h"

#include "rf25doc.h"
#include "rf25panel.h"

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

bool QucsApp::isRf25Document(QWidget *w)
{
  return w != nullptr && w->inherits("RF25Doc");
}

bool QucsApp::isRf25File(const QString &name)
{
  return QFileInfo(name).suffix().compare(QLatin1String("qrf25"), Qt::CaseInsensitive) == 0;
}

void QucsApp::initRf25()
{
  // A tab of the left dock, after Multiphysics: the tree of the model in
  // front. Its page is SidePage::Rf25, never a number - a page
  // added before another would send every jump to the wrong panel, and the
  // number a session file kept would mean another page the next time.
  a_rf25Panel = new RF25Panel;
  TabView->addTab(a_rf25Panel, tr("RF 2.5D"));
  a_sidePages.insert(int(SidePage::Rf25), a_rf25Panel);
  TabView->setTabToolTip(TabView->indexOf(a_rf25Panel),
                         tr("a planar RF model's tree: the stack in z, the layout in x and y, ports, mesh, study"));
  connect(a_rf25Panel, &RF25Panel::newModelRequested, this, [this] { newRf25Model(); });
  connect(a_rf25Panel, &RF25Panel::openModelRequested, this, [this](const QString &given) {
    QString path = given;
    if (path.isEmpty())
      path = QFileDialog::getOpenFileName(this, tr("Open RF 2.5D Model"),
                                          lastDirOpenSave.isEmpty() ? QucsSettings.qucsWorkspaceDir.absolutePath() : lastDirOpenSave,
                                          tr("RF 2.5D models") + QStringLiteral(" (*.qrf25)"));
    if (!path.isEmpty()) openFileFromProjectView(QFileInfo(path), QString());
  });

  // The RF 2.5D menu, before View (after Multiphysics' own).
  a_rf25Menu = new QMenu(tr("&RF 2.5D"), this);
  a_rf25Menu->setObjectName(QStringLiteral("rf25Menu"));
  a_rf25Menu->setToolTipsVisible(true);
  auto model = [this] { return qobject_cast<RF25Doc *>(DocumentTab->currentWidget()); };
  QAction *newModel = a_rf25Menu->addAction(tr("New Model"), this, [this] { newRf25Model(); });
  newModel->setObjectName(QStringLiteral("rfNewModelAction"));
  newModel->setStatusTip(tr("A new planar RF model in a tab of its own"));
  a_rf25Menu->addAction(tr("Open Model…"), this, [this] { emit a_rf25Panel->openModelRequested(QString()); });
  a_rf25Menu->addSeparator();
  QAction *layout = a_rf25Menu->addAction(tr("Read the Layout Again"), this, [model] {
    if (RF25Doc *doc = model()) doc->refreshLayout();
  });
  layout->setObjectName(QStringLiteral("rfReadLayoutAction"));
  layout->setStatusTip(tr("Read the GDSII or OASIS file the model's Layout names once more"));
  a_rf25Menu->addSeparator();
  a_rf25Menu->addAction(tr("Show the RF 2.5D Panel"), this, [this] { showRf25Panel(); });
  connect(a_rf25Menu, &QMenu::aboutToShow, this, &QucsApp::updateRf25Menu);
  const QList<QAction *> bar = menuBar()->actions();
  const int view = int(bar.indexOf(viewMenu->menuAction()));
  if (view >= 0) menuBar()->insertMenu(bar.at(view), a_rf25Menu);
  else menuBar()->addMenu(a_rf25Menu);
}

void QucsApp::updateRf25Menu()
{
  auto *doc = qobject_cast<RF25Doc *>(DocumentTab->currentWidget());
  for (QAction *a : a_rf25Menu->actions())
    if (a->objectName() == QLatin1String("rfReadLayoutAction"))
      a->setEnabled(doc != nullptr && !doc->isBusy());
}

RF25Doc *QucsApp::newRf25Model()
{
  slotHideEdit(); // disable text edit of component property
  auto *doc = new RF25Doc(this, QString());
  const int i = addDocumentTab(doc);
  DocumentTab->setCurrentIndex(i);
  showRf25Panel();
  statusBar()->showMessage(tr("A new RF 2.5D model: its stack first (the panel's Stack node), then the layout it is drawn "
                              "on."),
                           6000);
  return doc;
}

void QucsApp::showRf25Panel()
{
  if (a_rf25Panel == nullptr) return;
  if (dock != nullptr && !dock->isVisible()) dock->show();
  TabView->setCurrentWidget(a_rf25Panel);
}
