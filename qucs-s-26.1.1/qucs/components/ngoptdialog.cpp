/*
 * ngoptdialog.cpp - the NgOpt component's dialog (see ngoptdialog.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngoptdialog.h"

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
#include <QRadioButton>
#include <QRegularExpressionValidator>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "component.h"
#include "ngsweep.h"
#include "misc.h"
#include "schematic.h"
#include "valuereading.h"

using namespace qucs_s::ngopt;

namespace {

enum KnobColumn { KnobKindColumn, KnobNameColumn, KnobInitColumn, KnobLoColumn, KnobHiColumn };
enum TargetColumn { TargetAnalysisColumn, TargetExpressionColumn, TargetValueColumn, TargetWeightColumn };

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

} // namespace

NgOptDialog::NgOptDialog(Component* component, Schematic* schematic)
    : QDialog(schematic), a_comp(component), a_doc(schematic)
{
    setWindowTitle(tr("Edit ngspice Optimization"));
    const Command c = Command::read(component);

    auto* tabs = new QTabWidget;

    // ... the optimizer ................................................
    auto* general = new QWidget;
    auto* form = new QFormLayout(general);
    a_name = new QLineEdit(component->Name);
    a_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[\\w_]+")), this));
    form->addRow(tr("Name:"), a_name);
    a_method = new QComboBox;
    for (const auto& m : methods()) a_method->addItem(m.second, m.first);
    a_method->setCurrentIndex(std::max(0, a_method->findData(c.method)));
    form->addRow(tr("Method:"), a_method);
    a_methodNote = new QLabel;
    a_methodNote->setWordWrap(true);
    form->addRow(QString(), a_methodNote);
    a_maxIter = new QLineEdit(c.maxIter);
    a_maxIter->setValidator(new QIntValidator(1, 1000000, this));
    a_maxIter->setPlaceholderText(tr("ngspice's default"));
    form->addRow(tr("Maximum iterations:"), a_maxIter);
    a_tol = new QLineEdit(c.tol);
    a_tol->setPlaceholderText(tr("ngspice's default (1e-6)"));
    form->addRow(tr("Tolerance:"), a_tol);
    a_size = new QLineEdit(c.size);
    a_size->setValidator(new QIntValidator(4, 100000, this));
    a_size->setPlaceholderText(tr("automatic: 10 + 4 per parameter"));
    form->addRow(tr("Population:"), a_size);
    a_seed = new QLineEdit(c.seed);
    a_seed->setValidator(new QIntValidator(0, 2147483647, this));
    a_seed->setPlaceholderText(tr("none: a new search every run"));
    form->addRow(tr("Seed:"), a_seed);
    a_verbose = new QCheckBox(tr("Print every iteration in the simulation console"));
    a_verbose->setChecked(c.verbose);
    form->addRow(QString(), a_verbose);
    tabs->addTab(general, tr("Optimizer"));

    // ... the parameters ...............................................
    auto* knobPage = new QWidget;
    auto* knobLayout = new QVBoxLayout(knobPage);
    a_knobs = new QTableWidget(0, 5);
    a_knobs->setHorizontalHeaderLabels({tr("Kind"), tr("Name"), tr("Initial"), tr("Min"), tr("Max")});
    a_knobs->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    a_knobs->horizontalHeader()->setSectionResizeMode(KnobKindColumn, QHeaderView::ResizeToContents);
    a_knobs->verticalHeader()->hide();
    knobLayout->addWidget(a_knobs);
    auto* knobButtons = new QHBoxLayout;
    auto* addKnobButton = new QPushButton(tr("Add"));
    auto* removeKnobButton = new QPushButton(tr("Remove"));
    auto* equationsButton = new QPushButton(tr("Add Equation Variables"));
    equationsButton->setToolTip(tr("A .param parameter for every number an equation or .PARAM component defines"));
    knobButtons->addWidget(addKnobButton);
    knobButtons->addWidget(removeKnobButton);
    knobButtons->addStretch();
    knobButtons->addWidget(equationsButton);
    knobLayout->addLayout(knobButtons);
    auto* knobNote = new QLabel(
        tr("<b>.param</b>: a variable an equation or .PARAM component defines (ngspice re-reads the "
           "circuit for each value). <b>Instance</b>: a device or its parameter as SPICE names it, "
           "<tt>R1</tt>, <tt>@m1[w]</tt> (changed in place). <b>Model</b>: a .model parameter, "
           "<tt>@dmod[is]</tt> (changed in place). The search runs between Min and Max."));
    knobNote->setWordWrap(true);
    knobLayout->addWidget(knobNote);
    tabs->addTab(knobPage, tr("Parameters"));
    connect(addKnobButton, &QPushButton::clicked, this, [this] { addKnob(); });
    connect(removeKnobButton, &QPushButton::clicked, this, &NgOptDialog::removeKnob);
    connect(equationsButton, &QPushButton::clicked, this, &NgOptDialog::addEquationVariables);

    // ... the objective ................................................
    auto* objectivePage = new QWidget;
    auto* objectiveLayout = new QVBoxLayout(objectivePage);
    a_minimizeMode = new QRadioButton(tr("Minimize an expression"));
    a_fitMode = new QRadioButton(tr("Fit targets (least squares)"));
    auto* modes = new QHBoxLayout;
    modes->addWidget(a_minimizeMode);
    modes->addWidget(a_fitMode);
    modes->addStretch();
    objectiveLayout->addLayout(modes);
    a_objective = new QStackedWidget;

    auto* minimizePage = new QWidget;
    auto* minimizeForm = new QFormLayout(minimizePage);
    a_analysis = analysisBox(c.analysis);
    minimizeForm->addRow(tr("After the analysis:"), a_analysis);
    a_minimize = new QLineEdit(c.minimize);
    a_minimize->setPlaceholderText(QStringLiteral("(mag(v(out))-0.5)^2"));
    minimizeForm->addRow(tr("minimize:"), a_minimize);
    auto* minimizeNote = new QLabel(
        tr("An ngspice expression over the analysis' results; its last value counts. A simulation "
           "component's name runs its analysis, or write an ngspice analysis command."));
    minimizeNote->setWordWrap(true);
    minimizeForm->addRow(minimizeNote);
    a_objective->addWidget(minimizePage);

    auto* fitPage = new QWidget;
    auto* fitLayout = new QVBoxLayout(fitPage);
    fitLayout->setContentsMargins(0, 0, 0, 0);
    a_targets = new QTableWidget(0, 4);
    a_targets->setHorizontalHeaderLabels({tr("Analysis"), tr("Expression"), tr("Value"), tr("Weight")});
    a_targets->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    a_targets->verticalHeader()->hide();
    fitLayout->addWidget(a_targets);
    auto* targetButtons = new QHBoxLayout;
    auto* addTargetButton = new QPushButton(tr("Add"));
    auto* removeTargetButton = new QPushButton(tr("Remove"));
    targetButtons->addWidget(addTargetButton);
    targetButtons->addWidget(removeTargetButton);
    targetButtons->addStretch();
    fitLayout->addLayout(targetButtons);
    auto* fitNote = new QLabel(
        tr("Each target is the last value of its expression after its analysis: use a one-point "
           "analysis (<tt>ac lin 1 1meg 1meg</tt>) or an index (<tt>v(out)[20]</tt>). The weight "
           "scales the residual: 1 when empty; the reciprocal of the value (0.05 for 20) makes the fit "
           "relative."));
    fitNote->setWordWrap(true);
    fitLayout->addWidget(fitNote);
    a_objective->addWidget(fitPage);
    objectiveLayout->addWidget(a_objective);
    tabs->addTab(objectivePage, tr("Objective"));
    connect(addTargetButton, &QPushButton::clicked, this, [this] { addTarget(); });
    connect(removeTargetButton, &QPushButton::clicked, this, &NgOptDialog::removeTarget);

    // ... the command and the buttons ..................................
    auto* all = new QVBoxLayout(this);
    all->addWidget(tabs);
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
    connect(ok, &QPushButton::clicked, this, &NgOptDialog::slotOK);
    connect(applyButton, &QPushButton::clicked, this, &NgOptDialog::slotApply);
    connect(cancel, &QPushButton::clicked, this, &NgOptDialog::slotCancel);
    ok->setDefault(true);

    for (const Knob& k : c.knobs) addKnob(k);
    for (const Target& t : c.targets) addTarget(t);
    const bool fit = c.leastSquares() && (!c.targets.isEmpty() || c.analysis.isEmpty());
    (fit ? a_fitMode : a_minimizeMode)->setChecked(true);

    for (QLineEdit* e : {a_name, a_maxIter, a_tol, a_size, a_seed, a_minimize})
        connect(e, &QLineEdit::textChanged, this, &NgOptDialog::updateForm);
    connect(a_method, &QComboBox::currentIndexChanged, this, &NgOptDialog::updateForm);
    connect(a_analysis, &QComboBox::currentTextChanged, this, &NgOptDialog::updateForm);
    connect(a_verbose, &QCheckBox::toggled, this, &NgOptDialog::updateForm);
    connect(a_minimizeMode, &QRadioButton::toggled, this, &NgOptDialog::updateForm);
    connect(a_knobs, &QTableWidget::itemChanged, this, &NgOptDialog::updateForm);
    connect(a_targets, &QTableWidget::itemChanged, this, &NgOptDialog::updateForm);
    updateForm();
    resize(720, 560);
}

QStringList NgOptDialog::simulations() const
{
    QStringList names;
    if (a_doc == nullptr) return names;
    for (Component* c : a_doc->a_DocComps)
        if (c->isSimulation && c != a_comp && c->Model != QLatin1String(".NGOPT") && c->Model != QLatin1String(".Opt")
            && c->Model != QLatin1String(".SW") && !qucs_s::ngsweep::isSweep(c))
            names << c->Name;
    return names;
}

QComboBox* NgOptDialog::analysisBox(const QString& text) const
{
    auto* box = new QComboBox;
    box->setEditable(true);
    box->addItems(simulations());
    box->setEditText(text);
    box->setToolTip(tr("A simulation component of the schematic, or an ngspice analysis command"));
    return box;
}

void NgOptDialog::addKnob(const Knob& knob)
{
    const QSignalBlocker block(a_knobs);
    const int row = a_knobs->rowCount();
    a_knobs->insertRow(row);
    auto* kind = new QComboBox;
    kind->addItem(tr(".param"), QStringLiteral("dparam"));
    kind->addItem(tr("Instance"), QStringLiteral("param"));
    kind->addItem(tr("Model"), QStringLiteral("mparam"));
    kind->setCurrentIndex(std::max(0, kind->findData(Knob::kindName(knob.kind))));
    connect(kind, &QComboBox::currentIndexChanged, this, &NgOptDialog::updateForm);
    a_knobs->setCellWidget(row, KnobKindColumn, kind);
    a_knobs->setItem(row, KnobNameColumn, new QTableWidgetItem(knob.name));
    a_knobs->setItem(row, KnobInitColumn, new QTableWidgetItem(knob.init));
    a_knobs->setItem(row, KnobLoColumn, new QTableWidgetItem(knob.lo));
    a_knobs->setItem(row, KnobHiColumn, new QTableWidgetItem(knob.hi));
    if (a_preview != nullptr) updateForm();
}

void NgOptDialog::removeKnob()
{
    const int row = a_knobs->currentRow();
    if (row < 0) return;
    a_knobs->removeRow(row);
    updateForm();
}

void NgOptDialog::addEquationVariables()
{
    if (a_doc == nullptr) return;
    QStringList known;
    for (int r = 0; r < a_knobs->rowCount(); ++r) known << cell(a_knobs, r, KnobNameColumn).toLower();
    for (Component* c : a_doc->a_DocComps) {
        if (c->isActive != COMP_IS_ACTIVE) continue;
        int count = 0;
        if (c->Model == QLatin1String("Eqn")) count = c->Props.size() - 1;   // the last one is "Export"
        else if (c->Model == QLatin1String("SpicePar") || c->Model == QLatin1String("SpGlobPar")) count = c->Props.size();
        for (int i = 0; i < count; ++i) {
            const Property* p = c->Props.at(i);
            const qucs_s::units::Reading r = qucs_s::units::read(p->Value);
            if (r.kind != qucs_s::units::Reading::Number || known.contains(p->Name.toLower())) continue;
            known << p->Name.toLower();
            Knob k;
            k.kind = KnobKind::Param;
            k.name = p->Name;
            k.init = p->Value;
            // A decade each way, on the side of zero the value is on.
            const double v = r.value;
            const double lo = v == 0 ? -1 : (v > 0 ? v / 10 : v * 10);
            const double hi = v == 0 ? 1 : (v > 0 ? v * 10 : v / 10);
            k.lo = misc::num2str(lo, -1, QString());
            k.hi = misc::num2str(hi, -1, QString());
            addKnob(k);
        }
    }
}

void NgOptDialog::addTarget(const Target& target)
{
    const QSignalBlocker block(a_targets);
    const int row = a_targets->rowCount();
    a_targets->insertRow(row);
    QString analysis = target.analysis;
    if (analysis.isEmpty() && row > 0) {
        if (auto* above = qobject_cast<QComboBox*>(a_targets->cellWidget(row - 1, TargetAnalysisColumn)))
            analysis = above->currentText();
    }
    QComboBox* box = analysisBox(analysis);
    connect(box, &QComboBox::currentTextChanged, this, &NgOptDialog::updateForm);
    a_targets->setCellWidget(row, TargetAnalysisColumn, box);
    a_targets->setItem(row, TargetExpressionColumn, new QTableWidgetItem(target.expression));
    a_targets->setItem(row, TargetValueColumn, new QTableWidgetItem(target.value));
    a_targets->setItem(row, TargetWeightColumn, new QTableWidgetItem(target.weight));
    if (a_preview != nullptr) updateForm();
}

void NgOptDialog::removeTarget()
{
    const int row = a_targets->currentRow();
    if (row < 0) return;
    a_targets->removeRow(row);
    updateForm();
}

Command NgOptDialog::command() const
{
    Command c;
    c.method = a_method->currentData().toString();
    c.maxIter = a_maxIter->text().trimmed();
    c.tol = a_tol->text().trimmed();
    c.size = a_size->text().trimmed();
    c.seed = a_seed->text().trimmed();
    c.verbose = a_verbose->isChecked();
    for (int r = 0; r < a_knobs->rowCount(); ++r) {
        Knob k;
        if (auto* kind = qobject_cast<QComboBox*>(a_knobs->cellWidget(r, KnobKindColumn)))
            k.kind = Knob::kindOf(kind->currentData().toString());
        k.name = cell(a_knobs, r, KnobNameColumn);
        k.init = cell(a_knobs, r, KnobInitColumn);
        k.lo = cell(a_knobs, r, KnobLoColumn);
        k.hi = cell(a_knobs, r, KnobHiColumn);
        if (!k.name.isEmpty()) c.knobs << k;
    }
    for (int r = 0; r < a_targets->rowCount(); ++r) {
        Target t;
        if (auto* box = qobject_cast<QComboBox*>(a_targets->cellWidget(r, TargetAnalysisColumn)))
            t.analysis = box->currentText().trimmed();
        t.expression = cell(a_targets, r, TargetExpressionColumn);
        t.value = cell(a_targets, r, TargetValueColumn);
        t.weight = cell(a_targets, r, TargetWeightColumn);
        if (!t.expression.isEmpty()) c.targets << t;
    }
    c.analysis = a_analysis->currentText().trimmed();
    // Fitting: no expression to minimize (the targets are kept either way).
    if (a_minimizeMode->isChecked()) c.minimize = a_minimize->text().trimmed();
    return c;
}

QString NgOptDialog::preview() const
{
    if (a_minimizeMode->isChecked() && a_minimize->text().trimmed().isEmpty())
        return tr("(not complete: no expression to minimize)");
    QString line, why;
    if (!commandLine(command(), a_doc, &line, &why)) return tr("(not complete: %1)").arg(why);
    return line;
}

void NgOptDialog::updateForm()
{
    const QString method = a_method->currentData().toString();
    static const QHash<QString, const char*> notes = {
        {"de", QT_TR_NOOP("A population of candidates built from differences of its members; finds the global "
                          "minimum of rugged objectives.")},
        {"pso", QT_TR_NOOP("A swarm pulled toward its members' and its own best points; global.")},
        {"sa", QT_TR_NOOP("A single walker that accepts uphill steps while hot; global, one simulation a step.")},
        {"nm", QT_TR_NOOP("A downhill simplex from the initial values; fast on smooth objectives, finds the "
                          "nearest minimum.")},
        {"lm", QT_TR_NOOP("Gradient least squares: the fastest for fitting targets on smooth responses; "
                          "needs targets.")},
    };
    a_methodNote->setText(tr(notes.value(method, "")));
    const bool population = method == QLatin1String("de") || method == QLatin1String("pso");
    a_size->setEnabled(population);
    a_seed->setEnabled(method != QLatin1String("nm") && method != QLatin1String("lm"));
    a_objective->setCurrentIndex(a_minimizeMode->isChecked() ? 0 : 1);
    a_preview->setPlainText(preview());
}

bool NgOptDialog::apply()
{
    const QString before = propsText(a_comp);
    const QString name = a_name->text().trimmed();
    if (!name.isEmpty() && name != a_comp->Name) {
        bool taken = false;
        if (a_doc != nullptr)
            for (Component* c : a_doc->a_DocComps) taken |= c != a_comp && c->Name == name;
        if (taken) {
            QMessageBox::warning(this, tr("Edit ngspice Optimization"), tr("The name %1 is in use.").arg(name));
            a_name->setText(a_comp->Name);
        } else {
            a_comp->Name = name;
        }
    }
    command().write(a_comp);
    const bool changed = propsText(a_comp) != before;
    a_changed |= changed;
    if (changed && a_doc != nullptr) a_doc->viewport()->repaint();
    return changed;
}

void NgOptDialog::slotOK()
{
    apply();
    done(a_changed ? QDialog::Accepted : QDialog::Rejected);
}

void NgOptDialog::slotApply()
{
    apply();
}

void NgOptDialog::slotCancel()
{
    done(a_changed ? QDialog::Accepted : QDialog::Rejected);   // Apply may have changed it
}
