/***************************************************************************
                           abstractspicekernel.h
                             ----------------
    begin                : Sat Jan 10 2014
    copyright            : (C) 2014 by Vadim Kuznetsov
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



#ifndef ABSTRACTSPICEKERNEL_H
#define ABSTRACTSPICEKERNEL_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QDataStream>
#include <QTextStream>
#include <QProcess>
#include <QSet>

#include "schematic.h"
#include "extsimkernels/spicecompat.h"

class QPlainTextEdit;

/*!
  \file abstractspicekernel.h
  \brief Implementation of the AbstractSpiceKernel class
*/

/*!
 * \brief AbstractSpiceKernel class contains common methods for
 *        Ngspice and Xyce simulation kernels. Contains spice netlist builder
 *        and responsible for simulator execution. Ngspice and Xyce classes
 *        inherit this class.
 */
class AbstractSpiceKernel : public QObject
{
    Q_OBJECT

private:
    enum outType {xyceSTD, spiceRaw, spiceRawSwp, xyceSTDswp, spicePrn, Unknown};

    void normalizeVarsNames(QStringList &var_list, const QString &dataset_prefix, bool isCustom = false);
    int checkRawOutupt(QString ngspice_file, QStringList &values);
    void readVariableNames(QTextStream &ngsp_data, int &NumVars, QStringList &var_list,
                           QStringList &extra_vars, QList<int> &extra_vars_dims);
    void extractBinSamples(QDataStream &dbl, QList< QList<double> > &sim_points,
                           int NumPoints, int NumVars, bool isComplex);
    bool extractASCIISamples(QString &lin, QTextStream &ngsp_data, QList< QList<double> > &sim_points,
                             int NumVars, bool isComplex);

protected:
    QString a_workdir;
    QString a_simulator_cmd;
    QString a_simulator_parameters;
    QString a_output;
    QProcess *a_simProcess;

    QPlainTextEdit *a_console;
    QStringList a_sims;
    QStringList a_vars;
    QStringList a_output_files;

    bool a_DC_OP_only; // only calculate operating point to show DC bias
    bool a_needsPrefix;
    QString a_extraParameters;   // .PARAM lines ahead of the schematic's own
    Schematic *a_schematic;

    bool a_parseFourTHD;  // Fourier output is parsed twice, first freqencies, then THD
    bool a_parsePZzeros;  // PZ output is parsed twice, first poles, then zeros

    bool prepareSpiceNetlist(QTextStream &stream, bool isSubckt = false);
    virtual void startNetlist(QTextStream& stream, spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault);
    virtual void createNetlist(QTextStream& stream, int NumPorts,QStringList& simulations,
                               QStringList& vars, QStringList &outputs);
    virtual QSet<QString> getLabelledNets(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault);
    virtual QSet<QString> getActiveLabelledNets(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault);
    QSet<QString> getValidNets(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault);
    void removeAllSimulatorOutputs();
    bool checkGround();
    bool checkSimulations();
    bool checkDCSimulation();

public:

    explicit AbstractSpiceKernel(Schematic *schematic, QObject *parent = 0);
    ~AbstractSpiceKernel();

    bool checkSchematic(QStringList &incompat);
    virtual void createSubNetlist(QTextStream& stream, bool lib = false);

    void parseNgSpiceSimOutput(QString ngspice_file,
                               QList< QList<double> > &sim_points,
                               QStringList &var_list, bool &isComplex,
                               QStringList &extra_vars, QList<int> &extra_vars_dims);
    void parseHBOutput(QString ngspice_file, QList< QList<double> > &sim_points,
                       QStringList &var_list, bool &hasParSweep);
    void parseFourierOutput(QString ngspice_file, QList< QList<double> > &sim_points,
                            QStringList &var_list);
    void parseNoiseOutput(QString ngspice_file, QList< QList<double> > &sim_points,
                          QStringList &var_list, bool &ParSwp);
    void parsePZOutput(QString ngspice_file, QList< QList<double> > &sim_points,
                       QStringList &var_list, bool &ParSwp);
    void parseSENSOutput(QString ngspice_file, QList< QList<double> > &sim_points,
                         QStringList &var_list);
    void parseDC_OPoutput(QString ngspice_file);
    void parseDC_OPoutputXY(QString xyce_file);
    void parseSTEPOutput(QString ngspice_file,
                         QList< QList<double> > &sim_points,
                         QStringList &var_list, bool &isComplex,
                         QStringList &extra_vars, QList<int> &extra_vars_dims);
    void parsePrnOutput(const QString &ngspice_file,
                        QList< QList<double> > &sim_points,
                        QStringList &var_list,
                        bool isComplex);
    void parseXYCESTDOutput(QString std_file,
                            QList< QList<double> > &sim_points,
                            QStringList &var_list, bool &isComplex, bool &hasParSweep);
    void parseXYCENoiseLog(QString logfile, QList< QList<double> > &sim_points,
                           QStringList &var_list);
    void parseResFile(QString resfile, QString &var, QStringList &values);
    void convertToQucsData(const QString &qucs_dataset);
    QString getOutput();
    /// The Scratch folder the netlist and the simulator's output files are in.
    QString workdir() const { return a_workdir; }
    /// The simulator process's own account of its last error, and the
    /// command it was started with (for the messages of a failed start).
    QString processErrorString() const;
    QString simulatorCommand() const { return a_simulator_cmd; }

    virtual void setSimulatorCmd(QString cmd);
    virtual void setSimulatorParameters(QString parameters);
    void setWorkdir(QString path);
    /// Lines the netlist carries ahead of the schematic's parameters:
    /// ".PARAM Rload=50\n" for an optimization's variable that no
    /// component defines (optimization::NetlistScope).
    void setExtraParameters(const QString& lines) { a_extraParameters = lines; }
    /// A full simulation, not the operating point alone, whatever the
    /// schematic's DC bias state was when the kernel was made.
    void setOperatingPointOnly(bool only) { a_DC_OP_only = only; }
    virtual void SaveNetlist(QString filename);
    virtual bool waitEndOfSimulation();
    void setConsole(QPlainTextEdit *console) { a_console = console; }
    static QStringList collectSpiceLibraryFiles(Schematic *sch);
    /// The .va files the components of \a sch and of its subcircuits
    /// bring with them, and the .osdi models compiled from them
    /// (Component::getVerilogAFiles()).
    static QStringList collectVerilogAFiles(Schematic *sch);
    static QString collectSpiceLibs(Schematic* sch);

    /// For walks down the subcircuit hierarchy: true the first time
    /// \a file (a subcircuit's schematic) is met, false after - also for
    /// a subcircuit that includes itself, directly or through others,
    /// which recursed until the stack ran out. hierarchyStart() is the
    /// set to begin with: the top schematic itself.
    static bool firstVisit(const QString& file, QSet<QString>& visited);
    static QSet<QString> hierarchyStart(Schematic* sch);

signals:
    void started();
    void finished();
    void errors(QProcess::ProcessError);
    void progress(int);

protected slots:
    virtual void slotFinished();
    virtual void slotProcessOutput();

public slots:
    virtual void slotSimulate();
    /// Kills the simulator process (and, in a kernel that runs several,
    /// drops the ones still queued).
    virtual void killThemAll();
    void slotErrors(QProcess::ProcessError err);

};

#endif // ABSTRACTSPICEKERNEL_H
