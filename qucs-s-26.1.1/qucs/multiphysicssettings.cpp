/*
 * multiphysicssettings.cpp - the Multiphysics menu's Settings
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicssettings.h"
#include "multiphysicsview.h"
#include "settings.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace qucs_s::fem {

MultiphysicsSettingsDialog::MultiphysicsSettingsDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("multiphysicsSettings"));
    setWindowTitle(tr("Multiphysics Settings"));
    auto* layout = new QVBoxLayout(this);
    auto* view = new QGroupBox(tr("Graphics view"), this);
    auto* inside = new QVBoxLayout(view);
    a_gpu = new QCheckBox(tr("Draw with the GPU (OpenGL)"), view);
    a_gpu->setObjectName(QStringLiteral("mpGpuBox"));
    a_gpu->setChecked(gpuWanted());
    inside->addWidget(a_gpu);
    auto* about = new QLabel(tr("The geometry, the mesh, a plot's surface and lines drawn by the GPU from buffers made once: "
                                "zooming and panning a large mesh stay smooth. Off, the CPU draws the same picture. "
                                "The view's own menu has it too (right-click the view); either way the setting is kept. "
                                "QUCS_GPU=0 in the environment turns it off, whatever this says."),
                             view);
    about->setWordWrap(true);
    inside->addWidget(about);
    a_status = new QLabel(view);
    a_status->setObjectName(QStringLiteral("mpGpuStatus"));
    a_status->setWordWrap(true);
    inside->addWidget(a_status);
    layout->addWidget(view);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(a_gpu, &QCheckBox::toggled, this, &MultiphysicsSettingsDialog::showStatus);
    showStatus();
}

QString MultiphysicsSettingsDialog::gpuHindrance()
{
    if (qEnvironmentVariable("QUCS_GPU") == QLatin1String("0")) return tr("QUCS_GPU=0 is set: the CPU draws, whatever the setting.");
#ifndef QUCS_GPU_VIEW
    return tr("This build draws with the CPU alone (built without OpenGL).");
#else
    if (!GraphicsView::gpuAvailable()) return tr("No OpenGL 3.3 here (a remote or offscreen display, or no such driver): the CPU draws.");
    return QString();
#endif
}

bool MultiphysicsSettingsDialog::gpuWanted()
{
    return QucsSettingsFile().value(QStringLiteral("Multiphysics/gpu"), true).toBool();
}

void MultiphysicsSettingsDialog::showStatus()
{
    const QString hindrance = gpuHindrance();
    if (!hindrance.isEmpty()) a_status->setText(hindrance);
    else a_status->setText(a_gpu->isChecked() ? tr("The GPU draws the Graphics view.") : tr("The CPU draws the Graphics view."));
}

void MultiphysicsSettingsDialog::apply()
{
    const bool on = a_gpu->isChecked();
    QucsSettingsFile().setValue(QStringLiteral("Multiphysics/gpu"), on);
    // (Each open view drawn so - setGpu() keeps the setting too; a view
    // whose shaders failed stays with the CPU. The views found first: one
    // set off deletes its GPU canvas, a widget of the list - read after, a
    // crash where there is a GPU.)
    QList<GraphicsView*> views;
    for (QWidget* w : QApplication::allWidgets())
        if (auto* view = qobject_cast<GraphicsView*>(w)) views << view;
    for (GraphicsView* view : views) view->setGpu(on);
}

} // namespace qucs_s::fem
