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

private:
  int curDescr;
  QVBoxLayout *all;   // the mother of all widgets
  QVBoxLayout *subcirListLayout;
  QStackedWidget *stackedWidgets;
  QLabel *theLabel;
  QLabel *checkedCktName;
  QLabel *libSaveName;
  QLineEdit *NameEdit;
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
  QRegularExpression Expr;
  QRegularExpressionValidator *Validator;
};

#endif
