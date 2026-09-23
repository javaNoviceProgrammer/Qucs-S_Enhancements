/*
 * ngstatisticsdialog.cpp - the NgMonteCarlo and NgCorners components'
 * dialog (see ngstatisticsdialog.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngstatisticsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "component.h"
#include "schematic.h"

using namespace qucs_s::ngstats;

namespace {

QString cell(const QTableWidget* table, int row, int column)
{
    const QTableWidgetItem* item = table->item(row, column);
    return item != nullptr ? item->text().trimmed() : QString();
}

QString propsText(const Component* c)
{
    QStringList s{c->Name};
    for (const Property* p : c->Props) s << p->Name + QLatin1Char('=') + p->Value;
    return s.join(QLatin1Char('\n'));
}

QLabel* note(const QString& text)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    return label;
}

// A table with Add and Remove under it, on a page of its own.
QWidget* tablePage(QTableWidget* table, QPushButton* add, QPushButton* remove, QLabel* text)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    layout->addWidget(table);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);
    layout->addWidget(text);
    return page;
}

} // namespace

NgStatisticsDialog::NgStatisticsDialog(Component* component, Schematic* schematic)
    : QDialog(schematic), a_comp(component), a_doc(schematic),
      a_corners(component->Model == QLatin1String(kCornersModel))
{
    setWindowTitle(a_corners ? tr("Edit ngspice Corners") : tr("Edit ngspice Monte Carlo"));
    const MonteCarlo mc = MonteCarlo::read(component);
    const Corners cr = Corners::read(component);

    a_tabs = new QTabWidget;

    // ... the analysis and the loop .......................................
    auto* general = new QWidget;
    auto* form = new QFormLayout(general);
    a_name = new QLineEdit(component->Name);
    a_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[\\w_]+")), this));
    form->addRow(tr("Name:"), a_name);
    a_analysis = new QComboBox;
    a_analysis->setEditable(true);
    a_analysis->addItems(simulations());
    a_analysis->setEditText(a_corners ? cr.analysis : mc.analysis);
    a_analysis->setToolTip(tr("A simulation component of the schematic, or an ngspice analysis command"));
    form->addRow(tr("Analysis:"), a_analysis);

    a_samples = new QLineEdit(a_corners ? cr.samples : mc.samples);
    a_samples->setValidator(new QIntValidator(a_corners ? 0 : 1, 100000000, this));
    a_seed = new QLineEdit(a_corners ? cr.seed : mc.seed);
    a_seed->setValidator(new QIntValidator(0, 2147483647, this));
    a_seed->setPlaceholderText(tr("1: the same samples every run"));
    a_modelStats = new QCheckBox(tr("Draw the Verilog-A models' declared statistics (.option osdimc)"));
    a_modelStats->setChecked(a_corners ? cr.modelStatistics : mc.modelStatistics);
    a_modelStats->setToolTip(tr("Parameters with (* std=..., dist=... *) attributes vary as well"));

    if (!a_corners) {
        form->addRow(tr("Samples:"), a_samples);
        form->addRow(tr("Seed:"), a_seed);
        a_lhs = new QCheckBox(tr("Latin hypercube sampling (a steadier estimate from fewer samples)"));
        a_lhs->setChecked(mc.lhs);
        form->addRow(QString(), a_lhs);
        form->addRow(QString(), a_modelStats);
        form->addRow(note(tr("Every sample draws the circuit's random values anew - agauss(), aunif(), gauss() "
                             "in a .PARAM or a component's value, and with the option above the Verilog-A models' "
                             "own - and runs the analysis. A transient is interpolated onto its step, so every "
                             "sample has the same points.")));
    } else {
        a_cornerList = new QLineEdit(cr.corners);
        a_cornerList->setPlaceholderText(tr("every corner the models declare"));
        a_cornerList->setToolTip(tr("Corner names separated by commas: ss, ff"));
        form->addRow(tr("Corners:"), a_cornerList);
        a_nominal = new QCheckBox(tr("The nominal (tt) first"));
        a_nominal->setChecked(cr.nominal);
        form->addRow(QString(), a_nominal);
        a_waveforms = new QCheckBox(tr("Keep the voltages and currents at every corner (ac, dc, tran)"));
        a_waveforms->setChecked(cr.waveforms);
        form->addRow(QString(), a_waveforms);
        auto* mcBox = new QGroupBox(tr("A Monte Carlo at every corner"));
        auto* mcForm = new QFormLayout(mcBox);
        a_samples->setPlaceholderText(tr("none"));
        mcForm->addRow(tr("Samples:"), a_samples);
        mcForm->addRow(tr("Seed:"), a_seed);
        mcForm->addRow(QString(), a_modelStats);
        mcForm->addRow(note(tr("The corner holds the process parameters; the mismatch draws go on. Each corner "
                               "gets the yield of the specs instead of values.")));
        form->addRow(mcBox);
        form->addRow(note(tr("The corners are the ones the loaded Verilog-A models declare on their parameters, "
                             "(* corner=\"ss=+10%, ff=-10%\" *). The values and waveforms go to the dataset "
                             "against the corner's number; the status log names them.")));
    }
    a_tabs->addTab(general, tr("Analysis"));

    // ... the values ..................................................
    a_records = new QTableWidget(0, 2);
    a_records->setHorizontalHeaderLabels({tr("Name"), tr("Expression")});
    auto* addRecordButton = new QPushButton(tr("Add"));
    auto* removeRecordButton = new QPushButton(tr("Remove"));
    const QString prefix = component->Name.toLower();
    QLabel* recordNote = a_corners
        ? note(tr("The last value of each expression after the analysis at every corner: "
                  "<tt>maximum(db(v(out)))</tt>, an index <tt>v(out)[10]</tt>, or a one-point analysis. "
                  "In the dataset as <tt>%1.NAME</tt> against <tt>%1.corner</tt>.").arg(prefix))
        : note(tr("Evaluated after every sample's analysis. A number (<tt>maximum(db(v(out)))</tt>, "
                  "<tt>@r1[resistance]</tt>) is one value per sample, in the dataset as <tt>%1.NAME</tt> against "
                  "<tt>%1.sample</tt> - a Histogram diagram shows its distribution; a waveform (<tt>db(v(out))</tt> of an ac analysis) is a family of curves, one per sample. "
                  "A complex value is recorded as its magnitude.").arg(prefix));
    a_tabs->addTab(tablePage(a_records, addRecordButton, removeRecordButton, recordNote), tr("Values"));
    connect(addRecordButton, &QPushButton::clicked, this, [this] { addRecord(); });
    connect(removeRecordButton, &QPushButton::clicked, this, [this] {
        if (a_records->currentRow() >= 0) a_records->removeRow(a_records->currentRow());
        updateForm();
    });

    // ... the specs ...................................................
    a_specs = new QTableWidget(0, 3);
    a_specs->setHorizontalHeaderLabels({tr("Metric"), tr("Min"), tr("Max")});
    auto* addSpecButton = new QPushButton(tr("Add"));
    auto* removeSpecButton = new QPushButton(tr("Remove"));
    QLabel* specNote = note(
        tr("A sample passes when every metric is within its limits (either may be empty); the yield and its 95% "
           "confidence interval go to the status log%1.")
            .arg(a_corners ? tr(", a corner's in the dataset as %1.yield").arg(prefix)
                           : tr(" and the dataset (%1.yield), each metric's values as %1.spec1, %1.spec2, ...")
                                 .arg(prefix)));
    a_tabs->addTab(tablePage(a_specs, addSpecButton, removeSpecButton, specNote), tr("Specs"));
    connect(addSpecButton, &QPushButton::clicked, this, [this] { addSpec(); });
    connect(removeSpecButton, &QPushButton::clicked, this, [this] {
        if (a_specs->currentRow() >= 0) a_specs->removeRow(a_specs->currentRow());
        updateForm();
    });

    // ... the command and the buttons ..................................
    auto* all = new QVBoxLayout(this);
    all->addWidget(a_tabs);
    all->addWidget(new QLabel(tr("ngspice command:")));
    a_preview = new QPlainTextEdit;
    a_preview->setReadOnly(true);
    a_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    a_preview->setMaximumHeight(90);
    all->addWidget(a_preview);
    auto* buttons = new QHBoxLayout;
    auto* ok = new QPushButton(tr("OK"));
    auto* applyButton = new QPushButton(tr("Apply"));
    auto* cancel = new QPushButton(tr("Cancel"));
    buttons->addStretch();
    buttons->addWidget(ok);
    buttons->addWidget(applyButton);
    buttons->addWidget(cancel);
    all->addLayout(buttons);
    connect(ok, &QPushButton::clicked, this, &NgStatisticsDialog::slotOK);
    connect(applyButton, &QPushButton::clicked, this, &NgStatisticsDialog::slotApply);
    connect(cancel, &QPushButton::clicked, this, &NgStatisticsDialog::slotCancel);
    ok->setDefault(true);

    for (const Record& r : a_corners ? cr.records : mc.records) addRecord(r);
    for (const Spec& s : a_corners ? cr.specs : mc.specs) addSpec(s);

    for (QLineEdit* e : {a_name, a_samples, a_seed}) connect(e, &QLineEdit::textChanged, this, &NgStatisticsDialog::updateForm);
    if (a_cornerList != nullptr) connect(a_cornerList, &QLineEdit::textChanged, this, &NgStatisticsDialog::updateForm);
    for (QCheckBox* b : {a_lhs, a_modelStats, a_nominal, a_waveforms})
        if (b != nullptr) connect(b, &QCheckBox::toggled, this, &NgStatisticsDialog::updateForm);
    connect(a_analysis, &QComboBox::currentTextChanged, this, &NgStatisticsDialog::updateForm);
    connect(a_records, &QTableWidget::itemChanged, this, &NgStatisticsDialog::updateForm);
    connect(a_specs, &QTableWidget::itemChanged, this, &NgStatisticsDialog::updateForm);
    updateForm();
    resize(700, a_corners ? 640 : 540);
}

QStringList NgStatisticsDialog::simulations() const
{
    QStringList names;
    if (a_doc == nullptr) return names;
    for (Component* c : a_doc->a_DocComps)
        if (c->isSimulation && c != a_comp && c->Model != QLatin1String(".NGOPT") && c->Model != QLatin1String(".Opt")
            && c->Model != QLatin1String(".SW") && !isStatistics(c))
            names << c->Name;
    return names;
}

void NgStatisticsDialog::addRecord(const Record& record)
{
    const QSignalBlocker block(a_records);
    const int row = a_records->rowCount();
    a_records->insertRow(row);
    a_records->setItem(row, 0, new QTableWidgetItem(record.name));
    a_records->setItem(row, 1, new QTableWidgetItem(record.expression));
    if (a_preview != nullptr) updateForm();
}

void NgStatisticsDialog::addSpec(const Spec& spec)
{
    const QSignalBlocker block(a_specs);
    const int row = a_specs->rowCount();
    a_specs->insertRow(row);
    a_specs->setItem(row, 0, new QTableWidgetItem(spec.expression));
    a_specs->setItem(row, 1, new QTableWidgetItem(spec.min));
    a_specs->setItem(row, 2, new QTableWidgetItem(spec.max));
    if (a_preview != nullptr) updateForm();
}

MonteCarlo NgStatisticsDialog::monteCarlo() const
{
    MonteCarlo c;
    c.samples = a_samples->text().trimmed();
    c.seed = a_seed->text().trimmed();
    c.lhs = a_lhs != nullptr && a_lhs->isChecked();
    c.modelStatistics = a_modelStats->isChecked();
    c.analysis = a_analysis->currentText().trimmed();
    for (int r = 0; r < a_records->rowCount(); ++r) {
        const Record record{cell(a_records, r, 0), cell(a_records, r, 1)};
        if (!record.name.isEmpty() || !record.expression.isEmpty()) c.records << record;
    }
    for (int r = 0; r < a_specs->rowCount(); ++r) {
        const Spec spec{cell(a_specs, r, 0), cell(a_specs, r, 1), cell(a_specs, r, 2)};
        if (!spec.expression.isEmpty()) c.specs << spec;
    }
    return c;
}

Corners NgStatisticsDialog::cornersCommand() const
{
    const MonteCarlo mc = monteCarlo();
    Corners c;
    c.analysis = mc.analysis;
    c.corners = a_cornerList != nullptr ? a_cornerList->text().trimmed() : QString();
    c.nominal = a_nominal == nullptr || a_nominal->isChecked();
    c.waveforms = a_waveforms == nullptr || a_waveforms->isChecked();
    c.records = mc.records;
    c.samples = mc.samples;
    c.seed = mc.seed;
    c.modelStatistics = mc.modelStatistics;
    c.specs = mc.specs;
    return c;
}

QString NgStatisticsDialog::preview() const
{
    QString line, why;
    const bool ok = a_corners ? commandLine(cornersCommand(), a_doc, &line, &why)
                              : commandLine(monteCarlo(), a_doc, &line, &why);
    return ok ? line : tr("(not complete: %1)").arg(why);
}

void NgStatisticsDialog::updateForm()
{
    if (a_corners) {
        // A Monte Carlo at every corner judges specs; the plain run records
        // values and waveforms.
        const bool mc = cornersCommand().monteCarlo();
        a_tabs->setTabEnabled(1, !mc);
        a_tabs->setTabEnabled(2, mc);
        a_waveforms->setEnabled(!mc);
        a_seed->setEnabled(mc);
        a_modelStats->setEnabled(mc);
    }
    a_preview->setPlainText(preview());
}

bool NgStatisticsDialog::apply()
{
    const QString before = propsText(a_comp);
    const QString name = a_name->text().trimmed();
    if (!name.isEmpty() && name != a_comp->Name) {
        bool taken = false;
        if (a_doc != nullptr)
            for (Component* c : a_doc->a_DocComps) taken |= c != a_comp && c->Name == name;
        if (taken) {
            QMessageBox::warning(this, windowTitle(), tr("The name %1 is in use.").arg(name));
            a_name->setText(a_comp->Name);
        } else {
            a_comp->Name = name;
        }
    }
    if (a_corners) cornersCommand().write(a_comp);
    else monteCarlo().write(a_comp);
    const bool changed = propsText(a_comp) != before;
    a_changed |= changed;
    if (changed && a_doc != nullptr) a_doc->viewport()->repaint();
    return changed;
}

void NgStatisticsDialog::slotOK()
{
    apply();
    done(a_changed ? QDialog::Accepted : QDialog::Rejected);
}

void NgStatisticsDialog::slotApply()
{
    apply();
}

void NgStatisticsDialog::slotCancel()
{
    done(a_changed ? QDialog::Accepted : QDialog::Rejected);   // Apply may have changed it
}
