/*
 * The plot digitizer in the program: Claude's tools (find_plots, digitize_plot,
 * get_digitized), in the window and over stdio with no window; and the viewer's
 * panel, driven as a user would - the mouse on the pages, its buttons, Escape
 * and Ctrl+Z through the window's own shortcuts. (The numbers themselves are
 * held to the truth in test_plot_digitizer.cpp.)
 */
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPageSize>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include <cmath>
#include <cstdio>
#include <functional>

#include "config.h"
#include "dataset.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "pdfdoc.h"
#include "plotdigitizer.h"
#include "plotdigitizerpanel.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "settings.h"

using namespace qucs_s::digitizer;
using qucs_s::pdf::PageView;

namespace {

constexpr double kW = 612, kH = 792;
const QRectF kFrame(100, 120, 400, 280);

double px(double x) { return kFrame.left() + x / 10 * kFrame.width(); }
double py(double y) { return kFrame.bottom() - (y + 1) / 2 * kFrame.height(); }
double sine(double x) { return std::sin(x); }
double wave(double x) { return 0.5 * std::cos(1.7 * x); }

// A plot with labels, a legend and two curves; vector, or (\a picture) drawn
// into an image first, with no text at all when \a labels is false.
void drawPlot(QPainter& p, bool labels, bool legend)
{
    const double unit = 8;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 0.8));
    p.drawRect(kFrame);
    const QList<double> xs{0, 2, 4, 6, 8, 10}, ys{-1, -0.5, 0, 0.5, 1};
    for (const double x : xs) p.drawLine(QPointF(px(x), kFrame.bottom()), QPointF(px(x), kFrame.bottom() + 4));
    for (const double y : ys) p.drawLine(QPointF(kFrame.left() - 4, py(y)), QPointF(kFrame.left(), py(y)));
    QFont font(QStringLiteral("Helvetica"));
    font.setPixelSize(int(9 * unit));
    const auto text = [&](QRectF box, int align, const QString& s) {
        p.save();
        p.scale(1 / unit, 1 / unit);
        p.setFont(font);
        p.setPen(Qt::black);
        p.drawText(QRectF(box.topLeft() * unit, box.size() * unit), align, s);
        p.restore();
    };
    if (labels) {
        for (const double x : xs) text(QRectF(px(x) - 30, kFrame.bottom() + 6, 60, 12), Qt::AlignHCenter | Qt::AlignTop, QString::number(x));
        for (const double y : ys) text(QRectF(kFrame.left() - 66, py(y) - 7, 60, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(y));
    }
    p.setClipRect(kFrame);
    const auto curve = [&](const std::function<double(double)>& f, const QColor& colour) {
        QPolygonF line;
        for (int i = 0; i <= 300; ++i) {
            const double x = 10.0 * i / 300;
            line << QPointF(px(x), py(f(x)));
        }
        QPen pen(colour, 1.2);
        p.setPen(pen);
        p.drawPolyline(line);
    };
    curve(sine, Qt::blue);
    curve(wave, Qt::red);
    p.setClipping(false);
    if (legend) {
        const double x = kFrame.right() - 120;
        p.setPen(QPen(Qt::blue, 1.2));
        p.drawLine(QPointF(x, kFrame.top() + 14), QPointF(x + 24, kFrame.top() + 14));
        text(QRectF(x + 28, kFrame.top() + 7, 90, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("sine"));
        p.setPen(QPen(Qt::red, 1.2));
        p.drawLine(QPointF(x, kFrame.top() + 28), QPointF(x + 24, kFrame.top() + 28));
        text(QRectF(x + 28, kFrame.top() + 21, 90, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("wave"));
    }
}

bool writeVectorPlot(const QString& file, int pages = 1)
{
    QPdfWriter writer(file);
    writer.setPageSize(QPageSize(QSizeF(kW, kH), QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(720);
    QPainter p;
    if (!p.begin(&writer)) return false;
    p.scale(writer.width() / kW, writer.width() / kW);
    for (int i = 0; i < pages; ++i) {
        if (i > 0) writer.newPage();
        if (i == 0) drawPlot(p, true, true);
    }
    p.end();
    return true;
}

// The same as a scan: a picture of it, no text.
bool writeScannedPlot(const QString& file)
{
    const double k = 300.0 / 72;
    QImage image(int(kW * k), int(kH * k), QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(k, k);
        drawPlot(p, false, false);
    }
    QPdfWriter writer(file);
    writer.setPageSize(QPageSize(QSizeF(kW, kH), QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(720);
    QPainter p;
    if (!p.begin(&writer)) return false;
    p.scale(writer.width() / kW, writer.width() / kW);
    p.drawImage(QRectF(0, 0, kW, kH), image);
    p.end();
    return true;
}

} // namespace

class TestPlotDigitizerApp : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;
    QString vectorFile, scanFile;

    QJsonObject call(const QString& tool, const QJsonObject& args = {}) { return control->callNow(tool, args, 60000); }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QJsonObject json(const QJsonObject& r)
    {
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text"))
                return QJsonDocument::fromJson(v.toObject().value(QStringLiteral("text")).toString().toUtf8()).object();
        return {};
    }
    static QImage picture(const QJsonObject& r)
    {
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("image")) {
                QImage image;
                image.loadFromData(QByteArray::fromBase64(v.toObject().value(QStringLiteral("data")).toString().toLatin1()));
                return image;
            }
        return {};
    }
    QString path(const QString& name) const { return dir.filePath("ws/" + name); }

    // How far a series' values are from a function, in parts of a span of 2.
    static double worst(const QJsonObject& series, const std::function<double(double)>& f)
    {
        const QJsonArray xs = series.value("x").toArray(), ys = series.value("y").toArray();
        double most = 0;
        for (int i = 0; i < xs.size(); ++i) most = std::max(most, std::abs(ys.at(i).toDouble() - f(xs.at(i).toDouble())) / 2);
        return most;
    }

    PdfDoc* open(const QString& file)
    {
        const QJsonObject r = call("show_pdf", {{"path", file}});
        if (failed(r)) {
            qWarning("%s", qPrintable(QucsControl::textOf(r)));
            return nullptr;
        }
        auto* doc = dynamic_cast<PdfDoc*>(app->getDoc());
        if (doc != nullptr) {
            doc->view()->setFit(PageView::Fit::Page);
            QTest::qWait(30);
            QCoreApplication::processEvents();
        }
        return doc;
    }
    void closeAll()
    {
        for (const QString& file : {vectorFile, scanFile}) call("close_document", {{"path", file}});
        QCoreApplication::processEvents();
    }
    // The pointer on \a point of page \a page, in the view.
    static QPoint at(PageView* view, QPointF point, int page = 0)
    {
        return view->pageRect(page).topLeft() + (point * view->pixelsPerPoint()).toPoint();
    }
    static void clickAt(PageView* view, QPointF point)
    {
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at(view, point));
        QCoreApplication::processEvents();
    }
    static void drag(PageView* view, QPointF from, QPointF to)
    {
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, at(view, from));
        QTest::mouseMove(view->viewport(), at(view, (from + to) / 2));
        QTest::mouseMove(view->viewport(), at(view, to));
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, at(view, to));
        QCoreApplication::processEvents();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("ws"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("ws"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("ws"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        vectorFile = path("vector.pdf");
        scanFile = path("scan.pdf");
        QVERIFY(writeVectorPlot(vectorFile, 3));
        QVERIFY(writeScannedPlot(scanFile));
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1300, 950);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
    }

    void cleanupTestCase()
    {
        closeAll();
        delete app;
    }

    void cleanup()
    {
        closeAll();
        QFile::remove(sidecarPath(vectorFile));
        QFile::remove(sidecarPath(scanFile));
    }

    // ----- Claude's tools ---------------------------------------------------

    void findPlots()
    {
        QJsonObject o = json(call("find_plots", {{"path", vectorFile}}));
        QCOMPARE(o.value("pages_in_all").toInt(), 3);
        QCOMPARE(o.value("plots").toArray().size(), 1);
        const QJsonObject plot = o.value("plots").toArray().first().toObject();
        QCOMPARE(plot.value("page").toInt(), 1);
        QCOMPARE(plot.value("route").toString(), QStringLiteral("vector"));
        QVERIFY(plot.value("calibration").toObject().value("valid").toBool());
        const QJsonObject x = plot.value("calibration").toObject().value("x_axis").toObject();
        QCOMPARE(x.value("scale").toString(), QStringLiteral("linear"));
        QCOMPARE_LE(std::abs(x.value("from").toDouble()), 0.01);
        QCOMPARE_LE(std::abs(x.value("to").toDouble() - 10), 0.01);
        // Its pages narrowed.
        QCOMPARE(json(call("find_plots", {{"path", vectorFile}, {"pages", "2-3"}})).value("plots").toArray().size(), 0);
        QVERIFY(json(call("find_plots", {{"path", vectorFile}, {"pages", 2}})).contains("note"));
        // Not a PDF, not there.
        QVERIFY(failed(call("find_plots", {{"path", path("nothing.pdf")}})));
        QFile bad(path("bad.pdf"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("this is not a PDF");
        bad.close();
        QVERIFY(failed(call("find_plots", {{"path", path("bad.pdf")}})));
        QVERIFY(failed(call("find_plots", {{"path", vectorFile}, {"pages", "nope"}})));
        // An argument it does not know is refused, as every tool's is.
        QVERIFY(failed(call("find_plots", {{"path", vectorFile}, {"no_such", 1}})));
    }

    void digitizeInOneCall()
    {
        const QJsonObject r = call("digitize_plot", {{"path", vectorFile}, {"page", 1}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        const QJsonObject o = json(r);
        QCOMPARE(o.value("route").toString(), QStringLiteral("vector"));
        const QJsonArray series = o.value("series").toArray();
        QCOMPARE(series.size(), 2);
        const QJsonObject s = series.at(0).toObject(), w = series.at(1).toObject();
        QCOMPARE(s.value("name").toString(), QStringLiteral("sine"));
        QCOMPARE(w.value("name").toString(), QStringLiteral("wave"));
        QCOMPARE(s.value("points_in_all").toInt(), 301);
        QCOMPARE_LE(s.value("x").toArray().size(), 200);
        QCOMPARE_LE(worst(s, sine), 0.0005);
        QCOMPARE_LE(worst(w, wave), 0.0005);
        // How far to trust it: in a call.
        const QJsonObject trust = o.value("trust").toObject();
        QCOMPARE(trust.value("level").toString(), QStringLiteral("high"));
        QVERIFY(trust.value("each_value_good_to").toObject().contains("points"));
        // The overlay comes with it.
        const QImage overlay = picture(r);
        QVERIFY(!overlay.isNull());
        QCOMPARE_GE(overlay.width(), 400);
        QVERIFY(o.contains("ms"));
        // Without it, on request; and every point, on request.
        const QJsonObject all = call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"picture", false}, {"max_points", 0}});
        QVERIFY(picture(all).isNull());
        QCOMPARE(json(all).value("series").toArray().first().toObject().value("x").toArray().size(), 301);
        // No page named: the first with a plot.
        QCOMPARE(json(call("digitize_plot", {{"path", vectorFile}, {"picture", false}})).value("page").toInt(), 1);
        // A page without one says so.
        QVERIFY(failed(call("digitize_plot", {{"path", vectorFile}, {"page", 2}})));
        QVERIFY(failed(call("digitize_plot", {{"path", vectorFile}, {"page", 99}})));
        QVERIFY(failed(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"plot", 9}})));
    }

    void anEmptyRegionSaysSo()
    {
        const QJsonObject o = json(call("digitize_plot", {{"path", vectorFile}, {"page", 2}, {"region", QJsonArray{50, 50, 200, 200}}, {"picture", false}}));
        QCOMPARE(o.value("series").toArray().size(), 0);
        QVERIFY(!o.value("calibration").toObject().value("valid").toBool());
        QCOMPARE(o.value("trust").toObject().value("level").toString(), QStringLiteral("none"));
        QVERIFY(o.contains("notes"));
        // A region that is none.
        QVERIFY(failed(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"region", QJsonArray{900, 900, 950, 950}}})));
        QVERIFY(failed(call("digitize_plot", {{"path", vectorFile}, {"region", QJsonArray{0, 0, 1, 1}}})));
    }

    void aScanWithTicksGivenValues()
    {
        // First look: its frame and marks, no values to go with them.
        QJsonObject first = json(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"picture", false}}));
        QCOMPARE(first.value("route").toString(), QStringLiteral("raster"));
        QVERIFY(!first.value("calibration").toObject().value("valid").toBool());
        const QJsonObject calibration = first.value("calibration").toObject();
        QCOMPARE(calibration.value("x_marks_at").toArray().size(), 6);
        QCOMPARE(calibration.value("y_marks_at").toArray().size(), 5);
        // Claude reads them off the picture, and says.
        const QJsonObject r = call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"x_values", QJsonArray{0, 2, 4, 6, 8, 10}}, {"y_values", QJsonArray{1, 0.5, 0, -0.5, -1}}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        const QJsonObject o = json(r);
        QVERIFY(o.value("calibration").toObject().value("valid").toBool());
        QCOMPARE_LE(o.value("calibration").toObject().value("rms_points").toDouble(), 0.3);
        QCOMPARE(o.value("trust").toObject().value("level").toString(), QStringLiteral("medium"));
        // Which is which, by colour.
        const QJsonArray series = o.value("series").toArray();
        QCOMPARE(series.size(), 2);
        const auto blueOf = [](const QJsonObject& o) { const QColor c(o.value("colour").toString()); return c.blue() - c.red(); };
        const QJsonObject blue = blueOf(series.at(0).toObject()) > blueOf(series.at(1).toObject()) ? series.at(0).toObject() : series.at(1).toObject();
        QCOMPARE_LE(worst(blue, sine), 0.015);
        QVERIFY(!picture(r).isNull());
        // The wrong number of values: said, with where the marks are.
        const QJsonObject wrong = call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"x_values", QJsonArray{0, 2}}});
        QVERIFY(failed(wrong));
        QVERIFY2(QucsControl::textOf(wrong).contains("marks"), qPrintable(QucsControl::textOf(wrong)));
    }

    void rangesNamesAndRegionsLeftOut()
    {
        // The values at the frame's edges: four numbers calibrate a scan.
        const QJsonObject r = call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"x_range", QJsonArray{0, 10}}, {"y_range", QJsonArray{-1, 1}}, {"picture", false},
                                                     {"colours", QJsonArray{"#ff0000", "#0000ff"}}, {"names", QJsonArray{"wave", "sine"}}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        const QJsonObject o = json(r);
        QVERIFY(o.value("calibration").toObject().value("valid").toBool());
        // Named by colour, not by the order the series came in.
        const QJsonArray series = o.value("series").toArray();
        const QJsonObject* sineSeries = nullptr;
        QJsonObject found;
        for (const QJsonValue& v : series)
            if (v.toObject().value("name").toString() == QStringLiteral("sine")) found = v.toObject();
        QVERIFY2(!found.isEmpty(), qPrintable(QJsonDocument(series).toJson()));
        Q_UNUSED(sineSeries)
        QCOMPARE_LE(worst(found, sine), 0.015);
        QVERIFY(QJsonDocument(o.value("notes").toArray()).toJson().contains("Named by the colours"));
        // Half of both both ranges, or ranges with references: refused.
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"x_range", QJsonArray{0, 10}}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"x_range", QJsonArray{0, 10}}, {"y_range", QJsonArray{-1, 1}},
                                              {"references", QJsonArray{QJsonObject{{"at", QJsonArray{1, 2}}, {"x", 0}, {"y", 0}}}}})));
        // A region left out: nothing from it comes back; the picture shows it.
        const QRectF out(px(3), kFrame.top(), px(5) - px(3), kFrame.height());
        QJsonArray regions;
        regions.append(QJsonArray{out.left(), out.top(), out.right(), out.bottom()});
        const QJsonObject left = json(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"x_range", QJsonArray{0, 10}}, {"y_range", QJsonArray{-1, 1}}, {"picture", false},
                                                             {"exclude", regions}}));
        for (const QJsonValue& v : left.value("series").toArray()) {
            const QJsonArray xs = v.toObject().value("x").toArray();
            for (const QJsonValue& x : xs) QVERIFY2(!(x.toDouble() > 3.01 && x.toDouble() < 4.99), qPrintable(QString::number(x.toDouble())));
        }
        QJsonArray bad;
        bad.append(QJsonArray{1, 2});
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"exclude", bad}})));
    }

    void shownIsNotKeptUntilChanged()
    {
        QVERIFY(!QFile::exists(sidecarPath(vectorFile)));
        const QJsonObject r = call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"show", true}, {"picture", false}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        QVERIFY(json(r).value("shown").toBool());
        QVERIFY(!QFile::exists(sidecarPath(vectorFile)));
        auto* doc = dynamic_cast<PdfDoc*>(app->getDoc());
        QVERIFY(doc != nullptr && doc->digitizerShown());
        Panel* panel = doc->digitizer();
        QCOMPARE(panel->plots().size(), 1);
        QVERIFY(panel->plots().first().transient);
        // Changed by the user: kept.
        panel->seriesTable()->item(0, 1)->setText(QStringLiteral("mine"));
        QVERIFY(!panel->plots().first().transient);
        QVERIFY(QFile::exists(sidecarPath(vectorFile)));
        QCOMPARE(json(call("get_digitized", {{"path", vectorFile}})).value("plots").toArray().size(), 1);
    }

    void referencesInstead()
    {
        const QJsonArray refs{QJsonObject{{"at", QJsonArray{kFrame.left(), kFrame.bottom()}}, {"x", 0}, {"y", -1}},
                              QJsonObject{{"at", QJsonArray{kFrame.right(), kFrame.bottom()}}, {"x", 10}, {"y", -1}},
                              QJsonObject{{"at", QJsonArray{kFrame.left(), kFrame.top()}}, {"x", 0}, {"y", 1}}};
        QJsonArray seeds;
        seeds.append(QJsonArray{px(1.5), py(sine(1.5))});
        const QJsonObject o = json(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"references", refs}, {"seeds", seeds}, {"picture", false}}));
        QVERIFY(o.value("calibration").toObject().value("valid").toBool());
        QCOMPARE(o.value("calibration").toObject().value("references").toInt(), 3);
        QCOMPARE(o.value("series").toArray().size(), 1);
        QCOMPARE_LE(worst(o.value("series").toArray().first().toObject(), sine), 0.015);
        // Bad ones are refused, and why.
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"references", QJsonArray{QJsonObject{{"at", QJsonArray{1, 2}}, {"x", 0}}}}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"references", QJsonArray{QJsonObject{{"x", 1}}}}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"references", QJsonArray{QJsonObject{{"at", QJsonArray{1, 2}}}}}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"polar", true}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"model", "projective"}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"model", "nonsense"}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"x_scale", "nonsense"}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"route", "nonsense"}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"colours", QJsonArray{"not a colour"}}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"seeds", QJsonArray{1}}})));
        QVERIFY(failed(call("digitize_plot", {{"path", scanFile}, {"page", 1}, {"trace_scale", 1000}})));
    }

    void writtenOutAndIntoTheProgramsResults()
    {
        const QString csvFile = path("numbers.csv"), dataFile = path("numbers.dat");
        const QJsonObject o = json(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"picture", false}, {"csv", csvFile}, {"dataset", dataFile}}));
        QVERIFY(o.contains("csv") && o.contains("dataset"));
        QVERIFY(o.value("dataset_variables").toArray().contains(QJsonValue(QStringLiteral("sine"))));
        QFile csvIn(csvFile);
        QVERIFY(csvIn.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(csvIn.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.first(), QStringLiteral("series,x,y,piece"));
        QCOMPARE(lines.size(), 1 + 301 + 301);
        // The program's own reader takes the dataset, and each curve is there.
        qucs_s::dataset::Dataset data;
        QString readWhy;
        QVERIFY2(data.read(dataFile, &readWhy), qPrintable(readWhy));
        QVERIFY2(data.notes().isEmpty(), qPrintable(data.notes().join(QLatin1Char(' '))));
        const auto* sineY = data.find(QStringLiteral("sine"));
        QVERIFY(sineY != nullptr);
        QCOMPARE(sineY->size(), 301);
        QCOMPARE(sineY->dependencies, QStringList{QStringLiteral("sine_x")});
        const auto* sineX = data.find(QStringLiteral("sine_x"));
        QVERIFY(sineX != nullptr);
        for (int i = 0; i < sineY->size(); i += 37) QCOMPARE_LE(std::abs(sineY->re.at(i) - sine(sineX->re.at(i))), 0.002);
        // Where it cannot be written: said.
        QVERIFY(failed(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"csv", path("no/such/folder/x.csv")}})));
    }

    void keptAndReadBack()
    {
        QVERIFY(!QFile::exists(sidecarPath(vectorFile)));
        // A look writes nothing.
        QVERIFY(!failed(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"picture", false}})));
        QVERIFY(!QFile::exists(sidecarPath(vectorFile)));
        QJsonObject nothing = json(call("get_digitized", {{"path", vectorFile}}));
        QCOMPARE(nothing.value("plots").toArray().size(), 0);
        QJsonObject o = json(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"picture", false}, {"keep", true}}));
        QCOMPARE(o.value("kept_as").toInt(), 1);
        QVERIFY(QFile::exists(sidecarPath(vectorFile)));
        QJsonObject got = json(call("get_digitized", {{"path", vectorFile}}));
        QCOMPARE(got.value("plots").toArray().size(), 1);
        const QJsonObject plot = got.value("plots").toArray().first().toObject();
        QVERIFY(plot.value("calibration").toObject().value("valid").toBool());
        QCOMPARE(plot.value("series").toArray().size(), 2);
        QCOMPARE_LE(worst(plot.value("series").toArray().first().toObject(), sine), 0.0005);
        // One of them, with its picture.
        const QJsonObject one = call("get_digitized", {{"path", vectorFile}, {"plot", 1}, {"picture", true}});
        QVERIFY(!picture(one).isNull());
        QVERIFY(failed(call("get_digitized", {{"path", vectorFile}, {"plot", 5}})));
        // Replaced.
        o = json(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"picture", false}, {"keep", true}, {"replace", 1}, {"names", QJsonArray{"first name"}}}));
        QCOMPARE(o.value("kept_as").toInt(), 1);
        got = json(call("get_digitized", {{"path", vectorFile}}));
        QCOMPARE(got.value("plots").toArray().size(), 1);
        QCOMPARE(got.value("plots").toArray().first().toObject().value("series").toArray().first().toObject().value("name").toString(), QStringLiteral("first name"));
        // What is damaged is not written over.
        {
            QFile f(sidecarPath(vectorFile));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{ damaged");
        }
        QVERIFY(failed(call("get_digitized", {{"path", vectorFile}})));
        QVERIFY(failed(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"keep", true}})));
        QFile f(sidecarPath(vectorFile));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("{ damaged"));
    }

    void showOpensThePanel()
    {
        const QJsonObject r = call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"show", true}, {"picture", false}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        QVERIFY(json(r).value("shown").toBool());
        auto* doc = dynamic_cast<PdfDoc*>(app->getDoc());
        QVERIFY(doc != nullptr);
        QVERIFY(doc->digitizerShown());
        QCOMPARE(doc->digitizer()->plots().size(), 1);
        QCOMPARE(doc->digitizer()->plots().first().series.size(), 2);
        QVERIFY(doc->digitizeButton()->isChecked());
    }

    void toolsAreListed()
    {
        // Each has what a tool must have, for describe_tool and the search.
        for (const char* name : {"find_plots", "digitize_plot", "get_digitized"}) {
            const QJsonObject d = call("describe_tool", {{"name", name}});
            QVERIFY2(!failed(d), name);
        }
    }

    // Over stdio, against the program itself, with no window.
    void overStdioWithNoWindow()
    {
#ifdef QUCS_BINARY
        const QString program = QStringLiteral(QUCS_BINARY);
        QVERIFY(QFile::exists(program));
        QTemporaryDir home;
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QUCS_CLAUDE", "/nonexistent/claude");
        env.insert("QUCS_NO_SHELL_ENV", "1");
        env.insert("QUCS_SETTINGS_DIR", home.filePath("settings"));
        env.insert("HOME", home.path());
        env.insert("QT_QPA_PLATFORM", "offscreen");
        p.setProcessEnvironment(env);
        p.start(program, {"--mcp-server"});
        QVERIFY(p.waitForStarted(10000));
        const auto ask = [&](int id, const QString& method, const QJsonObject& params) {
            p.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact) + '\n');
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 60000) {
                if (!p.canReadLine()) p.waitForReadyRead(100);
                while (p.canReadLine()) {
                    const QJsonObject a = QJsonDocument::fromJson(p.readLine()).object();
                    if (a.value("id").toInt() == id) return a;
                }
            }
            return QJsonObject{};
        };
        QVERIFY(!ask(1, "initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}).isEmpty());
        p.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).toJson(QJsonDocument::Compact) + '\n');
        const QJsonObject listed = ask(2, "tools/list", {});
        QStringList names;
        for (const QJsonValue& v : listed.value("result").toObject().value("tools").toArray()) names << v.toObject().value("name").toString();
        QVERIFY(names.contains("find_plots") && names.contains("digitize_plot") && names.contains("get_digitized"));
        const QJsonObject found = ask(3, "tools/call", {{"name", "find_plots"}, {"arguments", QJsonObject{{"path", vectorFile}}}});
        QVERIFY2(!found.value("result").toObject().value("isError").toBool(), qPrintable(QJsonDocument(found).toJson()));
        const QJsonObject answer = ask(4, "tools/call", {{"name", "digitize_plot"}, {"arguments", QJsonObject{{"path", vectorFile}, {"page", 1}}}});
        const QJsonObject result = answer.value("result").toObject();
        QVERIFY2(!result.value("isError").toBool(), qPrintable(QJsonDocument(answer).toJson()));
        const QJsonObject o = json(result);
        QCOMPARE(o.value("series").toArray().size(), 2);
        QCOMPARE_LE(worst(o.value("series").toArray().first().toObject(), sine), 0.0005);
        QVERIFY(!picture(result).isNull());
        p.closeWriteChannel();
        // And it ends as a program should: what was asked is not all.
        const bool finished = p.waitForFinished(15000);
        if (!finished) p.kill();
        QVERIFY(finished);
        const QByteArray errors = p.readAllStandardError();
        QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0,
                 qPrintable(QStringLiteral("exit %1: %2").arg(p.exitCode()).arg(QString::fromUtf8(errors.right(2000)))));
        QVERIFY2(!errors.contains("terminate called"), errors.right(2000).constData());
#else
        QSKIP("The program's path is not known to this test.");
#endif
    }

    // ----- The viewer's panel ----------------------------------------------

    void buttonOpensThePanel()
    {
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        QVERIFY(!doc->digitizerShown());
        doc->digitizeButton()->click();
        QVERIFY(doc->digitizerShown());
        Panel* panel = doc->digitizer();
        QVERIFY(panel->isVisible());
        QCOMPARE(panel->plots().size(), 0);
        doc->digitizeButton()->click();
        QVERIFY(!doc->digitizerShown());
        QVERIFY(doc->view()->tool() == nullptr);
    }

    void findAndExtractOnThePage()
    {
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        panel->findButton()->click();
        QCOMPARE(panel->plots().size(), 1);
        const Plot& plot = panel->plots().first();
        QVERIFY(plot.calibration.valid());
        QCOMPARE(plot.series.size(), 2);
        QCOMPARE(panel->seriesTable()->rowCount(), 2);
        QCOMPARE(panel->seriesTable()->item(0, 1)->text(), QStringLiteral("sine"));
        QCOMPARE(panel->pointsTable()->rowCount(), 301);
        // A name changed in the table.
        panel->seriesTable()->item(0, 1)->setText(QStringLiteral("renamed"));
        QCOMPARE(panel->plots().first().series.first().name, QStringLiteral("renamed"));
        // Found again: not twice.
        panel->findButton()->click();
        QCOMPARE(panel->plots().size(), 1);
        // On the clipboard.
        QVERIFY(panel->currentCsv().startsWith(QStringLiteral("series,x,y,piece\n")));
        // Written out.
        QVERIFY(panel->writeCsv(path("panel.csv")));
        QVERIFY(panel->writeDataset(path("panel.dat")));
        QVERIFY(QFile::exists(path("panel.csv")));
        QVERIFY(!panel->writeCsv(path("no/folder/panel.csv")));
    }

    void theOverlayShowsOnThePage()
    {
        // Red marks where there were none: the page's pixels differ.
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        const auto grabView = [&] {
            QCoreApplication::processEvents();
            QTest::qWait(60);
            doc->view()->viewport()->repaint();
            return doc->view()->viewport()->grab().toImage();
        };
        const QImage before = grabView();
        doc->digitizer()->findButton()->click();
        const QImage after = grabView();
        QCOMPARE(before.size(), after.size());
        int differing = 0;
        for (int y = 0; y < before.height(); y += 2)
            for (int x = 0; x < before.width(); x += 2)
                if (before.pixel(x, y) != after.pixel(x, y)) ++differing;
        QCOMPARE_GE(differing, 200);
        // Hidden, it is gone.
        doc->setDigitizerShown(false);
        const QImage hidden = grabView();
        int still = 0;
        for (int y = 0; y < before.height() && y < hidden.height(); y += 2)
            for (int x = 0; x < before.width() && x < hidden.width(); x += 2)
                if (hidden.size() == before.size() && before.pixel(x, y) != hidden.pixel(x, y)) ++still;
        QCOMPARE_LE(still, differing / 4);
    }

    void aRegionMarkedWithTheMouse()
    {
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        PageView* view = doc->view();
        panel->modeButton(Panel::Mode::Region)->click();
        QCOMPARE(panel->mode(), Panel::Mode::Region);
        drag(view, kFrame.topLeft() - QPointF(70, 20), kFrame.bottomRight() + QPointF(15, 40));
        QCOMPARE(panel->plots().size(), 1);
        QVERIFY2(panel->plots().first().calibration.valid(), qPrintable(panel->status()));
        QCOMPARE(panel->plots().first().series.size(), 2);
        // The mode ends with it.
        QCOMPARE(panel->mode(), Panel::Mode::None);
        // Too small to mean anything: nothing.
        panel->modeButton(Panel::Mode::Region)->click();
        drag(view, QPointF(50, 50), QPointF(52, 51));
        QCOMPARE(panel->plots().size(), 1);
    }

    void pointsByHandWithUndo()
    {
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        PageView* view = doc->view();
        panel->findButton()->click();
        QCOMPARE(panel->plots().size(), 1);
        const int seriesBefore = panel->plots().first().series.size();
        // A new series by hand, its points by clicks.
        panel->findChildren<QPushButton*>().size();
        const QList<QPushButton*> buttons = panel->findChildren<QPushButton*>();
        QPushButton* newSeries = nullptr;
        for (QPushButton* b : buttons)
            if (b->text() == Panel::tr("New by Hand")) newSeries = b;
        QVERIFY(newSeries != nullptr);
        newSeries->click();
        QCOMPARE(panel->plots().first().series.size(), seriesBefore + 1);
        QCOMPARE(panel->mode(), Panel::Mode::AddPoint);
        const QPointF a(px(2), py(0.4)), b(px(4), py(0.2)), c(px(6), py(-0.3));
        clickAt(view, a);
        clickAt(view, b);
        clickAt(view, c);
        const int hand = panel->currentSeries();
        QCOMPARE(panel->plots().first().series.at(hand).points.size(), 3);
        // Their values, by the calibration.
        const QPointF v = panel->plots().first().calibration.toValue(panel->plots().first().series.at(hand).points.at(1));
        QCOMPARE_LE(std::abs(v.x() - 4), 0.05);
        QCOMPARE_LE(std::abs(v.y() - 0.2), 0.02);
        // Moved by the mouse.
        panel->modeButton(Panel::Mode::MovePoint)->click();
        const QPointF moved = b + QPointF(30, -20);
        drag(view, b, moved);
        QCOMPARE_LE(QLineF(panel->plots().first().series.at(hand).points.at(1), moved).length(), 1.5);
        // Ctrl+Z through the window's own shortcuts: back where it was.
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QCOMPARE_LE(QLineF(panel->plots().first().series.at(hand).points.at(1), b).length(), 1.5);
        // Deleted by a click.
        panel->setCurrentSeries(hand);
        panel->modeButton(Panel::Mode::DeletePoint)->click();
        clickAt(view, b);
        QCOMPARE(panel->plots().first().series.at(hand).points.size(), 2);
        // Off every point: nothing.
        clickAt(view, QPointF(30, 30));
        QCOMPARE(panel->plots().first().series.at(hand).points.size(), 2);
        panel->undo();
        QCOMPARE(panel->plots().first().series.at(hand).points.size(), 3);
        // Escape ends the mode, and only then takes the mouse back.
        QCOMPARE(panel->mode(), Panel::Mode::DeletePoint);
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Escape);
        QCOMPARE(panel->mode(), Panel::Mode::None);
        QVERIFY(!view->tool()->takesMouse());
    }

    void aRegionLeftOutInThePanel()
    {
        PdfDoc* doc = open(scanFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        PageView* view = doc->view();
        panel->findButton()->click();
        QCOMPARE(panel->plots().size(), 1);
        panel->modeButton(Panel::Mode::LeaveOut)->click();
        QCOMPARE(panel->mode(), Panel::Mode::LeaveOut);
        const QRectF out(px(3), kFrame.top() + 2, px(5) - px(3), kFrame.height() - 4);
        drag(view, out.topLeft(), out.bottomRight());
        QCOMPARE(panel->plots().first().exclude.size(), 1);
        for (const Series& s : panel->plots().first().series)
            for (const QPointF& q : s.points) QVERIFY(!panel->plots().first().exclude.first().contains(q));
        // Kept with the rest, and undone.
        QList<Plot> kept;
        QVERIFY(loadSidecar(scanFile, &kept));
        QCOMPARE(kept.first().exclude.size(), 1);
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(panel->plots().first().exclude.size(), 0);
        // Cleared, and the panel's own finding of insets turned off with it.
        drag(view, out.topLeft(), out.bottomRight());
        QCOMPARE(panel->plots().first().exclude.size(), 1);
        panel->clearLeftOut();
        QCOMPARE(panel->plots().first().exclude.size(), 0);
        QVERIFY(!panel->findsLeftOut());
    }

    void referencesByHand()
    {
        // A scan with no labels: three clicks and three values.
        PdfDoc* doc = open(scanFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        PageView* view = doc->view();
        panel->findButton()->click();
        QCOMPARE(panel->plots().size(), 1);
        QVERIFY(!panel->plots().first().calibration.valid());
        // Its frame's corners are there already, waiting for their values: the
        // ends of its axes, typed into the table's cells.
        QCOMPARE(panel->plots().first().calibration.references.size(), 4);
        QVERIFY(!panel->plots().first().calibration.valid());
        QTableWidget* t = panel->referenceTable();
        t->item(0, 1)->setText("0");
        t->item(1, 1)->setText("10");
        t->item(2, 2)->setText("-1");
        t->item(3, 2)->setText("1");
        QVERIFY2(panel->plots().first().calibration.valid(), qPrintable(panel->status()));
        panel->undo();
        panel->undo();
        panel->undo();
        panel->undo();
        // Or by clicks: three points whose values are known.
        for (int k = 3; k >= 0; --k) {
            t->selectRow(k);
            const QList<QPushButton*> buttons = panel->findChildren<QPushButton*>();
            for (QPushButton* b : buttons)
                if (b->text() == Panel::tr("Remove") && b->parentWidget() == t->parentWidget()) b->click();
        }
        QCOMPARE(panel->plots().first().calibration.references.size(), 0);
        panel->modeButton(Panel::Mode::Reference)->click();
        QCOMPARE(panel->mode(), Panel::Mode::Reference);
        QCOMPARE(panel->plots().size(), 1);
        clickAt(view, QPointF(px(0), py(-1)));
        QVERIFY2(panel->plots().first().calibration.references.size() == 1, qPrintable(panel->status() + QStringLiteral(" mode %1 plots %2 page %3").arg(int(panel->mode())).arg(panel->plots().size()).arg(panel->plots().first().page)));
        clickAt(view, QPointF(px(10), py(-1)));
        clickAt(view, QPointF(px(0), py(1)));
        QCOMPARE(panel->plots().first().calibration.references.size(), 3);
        QVERIFY(!panel->plots().first().calibration.valid());
        t->item(0, 1)->setText("0");
        t->item(0, 2)->setText("-1");
        t->item(1, 1)->setText("10");
        t->item(1, 2)->setText("-1");
        t->item(2, 1)->setText("0");
        t->item(2, 2)->setText("1");
        QVERIFY2(panel->plots().first().calibration.valid(), qPrintable(panel->status()));
        QCOMPARE_LE(panel->plots().first().calibration.rms(), 1.0);
        // Now the numbers are in the axes' units.
        const Plot plot = panel->plots().first();
        const Series* blue = nullptr;
        for (const Series& s : plot.series)
            if (s.colour.blue() > 150 && s.colour.red() < 100) blue = &s;
        QVERIFY(blue != nullptr);
        for (const QPointF& v : values(plot, *blue)) QCOMPARE_LE(std::abs(v.y() - sine(v.x())), 0.1);
        // Text a number cannot be made of: no value there, and no calibration.
        t->item(2, 2)->setText("nonsense");
        QVERIFY(!panel->plots().first().calibration.valid());
        panel->undo();
        QVERIFY(panel->plots().first().calibration.valid());
        // Log scales chosen: a plain line of y through its log.
        panel->setScales(Scale::Log, Scale::Linear);
        QVERIFY(!panel->plots().first().calibration.valid());   // (x = 0 on a log axis)
        panel->undo();
        QVERIFY(panel->plots().first().calibration.valid());
    }

    void followALineFromAClick()
    {
        PdfDoc* doc = open(scanFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        panel->findButton()->click();
        const int before = panel->plots().first().series.size();
        panel->modeButton(Panel::Mode::Trace)->click();
        clickAt(doc->view(), QPointF(px(1.5), py(sine(1.5))));
        QCOMPARE(panel->plots().first().series.size(), before + 1);
        const Series& s = panel->plots().first().series.last();
        QCOMPARE(s.origin, Series::Origin::Raster);
        QCOMPARE_GE(s.points.size(), 80);
        // Nowhere near a line: said, nothing added.
        clickAt(doc->view(), kFrame.topLeft() + QPointF(5, 200));
        QCOMPARE_LE(panel->plots().first().series.size(), before + 2);
    }

    void pickAColour()
    {
        PdfDoc* doc = open(scanFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        panel->findButton()->click();
        const int before = panel->plots().first().series.size();
        panel->modeButton(Panel::Mode::PickColour)->click();
        clickAt(doc->view(), QPointF(px(1.5), py(sine(1.5))));
        QVERIFY2(panel->plots().first().series.size() >= before + 1, qPrintable(panel->status()));
        const Series& s = panel->plots().first().series.last();
        QVERIFY2(s.colour.blue() > s.colour.red(), qPrintable(s.colour.name()));
    }

    void theMagnifierDrawsByThePointer()
    {
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        doc->setDigitizerShown(true);
        Panel* panel = doc->digitizer();
        PageView* view = doc->view();
        panel->findButton()->click();
        panel->modeButton(Panel::Mode::AddPoint)->click();
        QVERIFY(panel->magnifierOn());
        QTest::mouseMove(view->viewport(), at(view, QPointF(px(2), py(0.5))));
        QCoreApplication::processEvents();
        view->viewport()->repaint();
        const QImage with = view->viewport()->grab().toImage();
        panel->magnifierBox()->setChecked(false);
        view->viewport()->repaint();
        const QImage without = view->viewport()->grab().toImage();
        int differing = 0;
        for (int y = 0; y < with.height(); y += 2)
            for (int x = 0; x < with.width(); x += 2)
                if (with.pixel(x, y) != without.pixel(x, y)) ++differing;
        QCOMPARE_GE(differing, 200);
    }

    void restoredOnReopening()
    {
        {
            PdfDoc* doc = open(vectorFile);
            QVERIFY(doc != nullptr);
            doc->setDigitizerShown(true);
            doc->digitizer()->findButton()->click();
            QCOMPARE(doc->digitizer()->plots().size(), 1);
            closeAll();
        }
        QVERIFY(QFile::exists(sidecarPath(vectorFile)));
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        // It comes back with the file, the panel open on it.
        QVERIFY(doc->digitizerShown());
        Panel* panel = doc->digitizer();
        QCOMPARE(panel->plots().size(), 1);
        QVERIFY(panel->plots().first().calibration.valid());
        QCOMPARE(panel->plots().first().series.size(), 2);
        // Claude sees what the user did.
        QCOMPARE(json(call("get_digitized", {{"path", vectorFile}})).value("plots").toArray().size(), 1);
        // What Claude keeps shows in the open tab.
        QVERIFY(!failed(call("digitize_plot", {{"path", vectorFile}, {"page", 1}, {"picture", false}, {"keep", true}})));
        QCOMPARE(panel->plots().size(), 2);
        // The file read again keeps them.
        QVERIFY(doc->reload());
        QCOMPARE(panel->plots().size(), 2);
    }

    // Pictures of the window for the write-up, when asked for (where to put
    // them in QUCS_DIGITIZER_SHOTS): the panel on a vector plot with the
    // magnifier by the pointer, and on a scan traced from its picture.
    void picturesForTheWriteUp()
    {
        const QString shots = qEnvironmentVariable("QUCS_DIGITIZER_SHOTS");
        if (shots.isEmpty()) QSKIP("QUCS_DIGITIZER_SHOTS names no folder.");
        QDir().mkpath(shots);
        for (const QString& file : {vectorFile, scanFile}) {
            PdfDoc* doc = open(file);
            QVERIFY(doc != nullptr);
            doc->setDigitizerShown(true);
            Panel* panel = doc->digitizer();
            panel->findButton()->click();
            if (file == scanFile) {
                panel->modeButton(Panel::Mode::Reference)->click();
                clickAt(doc->view(), QPointF(px(0), py(-1)));
                clickAt(doc->view(), QPointF(px(10), py(-1)));
                clickAt(doc->view(), QPointF(px(0), py(1)));
                QTableWidget* t = panel->referenceTable();
                t->item(0, 1)->setText("0");
                t->item(0, 2)->setText("-1");
                t->item(1, 1)->setText("10");
                t->item(1, 2)->setText("-1");
                t->item(2, 1)->setText("0");
                t->item(2, 2)->setText("1");
            }
            doc->view()->showRegion(0, kFrame.adjusted(-80, -30, 30, 50));
            panel->setCurrentSeries(0);
            panel->modeButton(Panel::Mode::AddPoint)->click();
            QTest::mouseMove(doc->view()->viewport(), at(doc->view(), QPointF(px(3.2), py(sine(3.2)))));
            // (The pages' tiles come over several paints: all of them first.)
            for (int i = 0; i < 20; ++i) {
                QTest::qWait(50);
                doc->view()->viewport()->repaint();
            }
            app->repaint();
            QVERIFY(app->grab().toImage().save(QDir(shots).filePath(file == vectorFile ? "panel-vector.png" : "panel-scan.png")));
            panel->setMode(Panel::Mode::None);
            closeAll();
            QFile::remove(sidecarPath(file));
        }
    }

    void aDamagedKeptFileIsNotOverwritten()
    {
        {
            QFile f(sidecarPath(vectorFile));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[1, 2");
        }
        PdfDoc* doc = open(vectorFile);
        QVERIFY(doc != nullptr);
        QVERIFY(doc->digitizerShown());
        Panel* panel = doc->digitizer();
        QVERIFY2(panel->status().contains(QStringLiteral("left")), qPrintable(panel->status()));
        panel->findButton()->click();
        QCOMPARE(panel->plots().size(), 1);
        QFile f(sidecarPath(vectorFile));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("[1, 2"));
    }
};

int main(int argc, char** argv)
{
    int failed = 0;
    {
        QApplication app(argc, argv);
        TestPlotDigitizerApp test;
        failed = QTest::qExec(&test, argc, argv);
        // The tools kept PDFs read (digitizeInOneCall and the calls after it;
        // not when only functions without them were asked for).
        if (QucsControl::digitizerPdfsHeld() == 0) std::fprintf(stderr, "Note: the tools kept no PDF read; the check after the application went checks nothing\n");
    }
    // The application gone: so are they, not left to the program's end, where
    // QtPdf's own lock went first (on Linux, Qt 6.10: an abort after the last
    // test, docs/bug_hunts/2026-11-08-linux-debug-ci.md).
    if (QucsControl::digitizerPdfsHeld() != 0) {
        std::fprintf(stderr, "FAIL: %d PDFs still held by the digitizer's tools after the application went\n", QucsControl::digitizerPdfsHeld());
        failed = 1;
    }
    return failed;
}
#include "test_plot_digitizer_app.moc"
