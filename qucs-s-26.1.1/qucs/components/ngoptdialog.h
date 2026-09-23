/*
 * ngoptdialog.h - the NgOpt component's dialog: ngspice's optimize command
 * written with a form - the method and its settings, the parameters, the
 * expression to minimize or the targets to fit - and the command it makes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef NGOPTDIALOG_H
#define NGOPTDIALOG_H

#include <QDialog>

#include "ngoptimize.h"

class Component;
class Schematic;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QTableWidget;

class NgOptDialog : public QDialog
{
    Q_OBJECT
public:
    NgOptDialog(Component* component, Schematic* schematic);

    /// What the form says now.
    qucs_s::ngopt::Command command() const;
    /// The optimize line the form makes, or why there is none yet.
    QString preview() const;

    // The form, for the tests.
    QLineEdit* nameEdit() const { return a_name; }
    QComboBox* methodCombo() const { return a_method; }
    QTableWidget* knobTable() const { return a_knobs; }
    QTableWidget* targetTable() const { return a_targets; }
    QRadioButton* minimizeButton() const { return a_minimizeMode; }
    QRadioButton* fitButton() const { return a_fitMode; }
    QComboBox* analysisCombo() const { return a_analysis; }
    QLineEdit* minimizeEdit() const { return a_minimize; }
    QLineEdit* maxIterEdit() const { return a_maxIter; }
    QLineEdit* sizeEdit() const { return a_size; }
    QLineEdit* seedEdit() const { return a_seed; }

public slots:
    void addKnob(const qucs_s::ngopt::Knob& knob = qucs_s::ngopt::Knob());
    void addTarget(const qucs_s::ngopt::Target& target = qucs_s::ngopt::Target());
    /// A .param knob for every number an equation or .PARAM component
    /// defines that is not a knob yet, over a decade each way.
    void addEquationVariables();

private slots:
    void slotOK();
    void slotApply();
    void slotCancel();
    void removeKnob();
    void removeTarget();
    void updateForm();

private:
    bool apply();
    QComboBox* analysisBox(const QString& text) const;
    QStringList simulations() const;

    Component* a_comp;
    Schematic* a_doc;
    bool a_changed = false;

    QLineEdit* a_name = nullptr;
    QComboBox* a_method = nullptr;
    QLabel* a_methodNote = nullptr;
    QLineEdit* a_maxIter = nullptr;
    QLineEdit* a_tol = nullptr;
    QLineEdit* a_size = nullptr;
    QLineEdit* a_seed = nullptr;
    QCheckBox* a_verbose = nullptr;
    QTableWidget* a_knobs = nullptr;
    QRadioButton* a_minimizeMode = nullptr;
    QRadioButton* a_fitMode = nullptr;
    QStackedWidget* a_objective = nullptr;
    QComboBox* a_analysis = nullptr;
    QLineEdit* a_minimize = nullptr;
    QTableWidget* a_targets = nullptr;
    QPlainTextEdit* a_preview = nullptr;
};

#endif
