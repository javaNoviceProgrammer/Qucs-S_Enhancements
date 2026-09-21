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

class QDockWidget;
class QTabWidget;
class QPlainTextEdit;

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
  /// so), and brings the dock up when asked to.
  void showProblems(Schematic* doc, const QList<qucs_s::erc::Issue>& issues, bool raise);
  Schematic* problemsDocument() const;
  const QList<qucs_s::erc::Issue>& issues() const { return a_issues; }

  /*!
   * \brief admsOutput holds the make output of running admsXml
   */
  QPlainTextEdit *admsOutput;
  /*!
   * \brief cppOutput holds the make output of running a C++ compiler
   */
  QPlainTextEdit *cppOutput;

  void reset();

signals:
  /// A row of the Problems tab was chosen: issues()[index] is to be shown.
  void locateRequested(int index);


private slots:
  void slotAdmsChanged();
  void slotCppChanged();
  void slotCursor();
  void slotProblemChosen();

private:
  QPointer<Schematic> a_problemsDoc;
  QList<qucs_s::erc::Issue> a_issues;

};

#endif // MESSAGEDOCK_H
