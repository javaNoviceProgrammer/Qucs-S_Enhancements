/***************************************************************************
                           mgspice.h
                             ----------------
    begin                : Sat Jan 10 2015
    copyright            : (C) 2015 by Vadim Kuznetsov
    email                : ra3xdh@gmail.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#ifndef NGSPICE_H
#define NGSPICE_H

#include <QString>
#include <optional>
#include <QStringList>
#include <QDataStream>
#include "schematic.h"
#include "abstractspicekernel.h"
#include "osdiselection.h"

/*!
  \file ngspice.h
  \brief Declaration of the Ngspice class
*/

/*!
 * \brief The Ngspice class Responsible for Ngspice simulator execution.
 */
class Ngspice : public AbstractSpiceKernel
{
    Q_OBJECT

private:
    QString a_spinit_name;
    QStringList a_optimizations;
    QStringList a_statistics;
    QStringList a_sweeps;

    bool checkNodeNames(QStringList &incompat);
    QString getParentSWPscript(Component *pc_swp, QString sim, bool before, bool &hasDblSWP);
    QString getParentSWPCntVar(Component *pc_swp, QString sim);
    void cleanSpiceinit();
    void createSpiceinit(const QString &initial_spiceinit);
    void verilogAFiles(QStringList* sources, QStringList* libraries) const;
    QString osdiLoads(const QString& netlist) const;
    QSet<QString> modelTypesOf(const QString& netlist) const;
    // projectlibraries::alwaysLoadedModules() of the project, once a
    // kernel: a run asks for it three times (its compile step, the netlist
    // that step writes, the netlist it runs).
    mutable std::optional<QSet<QString>> a_alwaysLoaded;
    // projectlibraries::alwaysWrittenModelCards() of the project, once a kernel.
    mutable std::optional<QStringList> a_alwaysCards;
    QString projectOfCircuit() const;
    QStringList besideSchematic(const QStringList& patterns) const;

public:
    /// Where ngspice_mathfunc.inc is in this installation
    /// (share/qucs-s/xspice_cmlib/include), in \a mathf_inc; false when it
    /// is not there.
    static bool findMathFuncInc(QString &mathf_inc);
    explicit Ngspice(Schematic* schematic, QObject *parent = 0);
    /// The .spiceinit blocks of \a sch and its subcircuits (each once).
    static QString collectSpiceinit(Schematic* sch);
    void SaveNetlist(QString filename, bool netlist2Console);
    void setSimulatorCmd(QString cmd);
    void setSimulatorParameters(QString parameters);
    /// The Verilog-A sources to compile before this schematic is simulated
    /// - the project's, those beside it, those its components' libraries
    /// bring: those that define a module its netlist uses and whose library
    /// is missing, older or another platform's (osdi::builds()), each with
    /// where it goes (beside it, or the cache: osdi::buildTarget()).
    QList<qucs_s::osdi::Build> verilogABuilds();
    /// The NgOpt components whose optimize the last netlist runs, in
    /// order: their results come in this order in the output.
    const QStringList& optimizations() const { return a_optimizations; }
    /// The NgMonteCarlo and NgCorners components the last netlist runs.
    const QStringList& statistics() const { return a_statistics; }
    /// The NgSweep components the last netlist runs.
    const QStringList& sweeps() const { return a_sweeps; }

protected:
    void createNetlist(
            QTextStream& stream,
            QStringList& simulations,
            QStringList& vars,
            QStringList& outputs);

public slots:
    void slotSimulate();

protected slots:
    void slotProcessOutput();
};

#endif // NGSPICE_H
