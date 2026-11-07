/*
 * router_job.h - a routing job as the user and Claude give it, in µm:
 *                the layout and cell, the technology, the nets (by point
 *                or by label), keep-outs, guides and options. Read from
 *                JSON (a <layout>.route.json beside the layout) or a CSV
 *                of nets; turned into the router's Problem against a
 *                layout by layoutrouting.h
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ROUTER_JOB_H
#define QUCS_ROUTER_JOB_H

#include "router.h"

#include <QJsonObject>
#include <QPointF>
#include <QVector>

namespace qucs_s::router {

struct JobTerminal {
    QPointF at;                 ///< µm
    bool hasPoint = false;
    QString text;               ///< a label of the layout, when it is named by one
    QString metal;              ///< empty: whichever is there
    Terminal::Mode mode = Terminal::Mode::Centre;
};

struct JobNet {
    QString name;
    QString netClass;           ///< empty: the technology's default
    QVector<JobTerminal> terminals;   ///< the start, then its pads
    QVector<QPointF> guide;     ///< µm
    double band = 0;            ///< µm; 0: the router's (three of its lines' pitch)
    bool fixed = false;
};

struct JobKeepOut {
    QString name;
    QVector<QPointF> outline;   ///< µm
    QStringList metals;
    QStringList except;
    KeepOut::LetThrough letThrough = KeepOut::LetThrough::WayIn;
};

struct Job {
    QString layout;             ///< the file, as given
    QString cell;               ///< empty: the layout's main cell
    QString technologyFile;     ///< or:
    QJsonObject technology;     ///< the technology itself
    QVector<JobNet> nets;
    QVector<JobKeepOut> keepOuts;
    Options options;
    QString routeCell = QStringLiteral("ROUTES");   ///< the cell routes are committed as

    int netIndex(const QString& name) const;
};

/// Reads a job, refusing a key it does not know by name; \a error empty
/// when it could.
Job readJob(const QJsonObject& o, QString* error);
QJsonObject jobJson(const Job& job);

/// One net as JSON (the job's and the tools' form) and back.
bool readNet(const QJsonValue& v, JobNet* net, QString* error);
QJsonObject netJson(const JobNet& net);
bool readKeepOut(const QJsonValue& v, JobKeepOut* k, QString* error);
QJsonObject keepOutJson(const JobKeepOut& k);
bool readOptions(const QJsonValue& v, Options* options, QString* error);
QJsonObject optionsJson(const Options& options);

/// Nets from a CSV. With a header row naming its columns (net, class,
/// start_x, start_y, start_text, start_layer, pad_x, pad_y, pad_text,
/// pad_layer, mode), a net's further rows add pads. Without one, each
/// row is net, start x, start y, pad x, pad y[, class] - or net, start
/// label, pad label[, class].
QVector<JobNet> readNetsCsv(const QString& text, QString* error);

} // namespace qucs_s::router

#endif // QUCS_ROUTER_JOB_H
