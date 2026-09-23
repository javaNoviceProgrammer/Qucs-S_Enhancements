/*
 * oppoint.h - the operating point of every device after a DC bias run:
 * what ngspice's "show all" says about each transistor, diode, source,
 * Verilog-A (OSDI) device... - gm, gds, vth, id, cgs, cd, ...
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_OPPOINT_H
#define QUCS_OPPOINT_H

#include <QList>
#include <QString>
#include <QStringList>

namespace qucs_s::oppoint {

struct Parameter {
    QString name;       ///< as ngspice has it: "gm", "vdsat", "cd", "i_p"
    double value = 0;
};

/// One device of the circuit at its operating point.
struct Device {
    QString name;         ///< ngspice's, lower case: "mt1", "q2n3904_1", "m.xsub1.m9"
    QString type;         ///< "Mos1", "BSIM4", "BJT", "Diode", an OSDI module's name
    QString description;  ///< "Level 1 MOSfet model with Meyer capacitance model"
    QString model;        ///< the model it uses, empty if none
    QList<Parameter> parameters;   ///< the numeric ones, in ngspice's order
    /// The component of the schematic it belongs to ("T1"; "SUB1" for a
    /// device inside that subcircuit), empty if none is found.
    QString component;
    /// Inside a subcircuit: its name there ("m9", "x2.m9"); empty for a
    /// device of the schematic itself.
    QString inside;
};

/// Reads the output of ngspice's "show all": a block per device type
/// (" BJT: Bipolar Junction Transistor"), a "device" row with the names of
/// up to a few devices, a "model" row, then one row per parameter with a
/// value for each. Values that are not numbers ("-") are left out.
QList<Device> parseShow(const QString& text);

/// Fills in Device::component and Device::inside from the names of the
/// schematic's components. ngspice's instance of a component is its name
/// or, when the name does not start with the SPICE letter of its kind,
/// that letter and the name ("T1" -> "mt1", "Pr1" -> "vpr1", "SUB1" ->
/// "xsub1"); a device inside a subcircuit is "<letter>.<instance>.<name>".
void attribute(QList<Device>& devices, const QStringList& components);

/// The unit of a parameter: A for currents, V for voltages, S for
/// conductances, F for capacitances, C for charges, W for power, Ohm, H,
/// Hz, ...; empty when it is not an electrical quantity (a flag, a count,
/// a geometry) or not known.
QString unitOf(const QString& type, const QString& parameter);

/// Whether a parameter describes the operating point (a current, voltage,
/// conductance, capacitance, charge or power) rather than the device's
/// set-up (its geometry, flags, multipliers).
bool isOperatingQuantity(const QString& type, const QString& parameter);

/// The value as it is shown: engineering notation and unit ("1.2 mA").
QString valueText(const Device& device, const Parameter& parameter);

/// A tooltip for a component: its devices (one, or those inside a
/// subcircuit) with their operating quantities that are not zero (nor
/// below 1e-30: an "off" device's rounding), at most \a maxRows of them.
/// Empty when there are no devices.
QString tooltip(const QList<const Device*>& devices, int maxRows = 24);

} // namespace qucs_s::oppoint

#endif
