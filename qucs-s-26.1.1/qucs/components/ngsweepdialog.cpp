/*
 * ngsweepdialog.cpp - the NgSweep component's dialog (see ngsweepdialog.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngsweepdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
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
#include "ngoptimize.h"
#include "schematic.h"

using namespace qucs_s::ngsweep;
using qucs_s::ngstats::Record;

namespace {

enum OuterColumn { OuterName, OuterType, OuterStart, OuterStop, OuterPoints, OuterValues, OuterColumns };

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

void fillTypes(QComboBox* box)
{
    box->addItem(QObject::tr("linear"), QStringLiteral("lin"));
    box->addItem(QObject::tr("logarithmic"), QStringLiteral("log"));
    box->addItem(QObject::tr("list"), QStringLiteral("list"));
}

void selectType(QComboBox* box, const QString& type)
{
    const int i = box->findData(type.trimmed().toLower());
    box->setCurrentIndex(i < 0 ? 0 : i);
}

} // namespace

NgSweepDialog::NgSweepDialog(Component* component, Schematic* schematic)
    : QDialog(schematic), a_comp(component), a_doc(schematic)
{
    setWindowTitle(tr("Edit ngspice Sweep"));
    const Sweep c = Sweep::read(component);

    a_tabs = new QTabWidget;

    // ... the analysis and the parameter .................................
    auto* general = new QWidget;
    auto* form = new QFormLayout(general);
    a_name = new QLineEdit(component->Name);
    a_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[\\w_]+")), this));
    form->addRow(tr("Name:"), a_name);
    a_analysis = new QComboBox;
    a_analysis->setEditable(true);
    a_analysis->addItems(simulations());
    a_analysis->setEditText(c.analysis);
    a_analysis->setToolTip(tr("A simulation component of the schematic, or an ngspice analysis command"));
    form->addRow(tr("Analysis:"), a_analysis);

    a_parameter = new QComboBox;
    a_parameter->setEditable(true);
    a_parameter->addItems(parameters());
    a_parameter->setEditText(c.knob.name);
    a_parameter->setToolTip(tr("What ngspice changes: a component (R1, V1), an instance parameter (@m1[w]), "
                               "a model parameter (@dmod[is]), a .param (an equation's variable), or temp"));
    form->addRow(tr("Parameter:"), a_parameter);
    a_type = new QComboBox;
    fillTypes(a_type);
    selectType(a_type, c.knob.type);
    form->addRow(tr("Sweep:"), a_type);
    a_start = new QLineEdit(c.knob.start);
    a_stop = new QLineEdit(c.knob.stop);
    a_points = new QLineEdit(c.knob.points);
    a_points->setValidator(new QIntValidator(1, kMaxRuns, this));
    a_values = new QLineEdit(c.knob.values);
    a_values->setPlaceholderText(tr("1k; 2.2k; 4.7k"));
    form->addRow(tr("Start:"), a_start);
    form->addRow(tr("Stop:"), a_stop);
    form->addRow(tr("Points:"), a_points);
    form->addRow(tr("Values:"), a_values);
    a_waveforms = new QCheckBox(tr("Keep every point's voltages and currents (ac, dc, tran): a curve for each value"));
    a_waveforms->setChecked(c.waveforms);
    form->addRow(QString(), a_waveforms);
    const QString prefix = component->Name.toLower();
    form->addRow(note(tr("ngspice changes the parameter to each value and runs the analysis. Every point's voltages "
                         "and currents go to the dataset as <tt>%1.v(out)</tt> against the analysis' scale and "
                         "<tt>%1.PARAMETER</tt>: a diagram draws a curve for each value (choose auto colors to tell "
                         "them apart). After an op, the voltages and currents against the parameter. A transient is "
                         "interpolated onto its step, so every point has the same times. Deactivate the simulation "
                         "the sweep runs if only its sweep is wanted.")
                          .arg(prefix)));
    a_tabs->addTab(general, tr("Sweep"));

    // ... the outer parameters ...........................................
    a_outer = new QTableWidget(0, OuterColumns);
    a_outer->setHorizontalHeaderLabels({tr("Parameter"), tr("Sweep"), tr("Start"), tr("Stop"), tr("Points"), tr("Values")});
    auto* addOuterButton = new QPushButton(tr("Add"));
    auto* removeOuterButton = new QPushButton(tr("Remove"));
    a_tabs->addTab(tablePage(a_outer, addOuterButton, removeOuterButton,
                             note(tr("Up to %1 more parameters, swept around the first: the analysis runs at every "
                                     "combination, and each gives a curve of its own.")
                                      .arg(kMaxKnobs - 1))),
                   tr("Outer Sweeps"));
    connect(addOuterButton, &QPushButton::clicked, this, [this] { addOuter(); });
    connect(removeOuterButton, &QPushButton::clicked, this, [this] {
        if (a_outer->currentRow() >= 0) a_outer->removeRow(a_outer->currentRow());
        updateForm();
    });

    // ... the values recorded ...........................................
    a_records = new QTableWidget(0, 2);
    a_records->setHorizontalHeaderLabels({tr("Name"), tr("Expression")});
    auto* addRecordButton = new QPushButton(tr("Add"));
    auto* removeRecordButton = new QPushButton(tr("Remove"));
    a_tabs->addTab(tablePage(a_records, addRecordButton, removeRecordButton,
                             note(tr("The last value of each expression after every point's analysis: "
                                     "<tt>maximum(db(v(out)))</tt>, an index <tt>v(out)[10]</tt>. In the dataset "
                                     "as <tt>%1.NAME</tt> against the parameter.")
                                      .arg(prefix))),
                   tr("Values"));
    connect(addRecordButton, &QPushButton::clicked, this, [this] { addRecord(); });
    connect(removeRecordButton, &QPushButton::clicked, this, [this] {
        if (a_records->currentRow() >= 0) a_records->removeRow(a_records->currentRow());
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
    connect(ok, &QPushButton::clicked, this, &NgSweepDialog::slotOK);
    connect(applyButton, &QPushButton::clicked, this, &NgSweepDialog::slotApply);
    connect(cancel, &QPushButton::clicked, this, &NgSweepDialog::slotCancel);
    ok->setDefault(true);

    for (const Knob& k : c.outer) addOuter(k);
    for (const Record& r : c.records) addRecord(r);

    for (QLineEdit* e : {a_name, a_start, a_stop, a_points, a_values})
        connect(e, &QLineEdit::textChanged, this, &NgSweepDialog::updateForm);
    connect(a_waveforms, &QCheckBox::toggled, this, &NgSweepDialog::updateForm);
    connect(a_analysis, &QComboBox::currentTextChanged, this, &NgSweepDialog::updateForm);
    connect(a_parameter, &QComboBox::currentTextChanged, this, &NgSweepDialog::updateForm);
    connect(a_type, &QComboBox::currentIndexChanged, this, &NgSweepDialog::updateForm);
    connect(a_outer, &QTableWidget::itemChanged, this, &NgSweepDialog::updateForm);
    connect(a_records, &QTableWidget::itemChanged, this, &NgSweepDialog::updateForm);
    updateForm();
    resize(720, 600);
}

QStringList NgSweepDialog::simulations() const
{
    QStringList names;
    if (a_doc == nullptr) return names;
    for (Component* c : a_doc->a_DocComps)
        if (c->isSimulation && c != a_comp && c->Model != QLatin1String(".NGOPT") && c->Model != QLatin1String(".Opt")
            && c->Model != QLatin1String(".SW") && !qucs_s::ngstats::isStatistics(c) && !isSweep(c))
            names << c->Name;
    return names;
}

QStringList NgSweepDialog::parameters() const
{
    QStringList names;
    if (a_doc == nullptr) return names;
    for (Component* c : a_doc->a_DocComps) {
        if (c->isSimulation || c->isActive != COMP_IS_ACTIVE) continue;
        if (c->Model == QLatin1String("Eqn") || c->Model == QLatin1String("SpicePar")
            || c->Model == QLatin1String("SpGlobPar")) {
            // The variables an equation or a .PARAM defines: .param knobs.
            const int count = c->Model == QLatin1String("Eqn") ? int(c->Props.size()) - 1 : int(c->Props.size());
            for (int i = 0; i < count; ++i)
                if (!names.contains(c->Props.at(i)->Name)) names << c->Props.at(i)->Name;
            continue;
        }
        if (c->Model == QLatin1String("GND") || c->Name.isEmpty() || c->Name == QLatin1String("*")) continue;
        names << c->Name;
    }
    names.sort(Qt::CaseInsensitive);
    names << QStringLiteral("temp");
    return names;
}

QComboBox* NgSweepDialog::typeBox(const QString& type)
{
    auto* box = new QComboBox;
    fillTypes(box);
    selectType(box, type);
    connect(box, &QComboBox::currentIndexChanged, this, &NgSweepDialog::updateForm);
    return box;
}

void NgSweepDialog::addOuter(const Knob& knob)
{
    if (a_outer->rowCount() >= kMaxKnobs - 1) return;
    const QSignalBlocker block(a_outer);
    const int row = a_outer->rowCount();
    a_outer->insertRow(row);
    a_outer->setItem(row, OuterName, new QTableWidgetItem(knob.name));
    a_outer->setCellWidget(row, OuterType, typeBox(knob.type.isEmpty() ? QStringLiteral("list") : knob.type));
    a_outer->setItem(row, OuterStart, new QTableWidgetItem(knob.start));
    a_outer->setItem(row, OuterStop, new QTableWidgetItem(knob.stop));
    a_outer->setItem(row, OuterPoints, new QTableWidgetItem(knob.points));
    a_outer->setItem(row, OuterValues, new QTableWidgetItem(knob.values));
    if (a_preview != nullptr) updateForm();
}

void NgSweepDialog::addRecord(const Record& record)
{
    const QSignalBlocker block(a_records);
    const int row = a_records->rowCount();
    a_records->insertRow(row);
    a_records->setItem(row, 0, new QTableWidgetItem(record.name));
    a_records->setItem(row, 1, new QTableWidgetItem(record.expression));
    if (a_preview != nullptr) updateForm();
}

Sweep NgSweepDialog::sweep() const
{
    Sweep c;
    c.analysis = a_analysis->currentText().trimmed();
    c.knob.name = a_parameter->currentText().trimmed();
    c.knob.type = a_type->currentData().toString();
    c.knob.start = a_start->text().trimmed();
    c.knob.stop = a_stop->text().trimmed();
    c.knob.points = a_points->text().trimmed();
    c.knob.values = a_values->text().trimmed();
    c.waveforms = a_waveforms->isChecked();
    for (int r = 0; r < a_outer->rowCount(); ++r) {
        Knob k;
        k.name = cell(a_outer, r, OuterName);
        if (auto* box = qobject_cast<QComboBox*>(a_outer->cellWidget(r, OuterType))) k.type = box->currentData().toString();
        k.start = cell(a_outer, r, OuterStart);
        k.stop = cell(a_outer, r, OuterStop);
        k.points = cell(a_outer, r, OuterPoints);
        k.values = cell(a_outer, r, OuterValues);
        if (!k.name.isEmpty() || !k.values.isEmpty() || !k.start.isEmpty()) c.outer << k;
    }
    for (int r = 0; r < a_records->rowCount(); ++r) {
        const Record record{cell(a_records, r, 0), cell(a_records, r, 1)};
        if (!record.name.isEmpty() || !record.expression.isEmpty()) c.records << record;
    }
    return c;
}

QString NgSweepDialog::preview() const
{
    QString line, why;
    if (!commandLine(sweep(), a_doc, QString(), &line, &why)) return tr("(not complete: %1)").arg(why);
    return line + QLatin1Char('\n') + tr("(and -output with the voltages and currents the simulations save)");
}

void NgSweepDialog::updateForm()
{
    const bool list = a_type->currentData().toString() == QLatin1String("list");
    a_start->setEnabled(!list);
    a_stop->setEnabled(!list);
    a_points->setEnabled(!list);
    a_values->setEnabled(list);
    for (int r = 0; r < a_outer->rowCount(); ++r) {
        auto* box = qobject_cast<QComboBox*>(a_outer->cellWidget(r, OuterType));
        const bool outerList = box != nullptr && box->currentData().toString() == QLatin1String("list");
        const QSignalBlocker block(a_outer);
        for (int column : {int(OuterStart), int(OuterStop), int(OuterPoints)})
            if (QTableWidgetItem* item = a_outer->item(r, column))
                item->setFlags(outerList ? item->flags() & ~Qt::ItemIsEnabled : item->flags() | Qt::ItemIsEnabled);
        if (QTableWidgetItem* item = a_outer->item(r, OuterValues))
            item->setFlags(outerList ? item->flags() | Qt::ItemIsEnabled : item->flags() & ~Qt::ItemIsEnabled);
    }
    const QString analysis = qucs_s::ngopt::analysisCommand(a_doc, a_analysis->currentText());
    a_waveforms->setEnabled(analysis.isEmpty() || waveformAnalysis(analysis));
    a_preview->setPlainText(preview());
}

bool NgSweepDialog::apply()
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
    sweep().write(a_comp);
    const bool changed = propsText(a_comp) != before;
    a_changed |= changed;
    if (changed && a_doc != nullptr) a_doc->viewport()->repaint();
    return changed;
}

void NgSweepDialog::slotOK()
{
    apply();
    done(a_changed ? QDialog::Accepted : QDialog::Rejected);
}

void NgSweepDialog::slotApply()
{
    apply();
}

void NgSweepDialog::slotCancel()
{
    done(a_changed ? QDialog::Accepted : QDialog::Rejected);   // Apply may have changed it
}
