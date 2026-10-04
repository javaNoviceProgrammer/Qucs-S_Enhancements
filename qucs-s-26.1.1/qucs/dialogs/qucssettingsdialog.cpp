/***************************************************************************
                           qucssettingsdialog.cpp
                          ------------------------
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

/*!
 * \file qucssettingsdialog.cpp
 * \brief Implementation of the Application Settings dialog
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "qucssettingsdialog.h"
#include <iostream>
#include <cmath>
#include <QGridLayout>
#include <QVBoxLayout>
#include "main.h"
#include "textdoc.h"
#include "schematic.h"
#include "settings.h"
#include "misc.h"
#include "apptheme.h"
#include "projectView.h"
#include "syntax.h"
#include "syntaxsettings.h"
#include "claudehistory.h"
#include "workspacesession.h"

#include <QWidget>
#include <QLabel>
#include <QTabWidget>
#include <QScrollArea>
#include <QLayout>
#include <QColorDialog>
#include <QFontDialog>
#include <QValidator>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QMessageBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QGroupBox>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QFileDialog>
#include <QDirIterator>
#include <QDebug>
#include <QObject>
#include <QString>
#include <QList>

using namespace std;

auto getFontDescription = [](const auto& Font) -> QString {
    const QChar comma(u',');
    QString fontDescription = Font.family() + comma +
        QString::number(Font.pointSize());

    QString fontStyle = Font.styleName();
    if (!fontStyle.isEmpty())
        fontDescription += comma + fontStyle;

    return fontDescription;
};

QucsSettingsDialog::QucsSettingsDialog(QucsApp *parent)
    : QDialog(parent)
{
    App = parent;
    setWindowTitle(tr("Edit Qucs Properties"));

    Expr.setPattern("[\\w_]+");
    Validator  = new QRegularExpressionValidator(Expr, this);

    all = new QVBoxLayout(this); // to provide the necessary size
    QTabWidget *t = new QTabWidget();
    all->addWidget(t);

    // ...........................................................
    // The application settings tab
    QWidget *appSettingsTab = new QWidget(t);
    QGridLayout *appSettingsGrid = new QGridLayout(appSettingsTab);

    const QStringList appLanguages = {
        tr("system language"),
        tr("English") + " (en)",
        tr("Arabic") + " (ar)",
        tr("Catalan") + " (ca)",
        tr("Chinese") + " (zh_CN)",
        tr("Czech") + " (cs)",
        tr("French") + " (fr)",
        tr("German") + " (de)",
        tr("Hebrew") + " (he)",
        tr("Hungarian") + " (hu)",
        tr("Italian") + " (it)",
        tr("Japanese") + " (jp)",
        tr("Kazakh") + " (kk)",
        tr("Polish") + " (pl)",
        tr("Portuguese-BR") + " (pt_BR)",
        tr("Portuguese-PT") + " (pt_PT)",
        tr("Romanian") + " (ro)",
        tr("Russian") + " (ru)",
        tr("Spanish") + " (es)",
        tr("Swedish") + " (sv)",
        tr("Turkish") + " (tr)",
        tr("Ukrainian") + " (uk)"
    };

    appSettingsGrid->addWidget(new QLabel(tr("Language (set after reload):"), appSettingsTab) ,1, 0);
    LanguageCombo = new QComboBox(appSettingsTab);
    LanguageCombo->addItems(appLanguages);
    appSettingsGrid->addWidget(LanguageCombo, 1, 1);

    val200 = new QIntValidator(1, 200, this);   // an undo depth of 0 is not usable
    appSettingsGrid->addWidget(new QLabel(tr("Maximum undo operations:"), appSettingsTab) ,2, 0);
    undoNumEdit = new QLineEdit(appSettingsTab);
    undoNumEdit->setValidator(val200);
    appSettingsGrid->addWidget(undoNumEdit, 2, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Text editor:"), appSettingsTab), 3, 0);
    editorEdit = new QLineEdit(appSettingsTab);
    editorEdit->setToolTip(tr("Set to qucs, qucsedit or the path to your favorite text editor."));
    appSettingsGrid->addWidget(editorEdit, 3, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Start wiring when clicking open node:"), appSettingsTab), 4, 0);
    checkWiring = new QCheckBox(appSettingsTab);
    appSettingsGrid->addWidget(checkWiring, 4, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Load documents from future versions:")), 5, 0);
    checkLoadFromFutureVersions = new QCheckBox(appSettingsTab);
    checkLoadFromFutureVersions->setToolTip(tr("Try to load also documents created with newer versions of Qucs."));
    appSettingsGrid->addWidget(checkLoadFromFutureVersions, 5, 1);
    checkLoadFromFutureVersions->setChecked(QucsSettings.IgnoreFutureVersion);

    appSettingsGrid->addWidget(new QLabel(tr("Show trace name prefix on diagrams:")), 6, 0);
    checkFullTraceNames = new QCheckBox(appSettingsTab);
    checkFullTraceNames->setToolTip(tr("Show prefixes for trace names on diagrams like \"ngspice/\""));
    appSettingsGrid->addWidget(checkFullTraceNames, 6, 1);
    checkFullTraceNames->setChecked(QucsSettings.fullTraceName);

    appSettingsGrid->addWidget(new QLabel(tr("Always prefix the dataset with simulation label:")), 7, 0);
    alwaysPrefixDataset = new QCheckBox(appSettingsTab);
    alwaysPrefixDataset->setToolTip(tr("Always use the prefix for dataset, i.e. \"tr1.v(out)\" rather than \"v(out)\""));
    appSettingsGrid->addWidget(alwaysPrefixDataset, 7, 1);
    alwaysPrefixDataset->setChecked(QucsSettings.alwaysPrefixDataset);

    appSettingsGrid->addWidget(new QLabel(tr("Flexible wires (requires restart):"), appSettingsTab), 8, 0);
    allowFlexibleWires = new QCheckBox(appSettingsTab);
    appSettingsGrid->addWidget(allowFlexibleWires, 8, 1);

    t->addTab(appSettingsTab, tr("Settings"));

    appSettingsGrid->addWidget(new QLabel(tr("Set custom shortcut:"), appSettingsTab), 9, 0);
    ShortcutButton = new QPushButton(appSettingsTab);
    connect(ShortcutButton, SIGNAL(clicked()),
        parent, SLOT(slotShortcutDialog()));
    appSettingsGrid->addWidget(ShortcutButton, 9, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Refresh the Content panel automatically:"), appSettingsTab), 10, 0);
    contentAutoRefresh = new QCheckBox(appSettingsTab);
    contentAutoRefresh->setToolTip(tr("Every few seconds the project's files are listed again, and the "
                                      "Content panel is rebuilt when a file came, went or changed.\n"
                                      "Off: only Qucs' own actions and Refresh on the panel's menu list them again."));
    appSettingsGrid->addWidget(contentAutoRefresh, 10, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Content panel refresh interval (seconds):"), appSettingsTab), 11, 0);
    contentRefreshSeconds = new QSpinBox(appSettingsTab);
    contentRefreshSeconds->setRange(1, 3600);
    contentRefreshSeconds->setToolTip(tr("How often the project's files are looked at; a large project on a slow "
                                         "disk wants a longer interval."));
    appSettingsGrid->addWidget(contentRefreshSeconds, 11, 1);
    connect(contentAutoRefresh, &QCheckBox::toggled, contentRefreshSeconds, &QWidget::setEnabled);

    appSettingsGrid->addWidget(new QLabel(tr("Folder icons in the Content panel:"), appSettingsTab), 12, 0);
    contentFolderIcons = new QCheckBox(appSettingsTab);
    contentFolderIcons->setToolTip(tr("Show a folder icon on the folder rows of the Content panel's "
                                      "sub-trees (Toggle hierarchy search view). Off: plain rows."));
    appSettingsGrid->addWidget(contentFolderIcons, 12, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Pin names in subcircuit symbols:"), appSettingsTab), 13, 0);
    showPinNames = new QCheckBox(appSettingsTab);
    showPinNames->setToolTip(tr("Write the name of each pin inside the symbol of a subcircuit - the "
                                "name the netlist gives that pin.\n"
                                "Off: only what the symbol itself draws."));
    appSettingsGrid->addWidget(showPinNames, 13, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Pin directions in subcircuit symbols:"), appSettingsTab), 14, 0);
    showPinDirections = new QCheckBox(appSettingsTab);
    showPinDirections->setToolTip(tr("Mark which way each pin points, from the type of the port it stands "
                                     "for (in, out, inout).\n"
                                     "A symbol drawn anew then puts the inputs on the left and the outputs "
                                     "on the right."));
    appSettingsGrid->addWidget(showPinDirections, 14, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Embed Verilog-A files in exported libraries:"), appSettingsTab), 15, 0);
    embedVerilogA = new QCheckBox(appSettingsTab);
    embedVerilogA->setObjectName(QStringLiteral("embedVerilogA"));
    embedVerilogA->setToolTip(tr("Tools > Create Library copies the Verilog-A sources (.va) its subcircuits "
                                 "use, and the files they include, into the library's folder. Where the "
                                 "library is used, OpenVAF compiles them before the first simulation - beside "
                                 "them, or into Qucs-S's cache when the library's folder cannot be written: a "
                                 "compiled model (.osdi) runs on one platform only, so it is not embedded.\n"
                                 "Off: the library holds the subcircuits and their symbols only."));
    appSettingsGrid->addWidget(embedVerilogA, 15, 1);

    appSettingsGrid->addWidget(new QLabel(tr("Write a settings file (.cfg) beside each text document:"),
                                          appSettingsTab), 16, 0);
    writeDocSettings = new QCheckBox(appSettingsTab);
    writeDocSettings->setObjectName(QStringLiteral("writeDocSettings"));
    writeDocSettings->setToolTip(tr("Saving a text document also writes name.cfg beside it (notes.txt.cfg for "
                                    "notes.txt) with its File > Document Settings: for VHDL and Verilog the "
                                    "simulation duration, module and libraries, for Verilog-A the symbol's "
                                    "icon, descriptions and device type. Other text files have no use for it.\n"
                                    "Off: a settings file is written only for a document whose Document "
                                    "Settings were set or changed, so none is lost. The files already there "
                                    "are left alone."));
    appSettingsGrid->addWidget(writeDocSettings, 16, 1);

    // ...........................................................
    // The appearance settings tab
    QWidget *appAppearanceTab = new QWidget(t);
    QGridLayout *appAppearanceGrid = new QGridLayout(appAppearanceTab);

    appAppearanceGrid->addWidget(new QLabel(tr("Schematic font (set after reload):"), appSettingsTab), 0, 0);
    FontButton = new QPushButton(appSettingsTab);
    connect(FontButton, SIGNAL(clicked()), SLOT(slotFontDialog()));
    appAppearanceGrid->addWidget(FontButton, 0, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Application font (set after reload):"), appSettingsTab), 1, 0);
    AppFontButton = new QPushButton(appSettingsTab);
    connect(AppFontButton, SIGNAL(clicked()), SLOT(slotAppFontDialog()));
    appAppearanceGrid->addWidget(AppFontButton, 1, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Text document font (set after reload):"), appSettingsTab), 2, 0);
    TextFontButton = new QPushButton(appSettingsTab);
    connect(TextFontButton, SIGNAL(clicked()), SLOT(slotTextFontDialog()));
    appAppearanceGrid->addWidget(TextFontButton, 2, 1);

    val50 = new QIntValidator(1, 50, this);
    appAppearanceGrid->addWidget(new QLabel(tr("Large font size:"), appSettingsTab), 3, 0);
    LargeFontSizeEdit = new QLineEdit(appSettingsTab);
    LargeFontSizeEdit->setValidator(val50);
    appAppearanceGrid->addWidget(LargeFontSizeEdit, 3, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Document Background Color:"), appSettingsTab) ,4, 0);
    BGColorButton = new QPushButton("      ", appSettingsTab);
    connect(BGColorButton, SIGNAL(clicked()), SLOT(slotBGColorDialog()));
    appAppearanceGrid->addWidget(BGColorButton, 4, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Grid Color (set after reload):"), appSettingsTab) ,5, 0);
    GridColorButton = new QPushButton("      ", appSettingsTab);
    connect(GridColorButton, SIGNAL(clicked()), SLOT(slotGridColorDialog()));
    appAppearanceGrid->addWidget(GridColorButton, 5, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Draw diagrams with anti-aliasing feature:")), 6, 0);
    checkAntiAliasing = new QCheckBox(appSettingsTab);
    checkAntiAliasing->setToolTip(tr("Use anti-aliasing for graphs for a smoother appearance."));
    appAppearanceGrid->addWidget(checkAntiAliasing, 6, 1);
    checkAntiAliasing->setChecked(QucsSettings.GraphAntiAliasing);

    appAppearanceGrid->addWidget(new QLabel(tr("Draw text with anti-aliasing feature:")), 7, 0);
    checkTextAntiAliasing = new QCheckBox(appSettingsTab);
    checkTextAntiAliasing->setToolTip(tr("Use anti-aliasing for text for a smoother appearance."));
    appAppearanceGrid->addWidget(checkTextAntiAliasing, 7, 1);
    checkTextAntiAliasing->setChecked(QucsSettings.TextAntiAliasing);

    appAppearanceGrid->addWidget(new QLabel(tr("Default graph line thickness:"), appSettingsTab), 8, 0);
    graphLineWidthEdit = new QLineEdit(appSettingsTab);
    graphLineWidthEdit->setValidator(val50);
    appAppearanceGrid->addWidget(graphLineWidthEdit, 8, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("App Style:"), appSettingsTab), 9, 0);
    QStringList styles = QStyleFactory::keys(); // Get available styles
    StyleCombo = new QComboBox(appSettingsTab);
    StyleCombo->addItems(styles);
    appAppearanceGrid->addWidget(StyleCombo,9,1);


    // The style of the platform's themes: the one on the application, or
    // the one to go back to while a designed theme draws with its own.
    const QString currentStyle = qucs_s::apptheme::nativeStyle();
    int index = StyleCombo->findText(currentStyle, Qt::MatchFixedString);
    if (index != -1) {
        StyleCombo->setCurrentIndex(index);
    }

    appAppearanceGrid->addWidget(new QLabel(tr("Theme:"), appSettingsTab), 10, 0);
    ThemeCombo = new QComboBox(appSettingsTab);
    ThemeCombo->setObjectName("themeCombo");
    for (int theme : qucs_s::apptheme::themes()) {
        if (theme == qucs_s::apptheme::Daylight || theme == qucs_s::apptheme::Graphite)
            ThemeCombo->insertSeparator(ThemeCombo->count());
        ThemeCombo->addItem(qucs_s::apptheme::swatch(theme), qucs_s::apptheme::name(theme), theme);
    }
    ThemeCombo->setToolTip(tr("The colours of the application's windows, menus and dialogs. System, Dark "
                              "and Light are the platform's own look (the App Style above); the others "
                              "are designed themes that look the same on every platform, with a "
                              "schematic paper and grid of their own. Also under View > Theme."));
    ThemeCombo->setCurrentIndex(ThemeCombo->findData(QucsSettings.Theme));
    appAppearanceGrid->addWidget(ThemeCombo, 10, 1);
    // A designed theme draws with its own style: App Style is for the
    // platform's themes.
    auto styleFollowsTheme = [this] {
        const bool designed = qucs_s::apptheme::designedTheme(ThemeCombo->currentData().toInt()) != nullptr;
        StyleCombo->setEnabled(!designed);
        StyleCombo->setToolTip(designed ? tr("The designed themes draw with a style of their own; this one "
                                             "is used with System, Dark and Light.")
                                        : QString());
    };
    connect(ThemeCombo, &QComboBox::currentIndexChanged, this, styleFollowsTheme);
    styleFollowsTheme();

    appAppearanceGrid->addWidget(new QLabel(tr("Schematic paper and grid from the theme:"), appSettingsTab), 11, 0);
    paperFollowsTheme = new QCheckBox(appSettingsTab);
    paperFollowsTheme->setToolTip(tr("The schematic is drawn on the theme's paper and grid instead of the "
                                     "background and grid colours above: a designed theme's own, dark "
                                     "paper in the Dark theme (the light System and Light themes keep the "
                                     "colours above). On dark paper symbols, wires and texts are lightened "
                                     "to show. Prints and exports stay on white."));
    paperFollowsTheme->setChecked(QucsSettings.PaperFollowsTheme);
    appAppearanceGrid->addWidget(paperFollowsTheme, 11, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Schematic grid:"), appSettingsTab), 12, 0);
    gridModeCombo = new QComboBox(appSettingsTab);
    gridModeCombo->setObjectName("gridModeCombo");
    gridModeCombo->addItem(tr("As each schematic says"), 0);
    gridModeCombo->addItem(tr("Always hidden"), 1);
    gridModeCombo->addItem(tr("Always shown"), 2);
    gridModeCombo->setToolTip(tr("Whether the grid is drawn: as each schematic keeps it (View > Show Grid, "
                                 "Document Settings), or hidden or shown in every schematic regardless. "
                                 "The files are not changed, and elements still snap to the grid."));
    gridModeCombo->setCurrentIndex(gridModeCombo->findData(QucsSettings.GridMode));
    appAppearanceGrid->addWidget(gridModeCombo, 12, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Lock the toolbars:"), appSettingsTab), 13, 0);
    lockToolbarsCheck = new QCheckBox(appSettingsTab);
    lockToolbarsCheck->setObjectName("lockToolbarsCheck");
    lockToolbarsCheck->setToolTip(tr("The toolbars stay where they are: they cannot be dragged to another "
                                     "place or off the window by accident. Also under View > Toolbars, "
                                     "and in the menu of a right click on the toolbars."));
    lockToolbarsCheck->setChecked(QucsSettings.LockToolbars);
    appAppearanceGrid->addWidget(lockToolbarsCheck, 13, 1);

    appAppearanceGrid->addWidget(new QLabel(tr("Cut long file names after (characters):"), appSettingsTab), 14, 0);
    fileNameCapSpin = new QSpinBox(appSettingsTab);
    fileNameCapSpin->setObjectName("fileNameCapSpin");
    fileNameCapSpin->setRange(0, 1000);
    fileNameCapSpin->setSpecialValueText(tr("Never"));
    fileNameCapSpin->setToolTip(tr("A file's name longer than this is cut in its document's tab and in the "
                                   "Claude Code panel, and \u2026 stands for the rest: its extension is "
                                   "always shown whole (a_very_long_na\u2026.sch). The tab's tooltip gives "
                                   "the whole path.\n0 (Never): names are shown whole."));
    fileNameCapSpin->setValue(QucsSettings.FileNameCap);
    appAppearanceGrid->addWidget(fileNameCapSpin, 14, 1);

    t->addTab(appAppearanceTab, tr("Appearance"));

    // ...........................................................
    // The source code editor settings tab: how each language is highlighted
    syntaxPage = new SyntaxSettingsPage(t);
    t->addTab(syntaxPage, tr("Source Code Editor"));
    // Open on the language of the text document in front, when it has one.
    if (auto *text = qobject_cast<TextDoc *>(App->DocumentTab != nullptr ? App->DocumentTab->currentWidget() : nullptr);
        text != nullptr && text->language != LANG_NONE)
        syntaxPage->select(text->language);

    QPalette p;

    // ...........................................................
    // The file types tab
    QWidget *fileTypesTab = new QWidget(t);
    QGridLayout *fileTypesGrid = new QGridLayout(fileTypesTab);

    QLabel *note = new QLabel(
        tr("Register filename extensions here in order to\nopen files with an appropriate program.\n"
           "The program \"%1\" is the text editor built into Qucs.").arg(QucsApp::QucsEditorProgram));
    fileTypesGrid->addWidget(note,0,0,1,3);

    // the fileTypesTableWidget displays information on the file types
    fileTypesTableWidget = new QTableWidget(fileTypesTab);
    fileTypesTableWidget->setColumnCount(2);

    QTableWidgetItem *item1 = new QTableWidgetItem();
    QTableWidgetItem *item2 = new QTableWidgetItem();

    fileTypesTableWidget->setHorizontalHeaderItem(0, item1);
    fileTypesTableWidget->setHorizontalHeaderItem(1, item2);

    item1->setText(tr("Suffix"));
    item2->setText(tr("Program"));

    fileTypesTableWidget->horizontalHeader()->setStretchLastSection(true);
    fileTypesTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    fileTypesTableWidget->horizontalHeader()->setSectionsClickable(false); // no action when clicking on the header
    fileTypesTableWidget->verticalHeader()->hide();
    connect(fileTypesTableWidget, SIGNAL(cellClicked(int,int)), SLOT(slotTableClicked(int,int)));
    fileTypesGrid->addWidget(fileTypesTableWidget,1,0,4,1);

    // fill listview with already registered file extensions
    QStringList::Iterator it = QucsSettings.FileTypes.begin();
    while(it != QucsSettings.FileTypes.end())
    {
        int row = fileTypesTableWidget->rowCount();
        fileTypesTableWidget->setRowCount(row+1);
        QTableWidgetItem *suffix = new QTableWidgetItem(QString((*it).section('/',0,0)));
        QTableWidgetItem *program = new QTableWidgetItem(QString((*it).section('/',1)));   // may be a path
        suffix->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        program->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        fileTypesTableWidget->setItem(row, 0, suffix);
        fileTypesTableWidget->setItem(row, 1, program);
        it++;
    }

    QLabel *l5 = new QLabel(tr("Suffix:"), fileTypesTab);
    fileTypesGrid->addWidget(l5,1,1);
    Input_Suffix = new QLineEdit(fileTypesTab);
    Input_Suffix->setValidator(Validator);
    fileTypesGrid->addWidget(Input_Suffix,1,2);
//  connect(Input_Suffix, SIGNAL(returnPressed()), SLOT(slotGotoProgEdit())); //not implemented

    QLabel *l6 = new QLabel(tr("Program:"), fileTypesTab);
    fileTypesGrid->addWidget(l6,2,1);
    Input_Program = new QLineEdit(fileTypesTab);
    Input_Program->setPlaceholderText(tr("program [arguments], or %1").arg(QucsApp::QucsEditorProgram));
    fileTypesGrid->addWidget(Input_Program,2,2);

    // The built-in editor, one click away.
    QPushButton *QucsEditorButt = new QPushButton(tr("Qucs editor"));
    QucsEditorButt->setToolTip(tr("Open files with this suffix in the text editor built into Qucs"));
    fileTypesGrid->addWidget(QucsEditorButt,3,2);
    connect(QucsEditorButt, &QPushButton::clicked, this, [this] {
        Input_Program->setText(QucsApp::QucsEditorProgram);
    });

    QPushButton *AddButt = new QPushButton(tr("Set"));
    fileTypesGrid->addWidget(AddButt,4,1);
    connect(AddButt, SIGNAL(clicked()), SLOT(slotAddFileType()));
    QPushButton *RemoveButt = new QPushButton(tr("Remove"));
    fileTypesGrid->addWidget(RemoveButt,4,2);
    connect(RemoveButt, SIGNAL(clicked()), SLOT(slotRemoveFileType()));

    fileTypesGrid->setRowStretch(4,4);
    t->addTab(fileTypesTab, tr("File Types"));

    // ...........................................................
    // The contents tab: which files each category of the Content panel lists
    QWidget *contentsTab = new QWidget(t);
    QVBoxLayout *contentsLayout = new QVBoxLayout(contentsTab);
    QLabel *contentsNote = new QLabel(
        tr("Each category of the Content panel lists the project's files whose names match its patterns: "
           "extensions (*.txt, .txt or txt) or names with wildcards (notes*.md), separated by commas. "
           "A file is listed under the first category from the top that matches it; * in Others takes "
           "whatever no other category took. Scratch lists the files of the project's Scratch folder "
           "that match. Categories of your own - Touchstone files, measurements, reports - come after "
           "Text: add them below, name them and give their patterns."), contentsTab);
    contentsNote->setWordWrap(true);
    contentsLayout->addWidget(contentsNote);

    // The rows in a scroll area: the tab does not make the dialog taller.
    QScrollArea *contentsScroll = new QScrollArea(contentsTab);
    contentsScroll->setWidgetResizable(true);
    contentsScroll->setFrameShape(QFrame::NoFrame);
    QWidget *contentsRows = new QWidget(contentsScroll);
    QVBoxLayout *contentsColumn = new QVBoxLayout(contentsRows);
    contentsColumn->setContentsMargins(0, 0, 0, 0);
    // The built-in categories, in the panel's order: those up to Text, the
    // user's own (their table), then Others and Scratch.
    const auto builtIn = [this, contentsRows](int from, int to) {
        auto *grid = new QGridLayout();
        for (int category = from; category <= to; ++category) {
            QLabel *name = new QLabel(ProjectView::categoryName(category) + ":", contentsRows);
            QLineEdit *patterns = new QLineEdit(ProjectView::patterns(category), contentsRows);
            patterns->setObjectName("contentPatterns" + ProjectView::categoryKey(category));
            patterns->setCursorPosition(0);   // a long list shows its start
            patterns->setPlaceholderText(tr("none: the category lists no files"));
            patterns->setToolTip(tr("Default: %1").arg(ProjectView::defaultPatterns(category)));
            name->setBuddy(patterns);
            name->setMinimumWidth(110);
            grid->addWidget(name, category - from, 0);
            grid->addWidget(patterns, category - from, 1);
            contentPatternEdits.append(patterns);
        }
        grid->setColumnStretch(1, 1);
        return grid;
    };
    contentsColumn->addLayout(builtIn(0, ProjectView::Text));

    // The categories of the user's own: a name, its patterns; added,
    // removed and put in order here.
    QGroupBox *userBox = new QGroupBox(tr("Your categories (after Text, before Others)"), contentsRows);
    QVBoxLayout *userLayout = new QVBoxLayout(userBox);
    contentUserCategories = new QTableWidget(0, 2, userBox);
    contentUserCategories->setObjectName("contentUserCategories");
    contentUserCategories->setHorizontalHeaderLabels({tr("Name"), tr("Patterns")});
    contentUserCategories->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    contentUserCategories->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    contentUserCategories->setColumnWidth(0, 150);
    contentUserCategories->verticalHeader()->hide();
    contentUserCategories->setSelectionBehavior(QAbstractItemView::SelectRows);
    contentUserCategories->setSelectionMode(QAbstractItemView::SingleSelection);
    contentUserCategories->setMinimumHeight(110);
    contentUserCategories->setToolTip(tr("A name, and the patterns of the files it lists: *.s2p, *.s4p, touchstone*"));
    fillUserCategories(QucsSettings.ContentUserCategories);
    userLayout->addWidget(contentUserCategories);
    QHBoxLayout *userButtons = new QHBoxLayout();
    const auto userButton = [&](const QString& text, const char* name, auto slot) {
        auto *b = new QPushButton(text, userBox);
        b->setObjectName(name);
        connect(b, &QPushButton::clicked, this, slot);
        userButtons->addWidget(b);
        return b;
    };
    userButton(tr("Add Category"), "contentAddCategory", &QucsSettingsDialog::slotAddContentCategory);
    userButton(tr("Remove"), "contentRemoveCategory", &QucsSettingsDialog::slotRemoveContentCategory);
    userButton(tr("Move Up"), "contentCategoryUp", [this] { moveContentCategory(-1); });
    userButton(tr("Move Down"), "contentCategoryDown", [this] { moveContentCategory(1); });
    userButtons->addStretch();
    userLayout->addLayout(userButtons);
    contentsColumn->addWidget(userBox);

    contentsColumn->addLayout(builtIn(ProjectView::Others, ProjectView::Scratch));
    contentsColumn->addStretch(1);
    contentsScroll->setWidget(contentsRows);
    contentsScroll->viewport()->setAutoFillBackground(false);   // on the tab's own background
    contentsRows->setAutoFillBackground(false);
    contentsLayout->addWidget(contentsScroll, 1);

    QHBoxLayout *contentsButtons = new QHBoxLayout();
    contentsButtons->addStretch();
    QPushButton *restorePatterns = new QPushButton(tr("Restore Default Patterns"), contentsTab);
    connect(restorePatterns, &QPushButton::clicked, this, &QucsSettingsDialog::slotRestoreContentPatterns);
    contentsButtons->addWidget(restorePatterns);
    contentsLayout->addLayout(contentsButtons);
    t->addTab(contentsTab, tr("Contents"));

    // ...........................................................
    // The workspace tab: its folder, which folders are projects, and what
    // is brought back when Qucs-S starts (workspacesession.h).
    QWidget *workspaceTab = new QWidget(t);
    workspaceTab->setObjectName(QStringLiteral("workspaceTab"));
    QVBoxLayout *workspaceBox = new QVBoxLayout(workspaceTab);

    QGroupBox *folderGroup = new QGroupBox(tr("Workspace Folder"), workspaceTab);
    QGridLayout *folderGrid = new QGridLayout(folderGroup);
    folderGrid->addWidget(new QLabel(tr("Folder:"), folderGroup), 0, 0);
    homeEdit = new QLineEdit(folderGroup);
    homeEdit->setObjectName(QStringLiteral("workspaceFolder"));
    homeEdit->setToolTip(tr("The folder of your projects and of the user libraries (user_lib), which the "
                            "Projects panel lists (Qucs Home). Another closes the open documents."));
    folderGrid->addWidget(homeEdit, 0, 1);
    QPushButton *HomeButt = new QPushButton(tr("Browse"));
    folderGrid->addWidget(HomeButt, 0, 2);
    connect(HomeButt, SIGNAL(clicked()), SLOT(slotHomeDirBrowse()));
    workspaceBox->addWidget(folderGroup);

    // Which folders are projects: those named NAME_prj, or any folder.
    QGroupBox *projectsGroup = new QGroupBox(tr("Projects"), workspaceTab);
    projectsGroup->setObjectName("projectFolders");
    QVBoxLayout *projectsBox = new QVBoxLayout(projectsGroup);
    anyFolderIsProject = new QCheckBox(tr("Any folder is a project, not only one named NAME_prj"), projectsGroup);
    anyFolderIsProject->setObjectName("anyFolderIsProject");
    anyFolderIsProject->setToolTip(tr("Open Project and a folder dropped on the window open any folder as a "
                                      "project, and New Project names the folder as typed (no \"_prj\" added). "
                                      "A folder named NAME_prj is always a project; other Qucs-S installations "
                                      "know only those."));
    projectsBox->addWidget(anyFolderIsProject);
    QLabel *projectsNote = new QLabel(
        tr("When on, every folder of the workspace but user_lib (the user libraries) is a project, and "
           "Import Project and Link Project keep a folder's name."),
        projectsGroup);
    projectsNote->setWordWrap(true);
    projectsBox->addWidget(projectsNote);
    workspaceBox->addWidget(projectsGroup);

    // What the next start brings back.
    QGroupBox *startGroup = new QGroupBox(tr("When Qucs-S Starts"), workspaceTab);
    startGroup->setObjectName(QStringLiteral("workspaceAtStart"));
    QVBoxLayout *startBox = new QVBoxLayout(startGroup);
    restoreWorkspace = new QCheckBox(tr("Restore the workspace as it was when Qucs-S closed"), startGroup);
    restoreWorkspace->setObjectName(QStringLiteral("restoreWorkspace"));
    restoreWorkspace->setToolTip(tr("Qucs-S keeps what is open when it closes (and with each autosave), and opens it "
                                    "again at the next start - as the parts below say.\n"
                                    "Off: every start begins with an empty schematic, and nothing is kept."));
    startBox->addWidget(restoreWorkspace);
    const auto part = [&](const QString &text, const char *name, const QString &tip) {
        auto *box = new QCheckBox(text, startGroup);
        box->setObjectName(QLatin1String(name));
        box->setToolTip(tip);
        auto *row = new QHBoxLayout;
        row->addSpacing(22);
        row->addWidget(box, 1);
        startBox->addLayout(row);
        return box;
    };
    restoreProject = part(tr("The project that was open"), "restoreProject",
                          tr("Opened again, when its folder is still there. A project named when Qucs-S starts "
                             "(from the command line, or a folder opened with Qucs-S) opens instead."));
    restoreDocuments = part(tr("The documents that were open, in their panes - split and sized as they were"),
                            "restoreDocuments",
                            tr("Each in the pane it was in, in its order, the one in front in front again. A "
                               "document never saved has no file to open again, and a file no longer there is "
                               "left out; the status bar says so."));
    restorePanels = part(tr("The panels and toolbars: which were shown, where, and how big"), "restorePanels",
                         tr("The docks - Projects and Content, Components, Claude Code, the simulation console, "
                            "the Terminal ... - and the toolbars, as they were; the left dock's page too."));
    connect(restoreWorkspace, &QCheckBox::toggled, this, [this](bool on) {
        for (QCheckBox *box : {restoreProject, restoreDocuments, restorePanels}) box->setEnabled(on);
    });
    restoreWindowGeometry = new QCheckBox(tr("Remember the window's size and position"), startGroup);
    restoreWindowGeometry->setObjectName(QStringLiteral("restoreWindowGeometry"));
    restoreWindowGeometry->setToolTip(tr("Off: the window opens in the middle of the screen, half its size."));
    startBox->addWidget(restoreWindowGeometry);
    reopenConversations = new QCheckBox(tr("Reopen the Claude Code conversations that were open"), startGroup);
    reopenConversations->setObjectName(QStringLiteral("reopenConversations"));
    reopenConversations->setToolTip(tr("Each as it was, going on with its Claude Code session (as the Claude Code "
                                       "dock's ⋯ > Reopen Conversations at Start)."));
    startBox->addWidget(reopenConversations);
    QLabel *startNote = new QLabel(
        tr("After Qucs-S did not exit cleanly, it asks before it opens the workspace again. A document with "
           "unsaved changes is asked about when Qucs-S closes, as always; its autosaved copy after a crash is "
           "offered instead of the file."),
        startGroup);
    startNote->setWordWrap(true);
    startBox->addWidget(startNote);
    auto *keptRow = new QHBoxLayout;
    keptWorkspaceLabel = new QLabel(startGroup);
    keptWorkspaceLabel->setObjectName(QStringLiteral("keptWorkspace"));
    keptWorkspaceLabel->setWordWrap(true);
    keptRow->addWidget(keptWorkspaceLabel, 1);
    forgetWorkspaceButton = new QPushButton(tr("Forget It"), startGroup);
    forgetWorkspaceButton->setObjectName(QStringLiteral("forgetWorkspace"));
    forgetWorkspaceButton->setToolTip(tr("Forgets the workspace kept, at once: the next start begins with an empty "
                                         "schematic - unless Qucs-S keeps it again when it closes."));
    connect(forgetWorkspaceButton, &QPushButton::clicked, this, &QucsSettingsDialog::slotForgetWorkspace);
    keptRow->addWidget(forgetWorkspaceButton);
    startBox->addLayout(keptRow);
    workspaceBox->addWidget(startGroup);
    workspaceBox->addStretch(1);
    t->addTab(workspaceTab, tr("Workspace"));

    // ...........................................................
    // The locations tab
    QWidget *locationsTab = new QWidget(t);
    QGridLayout *locationsGrid = new QGridLayout(locationsTab);

    // Group box for standard paths and external tools
    QGroupBox *stdPathsGroup = new QGroupBox(tr("Standard Paths and External Applications"), locationsTab);
    QGridLayout *stdPathsGrid = new QGridLayout(stdPathsGroup);

    // (The workspace folder - Qucs Home - is on the Workspace tab.)
    stdPathsGrid->addWidget(new QLabel(tr("AdmsXml Path:"), stdPathsGroup), 1, 0);
    admsXmlEdit = new QLineEdit(locationsTab);
    stdPathsGrid->addWidget(admsXmlEdit, 1, 1);
    QPushButton *AdmsXmlButt = new QPushButton(tr("Browse"));
    stdPathsGrid->addWidget(AdmsXmlButt, 1, 2);
    connect(AdmsXmlButt, SIGNAL(clicked()), SLOT(slotAdmsXmlDirBrowse()));

    stdPathsGrid->addWidget(new QLabel(tr("ASCO Path:"), stdPathsGroup), 2, 0);
    ascoEdit = new QLineEdit(locationsTab);
    stdPathsGrid->addWidget(ascoEdit, 2, 1);
    QPushButton *ascoButt = new QPushButton(tr("Browse"));
    stdPathsGrid->addWidget(ascoButt, 2, 2);
    connect(ascoButt, SIGNAL(clicked()), SLOT(slotAscoDirBrowse()));

    stdPathsGrid->addWidget(new QLabel(tr("Octave Path:"), stdPathsGroup), 3, 0);
    octaveEdit = new QLineEdit(locationsTab);
    stdPathsGrid->addWidget(octaveEdit, 3, 1);
    QPushButton *OctaveButt = new QPushButton(tr("Browse"));
    stdPathsGrid->addWidget(OctaveButt, 3, 2);
    connect(OctaveButt, SIGNAL(clicked()), SLOT(slotOctaveDirBrowse()));

    stdPathsGrid->addWidget(new QLabel(tr("OpenVAF Path:"), stdPathsGroup), 4, 0);
    OpenVAFEdit = new QLineEdit(locationsTab);
    stdPathsGrid->addWidget(OpenVAFEdit, 4, 1);
    QPushButton *OpenVAFButt = new QPushButton(tr("Browse"));
    stdPathsGrid->addWidget(OpenVAFButt, 4, 2);
    connect(OpenVAFButt, SIGNAL(clicked()), SLOT(slotOpenVAFDirBrowse()));

    stdPathsGrid->addWidget(new QLabel(tr("RF Layout Path:"), stdPathsGroup), 5, 0);
    RFLayoutEdit = new QLineEdit(locationsTab);
    stdPathsGrid->addWidget(RFLayoutEdit, 5, 1);
    QPushButton *RFLButt = new QPushButton(tr("Browse"));
    stdPathsGrid->addWidget(RFLButt, 5, 2);
    connect(RFLButt, SIGNAL(clicked()), SLOT(slotRFLayoutDirBrowse()));

    stdPathsGrid->addWidget(new QLabel(tr("Python Path:"), stdPathsGroup), 6, 0);
    pythonEdit = new QLineEdit(locationsTab);
    pythonEdit->setPlaceholderText(tr("python3 on PATH"));
    pythonEdit->setToolTip(tr("The interpreter of the Python shell dock (View > Python Shell). "
                              "Empty: python3 (or python) found on PATH."));
    stdPathsGrid->addWidget(pythonEdit, 6, 1);
    QPushButton *PythonButt = new QPushButton(tr("Browse"));
    stdPathsGrid->addWidget(PythonButt, 6, 2);
    connect(PythonButt, SIGNAL(clicked()), SLOT(slotPythonBrowse()));

    locationsGrid->addWidget(stdPathsGroup, 0, 0, 1, 3);

    // (Which folders are projects: on the Workspace tab.)


    // The widgets related to the path searh are put in a groupbox widget
    QGroupBox *pathsGroup = new QGroupBox(tr("Subcircuit Search Paths"), locationsTab);
    QGridLayout *pathsGrid = new QGridLayout(pathsGroup);

    // the pathsTableWidget displays the path list
    // It includes a second column for buttons to remove entries
    pathsTableWidget = newPathTable(pathsGroup, tr("Subcircuit Search Path List"), QStringLiteral("subcircuitPaths"),
                                    tr("Subcircuit search paths"));
    pathsGrid->addWidget(pathsTableWidget, 0, 0, 3, 2);

    QPushButton *AddPathButt = new QPushButton(tr("Add Path"));
    pathsGrid->addWidget(AddPathButt, 0, 2);
    connect(AddPathButt, SIGNAL(clicked()), SLOT(slotAddPath()));

    QPushButton *AddPathSubFolButt = new QPushButton(tr("Add Path With SubFolders"));
    pathsGrid->addWidget(AddPathSubFolButt, 1, 2);
    connect(AddPathSubFolButt, SIGNAL(clicked()), SLOT(slotAddPathWithSubFolders()));


    // Button for removing all path on a row. It triggers slotClearAllPaths().
    QPushButton * ClearAllPathsButt = new QPushButton(tr("Clear All Paths"));
    pathsGrid->addWidget(ClearAllPathsButt, 2, 2);
    connect(ClearAllPathsButt, SIGNAL(clicked()), SLOT(slotClearAllPaths()));

    locationsGrid->addWidget(pathsGroup, 1, 0, 1, 3);

    // The folders of component libraries besides the installed ones and
    // user_lib: each a section of the Libraries panel.
    QGroupBox *libraryPathsGroup = new QGroupBox(tr("Library Search Paths"), locationsTab);
    QGridLayout *libraryPathsGrid = new QGridLayout(libraryPathsGroup);
    libraryPathsTableWidget = newPathTable(libraryPathsGroup, tr("Library Search Path List"), QStringLiteral("libraryPaths"),
                                           tr("Library search paths"));
    libraryPathsGrid->addWidget(libraryPathsTableWidget, 0, 0, 3, 2);
    QPushButton *addLibraryPathButt = new QPushButton(tr("Add Path"));
    libraryPathsGrid->addWidget(addLibraryPathButt, 0, 2);
    connect(addLibraryPathButt, &QPushButton::clicked, this, &QucsSettingsDialog::slotAddLibraryPath);
    QPushButton *addLibraryPathSubButt = new QPushButton(tr("Add Path With SubFolders"));
    libraryPathsGrid->addWidget(addLibraryPathSubButt, 1, 2);
    connect(addLibraryPathSubButt, &QPushButton::clicked, this, &QucsSettingsDialog::slotAddLibraryPathWithSubFolders);
    QPushButton *clearLibraryPathsButt = new QPushButton(tr("Clear All Paths"));
    libraryPathsGrid->addWidget(clearLibraryPathsButt, 2, 2);
    connect(clearLibraryPathsButt, &QPushButton::clicked, this, &QucsSettingsDialog::slotClearAllLibraryPaths);
    QLabel *libraryPathsNote = new QLabel(
        tr("The libraries (.lib) of each folder are a section of the Libraries panel, after the user libraries; a "
           "library a placed part names is looked for in them when it is not where it was. Create Library can "
           "save into one."),
        libraryPathsGroup);
    libraryPathsNote->setWordWrap(true);
    libraryPathsGrid->addWidget(libraryPathsNote, 3, 0, 1, 3);
    locationsGrid->addWidget(libraryPathsGroup, 2, 0, 1, 3);

    // create a copy of the current global path list
    currentPaths = QStringList(qucsPathList);
    makePathTable();
    currentLibraryPaths = QucsSettings.LibraryPaths;
    makePathTable(libraryPathsTableWidget, &currentLibraryPaths);

    t->addTab(locationsTab, tr("Locations"));

    // ...........................................................
    // buttons on the bottom of the dialog (independent of the TabWidget)

    QHBoxLayout *Butts = new QHBoxLayout();
    Butts->setSpacing(3);
    Butts->setContentsMargins(3,3,3,3);
    all->addLayout(Butts);

    QPushButton *OkButt = new QPushButton(tr("OK"));
    Butts->addWidget(OkButt);
    connect(OkButt, SIGNAL(clicked()), SLOT(slotOK()));
    QPushButton *ApplyButt = new QPushButton(tr("Apply"));
    Butts->addWidget(ApplyButt);
    connect(ApplyButt, SIGNAL(clicked()), SLOT(slotApply()));
    QPushButton *CancelButt = new QPushButton(tr("Cancel"));
    Butts->addWidget(CancelButt);
    connect(CancelButt, SIGNAL(clicked()), SLOT(reject()));
    QPushButton *DefaultButt = new QPushButton(tr("Default Values"));
    Butts->addWidget(DefaultButt);
    connect(DefaultButt, SIGNAL(clicked()), SLOT(slotDefaultValues()));

    OkButt->setDefault(true);

    // ...........................................................
    // fill the fields with the Qucs-Properties
    Font  = QucsSettings.font;
    AppFont = QucsSettings.appFont;
    TextFont = QucsSettings.textFont;



    FontButton->setText(getFontDescription(Font));
    AppFontButton->setText(getFontDescription(AppFont));
    TextFontButton->setText(getFontDescription(TextFont));
    QString s = QString::number(QucsSettings.largeFontSize, 'f', 1);
    LargeFontSizeEdit->setText(s);
    graphLineWidthEdit->setText(_settings::Get().item<QString>("DefaultGraphLineWidth"));

    p = BGColorButton->palette();
    p.setColor(BGColorButton->backgroundRole(), QucsSettings.BGColor);
    BGColorButton->setPalette(p);

    p = GridColorButton->palette();
    p.setColor(GridColorButton->backgroundRole(), _settings::Get().item<QColor>("GridColor"));
    GridColorButton->setPalette(p);

    undoNumEdit->setText(QString::number(QucsSettings.maxUndo));
    editorEdit->setText(QucsSettings.Editor);
    checkWiring->setChecked(QucsSettings.NodeWiring);
    allowFlexibleWires->setChecked(_settings::Get().item<bool>("AllowFlexibleWires"));
    contentAutoRefresh->setChecked(QucsSettings.ContentAutoRefresh);
    contentRefreshSeconds->setValue(QucsSettings.ContentRefreshSeconds);
    contentRefreshSeconds->setEnabled(QucsSettings.ContentAutoRefresh);
    contentFolderIcons->setChecked(QucsSettings.ContentFolderIcons);
    showPinNames->setChecked(QucsSettings.ShowPinNames);
    showPinDirections->setChecked(QucsSettings.ShowPinDirections);
    embedVerilogA->setChecked(QucsSettings.EmbedVerilogAInLibraries);
    writeDocSettings->setChecked(QucsSettings.WriteTextDocSettings);

    ShortcutButton->setText("Custom Shortcut");

    for(int z=LanguageCombo->count()-1; z>=0; z--)
        if(LanguageCombo->itemText(z).section('(',1,1).remove(')') == QucsSettings.Language)
            LanguageCombo->setCurrentIndex(z);

    /*! Load paths from settings */
    homeEdit->setText(QucsSettings.qucsWorkspaceDir.canonicalPath());
    anyFolderIsProject->setChecked(QucsSettings.AnyFolderIsProject);
    restoreWorkspace->setChecked(QucsSettings.RestoreWorkspace);
    restoreProject->setChecked(QucsSettings.RestoreProject);
    restoreDocuments->setChecked(QucsSettings.RestoreDocuments);
    restorePanels->setChecked(QucsSettings.RestorePanels);
    for (QCheckBox *box : {restoreProject, restoreDocuments, restorePanels}) box->setEnabled(QucsSettings.RestoreWorkspace);
    restoreWindowGeometry->setChecked(QucsSettings.RestoreWindowGeometry);
    reopenConversations->setChecked(qucs_s::claude::history::reopenAtStart());
    showKeptWorkspace();
    admsXmlEdit->setText(misc::canonicalDir(QucsSettings.AdmsXmlBinDir));
    ascoEdit->setText(misc::canonicalDir(QucsSettings.AscoBinDir));
    octaveEdit->setText(QucsSettings.OctaveExecutable);
    OpenVAFEdit->setText(QucsSettings.OpenVAFExecutable);
    RFLayoutEdit->setText(QucsSettings.RFLayoutExecutable);
    pythonEdit->setText(QucsSettings.PythonExecutable);


    resize(600, 200);
}

QucsSettingsDialog::~QucsSettingsDialog()
{
    delete all;
    delete val50;
    delete val200;
    delete Validator;
}

// -----------------------------------------------------------
void QucsSettingsDialog::slotAddFileType()
{
    QModelIndexList indexes = fileTypesTableWidget->selectionModel()->selection().indexes();
    if (indexes.count())
    {
        fileTypesTableWidget->item(indexes.at(0).row(),0)->setText(Input_Suffix->text());
        fileTypesTableWidget->item(indexes.at(0).row(),1)->setText(Input_Program->text());
        fileTypesTableWidget->selectionModel()->clear();
        return;
    }

    //check before append
    for(int r=0; r < fileTypesTableWidget->rowCount(); r++)
        if(fileTypesTableWidget->item(r,0)->text() == Input_Suffix->text())
        {
            QMessageBox::critical(this, tr("Error"),
                                  tr("This suffix is already registered!"));
            return;
        }

    int row = fileTypesTableWidget->rowCount();
    fileTypesTableWidget->setRowCount(row+1);

    QTableWidgetItem *newSuffix = new QTableWidgetItem(QString(Input_Suffix->text()));
    newSuffix->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    fileTypesTableWidget->setItem(row, 0, newSuffix);

    QTableWidgetItem *newProgram = new QTableWidgetItem(Input_Program->text());
    fileTypesTableWidget->setItem(row, 1, newProgram);
    newProgram->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    Input_Suffix->setFocus();
    Input_Suffix->clear();
    Input_Program->clear();
}

// -----------------------------------------------------------
void QucsSettingsDialog::slotRemoveFileType()
{
    QModelIndexList indexes = fileTypesTableWidget->selectionModel()->selection().indexes();
    if (indexes.count())
    {
        fileTypesTableWidget->removeRow(indexes.at(0).row());
        fileTypesTableWidget->selectionModel()->clear();
        Input_Suffix->setText("");
        Input_Program->setText("");
        return;
    }
}

// -----------------------------------------------------------
// Applies any changed settings and closes the dialog
void QucsSettingsDialog::slotOK()
{
    slotApply();
    accept();
}

// -----------------------------------------------------------
// Applies any changed settings
/// \todo simplify the conditionals involving `changed = true`
///  if user hit apply, save settings and refresh everything
void QucsSettingsDialog::slotApply()
{
    bool changed = false;
    bool homeDirChanged = false;

    // check QucsHome is changed, will require to close all files and refresh tree
    // (the field shows the canonical path: compare canonically, or a workspace
    // behind a symbolic link - /tmp, a linked home - is "changed" on every Apply)
    if (QDir(homeEdit->text()).canonicalPath() != QucsSettings.qucsWorkspaceDir.canonicalPath()
        && homeEdit->text() != QucsSettings.qucsWorkspaceDir.path()) {
      // close all open files, asking the user whether to save the modified ones
      // if user aborts closing, just return
      if(!App->closeAllFiles()) return;

      // The Projects panel lists the new one (the folder it shows was left
      // at the old workspace), made if it does not exist yet.
      App->setWorkspace(homeEdit->text());
      homeDirChanged = true;
      // later below the file tree will be refreshed
    }

    bool paperChanged = false;
    if(QucsSettings.BGColor != BGColorButton->palette().color(BGColorButton->backgroundRole()))
    {
        QucsSettings.BGColor = BGColorButton->palette().color(BGColorButton->backgroundRole());
        paperChanged = true;
        changed = true;
    }
    if (QucsSettings.GridMode != gridModeCombo->currentData().toInt())
    {
        QucsSettings.GridMode = gridModeCombo->currentData().toInt();
        App->applyGridSetting();
        changed = true;
    }
    if (QucsSettings.LockToolbars != lockToolbarsCheck->isChecked())
    {
        App->setToolbarsLocked(lockToolbarsCheck->isChecked());
        changed = true;
    }
    if (QucsSettings.FileNameCap != fileNameCapSpin->value())
    {
        QucsSettings.FileNameCap = fileNameCapSpin->value();
        App->titleFileNames();   // (at once: the tabs and the Claude Code panels)
        changed = true;
    }
    if (QucsSettings.PaperFollowsTheme != paperFollowsTheme->isChecked())
    {
        QucsSettings.PaperFollowsTheme = paperFollowsTheme->isChecked();
        paperChanged = true;
        changed = true;
    }

    QString selectedStyle = StyleCombo->currentText();
    bool styleChanged = false;
    if (_settings::Get().item<QString>("AppStyle") != selectedStyle )
    {
        if (QStyleFactory::keys().contains(selectedStyle, Qt::CaseInsensitive)) {
          // Shown now, or - under a designed theme - when a platform theme is back.
          qucs_s::apptheme::setNativeStyle(selectedStyle);
          _settings::Get().setItem<QString>("AppStyle",  selectedStyle);
          changed = true;
          styleChanged = true;
        }
    }

    const int selectedTheme = ThemeCombo->currentData().toInt();
    if (QucsSettings.Theme != selectedTheme || styleChanged)
    {
        // After the style: a new style brings its own palette. The paper
        // may follow the theme; the component list does.
        App->applyTheme(selectedTheme);
        changed = true;
    }
    else if (paperChanged) App->applyPaper();   // every open schematic, in every pane

    // Update all open schematics with the new grid color.
    if (_settings::Get().item<QColor>("GridColor") != GridColorButton->palette().color(GridColorButton->backgroundRole())) {
        _settings::Get().setItem<QColor>("GridColor", GridColorButton->palette().color(GridColorButton->backgroundRole()));

        for (int tab = 0; tab < App->DocumentTab->count(); tab++) {
            QWidget* widget = App->DocumentTab->widget(tab);
            if (Schematic* sch = QucsApp::schematicIn(widget)) {
                sch->setGridColor(_settings::Get().item<QColor>("GridColor"));
            }
        }

        changed = true;
    }

    QucsSettings.font=Font;
    QucsSettings.appFont = AppFont;
    QucsSettings.textFont = TextFont;

    QucsSettings.Language =
        LanguageCombo->currentText().section('(',1,1).remove(')');

    syntaxPage->apply();   // every language's formats

    bool ok;
    if(QucsSettings.maxUndo != undoNumEdit->text().toUInt(&ok))
    {
        QucsSettings.maxUndo = undoNumEdit->text().toInt(&ok);
        changed = true;
    }
    if(QucsSettings.Editor != editorEdit->text())
    {
        QucsSettings.Editor = editorEdit->text();
        changed = true;
    }
    if(QucsSettings.NodeWiring != (unsigned)checkWiring->isChecked())
    {
        QucsSettings.NodeWiring = checkWiring->isChecked();
        changed = true;
    }

    _settings::Get().setItem("AllowFlexibleWires", allowFlexibleWires->isChecked());
    QucsSettings.ContentAutoRefresh = contentAutoRefresh->isChecked();
    QucsSettings.ContentRefreshSeconds = contentRefreshSeconds->value();
    QucsSettings.ContentFolderIcons = contentFolderIcons->isChecked();
    for (int category = 0; category < contentPatternEdits.size(); ++category) {
        ProjectView::setPatterns(category, contentPatternEdits[category]->text());
        contentPatternEdits[category]->setText(ProjectView::patterns(category));   // as read
        contentPatternEdits[category]->setCursorPosition(0);
    }
    // The user's categories: those with a name, their patterns as read.
    QList<ContentCategory> userCategories;
    for (int row = 0; row < contentUserCategories->rowCount(); ++row) {
        const QTableWidgetItem *name = contentUserCategories->item(row, 0);
        const QTableWidgetItem *patterns = contentUserCategories->item(row, 1);
        const QString named = name != nullptr ? name->text().trimmed() : QString();
        if (named.isEmpty()) continue;
        userCategories.append({named, ProjectView::normalizedPatterns(patterns != nullptr ? patterns->text() : QString())});
    }
    QucsSettings.ContentUserCategories = userCategories;
    fillUserCategories(userCategories);
    QucsSettings.ShowPinNames = showPinNames->isChecked();
    QucsSettings.ShowPinDirections = showPinDirections->isChecked();
    QucsSettings.EmbedVerilogAInLibraries = embedVerilogA->isChecked();
    QucsSettings.WriteTextDocSettings = writeDocSettings->isChecked();

    QucsSettings.FileTypes.clear();
    for (int row=0; row < fileTypesTableWidget->rowCount(); row++)
    {
        QucsSettings.FileTypes.append(fileTypesTableWidget->item(row,0)->text()
                                      +"/"+
                                      fileTypesTableWidget->item(row,1)->text());
    }

    /*! Update QucsSettings, tool paths */
    QucsSettings.AdmsXmlBinDir.setPath(admsXmlEdit->text());
    QucsSettings.AscoBinDir.setPath(ascoEdit->text());
    QucsSettings.OctaveExecutable = octaveEdit->text();
    QucsSettings.OpenVAFExecutable = OpenVAFEdit->text();
    QucsSettings.RFLayoutExecutable = RFLayoutEdit->text();
    QucsSettings.PythonExecutable = pythonEdit->text().trimmed();

    if (QucsSettings.IgnoreFutureVersion != checkLoadFromFutureVersions->isChecked())
    {
      QucsSettings.IgnoreFutureVersion = checkLoadFromFutureVersions->isChecked();
      changed = true;
    }

    if (QucsSettings.GraphAntiAliasing != checkAntiAliasing->isChecked())
    {
      QucsSettings.GraphAntiAliasing = checkAntiAliasing->isChecked();
      changed = true;
    }

    if (QucsSettings.TextAntiAliasing != checkTextAntiAliasing->isChecked())
    {
      QucsSettings.TextAntiAliasing = checkTextAntiAliasing->isChecked();
      changed = true;
    }

    if (QucsSettings.fullTraceName != checkFullTraceNames->isChecked())
    {
      QucsSettings.fullTraceName = checkFullTraceNames->isChecked();
      changed = true;
    }

    if (QucsSettings.alwaysPrefixDataset != alwaysPrefixDataset->isChecked())
    {
      QucsSettings.alwaysPrefixDataset = alwaysPrefixDataset->isChecked();
      changed = true;
    }

    // use toDouble() as it can interpret the string according to the current locale
    if (QucsSettings.largeFontSize != LargeFontSizeEdit->text().toDouble(&ok))
    {
        QucsSettings.largeFontSize = LargeFontSizeEdit->text().toDouble(&ok);
        changed = true;
    }

    if (_settings::Get().item<QString>("DefaultGraphLineWidth") != graphLineWidthEdit->text())
    {
        _settings::Get().setItem<QString>("DefaultGraphLineWidth", graphLineWidthEdit->text());
        changed = true;
    }

    // What the next start brings back. Off: nothing is kept (and what was
    // is forgotten).
    QucsSettings.RestoreWorkspace = restoreWorkspace->isChecked();
    QucsSettings.RestoreProject = restoreProject->isChecked();
    QucsSettings.RestoreDocuments = restoreDocuments->isChecked();
    QucsSettings.RestorePanels = restorePanels->isChecked();
    QucsSettings.RestoreWindowGeometry = restoreWindowGeometry->isChecked();
    if (!QucsSettings.RestoreWorkspace) qucs_s::session::forget();
    qucs_s::claude::history::setReopenAtStart(reopenConversations->isChecked());
    showKeptWorkspace();

    // Which folders are projects: the Projects panel and the file browser
    // show them anew (after the workspace, which may have changed above).
    const bool projectsChanged = QucsSettings.AnyFolderIsProject != anyFolderIsProject->isChecked();
    QucsSettings.AnyFolderIsProject = anyFolderIsProject->isChecked();

    // The search paths, before they are saved (the subcircuits' were
    // saved only when Qucs-S closed).
    const bool librariesChanged = QucsSettings.LibraryPaths != currentLibraryPaths;
    QucsSettings.LibraryPaths = currentLibraryPaths;
    QucsMain->updatePathList(currentPaths);

    saveApplSettings();  // also sets the small and large font
    App->applySyntaxSettings();   // the text documents in the formats set
    if (projectsChanged) App->applyProjectSettings();
    // The Content panel as the settings now say (its categories' patterns).
    if (App->projectView() != nullptr) App->projectView()->applyRefreshSettings();

    // if QucsHome is changed, refresh projects tree
    // do this after updating the other paths
    if (homeDirChanged) {;
      // files were actuallt closed above, this will refresh the projects tree
      // and create an empty schematic
      App->slotMenuProjClose();
      changed = true;
    }

    if(changed)
    {
        App->readProjects();
        App->slotUpdateTreeview();
        App->repaint();
    }

    // The Libraries panel with the library search paths as they are now.
    if (librariesChanged) App->fillLibrariesTreeView();

}


// -----------------------------------------------------------
void QucsSettingsDialog::slotFontDialog()
{
    bool ok;
    QFont tmpFont = QFontDialog::getFont(&ok, Font, this);
    if(ok)
    {
        Font = tmpFont;
        FontButton->setText(getFontDescription(Font));
    }
}

void QucsSettingsDialog::slotAppFontDialog()
{
    bool ok;
    QFont tmpFont = QFontDialog::getFont(&ok, AppFont, this);
    if(ok)
    {
        AppFont = tmpFont;
        AppFontButton->setText(getFontDescription(AppFont));
    }
}

void QucsSettingsDialog::slotTextFontDialog()
{
    bool ok;
    QFont tmpFont = QFontDialog::getFont(&ok, TextFont, this);
    if(ok)
    {
        TextFont = tmpFont;
        TextFontButton->setText(getFontDescription(TextFont));
    }
}

// -----------------------------------------------------------
void QucsSettingsDialog::slotBGColorDialog()
{
    QColor c = QColorDialog::getColor(
                   BGColorButton->palette().color(BGColorButton->foregroundRole()),
                   this);
    if(c.isValid()) {
        QPalette p = BGColorButton->palette();
        p.setColor(BGColorButton->backgroundRole(), c);
        BGColorButton->setPalette(p);
    }
}

// -----------------------------------------------------------
void QucsSettingsDialog::slotGridColorDialog()
{
    QColor c = QColorDialog::getColor(
                   GridColorButton->palette().color(GridColorButton->foregroundRole()),
                   this);
    if(c.isValid()) {
        QPalette p = GridColorButton->palette();
        p.setColor(GridColorButton->backgroundRole(), c);
        GridColorButton->setPalette(p);
    }
}

// -----------------------------------------------------------
void QucsSettingsDialog::fillUserCategories(const QList<ContentCategory>& categories)
{
    contentUserCategories->setRowCount(0);
    for (const ContentCategory& c : categories) {
        const int row = contentUserCategories->rowCount();
        contentUserCategories->insertRow(row);
        contentUserCategories->setItem(row, 0, new QTableWidgetItem(c.name));
        contentUserCategories->setItem(row, 1, new QTableWidgetItem(c.patterns));
    }
}

void QucsSettingsDialog::slotAddContentCategory()
{
    // "New Category", "New Category 2", ...: a name no other has.
    QStringList taken;
    for (const int category : ProjectView::categories()) taken << ProjectView::categoryName(category);
    for (int row = 0; row < contentUserCategories->rowCount(); ++row)
        if (const QTableWidgetItem *name = contentUserCategories->item(row, 0)) taken << name->text().trimmed();
    QString name = tr("New Category");
    for (int n = 2; taken.contains(name, Qt::CaseInsensitive); ++n) name = tr("New Category %1").arg(n);
    const int row = contentUserCategories->rowCount();
    contentUserCategories->insertRow(row);
    contentUserCategories->setItem(row, 0, new QTableWidgetItem(name));
    contentUserCategories->setItem(row, 1, new QTableWidgetItem(QString()));
    contentUserCategories->setCurrentCell(row, 0);
    contentUserCategories->editItem(contentUserCategories->item(row, 0));
}

void QucsSettingsDialog::slotRemoveContentCategory()
{
    const int row = contentUserCategories->currentRow();
    if (row >= 0) contentUserCategories->removeRow(row);
}

void QucsSettingsDialog::moveContentCategory(int by)
{
    const int row = contentUserCategories->currentRow();
    const int to = row + by;
    if (row < 0 || to < 0 || to >= contentUserCategories->rowCount()) return;
    for (int column = 0; column < 2; ++column) {
        QTableWidgetItem *a = contentUserCategories->takeItem(row, column);
        QTableWidgetItem *b = contentUserCategories->takeItem(to, column);
        contentUserCategories->setItem(row, column, b);
        contentUserCategories->setItem(to, column, a);
    }
    contentUserCategories->setCurrentCell(to, contentUserCategories->currentColumn());
}

// -----------------------------------------------------------
void QucsSettingsDialog::slotRestoreContentPatterns()
{
    for (int category = 0; category < contentPatternEdits.size(); ++category) {
        contentPatternEdits[category]->setText(ProjectView::defaultPatterns(category));
        contentPatternEdits[category]->setCursorPosition(0);
    }
}

// -----------------------------------------------------------
void QucsSettingsDialog::slotDefaultValues()
{
    QPalette p;
    Font = QApplication::font();
    AppFont = QucsSettings.sysDefaultFont;
    TextFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    FontButton->setText(getFontDescription(Font));
    AppFontButton->setText(getFontDescription(AppFont));
    TextFontButton->setText(getFontDescription(TextFont));
    LargeFontSizeEdit->setText(QString::number(16.0));

    LanguageCombo->setCurrentIndex(0);

    p = BGColorButton->palette();
    p.setColor(BGColorButton->backgroundRole(), QColor(255,250,225));
    BGColorButton->setPalette(p);

    syntaxPage->restoreDefaults();

    undoNumEdit->setText("20");
    editorEdit->setText(QucsSettings.BinDir + "qucs");
    checkWiring->setChecked(false);
    allowFlexibleWires->setChecked(_settings::Get().itemDefault<bool>("AllowFlexibleWires"));
    contentAutoRefresh->setChecked(true);
    contentRefreshSeconds->setValue(3);
    contentFolderIcons->setChecked(false);
    slotRestoreContentPatterns();
    fillUserCategories({});   // (none of the user's: the defaults have none)
    anyFolderIsProject->setChecked(false);
    for (QCheckBox *box : {restoreWorkspace, restoreProject, restoreDocuments, restorePanels, restoreWindowGeometry,
                           reopenConversations})
        box->setChecked(true);
    showPinNames->setChecked(true);
    showPinDirections->setChecked(false);
    embedVerilogA->setChecked(_settings::Get().itemDefault<bool>("EmbedVerilogAInLibraries"));
    writeDocSettings->setChecked(_settings::Get().itemDefault<bool>("WriteTextDocSettings"));
    ThemeCombo->setCurrentIndex(ThemeCombo->findData(qucs_s::apptheme::System));
    paperFollowsTheme->setChecked(false);
    gridModeCombo->setCurrentIndex(gridModeCombo->findData(0));
    lockToolbarsCheck->setChecked(false);
    fileNameCapSpin->setValue(_settings::Get().itemDefault<int>("FileNameCap"));
    checkLoadFromFutureVersions->setChecked(false);
    checkAntiAliasing->setChecked(false);
    checkTextAntiAliasing->setChecked(true);
    checkFullTraceNames->setChecked(false);
}

void QucsSettingsDialog::slotForgetWorkspace()
{
    qucs_s::session::forget();
    showKeptWorkspace();
}

void QucsSettingsDialog::showKeptWorkspace()
{
    const qucs_s::session::Workspace kept = qucs_s::session::saved();
    const bool any = !kept.isEmpty() || !qucs_s::session::savedWindowState().isEmpty();
    keptWorkspaceLabel->setText(
        !any ? tr("Nothing is kept now.")
             : tr("Kept: %1%2.").arg(kept.summary(),
                                     kept.saved.isValid() ? tr(", at %1").arg(QLocale().toString(kept.saved, QLocale::ShortFormat))
                                                          : QString()));
    forgetWorkspaceButton->setEnabled(any);
}

void QucsSettingsDialog::slotTableClicked(int row, int col)
{
    Q_UNUSED(col);
    Input_Suffix->setText(fileTypesTableWidget->item(row,0)->text());
    Input_Program->setText(fileTypesTableWidget->item(row,1)->text());
}

// -----------------------------------------------------------
// The locations tab slots

void QucsSettingsDialog::slotHomeDirBrowse()
{
  QString d = QFileDialog::getExistingDirectory
    (this, tr("Select the home directory"),
     homeEdit->text(),
     QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

  if(!d.isEmpty())
    homeEdit->setText(d);
}

void QucsSettingsDialog::slotAdmsXmlDirBrowse()
{
  QString d = QFileDialog::getExistingDirectory
    (this, tr("Select the admsXml bin directory"),
     admsXmlEdit->text(),
     QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

  if(!d.isEmpty())
    admsXmlEdit->setText(d);
}

void QucsSettingsDialog::slotAscoDirBrowse()
{
  QString d = QFileDialog::getExistingDirectory
    (this, tr("Select the ASCO bin directory"),
     ascoEdit->text(),
     QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

  if(!d.isEmpty())
    ascoEdit->setText(d);
}

void QucsSettingsDialog::slotOctaveDirBrowse()
{
  QString d = QFileDialog::getOpenFileName(this, tr("Select the octave executable"),
                                           octaveEdit->text(), "All files (*)");

  if(!d.isEmpty())
    octaveEdit->setText(d);
}

void QucsSettingsDialog::slotOpenVAFDirBrowse()
{
  QString d = QFileDialog::getOpenFileName(this, tr("Select the OpenVAF executable"),
                                           OpenVAFEdit->text(), "All files (*)");

  if(!d.isEmpty())
    OpenVAFEdit->setText(d);
}

void QucsSettingsDialog::slotRFLayoutDirBrowse()
{
  QString d = QFileDialog::getOpenFileName(this, tr("Select the Qucs-RFLayout executable"),
                                           RFLayoutEdit->text(), "All files (*)");

  if(!d.isEmpty())
    RFLayoutEdit->setText(d);
}

void QucsSettingsDialog::slotPythonBrowse()
{
  QString d = QFileDialog::getOpenFileName(this, tr("Select the Python interpreter"),
                                           pythonEdit->text(), "All files (*)");

  if(!d.isEmpty())
    pythonEdit->setText(d);
}

QTableWidget *QucsSettingsDialog::newPathTable(QWidget *parent, const QString &header, const QString &name,
                                               const QString &accessible)
{
    // A second column for the buttons that remove a path.
    auto *table = new QTableWidget(parent);
    table->setObjectName(name);
    table->setAccessibleName(accessible);   // (Claude's key for it)
    table->setProperty("paths", true);      // (a list of folders: set_settings gives it whole)
    table->setColumnCount(2);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table->setColumnWidth(1, 36);
    table->setHorizontalHeaderItem(0, new QTableWidgetItem(header));
    table->setHorizontalHeaderItem(1, new QTableWidgetItem(QString()));
    // avoid drawing header text in bold when some data is selected
    table->horizontalHeader()->setSectionsClickable(false);
    table->verticalHeader()->hide();
    // allow multiple items to be selected
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    return table;
}

QStringList QucsSettingsDialog::chooseFolders(bool subfolders)
{
  QString d = QFileDialog::getExistingDirectory
    (this, tr("Select a directory"),
     QucsSettings.QucsWorkDir.canonicalPath(),
     QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

  if(d.isEmpty()){
    return {};   // user cancelled
  }
  if (!subfolders) return {d};

  // Collect all subdirectories first
  QStringList newPaths;
  newPaths.append(d);

  QDirIterator pathIter(d, QDirIterator::Subdirectories);
  while (pathIter.hasNext())
  {
    QString path = pathIter.next();
    QFileInfo pathfinfo = pathIter.fileInfo();

    if (pathfinfo.isDir() && !pathfinfo.isSymLink() &&
        pathIter.fileName() != "." && pathIter.fileName() != "..")
    {
      newPaths.append(QDir(path).canonicalPath());
    }
  }

  // Build confirmation dialog with a checkable, scrollable list
  QDialog confirmDialog(this);
  confirmDialog.setWindowTitle(tr("Add Path With Subfolders"));
  confirmDialog.setMinimumSize(500, 400);

  QVBoxLayout *layout = new QVBoxLayout(&confirmDialog);

  layout->addWidget(new QLabel(
      tr("Select the paths to add to the search list:"),
      &confirmDialog));

  // Select / deselect all checkbox
  QCheckBox *selectAllCheck = new QCheckBox(tr("Select / Deselect all"), &confirmDialog);
  selectAllCheck->setCheckState(Qt::Checked);
  layout->addWidget(selectAllCheck);

  // Path counter label
  QLabel *selectionCountLabel = new QLabel(
      tr("%1 of %1 paths selected").arg(newPaths.size()),
      &confirmDialog);
  layout->addWidget(selectionCountLabel);

  // Scrollable list with one checkbox per path
  QListWidget *pathList = new QListWidget(&confirmDialog);
  for (const QString &path : newPaths)
  {
    QListWidgetItem *item = new QListWidgetItem(path, pathList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
  }
  layout->addWidget(pathList);

  // Keep "Select all" checkbox in sync with individual items
  connect(selectAllCheck, &QCheckBox::stateChanged, [pathList](int state) {
    if (state == Qt::PartiallyChecked){
      return;
    }

    Qt::CheckState checkState = (state == Qt::Checked) ? Qt::Checked : Qt::Unchecked;

    for (int i = 0; i < pathList->count(); i++){
      pathList->item(i)->setCheckState(checkState);
    }
    });

  // Update "Select all" checkbox when individual items change
  connect(pathList, &QListWidget::itemChanged, [pathList, selectAllCheck, selectionCountLabel](QListWidgetItem *) {
    int checkedCount = 0;
    for (int i = 0; i < pathList->count(); i++) {
      if (pathList->item(i)->checkState() == Qt::Checked) {
        checkedCount++;
      }
    }

    selectionCountLabel->setText(
        tr("%1 of %2 paths selected").arg(checkedCount).arg(pathList->count()));

    // Block signals to avoid triggering stateChanged while we update it
    QSignalBlocker blocker(selectAllCheck);
    if (checkedCount == 0){
      selectAllCheck->setCheckState(Qt::Unchecked);
    } else if (checkedCount == pathList->count()){
      selectAllCheck->setCheckState(Qt::Checked);
    } else
      selectAllCheck->setCheckState(Qt::PartiallyChecked);
    });

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
      Qt::Horizontal, &confirmDialog);
  connect(buttons, SIGNAL(accepted()), &confirmDialog, SLOT(accept()));
  connect(buttons, SIGNAL(rejected()), &confirmDialog, SLOT(reject()));
  layout->addWidget(buttons);

  if (confirmDialog.exec() != QDialog::Accepted)
    return {};

  // Only the checked paths
  QStringList chosen;
  for (int i = 0; i < pathList->count(); i++){
    if (pathList->item(i)->checkState() == Qt::Checked){
      chosen.append(pathList->item(i)->text());
    }
  }
  return chosen;
}

bool QucsSettingsDialog::confirmClearAll(int count)
{
  // Dialog for user confirmation
  return count > 0
         && QMessageBox::question(this, tr("Clear All Paths"),
                                  tr("Are you sure you want to remove all %1 search paths?").arg(count),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                == QMessageBox::Yes;
}

void QucsSettingsDialog::slotAddPath()
{
  for (const QString &path : chooseFolders(false))
    if (!currentPaths.contains(path)) currentPaths.append(path);
  makePathTable();
}

void QucsSettingsDialog::slotAddPathWithSubFolders()
{
  for (const QString &path : chooseFolders(true))
    if (!currentPaths.contains(path)) currentPaths.append(path);
  makePathTable();
}

void QucsSettingsDialog::slotAddLibraryPath()
{
  for (const QString &path : chooseFolders(false))
    if (!currentLibraryPaths.contains(path)) currentLibraryPaths.append(path);
  makePathTable(libraryPathsTableWidget, &currentLibraryPaths);
}

void QucsSettingsDialog::slotAddLibraryPathWithSubFolders()
{
  for (const QString &path : chooseFolders(true))
    if (!currentLibraryPaths.contains(path)) currentLibraryPaths.append(path);
  makePathTable(libraryPathsTableWidget, &currentLibraryPaths);
}

void QucsSettingsDialog::slotClearAllLibraryPaths()
{
  if (!confirmClearAll(int(currentLibraryPaths.size()))) return;
  currentLibraryPaths.clear();
  makePathTable(libraryPathsTableWidget, &currentLibraryPaths);
}

void QucsSettingsDialog::setPathList(const QString &table, const QStringList &paths)
{
  QStringList unique;
  for (const QString &path : paths)
    if (!path.trimmed().isEmpty() && !unique.contains(path.trimmed())) unique << path.trimmed();
  if (table == QLatin1String("libraryPaths")) {
    currentLibraryPaths = unique;
    makePathTable(libraryPathsTableWidget, &currentLibraryPaths);
  } else if (table == QLatin1String("subcircuitPaths")) {
    currentPaths = unique;
    makePathTable();
  }
}

// makePathTable()
//
// Reconstructs the table containing the list of search paths
// in the locations tab
void QucsSettingsDialog::makePathTable()
{
  makePathTable(pathsTableWidget, &currentPaths);
}

void QucsSettingsDialog::makePathTable(QTableWidget *table, QStringList *paths)
{
  table->clearContents();
  table->setRowCount(0);

  for (const QString& pathstr : std::as_const(*paths))
  {
    int row = table->rowCount();
    table->setRowCount(row + 1);

    QTableWidgetItem *path = new QTableWidgetItem(pathstr);
    path->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    table->setItem(row, 0, path);

    // Button for removing the path
    QPushButton *removeButt = new QPushButton(tr("✕"), table);
    removeButt->setToolTip(tr("Remove this path"));
    removeButt->setStyleSheet("color: red;");

    connect(removeButt, &QPushButton::clicked, [this, table, paths, pathstr]() {
      paths->removeAll(pathstr);
      makePathTable(table, paths);
    });
    table->setCellWidget(row, 1, removeButt);
  }
}

void QucsSettingsDialog::slotClearAllPaths()
{
  if (!confirmClearAll(int(currentPaths.size()))) return;
  // Removes every entry from the search
  currentPaths.clear();
  makePathTable();
}
