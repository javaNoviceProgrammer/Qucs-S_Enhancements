/*
 * ngsweepdialog.h - the NgSweep component's dialog: ngspice's sweep
 * command written with a form - the analysis, the parameter and its
 * values, outer parameters for a family, the values to record - and the
 * command it makes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef NGSWEEPDIALOG_H
#define NGSWEEPDIALOG_H

#include <QDialog>

#include "ngsweep.h"

class Component;
class Schematic;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;

class NgSweepDialog : public QDialog
{
    Q_OBJECT
public:
    NgSweepDialog(Component* component, Schematic* schematic);

    /// What the form says now.
    qucs_s::ngsweep::Sweep sweep() const;
    /// The command line the form makes, or why there is none yet.
    QString preview() const;

    // The form, for the tests.
    QLineEdit* nameEdit() const { return a_name; }
    QComboBox* analysisCombo() const { return a_analysis; }
    QComboBox* parameterCombo() const { return a_parameter; }
    QComboBox* typeCombo() const { return a_type; }
    QLineEdit* startEdit() const { return a_start; }
    QLineEdit* stopEdit() const { return a_stop; }
    QLineEdit* pointsEdit() const { return a_points; }
    QLineEdit* valuesEdit() const { return a_values; }
    QCheckBox* waveformsBox() const { return a_waveforms; }
    QTableWidget* outerTable() const { return a_outer; }
    QTableWidget* recordTable() const { return a_records; }

public slots:
    void addOuter(const qucs_s::ngsweep::Knob& knob = qucs_s::ngsweep::Knob());
    void addRecord(const qucs_s::ngstats::Record& record = qucs_s::ngstats::Record());

private slots:
    void slotOK();
    void slotApply();
    void slotCancel();
    void updateForm();

private:
    bool apply();
    QStringList simulations() const;
    QStringList parameters() const;
    QComboBox* typeBox(const QString& type);

    Component* a_comp;
    Schematic* a_doc;
    bool a_changed = false;

    QTabWidget* a_tabs = nullptr;
    QLineEdit* a_name = nullptr;
    QComboBox* a_analysis = nullptr;
    QComboBox* a_parameter = nullptr;
    QComboBox* a_type = nullptr;
    QLineEdit* a_start = nullptr;
    QLineEdit* a_stop = nullptr;
    QLineEdit* a_points = nullptr;
    QLineEdit* a_values = nullptr;
    QCheckBox* a_waveforms = nullptr;
    QTableWidget* a_outer = nullptr;
    QTableWidget* a_records = nullptr;
    QPlainTextEdit* a_preview = nullptr;
};

#endif
