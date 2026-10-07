/*
 * librarydialog.h - declaration of dialog to create library
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

#ifndef LIBRARYDIALOG_H
#define LIBRARYDIALOG_H

#include <QRegularExpression>
#include <QRegularExpressionValidator>

#include <QList>
#include <QStringList>
#include <QTextStream>
#include <QDialog>
#include <QFile>
#include <QDir>
#include <QHash>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QComboBox>

class QLabel;
class QLineEdit;
class QTextEdit;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QTreeWidgetItem;
class QGroupBox;
class QRegExpValidator;
class QStackedWidget;
//class QStringList;
class QListWidget;


class Schematic;

class LibraryDialog : public QDialog {
   Q_OBJECT
public:
  LibraryDialog(QWidget *);
 ~LibraryDialog();

  void fillSchematicList(QStringList);

  /// A library made as Create creates it, without its pages (Claude's
  /// create_library): \a name, of the \a subcircuits (names as
  /// fillSchematicList was given them), into \a folder; each subcircuit's
  /// description by its name; analog only, or with Verilog and VHDL models;
  /// Verilog-A embedded or not; a library of that name there replaced only
  /// when \a replace - moved to the trash once the new one is made, and
  /// the new one not put in its place when it cannot be (\a trashed: where
  /// it went). False and why in \a error (nothing written, a library there
  /// as it was); what it said in \a log.
  struct Request {
    QString name;
    QStringList subcircuits;
    QString folder;
    QHash<QString, QString> descriptions;
    bool analogOnly = true;
    bool embedVerilogA = true;
    bool groundPin = false;   ///< each .SUBCKT's first pin gnd (QucsSettings.LibraryGroundPin)
    bool replace = false;
  };
  bool create(const Request &request, QString *log, QString *error, QStringList *trashed = nullptr);

  /// The part a subcircuit file becomes in a library: its file's name
  /// without ".sch" - div.v2 of div.v2.sch, deep of sub/deep.sch.
  static QString partName(const QString &subcircuit);
  /// Why the \a subcircuits cannot all be parts of one library \a name:
  /// two that would be parts of one name, or SPICE subcircuits of one name
  /// (any case, as SPICE reads names); empty when they can.
  static QString nameClash(const QString &name, const QStringList &subcircuits);

private slots:
  void slotCreateNext();
  void slotSave();
  void slotSelectNone();
  void slotSelectAll();
  void slotCheckDescrChanged(int);
  void slotPrevDescr();
  void slotNextDescr();
  void slotUpdateDescription();

private:
  void intoStream(QTextStream&, QString&, const char*);
  int intoFile(QString&, QString&,  QStringList&);
  /// The Verilog-A a subcircuit's SPICE netlist \a spice uses, into the
  /// library's folder (QucsSettings.EmbedVerilogAInLibraries): the sources
  /// defining the modules its .model cards name, with the files they
  /// include - from the project and from the libraries of the subcircuit's
  /// components. No compiled model (.osdi): it runs on one platform only,
  /// and a simulation compiles the source where the library is used. The
  /// .va names go to \a attached; returns the errors.
  int embedVerilogA(Schematic *doc, const QString &spice, const QString &baseDir, QStringList &attached);
  /// Copies \a from into the library's folder as \a name (a path in it);
  /// a name another file already took this time is an error.
  bool copyIntoLibrary(const QString &from, const QString &name);
  bool writeIntoLibrary(const QString &from, const QString &name, const QByteArray &bytes);
  QStringList verilogAModulesOf(Schematic *doc, const QString &spice, const QString &baseDir);
  static QHash<QString, QString> descriptionsOf(const QString &file);
  /// A SPICE file a subcircuit uses (a SPICE library part's, an .INCLUDE's)
  /// into the library's folder, with the files it includes where it finds
  /// them: under its own name, or in a folder named as the one it is in
  /// when another file took the name (two vendors' models.lib). What the
  /// library attaches it as; empty when it could not be copied.
  QString copySpiceFile(const QString &file);
  /// Where the library's folder of files is made: in the staging folder
  /// beside the library's (slotSave()), put in place when it is whole.
  QString modelsFolder() const;
  /// Why the library's folder of files (NAME/ where it goes) cannot be
  /// it: a folder of the project's own (Scratch, Libraries); empty when it
  /// can.
  QString folderTaken() const;

private:
  int curDescr;
  QVBoxLayout *all;   // the mother of all widgets
  QVBoxLayout *subcirListLayout;
  QStackedWidget *stackedWidgets;
  QLabel *theLabel;
  QLabel *checkedCktName;
  QLabel *libSaveName;
  QLineEdit *NameEdit;
  QComboBox *Destination;   // where the library goes: user_lib, the project, a library search path
  QPlainTextEdit *ErrText;
  QTextEdit *textDescr;
  QGroupBox *Group;
  QPushButton *ButtCreateNext, *ButtCancel, *ButtSelectAll, *ButtSelectNone;
  QPushButton *prevButt, *nextButt;
  QPushButton *createButt;
  QListWidget *subcirFileList;
  //QList<QCheckBox *> BoxList;
  QStringList SelectedNames;
  QStringList Descriptions;
  QCheckBox *checkDescr;
  QCheckBox *checkAnalogLib;

  QFile LibFile;
  QDir LibDir;
  QHash<QString, QString> a_copied;   // what the library's folder got this time: name -> source
  QHash<QString, QString> a_spiceRoots;   // a SPICE file taken in this time -> what it is attached as
  QString a_staging;       // .NAME.qucs-new beside the library: where it is made
  bool a_made = false;     // the last slotSave() put a library in place
  bool a_mustTrash = false;   // a library there replaced only when it can go to the trash (create())
  QStringList a_trashed;   // where what it replaced went
  QRegularExpression Expr;
  QRegularExpressionValidator *Validator;
};

#endif
