/***************************************************************************
                             messagedock.h
                             -------------
    begin                : Tue Mar 11 2014
    copyright            : (C) 2014 by Guilherme Brondani Torri
    email                : guitorri AT gmail DOT com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef MESSAGEDOCK_H
#define MESSAGEDOCK_H

#include "qucs.h"

#include <QWidget>
#include <QPointer>
#include "erc.h"
#include "textdoc.h"

class QDockWidget;
class QTabWidget;
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;

/*!
 * \file messagedock.h
 * \brief The MessageDock class definiion
 */
class QListWidget;
class Schematic;

class MessageDock : public QWidget {
  Q_OBJECT
public:
  MessageDock(QucsApp*);
 ~MessageDock() {};

public:

  QDockWidget *msgDock;

  QTabWidget *builderTabs;

  /*!
   * \brief problems lists what the electrical rule check found in a
   * schematic (erc.h); a click on a row locates it in the schematic.
   */
  QListWidget *problems;
  /// Shows \a issues of \a doc on the Problems tab (an empty list says
  /// so), and brings the dock up when asked to. \a hierarchy: they are
  /// those of \a doc and of every subcircuit it uses (Check Hierarchy).
  void showProblems(Schematic* doc, const QList<qucs_s::erc::Issue>& issues, bool raise, bool hierarchy = false);
  Schematic* problemsDocument() const;
  /// Shows \a list - what a check found in the text document \a doc (a
  /// Python script's) - on the Problems tab: a row each, its file, line
  /// and message, \a checkedBy in the tooltips (an empty list says none
  /// were found). A click on a row goes to its place (lineRequested()).
  /// The rows go with the document.
  /// \a failure: it could not be checked, and why (said in place of
  /// "No problems found").
  void showTextProblems(TextDoc* doc, const QList<TextDoc::Diagnostic>& list, const QString& checkedBy, bool raise,
                        const QString& failure = QString());
  /// The text document whose problems the tab shows; nullptr when it
  /// shows a schematic's, or none.
  TextDoc* textProblemsDocument() const;
  const QList<TextDoc::Diagnostic>& textProblems() const { return a_textProblems; }
  bool problemsOfHierarchy() const { return a_problemsHierarchy; }
  const QList<qucs_s::erc::Issue>& issues() const { return a_issues; }

  /*!
   * \brief operatingPoint lists the operating point of every device after
   * a DC bias run (oppoint.h): a component, its device(s), their
   * parameters; a click on a component locates it in the schematic.
   */
  QTreeWidget *operatingPoint;
  QLineEdit *operatingPointFilter;
  /// Shows the operating point of \a doc's last DC bias run on the
  /// Operating Point tab, and brings the tab up when asked to.
  void showOperatingPoint(Schematic* doc, bool raise);
  Schematic* operatingPointDocument() const;
  /// The Operating Point tab in front, the dock shown.
  void raiseOperatingPoint();
  /// The rows the filter leaves, as text: component, device, parameter,
  /// value, unit - tab-separated, one parameter a line.
  QString operatingPointText() const;

  /*!
   * \brief admsOutput holds the make output of running admsXml
   */
  QPlainTextEdit *admsOutput;
  /*!
   * \brief cppOutput holds the make output of running a C++ compiler
   */
  QPlainTextEdit *cppOutput;

  void reset();
  /// The build's output (OpenVAF's, admsXml's) in front: its tab chosen,
  /// the dock shown and raised over the docks it shares the bottom of the
  /// window with (the simulation console, the terminal...).
  void showBuildOutput();

signals:
  /// A row of the Problems tab was chosen: issues()[index] is to be shown.
  void locateRequested(int index);
  /// A row of a text document's problems was chosen: \a line and \a
  /// column (1-based; 0: the line's start) of \a document are to be shown.
  void lineRequested(QWidget *document, int line, int column);
  /// A row of the Operating Point tab was chosen: the component of that
  /// name in operatingPointDocument() is to be shown.
  void componentRequested(const QString &component);


private slots:
  void slotAdmsChanged();
  void slotCppChanged();
  void slotCursor();
  void slotProblemChosen();
  void slotFilterOperatingPoint();

private:
  QPointer<Schematic> a_problemsDoc;
  bool a_problemsHierarchy = false;
  // Kept as a QObject: ~QWidget emits destroyed() before QPointers let go,
  // and QPointer<Schematic>::data() would then cast the half-destroyed
  // widget back to a Schematic (UBSan: downcast of a QWidget).
  // operatingPointDocument() uses qobject_cast, null for such a widget.
  QPointer<QObject> a_operatingPointDoc;
  QMetaObject::Connection a_operatingPointGone;
  int a_operatingPointTab = -1;
  QList<qucs_s::erc::Issue> a_issues;
  QPointer<QObject> a_problemsText;   // textProblemsDocument()
  QMetaObject::Connection a_problemsTextGone;
  QList<TextDoc::Diagnostic> a_textProblems;
  /// The Problems tab's title and icon for \a errors and \a count.
  void titleProblems(int errors, int count);

};

#endif // MESSAGEDOCK_H
