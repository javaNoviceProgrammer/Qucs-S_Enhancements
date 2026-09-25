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
    bool findMathFuncInc(QString &mathf_inc);
    QString getParentSWPscript(Component *pc_swp, QString sim, bool before, bool &hasDblSWP);
    QString getParentSWPCntVar(Component *pc_swp, QString sim);
    void cleanSpiceinit();
    void createSpiceinit(const QString &initial_spiceinit);
    QString osdiLoads(const QString& netlist) const;

public:
    explicit Ngspice(Schematic* schematic, QObject *parent = 0);
    /// The .spiceinit blocks of \a sch and its subcircuits (each once).
    static QString collectSpiceinit(Schematic* sch);
    void SaveNetlist(QString filename, bool netlist2Console);
    void setSimulatorCmd(QString cmd);
    void setSimulatorParameters(QString parameters);
    /// The Verilog-A sources of the open project to compile before this
    /// schematic is simulated: those that define a module its netlist uses
    /// and whose library is missing or older (osdi::builds()). None
    /// without a project.
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
