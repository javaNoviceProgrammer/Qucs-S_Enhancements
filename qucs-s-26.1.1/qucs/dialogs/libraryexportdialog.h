/***************************************************************************
                            libraryexportdialog.h
                           -----------------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef LIBRARYEXPORTDIALOG_H
#define LIBRARYEXPORTDIALOG_H

#include <QDialog>
#include <QWidget>

#include "librarysettings.h"

class Component;
class Schematic;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

/// The library settings of a subcircuit (librarysettings.h), as Document
/// Settings > Library and a Library Export's dialog both show them.
class LibraryOptions : public QWidget {
  Q_OBJECT
public:
  explicit LibraryOptions(QWidget* parent = nullptr);

  void setSettings(const LibrarySettings& s);
  LibrarySettings settings() const;
  /// The lines of the .model cards that are no card ("2: .control"), as
  /// Schematic::modelCardsOf() finds them: none when all are.
  QStringList rejectedCards() const;
  /// A line at the top saying where the settings are held; empty, none.
  void setHeldBy(const QString& text);
  /// As tall as its notes are, wrapped: in a tab (Document Settings) it was
  /// squeezed, their lines drawn over each other.
  QSize minimumSizeHint() const override;
  QSize sizeHint() const override;

  QCheckBox* alwaysLoadOSDI;
  QPlainTextEdit* modelCards;
  QCheckBox* alwaysModelCards;
  QComboBox* groundPin;   ///< Default, With, Without (LibrarySettings::GroundPin as its data)

private:
  QLabel* m_heldBy;
};

/// A Library Export's own dialog (double-click): its name, whether its
/// settings are shown on the schematic, and the settings.
class LibraryExportDialog : public QDialog {
  Q_OBJECT
public:
  LibraryExportDialog(Component* part, Schematic* doc);

  LibraryOptions* options() const { return m_options; }

private slots:
  void slotOK();

private:
  Component* m_part;
  Schematic* m_doc;
  QLineEdit* m_name;
  QCheckBox* m_shown;
  LibraryOptions* m_options;
};

#endif
