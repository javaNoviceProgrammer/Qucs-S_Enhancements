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
#include "fem_sparse_external.h"
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
#include <QLocale>
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
    addSolverChoices(a_solver);
    a_solver->setCurrentIndex(solverWanted());
    line->addWidget(a_solver, 1);
    rows->addLayout(line);
    auto* explain = new QLabel(tr("Apple's Accelerate, MUMPS and STRUMPACK factorize on many threads: a large model's LU or "
                                  "Cholesky some 10 to 60 times faster than Eigen's (one thread). Below some 20,000 unknowns "
                                  "Eigen's is the quicker. Accelerate's LU needs macOS 26 or later, its Cholesky any macOS; MUMPS "
                                  "and STRUMPACK are there where this build has them (Linux's and Windows' releases do): MUMPS "
                                  "factorizes a full-wave solve's complex symmetric matrix by an LDLT, in half an LU's memory, "
                                  "STRUMPACK by an LU. Each solve's log line says which ran (\"Solved … with MUMPS's LU\"). "
                                  "QUCS_FEM_SOLVER=eigen, accelerate, mumps, strumpack or auto in the environment chooses, "
                                  "whatever this says."),
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

namespace {
// The setting's values, in the box's order - DirectSolver's.
const char* const kSolverValues[] = {"auto", "eigen", "accelerate", "mumps", "strumpack"};
constexpr int kSolverChoices = int(sizeof kSolverValues / sizeof kSolverValues[0]);

QString unknownsText(int n)
{
    return QLocale(QLocale::English).toString(n);
}
} // namespace

int MultiphysicsSettingsDialog::solverWanted()
{
    const QString v = QucsSettingsFile().value(QStringLiteral("Multiphysics/solver"), QStringLiteral("auto")).toString();
    for (int k = 0; k < kSolverChoices; ++k)
        if (v == QLatin1String(kSolverValues[k])) return k;
    return 0;
}

void MultiphysicsSettingsDialog::applySolverSetting()
{
    setDirectSolver(DirectSolver(solverWanted()));
}

void MultiphysicsSettingsDialog::addSolverChoices(QComboBox* box)
{
    // (Automatic named by what it takes in this build; a library this build
    // has not, said so - chosen, Eigen's factorizes.)
    const QString absent = tr(" - not in this build");
    QString automatic;
    if (accelerateInBuild()) automatic = tr("Automatic (Accelerate from 20,000 unknowns, Eigen below)");
    else if (mumpsInBuild()) automatic = tr("Automatic (MUMPS from %1 unknowns, Eigen below)").arg(unknownsText(mumpsFrom()));
    else if (strumpackInBuild()) automatic = tr("Automatic (STRUMPACK from %1 unknowns, Eigen below)").arg(unknownsText(strumpackFrom()));
    else automatic = tr("Automatic (Eigen: this build has no other)");
    box->addItem(automatic, QString::fromLatin1(kSolverValues[0]));
    box->addItem(tr("Eigen at every size"), QString::fromLatin1(kSolverValues[1]));
    box->addItem(tr("Accelerate at every size") + (accelerateInBuild() ? QString() : absent), QString::fromLatin1(kSolverValues[2]));
    box->addItem(tr("MUMPS from %1 unknowns, Eigen below").arg(unknownsText(mumpsFrom())) + (mumpsInBuild() ? QString() : absent),
                 QString::fromLatin1(kSolverValues[3]));
    box->addItem(tr("STRUMPACK from %1 unknowns, Eigen below").arg(unknownsText(strumpackFrom())) + (strumpackInBuild() ? QString() : absent),
                 QString::fromLatin1(kSolverValues[4]));
}

QString MultiphysicsSettingsDialog::solverStatus(int choice)
{
    const QByteArray asked = qgetenv("QUCS_FEM_SOLVER").trimmed().toLower();
    QString lead;
    for (int k = 0; k < kSolverChoices; ++k)
        if (asked == kSolverValues[k] || (k == 0 && asked == "automatic")) {
            lead = tr("QUCS_FEM_SOLVER=%1 is set, whatever this says: ").arg(QString::fromLatin1(asked));
            choice = k;
        }
    if (choice < 0 || choice >= kSolverChoices) choice = 0;
    const DirectSolver solver = DirectSolver(choice);
    const Backend lu = luBackendFor(solver), cholesky = choleskyBackendFor(solver);
    // (Each from its size: Accelerate's at every size where chosen.)
    auto size = [solver](Backend b) {
        const int from = b == Backend::Accelerate ? (solver == DirectSolver::Accelerate ? 0 : 20000) : backendFrom(b);
        return from == 0 ? tr("at every size") : tr("from %1 unknowns, Eigen's below").arg(unknownsText(from));
    };
    auto version = [](Backend b) {
        if (b == Backend::Mumps) return tr(" (MUMPS %1)").arg(QString::fromStdString(external::mumpsVersion()));
        if (b == Backend::Strumpack) return tr(" (STRUMPACK %1)").arg(QString::fromStdString(external::strumpackVersion()));
        return QString();
    };
    // Why a solver chosen is not what factorizes (the build, the macOS).
    QString why;
    switch (solver) {
    case DirectSolver::Accelerate: why = accelerateInBuild() ? acceleratedLuHindrance() : tr("this build has no Accelerate"); break;
    case DirectSolver::Mumps: why = mumpsHindrance(); break;
    case DirectSolver::Strumpack: why = strumpackHindrance(); break;
    case DirectSolver::Automatic:
        if (lu == Backend::Eigen)
            why = accelerateInBuild() ? acceleratedLuHindrance() : tr("this build has no Accelerate, MUMPS or STRUMPACK");
        else if (lu != Backend::Accelerate && accelerateInBuild())
            why = tr("Accelerate's LU: %1").arg(acceleratedLuHindrance());
        break;
    case DirectSolver::Eigen: break;
    }
    // (Named as the other cases name them, an LU in each: CI's Linux
    // build's test looks for one.)
    if (lu == cholesky) {
        if (lu == Backend::Eigen)
            return lead + tr("Eigen's LU and Cholesky (LDLT) at every size") + (why.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + why + QStringLiteral("."));
        const QString what = lu == Backend::Accelerate ? tr("Accelerate's LU and Cholesky")
                             : lu == Backend::Mumps    ? tr("MUMPS's LU, LDLT and Cholesky")
                                                       : tr("STRUMPACK's LU, for the Cholesky too,");
        return lead + tr("%1 %2%3.").arg(what, size(lu), version(lu));
    }
    const QString choleskyText = cholesky == Backend::Eigen       ? tr("Eigen's Cholesky (LDLT) at every size")
                                 : cholesky == Backend::Strumpack ? tr("STRUMPACK's LU for the Cholesky %1").arg(size(cholesky))
                                                                  : tr("%1's Cholesky %2").arg(backendName(cholesky), size(cholesky));
    const QString luText = lu == Backend::Eigen ? tr("Eigen's LU") : tr("%1's LU %2%3").arg(backendName(lu), size(lu), version(lu));
    return lead + choleskyText + QStringLiteral("; ") + luText + (why.isEmpty() ? QString() : QStringLiteral(" (") + why + QStringLiteral(")")) + QStringLiteral(".");
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
