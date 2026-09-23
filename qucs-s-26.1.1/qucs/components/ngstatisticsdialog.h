/*
 * ngstatisticsdialog.h - the NgMonteCarlo and NgCorners components' dialog:
 * ngspice's montecarlo or corners command written with a form - the
 * analysis, the samples or the corners, the values to record and the
 * specs to judge - and the command it makes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef NGSTATISTICSDIALOG_H
#define NGSTATISTICSDIALOG_H

#include <QDialog>

#include "ngstatistics.h"

class Component;
class Schematic;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;

class NgStatisticsDialog : public QDialog
{
    Q_OBJECT
public:
    NgStatisticsDialog(Component* component, Schematic* schematic);

    bool corners() const { return a_corners; }
    /// What the form says now.
    qucs_s::ngstats::MonteCarlo monteCarlo() const;
    qucs_s::ngstats::Corners cornersCommand() const;
    /// The command line the form makes, or why there is none yet.
    QString preview() const;

    // The form, for the tests.
    QLineEdit* nameEdit() const { return a_name; }
    QComboBox* analysisCombo() const { return a_analysis; }
    QLineEdit* samplesEdit() const { return a_samples; }
    QLineEdit* seedEdit() const { return a_seed; }
    QLineEdit* cornersEdit() const { return a_cornerList; }
    QCheckBox* waveformsBox() const { return a_waveforms; }
    QTableWidget* recordTable() const { return a_records; }
    QTableWidget* specTable() const { return a_specs; }

public slots:
    void addRecord(const qucs_s::ngstats::Record& record = qucs_s::ngstats::Record());
    void addSpec(const qucs_s::ngstats::Spec& spec = qucs_s::ngstats::Spec());

private slots:
    void slotOK();
    void slotApply();
    void slotCancel();
    void updateForm();

private:
    bool apply();
    QStringList simulations() const;

    Component* a_comp;
    Schematic* a_doc;
    bool a_corners;
    bool a_changed = false;

    QTabWidget* a_tabs = nullptr;
    QLineEdit* a_name = nullptr;
    QComboBox* a_analysis = nullptr;
    QLineEdit* a_samples = nullptr;
    QLineEdit* a_seed = nullptr;
    QCheckBox* a_lhs = nullptr;
    QCheckBox* a_modelStats = nullptr;
    QLineEdit* a_cornerList = nullptr;
    QCheckBox* a_nominal = nullptr;
    QCheckBox* a_waveforms = nullptr;
    QTableWidget* a_records = nullptr;
    QTableWidget* a_specs = nullptr;
    QPlainTextEdit* a_preview = nullptr;
};

#endif
