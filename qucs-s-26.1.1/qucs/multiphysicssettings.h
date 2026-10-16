/*
 * multiphysicssettings.h - the Multiphysics menu's Settings: how the
 *                          Graphics view is drawn (the GPU or the CPU)
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
class QLabel;

namespace qucs_s::fem {

/*!
 * The multiphysics settings: drawing the Graphics view with the GPU
 * (OpenGL) - the setting Multiphysics/gpu, as the view's own menu sets it;
 * on OK every open view drawn so. What keeps the GPU from drawing here
 * (QUCS_GPU=0, a build without OpenGL, no OpenGL 3.3) said under it.
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

    QCheckBox* gpuBox() const { return a_gpu; }
    QLabel* gpuStatus() const { return a_status; }
    /// The setting kept, every open Graphics view drawn so (OK).
    void apply();

private:
    void showStatus();

    QCheckBox* a_gpu = nullptr;
    QLabel* a_status = nullptr;
};

} // namespace qucs_s::fem

#endif
