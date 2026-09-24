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

#include "librarydialog.h"
#include "main.h"
#include "misc.h"
#include "osdiselection.h"
#include "painting.h"
#include "extsimkernels/abstractspicekernel.h"
#include "extsimkernels/spicecompat.h"

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

  // ...........................................................
  Group = new QGroupBox(tr("Choose subcircuits:"));
  selectSubcktLayout->addWidget(Group);

  subcirFileList = new QListWidget();
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

  LibDir = QDir(QucsSettings.qucsWorkspaceDir);
  if(!LibDir.cd("user_lib")) { // user library directory exists ?
    if(!LibDir.mkdir("user_lib")) { // no, then create it
      QMessageBox::warning(this, tr("Warning"),
                   tr("Cannot create user library directory !"));
      return;
    }
    LibDir.cd("user_lib");
  }

  /*LibFile.setFileName(QucsSettings.LibDir + NameEdit->text() + ".lib");
  if(LibFile.exists()) {
    QMessageBox::critical(this, tr("Error"), tr("A system library with this name already exists!"));
    return;
  }*/

  LibFile.setFileName(LibDir.absoluteFilePath(NameEdit->text()) + ".lib");
  if(LibFile.exists()) {
    auto ans = QMessageBox::question(this, tr("Error"),
                          tr("A library with this name already exists! Rewrite?"),
                          QMessageBox::Yes, QMessageBox::No);
    if (ans == QMessageBox::No) return;
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
int LibraryDialog::intoFile(QString &ifn, QString &ofn, QStringList &IFiles)
{
  int error = 0;
  QFile ifile(ifn);
  if(!ifile.open(QIODevice::ReadOnly)) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot open file \"%1\".\n").
        arg(ifn));
    error++;
  }
  else {
    QByteArray FileContent = ifile.readAll();
    ifile.close();
    if(ifile.fileName().right(4) == ".lst")
      LibDir.remove(ifile.fileName());
    QDir LibDirSub(LibDir);
    if(!LibDirSub.cd(NameEdit->text())) {
      if(!LibDirSub.mkdir(NameEdit->text())) {
        ErrText->insertPlainText(
        QObject::tr("ERROR: Cannot create user library subdirectory !\n"));
        error++;
      }
      LibDirSub.cd(NameEdit->text());
    }
    QFileInfo Info(ofn);
    ofn = Info.fileName();
    IFiles.append(ofn);
    QFile ofile;
    ofile.setFileName(LibDirSub.absoluteFilePath(ofn));
    if(!ofile.open(QIODevice::WriteOnly)) {
      ErrText->insertPlainText(
        QObject::tr("ERROR: Cannot create file \"%1\".\n").arg(ofn));
      error++;
    }
    else {
      QDataStream ds(&ofile);
      ds.writeRawData(FileContent.data(), FileContent.size());
      ofile.close();
    }
  }
  return error;
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
  QDir folder(LibDir);
  if (!folder.mkpath(NameEdit->text()) || !folder.cd(NameEdit->text())) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot create user library subdirectory !\n"));
    return false;
  }
  const QString target = folder.absoluteFilePath(name);
  if (QFileInfo(target).canonicalFilePath() == QFileInfo(source).canonicalFilePath()) {
    a_copied.insert(name, source);   // there already: the library made again, a component of it in use
    return true;
  }
  QDir().mkpath(QFileInfo(target).absolutePath());
  QFile::remove(target);
  if (!QFile::copy(source, target)) {
    ErrText->insertPlainText(QObject::tr("ERROR: Cannot create file \"%1\".\n").arg(name));
    return false;
  }
  a_copied.insert(name, source);
  return true;
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
  for (const QString &file : AbstractSpiceKernel::collectVerilogAFiles(doc)) {
    if (!QFileInfo(file).isFile()) continue;
    if (file.endsWith(".va", Qt::CaseInsensitive) && !sources.contains(file)) sources << file;
    else if (file.endsWith(".osdi", Qt::CaseInsensitive) && !libraries.contains(file)) libraries << file;
  }

  // The sources of the modules the .model cards name. A compiled model
  // (.osdi) runs on one platform only: the library brings the source, and
  // OpenVAF compiles it where the library is used.
  QStringList definingSources;
  QSet<QString> fromSource;
  for (const QString &va : std::as_const(sources))
    for (const QString &type : types)
      if (qucs_s::osdi::sourceDefines(va, type)) {
        if (!definingSources.contains(va)) definingSources << va;
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
  if (definingSources.isEmpty()) return 0;   // no Verilog-A in it

  const QDir folder(LibDir.absoluteFilePath(NameEdit->text()));
  int errors = 0;
  QStringList embedded;
  for (const QString &va : std::as_const(definingSources)) {
    const QString name = QFileInfo(va).fileName();
    const bool copied = !a_copied.contains(name)
        && QFileInfo(folder.absoluteFilePath(name)).canonicalFilePath() != QFileInfo(va).canonicalFilePath();
    if (!copyIntoLibrary(va, name)) { ++errors; continue; }
    // A model compiled from what the library brought before is compiled
    // again from this source.
    if (copied) QFile::remove(folder.absoluteFilePath(QFileInfo(name).completeBaseName() + ".osdi"));
    if (!attached.contains(name)) attached << name;
    embedded << name;
    // The files it includes, where it finds them: beside it, or below.
    const QDir sourceFolder = QFileInfo(va).absoluteDir();
    for (const QString &included : qucs_s::osdi::sourceIncludes(va)) {
      const QString relative = sourceFolder.relativeFilePath(included);
      if (relative.startsWith("..")) {
        ErrText->insertPlainText(tr("Warning: %1 includes %2 from outside its folder; it is not "
                                    "embedded.\n").arg(QFileInfo(va).fileName(),
                                                        QDir::toNativeSeparators(included)));
        continue;
      }
      if (copyIntoLibrary(included, relative)) embedded << relative;
      else ++errors;
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
  libSaveName->setText(NameEdit->text() + ".lib");

  ErrText->insertPlainText(tr("Saving library..."));

  if(!LibFile.open(QIODevice::WriteOnly)) {
    ErrText->appendPlainText(tr("Error: Cannot create library!"));
    return;
  }
  QTextStream Stream;
  Stream.setDevice(&LibFile);
  Stream << "<Qucs Library " PACKAGE_VERSION " \""
    << NameEdit->text() << "\">\n\n";

  bool Success = true, ret;

  QString tmp;
  QTextStream ts(&tmp, QIODevice::WriteOnly);
  a_copied.clear();

  for (int i=0; i < SelectedNames.count(); i++) {
    ErrText->insertPlainText("\n=================\n");

    QString description = "";
    if(checkDescr->checkState() == Qt::Checked)
      description = Descriptions[i];

    Stream << "<Component " + SelectedNames[i].section('.',0,0) + ">\n"
           << "  <Description>\n"
           << description
           << "\n  </Description>\n";

    Schematic *Doc = new Schematic(0, QucsSettings.QucsWorkDir.filePath(SelectedNames[i]));
    ErrText->insertPlainText(tr("Loading subcircuit \"%1\".\n").arg(SelectedNames[i]));
    if(!Doc->loadDocument()) {  // load document if possible
        delete Doc;
        ErrText->appendPlainText(tr("Error: Cannot load subcircuit \"%1\".").
          arg(SelectedNames[i]));
        break;
    }
    Doc->setDocName(NameEdit->text() + "_" + SelectedNames[i]);
    Success = false;

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
      Success = error > 0 ? false : true;
    }
    else {
        ErrText->insertPlainText("\n");
        ErrText->insertPlainText(tr("Error: Cannot create netlist for \"%1\".\n").arg(SelectedNames[i]));
    }

    //if (QucsSettings.DefaultSimulator != spicecompat::simQucsator ) { // SPICE
    if (QucsSettings.DefaultSimulator == spicecompat::simQucsator ) {
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
    }
        tmp.truncate(0);
        QTextStream ts(&tmp,QIODevice::WriteOnly);
        ErrText->insertPlainText("\n");
        ErrText->insertPlainText(tr("Creating SPICE netlist.\n"));
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

        QStringList libs = kern->collectSpiceLibraryFiles(Doc);
        QStringList copiedFiles;
        for (QString &file: libs) {
          QString ofile = file;
          intoFile(file, ofile, copiedFiles);
        }
        if (QucsSettings.EmbedVerilogAInLibraries) {
          const QString base = QFileInfo(QucsSettings.QucsWorkDir.filePath(SelectedNames[i])).absolutePath();
          if (embedVerilogA(Doc, spiceNetlist, base, copiedFiles) > 0)
            Success = false;
        }
        if (!copiedFiles.isEmpty()) {
          Stream << "<SpiceAttach \"" << copiedFiles.join("\" \"")
                 << "\">\n";
        }
        delete kern;
        QucsSettings.DefaultSimulator = sim;
    //}

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
      Success = error > 0 ? false : true;
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
      Success = error > 0 ? false : true;
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

      if(!Success) break;

  } // for

  LibFile.close();
  if(!Success) {
    LibFile.remove();
    ErrText->appendPlainText(tr("Error creating library."));
    return;
  }

  ErrText->appendPlainText(tr("Successfully created library."));
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
