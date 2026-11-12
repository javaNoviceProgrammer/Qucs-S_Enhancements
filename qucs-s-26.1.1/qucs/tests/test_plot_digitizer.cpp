/*
 * The plot digitizer's engine (plotdigitizer.h, pdfvector.h): PDFs written
 * here from known data - by QPdfWriter, as vector art and as pictures - read
 * back, and the numbers held to the truth. No window, no core of the
 * application: only the engine and Qt's PDF module.
 *
 * Each plot here is drawn by the same few functions: a frame, ticks with
 * their labels, curves from functions, so that what the digitizer returns can
 * be compared with what the function says at that x.
 */
#include <QBuffer>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPageSize>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <functional>
#include <random>

#include "pdfvector.h"
#include "plotdigitizer.h"
#include "plotdigitizer_p.h"

using namespace qucs_s::digitizer;
namespace vec = qucs_s::pdf::vec;

namespace {

// Where a plot goes on the page, in points, and what its axes run over.
struct Axes {
    QRectF frame{100, 120, 400, 280};
    double xMin = 0, xMax = 10, yMin = -1, yMax = 1;
    bool xLog = false, yLog = false;
    bool xReversed = false;
    QList<double> xTicks{0, 2, 4, 6, 8, 10};
    QList<double> yTicks{-1, -0.5, 0, 0.5, 1};
    bool grid = false;
    bool labels = true;
    QString (*format)(double) = nullptr;

    double px(double x) const
    {
        double t = xLog ? (std::log10(x) - std::log10(xMin)) / (std::log10(xMax) - std::log10(xMin)) : (x - xMin) / (xMax - xMin);
        if (xReversed) t = 1 - t;
        return frame.left() + t * frame.width();
    }
    double py(double y) const
    {
        const double t = yLog ? (std::log10(y) - std::log10(yMin)) / (std::log10(yMax) - std::log10(yMin)) : (y - yMin) / (yMax - yMin);
        return frame.bottom() - t * frame.height();
    }
    QPointF at(double x, double y) const { return {px(x), py(y)}; }
};

struct Curve {
    std::function<double(double)> f;
    QColor colour = Qt::blue;
    double width = 1.2;
    Qt::PenStyle style = Qt::SolidLine;
    QString name;
    int samples = 300;
    bool markers = false;
    double from = std::numeric_limits<double>::quiet_NaN(), to = std::numeric_limits<double>::quiet_NaN();
};

QString label(double v)
{
    return QString::number(v, 'g', 6).replace(QLatin1Char('-'), QChar(0x2212));
}

QFont plotFont(double size, double unit)
{
    QFont font(QStringLiteral("Helvetica"));
    font.setPixelSize(int(size * unit));
    return font;
}

void drawAxes(QPainter& p, const Axes& a, double unit)
{
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 0.8));
    p.drawRect(a.frame);
    p.setFont(plotFont(9, unit));
    for (const double t : a.xTicks) {
        const double x = a.px(t);
        if (a.grid) {
            p.setPen(QPen(QColor(190, 190, 190), 0.5));
            p.drawLine(QPointF(x, a.frame.top()), QPointF(x, a.frame.bottom()));
        }
        p.setPen(QPen(Qt::black, 0.8));
        p.drawLine(QPointF(x, a.frame.bottom()), QPointF(x, a.frame.bottom() + 4));
    }
    for (const double t : a.yTicks) {
        const double y = a.py(t);
        if (a.grid) {
            p.setPen(QPen(QColor(190, 190, 190), 0.5));
            p.drawLine(QPointF(a.frame.left(), y), QPointF(a.frame.right(), y));
        }
        p.setPen(QPen(Qt::black, 0.8));
        p.drawLine(QPointF(a.frame.left() - 4, y), QPointF(a.frame.left(), y));
    }
    if (!a.labels) return;
    // (Text drawn in a coarser system, so that the font's size is whole.)
    p.save();
    p.scale(1 / unit, 1 / unit);
    p.setPen(Qt::black);
    const auto text = [&](QRectF box, int align, const QString& s) {
        p.drawText(QRectF(box.topLeft() * unit, box.size() * unit), align, s);
    };
    for (const double t : a.xTicks) {
        const QString s = a.format != nullptr ? a.format(t) : label(t);
        text(QRectF(a.px(t) - 30, a.frame.bottom() + 6, 60, 12), Qt::AlignHCenter | Qt::AlignTop, s);
    }
    for (const double t : a.yTicks) {
        const QString s = a.format != nullptr ? a.format(t) : label(t);
        text(QRectF(a.frame.left() - 66, a.py(t) - 7, 60, 14), Qt::AlignRight | Qt::AlignVCenter, s);
    }
    p.restore();
}

void drawCurve(QPainter& p, const Axes& a, const Curve& c)
{
    QPolygonF line;
    const double from = std::isnan(c.from) ? a.xMin : c.from, to = std::isnan(c.to) ? a.xMax : c.to;
    for (int i = 0; i < c.samples; ++i) {
        const double t = double(i) / (c.samples - 1);
        const double x = a.xLog ? std::pow(10.0, std::log10(from) + t * (std::log10(to) - std::log10(from))) : from + t * (to - from);
        line << a.at(x, c.f(x));
    }
    if (c.markers) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.colour);
        for (const QPointF& q : std::as_const(line)) p.drawEllipse(q, 3, 3);
        return;
    }
    QPen pen(c.colour, c.width, c.style);
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(line);
}

void drawLegend(QPainter& p, const Axes& a, const QList<Curve>& curves, double unit)
{
    double y = a.frame.top() + 14;
    const double x = a.frame.right() - 120;
    for (const Curve& c : curves) {
        if (c.name.isEmpty()) continue;
        if (c.markers) {
            p.setPen(Qt::NoPen);
            p.setBrush(c.colour);
            p.drawEllipse(QPointF(x + 12, y), 3, 3);
        } else {
            QPen pen(c.colour, c.width, c.style);
            pen.setCapStyle(Qt::FlatCap);
            p.setPen(pen);
            p.drawLine(QPointF(x, y), QPointF(x + 24, y));
        }
        p.save();
        p.scale(1 / unit, 1 / unit);
        p.setPen(Qt::black);
        p.setFont(plotFont(9, unit));
        p.drawText(QRectF(QPointF(x + 28, y - 7) * unit, QSizeF(90, 14) * unit), Qt::AlignLeft | Qt::AlignVCenter, c.name);
        p.restore();
        y += 14;
    }
}

constexpr double kPageW = 612, kPageH = 792;

// A page in points; the painter measures in points, as in a PDF. (Qt's PDF
// engine works in 1200ths of an inch by default; a scale makes it points.)
bool writeVector(const QString& file, const std::function<void(QPainter&, double)>& draw, int pages = 1)
{
    QPdfWriter writer(file);
    writer.setPageSize(QPageSize(QSizeF(kPageW, kPageH), QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(720);
    QPainter p;
    if (!p.begin(&writer)) return false;
    const double k = writer.width() / kPageW;
    p.scale(k, k);
    for (int i = 0; i < pages; ++i) {
        if (i > 0) writer.newPage();
        draw(p, 8.0);
    }
    p.end();
    return true;
}

void drawPlot(QPainter& p, const Axes& a, const QList<Curve>& curves, double unit, bool legend = false)
{
    drawAxes(p, a, unit);
    p.setClipRect(a.frame);
    for (const Curve& c : curves) drawCurve(p, a, c);
    p.setClipping(false);
    if (legend) drawLegend(p, a, curves, unit);
}

// The plot as a picture, placed on the page as a figure or a scan is: at
// \a dpi, turned by \a degrees about the page's middle.
QImage plotImage(const Axes& a, const QList<Curve>& curves, double dpi, bool legend = false)
{
    const double k = dpi / 72;
    QImage image(int(kPageW * k), int(kPageH * k), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.scale(k, k);
    drawPlot(p, a, curves, 8.0, legend);
    p.end();
    return image;
}

bool writeScan(const QString& file, const QImage& image, double degrees = 0, int shearPermille = 0, const std::function<void(QPainter&, double)>& after = nullptr)
{
    QPdfWriter writer(file);
    writer.setPageSize(QPageSize(QSizeF(kPageW, kPageH), QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(720);
    QPainter p;
    if (!p.begin(&writer)) return false;
    const double k = writer.width() / kPageW;
    p.scale(k, k);
    p.save();
    p.translate(kPageW / 2, kPageH / 2);
    p.rotate(degrees);
    if (shearPermille != 0) p.shear(shearPermille / 1000.0, 0);
    p.translate(-kPageW / 2, -kPageH / 2);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRectF(0, 0, kPageW, kPageH), image);
    p.restore();
    if (after) after(p, 8.0);
    p.end();
    return true;
}

// How far a series' values are from what a function gives at their x, in
// parts of the axis's span: the worst and the rms.
struct Errors {
    double worst = 0, rms = 0;
    int n = 0;
};

Errors errorsOf(const Plot& plot, const Series& s, const std::function<double(double)>& f, double ySpan, bool yLog = false)
{
    Errors e;
    double sum = 0;
    for (const QPointF& v : values(plot, s)) {
        if (!std::isfinite(v.x()) || !std::isfinite(v.y())) continue;
        const double d = yLog ? std::abs(std::log10(v.y()) - std::log10(f(v.x()))) / ySpan : std::abs(v.y() - f(v.x())) / ySpan;
        e.worst = std::max(e.worst, d);
        sum += d * d;
        ++e.n;
    }
    e.rms = e.n > 0 ? std::sqrt(sum / e.n) : 0;
    return e;
}

const Series* named(const Plot& plot, const QString& name)
{
    for (const Series& s : plot.series)
        if (s.name == name) return &s;
    return nullptr;
}

// The series whose values lie nearest a function: the one meant by it.
const Series* nearest(const Plot& plot, const std::function<double(double)>& f, double ySpan)
{
    const Series* best = nullptr;
    double least = 1e300;
    for (const Series& s : plot.series) {
        const Errors e = errorsOf(plot, s, f, ySpan);
        if (e.n >= 10 && e.rms < least) {
            least = e.rms;
            best = &s;
        }
    }
    return best;
}

Plot firstPlot(Source& source, int page = 0)
{
    const QList<Plot> plots = findPlots(source, page);
    if (plots.isEmpty()) return Plot{};
    return plots.first();
}

double sine(double x) { return std::sin(x); }
double cosine(double x) { return 0.5 * std::cos(1.7 * x); }

} // namespace

class TestPlotDigitizer : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir a_dir;
    QString path(const QString& name) const { return a_dir.filePath(name); }

    // Prints what a case measured, for the write-up.
    void report(const char* what, const Errors& e) const { qInfo("%-34s rms %.5f %% worst %.5f %% of the span, %d points", what, e.rms * 100, e.worst * 100, e.n); }

private slots:
    void initTestCase()
    {
        QVERIFY(a_dir.isValid());
        // (To look at what the tests wrote.)
        if (!qEnvironmentVariableIsEmpty("QUCS_KEEP_TEST_PDFS")) {
            a_dir.setAutoRemove(false);
            qInfo("The PDFs are in %s", qPrintable(a_dir.path()));
        }
    }

    // ----- Calibration ------------------------------------------------------

    void calibrationOfTwoAxesAlone()
    {
        Calibration c;
        for (const auto& r : {std::pair{QPointF(100, 400), 0.0}, {QPointF(300, 400), 5.0}, {QPointF(500, 400), 10.0}}) {
            Reference ref;
            ref.at = r.first;
            ref.hasX = true;
            ref.x = r.second;
            c.references << ref;
        }
        for (const auto& r : {std::pair{QPointF(100, 400), -1.0}, {QPointF(100, 120), 1.0}}) {
            Reference ref;
            ref.at = r.first;
            ref.hasY = true;
            ref.y = r.second;
            c.references << ref;
        }
        QString why;
        QVERIFY2(c.fit(&why), qPrintable(why));
        const QPointF v = c.toValue(QPointF(300, 260));
        QCOMPARE_LE(std::abs(v.x() - 5), 1e-9);
        QCOMPARE_LE(std::abs(v.y() - 0), 1e-9);
        const QPointF back = c.toPage(QPointF(5, 0));
        QCOMPARE_LE(QLineF(back, QPointF(300, 260)).length(), 1e-9);
        QCOMPARE_LE(c.rms(), 1e-9);
    }

    void calibrationLogAndReversed()
    {
        Calibration c;
        c.xScale = Scale::Log;
        c.yScale = Scale::Log;
        Reference a;
        a.at = QPointF(100, 100);
        a.hasX = a.hasY = true;
        a.x = 1000;      // reversed: a thousand at the left
        a.y = 1e-3;      // and a thousandth at the top
        Reference b;
        b.at = QPointF(500, 100);
        b.hasX = b.hasY = true;
        b.x = 1;
        b.y = 1e-3;
        Reference d;
        d.at = QPointF(100, 500);
        d.hasX = d.hasY = true;
        d.x = 1000;
        d.y = 1;
        c.references = {a, b, d};
        QString why;
        QVERIFY2(c.fit(&why), qPrintable(why));
        const QPointF v = c.toValue(QPointF(300, 300));
        QCOMPARE_LE(std::abs(std::log10(v.x()) - 1.5), 1e-9);
        QCOMPARE_LE(std::abs(std::log10(v.y()) + 1.5), 1e-9);
        QCOMPARE_LE(QLineF(c.toPage(v), QPointF(300, 300)).length(), 1e-6);
    }

    void calibrationOfATurnedAndShearedPage()
    {
        // Three corners fix an affine map; the fourth then has no error.
        QTransform t;
        t.rotate(7.5);
        t.shear(0.08, 0.02);
        t.translate(30, -12);
        const auto ref = [&](double x, double y) {
            Reference r;
            r.at = t.map(QPointF(100 + 40 * x, 400 - 28 * y));
            r.hasX = r.hasY = true;
            r.x = x;
            r.y = y;
            return r;
        };
        Calibration c;
        c.references = {ref(0, 0), ref(10, 0), ref(0, 10)};
        QString why;
        QVERIFY2(c.fit(&why), qPrintable(why));
        const QPointF v = c.toValue(t.map(QPointF(100 + 40 * 7, 400 - 28 * 3)));
        QCOMPARE_LE(std::abs(v.x() - 7), 1e-8);
        QCOMPARE_LE(std::abs(v.y() - 3), 1e-8);
        // Four: least squares, and a residual that says one is out.
        c.references << ref(10, 10);
        QVERIFY(c.fit(&why));
        QCOMPARE_LE(c.rms(), 1e-8);
        c.references[3].at += QPointF(3, 0);
        QVERIFY(c.fit(&why));
        QCOMPARE_GE(c.worst(), 0.5);
    }

    void calibrationProjective()
    {
        // A page photographed at an angle: four corners fix it exactly, an
        // affine map cannot.
        QTransform t(1, 0.05, 0.0004, 0.02, 1, 0.0003, 20, 30, 1);
        const auto ref = [&](double x, double y) {
            Reference r;
            r.at = t.map(QPointF(100 + 40 * x, 400 - 28 * y));
            r.hasX = r.hasY = true;
            r.x = x;
            r.y = y;
            return r;
        };
        Calibration c;
        c.model = Model::Projective;
        c.references = {ref(0, 0), ref(10, 0), ref(10, 10), ref(0, 10)};
        QString why;
        QVERIFY2(c.fit(&why), qPrintable(why));
        const QPointF v = c.toValue(t.map(QPointF(100 + 40 * 3, 400 - 28 * 7)));
        QCOMPARE_LE(std::abs(v.x() - 3), 1e-6);
        QCOMPARE_LE(std::abs(v.y() - 7), 1e-6);
        QCOMPARE_LE(QLineF(c.toPage(QPointF(3, 7)), t.map(QPointF(100 + 40 * 3, 400 - 28 * 7))).length(), 1e-6);
        // Three: not enough.
        c.references.removeLast();
        QVERIFY(!c.fit(&why));
        QVERIFY(why.contains(QStringLiteral("four")));
    }

    void calibrationPolar()
    {
        // A pattern in dB: -40 at the centre, 0 at the rim, 0° up, clockwise.
        const QPointF centre(300, 300);
        const double radius = 200;
        const auto ref = [&](double r, double theta) {
            Reference ref;
            const double rho = (r + 40) / 40 * radius, a = theta * M_PI / 180;
            ref.at = centre + QPointF(rho * std::sin(a), -rho * std::cos(a));
            ref.hasX = ref.hasY = true;
            ref.x = r;
            ref.y = theta;
            return ref;
        };
        Calibration c;
        c.kind = Kind::Polar;
        c.rAtCentre = -40;
        c.references = {ref(0, 0), ref(0, 90), ref(-40, 0)};
        QString why;
        QVERIFY2(c.fit(&why), qPrintable(why));
        const QPointF v = c.toValue(ref(-10, 135).at);
        QCOMPARE_LE(std::abs(v.x() + 10), 1e-8);
        QCOMPARE_LE(std::abs(v.y() - 135), 1e-8);
    }

    void calibrationThatCannotBe()
    {
        QString why;
        Calibration none;
        QVERIFY(!none.fit(&why));
        QVERIFY(!none.valid());
        QVERIFY(std::isnan(none.toValue(QPointF(1, 1)).x()));

        // One place twice.
        Calibration same;
        for (int i = 0; i < 2; ++i) {
            Reference r;
            r.at = QPointF(10, 10);
            r.hasX = r.hasY = true;
            r.x = i;
            r.y = i;
            same.references << r;
        }
        QVERIFY(!same.fit(&why));

        // One value everywhere.
        Calibration flat;
        for (const QPointF& at : {QPointF(10, 10), QPointF(100, 10), QPointF(10, 100)}) {
            Reference r;
            r.at = at;
            r.hasX = r.hasY = true;
            r.x = 3;
            r.y = at.y();
            flat.references << r;
        }
        QVERIFY(!flat.fit(&why));

        // A value a log axis cannot have.
        Calibration log;
        log.xScale = Scale::Log;
        for (const double x : {0.0, 10.0}) {
            Reference r;
            r.at = QPointF(100 + x, 50);
            r.hasX = true;
            r.x = x;
            log.references << r;
        }
        for (const double y : {0.0, 10.0}) {
            Reference r;
            r.at = QPointF(100, 50 + y);
            r.hasY = true;
            r.y = y;
            log.references << r;
        }
        QVERIFY(!log.fit(&why));
        QVERIFY2(why.contains(QStringLiteral("log")), qPrintable(why));

        // Both axes along the same direction.
        Calibration parallel;
        for (const QPointF& at : {QPointF(10, 10), QPointF(100, 10)}) {
            Reference r;
            r.at = at;
            r.hasX = r.hasY = true;
            r.x = at.x();
            r.y = at.x();
            parallel.references << r;
        }
        QVERIFY(!parallel.fit(&why));
    }

    // ----- Tick labels ------------------------------------------------------

    void tickLabels_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("isNumber");
        QTest::addColumn<double>("value");
        QTest::newRow("whole") << "10" << true << 10.0;
        QTest::newRow("decimal") << "0.25" << true << 0.25;
        QTest::newRow("unicode minus") << QStringLiteral("−1.5") << true << -1.5;
        QTest::newRow("hyphen") << "-3" << true << -3.0;
        QTest::newRow("en dash") << QStringLiteral("–2") << true << -2.0;
        QTest::newRow("exponent") << "1e-3" << true << 1e-3;
        QTest::newRow("exponent E") << "2.5E+04" << true << 2.5e4;
        QTest::newRow("caret") << "10^3" << true << 1e3;
        QTest::newRow("caret minus") << QStringLiteral("10^−3") << true << 1e-3;
        QTest::newRow("superscript") << QStringLiteral("10⁻³") << true << 1e-3;
        QTest::newRow("times") << QStringLiteral("2×10^5") << true << 2e5;
        QTest::newRow("times superscript") << QStringLiteral("1.5×10⁵") << true << 1.5e5;
        QTest::newRow("kilo") << "100k" << true << 1e5;
        QTest::newRow("mega") << "1M" << true << 1e6;
        QTest::newRow("milli") << "100m" << true << 0.1;
        QTest::newRow("micro") << QStringLiteral("10µ") << true << 1e-5;
        QTest::newRow("thousands") << "1,000" << true << 1000.0;
        QTest::newRow("comma decimal") << "0,5" << true << 0.5;
        QTest::newRow("GHz") << "2.4GHz" << true << 2.4e9;
        QTest::newRow("percent") << "50%" << true << 50.0;
        QTest::newRow("dB") << "-3dB" << true << -3.0;
        QTest::newRow("word") << "Figure" << false << 0.0;
        QTest::newRow("panel") << "(a)" << false << 0.0;
        QTest::newRow("empty") << "" << false << 0.0;
        QTest::newRow("two numbers") << "1 2 3 x" << false << 0.0;
        QTest::newRow("too long") << QString(100, QLatin1Char('1')) << false << 0.0;
        QTest::newRow("huge") << "1e999" << false << 0.0;
        // (A sentence's "VOL. 25, NO. 8," is no tick label.)
        QTest::newRow("trailing comma") << "8," << false << 0.0;
        QTest::newRow("trailing comma two digits") << "25," << false << 0.0;
        QTest::newRow("trailing comma unit") << "5V," << false << 0.0;
    }

    void tickLabels()
    {
        QFETCH(QString, text);
        QFETCH(bool, isNumber);
        QFETCH(double, value);
        double v = 0;
        QCOMPARE(parseTickLabel(text, &v), isNumber);
        if (isNumber) QCOMPARE_LE(std::abs(v - value), std::abs(value) * 1e-12);
    }

    // ----- Vector art -------------------------------------------------------

    void vectorPlot()
    {
        const QString file = path(QStringLiteral("vector.pdf"));
        Axes a;
        a.grid = true;
        const QList<Curve> curves{Curve{sine, Qt::blue, 1.2, Qt::SolidLine, QStringLiteral("sine")},
                                  Curve{cosine, Qt::red, 1.2, Qt::SolidLine, QStringLiteral("cosine")}};
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, curves, u, true); }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        QVERIFY2(source.hasVectors(), qPrintable(source.vectorProblem()));
        const QList<Plot> plots = findPlots(source, 0);
        QCOMPARE(plots.size(), 1);
        Plot plot = plots.first();
        QVERIFY(plot.calibration.valid());
        QCOMPARE_LE(plot.calibration.rms(), 0.05);
        QCOMPARE(plot.xAxis.scale, Scale::Linear);
        QCOMPARE(plot.yAxis.scale, Scale::Linear);
        QVERIFY(extract(source, plot, Options{}));
        QCOMPARE(plot.route, QStringLiteral("vector"));
        QCOMPARE(plot.series.size(), 2);
        const Series* s = named(plot, QStringLiteral("sine"));
        const Series* c = named(plot, QStringLiteral("cosine"));
        QVERIFY2(s != nullptr && c != nullptr, "the legend's names");
        // The file's own vertices: exact to what QPdfWriter rounded them to.
        const Errors es = errorsOf(plot, *s, sine, 2), ec = errorsOf(plot, *c, cosine, 2);
        report("vector, linear, sine", es);
        report("vector, linear, cosine", ec);
        QCOMPARE_LE(es.worst, 0.0005);
        QCOMPARE_LE(ec.worst, 0.0005);
        QCOMPARE_GE(es.n, 250);
        QCOMPARE(s->colour.name(), QColor(Qt::blue).name());
        QCOMPARE(c->colour.name(), QColor(Qt::red).name());
        QCOMPARE_GE(s->agreement, 0.95);
        const Trust t = assess(plot);
        QCOMPARE(t.level, QStringLiteral("high"));
        QCOMPARE_LE(t.totalPoints, 0.1);
    }

    void vectorLogAxes()
    {
        const QString file = path(QStringLiteral("log.pdf"));
        Axes a;
        a.xLog = a.yLog = true;
        a.xMin = 1;
        a.xMax = 1e6;
        a.yMin = 1e-3;
        a.yMax = 10;
        a.xTicks = {1, 10, 100, 1e3, 1e4, 1e5, 1e6};
        a.yTicks = {1e-3, 1e-2, 1e-1, 1, 10};
        a.format = [](double v) { return QStringLiteral("1e%1").arg(int(std::round(std::log10(v)))); };
        const auto lpf = [](double f) { return 1 / std::sqrt(1 + std::pow(f / 1e3, 2)); };
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {Curve{lpf, Qt::black, 1.0}}, u); }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QCOMPARE(plot.xAxis.scale, Scale::Log);
        QCOMPARE(plot.yAxis.scale, Scale::Log);
        QVERIFY(extract(source, plot, Options{}));
        QCOMPARE(plot.series.size(), 1);
        const Errors e = errorsOf(plot, plot.series.first(), lpf, 4, true);
        report("vector, log-log", e);
        QCOMPARE_LE(e.worst, 0.0005);
    }

    void vectorKiloLabelsAndReversedAxis()
    {
        const QString file = path(QStringLiteral("reversed.pdf"));
        Axes a;
        a.xReversed = true;   // falling x, as a wavenumber axis or a depth
        a.xMin = 0;
        a.xMax = 5000;
        a.xTicks = {0, 1000, 2000, 3000, 4000, 5000};
        a.format = [](double v) {
            if (std::abs(v) >= 1000) return QStringLiteral("%1k").arg(v / 1000);
            return QString::number(v, 'g', 6).replace(QLatin1Char('-'), QChar(0x2212));
        };
        const auto f = [](double x) { return std::sin(x / 800); };
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {Curve{f, Qt::darkGreen, 1.0}}, u); }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QVERIFY(extract(source, plot, Options{}));
        const Errors e = errorsOf(plot, plot.series.first(), f, 2);
        report("vector, reversed x, 1k labels", e);
        QCOMPARE_LE(e.worst, 0.0005);
        // The left of the page is the high end.
        const QPointF left = plot.calibration.toValue(QPointF(a.frame.left(), a.frame.center().y()));
        QCOMPARE_LE(std::abs(left.x() - 5000), 2);
    }

    void vectorDashedLineAndMarkers()
    {
        const QString file = path(QStringLiteral("styles.pdf"));
        Axes a;
        a.xMin = 0.5;
        a.xMax = 9.5;
        a.xTicks = {1, 3, 5, 7, 9};
        const auto scatter = [](double x) { return 0.8 * std::sin(0.7 * x) - 0.1; };
        Curve dashed{sine, Qt::red, 1.2, Qt::DashLine, QStringLiteral("dashed")};
        Curve dots{scatter, QColor(0, 120, 0), 1, Qt::SolidLine, QStringLiteral("marked"), 19, true};
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {dashed, dots}, u, true); }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QVERIFY(extract(source, plot, Options{}));
        const Series* d = nullptr;
        const Series* m = nullptr;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape == Series::Shape::Markers) m = &s;
            if (s.shape == Series::Shape::Line) d = &s;
        }
        QVERIFY2(d != nullptr, "the dashed line");
        QVERIFY2(m != nullptr, "the markers");
        QVERIFY(d->dashed);
        const Errors ed = errorsOf(plot, *d, sine, 2), em = errorsOf(plot, *m, scatter, 2);
        report("vector, dashed", ed);
        report("vector, markers", em);
        QCOMPARE_LE(ed.worst, 0.0005);
        QCOMPARE_LE(em.worst, 0.002);
        QCOMPARE(m->points.size(), 19);
    }

    void severalPlotsOnAPage()
    {
        const QString file = path(QStringLiteral("two.pdf"));
        Axes top;
        top.frame = QRectF(100, 80, 400, 200);
        Axes bottom;
        bottom.frame = QRectF(100, 400, 400, 200);
        bottom.xMin = 0;
        bottom.xMax = 100;
        bottom.xTicks = {0, 25, 50, 75, 100};
        bottom.yMin = 0;
        bottom.yMax = 5;
        bottom.yTicks = {0, 1, 2, 3, 4, 5};
        const auto rise = [](double x) { return 5 * (1 - std::exp(-x / 30)); };
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            drawPlot(p, top, {Curve{sine, Qt::blue}}, u);
            drawPlot(p, bottom, {Curve{rise, Qt::red}}, u);
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        const QList<Plot> plots = findPlots(source, 0);
        QCOMPARE(plots.size(), 2);
        Plot a = plots.at(0), b = plots.at(1);
        QVERIFY(a.calibration.valid() && b.calibration.valid());
        QVERIFY(extract(source, a, Options{}));
        QVERIFY(extract(source, b, Options{}));
        QCOMPARE_LE(errorsOf(a, a.series.first(), sine, 2).worst, 0.0005);
        QCOMPARE_LE(errorsOf(b, b.series.first(), rise, 5).worst, 0.0005);
        // A region picks one.
        Plot only = plotInRegion(source, 0, QRectF(60, 360, 480, 280));
        QVERIFY(only.calibration.valid());
        QVERIFY(extract(source, only, Options{}));
        QCOMPARE_LE(errorsOf(only, only.series.first(), rise, 5).worst, 0.0005);
    }

    void clippedAtTheFrame()
    {
        // A curve that leaves the frame: what is outside was never shown.
        const QString file = path(QStringLiteral("clipped.pdf"));
        Axes a;
        a.yMin = -0.5;
        a.yMax = 0.5;
        a.yTicks = {-0.5, -0.25, 0, 0.25, 0.5};
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {Curve{sine, Qt::blue}}, u); }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(extract(source, plot, Options{}));
        const Series* s = nearest(plot, sine, 1);
        QVERIFY(s != nullptr);
        for (const QPointF& v : values(plot, *s)) QVERIFY2(std::abs(v.y()) <= 0.51, qPrintable(QString::number(v.y())));
        QCOMPARE_LE(errorsOf(plot, *s, sine, 1).worst, 0.001);
        QVERIFY(!s->breaks.isEmpty());
    }

    // ----- Pictures ---------------------------------------------------------

    void embeddedPicture()
    {
        const QString file = path(QStringLiteral("picture.pdf"));
        Axes a;
        a.grid = true;
        const QList<Curve> curves{Curve{sine, Qt::blue, 1.2}, Curve{cosine, Qt::red, 1.2}};
        // Its labels stay vector text over the picture, as a figure pasted
        // into a document whose axes were then typeset.
        Axes labelsOnly = a;
        QVERIFY(writeScan(file, plotImage(a, curves, 300), 0, 0, [&](QPainter& p, double u) {
            p.save();
            p.scale(1 / u, 1 / u);
            p.setPen(Qt::black);
            p.setFont(plotFont(9, u));
            for (const double t : a.xTicks) p.drawText(QRectF((QPointF(a.px(t) - 30, a.frame.bottom() + 20)) * u, QSizeF(60, 12) * u), Qt::AlignHCenter | Qt::AlignTop, label(t));
            for (const double t : a.yTicks) p.drawText(QRectF((QPointF(a.frame.left() - 80, a.py(t) - 7)) * u, QSizeF(60, 14) * u), Qt::AlignRight | Qt::AlignVCenter, label(t));
            p.restore();
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        QVERIFY(source.graphics(0).pictures.size() >= 1);
        // (Its own labels were not drawn in the picture? They are, and the
        // vector labels sit further out; the picture's alone would not be read.)
        Plot plot = firstPlot(source);
        QVERIFY2(!plot.frame.isNull(), "a frame found in the picture");
        QCOMPARE(plot.route, QStringLiteral("raster"));
        QCOMPARE_LE(QLineF(plot.frame.topLeft(), a.frame.topLeft()).length(), 1.0);
        QVERIFY2(plot.calibration.valid(), qPrintable(plot.notes.join(QLatin1Char(' '))));
        QCOMPARE_LE(plot.calibration.rms(), 0.5);
        QVERIFY(extract(source, plot, Options{}));
        QCOMPARE(plot.route, QStringLiteral("raster"));
        const Series* s = nearest(plot, sine, 2);
        const Series* c = nearest(plot, cosine, 2);
        QVERIFY(s != nullptr && c != nullptr);
        QVERIFY(s != c);
        const Errors es = errorsOf(plot, *s, sine, 2), ec = errorsOf(plot, *c, cosine, 2);
        report("picture 300 dpi, sine", es);
        report("picture 300 dpi, cosine", ec);
        QCOMPARE_LE(es.rms, 0.002);
        QCOMPARE_LE(ec.rms, 0.002);
        QCOMPARE_LE(es.worst, 0.01);
        QCOMPARE_LE(ec.worst, 0.01);
        QCOMPARE_GE(es.n, 100);
    }

    void pictureOfDashesAndMarkers()
    {
        const QString file = path(QStringLiteral("picture_styles.pdf"));
        Axes a;
        a.xMin = 0.5;
        a.xMax = 9.5;
        a.xTicks = {1, 3, 5, 7, 9};
        a.labels = false;
        const auto scatter = [](double x) { return 0.8 * std::sin(0.7 * x) - 0.1; };
        Curve dashed{sine, Qt::red, 1.2, Qt::DashLine};
        // (Whole marks: half of one at the frame's edge is no mark for a picture.)
        Curve dots{scatter, QColor(0, 120, 0), 1, Qt::SolidLine, QString(), 17, true, 1, 9};
        QVERIFY(writeScan(file, plotImage(a, {dashed, dots}, 300)));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY2(!plot.frame.isNull(), "a frame");
        QVERIFY2(!plot.calibration.valid(), "a page with no labels has none");
        // By the tick marks found, the values Claude can read off the picture.
        QString why;
        QCOMPARE(plot.xAxis.marks.size(), 5);
        QCOMPARE(plot.yAxis.marks.size(), 5);
        QVERIFY2(assignTickValues(plot, true, {1, 3, 5, 7, 9}, &why), qPrintable(why));
        QVERIFY2(assignTickValues(plot, false, {1, 0.5, 0, -0.5, -1}, &why), qPrintable(why));
        QVERIFY2(calibrateFromAxes(plot, &why), qPrintable(why));
        QCOMPARE_LE(plot.calibration.rms(), 0.2);
        QVERIFY(extract(source, plot, Options{}));
        const Series* d = nullptr;
        const Series* m = nullptr;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape == Series::Shape::Markers) m = &s;
            if (s.shape == Series::Shape::Line && s.points.size() > 20) d = &s;
        }
        QVERIFY2(m != nullptr, "the markers");
        QCOMPARE(m->points.size(), 17);
        const Errors em = errorsOf(plot, *m, scatter, 2);
        report("picture, markers", em);
        QCOMPARE_LE(em.worst, 0.005);
        QVERIFY2(d != nullptr, "the dashed line");
        const Errors ed = errorsOf(plot, *d, sine, 2);
        report("picture, dashed", ed);
        QCOMPARE_LE(ed.rms, 0.003);
        QCOMPARE_LE(ed.worst, 0.015);
        QCOMPARE_GE(d->bridged, 5);
        QVERIFY(d->dashed);
    }

    void turnedScan()
    {
        // A page scanned a little askew, no text on it.
        const QString file = path(QStringLiteral("turned.pdf"));
        Axes a;
        a.grid = false;
        a.labels = false;
        const auto f = [](double x) { return 0.8 * std::sin(x * 0.9) * std::exp(-x / 12); };
        QVERIFY(writeScan(file, plotImage(a, {Curve{f, Qt::black, 1.2}}, 300), 2.4));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY2(!plot.frame.isNull(), "a frame found on the turned page");
        QCOMPARE_LE(std::abs(plot.angle - 2.4), 0.1);
        QCOMPARE(plot.xAxis.marks.size(), 6);
        QCOMPARE(plot.yAxis.marks.size(), 5);
        QString why;
        QVERIFY2(assignTickValues(plot, true, {0, 2, 4, 6, 8, 10}, &why), qPrintable(why));
        QVERIFY2(assignTickValues(plot, false, {1, 0.5, 0, -0.5, -1}, &why), qPrintable(why));
        QVERIFY2(calibrateFromAxes(plot, &why), qPrintable(why));
        QVERIFY(extract(source, plot, Options{}));
        const Series* s = nearest(plot, f, 2);
        QVERIFY(s != nullptr);
        const Errors e = errorsOf(plot, *s, f, 2);
        report("scan turned 2.4 degrees", e);
        QCOMPARE_LE(e.rms, 0.003);
        QCOMPARE_LE(e.worst, 0.012);
    }

    void shearedScanByReferences()
    {
        // Sheared: its axes are not square, so no frame is found. Four points
        // whose values are known (given by hand, or by Claude from a picture).
        const QString file = path(QStringLiteral("sheared.pdf"));
        Axes a;
        a.labels = false;
        const auto f = [](double x) { return 0.6 * std::cos(x * 0.8); };
        constexpr double degrees = 3.0;
        constexpr int shear = 60;
        QVERIFY(writeScan(file, plotImage(a, {Curve{f, Qt::blue, 1.2}}, 300), degrees, shear));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        QTransform t;
        t.translate(kPageW / 2, kPageH / 2);
        t.rotate(degrees);
        t.shear(shear / 1000.0, 0);
        t.translate(-kPageW / 2, -kPageH / 2);
        QRectF region = t.mapRect(a.frame).adjusted(-6, -6, 6, 6);
        Plot plot = plotInRegion(source, 0, region);
        QVERIFY(plot.frame.isNull());
        plot.calibration = Calibration{};
        for (const auto& corner : {std::pair{0.0, -1.0}, {10.0, -1.0}, {10.0, 1.0}, {0.0, 1.0}}) {
            Reference r;
            r.at = t.map(a.at(corner.first, corner.second));
            r.hasX = r.hasY = true;
            r.x = corner.first;
            r.y = corner.second;
            plot.calibration.references << r;
        }
        QString why;
        QVERIFY2(plot.calibration.fit(&why), qPrintable(why));
        QCOMPARE_LE(plot.calibration.rms(), 0.05);
        // A seed on the curve: what Claude or the user points at.
        Options o;
        o.seeds << t.map(a.at(4, f(4)));
        QVERIFY2(extract(source, plot, o, &why), qPrintable(why));
        QCOMPARE(plot.series.size(), 1);
        const Errors e = errorsOf(plot, plot.series.first(), f, 2);
        report("scan sheared, by references", e);
        // (PDFium draws a sheared image as much as 2 points from where the file
        // puts it, as the frame's corners in its picture show. These references
        // are where the file puts them; clicked on the picture they would agree
        // with it. So this bound is PDFium's more than the trace's.)
        QCOMPARE_LE(e.rms, 0.006);
        QCOMPARE_LE(e.worst, 0.012);
        QCOMPARE_GE(e.n, 100);
    }

    void crossingCurvesInAPicture()
    {
        const QString file = path(QStringLiteral("crossing.pdf"));
        Axes a;
        a.labels = false;
        const auto up = [](double x) { return -0.8 + 0.16 * x; };
        const auto down = [](double x) { return 0.8 - 0.16 * x; };
        // Same colour: only the direction of the line tells them apart.
        QVERIFY(writeScan(file, plotImage(a, {Curve{up, Qt::black, 1.2}, Curve{down, Qt::black, 1.2}}, 300)));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QString why;
        QVERIFY(assignTickValues(plot, true, {0, 2, 4, 6, 8, 10}, &why));
        QVERIFY(assignTickValues(plot, false, {1, 0.5, 0, -0.5, -1}, &why));
        QVERIFY(calibrateFromAxes(plot, &why));
        QVERIFY(extract(source, plot, Options{}));
        const Series* u = nearest(plot, up, 2);
        const Series* d = nearest(plot, down, 2);
        QVERIFY(u != nullptr && d != nullptr);
        QVERIFY2(u != d, "two lines, not one");
        const Errors eu = errorsOf(plot, *u, up, 2), ed = errorsOf(plot, *d, down, 2);
        report("crossing, same colour, rising", eu);
        report("crossing, same colour, falling", ed);
        QCOMPARE_LE(eu.worst, 0.01);
        QCOMPARE_LE(ed.worst, 0.01);
        // Each goes the whole way across.
        const QList<QPointF> vu = values(plot, *u);
        double low = 1e9, high = -1e9;
        for (const QPointF& v : vu) {
            low = std::min(low, v.x());
            high = std::max(high, v.x());
        }
        QCOMPARE_LE(low, 0.3);
        QCOMPARE_GE(high, 9.7);
    }

    void aLoopInAPicture()
    {
        // A hysteresis loop: x goes both ways, so no column-by-column trace
        // would do.
        const QString file = path(QStringLiteral("loop.pdf"));
        Axes a;
        a.labels = false;
        a.xMin = -1.5;
        a.xMax = 1.5;
        a.xTicks = {-1, 0, 1};
        a.yMin = -1.5;
        a.yMax = 1.5;
        a.yTicks = {-1, 0, 1};
        const auto draw = [&] {
            const double k = 300.0 / 72;
            QImage image(int(kPageW * k), int(kPageH * k), QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(k, k);
            drawAxes(p, a, 8);
            QPolygonF loop;
            for (int i = 0; i <= 400; ++i) {
                const double t = i * 2 * M_PI / 400;
                loop << a.at(std::cos(t), std::sin(t + 0.5));
            }
            p.setPen(QPen(Qt::blue, 1.2));
            p.drawPolyline(loop);
            p.end();
            return image;
        };
        QVERIFY(writeScan(file, draw()));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QString why;
        QVERIFY2(assignTickValues(plot, true, {-1, 0, 1}, &why), qPrintable(why));
        QVERIFY2(assignTickValues(plot, false, {1, 0, -1}, &why), qPrintable(why));
        QVERIFY(calibrateFromAxes(plot, &why));
        QVERIFY(extract(source, plot, Options{}));
        QCOMPARE(plot.series.size(), 1);
        const QList<QPointF> v = values(plot, plot.series.first());
        QCOMPARE_GE(v.size(), 100);
        // Every point on the ellipse: x = cos t, y = sin(t + 0.5).
        double worst = 0;
        for (const QPointF& q : v) {
            double best = 1e9;
            for (int i = 0; i < 2000; ++i) {
                const double t = i * 2 * M_PI / 2000;
                best = std::min(best, std::hypot(q.x() - std::cos(t), q.y() - std::sin(t + 0.5)));
            }
            worst = std::max(worst, best);
        }
        qInfo("%-34s worst %.5f of a span of 3", "loop in a picture", worst);
        QCOMPARE_LE(worst / 3, 0.006);
        // It goes the whole way round.
        double xLow = 1e9, xHigh = -1e9, yLow = 1e9, yHigh = -1e9;
        for (const QPointF& q : v) {
            xLow = std::min(xLow, q.x());
            xHigh = std::max(xHigh, q.x());
            yLow = std::min(yLow, q.y());
            yHigh = std::max(yHigh, q.y());
        }
        QCOMPARE_LE(xLow, -0.97);
        QCOMPARE_GE(xHigh, 0.97);
        QCOMPARE_LE(yLow, -0.97);
        QCOMPARE_GE(yHigh, 0.97);
    }

    void legendNamesInAPicture()
    {
        const QString file = path(QStringLiteral("legend_picture.pdf"));
        Axes a;
        a.labels = false;
        const QList<Curve> curves{Curve{sine, Qt::blue, 1.2, Qt::SolidLine, QStringLiteral("alpha")},
                                  Curve{cosine, Qt::red, 1.2, Qt::SolidLine, QStringLiteral("beta")}};
        // The names as text over the picture, the samples in it.
        QVERIFY(writeScan(file, [&] {
            Axes bare = a;
            QImage image = plotImage(bare, curves, 300);
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            const double k = 300.0 / 72;
            p.scale(k, k);
            double y = a.frame.top() + 14;
            for (const Curve& c : curves) {
                QPen pen(c.colour, c.width);
                pen.setCapStyle(Qt::FlatCap);
                p.setPen(pen);
                p.drawLine(QPointF(a.frame.right() - 120, y), QPointF(a.frame.right() - 96, y));
                y += 14;
            }
            p.end();
            return image;
        }(), 0, 0, [&](QPainter& p, double u) {
            p.save();
            p.scale(1 / u, 1 / u);
            p.setPen(Qt::black);
            p.setFont(plotFont(9, u));
            double y = a.frame.top() + 14;
            for (const Curve& c : curves) {
                p.drawText(QRectF(QPointF(a.frame.right() - 92, y - 7) * u, QSizeF(90, 14) * u), Qt::AlignLeft | Qt::AlignVCenter, c.name);
                y += 14;
            }
            p.restore();
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(!plot.frame.isNull());
        QVERIFY(extract(source, plot, Options{}));
        const Series* s = named(plot, QStringLiteral("alpha"));
        const Series* c = named(plot, QStringLiteral("beta"));
        QVERIFY2(s != nullptr && c != nullptr, "both named");
        QCOMPARE_LE(colourDistanceTo(s->colour, Qt::blue), 120);
        QCOMPARE_LE(colourDistanceTo(c->colour, Qt::red), 120);
    }

    void aColourAsked()
    {
        const QString file = path(QStringLiteral("two_colours.pdf"));
        Axes a;
        a.labels = false;
        const QList<Curve> curves{Curve{sine, Qt::blue, 1.2}, Curve{cosine, Qt::red, 1.2}};
        QVERIFY(writeScan(file, plotImage(a, curves, 300)));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QString why;
        QVERIFY(assignTickValues(plot, true, {0, 2, 4, 6, 8, 10}, &why));
        QVERIFY(assignTickValues(plot, false, {1, 0.5, 0, -0.5, -1}, &why));
        QVERIFY(calibrateFromAxes(plot, &why));
        Options o;
        o.colours << QColor(Qt::red);
        QVERIFY(extract(source, plot, o));
        QCOMPARE(plot.series.size(), 1);
        QCOMPARE_LE(errorsOf(plot, plot.series.first(), cosine, 2).rms, 0.002);
    }

    void polarPlot_data()
    {
        QTest::addColumn<bool>("picture");
        QTest::newRow("vector") << false;
        QTest::newRow("picture") << true;
    }

    void polarPlot()
    {
        // A pattern in dB, -40 at the centre and 0 at the rim, 0 degrees up and
        // clockwise; circles and spokes for its grid, as such a plot has.
        QFETCH(bool, picture);
        const QPointF centre(306, 300);
        const double radius = 200;
        const auto pattern = [](double theta) { return -20 + 15 * std::cos(2 * theta * M_PI / 180); };
        const auto toPage = [&](double r, double theta) {
            const double rho = (r + 40) / 40 * radius, a = theta * M_PI / 180;
            return centre + QPointF(rho * std::sin(a), -rho * std::cos(a));
        };
        const auto draw = [&](QPainter& p) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(180, 180, 180), 0.6));
            for (const double r : {-30.0, -20.0, -10.0, 0.0}) p.drawEllipse(centre, (r + 40) / 40 * radius, (r + 40) / 40 * radius);
            for (int a = 0; a < 360; a += 30) p.drawLine(centre, toPage(0, a));
            QPolygonF line;
            for (int i = 0; i <= 720; ++i) line << toPage(pattern(i / 2.0), i / 2.0);
            p.setPen(QPen(Qt::blue, 1.2));
            p.drawPolyline(line);
        };
        const QString file = path(picture ? QStringLiteral("polar_picture.pdf") : QStringLiteral("polar.pdf"));
        if (picture) {
            const double k = 300.0 / 72;
            QImage image(int(kPageW * k), int(kPageH * k), QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(k, k);
            draw(p);
            p.end();
            QVERIFY(writeScan(file, image));
        } else {
            QVERIFY(writeVector(file, [&](QPainter& p, double) { draw(p); }));
        }
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = plotInRegion(source, 0, QRectF(centre - QPointF(radius + 6, radius + 6), QSizeF(2 * radius + 12, 2 * radius + 12)));
        QVERIFY(plot.frame.isNull());
        plot.calibration = Calibration{};
        plot.calibration.kind = Kind::Polar;
        plot.calibration.rAtCentre = -40;
        for (const auto& corner : {std::pair{0.0, 0.0}, {0.0, 90.0}, {0.0, 180.0}, {0.0, 270.0}, {-40.0, 0.0}}) {
            Reference r;
            r.at = toPage(corner.first, corner.second);
            r.hasX = r.hasY = true;
            r.x = corner.first;
            r.y = corner.second;
            plot.calibration.references << r;
        }
        QString why;
        QVERIFY2(plot.calibration.fit(&why), qPrintable(why));
        Options o;
        o.colours << QColor(Qt::blue);
        QVERIFY2(extract(source, plot, o, &why), qPrintable(why));
        QVERIFY(!plot.series.isEmpty());
        // Its points on the pattern, all the way round.
        const Series& s = plot.series.first();
        double worst = 0, sum = 0;
        QList<int> seen(36, 0);
        int n = 0;
        for (const QPointF& v : values(plot, s)) {
            const double error = std::abs(v.x() - pattern(v.y()));
            worst = std::max(worst, error);
            sum += error * error;
            ++n;
            seen[qBound(0, int((v.y() + 180) / 10), 35)] = 1;
        }
        qInfo("%-34s rms %.4f dB worst %.4f dB, %d points", picture ? "polar, picture" : "polar, vector", std::sqrt(sum / std::max(1, n)), worst, n);
        QCOMPARE_LE(worst, picture ? 0.6 : 0.05);
        QCOMPARE_GE(std::count(seen.begin(), seen.end(), 1), 34);
    }

    void barsAndAreasFromTheFile()
    {
        const QString file = path(QStringLiteral("bars.pdf"));
        Axes a;
        a.xMin = 0;
        a.xMax = 10;
        a.yMin = 0;
        a.yMax = 10;
        a.yTicks = {0, 2, 4, 6, 8, 10};
        const QList<double> heights{3, 5, 8, 2, 6};
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            drawAxes(p, a, u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(200, 80, 0));
            for (int i = 0; i < heights.size(); ++i) p.drawRect(QRectF(QPointF(a.px(1.5 + 1.5 * i), a.py(heights.at(i))), QPointF(a.px(2.5 + 1.5 * i), a.py(0))));
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QVERIFY(extract(source, plot, Options{}));
        const Series* bars = nullptr;
        for (const Series& s : std::as_const(plot.series))
            if (s.shape == Series::Shape::Bars) bars = &s;
        QVERIFY2(bars != nullptr, "the bars");
        const QList<QPointF> v = values(plot, *bars);
        QCOMPARE(v.size(), heights.size());
        for (int i = 0; i < v.size(); ++i) {
            QCOMPARE_LE(std::abs(v.at(i).x() - (2 + 1.5 * i)), 0.01);
            QCOMPARE_LE(std::abs(v.at(i).y() - heights.at(i)), 0.01);
        }
    }

    void aSecondAxisIsSaid()
    {
        const QString file = path(QStringLiteral("second_axis.pdf"));
        Axes a;
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            drawPlot(p, a, {Curve{sine, Qt::blue}}, u);
            p.save();
            p.scale(1 / u, 1 / u);
            p.setPen(Qt::black);
            p.setFont(plotFont(9, u));
            for (int i = 0; i < a.yTicks.size(); ++i)
                p.drawText(QRectF(QPointF(a.frame.right() + 6, a.py(a.yTicks.at(i)) - 7) * u, QSizeF(60, 14) * u), Qt::AlignLeft | Qt::AlignVCenter, QString::number(100 * i));
            p.restore();
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        const Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QVERIFY2(plot.notes.join(QLatin1Char(' ')).contains(QStringLiteral("second")), qPrintable(plot.notes.join(QLatin1Char(' '))));
    }

    void theReaderAgreesWithThePage_data()
    {
        QTest::addColumn<QByteArray>("box");
        QTest::newRow("plain") << QByteArray();
        QTest::newRow("turned 90") << QByteArray("/Rotate 90");
        QTest::newRow("turned 180") << QByteArray("/Rotate 180");
        QTest::newRow("turned 270") << QByteArray("/Rotate 270");
        QTest::newRow("cropped") << QByteArray("/CropBox [60 100 560 720]");
        QTest::newRow("cropped and turned") << QByteArray("/CropBox [60 100 560 720] /Rotate 90");
        QTest::newRow("a user unit") << QByteArray("/UserUnit 2");
    }

    void theReaderAgreesWithThePage()
    {
        // The reader and pdfium read one page: its zigzag's vertices fall on
        // what pdfium draws, however the page is boxed or turned.
        QFETCH(QByteArray, box);
        QByteArray content = "1 0 0 RG 1.5 w 100 200 m ";
        for (int i = 1; i <= 40; ++i) content += QByteArray::number(100 + i * 10) + ' ' + QByteArray::number(i % 2 == 0 ? 200 : 500) + " l ";
        content += "S\n";
        const QByteArray file = "%PDF-1.5\n1 0 obj <</Type /Catalog /Pages 2 0 R>> endobj\n2 0 obj <</Type /Pages /Kids [3 0 R] /MediaBox [0 0 612 792]>> endobj\n"
                                "3 0 obj <</Type /Page /Parent 2 0 R /Contents 4 0 R " + box + ">> endobj\n"
                                "4 0 obj <</Length " + QByteArray::number(content.size()) + ">> stream\n" + content + "endstream endobj\ntrailer <</Root 1 0 R>>\n";
        const QString name = path(QStringLiteral("agree_%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag()).replace(QLatin1Char(' '), QLatin1Char('_'))));
        QFile out(name);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(file);
        out.close();
        QPdfDocument pdf;
        QCOMPARE(pdf.load(name), QPdfDocument::Error::None);
        Source source(&pdf, name);
        QVERIFY2(source.hasVectors(), qPrintable(source.vectorProblem()));
        const vec::Graphics& g = source.graphics(0);
        QCOMPARE(g.size, pdf.pagePointSize(0));
        QCOMPARE(g.paths.size(), 1);
        const QPolygonF points = g.paths.first().parts.first().points;
        QCOMPARE(points.size(), 41);
        const double scale = 3;
        const QImage image = source.render(0, QRectF(), scale);
        QVERIFY(!image.isNull());
        int inked = 0, asked = 0;
        for (const QPointF& q : points) {
            const int x = int(q.x() * scale), y = int(q.y() * scale);
            if (x < 4 || y < 4 || x >= image.width() - 4 || y >= image.height() - 4) continue;
            ++asked;
            bool hit = false;
            for (int dy = -3; dy <= 3 && !hit; ++dy)
                for (int dx = -3; dx <= 3 && !hit; ++dx) hit = qGreen(image.pixel(x + dx, y + dy)) < 120;
            inked += hit;
        }
        QCOMPARE_GE(asked, 20);
        QCOMPARE(inked, asked);
    }

    // ----- A paper's figure: what the first test on a scanned paper met --------

    // A scan in the style of the paper's figures: a blue model line under black
    // ring markers that touch along its flanks and pile up in its tail, a red
    // curve with a circled label drawn over its peak, a legend in a box, and a
    // grey inset with red annotations drawn over the plot. No text at all.
    void aPapersFigure()
    {
        const QString file = path(QStringLiteral("paper_like.pdf"));
        Axes a;
        a.xMin = 0.6;
        a.xMax = 2.0;
        a.yMin = 0;
        a.yMax = 0.4;
        a.xTicks = {0.6, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0};
        a.yTicks = {0, 0.1, 0.2, 0.3, 0.4};
        a.labels = false;
        const auto model = [](double x) { return 0.36 / (1 + std::pow((x - 0.8) / 0.02, 2)) + 0.02 / (1 + std::pow((x - 0.62) / 0.05, 2)) + 0.003; };
        const auto red = [](double x) { return 0.3 / (1 + std::pow((x - 1.25) / 0.025, 2)) + 0.002; };
        const double k = 300.0 / 72;
        QImage image(int(kPageW * k), int(kPageH * k), QImage::Format_RGB32);
        image.fill(Qt::white);
        QList<QPointF> truthMarkers;
        const QRectF legend(a.frame.left() + 150, a.frame.top() + 10, 110, 36);
        const QRectF inset(a.frame.right() - 120, a.frame.top() + 20, 110, 70);
        {
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(k, k);
            drawAxes(p, a, 8);
            p.setClipRect(a.frame);
            QPolygonF line, redLine;
            for (int i = 0; i <= 1400; ++i) {
                const double x = 0.6 + i * 0.001;
                line << a.at(x, model(x));
                redLine << a.at(x, red(x));
            }
            p.setPen(QPen(QColor(55, 82, 166), 1.6));
            p.drawPolyline(line);
            p.setPen(QPen(QColor(230, 32, 28), 1.4));
            p.drawPolyline(redLine);
            // Rings: 3.4 pt across, thick, one each 0.01.
            p.setPen(QPen(Qt::black, 1.0));
            p.setBrush(Qt::NoBrush);
            for (int i = 0; i <= 140; ++i) {
                const double x = 0.6 + i * 0.01;
                const QPointF at = a.at(x, model(x));
                p.drawEllipse(at, 1.2, 1.2);
                if (a.frame.contains(at)) truthMarkers << at;
            }
            p.setClipping(false);
            // A circled 2 over the red peak, 5 pt above it.
            const QPointF top = a.at(1.25, red(1.25));
            p.setPen(QPen(QColor(230, 32, 28), 0.8));
            p.drawEllipse(top + QPointF(0, -11), 5, 5);
            QFont font(QStringLiteral("Helvetica"));
            font.setPixelSize(7);
            p.setFont(font);
            p.drawText(QRectF(top + QPointF(-5, -16), QSizeF(10, 10)), Qt::AlignCenter, QStringLiteral("2"));
            // The legend, boxed.
            p.setPen(QPen(Qt::black, 0.6));
            p.setBrush(Qt::white);
            p.drawRect(legend);
            p.setPen(QPen(QColor(55, 82, 166), 1.6));
            p.drawLine(legend.topLeft() + QPointF(6, 11), legend.topLeft() + QPointF(26, 11));
            p.setPen(QPen(Qt::black, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(legend.topLeft() + QPointF(16, 25), 1.2, 1.2);
            p.setPen(Qt::black);
            font.setPixelSize(9);
            p.setFont(font);
            p.drawText(QRectF(legend.topLeft() + QPointF(30, 4), QSizeF(70, 14)), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Model"));
            p.drawText(QRectF(legend.topLeft() + QPointF(30, 18), QSizeF(70, 14)), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("COMSOL"));
            // The inset: grey, with white slots, a red dashed line and a red
            // circled 1.
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(127, 127, 127));
            p.drawRect(inset);
            p.setBrush(Qt::white);
            p.drawRect(QRectF(inset.left() + 20, inset.top(), 12, 40));
            p.drawRect(QRectF(inset.left() + 70, inset.top(), 12, 30));
            p.drawRect(QRectF(inset.left(), inset.bottom() - 14, 100, 8));
            p.setPen(QPen(QColor(230, 32, 28), 0.8, Qt::DashLine));
            p.drawLine(inset.topLeft() + QPointF(20, 4), inset.topLeft() + QPointF(82, 4));
            p.drawEllipse(inset.topLeft() + QPointF(26, 20), 5, 5);
            p.end();
        }
        QVERIFY(writeScan(file, image));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY2(!plot.frame.isNull(), "a frame");
        QCOMPARE_LE(QLineF(plot.pageBox().topLeft(), a.frame.topLeft()).length(), 1.5);
        // Calibrated by the values at its frame's edges, as Claude reads them off.
        plot.calibration = Calibration{};
        for (const auto& r : {std::tuple{a.frame.bottomLeft(), true, 0.6}, {a.frame.bottomRight(), true, 2.0}, {a.frame.bottomLeft(), false, 0.0}, {a.frame.topLeft(), false, 0.4}}) {
            Reference ref;
            ref.at = std::get<0>(r);
            (std::get<1>(r) ? ref.hasX : ref.hasY) = true;
            (std::get<1>(r) ? ref.x : ref.y) = std::get<2>(r);
            plot.calibration.references << ref;
        }
        QString why;
        QVERIFY2(plot.calibration.fit(&why), qPrintable(why));
        QVERIFY2(extract(source, plot, Options{}, &why), qPrintable(why));
        // The inset and the legend were found and left out, and nothing came
        // from inside them.
        bool legendOut = false, insetOut = false;
        for (const QRectF& r : std::as_const(plot.leftOut)) {
            legendOut = legendOut || r.contains(legend.center());
            insetOut = insetOut || r.contains(inset.center());
        }
        QVERIFY2(legendOut, "the legend's box left out");
        QVERIFY2(insetOut, "the inset left out");
        for (const Series& s : std::as_const(plot.series))
            for (const QPointF& q : s.points) QVERIFY2(!legend.contains(q) && !inset.contains(q), qPrintable(s.name));
        // The markers: found, and where they are.
        const Series* markers = nullptr;
        for (const Series& s : std::as_const(plot.series))
            if (s.shape == Series::Shape::Markers && s.colour.lightness() < 80) markers = &s;
        QVERIFY2(markers != nullptr, "the black markers");
        int right = 0;
        for (const QPointF& q : markers->points) {
            double nearest = 1e9;
            for (const QPointF& t : std::as_const(truthMarkers)) nearest = std::min(nearest, QLineF(q, t).length());
            right += nearest <= 1.0;
        }
        int found = 0;
        for (const QPointF& t : std::as_const(truthMarkers)) {
            double nearest = 1e9;
            for (const QPointF& q : markers->points) nearest = std::min(nearest, QLineF(q, t).length());
            found += nearest <= 1.0;
        }
        qInfo("paper-like scan: %d of %lld markers found, %d of %lld found ones on a marker", found, qlonglong(truthMarkers.size()), right, qlonglong(markers->points.size()));
        QCOMPARE_GE(right, int(0.95 * markers->points.size()));
        QCOMPARE_GE(found, int(0.5 * truthMarkers.size()));
        // The blue line, under the markers: what is followed is on it, and it
        // reaches the peak.
        double blueTop = 0, blueWorst = 0;
        int bluePoints = 0;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape != Series::Shape::Line || !(s.colour.blue() > s.colour.red() + 60)) continue;
            for (const QPointF& v : values(plot, s)) {
                blueTop = std::max(blueTop, v.y());
                blueWorst = std::max(blueWorst, std::abs(v.y() - model(v.x())));
                ++bluePoints;
            }
        }
        qInfo("paper-like scan: blue line %d points, top %.3f (0.363), worst %.4f", bluePoints, blueTop, blueWorst);
        QCOMPARE_GE(bluePoints, 30);
        QCOMPARE_GE(blueTop, 0.30);
        QCOMPARE_LE(blueWorst, 0.02);
        // The red curve stops at its peak: it does not climb into the circled
        // label above it.
        double redTop = 0;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape != Series::Shape::Line || !(s.colour.red() > s.colour.blue() + 60)) continue;
            for (const QPointF& v : values(plot, s)) redTop = std::max(redTop, v.y());
        }
        qInfo("paper-like scan: red top %.3f (0.302)", redTop);
        QCOMPARE_LE(std::abs(redTop - 0.302), 0.012);
        // And not a heap of junk.
        QCOMPARE_LE(plot.series.size(), 10);
        // Every line in order of x, and the blue one, in pieces under the
        // rings, one curve.
        int blueLines = 0;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape != Series::Shape::Line) continue;
            blueLines += s.colour.blue() > s.colour.red() + 60;
            const QList<QPointF> v = values(plot, s);
            for (int i = 1; i < v.size(); ++i) QVERIFY2(v.at(i).x() >= v.at(i - 1).x() - 1e-9, qPrintable(QStringLiteral("%1: %2 then %3").arg(s.name).arg(v.at(i - 1).x()).arg(v.at(i).x())));
        }
        QCOMPARE(blueLines, 1);
        // Asked for black alone, no grey or blue comes back.
        Plot black = plot;
        Options o;
        o.colours << QColor(Qt::black);
        QVERIFY(extract(source, black, o, &why));
        for (const Series& s : std::as_const(black.series)) QVERIFY2(s.colour.lightness() < 90 && std::abs(s.colour.red() - s.colour.blue()) < 60, qPrintable(s.colour.name()));
        // Asked for blue alone: the black rings over it are still rings - no
        // markers come back as blue, and the line is followed under them to
        // its peak.
        Plot blue = plot;
        o.colours = {QColor(55, 82, 166)};
        QVERIFY(extract(source, blue, o, &why));
        double top = 0;
        for (const Series& s : std::as_const(blue.series)) {
            QVERIFY2(s.shape != Series::Shape::Markers, qPrintable(s.colour.name()));
            QCOMPARE(s.asked, 0);
            for (const QPointF& v : values(blue, s)) top = std::max(top, v.y());
        }
        QCOMPARE_GE(top, 0.30);
        // Asked for both, each series says which it was traced as: the rings'
        // black, though their average is grey, and the line's blue.
        Plot both = plot;
        o.colours = {QColor(55, 82, 166), QColor(Qt::black)};
        QVERIFY(extract(source, both, o, &why));
        bool ringsBlack = false;
        for (const Series& s : std::as_const(both.series)) {
            if (s.shape == Series::Shape::Markers) {
                QCOMPARE(s.asked, 1);
                ringsBlack = s.colour.lightness() < 60;
            } else if (s.colour.blue() > s.colour.red() + 60) {
                QCOMPARE(s.asked, 0);
            }
        }
        QVERIFY2(ringsBlack, "the rings' colour is their stroke's");
    }

    void blackIsNotGrey()
    {
        // The same line in black and in grey: two families by darkness, and
        // only one when one colour is asked for.
        const QString file = path(QStringLiteral("black_grey.pdf"));
        Axes a;
        a.labels = false;
        const auto low = [](double x) { return -0.5 + 0.3 * std::sin(x); };
        const auto high = [](double x) { return 0.4 + 0.3 * std::sin(x); };
        QVERIFY(writeScan(file, plotImage(a, {Curve{low, Qt::black, 1.2}, Curve{high, QColor(127, 127, 127), 1.2}}, 300)));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QString why;
        QVERIFY(assignTickValues(plot, true, {0, 2, 4, 6, 8, 10}, &why));
        QVERIFY(assignTickValues(plot, false, {1, 0.5, 0, -0.5, -1}, &why));
        QVERIFY(calibrateFromAxes(plot, &why));
        QVERIFY(extract(source, plot, Options{}, &why));
        const Series* dark = nearest(plot, low, 2);
        const Series* grey = nearest(plot, high, 2);
        QVERIFY(dark != nullptr && grey != nullptr && dark != grey);
        QCOMPARE_LE(errorsOf(plot, *dark, low, 2).worst, 0.01);
        QCOMPARE_LE(errorsOf(plot, *grey, high, 2).worst, 0.01);
        Options o;
        o.colours << QColor(Qt::black);
        QVERIFY(extract(source, plot, o, &why));
        QCOMPARE(plot.series.size(), 1);
        QCOMPARE_LE(errorsOf(plot, plot.series.first(), low, 2).worst, 0.01);
        o.colours = {QColor(127, 127, 127)};
        QVERIFY(extract(source, plot, o, &why));
        QCOMPARE(plot.series.size(), 1);
        QCOMPARE_LE(errorsOf(plot, plot.series.first(), high, 2).worst, 0.01);
    }

    void aRegionLeftOutByHand()
    {
        const QString file = path(QStringLiteral("left_out.pdf"));
        Axes a;
        a.labels = false;
        QVERIFY(writeScan(file, plotImage(a, {Curve{sine, Qt::blue, 1.2}}, 300)));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QString why;
        QVERIFY(assignTickValues(plot, true, {0, 2, 4, 6, 8, 10}, &why));
        QVERIFY(assignTickValues(plot, false, {1, 0.5, 0, -0.5, -1}, &why));
        QVERIFY(calibrateFromAxes(plot, &why));
        const QRectF out(a.px(4), a.frame.top(), a.px(6) - a.px(4), a.frame.height());
        plot.exclude << out;
        QVERIFY(extract(source, plot, Options{}, &why));
        for (const Series& s : std::as_const(plot.series))
            for (const QPointF& q : s.points) QVERIFY(!out.contains(q));
        // Kept with the rest.
        Plot back;
        QVERIFY(fromJson(toJson(plot), &back, &why));
        QCOMPARE(back.exclude.size(), 1);
        QCOMPARE(back.exclude.first(), out);
        // Points that fall in a region are taken out wherever they came from.
        Plot hand = plot;
        Series s;
        s.points = {QPointF(a.px(3), 200), QPointF(a.px(5), 200), QPointF(a.px(7), 200)};
        hand.series = {s};
        QStringList notes;
        dropExcluded(hand, &notes);
        QCOMPARE(hand.series.first().points.size(), 2);
        QCOMPARE(hand.series.first().breaks, QList<int>{1});
    }

    void aSeedRoundANarrowPeak()
    {
        // Seeded on one side of a peak narrower than the line is long: both
        // sides come back, in order of x.
        const QString file = path(QStringLiteral("narrow_peak.pdf"));
        Axes a;
        a.labels = false;
        a.yMin = 0;
        a.yMax = 1;
        a.yTicks = {0, 0.25, 0.5, 0.75, 1};
        const auto peak = [](double x) { return 0.9 / (1 + std::pow((x - 5) / 0.12, 2)) + 0.05; };
        Curve c{peak, Qt::blue, 1.4};
        c.samples = 2000;
        QVERIFY(writeScan(file, plotImage(a, {c}, 300)));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QString why;
        QVERIFY(assignTickValues(plot, true, {0, 2, 4, 6, 8, 10}, &why));
        QVERIFY(assignTickValues(plot, false, {1, 0.75, 0.5, 0.25, 0}, &why));
        QVERIFY(calibrateFromAxes(plot, &why));
        Options o;
        // (The second on the other side: the same curve, given once.)
        o.seeds << a.at(4.9, peak(4.9)) << a.at(5.1, peak(5.1));
        QVERIFY2(extract(source, plot, o, &why), qPrintable(why));
        QCOMPARE(plot.series.size(), 1);
        QVERIFY(plot.notes.join(QLatin1Char(' ')).contains(QStringLiteral("seed 2 lies on")));
        const QList<QPointF> v = values(plot, plot.series.first());
        double low = 1e9, high = -1e9, top = 0;
        for (const QPointF& q : v) {
            low = std::min(low, q.x());
            high = std::max(high, q.x());
            top = std::max(top, q.y());
        }
        qInfo("narrow peak, seeded: x %.2f to %.2f, top %.3f (0.95)", low, high, top);
        QCOMPARE_LE(low, 1.0);
        QCOMPARE_GE(high, 9.0);
        QCOMPARE_GE(top, 0.9);
        for (int i = 1; i < v.size(); ++i) QVERIFY2(v.at(i).x() >= v.at(i - 1).x() - 1e-9, qPrintable(QStringLiteral("in order of x: %1 then %2 at %3 of %4").arg(v.at(i - 1).x()).arg(v.at(i).x()).arg(i).arg(v.size())));
        QCOMPARE_LE(errorsOf(plot, plot.series.first(), peak, 1).worst, 0.03);
    }

    void aFrameIsNotTakenFromAPlotBesideIt()
    {
        // Two plots side by side, the right one's bottom a few points below the
        // left one's: each frame ends at its own axis.
        const QString file = path(QStringLiteral("side_by_side.pdf"));
        Axes left, right;
        left.labels = right.labels = false;
        left.frame = QRectF(70, 120, 220, 200);
        right.frame = QRectF(340, 100, 220, 223.5);
        const double k = 300.0 / 72;
        QImage image(int(kPageW * k), int(kPageH * k), QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(k, k);
            drawPlot(p, left, {Curve{sine, Qt::blue, 1.2}}, 8);
            drawPlot(p, right, {Curve{cosine, Qt::red, 1.2}}, 8);
        }
        QVERIFY(writeScan(file, image));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        const QList<Plot> plots = findPlots(source, 0);
        QCOMPARE(plots.size(), 2);
        for (const Plot& plot : plots) {
            const QRectF want = plot.pageBox().center().x() < 300 ? left.frame : right.frame;
            QCOMPARE_LE(std::abs(plot.pageBox().bottom() - want.bottom()), 1.0);
            QCOMPARE_LE(std::abs(plot.pageBox().top() - want.top()), 1.0);
        }
    }

    void ringsPackedOnASteepPeakAndAKey()
    {
        // A narrow peak with thick rings packed along its sides, the line
        // through their middles (their holes all but gone), more along the
        // tail, and a key of a filled square and an empty one, each with a
        // word, without a box: as the second recheck's Fig. 5.
        const QString file = path(QStringLiteral("packed_rings.pdf"));
        Axes a;
        a.frame = QRectF(100, 150, 300, 170);
        a.xMin = 0.6;
        a.xMax = 2.0;
        a.yMin = 0;
        a.yMax = 1;
        a.xTicks = {0.6, 1.0, 1.4, 1.8};
        a.yTicks = {0, 0.5, 1};
        a.labels = false;
        const auto model = [](double x) { return 0.8 / (1 + std::pow((x - 1.0) / 0.012, 2)) + 0.012; };
        const double k = 300.0 / 72;
        QImage image(int(kPageW * k), int(kPageH * k), QImage::Format_RGB32);
        image.fill(Qt::white);
        QList<QPointF> rings;
        const QRectF key(a.px(1.45), a.py(0.62), a.px(1.85) - a.px(1.45), 12);
        {
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(k, k);
            drawAxes(p, a, 8);
            p.setClipRect(a.frame);
            QPolygonF line;
            for (int i = 0; i <= 14000; ++i) line << a.at(0.6 + i * 0.0001, model(0.6 + i * 0.0001));
            p.setPen(QPen(QColor(55, 82, 166), 1.2));
            p.drawPolyline(line);
            // A ring each 2.6 pt along the curve: packed on its sides.
            double along = 0;
            for (int i = 1; i < line.size(); ++i) {
                along += QLineF(line.at(i - 1), line.at(i)).length();
                if (along < 2.6) continue;
                along = 0;
                rings << line.at(i);
            }
            p.setPen(QPen(Qt::black, 1.1));
            p.setBrush(Qt::NoBrush);
            for (const QPointF& r : std::as_const(rings)) p.drawEllipse(r, 1.3, 1.3);
            p.setClipping(false);
            // The key.
            p.setPen(QPen(Qt::black, 0.5));
            p.setBrush(QColor(127, 127, 127));
            p.drawRect(QRectF(key.left(), key.top() + 2, 7, 7));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(key.left() + 52, key.top() + 2, 7, 7));
            QFont font(QStringLiteral("Helvetica"));
            font.setPixelSize(10);
            p.setFont(font);
            p.setPen(Qt::black);
            p.drawText(QRectF(key.left() + 9, key.top(), 40, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Metal"));
            p.drawText(QRectF(key.left() + 61, key.top(), 40, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Air"));
            p.end();
        }
        QVERIFY(writeScan(file, image));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY2(!plot.frame.isNull(), "a frame");
        plot.calibration = Calibration{};
        for (const auto& r : {std::tuple{a.frame.bottomLeft(), true, 0.6}, {a.frame.bottomRight(), true, 2.0}, {a.frame.bottomLeft(), false, 0.0}, {a.frame.topLeft(), false, 1.0}}) {
            Reference ref;
            ref.at = std::get<0>(r);
            (std::get<1>(r) ? ref.hasX : ref.hasY) = true;
            (std::get<1>(r) ? ref.x : ref.y) = std::get<2>(r);
            plot.calibration.references << ref;
        }
        QString why;
        QVERIFY2(plot.calibration.fit(&why), qPrintable(why));
        QVERIFY2(extract(source, plot, Options{}, &why), qPrintable(why));
        const Series* markers = nullptr;
        int blueLines = 0;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape == Series::Shape::Markers) {
                if (s.colour.lightness() < 80) markers = &s;
                continue;
            }
            // No black line: what is left of the rings, a stretch of them
            // walked as a line, or the key's words, is no curve.
            QVERIFY2(!(s.colour.hslSaturation() < 70 && s.colour.lightness() < 120), qPrintable(QStringLiteral("%1 %2, %3 points").arg(s.name, s.colour.name()).arg(s.points.size())));
            if (s.colour.blue() <= s.colour.red() + 60) continue;
            ++blueLines;
            // One curve, in order of x, its steep sides in the order along
            // them: y turns back only at the peak, not between points a
            // sliver apart.
            int turns = 0;
            for (int i = 1; i + 1 < s.points.size(); ++i) {
                if (s.breaks.contains(i) || s.breaks.contains(i + 1)) continue;
                const double before = s.points.at(i).y() - s.points.at(i - 1).y(), after = s.points.at(i + 1).y() - s.points.at(i).y();
                turns += before * after < 0 && std::abs(before) > 1.5 && std::abs(after) > 1.5;
            }
            const QList<QPointF> v = values(plot, s);
            double top = 0;
            for (int i = 0; i < v.size(); ++i) {
                top = std::max(top, v.at(i).y());
                if (i > 0) QVERIFY(v.at(i).x() >= v.at(i - 1).x() - 1e-9);
            }
            qInfo("packed rings: blue line %lld points, %lld breaks, %d turns, top %.3f (0.812)", qlonglong(s.points.size()), qlonglong(s.breaks.size()), turns, top);
            QCOMPARE_LE(turns, 1);
            QCOMPARE_GE(top, 0.7);
            // Solid, only covered by rings in places: no dashes (T2).
            QVERIFY2(!s.dashed, "a solid line under rings is not dashed");
        }
        QCOMPARE(blueLines, 1);
        QVERIFY2(markers != nullptr, "the rings");
        int found = 0, right = 0;
        for (const QPointF& t : std::as_const(rings)) {
            double nearest = 1e9;
            for (const QPointF& q : markers->points) nearest = std::min(nearest, QLineF(q, t).length());
            found += nearest <= 1.0;
        }
        for (const QPointF& q : markers->points) {
            double nearest = 1e9;
            for (const QPointF& t : std::as_const(rings)) nearest = std::min(nearest, QLineF(q, t).length());
            right += nearest <= 1.0;
            QVERIFY2(!key.adjusted(-1, -1, 1, 1).contains(q), "no marker in the key");
        }
        qInfo("packed rings: %d of %lld found, %d of %lld on a ring", found, qlonglong(rings.size()), right, qlonglong(markers->points.size()));
        QCOMPARE_GE(found, int(0.7 * rings.size()));
        QCOMPARE_GE(right, int(0.95 * markers->points.size()));
    }

    void aNarrowPeakKeepsBothSides()
    {
        // A peak a couple of line widths wide at half height, scanned: its two
        // sides, near each other at the top, are both the line, not one a
        // double of the other (the third recheck's T1). Most of the x
        // between its half-height points is traced closely.
        for (const double halfWidthPt : {1.2, 2.0, 3.5}) {
            const QString file = path(QStringLiteral("narrow_peak_%1.pdf").arg(halfWidthPt));
            Axes a;
            a.frame = QRectF(100, 150, 300, 170);
            a.xMin = 0.6;
            a.xMax = 2.0;
            a.yMin = 0;
            a.yMax = 1;
            a.xTicks = {0.6, 1.0, 1.4, 1.8};
            a.yTicks = {0, 0.5, 1};
            a.labels = false;
            const double gamma = halfWidthPt * (a.xMax - a.xMin) / a.frame.width();
            const auto model = [gamma](double x) { return 0.8 / (1 + std::pow((x - 1.0) / gamma, 2)) + 0.05; };
            const double k = 300.0 / 72, width = 1.0;
            QImage image(int(kPageW * k), int(kPageH * k), QImage::Format_RGB32);
            image.fill(Qt::white);
            {
                QPainter p(&image);
                p.setRenderHint(QPainter::Antialiasing);
                p.scale(k, k);
                drawAxes(p, a, 8);
                p.setClipRect(a.frame);
                QPolygonF line;
                for (int i = 0; i <= 140000; ++i) line << a.at(0.6 + i * 0.00001, model(0.6 + i * 0.00001));
                p.setPen(QPen(QColor(40, 70, 200), width));
                p.drawPolyline(line);
                p.end();
            }
            QVERIFY(writeScan(file, image));
            QPdfDocument pdf;
            QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
            Source source(&pdf, file);
            Plot plot = firstPlot(source);
            QVERIFY2(!plot.frame.isNull(), "a frame");
            plot.calibration = Calibration{};
            for (const auto& r : {std::tuple{a.frame.bottomLeft(), true, 0.6}, {a.frame.bottomRight(), true, 2.0}, {a.frame.bottomLeft(), false, 0.0}, {a.frame.topLeft(), false, 1.0}}) {
                Reference ref;
                ref.at = std::get<0>(r);
                (std::get<1>(r) ? ref.hasX : ref.hasY) = true;
                (std::get<1>(r) ? ref.x : ref.y) = std::get<2>(r);
                plot.calibration.references << ref;
            }
            QString why;
            QVERIFY2(plot.calibration.fit(&why), qPrintable(why));
            QVERIFY2(extract(source, plot, Options{}, &why), qPrintable(why));
            const Series* line = nullptr;
            for (const Series& s : std::as_const(plot.series))
                if (s.shape == Series::Shape::Line && s.colour.blue() > s.colour.red() + 60 && (line == nullptr || s.points.size() > line->points.size())) line = &s;
            QVERIFY2(line != nullptr, "the line");
            // The x between the half-height points (page points) covered by
            // steps under a line width in x, within a piece.
            const double left = a.px(1.0 - gamma), right = a.px(1.0 + gamma);
            double covered = 0;
            for (int i = 1; i < line->points.size(); ++i) {
                if (line->breaks.contains(i)) continue;
                const QPointF p0 = line->points.at(i - 1), p1 = line->points.at(i);
                if (std::abs(p1.x() - p0.x()) > width) continue;
                const double lo = std::max(std::min(p0.x(), p1.x()), left), hi = std::min(std::max(p0.x(), p1.x()), right);
                covered += std::max(0.0, hi - lo);
            }
            const QList<QPointF> v = values(plot, *line);
            double top = 0;
            for (const QPointF& q : v) top = std::max(top, q.y());
            qInfo("narrow peak %.1f pt: %lld points, %lld breaks, %.0f%% of the half-height width traced, top %.3f (0.850)", halfWidthPt, qlonglong(line->points.size()),
                  qlonglong(line->breaks.size()), 100 * covered / (right - left), top);
            QCOMPARE_GE(covered, 0.9 * (right - left));
            QCOMPARE_GE(top, 0.75);
        }
    }

    void numbersAboveThePlotAreNoLabels()
    {
        // A running header over the plot, with numbers in it: the x axis is
        // read from the bottom, where its marks are.
        const QString file = path(QStringLiteral("header.pdf"));
        Axes a;
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            drawPlot(p, a, {Curve{sine, Qt::blue}}, u);
            p.save();
            p.scale(1 / u, 1 / u);
            p.setPen(Qt::black);
            p.setFont(plotFont(8, u));
            p.drawText(QRectF(QPointF(a.frame.left(), a.frame.top() - 20) * u, QSizeF(400, 12) * u), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("JOURNAL OF THINGS, VOL. 25, NO. 8, APRIL 15"));
            p.restore();
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QCOMPARE(plot.xAxis.side, 0);
        QVERIFY2(!plot.xAxis.title.contains(QStringLiteral("JOURNAL")), qPrintable(plot.xAxis.title));
    }

    void lonePointsBetweenGapsAreFolded()
    {
        // A piece of one point has no line to draw (the third recheck's T3):
        // joined to the nearer piece beside it, or, with none near, left out.
        Series s;
        s.shape = Series::Shape::Line;
        s.points = {{0, 0}, {1, 0}, {2, 0}, {12, 0}, {30, 0}, {31, 0}, {70, 0}, {100, 0}, {101, 0}};
        s.breaks = {3, 4, 6, 7};
        QCOMPARE(foldLonePoints(s, 16), 2);
        // (12 joins the first piece, 10 from it; 70, 30 from each, goes.)
        QCOMPARE(s.points.size(), 8);
        QCOMPARE(s.points.at(3), QPointF(12, 0));
        QCOMPARE(s.points.at(6), QPointF(100, 0));
        QList<int> breaks = s.breaks;
        std::sort(breaks.begin(), breaks.end());
        QCOMPARE(breaks, (QList<int>{4, 6}));
        // A line without gaps, or only pieces of two or more, is left alone.
        Series t;
        t.shape = Series::Shape::Line;
        t.points = {{0, 0}, {1, 0}, {20, 0}, {21, 0}};
        t.breaks = {2};
        QCOMPARE(foldLonePoints(t, 16), 0);
        QCOMPARE(t.points.size(), 4);
    }

    void aProfileDrawnInPiecesIsOneCurve()
    {
        // As Chen et al. (1996) draw SIMS profiles (the open items' B1, B2,
        // B6): a log axis, each noisy line written as a piece per run of
        // counts, broken at each zero, which a log axis cannot show; a
        // histogram held along the frame's bottom where it falls under the
        // axis; the x axis's first and last labels at the frame's corners
        // with no tick of their own, and a minor tick missing between the two
        // labels that have one.
        const QString file = path(QStringLiteral("pieces.pdf"));
        Axes a;
        a.xMin = 0;
        a.xMax = 0.3;
        a.yLog = true;
        a.yMin = 1e15;
        a.yMax = 1e21;
        unsigned seed = 12345;
        const auto random = [&] {
            seed = seed * 1103515245u + 12345u;
            return double((seed >> 8) & 0xffff) / 65536.0;
        };
        // Two profiles, each a peak falling to a floor of 1 or 2 counts
        // (1e16, 2e16) or none.
        QList<QList<QPolygonF>> profiles;
        int drawnPieces = 0;
        for (const double peak : {3e20, 2e18}) {
            QList<QPolygonF> pieces;
            QPolygonF piece;
            for (double x = 0.0015; x < 0.3; x += 0.0015) {
                double v = peak * std::exp(-std::pow((x - 0.03) / 0.018, 2) / 2);
                if (v < 3e16) {
                    const double r = random();
                    v = r < 0.35 ? 0 : (r < 0.7 ? 1e16 : 2e16);
                }
                if (v <= 0) {
                    if (piece.size() >= 2) pieces << piece;
                    piece.clear();
                    continue;
                }
                piece << a.at(x, v);
            }
            if (piece.size() >= 2) pieces << piece;
            profiles << pieces;
            drawnPieces += int(pieces.size());
        }
        // The histogram: two bumps, between them under the axis.
        QPolygonF histogram;
        for (double x = 0; x < 0.3 - 1e-9; x += 0.006) {
            const double mid = x + 0.003;
            const double v = 1e20 * std::exp(-std::pow((mid - 0.035) / 0.02, 2) / 2) + 1e18 * std::exp(-std::pow((mid - 0.2) / 0.012, 2) / 2);
            const double y = a.py(std::max(v, a.yMin));
            histogram << QPointF(a.px(x), y) << QPointF(a.px(x + 0.006), y);
        }
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::black, 0.8));
            p.drawRect(a.frame);
            // Ticks inward: major at 0.1 and 0.2, minor every 0.02 but 0.16;
            // none at the frame's ends.
            for (int k = 1; k < 15; ++k) {
                if (k == 8) continue;
                const double x = a.px(k * 0.02);
                if (k * 2 == 10 || k * 2 == 20) continue;
                p.drawLine(QPointF(x, a.frame.bottom()), QPointF(x, a.frame.bottom() - 3));
            }
            for (const double t : {0.1, 0.2}) p.drawLine(QPointF(a.px(t), a.frame.bottom()), QPointF(a.px(t), a.frame.bottom() - 6));
            for (const double t : {1e16, 1e17, 1e18, 1e19, 1e20}) p.drawLine(QPointF(a.frame.left(), a.py(t)), QPointF(a.frame.left() + 6, a.py(t)));
            p.save();
            p.scale(1 / u, 1 / u);
            p.setFont(plotFont(9, u));
            const auto text = [&](QRectF box, int align, const QString& s) { p.drawText(QRectF(box.topLeft() * u, box.size() * u), align, s); };
            for (const double t : {0.0, 0.1, 0.2, 0.3})
                text(QRectF(a.px(t) - 30, a.frame.bottom() + 6, 60, 12), Qt::AlignHCenter | Qt::AlignTop, QString::number(t, 'f', 1));
            for (int e = 15; e <= 21; ++e) text(QRectF(a.frame.left() - 66, a.py(std::pow(10.0, e)) - 7, 60, 14), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("1e%1").arg(e));
            p.restore();
            QPen red(Qt::red, 0.6);
            p.setPen(red);
            for (const QList<QPolygonF>& pieces : std::as_const(profiles))
                for (const QPolygonF& piece : pieces) p.drawPolyline(piece);
            p.setPen(QPen(Qt::blue, 0.6));
            p.drawPolyline(histogram);
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        // B1: four labels, linear, from 0.
        QCOMPARE(plot.xAxis.scale, Scale::Linear);
        QVERIFY2(!plot.xAxis.scaleGuessed, "with 0.0 and 0.3 read, linear is no guess");
        int used = 0;
        for (const Tick& t : std::as_const(plot.xAxis.ticks)) used += t.used;
        QCOMPARE(used, 4);
        QCOMPARE(plot.yAxis.scale, Scale::Log);
        int usedY = 0;
        for (const Tick& t : std::as_const(plot.yAxis.ticks)) usedY += t.used;
        QCOMPARE(usedY, 7);
        QVERIFY(extract(source, plot, Options{}));
        QList<const Series*> reds, blues;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape != Series::Shape::Line) continue;
            if (s.colour == QColor(Qt::red)) reds << &s;
            else if (s.colour == QColor(Qt::blue)) blues << &s;
        }
        // B2: a profile is one curve, a gap at each zero.
        QCOMPARE(reds.size(), 2);
        int breaks = 0, points = 0;
        for (const Series* s : std::as_const(reds)) {
            breaks += int(s->breaks.size());
            points += int(s->points.size());
        }
        QCOMPARE(breaks, drawnPieces - 2);
        int drawnPoints = 0;
        for (const QList<QPolygonF>& pieces : std::as_const(profiles))
            for (const QPolygonF& piece : pieces) drawnPoints += int(piece.size());
        QCOMPARE(points, drawnPoints);
        QVERIFY(!reds.first()->dashed);
        // B6: the histogram's run along the bottom is not data: left out,
        // a gap between the two bumps.
        QCOMPARE(blues.size(), 1);
        for (const QPointF& v : values(plot, *blues.first())) QVERIFY2(v.y() > 1.05e15, qPrintable(QString::number(v.y())));
        QCOMPARE_GE(blues.first()->breaks.size(), 1);
        QCOMPARE_LE(plot.calibration.toValue(blues.first()->points.first()).x(), 0.001);
    }

    void arrowsNameTheCurvesTheyPointAt()
    {
        // As Chen et al. (1996) label theirs (the open items' B4, B5): two
        // pairs of curves, measured (red) and model (blue, a few points off),
        // a legend in a rounded black box; a black arrow from "dose A" to the
        // first pair, one from "dose B" to an ellipse round the second.
        const QString file = path(QStringLiteral("arrows.pdf"));
        Axes a;
        a.xMin = 0;
        a.xMax = 10;
        a.yMin = 0;
        a.yMax = 10;
        a.xTicks = {0, 2, 4, 6, 8, 10};
        a.yTicks = {0, 2, 4, 6, 8, 10};
        const auto line = [&](double y0, double slope, double up) {
            QPolygonF q;
            for (double x = 0.2; x <= 9.8 + 1e-9; x += 0.2) q << a.at(x, y0 + slope * x) + QPointF(0, -up);
            return q;
        };
        const QPolygonF redA = line(8, -0.4, 0), blueA = line(8, -0.4, 4.5), redB = line(1.5, 0.3, 0), blueB = line(1.5, 0.3, 4.5);
        const QPointF tipA = a.at(3, 6.8), tailA = a.at(5.2, 8.6);
        const QRectF ring(a.at(6.6, 3.9) - QPointF(0, 6), QSizeF(a.px(8) - a.px(6.6), 22));
        const QPointF tipB(ring.left() + 1, ring.center().y()), tailB = a.at(4.4, 5.6);
        const auto arrow = [](QPainter& p, QPointF tail, QPointF tip) {
            p.drawLine(tail, tip);
            const QLineF back(tip, tail);
            QLineF w1 = QLineF::fromPolar(7, back.angle() + 25).translated(tip), w2 = QLineF::fromPolar(7, back.angle() - 25).translated(tip);
            p.drawPolyline(QPolygonF{w1.p2(), tip, w2.p2()});
        };
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            drawAxes(p, a, u);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::red, 0.8));
            p.drawPolyline(redA);
            p.drawPolyline(redB);
            p.setPen(QPen(Qt::blue, 0.8));
            p.drawPolyline(blueA);
            p.drawPolyline(blueB);
            // A short piece of red that no join or arrow gives to a curve.
            p.setPen(QPen(Qt::red, 0.8));
            p.drawPolyline(QPolygonF{a.at(1, 9), a.at(1.2, 9.1), a.at(1.4, 9)});
            // The legend, in its rounded box.
            const QPointF at = a.at(7, 9.6);
            p.setPen(QPen(Qt::red, 0.8));
            p.drawLine(at, at + QPointF(18, 0));
            p.setPen(QPen(Qt::blue, 0.8));
            p.drawLine(at + QPointF(0, 12), at + QPointF(18, 12));
            p.setPen(QPen(Qt::black, 0.5));
            p.drawRoundedRect(QRectF(at - QPointF(6, 9), QSizeF(78, 30)), 5, 5);
            p.setPen(QPen(Qt::black, 0.6));
            arrow(p, tailA, tipA);
            arrow(p, tailB, tipB);
            p.drawEllipse(ring);
            p.save();
            p.scale(1 / u, 1 / u);
            p.setFont(plotFont(8, u));
            const auto text = [&](QPointF left, const QString& s) { p.drawText(QRectF(QPointF(left.x(), left.y() - 6) * u, QSizeF(80, 12) * u), Qt::AlignLeft | Qt::AlignVCenter, s); };
            text(at + QPointF(22, 0), QStringLiteral("measured"));
            text(at + QPointF(22, 12), QStringLiteral("model"));
            text(tailA + QPointF(2, 0), QStringLiteral("dose A"));
            text(tailB + QPointF(-34, 0), QStringLiteral("dose B"));
            p.restore();
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QVERIFY(extract(source, plot, Options{}));
        QStringList names;
        for (const Series& s : std::as_const(plot.series)) {
            QVERIFY2(s.colour == QColor(Qt::red) || s.colour == QColor(Qt::blue), qPrintable(s.name + QLatin1Char(' ') + s.colour.name()));
            names << s.name;
        }
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("measured, dose A"), QStringLiteral("measured, dose B"), QStringLiteral("measured, unassigned piece"),
                                     QStringLiteral("model, dose A"), QStringLiteral("model, dose B")}));
        // Curves the arrows name are known for what they are: many of them
        // do not lower trust as many series would.
        {
            Plot many = plot;
            const Series one = many.series.first();
            Annotation all;
            for (int k = 0; k < 12; ++k) {
                Series s = one;
                s.name = QStringLiteral("dose %1").arg(k);
                many.series << s;
                all.names << s.name;
            }
            many.annotations << all;
            for (const QString& r : assess(many).reasons) QVERIFY2(!r.contains(QStringLiteral("more than most plots have")), qPrintable(r));
            many.annotations.clear();
            bool said = false;
            for (const QString& r : assess(many).reasons) said = said || r.contains(QStringLiteral("more than most plots have"));
            QVERIFY(said);
        }
        int arrows = 0, ellipses = 0;
        for (const Annotation& n : std::as_const(plot.annotations)) {
            if (n.kind == Annotation::Kind::Arrow) {
                ++arrows;
                QVERIFY2(n.text == QStringLiteral("dose A") || n.text == QStringLiteral("dose B"), qPrintable(n.text));
                QCOMPARE(n.names.size(), 2);
                const QPointF tip = n.text == QStringLiteral("dose A") ? tipA : tipB;
                QVERIFY2(QLineF(n.points.last(), tip).length() < 1.5, qPrintable(QStringLiteral("%1, %2").arg(n.points.last().x()).arg(n.points.last().y())));
            } else {
                ++ellipses;
                QCOMPARE(n.text, QStringLiteral("dose B"));
                QVERIFY(QLineF(n.points.boundingRect().center(), ring.center()).length() < 1.5);
            }
        }
        QCOMPARE(arrows, 2);
        QCOMPARE(ellipses, 1);   // (the legend's rounded box is not one)
        // Kept and read back.
        Plot back;
        QVERIFY(fromJson(toJson(plot), &back));
        QCOMPARE(back.annotations.size(), plot.annotations.size());
        QCOMPARE(back.annotations.first().names, plot.annotations.first().names);

        // In black alone, an ellipse may be the data (a loop): kept as a curve.
        const QString loops = path(QStringLiteral("loop.pdf"));
        QVERIFY(writeVector(loops, [&](QPainter& p, double u) {
            drawAxes(p, a, u);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::black, 0.8));
            p.drawEllipse(QRectF(a.at(2, 8), a.at(8, 2)));
        }));
        QPdfDocument loopPdf;
        QCOMPARE(loopPdf.load(loops), QPdfDocument::Error::None);
        Source loopSource(&loopPdf, loops);
        Plot loop = firstPlot(loopSource);
        QVERIFY(extract(loopSource, loop, Options{}));
        QVERIFY(loop.annotations.isEmpty());
        QCOMPARE(loop.series.size(), 1);
    }

    void aLegendsSampleAndABarAreTakenRight()
    {
        // As Chen et al. (1996) draw them (the Chen recheck's D1 to D3): a
        // model curve dashed dash by dash, in two pieces where it went under
        // the axis; between them, of its style, the legend's sample, dashed
        // the same way, just before its words, far above where the curve
        // stopped; after them a stroke of the style with no words, as far
        // above; and a histogram's bar, drawn before the curve it ends.
        const QString file = path(QStringLiteral("legend_sample.pdf"));
        Axes a;
        a.xMin = 0;
        a.xMax = 0.3;
        a.yLog = true;
        a.yMin = 1e15;
        a.yMax = 1e21;
        const auto logLine = [&](double x0, double v0, double x1, double v1) {
            QPolygonF line;
            for (int k = 0; k <= 200; ++k) {
                const double t = k / 200.0;
                line << a.at(x0 + t * (x1 - x0), std::pow(10.0, std::log10(v0) + t * (std::log10(v1) - std::log10(v0))));
            }
            return line;
        };
        // Dashes of 5 pt, 2.5 pt apart, each a segment of its own.
        const auto dashesOf = [](const QPolygonF& line) {
            QList<QPolygonF> dashes;
            QPolygonF dash;
            double along = 0;
            for (int i = 0; i + 1 < line.size(); ++i) {
                const QLineF step(line.at(i), line.at(i + 1));
                for (double s = 0; s < step.length(); s += 0.25) {
                    const bool on = std::fmod(along + s, 7.5) < 5;
                    if (on) dash << step.pointAt(s / step.length());
                    else if (!dash.isEmpty()) {
                        if (dash.size() >= 2) dashes << QPolygonF{dash.first(), dash.last()};
                        dash.clear();
                    }
                }
                along += step.length();
            }
            if (dash.size() >= 2) dashes << QPolygonF{dash.first(), dash.last()};
            return dashes;
        };
        const QPolygonF first = logLine(0.005, 1e20, 0.15, 2e15);
        const QPolygonF sample = logLine(0.153, 1e20, 0.168, 1e20);
        const QPolygonF second = logLine(0.175, 2e15, 0.28, 5e15);
        const QPolygonF stroke = logLine(0.285, 1e20, 0.295, 1e20);
        const QPolygonF bar{a.at(0.2, 3e16), a.at(0.2, 1e18), a.at(0.204, 1e18), a.at(0.204, 3e16)};
        const QPolygonF ended = logLine(0.05, 1e19, 0.2, 2e16);
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::black, 0.8));
            p.drawRect(a.frame);
            for (int k = 1; k < 15; ++k) p.drawLine(QPointF(a.px(k * 0.02), a.frame.bottom()), QPointF(a.px(k * 0.02), a.frame.bottom() - (k % 5 ? 3 : 6)));
            for (const double t : {1e16, 1e17, 1e18, 1e19, 1e20}) p.drawLine(QPointF(a.frame.left(), a.py(t)), QPointF(a.frame.left() + 6, a.py(t)));
            p.save();
            p.scale(1 / u, 1 / u);
            p.setFont(plotFont(9, u));
            const auto text = [&](QRectF box, int align, const QString& s) { p.drawText(QRectF(box.topLeft() * u, box.size() * u), align, s); };
            for (const double t : {0.0, 0.1, 0.2, 0.3})
                text(QRectF(a.px(t) - 30, a.frame.bottom() + 6, 60, 12), Qt::AlignHCenter | Qt::AlignTop, QString::number(t, 'f', 1));
            for (int e = 15; e <= 21; ++e) text(QRectF(a.frame.left() - 66, a.py(std::pow(10.0, e)) - 7, 60, 14), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("1e%1").arg(e));
            text(QRectF(a.px(0.171), a.py(1e20) - 7, 60, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Model"));
            p.restore();
            p.setPen(QPen(Qt::blue, 0.6));
            for (const QPolygonF& line : {first, sample, second, stroke})
                for (const QPolygonF& dash : dashesOf(line)) p.drawPolyline(dash);
            p.setPen(QPen(Qt::darkGreen, 0.6));
            p.drawPolyline(bar);
            p.drawPolyline(ended);
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QCOMPARE(plot.yAxis.scale, Scale::Log);
        QVERIFY(extract(source, plot, Options{}));
        QList<const Series*> blues, greens;
        for (const Series& s : std::as_const(plot.series)) {
            if (s.shape != Series::Shape::Line) continue;
            if (s.colour == QColor(Qt::blue)) blues << &s;
            else if (s.colour == QColor(Qt::darkGreen)) greens << &s;
        }
        // D2: the sample names the curve and is no series; D1: the curve is
        // its two pieces, and the stroke far above its end is not joined on.
        QCOMPARE(blues.size(), 2);
        const Series* curve = blues.first()->points.size() > blues.last()->points.size() ? blues.first() : blues.last();
        const Series* other = curve == blues.first() ? blues.last() : blues.first();
        QCOMPARE(curve->name, QStringLiteral("Model"));
        QVERIFY(curve->dashed);
        QCOMPARE(curve->breaks.size(), 1);
        for (const QPointF& v : values(plot, *curve)) QVERIFY2(v.x() < 0.152 || v.x() > 0.17, qPrintable(QString::number(v.x())));
        const QPointF last = values(plot, *curve).last();
        QVERIFY2(std::abs(last.x() - 0.28) < 0.003 && last.y() < 1e16, qPrintable(QStringLiteral("%1, %2").arg(last.x()).arg(last.y())));
        for (const QPointF& v : values(plot, *other)) {
            QVERIFY2(v.x() > 0.284, qPrintable(QString::number(v.x())));
            QVERIFY2(std::abs(std::log10(v.y()) - 20) < 0.05, qPrintable(QString::number(v.y())));
        }
        // D3: the bar is the end of the curve it was drawn before.
        QCOMPARE(greens.size(), 1);
        QCOMPARE(greens.first()->breaks.size(), 1);
        const QList<QPointF> green = values(plot, *greens.first());
        QVERIFY2(std::abs(green.first().x() - 0.05) < 0.002, qPrintable(QString::number(green.first().x())));
        QVERIFY2(std::abs(green.last().x() - 0.204) < 0.002 && std::abs(std::log10(green.last().y()) - std::log10(3e16)) < 0.05,
                 qPrintable(QStringLiteral("%1, %2").arg(green.last().x()).arg(green.last().y())));
    }

    void seriesLeftOutAndWhatWasAskedAreKept()
    {
        // B3: series beyond those asked for are counted and said; B8: what
        // the call asked is kept with the plot, a path's own ink is not
        // said, and a line starting a hair before the axis starts on it.
        const QString file = path(QStringLiteral("leftout.pdf"));
        Axes a;
        const QList<QColor> colours{Qt::blue, Qt::red, Qt::darkGreen};
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            drawAxes(p, a, u);
            for (int k = 0; k < colours.size(); ++k) {
                QPolygonF line;
                // (From 0.9 points left of the frame: no clip.)
                line << QPointF(a.frame.left() - 0.9, a.py(0.3 * k - 0.3));
                for (int i = 1; i <= 100; ++i) {
                    const double x = 10.0 * i / 100;
                    line << a.at(x, 0.3 * k - 0.3 + 0.2 * std::sin(x + k));
                }
                p.setPen(QPen(colours.at(k), 1.0));
                p.drawPolyline(line);
            }
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        Options options;
        options.maxSeries = 2;
        QVERIFY(extract(source, plot, options));
        QCOMPARE(plot.series.size(), 2);
        QCOMPARE(plot.seriesLeftOut, 1);
        const Trust t = assess(plot);
        QVERIFY(t.level != QStringLiteral("high"));
        QVERIFY2(t.reasons.join(QLatin1Char(' ')).contains(QStringLiteral("left out")), qPrintable(t.reasons.join(QLatin1Char('|'))));
        for (const Series& s : std::as_const(plot.series)) {
            // (0.9 points is 0.0225 in x; on the axis is 0, to its fit.)
            QCOMPARE_GE(values(plot, s).first().x(), -0.001);
            QCOMPARE_GE(s.agreement, 0.98);
            QVERIFY(!inkSaysSomething(s));
        }
        plot.asked = QJsonObject{{QStringLiteral("x_scale"), QStringLiteral("linear")}, {QStringLiteral("colours"), QJsonArray{QStringLiteral("#0000ff")}}};
        const QJsonObject o = toJson(plot);
        QVERIFY(!o.value(QLatin1String("series")).toArray().first().toObject().contains(QLatin1String("agreement")));
        Plot back;
        QString why;
        QVERIFY2(fromJson(o, &back, &why), qPrintable(why));
        QCOMPARE(back.asked, plot.asked);
        // A trace's ink is said, all of it or not.
        Series traced;
        traced.origin = Series::Origin::Raster;
        traced.agreement = 1;
        QVERIFY(inkSaysSomething(traced));
        Series covered;
        covered.origin = Series::Origin::Vector;
        covered.agreement = 0.9;
        QVERIFY(inkSaysSomething(covered));
    }

    void evenlySpacedMarksAreKept()
    {
        QCOMPARE(regularMarks({100, 120, 140, 160, 180}, 0.6).size(), 5);
        // A marker hanging over the axis between two ticks falls off the beat.
        const QList<double> kept = regularMarks({100, 113, 120, 140, 151.5, 160, 180}, 0.6);
        QCOMPARE(kept, (QList<double>{100, 120, 140, 160, 180}));
        // A log axis's minor marks are kept: they are most of the marks.
        const QList<double> log{0, 30.1, 47.7, 60.2, 69.9, 77.8, 84.5, 90.3, 95.4, 100};
        QCOMPARE(regularMarks(log, 0.6).size(), log.size());
    }

    void junkLowersTrust()
    {
        Plot plot;
        Reference a, b;
        a.at = QPointF(0, 100);
        a.hasX = a.hasY = true;
        b.at = QPointF(100, 0);
        b.hasX = b.hasY = true;
        b.x = b.y = 1;
        plot.calibration.references = {a, b};
        QVERIFY(plot.calibration.fit());
        Series line;
        line.origin = Series::Origin::Vector;
        for (int i = 0; i <= 100; ++i) line.points << QPointF(i, 100 - i);
        plot.series << line;
        QCOMPARE(assess(plot).level, QStringLiteral("high"));
        for (int k = 0; k < 6; ++k) {
            Series scrap;
            scrap.origin = Series::Origin::Vector;
            scrap.points = {QPointF(10 * k, 50), QPointF(10 * k + 2, 51)};
            plot.series << scrap;
        }
        const Trust t = assess(plot);
        QCOMPARE(t.level, QStringLiteral("low"));
        QVERIFY(t.reasons.join(QLatin1Char(' ')).contains(QStringLiteral("scraps")));
    }

    // ----- Bad input --------------------------------------------------------

    void emptyAndTextOnlyRegions()
    {
        const QString file = path(QStringLiteral("text.pdf"));
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            p.save();
            p.scale(1 / u, 1 / u);
            p.setFont(plotFont(11, u));
            for (int i = 0; i < 40; ++i) p.drawText(QPointF(72, 72 + i * 14) * u, QStringLiteral("A page of text with numbers 1 2 3 in it, and no plot at all."));
            p.restore();
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        QVERIFY(findPlots(source, 0).isEmpty());
        QVERIFY(findPlots(source, 99).isEmpty());
        QVERIFY(findPlots(source, -1).isEmpty());
        // A region with nothing in it.
        Plot empty = plotInRegion(source, 0, QRectF(10, 700, 100, 50));
        QVERIFY(empty.frame.isNull());
        QVERIFY(!empty.calibration.valid());
        QString why;
        QVERIFY(!extract(source, empty, Options{}, &why));
        QVERIFY(!why.isEmpty());
        // No region at all.
        Plot nothing;
        QVERIFY(!extract(source, nothing, Options{}, &why));
        // A seed nowhere.
        Series out;
        Plot plot = plotInRegion(source, 0, QRectF(300, 650, 100, 100));
        QVERIFY(!traceFrom(source, plot, QPointF(5, 5), &out, &why));
        const Trust t = assess(empty);
        QCOMPARE(t.level, QStringLiteral("none"));
    }

    void badCalibrationIsLowTrust()
    {
        const QString file = path(QStringLiteral("for_bad.pdf"));
        Axes a;
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {Curve{sine, Qt::blue}}, u); }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = plotInRegion(source, 0, a.frame.adjusted(-5, -5, 5, 5));
        QVERIFY(extract(source, plot, Options{}));
        // References that disagree by several points: the numbers come, the
        // trust says what they are worth.
        plot.calibration = Calibration{};
        for (const auto& r : {std::tuple{a.at(0, -1), 0.0, -1.0}, std::tuple{a.at(10, -1) + QPointF(12, 0), 10.0, -1.0}, std::tuple{a.at(0, 1), 0.0, 1.0}, std::tuple{a.at(10, 1), 10.0, 1.0}}) {
            Reference ref;
            ref.at = std::get<0>(r);
            ref.hasX = ref.hasY = true;
            ref.x = std::get<1>(r);
            ref.y = std::get<2>(r);
            plot.calibration.references << ref;
        }
        QString why;
        QVERIFY(plot.calibration.fit(&why));
        const Trust t = assess(plot);
        QCOMPARE(t.level, QStringLiteral("low"));
        QVERIFY(!t.reasons.isEmpty());
        QCOMPARE_GE(t.calibrationPoints, 1.0);
    }

    void aLabelThatDoesNotFitIsLeftOut()
    {
        // A page number under the plot, a stray figure among the ticks.
        const QString file = path(QStringLiteral("stray.pdf"));
        Axes a;
        QVERIFY(writeVector(file, [&](QPainter& p, double u) {
            drawPlot(p, a, {Curve{sine, Qt::blue}}, u);
            p.save();
            p.scale(1 / u, 1 / u);
            p.setPen(Qt::black);
            p.setFont(plotFont(9, u));
            p.drawText(QRectF(QPointF(a.px(5) - 30, a.frame.bottom() + 6) * u, QSizeF(60, 12) * u), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("37"));
            p.restore();
        }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(plot.calibration.valid());
        QCOMPARE_LE(plot.calibration.rms(), 0.2);
        // (Not by a tick mark, it is not taken for a label at all.)
        for (const Tick& t : std::as_const(plot.xAxis.ticks)) QVERIFY(!(t.used && t.label == QStringLiteral("37")));
        QCOMPARE(plot.xAxis.ticks.size(), 6);
    }

    void damagedPdf()
    {
        // Each damage: nothing may crash, and what can still be read is.
        const QString file = path(QStringLiteral("whole.pdf"));
        Axes a;
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {Curve{sine, Qt::blue}}, u); }));
        QFile in(file);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray whole = in.readAll();
        in.close();

        QString why;
        QVERIFY(vec::Reader::open(QByteArray(), &why) == nullptr);
        QVERIFY(vec::Reader::open(QByteArrayLiteral("not a pdf at all, not even close to one"), &why) == nullptr);

        // Its cross-reference gone: the file is searched for its objects.
        QByteArray noXref = whole;
        const qsizetype at = noXref.lastIndexOf("startxref");
        QVERIFY(at > 0);
        noXref.truncate(at);
        auto reader = vec::Reader::open(noXref, &why);
        QVERIFY2(reader != nullptr, qPrintable(why));
        QCOMPARE(reader->pageCount(), 1);
        vec::Graphics g;
        QVERIFY(reader->page(0, &g, &why));
        QCOMPARE_GE(g.paths.size(), 10);

        // Its offsets all wrong.
        QByteArray shifted = whole;
        const qsizetype xrefAt = shifted.lastIndexOf("xref\n0 ");
        if (xrefAt > 0) {
            shifted.insert(xrefAt - 1, QByteArray(137, ' '));
            auto r2 = vec::Reader::open(shifted, &why);
            QVERIFY(r2 != nullptr);
            vec::Graphics g2;
            QVERIFY(r2->page(0, &g2, &why));
            QCOMPARE_GE(g2.paths.size(), 10);
        }
        // A page that is not there.
        QVERIFY(!reader->page(5, &g, &why));
        QVERIFY(!reader->page(-1, &g, &why));

        // Cut short anywhere.
        for (const double share : {0.1, 0.3, 0.5, 0.7, 0.9, 0.99}) {
            auto r3 = vec::Reader::open(whole.left(qsizetype(whole.size() * share)), &why);
            if (r3 != nullptr) {
                for (int i = 0; i < r3->pageCount(); ++i) {
                    vec::Graphics g3;
                    r3->page(i, &g3, &why);
                }
            }
        }
    }

    void handWrittenPdfs()
    {
        // What QPdfWriter never writes: a rotated page, a crop box, indirect
        // lengths, a form drawn as a marker, LZW and ASCII filters, an object
        // stream, a reference to itself. Written by hand.
        const QByteArray page = "q 1 0 0 1 100 200 cm 0 0 m 100 50 l 200 0 l S Q\n";
        const auto writePdf = [&](const QByteArray& pageDict, const QByteArray& extra = {}) {
            QByteArray out = "%PDF-1.5\n";
            out += "1 0 obj <</Type /Catalog /Pages 2 0 R>> endobj\n";
            out += "2 0 obj <</Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 600 800]>> endobj\n";
            out += "3 0 obj <</Type /Page /Parent 2 0 R /Contents 4 0 R " + pageDict + ">> endobj\n";
            out += "4 0 obj <</Length 5 0 R>> stream\n" + page + "endstream endobj\n";
            out += "5 0 obj " + QByteArray::number(page.size()) + " endobj\n";
            out += extra;
            out += "trailer <</Root 1 0 R>>\n";
            return out;
        };
        const auto line = [](const QByteArray& pdf, QString* why) {
            vec::Graphics g;
            auto r = vec::Reader::open(pdf, why);
            if (r == nullptr || !r->page(0, &g, why) || g.paths.isEmpty()) return QPolygonF();
            return g.paths.first().parts.first().points;
        };
        QString why;
        // Plain: y runs down from the top of 800.
        QPolygonF q = line(writePdf(QByteArray()), &why);
        QVERIFY2(!q.isEmpty(), qPrintable(why));
        QCOMPARE(q.first(), QPointF(100, 600));
        QCOMPARE(q.at(1), QPointF(200, 550));
        // A crop box moves the origin.
        q = line(writePdf("/CropBox [50 100 550 700]"), &why);
        QVERIFY(!q.isEmpty());
        QCOMPARE(q.first(), QPointF(50, 500));
        // Rotated pages: where the page shows it.
        {
            vec::Graphics g;
            auto r = vec::Reader::open(writePdf("/Rotate 90"), &why);
            QVERIFY(r != nullptr);
            QVERIFY(r->page(0, &g, &why));
            QCOMPARE(g.size, QSizeF(800, 600));
            QCOMPARE(g.paths.first().parts.first().points.first(), QPointF(200, 100));
        }
        {
            vec::Graphics g;
            auto r = vec::Reader::open(writePdf("/Rotate 180"), &why);
            QVERIFY(r != nullptr);
            QVERIFY(r->page(0, &g, &why));
            QCOMPARE(g.paths.first().parts.first().points.first(), QPointF(500, 200));
        }
        {
            vec::Graphics g;
            auto r = vec::Reader::open(writePdf("/Rotate 270"), &why);
            QVERIFY(r != nullptr);
            QVERIFY(r->page(0, &g, &why));
            QCOMPARE(g.size, QSizeF(800, 600));
            QCOMPARE(g.paths.first().parts.first().points.first(), QPointF(600, 500));
        }
        // Encrypted: said, not misread.
        {
            bool encrypted = false;
            QByteArray pdf = writePdf(QByteArray());
            pdf.replace("trailer <</Root 1 0 R>>", "trailer <</Root 1 0 R /Encrypt 9 0 R>>");
            pdf.replace("trailer", "9 0 obj <</Filter /Standard>> endobj\ntrailer");
            QVERIFY(vec::Reader::open(pdf, &why, &encrypted) == nullptr);
            QVERIFY(encrypted);
        }
        // A page tree that points at itself, a form that draws itself: done
        // with, not followed for ever.
        {
            QByteArray pdf = "%PDF-1.5\n1 0 obj <</Type /Catalog /Pages 2 0 R>> endobj\n"
                             "2 0 obj <</Type /Pages /Kids [2 0 R 3 0 R] /MediaBox [0 0 100 100]>> endobj\n"
                             "3 0 obj <</Type /Page /Parent 2 0 R /Contents 4 0 R /Resources <</XObject <</F 5 0 R>>>>>> endobj\n"
                             "4 0 obj <</Length 22>> stream\n/F Do 0 0 m 10 10 l S \nendstream endobj\n"
                             "5 0 obj <</Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources <</XObject <</F 5 0 R>>>> /Length 20>> stream\n/F Do 1 1 m 2 2 l S \nendstream endobj\n"
                             "trailer <</Root 1 0 R>>\n";
            vec::Graphics g;
            auto r = vec::Reader::open(pdf, &why);
            QVERIFY2(r != nullptr, qPrintable(why));
            QCOMPARE(r->pageCount(), 1);
            QVERIFY(r->page(0, &g, &why));
            QCOMPARE(g.paths.size(), 2);
        }
        // A marker by form: the origin is where it was put.
        {
            QByteArray pdf = "%PDF-1.5\n1 0 obj <</Type /Catalog /Pages 2 0 R>> endobj\n"
                             "2 0 obj <</Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 100]>> endobj\n"
                             "3 0 obj <</Type /Page /Parent 2 0 R /Contents 4 0 R /Resources <</XObject <</M 5 0 R>>>>>> endobj\n"
                             "4 0 obj <</Length 60>> stream\nq 1 0 0 1 30 40 cm /M Do Q q 1 0 0 1 60 70 cm /M Do Q \nendstream endobj\n"
                             "5 0 obj <</Type /XObject /Subtype /Form /BBox [-5 -5 5 5] /Length 25>> stream\n-3 -3 6 6 re f \nendstream endobj\n"
                             "trailer <</Root 1 0 R>>\n";
            vec::Graphics g;
            auto r = vec::Reader::open(pdf, &why);
            QVERIFY2(r != nullptr, qPrintable(why));
            QVERIFY(r->page(0, &g, &why));
            QCOMPARE(g.paths.size(), 2);
            QCOMPARE(g.paths.at(0).origin, QPointF(30, 60));
            QCOMPARE(g.paths.at(1).origin, QPointF(60, 30));
            QCOMPARE(g.paths.at(0).form, g.paths.at(1).form);
        }
        // Filters by hand: ASCIIHex of the same content.
        {
            QByteArray pdf = "%PDF-1.5\n1 0 obj <</Type /Catalog /Pages 2 0 R>> endobj\n"
                             "2 0 obj <</Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 100]>> endobj\n"
                             "3 0 obj <</Type /Page /Parent 2 0 R /Contents 4 0 R>> endobj\n"
                             "4 0 obj <</Filter /ASCIIHexDecode /Length " + QByteArray::number(QByteArray("0 0 m 10 10 l S").toHex().size() + 1) + ">> stream\n" +
                             QByteArray("0 0 m 10 10 l S").toHex() + ">\nendstream endobj\ntrailer <</Root 1 0 R>>\n";
            vec::Graphics g;
            auto r = vec::Reader::open(pdf, &why);
            QVERIFY2(r != nullptr, qPrintable(why));
            QVERIFY(r->page(0, &g, &why));
            QCOMPARE(g.paths.size(), 1);
        }
    }

    void fuzzTheReader()
    {
        // Random damage to a real file and to hand-made ones: never a crash,
        // never an end that does not come. (Nothing to compare the answer with:
        // what is under test is that there is one.)
        const QString file = path(QStringLiteral("fuzz.pdf"));
        Axes a;
        a.grid = true;
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {Curve{sine, Qt::blue}, Curve{cosine, Qt::red, 1, Qt::DashLine}}, u, false); }));
        QFile in(file);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray whole = in.readAll();
        in.close();
        std::mt19937 random(0x51A7);
        const auto pick = [&](qsizetype n) { return qsizetype(random() % quint32(std::max<qsizetype>(1, n))); };
        static const char* const fragments[] = {"<<", ">>", "[", "]", "obj", "endobj", "stream", "endstream", "/Filter /FlateDecode", "/Length 999999999", "/Kids [3 0 R]", "/Type /ObjStm", "/N 99999999", "/First -1",
                                                "xref", "trailer", "startxref\n1\n", "99999 0 R", "/Prev 0", "/W [8 8 8]", "/Index [0 2147483647]", "/Predictor 15 /Columns 99999999", "(((((", "<ff"};
        int opened = 0;
        for (int round = 0; round < 1500; ++round) {
            QByteArray bytes = whole;
            const int edits = 1 + int(random() % 24);
            for (int e = 0; e < edits; ++e) {
                switch (random() % 5) {
                case 0: bytes[pick(bytes.size())] = char(random()); break;
                case 1: bytes.remove(pick(bytes.size()), pick(400)); break;
                case 2: bytes.insert(pick(bytes.size()), fragments[random() % std::size(fragments)]); break;
                case 3: {
                    const qsizetype from = pick(bytes.size());
                    bytes.insert(pick(bytes.size()), bytes.mid(from, pick(200)));
                    break;
                }
                default: bytes.truncate(std::max<qsizetype>(16, bytes.size() - pick(bytes.size() / 4 + 1))); break;
                }
            }
            QString why;
            auto reader = vec::Reader::open(bytes, &why);
            if (reader == nullptr) continue;
            ++opened;
            for (int page = 0; page < std::min(reader->pageCount(), 3); ++page) {
                vec::Graphics g;
                reader->page(page, &g, &why);
            }
        }
        qInfo("fuzz of the reader: %d of 1500 damaged files still opened", opened);
        QCOMPARE_GE(opened, 50);
    }

    void fuzzTheContentStream()
    {
        // Operators in any order, with any operands, in a stream Flate never saw.
        std::mt19937 random(0xC0DE);
        static const char* const ops[] = {"q", "Q", "cm", "w", "d", "m", "l", "c", "v", "y", "h", "re", "S", "s", "f", "F", "f*", "B", "B*", "b", "b*", "n", "W", "W*", "g", "G", "rg", "RG", "k", "K", "cs", "CS", "sc", "SC", "scn", "SCN", "gs", "Do", "BI", "ID", "EI", "BT", "ET", "Tj", "TJ", "sh", "BDC", "EMC", "(", ")", "<", ">", "<<", ">>", "[", "]", "/M", "/", "1e9", "-1e308", "nan", "99999999999999999999", "0.000000001", "0"};
        for (int round = 0; round < 400; ++round) {
            QByteArray content;
            const int n = int(random() % 400);
            for (int i = 0; i < n; ++i) {
                content += ops[random() % std::size(ops)];
                content += ' ';
            }
            const QByteArray pdf = "%PDF-1.5\n1 0 obj <</Type /Catalog /Pages 2 0 R>> endobj\n2 0 obj <</Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 100]>> endobj\n"
                                   "3 0 obj <</Type /Page /Parent 2 0 R /Contents 4 0 R /Resources <</XObject <</M 3 0 R>> /ExtGState <</M 1 0 R>> /ColorSpace <</M [/Indexed]>>>>>> endobj\n"
                                   "4 0 obj <</Length " + QByteArray::number(content.size()) + ">> stream\n" + content + "\nendstream endobj\ntrailer <</Root 1 0 R>>\n";
            QString why;
            auto reader = vec::Reader::open(pdf, &why);
            QVERIFY2(reader != nullptr, qPrintable(why));
            vec::Graphics g;
            QVERIFY(reader->page(0, &g, &why));
        }
    }

    // ----- Results ----------------------------------------------------------

    void resultsCsvAndDataset()
    {
        Plot plot;
        Reference a, b;
        a.at = QPointF(0, 100);
        a.hasX = a.hasY = true;
        a.x = 0;
        a.y = 0;
        b.at = QPointF(100, 0);
        b.hasX = b.hasY = true;
        b.x = 10;
        b.y = 1;
        plot.calibration.references = {a, b};
        QString why;
        QVERIFY2(plot.calibration.fit(&why), qPrintable(why));
        Series s;
        s.name = QStringLiteral("=SUM(A1), \"evil\"");
        s.points = {QPointF(0, 100), QPointF(50, 50), QPointF(100, 0)};
        plot.series << s;
        const QString text = csv(plot);
        QVERIFY(text.startsWith(QStringLiteral("series,x,y,piece\n")));
        QCOMPARE(text.count(QLatin1Char('\n')), 4);
        QVERIFY2(text.contains(QStringLiteral("'\"=SUM")) || text.contains(QStringLiteral("\"=SUM")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral(",5,0.5,1\n")));
        const QByteArray data = dataset(plot);
        QVERIFY(data.startsWith("<Qucs Dataset"));
        QVERIFY2(data.contains("<indep SUM_A1_evil_x 3>"), data.constData());
        QVERIFY(data.contains("<dep SUM_A1_evil SUM_A1_evil_x>"));
        // Two of the same name are told apart.
        plot.series << s;
        const QByteArray twice = dataset(plot);
        QVERIFY(twice.contains("<dep SUM_A1_evil_2 SUM_A1_evil_2_x>"));
        // A series with gaps: its pieces numbered in the csv; in the dataset
        // all of it under its name, a NaN in each gap (where a diagram ends
        // the line), whether it has gaps or not - and a variable each piece
        // too. Each series' own name taken first (the open items' A1).
        Plot gapped = plot;
        gapped.series.clear();
        Series model;
        model.name = QStringLiteral("model");
        model.points = {QPointF(0, 100), QPointF(10, 90), QPointF(40, 60), QPointF(50, 50), QPointF(90, 10), QPointF(100, 0)};
        model.breaks = {2, 4};
        gapped.series << model;
        Series model2 = model;
        model2.name = QStringLiteral("model 1");   // (whose name is a piece's)
        model2.breaks.clear();
        gapped.series << model2;
        const QStringList rows = csv(gapped).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(rows.size(), 1 + 6 + 6);
        QStringList pieceColumn;
        for (int k = 1; k < rows.size(); ++k) pieceColumn << rows.at(k).section(QLatin1Char(','), -1);
        QCOMPARE(pieceColumn.join(QLatin1Char(' ')), QStringLiteral("1 1 2 2 3 3 1 1 1 1 1 1"));
        QCOMPARE(rows.at(3), QStringLiteral("model,4,0.4,2"));
        QCOMPARE(rows.at(12), QStringLiteral("model 1,10,1,1"));
        QStringList variables;
        const QByteArray pieces = dataset(gapped, &variables);
        QCOMPARE(variables, (QStringList{QStringLiteral("model"), QStringLiteral("model_1_2"), QStringLiteral("model_2"), QStringLiteral("model_3"), QStringLiteral("model_1")}));
        QVERIFY2(pieces.contains("<indep model_x 8>\n") && pieces.contains("\n  1\n  2.5\n  4\n  5\n  7\n  9\n  10\n</indep>"), pieces.constData());
        QVERIFY2(pieces.contains("<dep model model_x>\n  0\n  0.1\n  nan\n  0.4\n  0.5\n  nan\n  0.9\n  1\n"), pieces.constData());
        QVERIFY(pieces.contains("<indep model_1_2_x 2>") && pieces.contains("<indep model_2_x 2>") && pieces.contains("<indep model_3_x 2>"));
        QVERIFY(pieces.contains("<dep model_2 model_2_x>\n  0.4\n  0.5\n"));
        QVERIFY(pieces.contains("<indep model_1_x 6>"));
        // Without gaps the same name: done again, a diagram's trace is there.
        Plot whole = gapped;
        whole.series[0].breaks.clear();
        QStringList again;
        dataset(whole, &again);
        QCOMPARE(again.first(), QStringLiteral("model"));
        QCOMPARE(again.last(), QStringLiteral("model_1"));
        QCOMPARE(variableName(QStringLiteral("1 weird name!")), QStringLiteral("s_1_weird_name"));
        QCOMPARE(variableName(QString()), QStringLiteral("s_"));
    }

    void thinnedKeepsTheShape()
    {
        QList<QPointF> line;
        for (int i = 0; i < 5000; ++i) line << QPointF(i * 0.1, i == 2500 ? 40.0 : std::sin(i * 0.002) * 10);
        const QList<int> kept = thinned(line, 100);
        QCOMPARE(kept.size(), 100);
        QCOMPARE(kept.first(), 0);
        QCOMPARE(kept.last(), 4999);
        QVERIFY2(kept.contains(2500), "the spike");
        QVERIFY(std::is_sorted(kept.begin(), kept.end()));
        QCOMPARE(thinned(line, 0).size(), 5000);
        QCOMPARE(thinned(line.mid(0, 3), 100).size(), 3);
        QCOMPARE(thinned({}, 10).size(), 0);
    }

    // ----- Kept with the PDF ------------------------------------------------

    void savedAndRestored()
    {
        const QString file = path(QStringLiteral("kept.pdf"));
        Axes a;
        a.xLog = true;
        a.xMin = 1;
        a.xMax = 1e4;
        a.xTicks = {1, 10, 100, 1000, 10000};
        const auto f = [](double x) { return std::sin(std::log10(x) * 2); };
        QVERIFY(writeVector(file, [&](QPainter& p, double u) { drawPlot(p, a, {Curve{f, Qt::blue, 1.2, Qt::SolidLine, QStringLiteral("name")}}, u, true); }));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        Plot plot = firstPlot(source);
        QVERIFY(extract(source, plot, Options{}));
        plot.name = QStringLiteral("The first");
        Series hand;
        hand.name = QStringLiteral("by hand");
        hand.shape = Series::Shape::ByHand;
        hand.origin = Series::Origin::Hand;
        hand.points = {QPointF(150, 200), QPointF(200, 210)};
        plot.series << hand;
        QString why;
        QVERIFY2(saveSidecar(file, {plot}, &why), qPrintable(why));
        QVERIFY(QFileInfo::exists(sidecarPath(file)));
        QList<Plot> back;
        bool stale = true;
        QVERIFY2(loadSidecar(file, &back, &why, &stale), qPrintable(why));
        QVERIFY(!stale);
        QCOMPARE(back.size(), 1);
        const Plot& p = back.first();
        QCOMPARE(p.name, plot.name);
        QCOMPARE(p.page, plot.page);
        QCOMPARE(p.frame, plot.frame);
        QVERIFY(p.calibration.valid());
        QCOMPARE(p.calibration.xScale, Scale::Log);
        QCOMPARE(p.series.size(), plot.series.size());
        QCOMPARE(p.series.first().points.size(), plot.series.first().points.size());
        QCOMPARE(p.series.last().shape, Series::Shape::ByHand);
        const QPointF was = plot.calibration.toValue(plot.series.first().points.at(10));
        const QPointF is = p.calibration.toValue(p.series.first().points.at(10));
        QCOMPARE_LE(QLineF(was, is).length(), 1e-9);
        // The PDF changes: still there, but said.
        {
            QFile touch(file);
            QVERIFY(touch.open(QIODevice::Append));
            touch.write("\n% more\n");
        }
        QVERIFY(loadSidecar(file, &back, &why, &stale));
        QVERIFY(stale);
        // Damaged: refused, said, left where it is.
        const QString side = sidecarPath(file);
        {
            QFile f2(side);
            QVERIFY(f2.open(QIODevice::WriteOnly));
            f2.write("{ not json");
        }
        why.clear();
        QVERIFY(!loadSidecar(file, &back, &why, &stale));
        QVERIFY(!why.isEmpty());
        QVERIFY(QFileInfo::exists(side));
        // Nothing left to keep: the file goes.
        QVERIFY(saveSidecar(file, {}, &why));
        QVERIFY(!QFileInfo::exists(side));
        // Nothing kept is no error.
        QVERIFY(loadSidecar(file, &back, &why));
        QVERIFY(back.isEmpty());
    }

    void fuzzTheSavedFile()
    {
        Plot plot;
        plot.page = 2;
        plot.region = QRectF(1, 2, 300, 200);
        plot.frame = QRectF(10, 10, 200, 100);
        plot.name = QStringLiteral("x");
        Reference a, b;
        a.at = QPointF(10, 110);
        a.hasX = a.hasY = true;
        b.at = QPointF(210, 10);
        b.hasX = b.hasY = true;
        b.x = 1;
        b.y = 1;
        plot.calibration.references = {a, b};
        Series s;
        s.name = QStringLiteral("c");
        s.points = {QPointF(1, 2), QPointF(3, 4), QPointF(5, 6)};
        s.breaks = {1};
        plot.series << s;
        const QJsonObject root{{QStringLiteral("format"), QStringLiteral("qucs-s plot digitizer")}, {QStringLiteral("version"), 1}, {QStringLiteral("plots"), QJsonArray{toJson(plot)}}};
        const QByteArray whole = QJsonDocument(root).toJson(QJsonDocument::Compact);
        QList<Plot> back;
        QString why;
        QVERIFY2(parseSidecar(whole, &back, &why), qPrintable(why));
        QCOMPARE(back.size(), 1);
        QCOMPARE(back.first().series.first().breaks, QList<int>{1});

        std::mt19937 random(0xFEED);
        const auto pick = [&](qsizetype n) { return qsizetype(random() % quint32(std::max<qsizetype>(1, n))); };
        static const char* const values_[] = {"null", "true", "-1", "1e308", "-1e308", "1e400", "\"\"", "[]", "{}", "[1]", "[[[[", "\"polar\"", "\"projective\"", "\"log\"", "0", "99999999999"};
        int good = 0;
        for (int round = 0; round < 4000; ++round) {
            QByteArray bytes = whole;
            const int edits = 1 + int(random() % 6);
            for (int e = 0; e < edits; ++e) {
                switch (random() % 4) {
                case 0: bytes[pick(bytes.size())] = char(random()); break;
                case 1: bytes.remove(pick(bytes.size()), pick(30)); break;
                case 2: bytes.insert(pick(bytes.size()), values_[random() % std::size(values_)]); break;
                default: {
                    // A number replaced by another kind of thing.
                    const qsizetype at = bytes.indexOf(':', pick(bytes.size()));
                    if (at > 0) {
                        qsizetype end = at + 1;
                        while (end < bytes.size() && bytes.at(end) != ',' && bytes.at(end) != '}' && bytes.at(end) != ']') ++end;
                        bytes.replace(at + 1, end - at - 1, values_[random() % std::size(values_)]);
                    }
                }
                }
            }
            QList<Plot> plots;
            if (parseSidecar(bytes, &plots, &why)) {
                ++good;
                // What is taken must be safe to use.
                for (const Plot& p : std::as_const(plots)) {
                    (void)assess(p);
                    (void)csv(p);
                    (void)dataset(p);
                    (void)toJson(p);
                    for (const Series& se : p.series) (void)values(p, se);
                    QVERIFY(p.page >= 0);
                }
            }
        }
        qInfo("fuzz of the saved file: %d of 4000 damaged files still read", good);
        QCOMPARE_GE(good, 100);
    }

    void aRealPdfFromTheRepository()
    {
#ifdef QUCS_REPO_PDF
        // The repository's own plot: test1.pdf of the Python tools, written by
        // matplotlib from their sample dataset.
        const QString file = QStringLiteral(QUCS_REPO_PDF);
        QPdfDocument pdf;
        QCOMPARE(pdf.load(file), QPdfDocument::Error::None);
        Source source(&pdf, file);
        QVERIFY2(source.hasVectors(), qPrintable(source.vectorProblem()));
        const QList<Plot> plots = findPlots(source, 0);
        qInfo("%s: %lld plots on its first page", qPrintable(QFileInfo(file).fileName()), qlonglong(plots.size()));
        QCOMPARE_GE(plots.size(), 1);
        Plot plot = plots.first();
        QVERIFY2(plot.calibration.valid(), qPrintable(plot.notes.join(QLatin1Char(' '))));
        QCOMPARE_LE(plot.calibration.rms(), 0.5);
        QVERIFY(extract(source, plot, Options{}));
        QCOMPARE_GE(plot.series.size(), 1);
        for (const Series& s : std::as_const(plot.series)) {
            qInfo("  %s: %lld points, ink under %.3f of them", qPrintable(s.name), qlonglong(s.points.size()), s.agreement);
            if (s.agreement >= 0) QCOMPARE_GE(s.agreement, 0.9);
        }
#else
        QSKIP("No PDF of the repository's named to this build.");
#endif
    }

private:
    static double colourDistanceTo(const QColor& a, const QColor& b)
    {
        return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
    }
};

QTEST_MAIN(TestPlotDigitizer)
#include "test_plot_digitizer.moc"
