/***************************************************************************
                                   qucs.h
                                  --------
    begin                : Thu Aug 28 2003
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QUCS_H
#define QUCS_H

#include "dialogs/qucsshortcutdialog.h"
#include "qucsshortcutmanager.h"
#include <QFileSystemModel>
#include <QHash>
#include <QMainWindow>
#include <QProcess>
#include <QSortFilterProxyModel>
#include <QStack>
#include <QString>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

class QucsDoc;
class Schematic;
class TextDoc;
class SimMessage;
class MouseActions;
class SearchDialog;
class OctaveWindow;
class MessageDock;
class ProjectView;
class ContextMenuTabWidget;
class TunerDialog;
class tunerElement;
class SimulationRun;
class SimulationConsole;
class ProcessConsole;

class QLabel;
class QAction;
class QLineEdit;
class QComboBox;
class QTabWidget;
class QSplitter;
class PaneWidget;
class FindBar;
class FindReplaceDialog;
class QDir;
class QMouseEvent;
class QCloseEvent;
class QMenu;
class QToolBar;
class QSettings;
class QListWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QShortcut;
class QListView;
class QModelIndex;
class QPushButton;
class QTextEdit;
class QFrame;
class QTimer;
namespace qucs_s { namespace autosave { struct Entry; } }

class SymbolWidget;

typedef bool (Schematic::*pToggleFunc)();
typedef void (MouseActions::*pMouseFunc)(Schematic *, QMouseEvent *);
typedef void (MouseActions::*pMouseFunc2)(Schematic *, QMouseEvent *, float,
                                          float);

class QucsFileSystemModel : public QFileSystemModel {
  Q_OBJECT
public:
  explicit QucsFileSystemModel(QObject *parent = nullptr)
      : QFileSystemModel(parent){};
  QVariant data(const QModelIndex &index, int role) const override;
};

class QucsSortFilterProxyModel : public QSortFilterProxyModel {
  Q_OBJECT

public:
  explicit QucsSortFilterProxyModel(QObject *parent = nullptr)
      : QSortFilterProxyModel(parent){};

protected:
  bool lessThan(const QModelIndex &left,
                const QModelIndex &right) const override;
};

class QucsApp : public QMainWindow {
  Q_OBJECT
public:
  QucsApp(bool netlist2Console);
  ~QucsApp();
  bool eventFilter(QObject *watched, QEvent *event) override;
  bool closeTabsRange(int startTab, int stopTab, int exceptTab = -1);
  bool closeAllFiles(int exceptTab = -1);
  bool closeAllLeft(int);
  bool closeAllRight(int);
  /// Loads a document into a tab. With \a checkDataNames the user is asked to
  /// rename dataset/display files that do not match the schematic's name.
  bool gotoPage(const QString &, bool reloadPage = false, bool checkDataNames = true);
  /// The document in tab \a No of the active pane; the current one for
  /// No < 0.
  QucsDoc *getDoc(int No = -1);
  /// The open document with this file name, in any pane; \a Pos gets its
  /// tab index within its pane (paneOf() names the pane).
  QucsDoc *findDoc(QString, int *Pos = 0);
  /// The open text document with this file name, or nullptr if it is not
  /// open or the open document with that name is a schematic.
  TextDoc *findTextDoc(const QString &fileName);
  /// The schematic shown in the current tab, or nullptr when the tab holds
  /// a text document (or there is no tab). Use this instead of casting
  /// DocumentTab->currentWidget().
  Schematic *currentSchematic() const;

  // --- Editor panes. Documents open in tab widgets ("panes") laid out in
  // up to a 2x2 grid; DocumentTab is the active pane, where documents
  // open and the actions apply. A pane is a ContextMenuTabWidget inside a
  // PaneWidget (which draws the active-pane marker) inside a row
  // QSplitter inside the vertical splitter that is the central widget.
  ContextMenuTabWidget *activePane() const { return DocumentTab; }
  /// The panes, row by row, left to right.
  QList<ContextMenuTabWidget *> panes() const;
  /// The pane a document widget is in, or nullptr.
  ContextMenuTabWidget *paneOf(QWidget *document) const;
  /// Makes a pane the active one (the UI follows its current document).
  void setActivePane(ContextMenuTabWidget *pane);
  /// Makes the pane holding this widget the active one.
  void activatePaneOf(QWidget *widget);
  /// Puts an open document in front: its pane active, its tab current.
  void showDocument(QWidget *document);
  /// Gives every open schematic the paper of the settings and the theme
  /// (misc::paperColor()), and the inline text editor with it.
  void applyPaper();
  /// Draws every open schematic again with the grid the settings say
  /// (QucsSettings.GridMode), and brings View > Show Grid in line.
  void applyGridSetting();
  /// View > Show Grid: for the current document, or - when the settings
  /// show or hide the grid everywhere - for all schematics.
  void updateGridAction();
  /// The find bar under a pane's documents.
  FindBar *findBarOf(ContextMenuTabWidget *pane) const;
  /// All open documents, pane by pane, in tab order.
  QList<QucsDoc *> allDocuments() const;
  /// The widget a document is shown in (a Schematic or a TextDoc).
  static QWidget *documentWidget(QucsDoc *doc);
  bool canSplitRight() const;
  bool canSplitDown() const;
  bool canClosePane() const { return panes().size() > 1; }
  /// Moves a document to another pane, keeping its tab title and marker.
  void moveDocument(QWidget *document, ContextMenuTabWidget *to);

  ProjectView *projectView() const { return Content; }
  MessageDock *messages() const { return messageDock; }
  SimulationConsole *simulationConsole() const { return simConsole; }
  /// The Terminal dock's shell and the Python Shell dock's interpreter.
  ProcessConsole *terminalConsole() const { return terminal; }
  ProcessConsole *pythonConsole() const { return pythonShell; }
  QDockWidget *terminalDockWidget() const { return terminalDock; }
  QDockWidget *pythonDockWidget() const { return pythonDock; }
  /// The shell the Terminal dock runs: $SHELL (or /bin/sh) as a login
  /// shell on Unix, PowerShell on Windows.
  static QString shellProgram();
  static QStringList shellArguments();
  /// The interpreter the Python Shell dock runs: the one set under
  /// Application Settings, Locations, else python3 (or python) on PATH.
  static QString pythonProgram();
  /// Points the docks at the programs the settings name now (they take
  /// effect at the next Restart).
  void updateConsolePrograms();
  /// The program name that, registered for a suffix under Application
  /// Settings, File Types, opens the file in Qucs' own text editor.
  static constexpr const char *QucsEditorProgram = "qucs-editor";
  /// The suffixes Qucs opens in its own text editor by default (its own
  /// document types: HDL, Verilog-A, Octave, netlists, SPICE).
  static const QStringList &textDocumentSuffixes();
  /// Other text formats, opened with the text editor from the settings
  /// (the built-in one unless an external editor is configured).
  static const QStringList &textFileSuffixes();
  /// The program registered for a suffix under File Types, or empty.
  QString userProgramFor(const QString &suffix) const;
  /// Opens the project directory (".../name_prj"): closes the open
  /// documents, points the work directory, the Content panel and the
  /// Scratch folder at it.
  void openProject(const QString &);
  /// Opens a file the way a double-click in the Content panel does: by its
  /// suffix, in the schematic view, the text editor, a registered program
  /// or the system's application. \a note is the panel's note column
  /// ("2-port" for a subcircuit schematic).
  void openFileFromProjectView(const QFileInfo &Info, const QString &note);
  /// Opens files dropped on the document area (from the Content panel or a
  /// file manager), each in its viewer: schematics, data displays and
  /// symbols in the schematic view, Qucs text documents and any other text
  /// file in the text editor, the rest as a double-click in the Content
  /// panel would. Deferred to the event loop, so the tab the drop landed on
  /// may be closed by it (an untitled, unchanged document).
  /// Opens files dropped on \a target (a document or a pane's tabs) - in
  /// the pane the target is in; the active pane when there is no target.
  void openDroppedFiles(const QStringList &files, QWidget *target = nullptr);
  void openDroppedFile(const QString &file);
  /// Opens what the system hands over - the command line, the Finder or
  /// the Dock (QFileOpenEvent), a desktop's file manager: paths or file:
  /// URLs of documents, and of a project directory (its name ends in
  /// "_prj"), which opens as the project first. Brings the window to the
  /// front. Returns how many documents and projects were opened.
  int openFromSystem(const QStringList &items);
  QString fileType(const QString &);
  static bool isTextDocument(QWidget *);

  QString ProjName; // name of the project, that is open
  /// The Scratch folder of the schematic in front - or, with a text
  /// document (a netlist, say) in front, of the last simulated schematic.
  QString currentScratchDir() const;
  // QHash<QString,QString> schNameHash; // QHash for the schematic files lookup
  // QHash<QString,QString> spiceNameHash; // QHash for the spice files lookup

  QLineEdit *editText;     // for edit component properties on schematic
  SearchDialog *SearchDia; // global in order to keep values
  /// Edit > Replace for schematics, made when first asked for.
  FindReplaceDialog *findReplaceDialog() const { return a_findReplace; }
  TunerDialog *tunerDia;   // global in order to keep values
  SimMessage *sim;         // global in order to keep values

  // current mouse methods
  void (MouseActions::*MouseMoveAction)(Schematic *, QMouseEvent *);
  void (MouseActions::*MousePressAction)(Schematic *, QMouseEvent *, float,
                                         float);
  void (MouseActions::*MouseDoubleClickAction)(Schematic *, QMouseEvent *);
  void (MouseActions::*MouseReleaseAction)(Schematic *, QMouseEvent *);

  /**
   * @brief Registers the default keyboard shortcuts
   */
  void setDefaultShortcut();

protected:
  void closeEvent(QCloseEvent *);

public slots:
  void slotFileNew();         // generate a new schematic in the view TabBar
  void slotTextNew();         // generate a new text editor in the view TabBar
  void slotSymbolNew();       // create new symbol
  void slotFileOpen();        // open a document
  void slotFileSave();        // save a document
  void slotFileSaveAs();      // save a document under a different filename
  void slotFileSaveAll();     // save all open documents
  void slotFileClose();       // close the actual file
  void slotFileCloseOthers(); // close all documents except the current one
  void
  slotFileCloseAllLeft(); // close all documents to the left of the current one
  void slotFileCloseAllRight(); // close all documents to the right of the
                                // current one
  void slotFileCloseAll();      //  close all documents
  // Panes (View > Panes, and the tab context menu)
  void slotSplitPaneRight();    //  a new pane to the right of the active one
  void slotSplitPaneDown();     //  a new row of panes below
  void slotClosePane();         //  the active pane's documents go to a neighbour
  void slotMoveDocumentToNextPane();   // the current document, to the next pane (splitting first if there is one pane)
  void slotNextPane();          //  the next pane becomes active
  void slotFileExamples();      // show the examples in a file browser
  void slotHelpTutorial();      // Open a pdf tutorial
  void slotHelpReport();        // Open a pdf report
  void slotHelpTechnical();     // Open a pdf technical document
  void slotFileClose(int);      // close the file with given index
  Schematic* symbolDocument();  // the schematic the symbol commands work on
  void slotSymbolEdit();        // edit the symbol for the schematic
  void slotSymbolRecreate();    // draw the symbol for the schematic anew
  void slotSymbolPinOrder();    // set the order of the subcircuit's pins
  void slotSymbolSaveAs();      // write the symbol into a .sym file
  void slotSymbolLoad();        // take the symbol from a .sym file
  void slotFileSettings();      // open dialog to change file settings
  void slotFilePrint();         // print the current file
  void slotFilePrintFit();      // Print and fit to page
  void slotFileQuit();          // exits the application
  void slotApplSettings();      // open dialog to change application settings
  void slotRefreshSchPath();    // refresh the schematic path hash

  void slotIntoHierarchy();
  void slotPopHierarchy();

  void slotShowAll();
  void slotZoomToSelection();
  void slotShowOne();
  void slotZoomOut(); // Zoom out by 2

  void slotToPage();
  void slotSelectComponent(QListWidgetItem *);
  void slotSearchComponent(const QString &);
  void slotSearchClear();

  void slotEditElement();
  void slotPowerMatching();
  void slot2PortMatching();

  // for menu that appears by right click in content ListView
  void slotShowContentMenu(const QPoint &);

  void slotCMenuBuildAllVerilogA();
  void slotCMenuContentView(QAction *mode);
  void slotVerilogABuildOutput();
  void slotVerilogABuildFinished(int exitCode, QProcess::ExitStatus status);
  void slotVerilogABuildError(QProcess::ProcessError error);

  void slotCMenuOpen();
  void slotCMenuCopy();
  void slotCMenuRename();
  void slotCMenuDelete();
  void slotCMenuInsert();

  void slotUpdateTreeview();

  void slotMenuProjClose();

  void slotSimulate(QWidget *w = nullptr);
  void slotSimulateWithSpice();
  void slotTune(bool checked);
  /// Every element of \a doc was destroyed and recreated (undo, redo,
  /// reload): drop cached pointers and abandon any drag in progress.
  void slotDocumentRebuilt(Schematic *doc);

  /// Writes an autosave copy of every modified document. Returns how many
  /// were written. With \a emergency set it is running inside the crash
  /// handler and must not touch the UI.
  int autosaveAll(bool emergency = false);
  /// Offers to restore documents left behind by a previous session
  /// (autosave copies), and reports a crash of that session if there was one.
  void recoverPreviousSession(bool crashedLastTime);

private slots:
  void slotMenuProjOpen();
  void slotMenuProjDel();
  void slotListProjOpen(const QModelIndex &);
  void slotSelectSubcircuit(const QModelIndex &);
  void slotSelectLibComponent(QTreeWidgetItem *);
  void slotOpenContent(const QModelIndex &);
  void slotSetCompView(int);
  void slotChangeSimulator(int);
  void slotButtonProjNew();
  void slotButtonProjOpen();
  void slotButtonProjDel();
  void slotChangeView();
  void slotAfterSimulation(int, SimMessage *);
  void slotDCbias();
  void slotChangePage(const QString &, const QString &);
  void slotHideEdit();
  void slotFileChanged(bool);
  void slotSimSettings();
  void slotSaveNetlist();
  /// Simulation > Generate Netlist: writes the schematic's netlist into
  /// its Scratch folder (spice4qucs.cir, as a run would) and opens it.
  void slotGenerateNetlist();
  void slotSaveCdlNetlist();
  void slotCdlSettings();
  void slotAfterSpiceSimulation(SimulationRun *run);
  void slotBuildVAModule();
  /*void slotBuildXSPICEIfs(int mode = 0);
  void slotEDDtoIFS();
  void slotEDDtoMOD();*/
  void slotShowModel();
  void slotSearchLibComponent(const QString &);
  void slotSearchLibClear();

  // Tab navigation
  ///
  /// \brief Go to the first tab
  ///
  void slotFirstTab();

  ///
  /// \brief Go to the next tab
  ///
  void slotNextTab();

  ///
  /// \brief Go to the previous tab
  ///
  void slotPreviousTab();

  ///
  /// \brief Go to the last tab
  ///
  void slotLastTab();

  void slotSwitchToTab(int index);
  void slotAutosave();

signals:
  void signalKillEmAll();

public:
  MouseActions *view;
  ContextMenuTabWidget *DocumentTab;   // the active pane
  QSplitter *a_paneArea = nullptr;     // rows of panes (the central widget)
  QListWidget *CompComps;
  QTreeWidget *libTreeWidget;
  QTextEdit *CompDescr;
  QLineEdit *LibCompSearch;
  SymbolWidget *Symbol;
  QPushButton *btnShowModel;

  // menu appearing by right mouse button click on a file in the content
  // listview, ...
  QMenu *ContentMenu;
  // ...on its "Verilog-A" category row, ...
  QMenu *ContentVerilogAMenu;
  // ...and on its empty area (the panel's own settings)
  QMenu *ContentPanelMenu;
  // its "Toggle hierarchy search view" sub-menu
  QMenu *ContentViewMenu;

  // corresponding actions
  QAction *ActionCMenuOpen, *ActionCMenuCopy, *ActionCMenuRename,
      *ActionCMenuDelete, *ActionCMenuInsert, *ActionCMenuBuildAllVerilogA,
      *ActionCMenuViewFlat, *ActionCMenuViewTree, *ActionCMenuRefresh;

  // "Build All" for Verilog-A: the files still to compile with OpenVAF,
  // the running compiler, and the tally for the summary line.
  QStringList a_vaBuildQueue;
  QProcess *a_vaBuilder = nullptr;
  int a_vaBuildTotal = 0;
  int a_vaBuildFailed = 0;
  void startNextVerilogABuild();

  QAction *fileNew, *textNew, *symNew, *fileNewDpl, *fileOpen, *fileSave,
      *fileSaveAs, *fileSaveAll, *fileClose, *fileCloseOthers,
      *fileCloseAllLeft, *fileCloseAllRight, *fileCloseAll, *fileExamples,
      *fileSettings, *filePrint, *fileQuit, *projNew, *projOpen, *projDel,
      *projClose, *applSettings, *refreshSchPath, *editCut, *editCopy, *magAll,
      *magSel, *magOne, *magMinus, *filePrintFit, *tune, *symEdit, *intoH,
      *popH, *simulate, *save_netlist, *generateNetlist, *dpl_sch, *undo, *redo, *dcbias,
      *saveCdlNetlist, *cdlSettings,
      *symRecreate, *symPinOrder, *symSaveAs, *symLoad;

  // Navigate tabs
  QAction *TabFirstAction;    /// Action for raising the first document tab
  QAction *TabLastAction;     /// Action for raising the last document tab
  QAction *TabNextAction;     /// Action for raising the next document tab
  QAction *TabPreviousAction; /// Action for raising the previous document tab

  QAction *exportAsImage;

  QAction *activeAction; // pointer to the action selected by the user
  bool TuningMode;
  QString windowTitle;

private:
  // ********* Widgets on the main area **********************************
  QDockWidget *dock;
  QTabWidget *TabView;
  QDockWidget *octDock;
  OctaveWindow *octave;
  MessageDock *messageDock;
  // the simulation console dock (the external simulators' output)
  SimulationConsole *simConsole;
  QDockWidget *terminalDock;
  ProcessConsole *terminal;
  QDockWidget *pythonDock;
  ProcessConsole *pythonShell;

  QListView *Projects;
  ProjectView *Content;

  QLineEdit *CompSearch;
  QPushButton *CompSearchClear;
  QComboBox *CompChoose;

  // ********** Properties ************************************************
  QStack<QString> HierarchyHistory; // keeps track of "go into subcircuit"
  QString QucsFileFilter;
  QFileSystemModel *a_homeDirModel;
  QucsSortFilterProxyModel *a_proxyModel;
  int ccCurIdx; // CompChooser current index (used during search)
  bool a_netlist2Console;

  // ********** Methods ***************************************************
  void initView();
  void initCursorMenu();

  void initPaneArea();   // the central widget: one pane to begin with
  void startPaneWithUntitled(ContextMenuTabWidget *pane);
  void dropPlaceholder(ContextMenuTabWidget *pane, QWidget *keep);
  ContextMenuTabWidget *createPane();
  void removePane(ContextMenuTabWidget *pane);   // an empty pane; a neighbour becomes active
  QSplitter *rowOf(ContextMenuTabWidget *pane) const;
  PaneWidget *frameOf(ContextMenuTabWidget *pane) const;
  FindReplaceDialog *a_findReplace = nullptr;
  void updatePaneActions();
  void slotFocusChanged(QWidget *old, QWidget *now);
  int addDocumentTab(QFrame *widget, const QString &title = QString());
  int addDocumentTabTo(ContextMenuTabWidget *pane, QFrame *widget, const QString &title = QString());
  void setDocumentTabChanged(int index, bool changed);
  /// The modified marker of a document's tab, in whichever pane it is.
  void setDocumentChanged(QWidget *document, bool changed);
  void printCurrentDocument(bool);
  bool saveFile(QucsDoc *Doc = 0);
  bool saveAs();
  bool deleteProject(const QString &);
  void updatePortNumber(QucsDoc *, int);
  int fillComboBox(bool);
  void fillSimulatorsComboBox();
  void switchSchematicDoc(bool);
  void switchEditMode(bool);
  void changeSchematicSymbolMode(Schematic *);
  static bool recurRemove(const QString &);
  void closeFile(int);

  void updateRecentFilesList(QString s);
  void updateRecentProjectsList(QString pathToProj);
  void updateRecentProjectsList();
  void successExportMessages(bool ok);
  void fillLibrariesTreeView(void);
  bool populateLibTreeFromDir(const QString &LibDirPath,
                              QList<QTreeWidgetItem *> &topitems,
                              bool relpath = false);
  void saveSettings();
  QWidget *getSchematicWidget(QucsDoc *Doc);

public:
  void readProjects();
  void
  updatePathList(void); // update the list of paths, pruning non-existing paths
  void updatePathList(QStringList);
  // void updateSchNameHash(void); // maps all schematic files in the path list
  // void updateSpiceNameHash(void); // maps all spice files in the path list

  /* **************************************************
   *****  The following methods are located in  *****
   *****  "qucs_init.cpp".                      *****
   ************************************************** */

public slots:
  void slotShowWarnings();
  void slotResetWarnings();
  void printCursorPosition(int, int, QString);
  void slotUpdateUndo(bool); // update undo available state
  void slotUpdateRedo(bool); // update redo available state

private slots:
  void slotViewBrowseDock(bool toggle); // toggle the dock window
  void slotViewOctaveDock(bool);        // toggle the dock window
  void slotToggleOctave(bool);
  void slotToggleDock(bool);
  void slotHelpAbout(); // shows an about dialog

  ///
  /// @brief Opens the shortcut editor dialog
  ///
  void slotShortcutDialog();

private:
  void initActions();   // initializes all QActions of the application
  void initMenuBar();   // creates the menu_bar and inserts the menuitems
  void initToolBar();   // creates the toolbars
  void initStatusBar(); // setup the statusbar

  void openTextOrSchematicTab(const QString &absolutePath);
  void launchUserProgram(const QString &program, const QString &absolutePath);
  void useProjectScratch(bool on);

  QAction *helpAboutApp, *helpAboutQt, *viewBrowseDock, *viewOctaveDock;
  QAction *splitPaneRight = nullptr, *splitPaneDown = nullptr, *closePaneAction = nullptr,
          *moveDocumentToNextPane = nullptr, *nextPaneAction = nullptr;

  // menus contain the items of their menubar
  enum { MaxRecentFiles = 8, MaxRecentProjects = 8 };
  QMenu *fileMenu, *editMenu, *insMenu, *projMenu, *recentProjMenu, *simMenu,
      *viewMenu, *helpMenu, *alignMenu, *toolMenu, *recentFilesMenu, *cmMenu,
      *symbolMenu;
  QAction *fileRecentAction[MaxRecentFiles];
  QAction *fileClearRecent;

  QAction *projRecentActions[MaxRecentProjects];
  QAction *projClearRecent;

  // submenus for the PDF documents
  QMenu *helpTechnical, *helpReport, *helpTutorial;

  QComboBox *simulatorsCombobox;
  QToolBar *fileToolbar, *editToolbar, *viewToolbar, *workToolbar,
      *simulateToolbar, *hierarchyToolbar;

  // Shortcuts for scrolling schematic / TextEdit
  // This is rather cumbersome -> Make this with a QScrollView instead??
  QShortcut *cursorUp, *cursorLeft, *cursorRight, *cursorDown;

  QLabel *WarningLabel, *PositionLabel,
      *DiagramValuesLabel; // labels in status bar
  // QLabel *SimulatorLabel;

  /* **************************************************
   *****  The following methods are located in  *****
   *****  "qucs_actions.cpp".                   *****
   ************************************************** */

public:
  void editFile(const QString &, bool reloadFile = false);

  QAction *insWire, *insLabel, *insGround, *insPort, *insEquation, *magPlus,
      *editRotate, *editMirror, *editMirrorY, *editPaste, *select, *editStretch,
      *editMove, *editActivate, *wire, *editDelete, *setMarker,
      *setDiagramLimits, *resetDiagramLimits, *showGrid, *onGrid, *moveText,
      *helpIndex, *helpGetStart, *callEditor, *callFilter, *callLine,
      *callActiveFilter, *showMsg, *showNet, *checkSchematicAction, *checkHierarchyAction, *alignTop, *alignBottom,
      *alignLeft, *alignRight, *distrHor, *distrVert, *selectAll, *callMatch,
      *changeProps, *addToProj, *editFind, *insEntity, *selectMarker,
      *createLib, *callConverter, *graph2csv, *callAtt, *centerHor, *centerVert,
      *loadModule, *buildModule, *callPwrComb, *callRFLayout, *callSPAR_Viewer,
      *callRxcalc;

  QAction *helpQucsIndex;
  QAction *simSettings;
  QAction *buildVAModule;
  QAction *ShortcutManagerAction; /// Action to open the keyboard shortcut
                                  /// manager dialog

public slots:
  void slotEditRotate(bool);  // rotate the selected items
  void slotEditMirrorX(bool); // mirror the selected items about X axis
  void slotEditMirrorY(bool); // mirror the selected items about Y axis
  void slotEditCut();         // put marked object into clipboard and delete it
  void slotEditCopy();        // put the marked object into the clipboard
  void slotEditPaste(bool);   // paste the clipboard into the document
  void slotEditDelete(bool);  // delete the selected items
  void slotInsertEquation(bool);
  void slotInsertGround(bool);
  void slotInsertPort(bool);
  void slotInsertEntity();
  void slotSetWire(bool);
  void slotEscape();
  void slotSelect(bool);
  void slotEditActivate(bool);
  void slotEditStretch(bool); // move selection of components w/wire stretching
  void slotEditMove(bool); // move selection of components and disconnect wires.
  void slotInsertLabel(bool);
  void slotSetMarker(bool);
  void slotSetDiagramLimits(bool);
  void slotResetDiagramLimits();
  void slotShowGrid();     // turn the grid on or off
  void slotOnGrid(bool);   // set selected elements on grid
  void slotMoveText(bool); // move property text of components
  void slotZoomIn(bool);
  void slotEditUndo();     // makes the last operation undone
  void slotEditRedo();     // makes the last undo undone
  void slotEditFind();     // searches for a piece of text
  void slotAlignTop();     // align selected elements with respect to top
  void slotAlignBottom();  // align selected elements with respect to bottom
  void slotAlignLeft();    // align selected elements with respect to left
  void slotAlignRight();   // align selected elements with respect to right
  void slotDistribHoriz(); // distribute horizontally selected elements
  void slotDistribVert();  // distribute vertically selected elements
  void slotCenterHorizontal();
  void slotCenterVertical();
  void slotSelectAll();
  void slotSelectMarker();
  void slotShowLastMsg();
  void slotShowLastNetlist();
  /// Simulation > Check Schematic: the electrical rule check of the
  /// schematic in front, listed on the Problems tab of the message dock.
  void slotCheckSchematic();
  /// The same for the schematic in front and every subcircuit it uses,
  /// at any depth (open documents as they are, the others from disk).
  void slotCheckHierarchy();
  /// A row of the Problems tab: selects the component (if one is meant)
  /// and centres the schematic on the place.
  void slotLocateProblem(int index);
  /// Selects and centres the component of that name in the schematic of
  /// the Operating Point tab.
  void slotShowOperatingPointComponent(const QString &component);
private:
  QString a_lastSimulatedDoc;   // for currentScratchDir()
  /// Runs the check on \a doc and shows the result; the dock comes up
  /// when there are errors (always when \a always). Returns the error count.
  int checkSchematic(Schematic* doc, bool always);
public slots:
  void slotCallEditor();
  void slotCallFilter();
  void slotCallActiveFilter();
  void slotCallLine();
  void slotCallMatch();
  void slotCallAtt();
  void slotCallPwrComb();
  void slotCallSPAR_Viewer();
  void slotCallRxCalc();
  void slotCallRFLayout();
  void slotHelpIndex(); // shows a HTML docu: Help Index
  void slotHelpQucsIndex();
  void slotGettingStarted(); // shows a HTML docu: Getting started
  void slotChangeProps();
  void slotAddToProject();
  void slotApplyCompText();
  void slotOpenRecentFile();
  void slotOpenRecentProject();
  void slotSaveDiagramToGraphicsFile();
  void slotSaveSchematicToGraphicsFile(bool diagram = false);

private slots:
  void slotCursorLeft(bool left = true);
  void slotCursorRight() { return slotCursorLeft(false); }
  void slotCursorUp(bool up = true);
  void slotCursorDown() { return slotCursorUp(false); }
  void slotResizePropEdit(const QString &);
  void slotCreateLib();
  void slotImportData();
  void slotExportGraphAsCsv();
  void slotUpdateRecentFiles();
  void slotClearRecentFiles();
  void slotUpdateRecentProjects();
  void slotClearRecentProjects();
  void slotLoadModule();
  void slotBuildModule();

private:
  void buildWithOpenVAF();
  bool performToggleAction(bool, QAction *, pToggleFunc, pMouseFunc,
                           pMouseFunc2);
  void launchTool(const QString &, const QString &,
                  const QStringList & = QStringList(),
                  bool qucs_tool = false); // tool, description and args
  friend class SaveDialog;

  /// @brief Scans the schematic for CMD components and executes their commands
  ///        after simulation completes.
  ///
  ///
  /// Multi-line commands are joined with @c && so that they run sequentially
  /// in the same shell session. Comment lines (starting with @c #) and blank
  /// lines are stripped before joining. The @c ~ home-directory shorthand is
  /// expanded before the command is passed to the shell.
  ///
  /// On Linux, different terminal emulators are tried:
  /// konsole, gnome-terminal, xfce4-terminal, lxterminal, xterm. Each is
  /// launched with a clean environment (without @c LD_LIBRARY_PATH and
  /// @c LD_PRELOAD) to avoid Qt library version conflicts between Qucs-S and
  /// the terminal emulator.
  ///
  /// @param sch  Pointer to the schematic to scan. If @c nullptr, returns
  ///             immediately without doing anything.
  ///
  /// @note This method is called from both @c slotAfterSimulation() and
  ///       @c slotAfterSpiceSimulation(), covering all simulation backends.
  void runPostSimCommands(Schematic* sch);

  QString lastExportFilename;
  QTimer *autosaveTimer = nullptr;

public:
  /// Opens the autosaved copies as their original documents, marked
  /// modified, without asking. recoverPreviousSession() asks first.
  void restoreAutosaved(const QList<qucs_s::autosave::Entry> &entries);
};

/** \brief Provide a template to declare singleton classes.
 *
 * Classes implemented using this template will be singletons (i.e., only
 * one instance of the class will exist per invokation). Primarily this
 * is used to support static / access to an application-wide function, e.g.
 * settings.
 *
 */
template <typename T> class QucsSingleton final {
public:
  static T &Get() {
    static T instance;
    return instance;
  }

  // Prevent overriding default ctor, dtor, copying, or multiple instances.
private:
  QucsSingleton() = default;
  ~QucsSingleton() = default;

  QucsSingleton(const QucsSingleton &) = delete;
  QucsSingleton &operator=(const QucsSingleton &) = delete;
  QucsSingleton(QucsSingleton &&) = delete;
  QucsSingleton &operator=(QucsSingleton &&) = delete;
};

class ContextMenuTabWidget : public QTabWidget {
  Q_OBJECT
public:
  ContextMenuTabWidget(QucsApp *parent = 0);
public slots:
  void showContextMenu(const QPoint &point);

protected:
  // Files dropped on the tab bar, or on the empty area when no document
  // is open, are opened; the documents handle drops on themselves.
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  int contextTabIndex; // index of tab where context menu was opened
  QucsApp *App;        // the main application - parent widget
private slots:
  void slotCxMenuClose();
  void slotCxMenuCloseOthers();
  void slotCxMenuCloseAll();
  void slotCxMenuCloseRight();
  void slotCxMenuCloseLeft();
  void slotCxMenuCopyPath();
  void slotCxMenuOpenFolder();
  void slotCxMenuMoveToNextPane();
};

#endif /* QUCS_H */
