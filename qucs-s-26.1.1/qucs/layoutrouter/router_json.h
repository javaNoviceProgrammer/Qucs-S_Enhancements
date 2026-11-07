/*
 * router_json.h - what the router's readers share: a key a JSON object
 *                 should not have is refused by its name
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ROUTER_JSON_H
#define QUCS_ROUTER_JSON_H

#include "router.h"

#include <QJsonObject>
#include <QStringList>

#include <initializer_list>

namespace qucs_s::router {

/// False, and which key in \a error, when \a o has a key not in \a keys.
inline bool onlyKeys(const QJsonObject& o, std::initializer_list<const char*> keys, const QString& where, QString* error)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool known = false;
        for (const char* k : keys) known = known || it.key() == QLatin1String(k);
        if (!known) {
            QStringList all;
            for (const char* k : keys) all << QLatin1String(k);
            *error = QStringLiteral("%1 has no '%2' (it takes %3)").arg(where, it.key(), all.join(QStringLiteral(", ")));
            return false;
        }
    }
    return true;
}

bool readLayerId(const QJsonValue& v, LayerId* id, QString* error, const QString& where);

} // namespace qucs_s::router

#endif // QUCS_ROUTER_JSON_H
