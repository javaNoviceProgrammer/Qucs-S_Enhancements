/*
 * multiphysicssettings.h - the Multiphysics menu's Settings: how the
 *                          Graphics view is drawn (the GPU or the CPU),
 *                          which sparse direct solver factorizes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSSETTINGS_H
#define QUCS_MULTIPHYSICSSETTINGS_H

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;

namespace qucs_s::fem {

/*!
 * The multiphysics settings: drawing the Graphics view with the GPU
 * (OpenGL) - the setting Multiphysics/gpu, as the view's own menu sets it;
 * on OK every open view drawn so. What keeps the GPU from drawing here
 * (QUCS_GPU=0, a build without OpenGL, no OpenGL 3.3) said under it. The
 * sparse direct solver - Automatic, Eigen's or Accelerate's at every size
 * (Multiphysics/solver) - and what is in effect: QUCS_FEM_SOLVER over it,
 * Accelerate's LU only on macOS 26 and later.
 */
class MultiphysicsSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MultiphysicsSettingsDialog(QWidget* parent = nullptr);

    /// Why the GPU cannot draw here, whatever the setting (empty: it can).
    static QString gpuHindrance();
    /// The setting (Multiphysics/gpu; on unless set off).
    static bool gpuWanted();

    /// The solver setting (Multiphysics/solver: auto, eigen, accelerate;
    /// Automatic unless set) - 0 Automatic, 1 Eigen, 2 Accelerate.
    static int solverWanted();
    /// The setting given to the solver (at the start, and on OK).
    static void applySolverSetting();
    /// What factorizes here with \a choice (0, 1, 2), the environment's
    /// QUCS_FEM_SOLVER and this macOS taken in.
    static QString solverStatus(int choice);

    QCheckBox* gpuBox() const { return a_gpu; }
    QLabel* gpuStatus() const { return a_status; }
    QComboBox* solverBox() const { return a_solver; }
    QLabel* solverStatusLabel() const { return a_solverStatus; }
    /// The setting kept, every open Graphics view drawn so (OK).
    void apply();

private:
    void showStatus();

    QCheckBox* a_gpu = nullptr;
    QLabel* a_status = nullptr;
    QComboBox* a_solver = nullptr;
    QLabel* a_solverStatus = nullptr;
};

} // namespace qucs_s::fem

#endif
