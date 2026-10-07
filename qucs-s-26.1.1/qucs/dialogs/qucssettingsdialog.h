/***************************************************************************
                           qucssettingsdialog.h
                          ----------------------
    begin                : Sun May 23 2004
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
    copyright            : (C) 2016 by Qucs Team (see AUTHORS file)
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QUCSSETTINGSDIALOG_H
#define QUCSSETTINGSDIALOG_H

#include "qucs.h"
#include "qucsshortcutdialog.h"

#include <QDialog>
#include <QFont>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

class QLineEdit;
class QCheckBox;
class QSpinBox;
class QVBoxLayout;
class QPushButton;
class QComboBox;
class QIntValidator;
class QRegExpValidator;
class QStandardItemModel;
class QTableWidget;
struct ContentCategory;
class SyntaxSettingsPage;

///
/// @class QucsSettingsDialog class
/// @brief Application settings dialog for Qucs-S.
/// Provides a tabbed interface for editing application-wide settings including appearance, source code editor colors,
/// file type associations, and library search paths. Changes can be applied immediately or on dialog acceptance.
///
class QucsSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    /// @brief Class constructor
    QucsSettingsDialog(QucsApp *parent=0);

    /// @brief Class destructor
    ~QucsSettingsDialog();

private slots:
    /// @brief Applies current settings and closes the dialog.
    void slotOK();

    /// @brief Applies current settings without closing the dialog.
    void slotApply();

    /// @brief Opens the font picker for the schematic font.
    void slotFontDialog();

    /// @brief Opens the font picker for the application font.
    void slotAppFontDialog();

    /// @brief Opens the font picker for the text document font.
    void slotTextFontDialog();

    /// @brief Opens the color picker for the document background color.
    void slotBGColorDialog();

    /// @brief Opens the color picker for the schematic grid color.
    void slotGridColorDialog();

    /// @brief Resets all settings fields to their default values.
    void slotDefaultValues();
    /// The Contents tab: every category's patterns back to its defaults.
    void slotRestoreContentPatterns();
    /// The Contents tab: a category of the user's own added (named "New
    /// Category", its name being edited), the one selected removed.
    void slotAddContentCategory();
    void slotRemoveContentCategory();

    /// @brief Adds or updates a file type entry in the file types table.
    void slotAddFileType();

    /// @brief Removes the selected file type entry from the file types table.
    void slotRemoveFileType();

    /// @brief Populates the suffix and program fields when a file type row is clicked.
    void slotTableClicked(int,int);

    /// @brief Opens a directory picker to set the Qucs home directory.
    void slotHomeDirBrowse();

    /// @brief Opens a directory picker to set the AdmsXml binary directory.
    void slotAdmsXmlDirBrowse();

    /// @brief Opens a directory picker to set the ASCO binary directory.
    void slotAscoDirBrowse();

    /// @brief Opens a file picker to set the Octave executable path.
    void slotOctaveDirBrowse();

    /// @brief Opens a file picker to set the OpenVAF executable path.
    void slotOpenVAFDirBrowse();

    /// @brief Opens a file picker to set the Qucs-RFLayout executable path.
    void slotRFLayoutDirBrowse();

    /// @brief Opens a file picker to set the Python interpreter of the Python shell dock.
    void slotPythonBrowse();

    /// @brief Opens a directory picker to add a single path to the search list.
    void slotAddPath();

    /// @brief Opens a directory picker and adds the selected directory and all
    /// its subdirectories to the subcircuit search path list.
    /// @see slotClearAllPaths(), makePathTable()
    void slotAddPathWithSubFolders();

    ///
    /// @brief Clears all entries from the subcircuit search path list.
    /// Prompts the user for confirmation before removing all paths from
    /// currentPaths and rebuilding the path table. This is useful when
    /// a large number of paths have been added unintentionally, e.g. via
    /// slotAddPathWithSubFolders()
    /// @see makePathTable(), ClearAllPathsButt
    ///
    void slotClearAllPaths();
    /// The same three for the library search paths: a folder, a folder and
    /// those in it, none.
    void slotAddLibraryPath();
    void slotAddLibraryPathWithSubFolders();
    void slotClearAllLibraryPaths();
    /// Forgets the workspace kept for the next start, at once.
    void slotForgetWorkspace();

public:

    /// @brief Pointer to the parent application instance.
    QucsApp *App;

    /// @brief Schematic font
    QFont Font;

    /// @brief Application-wide font
    QFont AppFont;

    /// @brief Text document font
    QFont TextFont;

    /// @brief Enables wiring when clicking an open node.
    QCheckBox *checkWiring;

    /// @brief Allows loading documents from newer versions of Qucs-S
    QCheckBox *checkLoadFromFutureVersions;

    /// @brief Enables flexible wire routing
    QCheckBox *allowFlexibleWires;
    QCheckBox *contentAutoRefresh;      ///< Content panel: look for new/removed files by itself.
    QSpinBox *contentRefreshSeconds;    ///< ...every this many seconds.

    /// @brief Folder icons on the Content panel's sub-tree folder rows.
    QCheckBox *contentFolderIcons;
    // The Contents tab: the patterns of each category of the Content panel,
    // in its order (ProjectView::Category).
    QList<QLineEdit*> contentPatternEdits;
    /// The user's categories: a row each, name and patterns.
    QTableWidget *contentUserCategories = nullptr;
    void fillUserCategories(const QList<ContentCategory>& categories);
    /// The selected category of the user's one place up (-1) or down (1).
    void moveContentCategory(int by);
    QCheckBox *showPinNames;
    QCheckBox *showPinDirections;
    QCheckBox *embedVerilogA;        ///< Create Library copies the .va and .osdi files the subcircuits use.
    QCheckBox *libraryGroundPin;     ///< Create Library gives each .SUBCKT a first pin, gnd.
    QCheckBox *writeDocSettings;     ///< Saving a text document writes its settings file (.cfg).
    QComboBox *wheelCombo;           ///< A mouse wheel zooms (true) or scrolls (false).

    /// @brief Enables anti-aliasing for diagram graphs.
    QCheckBox *checkAntiAliasing;

    /// @brief Enables anti-aliasing for text rendering.
    QCheckBox *checkTextAntiAliasing;

    /// @brief Shows full trace name prefixes on diagrams.
    QCheckBox *checkFullTraceNames;

    /// @brief Always prefixes the dataset with the simulation label.
    QCheckBox *alwaysPrefixDataset;

    /// @brief Selects the application language.
    QComboBox *LanguageCombo;

    /// @brief Selects the application Qt style.
    QComboBox *StyleCombo;

    /// @brief Selects the application's theme: the system's, dark or light.
    QComboBox *ThemeCombo;
    QCheckBox *paperFollowsTheme;
    QComboBox *gridModeCombo;
    QCheckBox *lockToolbarsCheck;
    QSpinBox *fileNameCapSpin;        ///< Characters of a file name shown in a tab, the Claude Code panel.

    /// @brief Opens the schematic font picker dialog.
    QPushButton *FontButton;

    /// @brief Opens the application font picker dialog.
    QPushButton *AppFontButton;

    /// @brief Opens the text document font picker dialog.
    QPushButton *TextFontButton;

    /// @brief Opens the background color picker dialog.
    QPushButton *BGColorButton;

    /// @brief Opens the grid color picker dialog.
    QPushButton *GridColorButton;

    QLineEdit *LargeFontSizeEdit;   ///< Large font size value.
    QLineEdit *undoNumEdit;         ///< Maximum number of undo operations.
    QLineEdit *editorEdit;          ///< Path or name of the external text editor.
    QLineEdit *Input_Suffix;        ///< File suffix field for file type registration.
    QLineEdit *Input_Program;       ///< Program field for file type registration.
    QLineEdit *homeEdit;            ///< The workspace folder (Qucs Home), on the Workspace tab.
    /// Application Settings > Workspace: what the next start brings back
    /// (QucsSettings.Restore...; the Claude Code dock's conversations).
    QCheckBox *restoreWorkspace;
    QCheckBox *restoreProject;
    QCheckBox *restoreDocuments;
    QCheckBox *restorePanels;
    QCheckBox *restoreWindowGeometry;
    QCheckBox *reopenConversations;
    QLabel *keptWorkspaceLabel;          ///< What is kept now, in words.
    QPushButton *forgetWorkspaceButton;
    /// A project is any folder, not only one named NAME_prj
    /// (QucsSettings.AnyFolderIsProject).
    QCheckBox *anyFolderIsProject;
    QLineEdit *admsXmlEdit;         ///< AdmsXml binary directory path.
    QLineEdit *ascoEdit;            ///< ASCO binary directory path.
    QLineEdit *octaveEdit;          ///< Octave executable path.
    QLineEdit *OpenVAFEdit;         ///< OpenVAF executable path.
    QLineEdit *RFLayoutEdit;        ///< Qucs-RFLayout executable path.
    QLineEdit *pythonEdit;          ///< Python interpreter of the Python shell dock (empty: python3 on PATH).
    QLineEdit *graphLineWidthEdit;  ///< Default graph line thickness.

    /// @brief Table displaying registered file type suffix/program pairs.
    QTableWidget *fileTypesTableWidget;

    /// @brief Table displaying the subcircuit search path list.
    QTableWidget *pathsTableWidget;
    /// The library search paths: a folder a row.
    QTableWidget *libraryPathsTableWidget;
    QStandardItemModel *model;

    /// @brief The Source Code Editor tab: how each language is highlighted.
    SyntaxSettingsPage *syntaxPage;

    /// @brief Opens the custom shortcut configuration dialog.
    QPushButton *ShortcutButton;

    /// @brief Top-level vertical layout of the dialog
    QVBoxLayout *all;

    /// @brief Validator for integer fields with range 1–50.
    QIntValidator *val50;

    /// @brief Validator for integer fields with range 0–200.
    QIntValidator *val200;

    /// @brief Regular expression used for suffix validation.
    QRegularExpression Expr;

    /// @brief Validator based on Expr
    QRegularExpressionValidator *Validator;

public:
    /// A list of search paths given whole, as Claude's set_settings gives
    /// it: \a table names the table (subcircuitPaths, libraryPaths).
    Q_INVOKABLE void setPathList(const QString &table, const QStringList &paths);

private:
    QStringList currentPaths;
    QStringList currentLibraryPaths;


private:
    /// @brief Reconstructs the subcircuit search path table from @c currentPaths.
    void makePathTable();
    /// A table of search paths, its rows \a paths, each with a button
    /// that removes it.
    void makePathTable(QTableWidget *table, QStringList *paths);
    /// A search path table under \a header, named \a name for Claude.
    QTableWidget *newPathTable(QWidget *parent, const QString &header, const QString &name, const QString &accessible);
    /// Folders chosen: one, or (\a subfolders) one and those in it that
    /// the user keeps ticked; none when cancelled.
    QStringList chooseFolders(bool subfolders);
    /// The user agrees to remove all \a count paths.
    bool confirmClearAll(int count);
    /// What is kept for the next start, in words, and the Forget It button
    /// enabled when anything is.
    void showKeptWorkspace();

};

#endif
