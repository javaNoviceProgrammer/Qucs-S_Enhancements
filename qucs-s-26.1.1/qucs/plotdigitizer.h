/*
 * plotdigitizer.h - numbers read off a plot in a PDF: its frame and axes
 *                   found, calibrated, its curves and markers extracted -
 *                   from the page's vector art where it has any (the very
 *                   vertices), else from a picture of the page
 *
 * No widgets here: the viewer's panel (plotdigitizerpanel.h) and Claude's
 * tools (qucscontrol_digitizer.cpp) both work through this.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PLOTDIGITIZER_H
#define QUCS_PLOTDIGITIZER_H

#include "pdfvector.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <memory>

class QPdfDocument;

namespace qucs_s::digitizer {

enum class Scale { Linear, Log };
enum class Kind { Cartesian, Polar };
/// How page points map to the axes. Auto: Affine, which holds for a rotated
/// or sheared page; Projective (four or more references, each with both
/// values) for a page photographed at an angle.
enum class Model { Affine, Projective };

/// A point of the page whose value is known: on an axis, x or y alone; a
/// corner or a data point, both. In a polar plot x is r and y the angle, in
/// degrees.
struct Reference {
    QPointF at;   ///< page points, from the top left
    bool hasX = false, hasY = false;
    double x = 0, y = 0;
};

/*!
 * From page points to the axes' values and back, fitted to its references
 * by least squares: linear in x and y (or their logs) for a plot on
 * rectangular axes, whichever way those lie on the page.
 */
class Calibration
{
public:
    Kind kind = Kind::Cartesian;
    Scale xScale = Scale::Linear;   ///< (polar: r's)
    Scale yScale = Scale::Linear;
    Model model = Model::Affine;
    double rAtCentre = 0;           ///< polar: r's value at the centre (a pattern in dB has one)
    QList<Reference> references;

    /// Fits to the references; false, with why, when they cannot give one.
    bool fit(QString* why = nullptr);
    bool valid() const { return a_valid; }
    /// NaN off the scale (a point on the wrong side of a projective horizon).
    QPointF toValue(QPointF page) const;
    QPointF toPage(QPointF value) const;
    /// How far each reference lands from where it was put, in points.
    const QList<double>& residuals() const { return a_residuals; }
    double rms() const { return a_rms; }
    double worst() const { return a_worst; }
    /// What a point of the page is worth at \a page along each axis, in the
    /// axis's units (a log axis: decades).
    QPointF unitsPerPoint(QPointF page) const;
    /// Undoes fit(): no longer valid.
    void reset();

private:
    bool fitAffine(QString* why);
    bool fitProjective(QString* why);
    QPointF forward(double x, double y) const;      // values -> (u, v)
    QPointF backward(double u, double v) const;     // (u, v) -> values
    void measure();

    bool a_valid = false;
    // (u, v) = M (page x, page y, 1), homogeneous; affine: its last row 0 0 1.
    double a_m[9] = {};
    double a_inverse[9] = {};
    QList<double> a_residuals;
    double a_rms = 0, a_worst = 0;
};

/// A line of the page's text in pieces, a number where it is one.
struct Token {
    QString text;
    QRectF box;           ///< page points
    bool numeric = false;
    double value = 0;
};

/// A number as a tick label may write it: 1.5, −3, 1e-3, 10^3, 2×10⁻³, 100k,
/// 1,000, 10 µ. False when it is not.
bool parseTickLabel(const QString& text, double* value);

/// A tick on an axis.
struct Tick {
    double position = 0;     ///< along the axis, in the plot's working points
    bool hasValue = false;
    double value = 0;
    QString label;
    bool onMark = false;     ///< placed on a tick mark or grid line, not just by its label
    bool used = false;       ///< kept by the fit
};

/// What was found of one axis.
struct Axis {
    bool found = false;      ///< calibrated from the page
    Scale scale = Scale::Linear;
    bool scaleGuessed = false;   ///< two labels, or nothing to tell linear from log
    int side = 0;            ///< x: 0 bottom, 1 top; y: 0 left, 1 right
    QList<Tick> ticks;       ///< with their values
    QList<double> marks;     ///< every tick mark found, labelled or not
    double multiplier = 1;   ///< a "×10⁻³" or "1e6" at its end, applied
    QString title;
    double fitError = 0;     ///< rms of the ticks about the fit, in points
};

/// A curve or a set of markers.
struct Series {
    enum class Shape { Line, Markers, Area, Bars, ByHand };
    enum class Origin { Vector, Raster, Hand };
    QString name;
    QColor colour = Qt::black;
    Shape shape = Shape::Line;
    Origin origin = Origin::Hand;
    QList<QPointF> points;     ///< page points
    QList<int> breaks;         ///< indices that start a new piece (the curve left the frame, a gap too long)
    bool dashed = false;
    double width = 0;          ///< of the line, points
    int bridged = 0;           ///< gaps crossed by following the line (raster)
    double agreement = -1;     ///< the share of its points with ink under them; -1 not checked
};

/// A plot on a page.
struct Plot {
    int page = 0;
    QString name;
    QRectF region;             ///< page points: what was looked at
    QRectF frame;              ///< working points: the axes' box; null if none
    /// Page points to working points: identity but for a scan set straight.
    QTransform work;
    double angle = 0;          ///< degrees the page's scan is turned by
    QString route;             ///< "vector", "raster", "hand"
    Calibration calibration;
    Axis xAxis, yAxis;
    QList<Series> series;
    QStringList notes;
    double renderScale = 0;    ///< raster: pixels a point it was traced at

    QRectF pageBox() const;    ///< the frame (or the region) on the page
};

/// How far to trust a plot's numbers.
struct Trust {
    QString level;             ///< high, medium, low, none
    QStringList reasons;       ///< what lowers it (and what supports it)
    double calibrationPoints = 0;    ///< rms of the references, points
    double tracePoints = 0;          ///< what the route itself is good to, points
    double totalPoints = 0;
    QPointF unitsPerPoint;           ///< at the middle of the frame (a log axis: decades)
    QPointF uncertainty;             ///< totalPoints in each axis's units
};
Trust assess(const Plot& plot);

/// One PDF, for several calls: its document (not owned), its reader, what
/// was read of its pages kept.
class Source
{
public:
    explicit Source(QPdfDocument* pdf, const QString& file = QString());
    ~Source();
    QPdfDocument* pdf() const { return a_pdf; }
    /// The page's vector art (none: an encrypted or unreadable file); why
    /// said once in \a why.
    const pdf::vec::Graphics& graphics(int page);
    bool hasVectors() const;
    QString vectorProblem() const { return a_problem; }
    /// The words and numbers in \a within (page points).
    QList<Token> tokens(int page, const QRectF& within);
    QImage render(int page, const QRectF& region, double scale) const;

private:
    QPdfDocument* a_pdf;
    std::shared_ptr<pdf::vec::Reader> a_reader;
    QString a_problem;
    QHash<int, pdf::vec::Graphics> a_graphics;
    struct Text;
    QHash<int, std::shared_ptr<Text>> a_text;
};

/// What to take from a plot.
struct Options {
    enum class Route { Auto, Vector, Raster };
    Route route = Route::Auto;
    QList<QColor> colours;     ///< only these (raster: of the picture; vector: of the paths)
    QList<QPointF> seeds;      ///< page points on the curves to follow
    int maxSeries = 24;
    double scale = 0;          ///< raster: pixels a point; 0: chosen
};

/// The plots on \a page (within \a within, page points, when given): each
/// with its frame and, where its labels say, its calibration. No series yet.
QList<Plot> findPlots(Source& source, int page, const QRectF& within = QRectF());
/// One plot from a region a user or Claude has marked, framed if a frame is
/// in it, calibrated if its labels can.
Plot plotInRegion(Source& source, int page, const QRectF& region);
/// Sets \a plot's calibration from its axes, as given: \a xScale / \a yScale
/// ("auto" is Scale's absence) - false, with why, when they do not give one.
bool calibrateFromAxes(Plot& plot, QString* why);
/// Extracts the series of \a plot into it, replacing what it had.
bool extract(Source& source, Plot& plot, const Options& options, QString* why = nullptr);
/// A series of \a plot traced from \a seed (page points): the path under it,
/// or the line in the picture followed both ways.
bool traceFrom(Source& source, Plot& plot, QPointF seed, Series* out, QString* why = nullptr);
/// \a tick values for the marks found on an axis of a scan, whose labels
/// could not be read: a value for each mark, in order along the axis.
bool assignTickValues(Plot& plot, bool xAxis, const QList<double>& values, QString* why = nullptr);

/// The values of a series, each [x, y]; \a pointsOut its page points.
QList<QPointF> values(const Plot& plot, const Series& series);
/// At most \a most points of \a points, the shape kept (Douglas & Peucker's,
/// on the page), the indices kept; everything when it has no more.
QList<int> thinned(const QList<QPointF>& points, int most);

/// The plot on its page, with what was extracted drawn over it - frame,
/// references, each series - at \a scale pixels a point.
QImage overlay(Source& source, const Plot& plot, double scale);
/// Draws the plot's marks with \a painter already mapping page points.
void paintMarks(class QPainter& painter, const Plot& plot, double pixelsPerPoint, int selectedSeries = -1, int selectedPoint = -1);

// --- Results ---------------------------------------------------------------

/// "series,x,y" lines, every point of every series.
QString csv(const Plot& plot);
/// A Qucs dataset: each series a dependent over an independent of its own.
QByteArray dataset(const Plot& plot);
/// Names made fit for a dataset's variables.
QString variableName(const QString& name);

// --- Kept with the PDF -----------------------------------------------------

QJsonObject toJson(const Plot& plot);
/// \a o as a plot; false, with why, for what is not one. Its numbers are
/// checked: nothing here trusts the file.
bool fromJson(const QJsonObject& o, Plot* plot, QString* why = nullptr);
/// The file a PDF's plots are kept in: beside it, or - that folder not being
/// writable - in the application's own data, under a name from its path.
QString sidecarPath(const QString& pdfFile, bool forWriting = false);
/// \a plots written; false, with why.
bool saveSidecar(const QString& pdfFile, const QList<Plot>& plots, QString* why = nullptr);
/// What was kept for \a pdfFile (nothing, and no error, when nothing was).
/// \a stale: the PDF has changed since. False, with why, when the file is
/// there and cannot be read; it is then left alone.
bool loadSidecar(const QString& pdfFile, QList<Plot>* plots, QString* why = nullptr, bool* stale = nullptr);
/// The same from bytes (the fuzz's way in).
bool parseSidecar(const QByteArray& bytes, QList<Plot>* plots, QString* why = nullptr);

} // namespace qucs_s::digitizer

#endif // QUCS_PLOTDIGITIZER_H
