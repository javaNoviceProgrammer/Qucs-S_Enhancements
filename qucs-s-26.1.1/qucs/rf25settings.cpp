/*
 * rf25settings.cpp - the RF 2.5D menu's Settings
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25settings.h"
#include "rf25/rf25_dense.h"
#include "rf25/rf25_fast.h"
#include "rf25/rf25_fullwave.h"
#include "settings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

const QString kLuKey = QStringLiteral("RF25/lu");
const QString kLimitKey = QStringLiteral("RF25/denseLimit");
const QString kMemoryKey = QStringLiteral("RF25/sweepMemory");

/// The LU \a choice (0, 1, 2) as the environment leaves it, and what it
/// says of it.
int chosenAfterEnvironment(int choice, QString* lead)
{
    const QByteArray asked = qgetenv("QUCS_RF25_LU").trimmed().toLower();
    if (asked == "eigen" || asked == "accelerate" || asked == "auto" || asked == "automatic") {
        if (lead != nullptr)
            *lead = RF25SettingsDialog::tr("QUCS_RF25_LU=%1 is set, whatever this says: ").arg(QString::fromLatin1(asked));
        return asked == "eigen" ? 1 : asked == "accelerate" ? 2 : 0;
    }
    if (qgetenv("QUCS_FEM_SOLVER").trimmed().toLower() == "eigen") {
        if (lead != nullptr) *lead = RF25SettingsDialog::tr("QUCS_FEM_SOLVER=eigen is set, whatever this says: ");
        return 1;
    }
    return choice;
}

} // namespace

RF25SettingsDialog::RF25SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("rf25Settings"));
    setWindowTitle(tr("RF 2.5D Settings"));
    auto* layout = new QVBoxLayout(this);

    auto* lu = new QGroupBox(tr("Dense LU"), this);
    auto* luRows = new QVBoxLayout(lu);
    auto* luLine = new QHBoxLayout();
    luLine->addWidget(new QLabel(tr("Factorized by:"), lu));
    a_lu = new QComboBox(lu);
    a_lu->setObjectName(QStringLiteral("rf25LuBox"));
    a_lu->addItem(tr("Automatic (Accelerate's LAPACK on macOS 13.3 and later, Eigen's elsewhere)"), QStringLiteral("auto"));
    a_lu->addItem(tr("Eigen's LU"), QStringLiteral("eigen"));
    a_lu->addItem(tr("Accelerate's LAPACK where this macOS has it"), QStringLiteral("accelerate"));
    a_lu->setCurrentIndex(luWanted());
    luLine->addWidget(a_lu, 1);
    luRows->addLayout(luLine);
    auto* luAbout = new QLabel(tr("The full wave's system of runs of current and the quasi-static solve's charges are each a dense "
                                  "matrix of some thousands of rows. Apple's Accelerate factorizes them on many threads: a full wave's "
                                  "of 4000 to 6000 runs 9 to 11 times faster than Eigen's (one thread), the charges' 15 to 17 times, "
                                  "to the same accuracy. Linux and Windows have Eigen's alone. QUCS_RF25_LU=eigen, accelerate or auto "
                                  "in the environment chooses, whatever this says, and QUCS_FEM_SOLVER=eigen keeps Eigen's."),
                               lu);
    luAbout->setWordWrap(true);
    luRows->addWidget(luAbout);
    a_luStatus = new QLabel(lu);
    a_luStatus->setObjectName(QStringLiteral("rf25LuStatus"));
    a_luStatus->setWordWrap(true);
    luRows->addWidget(a_luStatus);
    layout->addWidget(lu);

    auto* size = new QGroupBox(tr("Full wave"), this);
    auto* sizeRows = new QVBoxLayout(size);
    auto* limitLine = new QHBoxLayout();
    limitLine->addWidget(new QLabel(tr("Held dense up to:"), size));
    a_limit = new QSpinBox(size);
    a_limit->setObjectName(QStringLiteral("rf25LimitBox"));
    a_limit->setRange(0, 30000);
    a_limit->setSingleStep(500);
    a_limit->setSpecialValueText(tr("Automatic"));
    a_limit->setSuffix(tr(" runs and cells"));
    a_limit->setValue(limitWanted());
    limitLine->addWidget(a_limit, 1);
    sizeRows->addLayout(limitLine);
    auto* memoryLine = new QHBoxLayout();
    memoryLine->addWidget(new QLabel(tr("A sweep's frequencies at once may hold:"), size));
    a_memory = new QDoubleSpinBox(size);
    a_memory->setObjectName(QStringLiteral("rf25MemoryBox"));
    a_memory->setDecimals(1);
    a_memory->setSingleStep(0.5);
    a_memory->setRange(0, std::max(1.0, std::floor(physicalMemory() / 1e9)));
    a_memory->setSpecialValueText(tr("Automatic"));
    a_memory->setSuffix(tr(" GB"));
    a_memory->setValue(memoryWanted());
    memoryLine->addWidget(a_memory, 1);
    sizeRows->addLayout(memoryLine);
    auto* sizeAbout = new QLabel(tr("Past the limit a full wave's system is held compressed and solved by iteration, unless the "
                                    "study's Matrix asks otherwise. Automatic is 6000 runs and cells: past it the compressed solve "
                                    "was the quicker for a microstrip, 7 to 17 times at 7000 to 13,000, and about even for a "
                                    "coplanar line of narrow gaps - a higher limit pays only for such a layout. A dense frequency "
                                    "holds some 16 (runs² + cells²) bytes, its system and its cells' potentials; a sweep solves as "
                                    "many frequencies at once as the machine's threads and this memory allow, and more at once is "
                                    "quicker (forty frequencies of 7000: 73 s in 1.5 GB, 31 s in 6). Automatic is a quarter of "
                                    "this machine's memory, 1.5 GB at least."),
                                 size);
    sizeAbout->setWordWrap(true);
    sizeRows->addWidget(sizeAbout);
    a_sizeStatus = new QLabel(size);
    a_sizeStatus->setObjectName(QStringLiteral("rf25SizeStatus"));
    a_sizeStatus->setWordWrap(true);
    sizeRows->addWidget(a_sizeStatus);
    layout->addWidget(size);

    connect(a_lu, &QComboBox::currentIndexChanged, this, &RF25SettingsDialog::showStatus);
    connect(a_limit, &QSpinBox::valueChanged, this, &RF25SettingsDialog::showStatus);
    connect(a_memory, &QDoubleSpinBox::valueChanged, this, &RF25SettingsDialog::showStatus);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    showStatus();
}

int RF25SettingsDialog::luWanted()
{
    const QString v = QucsSettingsFile().value(kLuKey, QStringLiteral("auto")).toString();
    return v == QLatin1String("eigen") ? 1 : v == QLatin1String("accelerate") ? 2 : 0;
}

int RF25SettingsDialog::limitWanted()
{
    bool ok = false;
    const int v = QucsSettingsFile().value(kLimitKey, 0).toInt(&ok);
    return ok && v > 0 ? std::min(v, 30000) : 0;
}

double RF25SettingsDialog::memoryWanted()
{
    bool ok = false;
    const double v = QucsSettingsFile().value(kMemoryKey, 0.0).toDouble(&ok);
    return ok && std::isfinite(v) && v > 0 ? v : 0.0;
}

void RF25SettingsDialog::applySettings()
{
    const int c = luWanted();
    setDenseLu(c == 1 ? DenseLu::Eigen : c == 2 ? DenseLu::Accelerate : DenseLu::Automatic);
    setDenseLimit(limitWanted());
    setSweepMemory(memoryWanted() * 1e9);
}

QString RF25SettingsDialog::luStatus(int choice)
{
    QString lead;
    choice = chosenAfterEnvironment(choice, &lead);
    if (choice == 1) return lead + tr("Eigen's LU factorizes the dense systems.");
    if (accelerateLuHere()) return lead + tr("Accelerate's LAPACK factorizes the dense systems.");
#if defined(Q_OS_MACOS)
    return lead + tr("Eigen's LU factorizes the dense systems: Accelerate's LAPACK needs macOS 13.3 or later, and a build whose "
                     "SDK has it.");
#else
    return lead + tr("Eigen's LU factorizes the dense systems: this build has no Accelerate.");
#endif
}

QString RF25SettingsDialog::sizeStatus(int limit, double gigabytes)
{
    const int most = limit > 0 ? limit : automaticDenseLimit();
    const double bytes = gigabytes > 0 ? gigabytes * 1e9 : automaticSweepMemory();
    // (A system of the limit's size: two thirds of it runs, as the solved
    //  models have it.)
    const double runs = 2.0 * most / 3, cells = most / 3.0;
    const double each = 16.0 * (runs * runs + cells * cells) + 1e6;
    const int atOnce = std::max(1, int(bytes / each));
    const QString limitSaid = limit > 0 ? tr("Held dense up to %1 runs and cells").arg(most)
                                        : tr("Automatic: held dense up to %1 runs and cells").arg(most);
    const QString memorySaid = gigabytes > 0 ? tr("%1 GB").arg(bytes / 1e9, 0, 'f', 1)
                                             : tr("%1 GB (Automatic)").arg(bytes / 1e9, 0, 'f', 1);
    return tr("%1. A sweep's frequencies at once may hold %2: %3 of that size at a time (some %4 MB each), as the threads "
              "allow.")
        .arg(limitSaid, memorySaid)
        .arg(atOnce)
        .arg(each / 1e6, 0, 'f', 0);
}

void RF25SettingsDialog::showStatus()
{
    a_luStatus->setText(luStatus(a_lu->currentIndex()));
    a_sizeStatus->setText(sizeStatus(a_limit->value(), a_memory->value()));
}

void RF25SettingsDialog::apply()
{
    {
        QucsSettingsFile settings;
        settings.setValue(kLuKey, a_lu->currentData().toString());
        settings.setValue(kLimitKey, a_limit->value());
        settings.setValue(kMemoryKey, a_memory->value());
    }
    applySettings();
}

} // namespace qucs_s::rf25
