/*
 * qucscontrol_digitizer.cpp - Claude's tools for a plot in a PDF (find_plots,
 *                             digitize_plot, get_digitized): the numbers, how
 *                             far to trust them and a picture to check them
 *                             by, in a call. Every one works with no window
 *                             (--mcp-server); see plotdigitizer.h.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#ifdef QUCS_HAVE_QTPDF
#include "pdfdoc.h"
#include "plotdigitizer.h"
#include "plotdigitizerpanel.h"
#include "qucs.h"

#include <QCache>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPdfDocument>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <memory>
#endif

using namespace qucs_s::control;

#ifdef QUCS_HAVE_QTPDF

namespace dg = qucs_s::digitizer;

namespace {

// A PDF read once, for the calls that follow: its pages' paths and words stay
// read, so that a second look (other scales, another colour) costs nothing.
struct Opened {
    QPdfDocument pdf;
    std::unique_ptr<dg::Source> source;
};

Opened* openedPdf(const QString& file, QString* error)
{
    static QCache<QString, Opened> cache(4);
    const QFileInfo info(file);
    const QString key = file + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
    if (Opened* have = cache.object(key)) return have;
    auto* opened = new Opened;
    if (opened->pdf.load(file) != QPdfDocument::Error::None || opened->pdf.status() != QPdfDocument::Status::Ready) {
        delete opened;
        *error = QCoreApplication::translate("QucsControl", "%1 cannot be read as a PDF (locked, or not a PDF).").arg(info.fileName());
        return nullptr;
    }
    opened->source = std::make_unique<dg::Source>(&opened->pdf, file);
    cache.insert(key, opened);
    return cache.object(key);
}

// A number with no more digits than it can mean: every token counts.
double rounded(double v, int digits = 6)
{
    if (!std::isfinite(v) || v == 0) return v;
    const double magnitude = std::pow(10.0, digits - 1 - int(std::floor(std::log10(std::abs(v)))));
    if (!std::isfinite(magnitude) || magnitude == 0) return v;
    const double r = std::round(v * magnitude) / magnitude;
    return std::isfinite(r) ? r : v;
}

QJsonValue numberOrNull(double v) { return std::isfinite(v) ? QJsonValue(rounded(v)) : QJsonValue(); }

QJsonArray boxOf(const QRectF& r)
{
    return QJsonArray{rounded(r.left()), rounded(r.top()), rounded(r.right()), rounded(r.bottom())};
}

QJsonObject axisJson(const dg::Axis& axis, const dg::Calibration& c, const QRectF& box, bool xAxis)
{
    QJsonObject o;
    o.insert(QStringLiteral("scale"), (xAxis ? c.xScale : c.yScale) == dg::Scale::Log ? QStringLiteral("log") : QStringLiteral("linear"));
    if (!axis.title.isEmpty()) o.insert(QStringLiteral("title"), axis.title);
    if (axis.multiplier != 1) o.insert(QStringLiteral("multiplier"), axis.multiplier);
    if (axis.scaleGuessed) o.insert(QStringLiteral("scale_is_a_guess"), true);
    QJsonArray ticks;
    for (const dg::Tick& t : axis.ticks)
        if (t.used) ticks.append(rounded(t.value * axis.multiplier, 8));
    if (!ticks.isEmpty()) o.insert(QStringLiteral("ticks_used"), ticks);
    QStringList unused;
    for (const dg::Tick& t : axis.ticks)
        if (!t.used) unused << t.label;
    if (!unused.isEmpty()) o.insert(QStringLiteral("labels_left_out"), unused.join(QStringLiteral(", ")));
    // What the frame runs from and to, which a reader checks first.
    if (c.valid()) {
        const QPointF a = c.toValue(xAxis ? QPointF(box.left(), box.bottom()) : QPointF(box.left(), box.bottom()));
        const QPointF b = c.toValue(xAxis ? QPointF(box.right(), box.bottom()) : QPointF(box.left(), box.top()));
        o.insert(QStringLiteral("from"), numberOrNull(xAxis ? a.x() : a.y()));
        o.insert(QStringLiteral("to"), numberOrNull(xAxis ? b.x() : b.y()));
    }
    return o;
}

QJsonObject trustJson(const dg::Plot& plot)
{
    const dg::Trust t = dg::assess(plot);
    QJsonObject o{{QStringLiteral("level"), t.level}};
    if (!t.reasons.isEmpty()) o.insert(QStringLiteral("reasons"), QJsonArray::fromStringList(t.reasons));
    if (plot.calibration.valid()) {
        o.insert(QStringLiteral("calibration_rms_points"), rounded(t.calibrationPoints, 3));
        o.insert(QStringLiteral("trace_points"), rounded(t.tracePoints, 3));
        const bool xLog = plot.calibration.xScale == dg::Scale::Log && plot.calibration.kind == dg::Kind::Cartesian;
        const bool yLog = plot.calibration.yScale == dg::Scale::Log && plot.calibration.kind == dg::Kind::Cartesian;
        o.insert(QStringLiteral("a_point_of_the_page_is"),
                 QJsonObject{{QStringLiteral("x"), numberOrNull(t.unitsPerPoint.x())},
                             {QStringLiteral("x_unit"), xLog ? QStringLiteral("decades") : QStringLiteral("x's units")},
                             {QStringLiteral("y"), numberOrNull(t.unitsPerPoint.y())},
                             {QStringLiteral("y_unit"), yLog ? QStringLiteral("decades") : QStringLiteral("y's units")}});
        // On a log axis, a share of the value, which is how it is read.
        const auto share = [](bool log, double u) {
            return log ? QJsonValue(rounded((std::pow(10.0, u) - 1) * 100, 3)) : QJsonValue(rounded(u, 4));
        };
        QJsonObject uncertainty{{QStringLiteral("points"), rounded(t.totalPoints, 3)}};
        uncertainty.insert(xLog ? QStringLiteral("x_percent") : QStringLiteral("x"), share(xLog, t.uncertainty.x()));
        uncertainty.insert(yLog ? QStringLiteral("y_percent") : QStringLiteral("y"), share(yLog, t.uncertainty.y()));
        o.insert(QStringLiteral("each_value_good_to"), uncertainty);
    }
    return o;
}

QJsonObject seriesJson(const dg::Plot& plot, const dg::Series& s, int most)
{
    QJsonObject o{{QStringLiteral("name"), s.name},
                  {QStringLiteral("shape"), s.shape == dg::Series::Shape::Markers ? QStringLiteral("markers")
                                            : s.shape == dg::Series::Shape::Area  ? QStringLiteral("area")
                                            : s.shape == dg::Series::Shape::Bars  ? QStringLiteral("bars")
                                            : s.shape == dg::Series::Shape::ByHand ? QStringLiteral("by hand")
                                                                                  : QStringLiteral("line")},
                  {QStringLiteral("origin"), s.origin == dg::Series::Origin::Vector ? QStringLiteral("the file's own vertices")
                                             : s.origin == dg::Series::Origin::Raster ? QStringLiteral("traced from the picture")
                                                                                      : QStringLiteral("placed by hand")},
                  {QStringLiteral("colour"), s.colour.name()},
                  {QStringLiteral("points_in_all"), int(s.points.size())}};
    if (s.dashed) o.insert(QStringLiteral("dashed"), true);
    if (s.bridged > 0) o.insert(QStringLiteral("gaps_crossed"), s.bridged);
    if (s.agreement >= 0) o.insert(QStringLiteral("ink_under_points"), rounded(s.agreement, 3));
    const QList<int> kept = dg::thinned(s.points, most);
    const bool calibrated = plot.calibration.valid();
    QJsonArray xs, ys;
    for (const int i : kept) {
        const QPointF v = calibrated ? plot.calibration.toValue(s.points.at(i)) : s.points.at(i);
        xs.append(numberOrNull(v.x()));
        ys.append(numberOrNull(v.y()));
    }
    o.insert(calibrated ? QStringLiteral("x") : QStringLiteral("page_x"), xs);
    o.insert(calibrated ? QStringLiteral("y") : QStringLiteral("page_y"), ys);
    if (kept.size() < s.points.size()) o.insert(QStringLiteral("thinned_to"), int(kept.size()));
    if (!s.breaks.isEmpty()) {
        // Where a piece starts, among the points returned.
        QJsonArray breaks;
        int k = 0;
        for (const int b : s.breaks) {
            while (k < kept.size() && kept.at(k) < b) ++k;
            if (k < kept.size() && k > 0 && (breaks.isEmpty() || breaks.last().toInt() != k)) breaks.append(k);
        }
        if (!breaks.isEmpty()) o.insert(QStringLiteral("pieces_start_at"), breaks);
    }
    return o;
}

QJsonObject plotJson(const dg::Plot& plot, int most)
{
    QJsonObject o{{QStringLiteral("page"), plot.page + 1}, {QStringLiteral("route"), plot.route}};
    if (!plot.name.isEmpty()) o.insert(QStringLiteral("name"), plot.name);
    const QRectF box = plot.pageBox();
    o.insert(QStringLiteral("box"), boxOf(box));
    if (plot.angle != 0) o.insert(QStringLiteral("turned_degrees"), rounded(plot.angle, 3));
    QJsonObject calibration{{QStringLiteral("valid"), plot.calibration.valid()}};
    if (plot.calibration.valid()) {
        calibration.insert(QStringLiteral("references"), int(plot.calibration.references.size()));
        calibration.insert(QStringLiteral("rms_points"), rounded(plot.calibration.rms(), 3));
        calibration.insert(QStringLiteral("worst_points"), rounded(plot.calibration.worst(), 3));
        if (plot.calibration.kind == dg::Kind::Polar) calibration.insert(QStringLiteral("kind"), QStringLiteral("polar"));
        if (plot.calibration.model == dg::Model::Projective) calibration.insert(QStringLiteral("model"), QStringLiteral("projective"));
    }
    calibration.insert(QStringLiteral("x_axis"), axisJson(plot.xAxis, plot.calibration, box, true));
    calibration.insert(QStringLiteral("y_axis"), axisJson(plot.yAxis, plot.calibration, box, false));
    // Marks found and nothing to say what they are: what x_values and
    // y_values fill in.
    if (!plot.xAxis.found && !plot.frame.isNull() && !plot.xAxis.marks.isEmpty() && !plot.calibration.valid()) {
        QJsonArray at;
        for (const double m : plot.xAxis.marks) at.append(rounded(m, 5));
        calibration.insert(QStringLiteral("x_marks_at"), at);
    }
    if (!plot.yAxis.found && !plot.frame.isNull() && !plot.yAxis.marks.isEmpty() && !plot.calibration.valid()) {
        QJsonArray at;
        for (const double m : plot.yAxis.marks) at.append(rounded(m, 5));
        calibration.insert(QStringLiteral("y_marks_at"), at);
    }
    o.insert(QStringLiteral("calibration"), calibration);
    if (most >= 0) {
        QJsonArray series;
        for (const dg::Series& s : plot.series) series.append(seriesJson(plot, s, most));
        o.insert(QStringLiteral("series"), series);
        o.insert(QStringLiteral("trust"), trustJson(plot));
    }
    if (!plot.notes.isEmpty()) o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(plot.notes));
    return o;
}

QJsonObject withPicture(const QJsonObject& result, const QImage& picture, const PictureAsked& asked)
{
    QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}}};
    if (!picture.isNull()) content.append(pngContent(fitted(picture, asked), asked));
    return {{QStringLiteral("content"), content}, {QStringLiteral("isError"), false}};
}

double overlayScale(const dg::Plot& plot, int longest)
{
    const QRectF region = plot.region.isNull() ? plot.pageBox() : plot.region;
    const double side = std::max({region.width(), region.height(), 1.0});
    return std::clamp(double(longest) / side, 1.0, 5.0);
}

// Writes bytes where a tool was told to: the folder must be there.
bool writeOut(const QString& file, const QByteArray& bytes, QString* error)
{
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        *error = QCoreApplication::translate("QucsControl", "%1 could not be written: %2").arg(QDir::toNativeSeparators(file), out.errorString());
        return false;
    }
    out.write(bytes);
    if (!out.commit()) {
        *error = QCoreApplication::translate("QucsControl", "%1 could not be written: %2").arg(QDir::toNativeSeparators(file), out.errorString());
        return false;
    }
    return true;
}

} // namespace

// Which file: as read_pdf has it. The PDF in front, when none is named.
static QString digitizerFile(const QJsonObject& args, QucsApp* app, QString* error)
{
    QString file = args.value(QLatin1String("path")).toString().trimmed();
    if (file.isEmpty()) {
        QucsDoc* doc = app->DocumentTab->count() > 0 ? app->getDoc() : nullptr;
        if (doc != nullptr && doc->getDocName().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) file = doc->getDocName();
        if (file.isEmpty()) {
            *error = QCoreApplication::translate("QucsControl", "Which PDF? 'path' (the document in front is none).");
            return {};
        }
    }
    file = absolute(file);
    if (!QFileInfo(file).isFile()) {
        *error = QCoreApplication::translate("QucsControl", "There is no file %1.").arg(QDir::toNativeSeparators(file));
        return {};
    }
    return file;
}

// A point of a plot whose value is known, as digitize_plot's 'references' has it.
static bool readDigitizerReference(const QJsonObject& item, bool polar, dg::Reference* r, QString* why)
{
    const QJsonArray at = item.value(QLatin1String("at")).toArray();
    if (at.size() != 2 || !at.at(0).isDouble() || !at.at(1).isDouble()) {
        *why = QCoreApplication::translate("QucsControl", "'at' is [x, y] in points from the page's top left.");
        return false;
    }
    r->at = QPointF(at.at(0).toDouble(), at.at(1).toDouble());
    if (polar) {
        if (!item.value(QLatin1String("r")).isDouble() || !item.value(QLatin1String("theta")).isDouble()) {
            *why = QCoreApplication::translate("QucsControl", "a polar plot's takes 'r' and 'theta' (degrees).");
            return false;
        }
        r->hasX = r->hasY = true;
        r->x = item.value(QLatin1String("r")).toDouble();
        r->y = item.value(QLatin1String("theta")).toDouble();
        return true;
    }
    if (item.value(QLatin1String("x")).isDouble()) {
        r->hasX = true;
        r->x = item.value(QLatin1String("x")).toDouble();
    }
    if (item.value(QLatin1String("y")).isDouble()) {
        r->hasY = true;
        r->y = item.value(QLatin1String("y")).toDouble();
    }
    if (!r->hasX && !r->hasY) {
        *why = QCoreApplication::translate("QucsControl", "it has neither 'x' nor 'y'.");
        return false;
    }
    return true;
}

QJsonObject QucsControl::findPlots(const QJsonObject& args)
{
    QElapsedTimer clock;
    clock.start();
    QString error;
    const QString file = digitizerFile(args, a_app, &error);
    if (file.isEmpty()) return errorResult(error);
    Opened* opened = openedPdf(file, &error);
    if (opened == nullptr) return errorResult(error);
    QPdfDocument& pdf = opened->pdf;
    dg::Source& source = *opened->source;
    const int count = pdf.pageCount();
    QList<int> pages;
    bool cut = false;
    const auto add = [&](int page) {
        if (page < 1 || page > count || pages.contains(page)) return;
        if (pages.size() >= 500) {
            cut = true;
            return;
        }
        pages << page;
    };
    const QJsonValue given = args.value(QLatin1String("pages"));
    const QString word = given.toString().trimmed().toLower();
    if (given.isDouble()) add(given.toInt());
    else if (given.isArray()) {
        for (const QJsonValue& v : given.toArray()) add(v.toInt());
    } else if (given.isString() && word != QLatin1String("all") && !word.isEmpty()) {
        if (!word.contains(QRegularExpression(QStringLiteral("[0-9]")))) return errorResult(tr("'pages' is a page number, a list of them, a range (\"2-5\") or \"all\"."));
        for (const QString& part : word.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList ends = part.split(QLatin1Char('-'));
            const int a = ends.value(0).trimmed().toInt(), b = ends.size() > 1 ? ends.at(1).trimmed().toInt() : a;
            for (int k = a; k <= b && k - a < 1000; ++k) add(k);
        }
    } else {
        for (int k = 1; k <= count; ++k) add(k);
    }
    if (pages.isEmpty()) return errorResult(tr("%1 has pages 1 to %2.").arg(QFileInfo(file).fileName()).arg(count));
    // A page costs little by its paths, more by a picture: enough for one call.
    constexpr qint64 kMostMilliseconds = 20000;
    QJsonArray found;
    int looked = 0;
    QStringList notReached;
    for (const int page : std::as_const(pages)) {
        if (clock.elapsed() > kMostMilliseconds) {
            notReached << QString::number(page);
            continue;
        }
        ++looked;
        const QList<dg::Plot> plots = dg::findPlots(source, page - 1);
        int number = 0;
        for (const dg::Plot& plot : plots) {
            QJsonObject o = plotJson(plot, -1);
            o.insert(QStringLiteral("plot"), ++number);
            found.append(o);
        }
    }
    QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("pages_in_all"), count},
                       {QStringLiteral("pages_looked_at"), looked},
                       {QStringLiteral("plots"), found},
                       {QStringLiteral("ms"), int(clock.elapsed())}};
    if (!source.hasVectors() && !source.vectorProblem().isEmpty()) result.insert(QStringLiteral("paths_not_read"), source.vectorProblem());
    if (!notReached.isEmpty()) result.insert(QStringLiteral("not_reached"), tr("Time ran out before pages %1: ask for them.").arg(notReached.mid(0, 40).join(QStringLiteral(", "))));
    if (cut) result.insert(QStringLiteral("left_out"), tr("A call looks at 500 pages at most."));
    if (found.isEmpty())
        result.insert(QStringLiteral("note"), tr("No plot with a frame and axes was found. Give digitize_plot a 'region' and 'references' for one without them (axes with no box round them, a polar plot)."));
    else result.insert(QStringLiteral("next"), tr("digitize_plot with 'page' and 'plot' takes the numbers from one."));
    return jsonResult(result);
}

QJsonObject QucsControl::digitizePlot(const QJsonObject& args)
{
    QElapsedTimer clock;
    clock.start();
    QString error;
    const QString file = digitizerFile(args, a_app, &error);
    if (file.isEmpty()) return errorResult(error);
    Opened* opened = openedPdf(file, &error);
    if (opened == nullptr) return errorResult(error);
    QPdfDocument& pdf = opened->pdf;
    dg::Source& source = *opened->source;
    const int count = pdf.pageCount();

    // --- Which plot.
    int page = args.value(QLatin1String("page")).toInt(0);
    if (args.contains(QLatin1String("page")) && (page < 1 || page > count)) return errorResult(tr("%1 has pages 1 to %2.").arg(QFileInfo(file).fileName()).arg(count));
    QRectF region;
    if (args.contains(QLatin1String("region"))) {
        if (page < 1) return errorResult(tr("'region' is of a page: give 'page' too."));
        QList<double> corners;
        for (const QJsonValue& v : args.value(QLatin1String("region")).toArray()) corners << v.toDouble();
        const QSizeF paper = pdf.pagePointSize(page - 1);
        region = qucs_s::pdf::regionOf(corners, paper).intersected(QRectF(QPointF(0, 0), paper));
        if (region.isEmpty()) return errorResult(tr("'region' is [x1, y1, x2, y2] of a page: points from its top left (page %1 is %2 x %3), or fractions of it.").arg(page).arg(paper.width()).arg(paper.height()));
    }
    dg::Plot plot;
    if (!region.isNull()) {
        plot = dg::plotInRegion(source, page - 1, region);
    } else {
        // Not named: the page's plots, or the first page that has one.
        QList<dg::Plot> plots;
        if (page >= 1) plots = dg::findPlots(source, page - 1);
        else {
            int front = 0;
            if (auto* doc = dynamic_cast<PdfDoc*>(a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr); doc != nullptr && sameFile(doc->getDocName(), file)) front = doc->view()->currentPage();
            for (int k = 0; k < count && plots.isEmpty(); ++k) {
                const int at = (front + k) % count;
                plots = dg::findPlots(source, at);
                if (!plots.isEmpty()) page = at + 1;
                if (clock.elapsed() > 15000) break;
            }
            if (plots.isEmpty()) return errorResult(tr("No plot was found (%1 of %2 pages looked at): give 'page' and 'region', and 'references' if it has no labels.").arg(std::min(count, 500)).arg(count));
        }
        const int number = args.value(QLatin1String("plot")).toInt(plots.size() == 1 ? 1 : 0);
        if (plots.isEmpty()) return errorResult(tr("No plot with a frame and axes was found on page %1: give 'region', and 'references' if it has no labels. (read_pdf with 'image' shows the page.)").arg(page));
        if (number == 0) {
            // Several, and which was not said: they are the answer.
            QJsonArray list;
            int k = 0;
            for (const dg::Plot& p : std::as_const(plots)) {
                QJsonObject o = plotJson(p, -1);
                o.insert(QStringLiteral("plot"), ++k);
                list.append(o);
            }
            return jsonResult(QJsonObject{{QStringLiteral("file"), QDir::toNativeSeparators(file)},
                                          {QStringLiteral("page"), page},
                                          {QStringLiteral("note"), tr("Page %1 has %2 plots: 'plot' says which.").arg(page).arg(plots.size())},
                                          {QStringLiteral("plots"), list}});
        }
        if (number < 1 || number > plots.size()) return errorResult(tr("Page %1 has %2 plots: 'plot' is 1 to %2.").arg(page).arg(plots.size()));
        plot = plots.at(number - 1);
    }
    const qint64 foundAt = clock.elapsed();

    // --- The calibration: as read, as given.
    QStringList notes;
    const auto scaleOf = [&](const char* key, dg::Scale* out, bool* set) {
        const QString s = args.value(QLatin1String(key)).toString().trimmed().toLower();
        if (s.isEmpty() || s == QLatin1String("auto")) return true;
        if (s == QLatin1String("linear") || s == QLatin1String("lin")) *out = dg::Scale::Linear;
        else if (s == QLatin1String("log") || s == QLatin1String("logarithmic")) *out = dg::Scale::Log;
        else {
            error = tr("'%1' is linear, log or auto.").arg(QString::fromLatin1(key));
            return false;
        }
        *set = true;
        return true;
    };
    dg::Scale xScale = plot.xAxis.scale, yScale = plot.yAxis.scale;
    bool xSet = false, ySet = false;
    if (!scaleOf("x_scale", &xScale, &xSet) || !scaleOf("y_scale", &yScale, &ySet)) return errorResult(error);
    const QJsonArray xValues = args.value(QLatin1String("x_values")).toArray(), yValues = args.value(QLatin1String("y_values")).toArray();
    const QJsonArray references = args.value(QLatin1String("references")).toArray();
    const bool polar = args.value(QLatin1String("polar")).toBool();
    const QString model = args.value(QLatin1String("model")).toString().trimmed().toLower();
    if (!model.isEmpty() && model != QLatin1String("affine") && model != QLatin1String("projective")) return errorResult(tr("'model' is affine or projective."));
    bool useReferences = !references.isEmpty() || args.contains(QLatin1String("x_range")) || args.contains(QLatin1String("y_range"));
    if (polar && references.isEmpty()) return errorResult(tr("A polar plot takes 'references': three or more points with r and theta (degrees), one may be the centre."));
    if (model == QLatin1String("projective") && references.isEmpty()) return errorResult(tr("A projective fit takes 'references': four or more points with both x and y."));
    const auto readValues = [&](const QJsonArray& given, bool xAxis) {
        QList<double> list;
        for (const QJsonValue& v : given) {
            if (!v.isDouble()) {
                error = tr("'%1' is a list of numbers.").arg(xAxis ? QStringLiteral("x_values") : QStringLiteral("y_values"));
                return false;
            }
            list << v.toDouble();
        }
        return dg::assignTickValues(plot, xAxis, list, &error);
    };
    if (!xValues.isEmpty() && !readValues(xValues, true)) return errorResult(error);
    if (!yValues.isEmpty() && !readValues(yValues, false)) return errorResult(error);
    // The values at the frame's edges: the quickest calibration of a scan, the
    // four numbers at the ends of its axes.
    const QJsonArray xRange = args.value(QLatin1String("x_range")).toArray(), yRange = args.value(QLatin1String("y_range")).toArray();
    const bool ranges = args.contains(QLatin1String("x_range")) || args.contains(QLatin1String("y_range"));
    if (ranges && !references.isEmpty()) return errorResult(tr("'x_range' and 'y_range' or 'references': one or the other."));
    if (ranges) {
        if (xRange.size() != 2 || yRange.size() != 2 || !xRange.at(0).isDouble() || !xRange.at(1).isDouble() || !yRange.at(0).isDouble() || !yRange.at(1).isDouble())
            return errorResult(tr("'x_range' is [left, right] and 'y_range' [bottom, top]: the values at the frame's edges, both given."));
        if (plot.frame.isNull()) return errorResult(tr("No frame was found to put 'x_range' and 'y_range' on: give 'references'."));
    }
    if (useReferences) {
        dg::Calibration c;
        c.xScale = xSet || !plot.xAxis.found ? xScale : plot.xAxis.scale;
        c.yScale = ySet || !plot.yAxis.found ? yScale : plot.yAxis.scale;
        if (polar) {
            c.kind = dg::Kind::Polar;
            c.rAtCentre = args.value(QLatin1String("r_at_centre")).toDouble(0);
        }
        if (model == QLatin1String("projective")) c.model = dg::Model::Projective;
        if (args.contains(QLatin1String("x_range"))) {
            const QTransform back = plot.work.inverted();
            const QRectF f = plot.frame;
            const auto ref = [&](QPointF at, bool x, double value) {
                dg::Reference r;
                r.at = back.map(at);
                (x ? r.hasX : r.hasY) = true;
                (x ? r.x : r.y) = value;
                c.references << r;
            };
            ref(f.bottomLeft(), true, xRange.at(0).toDouble());
            ref(f.bottomRight(), true, xRange.at(1).toDouble());
            ref(f.bottomLeft(), false, yRange.at(0).toDouble());
            ref(f.topLeft(), false, yRange.at(1).toDouble());
        }
        int number = 0;
        for (const QJsonValue& v : references) {
            ++number;
            dg::Reference r;
            QString why;
            if (!readDigitizerReference(v.toObject(), polar, &r, &why)) return errorResult(tr("Reference %1: %2").arg(number).arg(why));
            c.references << r;
        }
        QString why;
        if (!c.fit(&why)) return errorResult(tr("The references give no calibration: %1").arg(why));
        plot.calibration = c;
        plot.xAxis.found = plot.yAxis.found = false;
    } else if (!xValues.isEmpty() || !yValues.isEmpty() || xSet || ySet || (plot.xAxis.found && plot.yAxis.found)) {
        if (xSet) {
            plot.xAxis.scale = xScale;
            plot.xAxis.scaleGuessed = false;
        }
        if (ySet) {
            plot.yAxis.scale = yScale;
            plot.yAxis.scaleGuessed = false;
        }
        if (plot.xAxis.found && plot.yAxis.found) {
            QString why;
            if (!dg::calibrateFromAxes(plot, &why)) notes << tr("No calibration from the axes: %1").arg(why);
        } else if (!plot.frame.isNull()) {
            notes << tr("The %1 axis still has no values: 'x_values' and 'y_values' give one for each of the marks listed.").arg(plot.xAxis.found ? QStringLiteral("y") : QStringLiteral("x"));
        }
    }
    const qint64 calibratedAt = clock.elapsed();

    // --- The series.
    dg::Options options;
    const QString route = args.value(QLatin1String("route")).toString().trimmed().toLower();
    if (route == QLatin1String("vector")) options.route = dg::Options::Route::Vector;
    else if (route == QLatin1String("raster") || route == QLatin1String("picture")) options.route = dg::Options::Route::Raster;
    else if (!route.isEmpty() && route != QLatin1String("auto")) return errorResult(tr("'route' is auto, vector or raster."));
    for (const QJsonValue& v : args.value(QLatin1String("colours")).toArray()) {
        const QColor colour(v.toString());
        if (!colour.isValid()) return errorResult(tr("'colours' holds %1, which is no colour (\"#rrggbb\").").arg(v.toString()));
        options.colours << colour;
    }
    for (const QJsonValue& v : args.value(QLatin1String("seeds")).toArray()) {
        const QJsonArray at = v.toArray();
        if (at.size() != 2 || !at.at(0).isDouble() || !at.at(1).isDouble()) return errorResult(tr("'seeds' holds points on the curves: [[x, y], ...], in points from the page's top left."));
        options.seeds << QPointF(at.at(0).toDouble(), at.at(1).toDouble());
    }
    // Regions to leave out, given: then only those (an empty list, none).
    if (args.contains(QLatin1String("exclude"))) {
        plot.exclude.clear();
        options.findLeftOut = false;
        const QSizeF paper = pdf.pagePointSize(plot.page);
        for (const QJsonValue& v : args.value(QLatin1String("exclude")).toArray()) {
            QList<double> corners;
            for (const QJsonValue& n : v.toArray()) corners << n.toDouble();
            const QRectF r = qucs_s::pdf::regionOf(corners, paper);
            if (r.isEmpty()) return errorResult(tr("'exclude' holds regions [x1, y1, x2, y2] of the page: points from its top left, or fractions of it."));
            plot.exclude << r;
        }
    }
    const double wantedScale = args.value(QLatin1String("trace_scale")).toDouble(0);
    if (wantedScale < 0 || wantedScale > 16) return errorResult(tr("'trace_scale' is the pixels a point of the picture to trace from, up to 16."));
    options.scale = wantedScale;
    QString why;
    dg::extract(source, plot, options, &why);
    if (plot.series.isEmpty() && !why.isEmpty()) notes << why;
    const qint64 extractedAt = clock.elapsed();
    // New names: by the colours asked for, the first name for the first
    // colour's series; else in the order the series come. Said, either way.
    {
        const QJsonArray names = args.value(QLatin1String("names")).toArray();
        QStringList given;
        if (!names.isEmpty() && !options.colours.isEmpty()) {
            for (int k = 0; k < names.size() && k < options.colours.size(); ++k) {
                if (!names.at(k).isString()) continue;
                const QString name = names.at(k).toString().left(200);
                int count = 0;
                for (dg::Series& series : plot.series) {
                    // (By the colour it was traced as, where it says; else the
                    // nearest - a file's own colours are the colours named.)
                    int nearest = series.asked;
                    double best = nearest >= 0 ? -1 : 1e9;
                    for (int j = 0; j < options.colours.size(); ++j) {
                        const QColor& c = options.colours.at(j);
                        const double d = std::abs(c.red() - series.colour.red()) + std::abs(c.green() - series.colour.green()) + std::abs(c.blue() - series.colour.blue());
                        if (d < best) {
                            best = d;
                            nearest = j;
                        }
                    }
                    if (nearest != k) continue;
                    series.name = count == 0 ? name : QStringLiteral("%1 %2").arg(name).arg(count + 1);
                    given << QStringLiteral("%1 (%2)").arg(series.name, series.colour.name());
                    ++count;
                }
            }
            if (!given.isEmpty()) notes << tr("Named by the colours asked for: %1.").arg(given.join(QStringLiteral(", ")));
        } else {
            for (int k = 0; k < names.size() && k < plot.series.size(); ++k)
                if (names.at(k).isString()) {
                    plot.series[k].name = names.at(k).toString().left(200);
                    given << QStringLiteral("%1 (%2)").arg(plot.series.at(k).name, plot.series.at(k).colour.name());
                }
            if (!given.isEmpty()) notes << tr("Named in the order the series came: %1. With 'colours', names go by colour.").arg(given.join(QStringLiteral(", ")));
        }
    }
    if (!source.hasVectors() && !source.vectorProblem().isEmpty()) notes << source.vectorProblem();
    plot.notes << notes;

    // --- What it answers.
    const int most = args.contains(QLatin1String("max_points")) ? std::max(0, args.value(QLatin1String("max_points")).toInt()) : 200;
    QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(file)}};
    const QJsonObject described = plotJson(plot, most == 0 ? 2000000000 : most);
    for (auto i = described.constBegin(); i != described.constEnd(); ++i) result.insert(i.key(), i.value());

    // --- Written out.
    const QString csvGiven = args.value(QLatin1String("csv")).toString().trimmed();
    if (!csvGiven.isEmpty()) {
        const QString csvFile = absolute(csvGiven);
        if (!writeOut(csvFile, dg::csv(plot).toUtf8(), &error)) return errorResult(error);
        result.insert(QStringLiteral("csv"), QDir::toNativeSeparators(csvFile));
    }
    const QString datasetGiven = args.value(QLatin1String("dataset")).toString().trimmed();
    if (!datasetGiven.isEmpty()) {
        const QString datasetFile = absolute(datasetGiven);
        if (!writeOut(datasetFile, dg::dataset(plot), &error)) return errorResult(error);
        result.insert(QStringLiteral("dataset"), QDir::toNativeSeparators(datasetFile));
    }
    const bool keep = args.value(QLatin1String("keep")).toBool();
    const bool show = args.value(QLatin1String("show")).toBool();
    if (keep) {
        QList<dg::Plot> kept;
        QString loadWhy;
        if (!dg::loadSidecar(file, &kept, &loadWhy)) return errorResult(tr("What is kept with the PDF could not be read, and is left alone: %1").arg(loadWhy));
        const int replaced = args.value(QLatin1String("replace")).toInt(0);
        if (replaced < 0 || replaced > kept.size()) return errorResult(tr("'replace' is one of the plots kept, 1 to %1.").arg(kept.size()));
        if (plot.name.isEmpty()) plot.name = tr("Plot on page %1").arg(plot.page + 1);
        if (replaced > 0) kept[replaced - 1] = plot;
        else kept << plot;
        if (!dg::saveSidecar(file, kept, &error)) return errorResult(error);
        result.insert(QStringLiteral("kept_as"), replaced > 0 ? replaced : int(kept.size()));
        result.insert(QStringLiteral("kept_in"), QDir::toNativeSeparators(dg::sidecarPath(file)));
        // An open tab shows it.
        for (QucsDoc* d : a_app->allDocuments())
            if (auto* p = dynamic_cast<PdfDoc*>(d); p != nullptr && sameFile(p->getDocName(), file)) p->digitizedChanged();
    }
    if (show) {
        QucsDoc* opened2 = nullptr;
        for (QucsDoc* d : a_app->allDocuments())
            if (auto* p = dynamic_cast<PdfDoc*>(d); p != nullptr && sameFile(p->getDocName(), file)) opened2 = p;
        if (opened2 == nullptr && a_app->gotoPage(file, false, false)) opened2 = a_app->getDoc();
        if (auto* p = dynamic_cast<PdfDoc*>(opened2)) {
            a_app->showDocument(QucsApp::documentWidget(p));
            // Kept: the panel on what is kept. Not: shown alone, written with
            // the rest only once the user changes it.
            if (keep) p->showDigitized(result.value(QStringLiteral("kept_as")).toInt() - 1);
            else {
                dg::Plot shown = plot;
                shown.transient = true;
                p->showTransient(shown);
                result.insert(QStringLiteral("kept"), tr("not kept: written beside the PDF only if it is changed in the panel, or with 'keep'"));
            }
            result.insert(QStringLiteral("shown"), true);
        }
    }

    const bool wantPicture = !args.contains(QLatin1String("picture")) || args.value(QLatin1String("picture")).toBool();
    QJsonObject ms{{QStringLiteral("find"), int(foundAt)}, {QStringLiteral("calibrate"), int(calibratedAt - foundAt)}, {QStringLiteral("extract"), int(extractedAt - calibratedAt)}};
    if (!wantPicture) {
        ms.insert(QStringLiteral("all"), int(clock.elapsed()));
        result.insert(QStringLiteral("ms"), ms);
        return jsonResult(result);
    }
    QString pictureError;
    PictureAsked asked = PictureAsked::of(args, &pictureError);
    if (!pictureError.isEmpty()) return errorResult(pictureError);
    if (!args.contains(QLatin1String("max_width"))) asked.longest = 1000;
    const QImage picture = dg::overlay(source, plot, overlayScale(plot, asked.longest));
    ms.insert(QStringLiteral("all"), int(clock.elapsed()));
    result.insert(QStringLiteral("ms"), ms);
    return withPicture(result, picture, asked);
}

QJsonObject QucsControl::getDigitized(const QJsonObject& args)
{
    QElapsedTimer clock;
    clock.start();
    QString error;
    const QString file = digitizerFile(args, a_app, &error);
    if (file.isEmpty()) return errorResult(error);
    QList<dg::Plot> plots;
    bool stale = false;
    if (!dg::loadSidecar(file, &plots, &error, &stale)) return errorResult(error);
    QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(file)}};
    if (plots.isEmpty()) {
        result.insert(QStringLiteral("plots"), QJsonArray());
        result.insert(QStringLiteral("note"), tr("Nothing is kept with this PDF: the viewer's Digitize panel, or digitize_plot with 'keep', keeps a plot."));
        return jsonResult(result);
    }
    if (stale) result.insert(QStringLiteral("warning"), tr("The PDF has changed since these were kept: its pages may no longer be where they were."));
    const int most = args.contains(QLatin1String("max_points")) ? std::max(0, args.value(QLatin1String("max_points")).toInt()) : 200;
    const int only = args.value(QLatin1String("plot")).toInt(0);
    if (only < 0 || only > plots.size()) return errorResult(tr("%1 plots are kept: 'plot' is 1 to %1.").arg(plots.size()));
    QJsonArray list;
    for (int i = 0; i < plots.size(); ++i) {
        if (only != 0 && i != only - 1) continue;
        QJsonObject o = plotJson(plots.at(i), most == 0 ? 2000000000 : most);
        o.insert(QStringLiteral("plot"), i + 1);
        list.append(o);
    }
    result.insert(QStringLiteral("plots"), list);
    result.insert(QStringLiteral("kept_in"), QDir::toNativeSeparators(dg::sidecarPath(file)));
    // The picture of one, when one is named and it is asked for.
    if (only != 0 && args.value(QLatin1String("picture")).toBool()) {
        Opened* opened = openedPdf(file, &error);
        if (opened == nullptr) return errorResult(error);
        QString pictureError;
        PictureAsked asked = PictureAsked::of(args, &pictureError);
        if (!pictureError.isEmpty()) return errorResult(pictureError);
        if (!args.contains(QLatin1String("max_width"))) asked.longest = 1000;
        const dg::Plot& plot = plots.at(only - 1);
        result.insert(QStringLiteral("ms"), int(clock.elapsed()));
        return withPicture(result, dg::overlay(*opened->source, plot, overlayScale(plot, asked.longest)), asked);
    }
    result.insert(QStringLiteral("ms"), int(clock.elapsed()));
    return jsonResult(result);
}

#else // QUCS_HAVE_QTPDF

QJsonObject QucsControl::findPlots(const QJsonObject&) { return errorResult(tr("This build of Qucs-S has no PDF reader (Qt PDF).")); }
QJsonObject QucsControl::digitizePlot(const QJsonObject&) { return errorResult(tr("This build of Qucs-S has no PDF reader (Qt PDF).")); }
QJsonObject QucsControl::getDigitized(const QJsonObject&) { return errorResult(tr("This build of Qucs-S has no PDF reader (Qt PDF).")); }

#endif // QUCS_HAVE_QTPDF
