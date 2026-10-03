/***************************************************************************
                           simsettingsdialog.cpp
                             ----------------
    begin                : Tue Apr 21 2015
    copyright            : (C) 2015 by Vadim Kuznetsov
    email                : ra3xdh@gmail.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "simsettingsdialog.h"
#include "ngspice.h"
#include "main.h"
#include "settings.h"

SimSettingsDialog::SimSettingsDialog(QWidget *parent) :
    QDialog(parent),
    a_lblXyce(new QLabel(tr("Xyce executable location"))),
    a_lblNgspice(new QLabel(tr("Ngspice executable location"))),
    a_lblSpiceOpus(new QLabel(tr("SpiceOpus executable location"))),
    a_lblQucsator(new QLabel(tr("Qucsator executable location"))),
    a_lblNgspiceSimParam(new QLabel(tr("Ngspice CLI parameters"))),
    a_lblXyceSimParam(new QLabel(tr("Xyce CLI parameters"))),
    a_lblSpopusSimParam(new QLabel(tr("SpiceOpus CLI parameters"))),
    a_lblCompatMode(new QLabel(tr("Ngspice compatibility mode"))),
    a_cbxCompatMode(new QComboBox),
    a_edtNgspice(new QLineEdit(QucsSettings.NgspiceExecutable)),
    a_edtSpiceOpus(new QLineEdit(QucsSettings.SpiceOpusExecutable)),
    a_edtXyce(new QLineEdit(QucsSettings.XyceExecutable)),
    a_edtQucsator(new QLineEdit(QucsSettings.Qucsator)),
    a_edtNgspiceSimParam(new QLineEdit()),
    a_edtXyceSimParam(new QLineEdit()),
    a_edtSpopusSimParam(new QLineEdit()),
    a_btnOK(new QPushButton(tr("Apply changes"))),
    a_btnCancel(new QPushButton(tr("Cancel"))),
    a_btnSetNgspice(new QPushButton(tr("Select ..."))),
    a_btnSetSpOpus(new QPushButton(tr("Select ..."))),
    a_btnSetXyce(new QPushButton(tr("Select ..."))),
    a_btnSetQucsator(new QPushButton(tr("Select ..."))),
    a_rbConsoleDock(new QRadioButton(tr("in the Simulation dock of the main window"))),
    a_rbConsoleWindow(new QRadioButton(tr("in a separate window"))),
    a_rbConsoleLegacy(new QRadioButton(tr("in the legacy window, which blocks the application until closed")))
{
    qDebug()<<QucsSettings.DefaultSimulator;

    a_edtNgspiceSimParam->setText(_settings::Get().item<QString>("NgspiceParams"));
    a_edtXyceSimParam->setText(_settings::Get().item<QString>("XyceParams"));
    a_edtSpopusSimParam->setText(_settings::Get().item<QString>("SpopusParams"));


    connect(a_btnOK,SIGNAL(clicked()),this,SLOT(slotApply()));
    connect(a_btnCancel,SIGNAL(clicked()),this,SLOT(reject()));

    connect(a_btnSetNgspice,SIGNAL(clicked()),this,SLOT(slotSetNgspice()));
    connect(a_btnSetXyce,SIGNAL(clicked()),this,SLOT(slotSetXyce()));
    connect(a_btnSetSpOpus,SIGNAL(clicked()),this,SLOT(slotSetSpiceOpus()));
    connect(a_btnSetQucsator,SIGNAL(clicked()),this,SLOT(slotSetQucsator()));

    QStringList lst_modes;
    lst_modes<<"Default"<<"LTspice"<<"HSPICE"<<"Spice3";
    a_cbxCompatMode->addItems(lst_modes);
    auto compat_mode = _settings::Get().item<int>("NgspiceCompatMode");
    a_cbxCompatMode->setCurrentIndex(compat_mode);

    QVBoxLayout *top = new QVBoxLayout;
    QTabWidget *tabs = new QTabWidget(this);
    top->addWidget(tabs);

    // Tab 1: the simulators.
    QWidget *simulatorsTab = new QWidget(tabs);
    QVBoxLayout *simulatorsLayout = new QVBoxLayout(simulatorsTab);
    tabs->addTab(simulatorsTab, tr("Simulators"));

    QGroupBox *gbp1 = new QGroupBox(this);
    gbp1->setTitle(tr("SPICE settings"));
    QVBoxLayout *top2 = new QVBoxLayout;
    top2->addWidget(a_lblNgspice);
    QHBoxLayout *h1 = new QHBoxLayout;
    h1->addWidget(a_edtNgspice,3);
    h1->addWidget(a_btnSetNgspice,1);
    top2->addLayout(h1);

    QHBoxLayout *h4 = new QHBoxLayout;
    h4->addWidget(a_lblCompatMode);
    h4->addWidget(a_cbxCompatMode);
    top2->addLayout(h4);
    top2->addWidget(a_lblNgspiceSimParam);
    top2->addWidget(a_edtNgspiceSimParam);

    top2->addWidget(a_lblXyce);
    QHBoxLayout *h2 = new QHBoxLayout;
    h2->addWidget(a_edtXyce,3);
    h2->addWidget(a_btnSetXyce,1);
    top2->addLayout(h2);
    top2->addWidget(a_lblXyceSimParam);
    top2->addWidget(a_edtXyceSimParam);

    top2->addWidget(a_lblSpiceOpus);
    QHBoxLayout *h7 = new QHBoxLayout;
    h7->addWidget(a_edtSpiceOpus,3);
    h7->addWidget(a_btnSetSpOpus,1);
    top2->addLayout(h7);
    top2->addWidget(a_lblSpopusSimParam);
    top2->addWidget(a_edtSpopusSimParam);

    gbp1->setLayout(top2);
    simulatorsLayout->addWidget(gbp1);

    QGroupBox *gbp2 = new QGroupBox;
    gbp2->setTitle(tr("Qucsator settings"));
    QVBoxLayout *top3 = new QVBoxLayout;
    top3->addWidget(a_lblQucsator);
    QHBoxLayout *h9 = new QHBoxLayout;
    h9->addWidget(a_edtQucsator,3);
    h9->addWidget(a_btnSetQucsator,1);
    top3->addLayout(h9);
    gbp2->setLayout(top3);

    simulatorsLayout->addWidget(gbp2);

    // What a simulation insists on before it starts.
    QGroupBox *gbChecks = new QGroupBox(tr("Before a simulation"), simulatorsTab);
    QVBoxLayout *checksLayout = new QVBoxLayout;
    a_cbRequireGround = new QCheckBox(tr("A schematic must have a ground symbol"), gbChecks);
    a_cbRequireGround->setObjectName(QStringLiteral("cbRequireGround"));
    a_cbRequireGround->setChecked(QucsSettings.RequireGround);
    a_cbRequireGround->setToolTip(
        tr("On: a circuit without a ground symbol is not simulated, and Check Schematic calls it an error. "
           "Off: it is simulated as it is - node 0 then comes from a net named 0 or from a component "
           "(a SPICE netlist, a library part) that brings it - and Check Schematic says nothing of it."));
    checksLayout->addWidget(a_cbRequireGround);
    a_cbCheckCommands = new QCheckBox(tr("Warn of commands a simulation runs besides the simulator"), gbChecks);
    a_cbCheckCommands->setObjectName(QStringLiteral("cbCheckCommands"));
    a_cbCheckCommands->setChecked(QucsSettings.CheckCommands);
    a_cbCheckCommands->setToolTip(
        tr("On: Check Schematic warns of each command a simulation of the schematic runs with your rights - a System "
           "command part, ngspice's shell in a custom simulation, Nutmeg or .spiceinit text, the Octave script run "
           "after it - and Claude's tools do not simulate such a schematic unless asked to run them. "
           "Off: they run as they are, unsaid."));
    checksLayout->addWidget(a_cbCheckCommands);
    gbChecks->setLayout(checksLayout);
    simulatorsLayout->addWidget(gbChecks);
    simulatorsLayout->addStretch(1);

    // Tab 2: the simulation console - a dock, or the classic window.
    QWidget *consoleTab = new QWidget(tabs);
    QVBoxLayout *consoleLayout = new QVBoxLayout(consoleTab);
    tabs->addTab(consoleTab, tr("Simulation console"));

    QGroupBox *gbp3 = new QGroupBox(tr("Show the simulator's output"), consoleTab);
    QVBoxLayout *consoleChoice = new QVBoxLayout;
    consoleChoice->addWidget(a_rbConsoleDock);
    consoleChoice->addWidget(a_rbConsoleWindow);
    consoleChoice->addWidget(a_rbConsoleLegacy);
    gbp3->setLayout(consoleChoice);
    consoleLayout->addWidget(gbp3);
    a_rbConsoleDock->setObjectName(QStringLiteral("rbConsoleDock"));
    a_rbConsoleWindow->setObjectName(QStringLiteral("rbConsoleWindow"));
    a_rbConsoleLegacy->setObjectName(QStringLiteral("rbConsoleLegacy"));
    switch (QucsSettings.SimulationConsoleHost) {
    case tQucsSettings::SimConsoleWindow:       a_rbConsoleWindow->setChecked(true); break;
    case tQucsSettings::SimConsoleLegacyWindow: a_rbConsoleLegacy->setChecked(true); break;
    default:                                    a_rbConsoleDock->setChecked(true);   break;
    }
    QLabel *consoleNote = new QLabel(
        tr("The dock shares the bottom of the main window with the build messages "
           "and can be shown or hidden with View > Simulation Console. "
           "The separate window shows the same console; the simulation runs in the "
           "background and the schematic stays usable. The legacy window is the "
           "simulation dialog of earlier versions: it opens with every simulation, "
           "and closing it stops a simulation still running."), consoleTab);
    consoleNote->setWordWrap(true);
    consoleLayout->addWidget(consoleNote);
    consoleLayout->addStretch(1);

    // Tab 3: what goes into the netlist.
    QWidget *netlistTab = new QWidget(tabs);
    QVBoxLayout *netlistLayout = new QVBoxLayout(netlistTab);
    tabs->addTab(netlistTab, tr("Netlist"));
    QGroupBox *gbNgspice = new QGroupBox(tr("ngspice netlist"), netlistTab);
    QVBoxLayout *ngspiceLayout = new QVBoxLayout;
    a_cbMathFuncs = new QCheckBox(tr("Include ngspice_mathfunc.inc (limexp, step, stp)"), gbNgspice);
    a_cbMathFuncs->setObjectName(QStringLiteral("cbNgspiceMathFuncs"));
    a_cbMathFuncs->setChecked(QucsSettings.NgspiceMathFuncs);
    ngspiceLayout->addWidget(a_cbMathFuncs);
    QLabel *mathFuncsNote = new QLabel(
        tr("On, each ngspice netlist begins with an .INCLUDE of this file of the installation. It defines limexp(x), step(x) "
           "and stp(x): functions of expressions written for Qucsator, which ngspice has not. Off, the line is left out: the "
           "netlist names no file of the installation, and an expression that uses them fails under ngspice. (Never for "
           "SPICE OPUS.)"), gbNgspice);
    mathFuncsNote->setWordWrap(true);
    ngspiceLayout->addWidget(mathFuncsNote);
    QString mathFuncs;
    const bool mathFuncsThere = Ngspice::findMathFuncInc(mathFuncs);
    QLabel *mathFuncsFile = new QLabel(gbNgspice);
    mathFuncsFile->setObjectName(QStringLiteral("lblNgspiceMathFuncsFile"));
    mathFuncsFile->setText(mathFuncsThere
                               ? fontMetrics().elidedText(QDir::toNativeSeparators(mathFuncs), Qt::ElideMiddle, 400)
                               : tr("Not in this installation: the line is left out either way."));
    mathFuncsFile->setToolTip(QDir::toNativeSeparators(mathFuncs));
    mathFuncsFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ngspiceLayout->addWidget(mathFuncsFile);
    a_cbMathFuncs->setToolTip(mathFuncsNote->text());
    gbNgspice->setLayout(ngspiceLayout);
    netlistLayout->addWidget(gbNgspice);
    netlistLayout->addStretch(1);

    // Tab 4: how a run's results are kept.
    QWidget *resultsTab = new QWidget(tabs);
    QVBoxLayout *resultsLayout = new QVBoxLayout(resultsTab);
    tabs->addTab(resultsTab, tr("Results"));
    QGroupBox *gbDataset = new QGroupBox(tr("Datasets of ngspice and Xyce"), resultsTab);
    QVBoxLayout *datasetLayout = new QVBoxLayout;
    a_cbDatasetBinary = new QCheckBox(tr("Keep large results binary"), gbDataset);
    a_cbDatasetBinary->setObjectName(QStringLiteral("cbDatasetBinary"));
    a_cbDatasetBinary->setChecked(QucsSettings.DatasetBinary);
    datasetLayout->addWidget(a_cbDatasetBinary);
    QHBoxLayout *limitRow = new QHBoxLayout;
    QLabel *limitLabel = new QLabel(tr("Binary above:"), gbDataset);
    a_sbDatasetTextLimit = new QSpinBox(gbDataset);
    a_sbDatasetTextLimit->setObjectName(QStringLiteral("sbDatasetTextLimit"));
    a_sbDatasetTextLimit->setRange(0, 1000000);
    a_sbDatasetTextLimit->setSuffix(tr(" MB"));
    a_sbDatasetTextLimit->setValue(QucsSettings.DatasetTextLimitMB);
    a_sbDatasetTextLimit->setToolTip(tr("The size of the simulator's output above which its dataset is binary (0: every run's)"));
    limitLabel->setBuddy(a_sbDatasetTextLimit);
    limitRow->addWidget(limitLabel);
    limitRow->addWidget(a_sbDatasetTextLimit);
    limitRow->addStretch(1);
    datasetLayout->addLayout(limitRow);
    QLabel *binaryNote = new QLabel(
        tr("A run whose simulator output is larger keeps its dataset binary: the simulator's numbers as they are, every "
           "digit, in less than half the room of text, written in about the time it takes to copy them, and a variable read "
           "without the others - and the simulator's raw file is not kept beside it. Smaller runs, and every run with this "
           "off, are text, as before. A schematic that runs an Octave script after the simulation keeps text, which the "
           "script reads. The Content panel's menu has Save as Text… for a binary dataset."), gbDataset);
    binaryNote->setWordWrap(true);
    datasetLayout->addWidget(binaryNote);
    a_cbDatasetBinary->setToolTip(binaryNote->text());
    gbDataset->setLayout(datasetLayout);
    resultsLayout->addWidget(gbDataset);
    resultsLayout->addStretch(1);

    QHBoxLayout *h3 = new QHBoxLayout;
    h3->addWidget(a_btnOK);
    h3->addWidget(a_btnCancel);
    h3->addStretch(2);
    top->addLayout(h3);

    this->setLayout(top);
    this->setFixedWidth(500);
    this->setWindowTitle(tr("Setup simulators executable location"));

}


void SimSettingsDialog::slotApply()
{
    QucsSettings.NgspiceExecutable = a_edtNgspice->text();
    QucsSettings.XyceExecutable = a_edtXyce->text();
    QucsSettings.SpiceOpusExecutable = a_edtSpiceOpus->text();
    QucsSettings.Qucsator = a_edtQucsator->text();
    settingsManager& qs = _settings::Get();
    qs.setItem<int>("NgspiceCompatMode", a_cbxCompatMode->currentIndex());
    qs.setItem<QString>("NgspiceParams", a_edtNgspiceSimParam->text());
    qs.setItem<QString>("XyceParams", a_edtXyceSimParam->text());
    qs.setItem<QString>("SpopusParams", a_edtSpopusSimParam->text());
    QucsSettings.RequireGround = a_cbRequireGround->isChecked();
    QucsSettings.CheckCommands = a_cbCheckCommands->isChecked();
    QucsSettings.NgspiceMathFuncs = a_cbMathFuncs->isChecked();
    QucsSettings.DatasetBinary = a_cbDatasetBinary->isChecked();
    QucsSettings.DatasetTextLimitMB = a_sbDatasetTextLimit->value();
    QucsSettings.SimulationConsoleHost = a_rbConsoleLegacy->isChecked() ? tQucsSettings::SimConsoleLegacyWindow
                                       : a_rbConsoleWindow->isChecked() ? tQucsSettings::SimConsoleWindow
                                                                        : tQucsSettings::SimConsoleDock;
    accept();
    saveApplSettings();
  }

void SimSettingsDialog::slotCancel()
{
    reject();
}

void SimSettingsDialog::slotSetNgspice()
{
    QString s = QFileDialog::getOpenFileName(this,tr("Select Ngspice executable location"),a_edtNgspice->text(),"All files (*)");
    if (!s.isEmpty()) {
        a_edtNgspice->setText(s);
    }
}

void SimSettingsDialog::slotSetXyce()
{
    QString s = QFileDialog::getOpenFileName(this,tr("Select Xyce executable location"),a_edtXyce->text(),"All files (*)");
    if (!s.isEmpty()) {
        a_edtXyce->setText(s);
    }
}

void SimSettingsDialog::slotSetXycePar() // TODO ZERGUD
{

}

void SimSettingsDialog::slotSetSpiceOpus()
{
    QString s = QFileDialog::getOpenFileName(this,tr("Select SpiceOpus executable location"),a_edtSpiceOpus->text(),"All files (*)");
    if (!s.isEmpty()) {
        a_edtSpiceOpus->setText(s);
    }
}

void SimSettingsDialog::slotSetQucsator()
{
    QString s = QFileDialog::getOpenFileName(this,tr("Select Qucsator executable location"),a_edtQucsator->text(),"All files (*)");
    if (!s.isEmpty()) {
        a_edtQucsator->setText(s);
    }
}
