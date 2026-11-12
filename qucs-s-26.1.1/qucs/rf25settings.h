/*
 * rf25settings.h - the RF 2.5D menu's Settings: which library factorizes
 *                  the dense systems, how large a full-wave system is held
 *                  dense, and how much memory a dense sweep's frequencies at
 *                  once may hold
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25SETTINGS_H
#define QUCS_RF25SETTINGS_H

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace qucs_s::rf25 {

/*!
 * The RF 2.5D settings, each kept in the settings file and given to the
 * solver at the start and on OK (rf25/rf25_dense.h, rf25/rf25_fullwave.h):
 * - the dense LU (RF25/lu: auto, eigen, accelerate, openblas): Automatic
 *   is Accelerate's LAPACK where this macOS has it, else OpenBLAS's where
 *   the build has it, else Eigen's - QUCS_RF25_LU, and
 *   QUCS_FEM_SOLVER=eigen, over it;
 * - the most runs and cells a full-wave system is held dense for, past which
 *   it is held compressed (RF25/denseLimit; 0 Automatic);
 * - the memory a dense sweep's frequencies at once may hold, GB
 *   (RF25/sweepMemory; 0 Automatic).
 */
class RF25SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RF25SettingsDialog(QWidget* parent = nullptr);

    /// The settings as kept: the LU 0 Automatic, 1 Eigen, 2 Accelerate, 3
    /// OpenBLAS; the limit in runs and cells and the memory in GB, 0 for
    /// Automatic.
    static int luWanted();
    static int limitWanted();
    static double memoryWanted();
    /// The settings given to the solver (at the start, and on OK).
    static void applySettings();
    /// What factorizes here with \a choice (0 to 3), QUCS_RF25_LU,
    /// QUCS_FEM_SOLVER, this build and this macOS taken in.
    static QString luStatus(int choice);
    /// How large a system is held dense and how many of that size are
    /// solved at once, with \a limit and \a gigabytes (0: Automatic).
    static QString sizeStatus(int limit, double gigabytes);

    QComboBox* luBox() const { return a_lu; }
    QLabel* luStatusLabel() const { return a_luStatus; }
    QSpinBox* limitBox() const { return a_limit; }
    QDoubleSpinBox* memoryBox() const { return a_memory; }
    QLabel* sizeStatusLabel() const { return a_sizeStatus; }
    /// The settings kept and given to the solver (OK).
    void apply();

private:
    void showStatus();

    QComboBox* a_lu = nullptr;
    QLabel* a_luStatus = nullptr;
    QSpinBox* a_limit = nullptr;
    QDoubleSpinBox* a_memory = nullptr;
    QLabel* a_sizeStatus = nullptr;
};

} // namespace qucs_s::rf25

#endif
