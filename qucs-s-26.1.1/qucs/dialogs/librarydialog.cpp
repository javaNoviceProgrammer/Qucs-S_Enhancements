/*
 * librarydialog.cpp - implementation of dialog to create library
 *
 * Copyright (C) 2006, Michael Margraf, michael.margraf@alumni.tu-berlin.de
 * Copyright (C) 2014, Yodalee, lc85301@gmail.com
 *
 * This file is part of Qucs
 *
 * Qucs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Qucs.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextStream>

#include <QDataStream>
#include <QCheckBox>
#include <QTreeWidgetItem>
#include <QValidator>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QGroupBox>
#include <QStringList>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QDirIterator>

#include "librarydialog.h"
#include "main.h"
#include "qucs.h"
#include "misc.h"
#include "osdiselection.h"
#include "projectlibraries.h"
#include "painting.h"
#include "components/libcomp.h"
#include "extsimkernels/abstractspicekernel.h"
#include "extsimkernels/spicecompat.h"

#include <algorithm>

extern SubMap FileList;

LibraryDialog::LibraryDialog(QWidget *parent)
      : QDialog(parent)
{
  setWindowTitle(tr("Create Library"));

  Expr.setPattern("[\\w_]+");
  Validator = new QRegularExpressionValidator(Expr, this);

  curDescr = 0; // description counter, prev, next

 // ...........................................................
  all = new QVBoxLayout(this);
  all->setContentsMargins(5,5,5,5);
  all->setSpacing(6);

  stackedWidgets = new QStackedWidget(this);
  all->addWidget(stackedWidgets);


  // stacked 0 - select subcirbuit, name, and descriptions
  // ...........................................................
  QWidget *selectSubckt = new QWidget();
  stackedWidgets->addWidget(selectSubckt);

  QVBoxLayout *selectSubcktLayout = new QVBoxLayout();
  selectSubckt->setLayout(selectSubcktLayout);


  QHBoxLayout *h1 = new QHBoxLayout();
  selectSubcktLayout->addLayout(h1);
  theLabel = new QLabel(tr("Library Name:"));
  h1->addWidget(theLabel);
  NameEdit = new QLineEdit();
  h1->addWidget(NameEdit);
  NameEdit->setValidator(Validator);

  // Where it goes: the user libraries (user_lib), the project, or a folder
  // of the library search paths - each a section of the Libraries panel.
  QHBoxLayout *hDestination = new QHBoxLayout();
  selectSubcktLayout->addLayout(hDestination);
  QLabel *destinationLabel = new QLabel(tr("Save in:"));
  hDestination->addWidget(destinationLabel);
  Destination = new QComboBox();
  Destination->setObjectName(QStringLiteral("destination"));
  destinationLabel->setBuddy(Destination);
  hDestination->addWidget(Destination, 1);
  QStringList offered;
  const auto offer = [this, &offered](const QString &text, const QString &folder) {
    const QString key = QFileInfo(folder).exists() ? QFileInfo(folder).canonicalFilePath() : QDir::cleanPath(folder);
    if (offered.contains(key)) return;
    offered << key;
    Destination->addItem(text, folder);
    Destination->setItemData(Destination->count() - 1, QDir::toNativeSeparators(folder), Qt::ToolTipRole);
  };
  const QString userLib = QucsSettings.qucsWorkspaceDir.filePath(QStringLiteral("user_lib"));
  offer(tr("User libraries (user_lib)"), userLib);
  if (QucsMain != nullptr && !QucsMain->ProjName.isEmpty())
    offer(tr("The project %1").arg(QucsMain->ProjName), QucsSettings.QucsWorkDir.absolutePath());
  for (const QString &folder : std::as_const(QucsSettings.LibraryPaths))
    if (QFileInfo(folder).isDir()) offer(QDir::toNativeSeparators(QDir::cleanPath(folder)), QDir::cleanPath(folder));

  // ...........................................................
  Group = new QGroupBox(tr("Choose subcircuits:"));
  selectSubcktLayout->addWidget(Group);

  subcirFileList = new QListWidget();
  subcirFileList->setAccessibleName(tr("Subcircuits"));   // (its name to Claude: each ticked or not)
  subcirListLayout = new QVBoxLayout();
  Group->setLayout(subcirListLayout);

  // ...........................................................
  QHBoxLayout *hCheck = new QHBoxLayout();
  selectSubcktLayout->addLayout(hCheck);
  checkDescr = new QCheckBox(tr("Add subcircuit description"));
  checkDescr->setChecked(true);
  hCheck->addWidget(checkDescr);
  hCheck->addStretch();
  connect(checkDescr, SIGNAL(stateChanged(int)), this, SLOT(slotCheckDescrChanged(int)));

  checkAnalogLib = new QCheckBox(tr("Analog models only"));
  checkAnalogLib->setChecked(true);
  selectSubcktLayout->addWidget(checkAnalogLib);

  // ...........................................................
  QGridLayout *gridButts = new QGridLayout();
  selectSubcktLayout->addLayout(gridButts);
  ButtSelectAll = new QPushButton(tr("Select All"));
  gridButts->addWidget(ButtSelectAll, 0, 0);
  connect(ButtSelectAll, SIGNAL(clicked()), SLOT(slotSelectAll()));
  ButtSelectNone = new QPushButton(tr("Deselect All"));
  gridButts->addWidget(ButtSelectNone, 0, 1);
  connect(ButtSelectNone, SIGNAL(clicked()), SLOT(slotSelectNone()));
  // ...........................................................
  ButtCancel = new QPushButton(tr("Cancel"));
  gridButts->addWidget(ButtCancel, 1, 0);
  connect(ButtCancel, SIGNAL(clicked()), SLOT(reject()));
  ButtCreateNext = new QPushButton(tr("Next >>"));
  gridButts->addWidget(ButtCreateNext, 1, 1);
  connect(ButtCreateNext, SIGNAL(clicked()), SLOT(slotCreateNext()));
  ButtCreateNext->setDefault(true);


  // stacked 1 - enter description, loop over checked subckts
  // ...........................................................
  QWidget *subcktDescr = new QWidget();
  stackedWidgets->addWidget(subcktDescr);

  QVBoxLayout *subcktDescrLayout = new QVBoxLayout();
  subcktDescr->setLayout(subcktDescrLayout);

  QHBoxLayout *hbox = new QHBoxLayout();
  subcktDescrLayout->addLayout(hbox);
  QLabel *libName = new QLabel(tr("Enter description for:"));
  hbox->addWidget(libName);
  checkedCktName = new QLabel();
  checkedCktName->setText("dummy");
  hbox->addWidget(checkedCktName);

  QGroupBox *descrBox = new QGroupBox(tr("Description:"));
  subcktDescrLayout->addWidget(descrBox);
  textDescr = new QTextEdit();
  textDescr->toPlainText();
  textDescr->setWordWrapMode(QTextOption::NoWrap);
  connect(textDescr, SIGNAL(textChanged()), SLOT(slotUpdateDescription()));
  QVBoxLayout *vGroup = new QVBoxLayout;
  vGroup->addWidget(textDescr);
  descrBox->setLayout(vGroup);

  // ...........................................................
  gridButts = new QGridLayout();
  subcktDescrLayout->addLayout(gridButts);
  prevButt = new QPushButton(tr("Previous"));
  gridButts->addWidget(prevButt, 0, 0);
  prevButt->setDisabled(true);
  connect(prevButt, SIGNAL(clicked()), SLOT(slotPrevDescr()));
  nextButt = new QPushButton(tr("Next >>"));
  nextButt->setDefault(true);
  gridButts->addWidget(nextButt, 0, 1);
  connect(nextButt, SIGNAL(clicked()), SLOT(slotNextDescr()));
  // ...........................................................
  ButtCancel = new QPushButton(tr("Cancel"));
  gridButts->addWidget(ButtCancel, 1, 0);
  connect(ButtCancel, SIGNAL(clicked()), SLOT(reject()));
  createButt = new QPushButton(tr("Create"));
  connect(createButt, SIGNAL(clicked()), SLOT(slotSave()));
  gridButts->addWidget(createButt, 1, 1);
  createButt->setDisabled(true);


  // stacked 2 - show error / success message
  // ...........................................................
  QWidget *msg = new QWidget();
  stackedWidgets->addWidget(msg);

  QVBoxLayout *msgLayout = new QVBoxLayout();
  msg->setLayout(msgLayout);

  QHBoxLayout *hbox1 = new QHBoxLayout();
  msgLayout->addLayout(hbox1);
  QLabel *finalLabel = new QLabel(tr("Library Name:"));
  hbox1->addWidget(finalLabel);
  libSaveName = new QLabel();
  hbox1->addWidget(libSaveName);

  QGroupBox *msgBox = new QGroupBox(tr("Message:"));
  msgLayout->addWidget(msgBox);
  ErrText = new QPlainTextEdit();
  ErrText->setWordWrapMode(QTextOption::NoWrap);
  ErrText->setReadOnly(true);
  QVBoxLayout *vbox1 = new QVBoxLayout();
  vbox1->addWidget(ErrText);
  msgBox->setLayout(vbox1);

  QHBoxLayout *hbox2 = new QHBoxLayout();
  hbox2->addStretch();
  QPushButton  *close = new QPushButton(tr("Close"));
  hbox2->addWidget(close);
  connect(close, SIGNAL(clicked()), SLOT(reject()));
  msgLayout->addLayout(hbox2);
}


LibraryDialog::~LibraryDialog()
{
  delete all;
  delete Validator;
}

void
LibraryDialog::fillSchematicList(QStringList SchematicList)
{
  // ...........................................................
  // insert all subcircuits of into checklist
  if (SchematicList.size() == 0) {
    ButtCreateNext->setEnabled(false);
    QLabel *noProj = new QLabel(tr("No projects!"));
    subcirListLayout->addWidget(noProj);
  } else {
    subcirListLayout->addWidget(subcirFileList);
    for(const auto &filename: SchematicList) {
      QListWidgetItem *itm = new QListWidgetItem;
      itm->setFlags(itm->flags()|Qt::ItemIsUserCheckable);
      itm->setText(filename);
      itm->setCheckState(Qt::Checked);
      subcirFileList->addItem(itm);
    }
  }
}

// ---------------------------------------------------------------
void LibraryDialog::slotCreateNext()
{
  if(NameEdit->text().isEmpty()) {
    QMessageBox::critical(this, tr("Error"), tr("Please insert a library name!"));
    return;
  }

  // (Chosen afresh each time: a "No" to Rewrite? and another name, or a
  // folder that could not be made, came back here and added them again -
  // each part twice in the library.)
  SelectedNames.clear();
  Descriptions.clear();
  curDescr = 0;
  int count=0;
  for(int i = 0; i < subcirFileList->count(); i++) {
      auto itm = subcirFileList->item(i);
      if (itm == nullptr) continue;
      if (itm->checkState() == Qt::Checked) {
          SelectedNames.append(itm->text());
          Descriptions.append("");
          count++;
      }
  }

  if(count < 1) {
    QMessageBox::critical(this, tr("Error"), tr("Please choose at least one subcircuit!"));
    return;
  }
  if (const QString clash = nameClash(NameEdit->text(), SelectedNames); !clash.isEmpty()) {
    QMessageBox::critical(this, tr("Error"), clash + QLatin1Char('.'));
    return;
  }

  // A library is made of the subcircuits' files: one open with changes is
  // saved first, or the library is not made (it took the file as last
  // saved, the changes left out without a word).
  if (QucsMain != nullptr) {
    QList<QucsDoc*> unsaved;
    QStringList names;
    for (QucsDoc *doc : QucsMain->allDocuments())
      for (const QString &sub : std::as_const(SelectedNames))
        if (doc->getDocChanged() && !doc->getDocName().isEmpty()
            && misc::isSameFile(doc->getDocName(), QucsSettings.QucsWorkDir.filePath(sub)) && !unsaved.contains(doc)) {
          unsaved << doc;
          names << sub;
        }
    if (!unsaved.isEmpty()) {
      QMessageBox box(QMessageBox::Question, tr("Create Library"),
                      tr("%1 has changes that are not saved, and the library is made of the saved file.", "", int(names.size()))
                          .arg(names.join(QStringLiteral(", "))),
                      QMessageBox::NoButton, this);
      box.setObjectName(QStringLiteral("saveSubcircuits"));
      QPushButton *save = box.addButton(tr("Save and Go On"), QMessageBox::AcceptRole);
      box.addButton(QMessageBox::Cancel);
      box.setDefaultButton(save);
      box.exec();
      if (box.clickedButton() != save) return;
      for (QucsDoc *doc : std::as_const(unsaved))
        if (!QucsMain->saveFile(doc)) return;
    }
  }

  // The folder chosen (user_lib made when it is not there yet).
  const QString destination = Destination->currentData().toString();
  if (!QDir().mkpath(destination)) {
    QMessageBox::warning(this, tr("Warning"),
                 tr("Cannot create the folder %1.").arg(QDir::toNativeSeparators(destination)));
    return;
  }
  LibDir = QDir(destination);

  /*LibFile.setFileName(QucsSettings.LibDir + NameEdit->text() + ".lib");
  if(LibFile.exists()) {
    QMessageBox::critical(this, tr("Error"), tr("A system library with this name already exists!"));
    return;
  }*/

  LibFile.setFileName(LibDir.absoluteFilePath(NameEdit->text()) + ".lib");
  if(LibFile.exists()) {
    auto ans = QMessageBox::question(this, tr("Error"),
                          tr("A library with this name already exists! Rewrite?") + QLatin1Char('\n')
                              + tr("(It goes to the trash once the new one is made, and stays as it is if that cannot be made.)"),
                          QMessageBox::Yes, QMessageBox::No);
    if (ans == QMessageBox::No) return;
    // Each part's description the library had, to keep or change.
    const QHash<QString, QString> before = descriptionsOf(LibFile.fileName());
    for (int i = 0; i < SelectedNames.size() && i < Descriptions.size(); ++i)
      if (Descriptions[i].isEmpty()) Descriptions[i] = before.value(partName(SelectedNames[i]));
  }

  if (checkDescr->checkState() == Qt::Checked){
    // user enter descriptions
    stackedWidgets->setCurrentIndex(1);  // subcircuit description view

    checkedCktName->setText(SelectedNames[0]);
    textDescr->setText(Descriptions[0]);

    if (SelectedNames.count() == 1){
        prevButt->setDisabled(true);
        nextButt->setDisabled(true);
        createButt->setEnabled(true);
      }
  }
  else {
      // save without description
      emit slotSave();
  }
}

// ---------------------------------------------------------------
void LibraryDialog::intoStream(QTextStream &Stream, QString &tmp,
             const char *sec)
{
  int i = tmp.indexOf("TOP LEVEL MARK");
  if(i >= 0) {
    i = tmp.indexOf('\n',i) + 1;
    tmp = tmp.mid(i);
  }
  Stream << "  <" << sec << ">";
  Stream << tmp;
  Stream << "  </" << sec << ">\n";
}

// ---------------------------------------------------------------
// A model file of a subcircuit (its .lst, Verilog, VHDL) into the library's
// folder as \a ofn's name; a .lst read is removed.
int LibraryDialog::intoFile(QString &ifn, QString &ofn, QStringList &IFiles)
{
  QFile ifile(ifn);
  if(!ifile.open(QIODevice::ReadOnly)) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot open file \"%1\".\n").
        arg(ifn));
    return 1;
  }
  QByteArray FileContent = ifile.readAll();
  ifile.close();
  if(ifile.fileName().right(4) == ".lst")
    QFile::remove(ifile.fileName());
  ofn = QFileInfo(ofn).fileName();
  // (Two subcircuits of one file name in two folders: one would be the
  // other's in the library.)
  const QString source = QFileInfo(ifn).absoluteFilePath();
  if (a_copied.contains(ofn) && a_copied.value(ofn) != source) {
    ErrText->insertPlainText(tr("ERROR: %1 and %2 would both be \"%3\" in the library.\n")
                               .arg(QDir::toNativeSeparators(a_copied.value(ofn)), QDir::toNativeSeparators(source), ofn));
    return 1;
  }
  a_copied.insert(ofn, source);
  IFiles.append(ofn);
  QFile ofile(QDir(modelsFolder()).absoluteFilePath(ofn));
  if (!QDir().mkpath(modelsFolder()) || !ofile.open(QIODevice::WriteOnly)) {
    ErrText->insertPlainText(
      QObject::tr("ERROR: Cannot create file \"%1\".\n").arg(ofn));
    return 1;
  }
  QDataStream ds(&ofile);
  ds.writeRawData(FileContent.data(), FileContent.size());
  ofile.close();
  return 0;
}

// ---------------------------------------------------------------
QString LibraryDialog::modelsFolder() const
{
  return QDir(a_staging).absoluteFilePath(NameEdit->text());
}

// ---------------------------------------------------------------
bool LibraryDialog::copyIntoLibrary(const QString &from, const QString &name)
{
  const QString source = QFileInfo(from).absoluteFilePath();
  if (a_copied.contains(name)) {
    if (a_copied.value(name) == source) return true;   // another subcircuit uses it too
    ErrText->insertPlainText(tr("ERROR: %1 and %2 would both be \"%3\" in the library.\n")
                               .arg(QDir::toNativeSeparators(a_copied.value(name)),
                                    QDir::toNativeSeparators(source), name));
    return false;
  }
  const QString target = QDir(modelsFolder()).absoluteFilePath(name);
  if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot create user library subdirectory !\n"));
    return false;
  }
  // Copied beside and renamed over: a copy that fails leaves the old file.
  QString why;
  if (!misc::copyFileOver(source, target, &why)) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot create file \"%1\".\n").arg(name) + why + QLatin1Char('\n'));
    return false;
  }
  a_copied.insert(name, source);
  return true;
}

// ---------------------------------------------------------------
// Each component's description in the library \a file, by its name.
QHash<QString, QString> LibraryDialog::descriptionsOf(const QString &file)
{
  QHash<QString, QString> found;
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) return found;
  const QString text = QString::fromUtf8(f.readAll());
  static const QRegularExpression component(QStringLiteral("\\n<Component ([^>\\n]+)>\\s*\\n\\s*<Description>\\n(.*?)\\n\\s*</Description>"),
                                            QRegularExpression::DotMatchesEverythingOption);
  for (auto it = component.globalMatch(text); it.hasNext();) {
    const QRegularExpressionMatch m = it.next();
    found.insert(m.captured(1).trimmed(), m.captured(2).trimmed());
  }
  return found;
}

// ---------------------------------------------------------------
// The modules (in their case, sorted) of the .model cards of \a spice (and
// the files it includes, from \a baseDir) that a Verilog-A source or a
// compiled model of the project, or of the subcircuit's library parts,
// defines: what the part needs of Verilog-A.
QStringList LibraryDialog::verilogAModulesOf(Schematic *doc, const QString &spice, const QString &baseDir)
{
  const QSet<QString> types = qucs_s::osdi::usedModelTypes(spice, baseDir);
  if (types.isEmpty()) return {};
  QStringList sources, libraries;
  const QDir project(QucsSettings.QucsWorkDir);
  for (const QString &file : misc::projectFiles(project, {"*.va"})) sources << project.absoluteFilePath(file);
  for (const QString &file : misc::projectFiles(project, {"*.osdi"})) libraries << project.absoluteFilePath(file);
  for (const QString &file : AbstractSpiceKernel::collectVerilogAFiles(doc))
    (file.endsWith(".osdi", Qt::CaseInsensitive) ? libraries : sources) << file;
  QStringList modules;
  for (const QString &type : types) {
    const bool verilogA = std::any_of(sources.cbegin(), sources.cend(), [&](const QString &va) { return qucs_s::osdi::sourceDefines(va, type); })
        || std::any_of(libraries.cbegin(), libraries.cend(), [&](const QString &osdi) { return qucs_s::osdi::defines(osdi, type); });
    if (verilogA) modules << type;
  }
  modules.sort();
  return modules;
}

// ---------------------------------------------------------------
// As copyIntoLibrary(), with \a bytes for \a from's: its include lines
// rewritten for where the library has the files.
bool LibraryDialog::writeIntoLibrary(const QString &from, const QString &name, const QByteArray &bytes)
{
  const QString source = QFileInfo(from).absoluteFilePath();
  if (a_copied.contains(name)) {
    if (a_copied.value(name) == source) return true;   // another subcircuit uses it too
    ErrText->insertPlainText(tr("ERROR: %1 and %2 would both be \"%3\" in the library.\n")
                               .arg(QDir::toNativeSeparators(a_copied.value(name)), QDir::toNativeSeparators(source), name));
    return false;
  }
  const QString target = QDir(modelsFolder()).absoluteFilePath(name);
  QSaveFile out(target);
  if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()
      || !out.commit()) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot create file \"%1\".\n").arg(name));
    return false;
  }
  a_copied.insert(name, source);
  return true;
}

// ---------------------------------------------------------------
QString LibraryDialog::copySpiceFile(const QString &file)
{
  const QString source = QFileInfo(file).absoluteFilePath();
  if (a_spiceRoots.contains(source)) return a_spiceRoots.value(source);
  if (!QFileInfo(source).isFile()) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot open file \"%1\".\n").arg(QDir::toNativeSeparators(source)));
    return {};
  }
  // The file and those it includes, each where it is from the file's
  // folder: they keep their places, as the file names them so.
  const QDir root = QFileInfo(source).absoluteDir();
  QList<std::pair<QString, QString>> files{{source, QFileInfo(source).fileName()}};   // source, its path from root
  QSet<QString> seen{QFileInfo(source).canonicalFilePath()};
  for (int i = 0; i < files.size() && files.size() < 1000; ++i) {
    QFile f(files.at(i).first);
    if (!f.open(QIODevice::ReadOnly)) continue;
    const QString text = QString::fromUtf8(f.readAll());
    for (const QString &included : qucs_s::osdi::includedFiles(text, QFileInfo(files.at(i).first).absolutePath())) {
      const QFileInfo info(included);
      if (!info.isFile() || seen.contains(info.canonicalFilePath())) continue;   // (a .lib line naming a section)
      seen.insert(info.canonicalFilePath());
      const QString relative = root.relativeFilePath(info.absoluteFilePath());
      if (relative.startsWith("..") || QFileInfo(relative).isAbsolute()) {
        ErrText->insertPlainText(tr("Warning: %1 includes %2 from outside its folder; it is not embedded, and the library "
                                    "needs it there.\n").arg(QFileInfo(source).fileName(), QDir::toNativeSeparators(included)));
        continue;
      }
      files.append({info.absoluteFilePath(), relative});
    }
  }
  // Its own name in the library's folder, else a folder named as the one it
  // is in (two vendors' models.lib), numbered when that is taken too.
  const auto fits = [this, &files](const QString &folder) {
    for (const auto &[from, relative] : files) {
      const QString name = folder.isEmpty() ? relative : folder + "/" + relative;
      if (a_copied.contains(name) && a_copied.value(name) != from) return false;
    }
    return true;
  };
  const QString dirName = root.dirName().isEmpty() ? QStringLiteral("spice") : root.dirName();
  QString folder;
  for (int n = 1; !fits(folder); ++n) folder = n == 1 ? dirName : QStringLiteral("%1_%2").arg(dirName).arg(n);
  for (const auto &[from, relative] : files)
    if (!copyIntoLibrary(from, folder.isEmpty() ? relative : folder + "/" + relative)) return {};
  const QString attached = folder.isEmpty() ? files.first().second : folder + "/" + files.first().second;
  a_spiceRoots.insert(source, attached);
  return attached;
}

// ---------------------------------------------------------------
QString LibraryDialog::folderTaken() const
{
  const QString name = NameEdit->text();
  // The project's own folders: its temporary files', and the one Qucs-S
  // keeps the libraries' Verilog-A in.
  if (QucsMain != nullptr && !QucsMain->ProjName.isEmpty() && misc::isSameFile(LibDir.absolutePath(), QucsSettings.QucsWorkDir.absolutePath())
      && (name.compare(QLatin1String("Scratch"), Qt::CaseInsensitive) == 0 || name.compare(QLatin1String("Libraries"), Qt::CaseInsensitive) == 0))
    return tr("%1 is a folder the project keeps for itself: a library's files would go into it - choose another name").arg(name);
  return {};
}

// ---------------------------------------------------------------
QString LibraryDialog::partName(const QString &subcircuit)
{
  QString name = QFileInfo(subcircuit).fileName();
  if (name.endsWith(QLatin1String(".sch"), Qt::CaseInsensitive)) name.chop(4);
  return name;
}

// ---------------------------------------------------------------
QString LibraryDialog::nameClash(const QString &name, const QStringList &subcircuits)
{
  QHash<QString, QString> parts, spice;   // in lower case -> the subcircuit
  for (const QString &sub : subcircuits) {
    const QString part = partName(sub);
    if (const QString other = parts.value(part.toLower()); !other.isEmpty())
      return tr("%1 and %2 would both be the part %3: rename one, or put them in two libraries").arg(other, sub, part);
    parts.insert(part.toLower(), sub);
    const QString subcircuit = LibComp::subcircuitName(name, part);
    if (const QString other = spice.value(subcircuit.toLower()); !other.isEmpty())
      return tr("%1 and %2 would both be the SPICE subcircuit %3: rename one, or put them in two libraries").arg(other, sub, subcircuit);
    spice.insert(subcircuit.toLower(), sub);
  }
  return {};
}

// ---------------------------------------------------------------
int LibraryDialog::embedVerilogA(Schematic *doc, const QString &spice, const QString &baseDir,
                                 QStringList &attached)
{
  const QSet<QString> types = qucs_s::osdi::usedModelTypes(spice, baseDir);
  if (types.isEmpty()) return 0;

  // What there is: the project's, and what the libraries of the
  // subcircuit's components bring.
  QStringList sources, libraries;
  const QDir project(QucsSettings.QucsWorkDir);
  for (const QString &file : misc::projectFiles(project, {"*.va"}))
    sources << project.absoluteFilePath(file);
  for (const QString &file : misc::projectFiles(project, {"*.osdi"}))
    libraries << project.absoluteFilePath(file);
  // A library's source the project has a link to (or a copy of) once.
  QSet<QString> linked;
  for (const QString &source : std::as_const(sources)) {
    linked.insert(QFileInfo(source).canonicalFilePath());
    if (const auto entry = qucs_s::projectlibraries::entryOf(source); !entry.isEmpty())
      linked.insert(QFileInfo(entry.original).canonicalFilePath());
  }
  linked.remove(QString());
  for (const QString &file : AbstractSpiceKernel::collectVerilogAFiles(doc)) {
    if (!QFileInfo(file).isFile()) continue;
    if (file.endsWith(".va", Qt::CaseInsensitive) && !sources.contains(file)
        && !linked.contains(QFileInfo(file).canonicalFilePath())) sources << file;
    else if (file.endsWith(".osdi", Qt::CaseInsensitive) && !libraries.contains(file)) libraries << file;
  }

  // The sources of the modules the .model cards name. A compiled model
  // (.osdi) runs on one platform only: the library brings the source, and
  // OpenVAF compiles it where the library is used.
  //
  // One source a module: that of the library a part in the subcircuit is
  // of, for a module it uses (what the parts bring: collectVerilogAFiles());
  // the project's for the subcircuit's own cards. Every project file that
  // defined the module was taken - an unrelated one beside the part's own
  // - and a circuit using the library ran whichever was built last. Two
  // that differ still: refused, the module named. The library made again
  // here is not one: its folder is the old library's.
  QSet<QString> brought;
  for (const QString &file : AbstractSpiceKernel::collectVerilogAFiles(doc))
    if (file.endsWith(".va", Qt::CaseInsensitive)) brought.insert(QFileInfo(file).canonicalFilePath());
  brought.remove(QString());
  const QString replaced = QFileInfo(LibDir.absoluteFilePath(NameEdit->text().trimmed())).canonicalFilePath();
  const auto sameBytes = [](const QString &a, const QString &b) {
    QFile fa(a), fb(b);
    return fa.open(QIODevice::ReadOnly) && fb.open(QIODevice::ReadOnly) && fa.size() == fb.size() && fa.readAll() == fb.readAll();
  };
  QStringList definingSources;
  QSet<QString> fromSource;
  int errors = 0;
  QStringList sortedTypes(types.cbegin(), types.cend());
  sortedTypes.sort();
  for (const QString &type : std::as_const(sortedTypes)) {
    QStringList defining, ofParts;
    for (const QString &va : std::as_const(sources)) {
      const QString real = QFileInfo(va).canonicalFilePath();
      if (!replaced.isEmpty() && real.startsWith(replaced + QLatin1Char('/'))) continue;
      if (!qucs_s::osdi::sourceDefines(va, type)) continue;
      defining << va;
      if (brought.contains(real)) ofParts << va;
    }
    if (defining.isEmpty()) continue;
    QStringList distinct;   // (copies of one file are one)
    for (const QString &va : ofParts.isEmpty() ? std::as_const(defining) : std::as_const(ofParts))
      if (std::none_of(distinct.cbegin(), distinct.cend(), [&](const QString &d) { return sameBytes(d, va); })) distinct << va;
    if (distinct.size() > 1) {
      QStringList shown;
      for (const QString &va : std::as_const(distinct)) shown << QDir::toNativeSeparators(project.relativeFilePath(va));
      ErrText->insertPlainText(tr("Error: the Verilog-A module %1 is defined differently by %2: a circuit using the library "
                                  "would load one of them. Rename the module in one, or take the one not meant out of the "
                                  "project.\n").arg(type, shown.join(", ")));
      ++errors;
      continue;
    }
    if (!definingSources.contains(distinct.first())) definingSources << distinct.first();
    fromSource.insert(type);
  }
  QSet<QString> compiledOnly;
  for (const QString &osdi : std::as_const(libraries))
    for (const QString &type : types)
      if (!fromSource.contains(type) && qucs_s::osdi::defines(osdi, type)) compiledOnly.insert(type);
  if (!compiledOnly.isEmpty()) {
    QStringList modules(compiledOnly.cbegin(), compiledOnly.cend());
    modules.sort();
    ErrText->insertPlainText(tr("Warning: no Verilog-A source of %1, only a compiled model (.osdi): "
                                "it is not embedded, as a compiled model runs on one platform only.\n")
                               .arg(modules.join(", ")));
  }
  if (definingSources.isEmpty()) return errors;   // no Verilog-A in it

  const QDir folder(modelsFolder());
  QStringList embedded;
  for (const QString &va : std::as_const(definingSources)) {
    const QString name = QFileInfo(va).fileName();
    const bool copied = !a_copied.contains(name)
        && QFileInfo(folder.absoluteFilePath(name)).canonicalFilePath() != QFileInfo(va).canonicalFilePath();
    // Where each file goes in the library: the source by its name, what it
    // includes from its folder (or below) at its path from there (beside
    // the file a link leads to, for a library's linked source), and one
    // from outside it - `include "../common/up.vams" - below NAME.includes/
    // beside it, at its path from the folder they all share: left out, the
    // library compiled nowhere ("failed to read '../common/up.vams'"). An
    // include line naming a file now elsewhere is rewritten, in the
    // library's copy.
    const QFileInfo vaInfo(va);
    const QDir sourceFolder = vaInfo.isSymLink() ? QFileInfo(vaInfo.symLinkTarget()).absoluteDir() : vaInfo.absoluteDir();
    QList<std::pair<QString, QString>> places{{va, name}};   // source, its path in the library
    QHash<QString, QString> placeOf{{QDir::cleanPath(vaInfo.absoluteFilePath()), name}};
    QStringList outside;
    for (const QString &included : qucs_s::osdi::sourceIncludes(va)) {
      const QString relative = sourceFolder.relativeFilePath(included);
      if (relative.startsWith("..")) {
        outside << included;
        continue;
      }
      places.append({included, relative});
      placeOf.insert(QDir::cleanPath(included), relative);
    }
    if (!outside.isEmpty()) {
      QStringList common = QDir::cleanPath(sourceFolder.absolutePath()).split(QLatin1Char('/'));
      for (const QString &file : std::as_const(outside)) {
        const QStringList parts = QDir::cleanPath(QFileInfo(file).absolutePath()).split(QLatin1Char('/'));
        int same = 0;
        while (same < common.size() && same < parts.size() && common.at(same) == parts.at(same)) ++same;
        common = common.mid(0, same);
      }
      const QDir shared(common.join(QLatin1Char('/')).isEmpty() ? QStringLiteral("/") : common.join(QLatin1Char('/')));
      for (const QString &file : std::as_const(outside)) {
        const QString relative = QFileInfo(name).completeBaseName() + QStringLiteral(".includes/") + shared.relativeFilePath(QDir::cleanPath(file));
        places.append({file, relative});
        placeOf.insert(QDir::cleanPath(file), relative);
      }
      QStringList shown;
      for (const QString &file : std::as_const(outside)) shown << QDir::toNativeSeparators(file);
      ErrText->insertPlainText(tr("%1 includes %2 from outside its folder: embedded below %3.includes/, its include lines "
                                  "naming it there.\n").arg(name, shown.join(", "), QFileInfo(name).completeBaseName()));
    }
    for (const auto &[from, to] : std::as_const(places)) {
      // Its include lines that name a file the library has elsewhere.
      static const QRegularExpression includeLine(QStringLiteral("`include\\s+\"([^\"]+)\""));
      QFile f(from);
      QString text = f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()) : QString();   // (bytes kept)
      const QFileInfo info(from);
      const QDir here = info.isSymLink() ? QFileInfo(info.symLinkTarget()).absoluteDir() : info.absoluteDir();
      const QString root = QDir::rootPath() + QStringLiteral("library/"), toDir = QFileInfo(root + to).absolutePath();   // (paths only)
      QList<QRegularExpressionMatch> lines;   // (last first: an earlier one's place kept)
      for (auto it = includeLine.globalMatch(text); it.hasNext();) lines.prepend(it.next());
      bool rewrite = false;
      for (const QRegularExpressionMatch &m : std::as_const(lines)) {
        const QString named = QString::fromUtf8(m.captured(1).toLatin1());
        const QString target = placeOf.value(QDir::cleanPath(here.absoluteFilePath(named)));
        if (target.isEmpty() || QDir::cleanPath(toDir + QLatin1Char('/') + named) == QDir::cleanPath(root + target)) continue;
        text.replace(m.capturedStart(1), m.capturedLength(1), QString::fromLatin1(QDir(toDir).relativeFilePath(root + target).toUtf8()));
        rewrite = true;
      }
      if (!(rewrite ? writeIntoLibrary(from, to, text.toLatin1()) : copyIntoLibrary(from, to))) {
        ++errors;
        continue;
      }
      if (to == name) {
        // A model compiled from what the library brought before is compiled
        // again from this source.
        if (copied) QFile::remove(folder.absoluteFilePath(QFileInfo(name).completeBaseName() + ".osdi"));
        if (!attached.contains(name)) attached << name;
      }
      embedded << to;
    }
  }
  embedded.removeDuplicates();
  if (!embedded.isEmpty())
    ErrText->insertPlainText(tr("Embedding Verilog-A: %1 (compiled with OpenVAF where the library is used)\n")
                               .arg(embedded.join(", ")));
  return errors;
}

// ---------------------------------------------------------------
void LibraryDialog::slotCheckDescrChanged(int state)
{
  if (state == Qt::Unchecked){
    ButtCreateNext->setText(tr("Create"));
  }
  else {
    ButtCreateNext->setText(tr("Next..."));
  }
}

// ---------------------------------------------------------------
void LibraryDialog::slotPrevDescr()
{
  if ( curDescr > 0 ) {
    nextButt->setDisabled(false);
    checkedCktName->setText(SelectedNames[curDescr]);
    curDescr--;
    checkedCktName->setText(SelectedNames[curDescr]);
    textDescr->setText(Descriptions[curDescr]);
  }

  if (curDescr == 0){
    prevButt->setDisabled(true);
    nextButt->setEnabled(true);
  }
}

// ---------------------------------------------------------------
void LibraryDialog::slotNextDescr()
{
  if ( curDescr < SelectedNames.count()) {
    prevButt->setDisabled(false);
    checkedCktName->setText(SelectedNames[curDescr]);
    curDescr++;
    checkedCktName->setText(SelectedNames[curDescr]);
    textDescr->setText(Descriptions[curDescr]);
  }

  if (curDescr == SelectedNames.count()-1){
    nextButt->setDisabled(true);
    createButt->setEnabled(true);
  }
}

void LibraryDialog::slotUpdateDescription()
{
  // store on every change
  Descriptions[curDescr] = textDescr->toPlainText();
}

// ---------------------------------------------------------------
void LibraryDialog::slotSave()
{
  stackedWidgets->setCurrentIndex(2); //message window
  const QString name = NameEdit->text();
  libSaveName->setText(name + ".lib");
  a_made = false;
  a_trashed.clear();

  ErrText->insertPlainText(tr("Saving library..."));

  // Made in a folder of its own beside where it goes (.NAME.qucs-new) and
  // put in place when it is whole: a library of its name there is read
  // while this one is made - a subcircuit may place its parts - and stays
  // as it was when this one cannot be made (it was written over first, and
  // removed with the failed one).
  a_staging = LibDir.absoluteFilePath(QStringLiteral(".%1.qucs-new").arg(name));
  QDir(a_staging).removeRecursively();
  if (const QString clash = nameClash(name, SelectedNames); !clash.isEmpty()) {
    ErrText->appendPlainText(tr("Error: %1.").arg(clash));
    ErrText->appendPlainText(tr("Error creating library."));
    return;
  }
  if (const QString taken = folderTaken(); !taken.isEmpty()) {
    ErrText->appendPlainText(tr("Error: %1.").arg(taken));
    ErrText->appendPlainText(tr("Error creating library."));
    return;
  }
  QFile staged(QDir(a_staging).filePath(name + ".lib"));
  if (!QDir().mkpath(a_staging) || !staged.open(QIODevice::WriteOnly)) {
    ErrText->appendPlainText(tr("Error: Cannot create library!"));
    QDir(a_staging).removeRecursively();
    return;
  }
  QTextStream Stream;
  Stream.setDevice(&staged);
  Stream << "<Qucs Library " PACKAGE_VERSION " \""
    << name << "\">\n\n";

  bool Success = true, ret;
  // A load's errors among the messages, not in boxes over the dialog.
  misc::ErrorCapture capture;
  // The Qucs models of library parts the subcircuits place (Schematic::
  // setLibraryScratch()).
  QTemporaryDir scratch(QDir(a_staging).filePath(QStringLiteral("scratch-XXXXXX")));

  QString tmp;
  QTextStream ts(&tmp, QIODevice::WriteOnly);
  a_copied.clear();
  a_spiceRoots.clear();

  for (int i=0; i < SelectedNames.count(); i++) {
    ErrText->insertPlainText("\n=================\n");

    QString description = "";
    if(checkDescr->checkState() == Qt::Checked)
      description = Descriptions[i];

    // The part: its file's name without .sch (a folder's subcircuit was
    // "sub/deep", a dotted name cut at its first dot - "div.v2" and "div"
    // one part's name).
    Stream << "<Component " + partName(SelectedNames[i]) + ">\n"
           << "  <Description>\n"
           << description
           << "\n  </Description>\n";

    Schematic *Doc = new Schematic(0, QucsSettings.QucsWorkDir.filePath(SelectedNames[i]));
    ErrText->insertPlainText(tr("Loading subcircuit \"%1\".\n").arg(SelectedNames[i]));
    if(!Doc->loadDocument()) {  // load document if possible
        delete Doc;
        ErrText->appendPlainText(tr("Error: Cannot load subcircuit \"%1\".").
          arg(SelectedNames[i]));
        Success = false;   // (it said "Successfully created" and kept the half-written library)
        break;
    }
    // Its subcircuit's name: the library's and the file's (not the folder's:
    // sub/deep.sch was a subcircuit "deep", called as Lib_sub_deep).
    Doc->setDocName(name + "_" + QFileInfo(SelectedNames[i]).fileName());
    Doc->setLibraryScratch(scratch.path());
    bool partMade = true;

    // save analog model
    tmp.truncate(0);
    Doc->setIsAnalog(true);

    ErrText->insertPlainText("\n");
    ErrText->insertPlainText(tr("Creating Qucs netlist.\n"));
    int sim = QucsSettings.DefaultSimulator;
    QucsSettings.DefaultSimulator = spicecompat::simQucsator;
    ret = Doc->createLibNetlist(&ts, ErrText, -1);
    QucsSettings.DefaultSimulator = sim;
    if(ret) {
      const QString qucsModel = tmp;   // (said below when it has no device of Qucsator's)
      intoStream(Stream, tmp, "Model");
      int error = 0;
      QStringList IFiles;
      SubMap::Iterator it = FileList.begin();
      while(it != FileList.end()) {
          QString f = it.value().File;
          QString ifn, ofn;
          if(it.value().Type == "SCH") {
              ifn = f + ".lst";
              ofn = ifn;
          }
          else if(it.value().Type == "CIR") {
              ifn = f + ".lst";
              ofn = ifn;
          }
          if (!ifn.isEmpty()) error += intoFile(ifn, ofn, IFiles);
          it++;
      }
      FileList.clear();
      if(!IFiles.isEmpty()) {
          Stream << "  <ModelIncludes \"" << IFiles.join("\" \"") << "\">\n";
      }
      // No device of Qucsator's in it, nor in what it includes - a wrapper of
      // SPICE or Verilog-A devices: Qucsator leaves the part out, every node
      // through it open (Check Schematic says so where it is placed).
      QString included = qucsModel;
      for (const QString &lst : std::as_const(IFiles)) {
        QFile f(QDir(modelsFolder()).absoluteFilePath(lst));
        if (f.open(QIODevice::ReadOnly)) included += QString::fromUtf8(f.readAll());
      }
      bool spiceDevices = false;
      for (Component *pc : Doc->a_DocComps)
        if (pc->isActive == COMP_IS_ACTIVE && !pc->isEquation && !pc->isProbe && !pc->SpiceModel.isEmpty() && pc->Model != QLatin1String("Port")
            && !pc->SpiceModel.startsWith(QLatin1Char('.')))
          spiceDevices = true;
      if (spiceDevices && !LibComp::qucsDevicesIn(included))
        ErrText->insertPlainText(tr("Note: \"%1\" has no Qucs model (its devices are SPICE's or Verilog-A): Qucsator leaves "
                                    "this part out - simulate it with ngspice.\n").arg(SelectedNames[i]));
      if (error > 0) partMade = false;
    }
    else {
        ErrText->insertPlainText("\n");
        ErrText->insertPlainText(tr("Error: Cannot create netlist for \"%1\".\n").arg(SelectedNames[i]));
        partMade = false;
    }

    if (QucsSettings.DefaultSimulator == spicecompat::simQucsator ) {
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
    }
        tmp.truncate(0);
        QTextStream ts(&tmp,QIODevice::WriteOnly);
        ErrText->insertPlainText("\n");
        ErrText->insertPlainText(tr("Creating SPICE netlist.\n"));
        // Its own ground pin, whatever the settings (or the request) say.
        if (const LibrarySettings own = Doc->librarySettings(); own.groundPin != LibrarySettings::Default) {
          const Component* held = Doc->libraryExport();
          const QString where = held != nullptr ? tr("its Library Export %1").arg(held->Name) : tr("its Document Settings > Library");
          ErrText->insertPlainText(own.groundPin == LibrarySettings::With
              ? tr("Ground pin: a first pin gnd in its .SUBCKT, as %1 asks.\n").arg(where)
              : tr("Ground pin: none in its .SUBCKT, as %1 asks.\n").arg(where));
        }
        AbstractSpiceKernel *kern = new AbstractSpiceKernel(Doc);
        QStringList err_lst;
        if (!kern->checkSchematic(err_lst)) {
             ErrText->insertPlainText(QStringLiteral("Component %1 contains SPICE-incompatible components.\n"
                                "Check these components: %2 \n")
                    .arg(Doc->getDocName()).arg(err_lst.join("; ")));
        }
        kern->createSubNetlist(ts,true);
        const QString spiceNetlist = tmp;
        intoStream(Stream, tmp, "Spice");

        // The SPICE files it uses - its SPICE library parts', its
        // .INCLUDEs', the library parts' it places - with the files they
        // include, each under a name of its own (copySpiceFile()).
        QStringList copiedFiles;
        const QStringList spiceFiles = kern->collectSpiceLibraryFiles(Doc);
        for (const QString &file : spiceFiles) {
          const QString attached = copySpiceFile(file);
          if (attached.isEmpty()) partMade = false;
          else if (!copiedFiles.contains(attached)) copiedFiles << attached;
        }
        // A .LIB directive's file and section are not taken in: the part
        // needs the file where it is and the circuit's own .LIB of it.
        for (Component *pc : Doc->a_DocComps)
          if (pc->Model == QLatin1String("SpiceLib") && pc->isActive == COMP_IS_ACTIVE)
            ErrText->insertPlainText(tr("Warning: %1, a .LIB directive (%2), is not embedded: a circuit using the part needs "
                                        "a .LIB of its own of that file and section.\n")
                                       .arg(pc->Name, QDir::toNativeSeparators(pc->Props.value(0) != nullptr ? pc->Props.at(0)->Value : QString())));
        if (QucsSettings.EmbedVerilogAInLibraries) {
          const QString base = QFileInfo(QucsSettings.QucsWorkDir.filePath(SelectedNames[i])).absolutePath();
          // The .model cards of the SPICE files it attaches too, and of
          // those they include: a wrapper subcircuit in a SPICE file brings
          // its N device's card, whose Verilog-A was left behind - placed
          // elsewhere, the part found no model.
          QString scanned = spiceNetlist;
          for (const QString &file : spiceFiles) scanned += QStringLiteral("\n.include \"%1\"\n").arg(file);
          if (embedVerilogA(Doc, scanned, base, copiedFiles) > 0)
            partMade = false;
        }
        if (!copiedFiles.isEmpty()) {
          Stream << "<SpiceAttach \"" << copiedFiles.join("\" \"")
                 << "\">\n";
        }
        // The Verilog-A modules its .model cards name (those a source or a
        // compiled model here defines), embedded or not: a library made
        // without them recorded nothing, and where it was brought only the
        // run told - "no loaded OSDI defines" - not that the library lacked
        // them (LibComp::verilogAModules()).
        {
          QString scanned = spiceNetlist;
          for (const QString &file : spiceFiles) scanned += QStringLiteral("\n.include \"%1\"\n").arg(file);
          const QStringList modules = verilogAModulesOf(Doc, scanned, QFileInfo(QucsSettings.QucsWorkDir.filePath(SelectedNames[i])).absolutePath());
          if (!modules.isEmpty()) Stream << "  <VerilogAModules \"" << modules.join("\" \"") << "\">\n";
        }
        // Its Verilog-A loaded in every circuit of a project that has the
        // library, placed or not: its Document Settings > Library ask for it.
        if (Doc->getAlwaysLoadOSDI()) {
          Stream << "  <AlwaysLoadOSDI>\n";
          const bool verilogA = std::any_of(copiedFiles.cbegin(), copiedFiles.cend(), [](const QString &f) {
            return f.endsWith(QLatin1String(".va"), Qt::CaseInsensitive) || f.endsWith(QLatin1String(".osdi"), Qt::CaseInsensitive);
          });
          ErrText->insertPlainText(verilogA
              ? tr("Marked: every circuit of a project that has the library loads its Verilog-A models.\n")
              : tr("Marked to load its Verilog-A models in the project's circuits, but the library has none "
                   "of it (embedding Verilog-A is off, or it uses none): the mark loads nothing.\n"));
        }
        // Its .model cards written in every circuit of a project that has the
        // library, placed or not: its Document Settings > Library ask for it.
        if (Doc->getAlwaysModelCards()) {
          Stream << "  <AlwaysModelCards>\n";
          ErrText->insertPlainText(!Schematic::modelCardsOf(Doc->getModelCards()).isEmpty()
              ? tr("Marked: every circuit of a project that has the library has its .model cards.\n")
              : tr("Marked to write its .model cards in the project's circuits, but it has none (Document Settings > "
                   "Library): the mark writes nothing.\n"));
        }
        delete kern;
        // The subcircuits written into the SPICE netlist: forgotten. Kept, the
        // next netlist built - a simulation's, the Verilog-A it compiles -
        // took the library's parts for written and left their model out.
        FileList.clear();
        QucsSettings.DefaultSimulator = sim;

  if (!checkAnalogLib->isChecked()) {
    // save verilog model
    tmp.truncate(0);
    Doc->setIsVerilog(true);
    Doc->setIsAnalog(false);

    ErrText->insertPlainText("\n");
    ErrText->insertPlainText(tr("Creating Verilog netlist.\n"));
    ret = Doc->createLibNetlist(&ts, ErrText, 0);
    if(ret) {
      intoStream(Stream, tmp, "VerilogModel");
      int error = 0;
      QStringList IFiles;
      SubMap::Iterator it = FileList.begin();
      while(it != FileList.end()) {
          QString f = it.value().File;
          QString ifn, ofn;
          if(it.value().Type == "SCH") {
              ifn = f + ".lst";
              ofn = f + ".v";
          }
          else if(it.value().Type == "VER") {
              ifn = f;
              ofn = ifn;
          }
          if (!ifn.isEmpty()) error += intoFile(ifn, ofn, IFiles);
          it++;
      }
      FileList.clear();
      if(!IFiles.isEmpty()) {
          Stream << "  <VerilogModelIncludes \""
                 << IFiles.join("\" \"") << "\">\n";
      }
      // (An earlier failure - a Verilog-A source not embedded - stays one.)
      if (error > 0) partMade = false;
    }
    else {
        ErrText->insertPlainText("\n");
    }

    // save vhdl model
    tmp.truncate(0);
    Doc->setIsVerilog(false);
    Doc->setIsAnalog(false);

    ErrText->insertPlainText(tr("Creating VHDL netlist.\n"));
    ret = Doc->createLibNetlist(&ts, ErrText, 0);
    if(ret) {
      intoStream(Stream, tmp, "VHDLModel");
      int error = 0;
      QStringList IFiles;
      SubMap::Iterator it = FileList.begin();
      while(it != FileList.end()) {
          QString f = it.value().File;
          QString ifn, ofn;
          if(it.value().Type == "SCH") {
              ifn = f + ".lst";
              ofn = f + ".vhdl";
          }
          else if(it.value().Type == "VHD") {
              ifn = f;
              ofn = ifn;
          }
          if (!ifn.isEmpty()) error += intoFile(ifn, ofn, IFiles);
          it++;
      }
      FileList.clear();
      if(!IFiles.isEmpty()) {
          Stream << "  <VHDLModelIncludes \""
                 << IFiles.join("\" \"") << "\">\n";
      }
      if (error > 0) partMade = false;
      }
      else {
          ErrText->insertPlainText("\n");
      }
    }

      Stream << "  <Symbol>\n";
      Doc->createSubcircuitSymbol();
      for(Painting* pp : Doc->a_SymbolPaints)
        Stream << "    <" << pp->save() << ">\n";

      Stream << "  </Symbol>\n"
             << "</Component>\n\n";

      delete Doc;

      if(!partMade) {
        Success = false;
        break;
      }

  } // for

  Stream.flush();
  staged.close();
  for (const QString &e : capture.errors()) ErrText->appendPlainText(e);
  const QString finalLib = LibFile.fileName();
  const QString finalModels = LibDir.absoluteFilePath(name);
  const QString stagedModels = modelsFolder();
  if (Success && staged.error() != QFileDevice::NoError) {
    ErrText->appendPlainText(tr("Error: %1 could not be written: %2").arg(QDir::toNativeSeparators(staged.fileName()), staged.errorString()));
    Success = false;
  }
  const auto there = [](const QString &path) { return QFileInfo::exists(path) || QFileInfo(path).isSymLink(); };
  if(!Success) {
    QDir(a_staging).removeRecursively();
    ErrText->appendPlainText(tr("Error creating library."));
    if (there(finalLib))
      ErrText->appendPlainText(tr("The library %1 there is as it was.").arg(QDir::toNativeSeparators(finalLib)));
    return;
  }

  // In place: what was there (the library, its folder) to the trash first.
  // A folder of its name with no library of it (one whose .lib was taken
  // away, someone's own) is not taken away: the library's files go into it,
  // as they always did.
  const bool replacing = there(finalLib);
  QStringList removed;
  for (const QString &old : {finalLib, finalModels}) {
    if (!there(old) || (old == finalModels && !replacing)) continue;
    QString where;
    if (misc::moveToTrash(old, &where)) {
      a_trashed << where;
      removed << old;
      continue;
    }
    // No trash there (a share): the old library written over, as Rewrite
    // asked - unless asked to keep it then (create()'s replace).
    if (!a_mustTrash && (QFileInfo(old).isDir() && !QFileInfo(old).isSymLink() ? QDir(old).removeRecursively() : QFile::remove(old))) {
      ErrText->appendPlainText(tr("%1 could not be moved to the trash: it was written over.").arg(QDir::toNativeSeparators(old)));
      removed << old;
      continue;
    }
    // Put back what went already; the new one is not put in place.
    for (int k = 0; k < a_trashed.size(); ++k) QDir().rename(a_trashed.at(k), removed.at(k));
    a_trashed.clear();
    QDir(a_staging).removeRecursively();
    ErrText->appendPlainText(tr("Error: %1 could not be moved to the trash: the library there is as it was.").arg(QDir::toNativeSeparators(old)));
    ErrText->appendPlainText(tr("Error creating library."));
    return;
  }
  bool placed = QDir().rename(staged.fileName(), finalLib);
  if (placed && QFileInfo(stagedModels).isDir() && !QDir(stagedModels).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
    if (!there(finalModels)) {
      placed = QDir().rename(stagedModels, finalModels);
    } else {
      // Into the folder there: each file over its own, and a model compiled
      // from an earlier source of a Verilog-A file (name.osdi beside
      // name.va) taken away - compiled again from this one where used.
      QStringList files;
      for (QDirIterator it(stagedModels, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); it.hasNext();) files << it.next();
      for (const QString &file : std::as_const(files)) {
        const QString relative = QDir(stagedModels).relativeFilePath(file);
        const QString target = QDir(finalModels).filePath(relative);
        QString why;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !misc::copyFileOver(file, target, &why)) {
          ErrText->appendPlainText(tr("Error: %1 could not be written: %2").arg(QDir::toNativeSeparators(target), why));
          placed = false;
          break;
        }
        if (relative.endsWith(QLatin1String(".va"), Qt::CaseInsensitive))
          QFile::remove(QDir(finalModels).filePath(relative.chopped(3) + QStringLiteral(".osdi")));
      }
      if (!placed) QFile::remove(finalLib);   // (not a library without its files)
    }
  }
  QDir(a_staging).removeRecursively();
  if (!placed) {
    ErrText->appendPlainText(tr("Error: the library could not be put in %1.").arg(QDir::toNativeSeparators(LibDir.absolutePath())));
    ErrText->appendPlainText(tr("Error creating library."));
    return;
  }
  a_made = true;

  ErrText->appendPlainText(tr("Successfully created library."));
  if (!a_trashed.isEmpty()) ErrText->appendPlainText(tr("The library it replaced is in the trash."));
  // Another library of its name: a part placed by the name could be either's.
  if (const QStringList others = LibComp::librariesNamedLike(LibFile.fileName()); !others.isEmpty()) {
    QStringList shown;
    for (const QString& other : others) shown << QDir::toNativeSeparators(other);
    ErrText->appendPlainText(tr("Note: another library is named %1 too: %2. A part placed by that name is taken from the first of "
                                "them that has it - installed, beside the schematic, the project's, user_lib's, the library search "
                                "paths' in their order; the Libraries panel places this one's parts by their path where the name "
                                "finds another.").arg(name, shown.join(QStringLiteral(", "))));
  }
}

// ---------------------------------------------------------------
bool LibraryDialog::create(const Request &request, QString *log, QString *error, QStringList *trashed)
{
  const auto fail = [error](const QString &why) {
    if (error != nullptr) *error = why;
    return false;
  };
  static const QRegularExpression whole(QStringLiteral("^\\w+$"));   // (as the name field takes it)
  if (!whole.match(request.name).hasMatch())
    return fail(tr("a library's name is letters, digits and _ (%1 is not)").arg(request.name));
  if (request.subcircuits.isEmpty()) return fail(tr("no subcircuit to put in it"));
  if (const QString clash = nameClash(request.name, request.subcircuits); !clash.isEmpty()) return fail(clash);
  if (!QDir().mkpath(request.folder))
    return fail(tr("the folder %1 cannot be made").arg(QDir::toNativeSeparators(request.folder)));
  LibDir = QDir(request.folder);
  LibFile.setFileName(LibDir.absoluteFilePath(request.name) + ".lib");
  if (LibFile.exists() && !request.replace)
    return fail(tr("%1 is there already ('replace' writes over it)").arg(QDir::toNativeSeparators(LibFile.fileName())));
  NameEdit->setText(request.name);
  if (const QString taken = folderTaken(); !taken.isEmpty()) return fail(taken);
  SelectedNames = request.subcircuits;
  Descriptions.clear();
  // One not given, of a library it replaces: the description that part had
  // there (create_library with 'replace' dropped them all).
  const QHash<QString, QString> before = LibFile.exists() ? descriptionsOf(LibFile.fileName()) : QHash<QString, QString>();
  for (const QString &sub : request.subcircuits) {
    const QString bare = QFileInfo(sub).completeBaseName();
    const QString given = request.descriptions.value(sub, request.descriptions.value(bare));
    Descriptions.append(given.isEmpty() && !request.descriptions.contains(sub) && !request.descriptions.contains(bare)
                            ? before.value(partName(sub)) : given);
  }
  checkAnalogLib->setChecked(request.analogOnly);
  const bool embed = QucsSettings.EmbedVerilogAInLibraries, ground = QucsSettings.LibraryGroundPin;
  QucsSettings.EmbedVerilogAInLibraries = request.embedVerilogA;
  QucsSettings.LibraryGroundPin = request.groundPin;
  a_mustTrash = true;   // (a replaced library is said to be in the trash)
  slotSave();
  a_mustTrash = false;
  QucsSettings.EmbedVerilogAInLibraries = embed;
  QucsSettings.LibraryGroundPin = ground;
  if (log != nullptr) *log = ErrText->toPlainText();
  if (trashed != nullptr) *trashed = a_trashed;
  if (!a_made)
    return fail(LibFile.exists() ? tr("the library was not made (its messages say why); the one there is as it was")
                                 : tr("the library was not made (its messages say why)"));
  return true;
}

// ---------------------------------------------------------------
void LibraryDialog::slotSelectAll()
{
    for (int i = 0; i < subcirFileList->count(); i++) {
        auto itm = subcirFileList->item(i);
        itm->setCheckState(Qt::Checked);
    }
}

// ---------------------------------------------------------------
void LibraryDialog::slotSelectNone()
{
    for (int i = 0; i < subcirFileList->count(); i++) {
        auto itm = subcirFileList->item(i);
        itm->setCheckState(Qt::Unchecked);
    }
}
