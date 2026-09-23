/*
 * vamodule.h - a Verilog-A module as a Qucs component describes it: its
 * parameters with their defaults, descriptions and units, read from the
 * library OpenVAF built (OSDI) or, before there is one, from the source;
 * and the JSON files the component is made from
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_VAMODULE_H
#define QUCS_VAMODULE_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace qucs_s::vamodule {

struct Parameter {
    QString name;          ///< "r"
    QString value;         ///< its default: "1000", "27"; a string quoted, "\"nmos\"", as .model needs it
    QString description;   ///< "Resistance at the nominal temperature"
    QString units;         ///< "Ohm"
    bool instance = false; ///< an instance parameter (shown on the symbol)
};

struct VerilogModule {
    QString name;
    QList<Parameter> parameters;
};

/// The parameters of the module \a wanted (any case; the first if there is
/// none of that name) in the OSDI library \a osdiFile, as OpenVAF compiled
/// it (OSDI 0.3 or later): false, and why in \a error, if the library
/// cannot be read.
bool readOsdi(const QString& osdiFile, const QString& wanted, VerilogModule* module, QString* error = nullptr);

/// The names of the modules in the OSDI library \a osdiFile (OSDI 0.3 or
/// later), as they are written: false, and why in \a error, if the
/// library cannot be read.
bool osdiModules(const QString& osdiFile, QStringList* names, QString* error = nullptr);

/// The same read from Verilog-A source, for when there is no library yet:
/// "(* desc="...", units="...", type="instance" *) parameter real r = 1e3
/// from (0:inf);" - comments left out, local parameters too.
VerilogModule readSource(const QString& source, const QString& wanted = QString());

/// The names of the modules in Verilog-A source, as written, in order
/// (comments left out).
QStringList sourceModules(const QString& source);

/// The component's properties (NAME_props.json): the module's parameters,
/// each with its description and, in brackets, its units.
QJsonObject propsObject(const VerilogModule& module);

/// The component file (NAME_symbol.json): the properties and the symbol
/// (NAME_sym.json) in one object.
QJsonObject merged(const QJsonObject& props, const QJsonObject& symbol);

/// Reads a JSON file as Qucs writes them - and as older versions did,
/// ending arrays and objects with a comma. An empty object and the reason
/// in \a error if it is not JSON.
QJsonObject parseJson(const QByteArray& data, QString* error = nullptr);

/// \a text as a JSON string, quotes included: for the pieces of JSON the
/// paintings of a symbol write.
QString jsonString(const QString& text);

/// Sets the icon ("BitmapFile") of the component file \a file, the rest as
/// it was; false, and why in \a error, if it cannot be read or written.
bool setIcon(const QString& file, const QString& icon, QString* error = nullptr);

} // namespace qucs_s::vamodule

#endif
