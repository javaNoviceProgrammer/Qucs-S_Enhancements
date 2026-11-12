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
#include "rf25settings.h"

#include "main.h"
#include "qucscontrol.h"

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QRegularExpression>
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
  QAction *converge = a_rf25Menu->addAction(tr("Check Mesh Convergence…"), this, [this, model] {
    RF25Doc *doc = model();
    if (doc == nullptr || doc->isSolving()) return;
    QStringList given;
    for (const int n : doc->convergenceLevels()) given << QString::number(n);
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Check Mesh Convergence"),
                                               tr("The meshes, coarsest first: cells across the narrowest conductor of each.\n"
                                                  "Each is solved as the study says - a full wave at the top of its sweep - "
                                                  "and what each value does says how far the finest is from the limit.\n"
                                                  "The model's own mesh and solution are left as they are."),
                                               QLineEdit::Normal, given.join(QStringLiteral(", ")), &ok);
    if (!ok) return;
    QList<int> levels;
    for (const QString &part : text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
      const int n = part.toInt();
      if (n >= 1 && n <= 200 && !levels.contains(n)) levels << n;
    }
    std::sort(levels.begin(), levels.end());
    if (levels.size() < 2) {
      QMessageBox::warning(this, tr("Check Mesh Convergence"), tr("Two meshes at least, three to tell whether a value converges: cells across from 1 to 200."));
      return;
    }
    RF25Doc::SolveRequest request;
    request.levels = levels;
    doc->solveWith(request);
  });
  converge->setObjectName(QStringLiteral("rfConvergeAction"));
  converge->setStatusTip(tr("Solve the model on meshes ever finer and say how far its own is from the limit"));
  a_rf25Menu->addSeparator();
  QAction *hfssExport = a_rf25Menu->addAction(tr("Export HFSS™ 3D Layout…"), this, [this] { exportRf25Hfss(); });
  hfssExport->setObjectName(QStringLiteral("rfHfssExportAction"));
  hfssExport->setStatusTip(tr("An Ansys® Electronics Desktop script that builds the model as an HFSS™ 3D Layout design, with a GDSII file and an XML control file for its other way in"));
  QAction *hfssImport = a_rf25Menu->addAction(tr("Import HFSS™ 3D Layout…"), this, [this] { importRf25Hfss(); });
  hfssImport->setObjectName(QStringLiteral("rfHfssImportAction"));
  hfssImport->setStatusTip(tr("A model of an HFSS™ 3D Layout project (.aedt and its .aedb), of a GDSII file with its control file, or of a script exported from here"));
  QAction *compare = a_rf25Menu->addAction(tr("Compare With Touchstone…"), this, [this] { compareRf25WithTouchstone(); });
  compare->setObjectName(QStringLiteral("rfCompareAction"));
  compare->setStatusTip(tr("The full-wave solve's S beside a Touchstone file's, term by term, each into the file's reference"));
  a_rf25Menu->addSeparator();
  a_rf25Menu->addAction(tr("Show the RF 2.5D Panel"), this, [this] { showRf25Panel(); });
  a_rf25Menu->addSeparator();
  // Its settings: the dense LU, how large a full wave is held dense, and a
  // sweep's memory. (No role: on macOS a "Settings…" would be taken into
  // the application's menu as its Preferences.)
  QAction *settings = a_rf25Menu->addAction(tr("Settings…"), this, [this] {
    qucs_s::rf25::RF25SettingsDialog dialog(this);
    dialog.exec();
  });
  settings->setObjectName(QStringLiteral("rf25SettingsAction"));
  settings->setMenuRole(QAction::NoRole);
  settings->setStatusTip(tr("The RF 2.5D settings: the dense LU, how large a full wave is held dense, a sweep's memory, the layout drawn by the GPU or the CPU"));
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
    else if (a->objectName() == QLatin1String("rfHfssExportAction"))
      a->setEnabled(doc != nullptr);
    else if (a->objectName() == QLatin1String("rfConvergeAction"))
      a->setEnabled(doc != nullptr && !doc->isSolving());
    else if (a->objectName() == QLatin1String("rfCompareAction"))
      a->setEnabled(doc != nullptr && doc->fullWave() && doc->fullWave()->ok());
}

void QucsApp::exportRf25Hfss()
{
  auto *doc = qobject_cast<RF25Doc *>(DocumentTab->currentWidget());
  auto *control = findChild<QucsControl *>();
  if (doc == nullptr || control == nullptr) return;
  const QString title = tr("Export HFSS™ 3D Layout");
  const QString name = doc->getDocName();
  const QFileInfo info(name);
  const QString suggested = name.isEmpty() ? QDir(lastDirOpenSave.isEmpty() ? QucsSettings.qucsWorkspaceDir.absolutePath() : lastDirOpenSave).filePath(QStringLiteral("rf25_hfss.py"))
                                           : info.dir().filePath(info.completeBaseName() + QStringLiteral("_hfss.py"));
  const QString file = QFileDialog::getSaveFileName(this, title, suggested, tr("Electronics Desktop scripts") + QStringLiteral(" (*.py)"));
  if (file.isEmpty()) return;
  const QJsonObject result = control->callNow(QStringLiteral("rf25_hfss_export"), {{QStringLiteral("file"), file}}, 300000);
  if (result.value(QStringLiteral("isError")).toBool()) {
    QMessageBox::warning(this, title, QucsControl::textOf(result));
    return;
  }
  const QJsonObject answer = QJsonDocument::fromJson(QucsControl::textOf(result).toUtf8()).object();
  QStringList said{tr("Written beside each other: the script, and for its other way in the GDSII file and the control file. No Electronics Desktop has run them here: the report the script leaves says what came of it.")};
  for (const QJsonValue &v : answer.value(QStringLiteral("notes")).toArray()) said << v.toString();
  QMessageBox::information(this, title, said.join(QStringLiteral("\n\n")));
}

void QucsApp::importRf25Hfss()
{
  auto *control = findChild<QucsControl *>();
  if (control == nullptr) return;
  const QString title = tr("Import HFSS™ 3D Layout");
  const QString file = QFileDialog::getOpenFileName(this, title, lastDirOpenSave.isEmpty() ? QucsSettings.qucsWorkspaceDir.absolutePath() : lastDirOpenSave,
                                                    tr("HFSS™ 3D Layout: projects, control files, scripts from here") + QStringLiteral(" (*.aedt edb.def *.xml *.py);;") + tr("All files") + QStringLiteral(" (*)"));
  if (file.isEmpty()) return;
  const QFileInfo info(file);
  // (An edb.def by itself: the model takes the name of its folder.)
  const QString stem = info.fileName() == QLatin1String("edb.def") ? info.absoluteDir().dirName().section(QLatin1Char('.'), 0, -2) : info.completeBaseName();
  const QString model = (info.fileName() == QLatin1String("edb.def") ? QFileInfo(info.absolutePath()).dir() : info.dir()).filePath(stem + QStringLiteral(".qrf25"));
  // (A project's shapes go in a GDSII file of the model's name.)
  const bool project = info.fileName() == QLatin1String("edb.def") || info.suffix().compare(QLatin1String("aedt"), Qt::CaseInsensitive) == 0;
  const QString shapes = QFileInfo(model).absoluteDir().filePath(stem + QStringLiteral(".gds"));
  bool replace = false;
  if (const QString there = QFileInfo::exists(model) ? model : project && QFileInfo::exists(shapes) ? shapes : QString(); !there.isEmpty()) {
    if (QMessageBox::question(this, title, tr("%1 is there already: write over it?").arg(QDir::toNativeSeparators(there))) != QMessageBox::Yes) return;
    replace = true;
  }
  const QJsonObject result = control->callNow(QStringLiteral("rf25_hfss_import"),
                                              {{QStringLiteral("path"), file}, {QStringLiteral("save_as"), model}, {QStringLiteral("replace"), replace}}, 300000);
  if (result.value(QStringLiteral("isError")).toBool()) {
    QMessageBox::warning(this, title, QucsControl::textOf(result));
    return;
  }
  const QJsonObject answer = QJsonDocument::fromJson(QucsControl::textOf(result).toUtf8()).object();
  QStringList said;
  for (const QJsonValue &v : answer.value(QStringLiteral("notes")).toArray()) said << v.toString();
  for (const QJsonValue &v : answer.value(QStringLiteral("problems")).toArray()) said << v.toString();
  showRf25Panel();
  if (!said.isEmpty()) QMessageBox::information(this, title, said.join(QStringLiteral("\n\n")));
}

void QucsApp::compareRf25WithTouchstone()
{
  auto *doc = qobject_cast<RF25Doc *>(DocumentTab->currentWidget());
  auto *control = findChild<QucsControl *>();
  if (doc == nullptr || control == nullptr) return;
  const QString title = tr("Compare With Touchstone");
  const QString name = doc->getDocName();
  const QString file = QFileDialog::getOpenFileName(this, title, name.isEmpty() ? QucsSettings.qucsWorkspaceDir.absolutePath() : QFileInfo(name).absolutePath(),
                                                    tr("Touchstone files") + QStringLiteral(" (*.s1p *.s2p *.s3p *.s4p *.s*p);;") + tr("All files") + QStringLiteral(" (*)"));
  if (file.isEmpty()) return;
  const QFileInfo info(file);
  const QJsonObject result = control->callNow(QStringLiteral("rf25_compare"),
                                              {{QStringLiteral("file"), file}, {QStringLiteral("table"), info.dir().filePath(info.completeBaseName() + QStringLiteral("_compared.csv"))}}, 300000);
  if (result.value(QStringLiteral("isError")).toBool()) {
    QMessageBox::warning(this, title, QucsControl::textOf(result));
    return;
  }
  const QJsonObject answer = QJsonDocument::fromJson(QucsControl::textOf(result).toUtf8()).object();
  QStringList said{answer.value(QStringLiteral("said")).toString()};
  for (const QJsonValue &v : answer.value(QStringLiteral("terms")).toArray()) said << v.toObject().value(QStringLiteral("said")).toString();
  for (const QJsonValue &v : answer.value(QStringLiteral("notes")).toArray()) said << v.toString();
  said << tr("Both, at every frequency: %1").arg(QDir::toNativeSeparators(answer.value(QStringLiteral("table")).toString()));
  QMessageBox::information(this, title, said.join(QStringLiteral("\n\n")));
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
