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
#include "fem_sparse.h"
#include "gpurendering.h"
#include "multiphysicsview.h"
#include "settings.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
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
    a_gpu = new QCheckBox(tr("Draw with the GPU"), view);
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
    auto* solver = new QGroupBox(tr("Solver"), this);
    auto* rows = new QVBoxLayout(solver);
    auto* line = new QHBoxLayout();
    line->addWidget(new QLabel(tr("Sparse direct solver:"), solver));
    a_solver = new QComboBox(solver);
    a_solver->setObjectName(QStringLiteral("mpSolverBox"));
    a_solver->addItem(tr("Automatic (Accelerate from 20,000 unknowns, Eigen below)"), QStringLiteral("auto"));
    a_solver->addItem(tr("Eigen at every size"), QStringLiteral("eigen"));
    a_solver->addItem(tr("Accelerate at every size"), QStringLiteral("accelerate"));
    a_solver->setCurrentIndex(solverWanted());
    line->addWidget(a_solver, 1);
    rows->addLayout(line);
    auto* explain = new QLabel(tr("Apple's Accelerate factorizes on many threads: a large model's LU or Cholesky some 10 to 60 "
                                  "times faster than Eigen's (one thread). Below some 20,000 unknowns Eigen's is the quicker. "
                                  "Accelerate's LU needs macOS 26 or later; its Cholesky any macOS; Linux and Windows have Eigen's "
                                  "alone. Each solve's log line says which ran (\"Solved … with Accelerate's LU\"). "
                                  "QUCS_FEM_SOLVER=eigen, accelerate or auto in the environment chooses, whatever this says."),
                               solver);
    explain->setWordWrap(true);
    rows->addWidget(explain);
    a_solverStatus = new QLabel(solver);
    a_solverStatus->setObjectName(QStringLiteral("mpSolverStatus"));
    a_solverStatus->setWordWrap(true);
    rows->addWidget(a_solverStatus);
    layout->addWidget(solver);
    connect(a_solver, &QComboBox::currentIndexChanged, this, &MultiphysicsSettingsDialog::showStatus);
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
    return tr("This build draws with the CPU alone (built without Qt's RHI).");
#else
    if (!GraphicsView::gpuAvailable())
        return tr("No GPU backend here fit for the viewers (Metal; Direct3D 11 at feature level 11; OpenGL 3.0 or OpenGL ES 3.0 - a "
                  "remote or offscreen display, or an older driver): the CPU draws.");
    return QString();
#endif
}

bool MultiphysicsSettingsDialog::gpuWanted()
{
    return gpu::wanted(gpu::Viewer::Multiphysics);
}

int MultiphysicsSettingsDialog::solverWanted()
{
    const QString v = QucsSettingsFile().value(QStringLiteral("Multiphysics/solver"), QStringLiteral("auto")).toString();
    return v == QLatin1String("eigen") ? 1 : v == QLatin1String("accelerate") ? 2 : 0;
}

void MultiphysicsSettingsDialog::applySolverSetting()
{
    const int c = solverWanted();
    setDirectSolver(c == 1 ? DirectSolver::Eigen : c == 2 ? DirectSolver::Accelerate : DirectSolver::Automatic);
}

QString MultiphysicsSettingsDialog::solverStatus(int choice)
{
    const QByteArray asked = qgetenv("QUCS_FEM_SOLVER").trimmed().toLower();
    QString lead;
    if (asked == "eigen" || asked == "accelerate" || asked == "auto" || asked == "automatic") {
        lead = tr("QUCS_FEM_SOLVER=%1 is set, whatever this says: ").arg(QString::fromLatin1(asked));
        choice = asked == "eigen" ? 1 : asked == "accelerate" ? 2 : 0;
    }
    // (asked of the solver: the build's macros are its own)
    // (Named as the other cases name them: "this build has Eigen's solvers
    // alone" named no LU - CI's Linux build's test looked for one.)
    if (!accelerateInBuild()) return lead + tr("Eigen's LU and Cholesky (LDLT) at every size: this build has no Accelerate.");
    if (choice == 1) return lead + tr("Eigen's LU and Cholesky (LDLT) at every size.");
    const QString from = choice == 2 ? tr("at every size") : tr("from 20,000 unknowns, Eigen's below");
    const QString lu = acceleratedLuHindrance();
    if (lu.isEmpty()) return lead + tr("Accelerate's LU and Cholesky %1.").arg(from);
    return lead + tr("Accelerate's Cholesky %1; Eigen's LU (%2).").arg(from, lu);
}

void MultiphysicsSettingsDialog::showStatus()
{
    const QString hindrance = gpuHindrance();
    if (!hindrance.isEmpty()) a_status->setText(hindrance);
    else a_status->setText(a_gpu->isChecked() ? tr("The GPU draws the Graphics view.") : tr("The CPU draws the Graphics view."));
    if (a_solver) a_solverStatus->setText(solverStatus(a_solver->currentIndex()));
}

void MultiphysicsSettingsDialog::apply()
{
    QucsSettingsFile().setValue(QStringLiteral("Multiphysics/solver"), a_solver->currentData().toString());
    applySolverSetting();
    // (The setting kept and each open view told, as Settings… > Viewers
    // does: drawn so where nothing is in the GPU's way - QUCS_GPU=0, no
    // backend -, a view whose shaders failed with the CPU.)
    gpu::setWanted(gpu::Viewer::Multiphysics, a_gpu->isChecked());
}

} // namespace qucs_s::fem
