/*
 * File > Export (graphicsexport.h, exportdevices.h, the export dialog):
 * every format written without another program - PNG, JPEG, BMP, TIFF,
 * WebP, SVG, PDF, EPS, PDF + LaTeX - at the size of the drawing, in
 * colour, grey or black and white, on paper or transparent, text as text,
 * as outlines or set by LaTeX; the clipboard; the dialog.
 *
 * Where Ghostscript, pdftoppm or pdflatex are on the machine, the EPS, the
 * PDF and the LaTeX overlay are rendered by them too.
 */
#include <QtTest>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTimer>
#include <QXmlStreamReader>

#include <memory>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "graphicsexport.h"
#include "exportdevices.h"
#include "components/component.h"
#include "dialogs/exportdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s;
using namespace qucs_s::graphicsexport;
using exportdevices::Role;

namespace {

// A resistor, a labelled wire, ground, a hatched rectangle with a red
// border, a dashed ellipse filled yellow, a green text with characters
// LaTeX takes as commands, and a text turned upwards.
QString fixture()
{
    return "<Qucs Schematic " PACKAGE_VERSION ">\n"
           "<Properties>\n</Properties>\n"
           "<Symbol>\n</Symbol>\n"
           "<Components>\n"
           "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
           "  <GND * 1 300 200 0 0 0 0>\n"
           "</Components>\n"
           "<Wires>\n"
           "  <100 100 220 100 \"in\" 120 70 0 \"\">\n"
           "  <280 100 300 100 \"\" 0 0 0 \"\">\n"
           "  <300 100 300 200 \"\" 0 0 0 \"\">\n"
           "</Wires>\n"
           "<Diagrams>\n</Diagrams>\n"
           "<Paintings>\n"
           "  <Rectangle 400 60 120 80 #ff0000 3 1 #00c000 12 1>\n"
           "  <Ellipse 560 60 80 80 #0000ff 3 2 #ffff00 1 1>\n"
           "  <Text 100 260 14 #008000 0 \"Gain & loss: 50% #1\">\n"
           "  <Text 700 100 12 #000000 90 \"Rotated\">\n"
           "</Paintings>\n";
}

// The box of what is drawn: pixels neither white nor transparent.
QRect inkBox(const QImage& image)
{
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    int left = img.width(), right = -1, top = img.height(), bottom = -1;
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = line[x];
            if (qAlpha(p) > 64 && (qRed(p) < 200 || qGreen(p) < 200 || qBlue(p) < 200)) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    return right < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

bool near(const QRect& a, const QRect& b, int tolerance)
{
    return std::abs(a.left() - b.left()) <= tolerance && std::abs(a.top() - b.top()) <= tolerance
        && std::abs(a.right() - b.right()) <= tolerance && std::abs(a.bottom() - b.bottom()) <= tolerance;
}

QString describe(const QRect& r)
{
    return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Runs a program, its output in \a log; false when it is not there or fails.
bool run(const QString& program, const QStringList& args, const QString& dir, QString* log = nullptr)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForFinished(120000)) {
        p.kill();
        return false;
    }
    if (log)
        *log = QString::fromLocal8Bit(p.readAll());
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

} // namespace

class TestGraphicsExport : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    Schematic* doc = nullptr;

    QString path(const QString& name) const { return dir.filePath(name); }

    Component* resistor() const
    {
        for (Component* c : *doc->a_Components)
            if (c->Name == QLatin1String("R1"))
                return c;
        return nullptr;
    }

    void deselectAll()
    {
        for (Component* c : *doc->a_Components) c->isSelected = false;
        for (auto* w : *doc->a_Wires) w->isSelected = false;
        for (auto* p : *doc->a_Paintings) p->isSelected = false;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.QucsWorkDir.setPath(dir.path());
        QucsSettings.font = QApplication::font();
        Module::registerModules();

        QFile file(path("drawing.sch"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(fixture().toUtf8());
        file.close();
        doc = new Schematic(nullptr, path("drawing.sch"));
        QVERIFY(doc->load());
        QVERIFY(resistor() != nullptr);
    }

    void cleanupTestCase() { delete doc; }

    void init() { deselectAll(); }

    // ---- formats -----------------------------------------------------

    // A clip set between save() and restore() ends with the restore: what
    // is painted afterwards, outside it, reaches the target. (A histogram
    // clips its bars to the plot; the axis labels after them went missing.)
    void aClipEndsWithItsRestore()
    {
        using namespace qucs_s::exportdevices;
        for (const bool outerClip : {false, true}) {
            QImage image(200, 100, QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter target(&image);
            {
                RelayDevice device(&target, TextMode::Text, Colours::Colour);
                QPainter p(&device);
                if (outerClip) p.setClipRect(0, 0, 190, 100);
                p.translate(0, 100);
                p.scale(1, -1);   // as a diagram paints its graphs
                p.save();
                p.setClipRect(QRectF(0, 0, 50, 50));
                p.fillRect(QRectF(0, 0, 200, 100), Qt::red);
                p.restore();
                p.fillRect(QRectF(100, 0, 50, 50), Qt::blue);
                p.resetTransform();
                p.fillRect(QRectF(160, 0, 20, 20), Qt::green);
            }
            target.end();
            const QString why = outerClip ? QStringLiteral("with an outer clip") : QStringLiteral("without");
            QVERIFY2(image.pixelColor(25, 75) == QColor(Qt::red), qPrintable(why));     // inside the clip
            QVERIFY2(image.pixelColor(47, 53) == QColor(Qt::red), qPrintable(why));     // all of it
            QVERIFY2(image.pixelColor(53, 75) == QColor(Qt::white), qPrintable(why));   // and no more
            QVERIFY2(image.pixelColor(75, 25) == QColor(Qt::white), qPrintable(why));   // clipped away
            QVERIFY2(image.pixelColor(125, 75) == QColor(Qt::blue), qPrintable(why));   // after the restore
            QVERIFY2(image.pixelColor(170, 10) == QColor(Qt::green), qPrintable(why));
        }
    }

    void theFormatsAndTheirSuffixes()
    {
        const QList<Format> all = formats();
        for (const Format f : {Format::Png, Format::Jpeg, Format::Bmp, Format::Svg, Format::Pdf,
                               Format::Eps, Format::PdfTex})
            QVERIFY2(all.contains(f), qPrintable(description(f)));

        QCOMPARE(formatOf("a.JPEG"), std::optional<Format>(Format::Jpeg));
        QCOMPARE(formatOf("a.jpg"), std::optional<Format>(Format::Jpeg));
        QCOMPARE(formatOf("x/fig.pdf_tex"), std::optional<Format>(Format::PdfTex));
        QCOMPARE(formatOf("a.tiff"), std::optional<Format>(Format::Tiff));
        QVERIFY(!formatOf("a.txt").has_value());
        QVERIFY(!formatOf("noext").has_value());

        QCOMPARE(withSuffix("a.png", Format::Svg), QString("a.svg"));
        QCOMPARE(withSuffix("a.PNG", Format::Png), QString("a.PNG"));
        QCOMPARE(withSuffix("a", Format::Pdf), QString("a.pdf"));
        QCOMPARE(withSuffix("a.txt", Format::Png), QString("a.txt.png"));
        QCOMPARE(withSuffix("dir.v2/fig", Format::Eps), QString("dir.v2/fig.eps"));
        QCOMPARE(withSuffix("fig.pdf", Format::PdfTex), QString("fig.pdf_tex"));
        QCOMPARE(pdfOf("x/fig.pdf_tex"), QString("x/fig.pdf"));

        QVERIFY(nameFilters().contains("(*.jpg *.jpeg)"));
        QVERIFY(isVector(Format::Eps) && !isVector(Format::Tiff));
        QVERIFY(!hasTransparency(Format::Jpeg) && hasTransparency(Format::Png) && hasTransparency(Format::Svg));
        QVERIFY(hasQuality(Format::Jpeg) && !hasQuality(Format::Png));
    }

    // ---- the area ----------------------------------------------------

    void theAreaIsTheDrawingWithItsMargin()
    {
        const QRect all = area(doc, false);
        QVERIFY(!all.isEmpty());
        QCOMPARE(all, doc->allBoundingRect().marginsAdded(QMargins(Margin, Margin, Margin, Margin)));
        QVERIFY(area(doc, true).isEmpty());   // nothing selected

        resistor()->isSelected = true;
        const QRect selected = area(doc, true);
        QVERIFY(!selected.isEmpty());
        QVERIFY(selected.width() < all.width());
        QVERIFY(selected.contains(resistor()->boundingRect()));
    }

    void nothingToExport()
    {
        QString error;
        Options options;
        options.selectionOnly = true;
        QVERIFY(!write(doc, path("nothing.png"), Format::Png, options, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(path("nothing.png")));

        Schematic empty(nullptr, QString());
        QVERIFY(!write(&empty, path("empty.png"), Format::Png, Options(), &error));
        QVERIFY(!QFile::exists(path("empty.png")));
    }

    // ---- raster images -----------------------------------------------

    void everyRasterFormatIsWritten()
    {
        const QSize expected = area(doc, false).size();
        for (const Format f : formats()) {
            if (isVector(f))
                continue;
            const QString file = path("raster." + suffix(f));
            QString error;
            QVERIFY2(write(doc, file, f, Options(), &error), qPrintable(error));
            QImageReader reader(file);
            const QImage image = reader.read();
            QVERIFY2(!image.isNull(), qPrintable(file + ": " + reader.errorString()));
            QCOMPARE(image.size(), expected);
            QVERIFY2(!inkBox(image).isEmpty(), qPrintable(file));
        }
    }

    void theScaleSetsThePixelsAndTheResolution()
    {
        Options options;
        options.scale = 2.5;
        QVERIFY(write(doc, path("scaled.png"), Format::Png, options));
        const QImage image(path("scaled.png"));
        const QSize base = area(doc, false).size();
        QCOMPARE(image.width(), int(std::lround(base.width() * 2.5)));
        QCOMPARE(image.height(), int(std::lround(base.height() * 2.5)));
        QCOMPARE(qRound(image.dotsPerMeterX() * 0.0254), 240);   // 96 dpi x 2.5
        QCOMPARE(pixelSize(doc, options), image.size());
    }

    void thePaperOrNone()
    {
        Options options;
        QVERIFY(write(doc, path("paper.png"), Format::Png, options));
        QImage image(path("paper.png"));
        QCOMPARE(QColor(image.pixel(0, 0)), QColor(Qt::white));
        QVERIFY(!image.hasAlphaChannel());

        options.transparent = true;
        QVERIFY(write(doc, path("clear.png"), Format::Png, options));
        image = QImage(path("clear.png"));
        QVERIFY(image.hasAlphaChannel());
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
        QVERIFY(!inkBox(image).isEmpty());

        // A JPEG has no transparency: white paper whatever is asked.
        QVERIFY(write(doc, path("clear.jpg"), Format::Jpeg, options));
        image = QImage(path("clear.jpg"));
        QVERIFY(QColor(image.pixel(0, 0)).lightness() > 245);
    }

    void greyAndBlackAndWhite()
    {
        const QImage colour = image(doc, Options());
        bool red = false;
        for (int y = 0; y < colour.height() && !red; ++y)
            for (int x = 0; x < colour.width() && !red; ++x) {
                const QRgb p = colour.pixel(x, y);
                red = qRed(p) > 200 && qGreen(p) < 60 && qBlue(p) < 60;
            }
        QVERIFY(red);   // the rectangle's border

        Options options;
        options.colours = Colours::Grayscale;
        QVERIFY(write(doc, path("grey.png"), Format::Png, options));
        QImage grey = QImage(path("grey.png")).convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < grey.height(); ++y)
            for (int x = 0; x < grey.width(); ++x) {
                const QRgb p = grey.pixel(x, y);
                QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p));
            }

        options.colours = Colours::Monochrome;
        QVERIFY(write(doc, path("mono.png"), Format::Png, options));
        const QImage mono(path("mono.png"));
        QCOMPARE(mono.depth(), 1);
        QVERIFY(!inkBox(mono).isEmpty());
        // The yellow ellipse: a light fill, white; its blue border, black.
        const QImage ink = image(doc, options);
        for (int y = 0; y < ink.height(); ++y)
            for (int x = 0; x < ink.width(); ++x) {
                const QRgb p = ink.pixel(x, y);
                QVERIFY(qRed(p) == qGreen(p) && qGreen(p) == qBlue(p) && (qRed(p) == 0 || qRed(p) == 255));
            }
    }

    void theColoursOfBlackAndWhite()
    {
        using exportdevices::exported;
        const auto level = [](const QColor& c) { return c.red(); };
        // Lines and text: black unless white; fills: black when dark.
        QCOMPARE(level(exported(QColor(0, 255, 255), Colours::Monochrome, Role::Line)), 0);
        QCOMPARE(level(exported(QColor(Qt::lightGray), Colours::Monochrome, Role::Line)), 0);
        QCOMPARE(level(exported(QColor(Qt::white), Colours::Monochrome, Role::Line)), 255);
        QCOMPARE(level(exported(QColor(0, 255, 255), Colours::Monochrome, Role::Fill)), 255);
        QCOMPARE(level(exported(QColor(0, 0, 128), Colours::Monochrome, Role::Fill)), 0);
        QCOMPARE(exported(QColor(10, 20, 30, 77), Colours::Grayscale, Role::Fill).alpha(), 77);
        QCOMPARE(exported(QColor(10, 20, 30), Colours::Colour, Role::Fill), QColor(10, 20, 30));
    }

    // ---- SVG ---------------------------------------------------------

    void theSvgHasTheSizeOfTheDrawing()
    {
        const QSize size = area(doc, false).size();
        Options options;
        options.textAsOutlines = true;
        QVERIFY(write(doc, path("outlines.svg"), Format::Svg, options));
        const QByteArray svg = readAll(path("outlines.svg"));

        QXmlStreamReader xml(svg);
        QVERIFY(xml.readNextStartElement());
        QCOMPARE(xml.name().toString(), QString("svg"));
        QCOMPARE(xml.attributes().value("viewBox").toString(),
                 QStringLiteral("0 0 %1 %2").arg(size.width()).arg(size.height()));
        const double mm = xml.attributes().value("width").toString().remove("mm").toDouble();
        QVERIFY(std::abs(mm - size.width() * 25.4 / 96.0) < 0.01);
        QVERIFY2(!svg.contains("<text"), "text as outlines");
        QVERIFY(svg.contains("<title>drawing</title>"));

        // Drawn where the image has it.
        QSvgRenderer renderer(svg);
        QVERIFY(renderer.isValid());
        QImage rendered(size, QImage::Format_ARGB32);
        rendered.fill(Qt::white);
        {
            QPainter p(&rendered);
            renderer.render(&p);
        }
        const QRect expected = inkBox(image(doc, Options()));
        QVERIFY2(near(inkBox(rendered), expected, 3),
                 qPrintable(describe(inkBox(rendered)) + " vs " + describe(expected)));
    }

    void anSvgWithTextNamesAFontOthersHave()
    {
        Options options;
        options.textAsOutlines = false;
        const QByteArray svg = graphicsexport::svg(doc, options);
        QVERIFY(svg.contains("<text"));
        QVERIFY(svg.contains("Gain &amp; loss: 50% #1"));
        const QRegularExpression family("font-family=\"([^\"]*)\"");
        auto it = family.globalMatch(QString::fromUtf8(svg));
        QVERIFY(it.hasNext());
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            QVERIFY2(!name.startsWith('.'), qPrintable(name));
        }
    }

    // ---- PDF ---------------------------------------------------------

    void thePdfPageIsTheDrawing()
    {
        const QSize size = area(doc, false).size();
        QVERIFY(write(doc, path("drawing.pdf"), Format::Pdf, Options()));
        const QByteArray pdf = readAll(path("drawing.pdf"));
        QVERIFY(pdf.startsWith("%PDF-"));
        const QRegularExpression box("/MediaBox \\[0 0 ([0-9.]+) ([0-9.]+)\\]");
        const auto m = box.match(QString::fromLatin1(pdf));
        QVERIFY(m.hasMatch());
        QVERIFY(std::abs(m.captured(1).toDouble() - size.width() * 0.75) < 1.0);
        QVERIFY(std::abs(m.captured(2).toDouble() - size.height() * 0.75) < 1.0);
        QVERIFY2(pdf.contains("/FontDescriptor"), "text as text: fonts embedded");

        Options outlines;
        outlines.textAsOutlines = true;
        QVERIFY(!graphicsexport::pdf(doc, outlines).contains("/FontDescriptor"));

        const QString pdftoppm = QStandardPaths::findExecutable("pdftoppm");
        if (pdftoppm.isEmpty())
            return;
        QString log;
        QVERIFY2(run(pdftoppm, {"-r", "96", "-png", "-singlefile", "drawing.pdf", "pdfpage"}, dir.path(), &log),
                 qPrintable(log));
        const QImage page(path("pdfpage.png"));
        QVERIFY(!page.isNull());
        const QRect expected = inkBox(image(doc, Options()));
        QVERIFY2(near(inkBox(page), expected, 3), qPrintable(describe(inkBox(page)) + " vs " + describe(expected)));
    }

    // ---- EPS ---------------------------------------------------------

    void theEpsIsWellFormed()
    {
        const QSize size = area(doc, false).size();
        QVERIFY(write(doc, path("drawing.eps"), Format::Eps, Options()));
        const QByteArray eps = readAll(path("drawing.eps"));
        QVERIFY(eps.startsWith("%!PS-Adobe-3.0 EPSF-3.0\n"));
        QVERIFY(eps.contains(QStringLiteral("%%BoundingBox: 0 0 %1 %2\n")
                                 .arg(qCeil(size.width() * 0.75)).arg(qCeil(size.height() * 0.75)).toLatin1()));
        QVERIFY(eps.contains("%%Title: drawing\n"));
        QVERIFY(eps.contains(" translate 0.75 -0.75 scale\n"));
        QVERIFY(eps.endsWith("showpage\n%%Trailer\n%%EOF\n"));
        QCOMPARE(eps.count("gsave"), eps.count("grestore"));
        QVERIFY2(eps.contains("] 0 setdash"), "the dashed ellipse");
        QVERIFY2(eps.contains("eoclip newpath") || eps.contains("clip newpath"), "the hatched rectangle");
        QVERIFY2(eps.contains("1 0 0 rg"), "the red border");
        // Printable ASCII and newlines only (DocumentData: Clean7Bit).
        for (const char c : eps)
            QVERIFY(c == '\n' || (c >= 0x20 && c < 0x7f));

        const QString gs = QStandardPaths::findExecutable("gs");
        if (gs.isEmpty())
            return;
        QString log;
        QVERIFY2(run(gs, {"-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-dEPSCrop", "-sDEVICE=png16m", "-r72",
                          "-dGraphicsAlphaBits=4", "-dTextAlphaBits=4", "-sOutputFile=epspage.png", "drawing.eps"},
                     dir.path(), &log),
                 qPrintable(log));
        // At 72 dpi a unit of the schematic (1/96 inch) is 0.75 pixels.
        const QImage page(path("epspage.png"));
        QVERIFY(!page.isNull());
        const QRect ink = inkBox(page);
        const QRect expected = inkBox(image(doc, Options()));
        const QRect scaled(QPoint(qRound(expected.left() * 0.75), qRound(expected.top() * 0.75)),
                           QPoint(qRound(expected.right() * 0.75), qRound(expected.bottom() * 0.75)));
        QVERIFY2(near(ink, scaled, 3), qPrintable(describe(ink) + " vs " + describe(scaled)));
    }

    void theEpsEngineOnItsOwn()
    {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        {
            exportdevices::EpsDevice device(&buffer, QSize(200, 100), QStringLiteral("t(1)"));
            QPainter p(&device);
            p.fillRect(QRect(10, 10, 20, 20), QColor(255, 0, 0, 128));   // on white: pink
            p.setClipRect(QRect(50, 0, 50, 100));
            p.setPen(QPen(Qt::black, 2));
            p.drawLine(0, 50, 200, 50);
            p.setClipping(false);
            QImage tiny(2, 1, QImage::Format_RGB32);
            tiny.setPixel(0, 0, qRgb(0, 0, 255));
            tiny.setPixel(1, 0, qRgb(0, 255, 0));
            p.drawImage(QRect(150, 10, 20, 10), tiny);
        }
        const QByteArray eps = buffer.data();
        QVERIFY(eps.contains("%%BoundingBox: 0 0 150 75\n"));
        QVERIFY(eps.contains("1 0.498 0.498 rg"));
        QVERIFY(eps.contains("clip newpath"));
        QVERIFY(eps.contains("/ASCIIHexDecode filter"));
        QVERIFY(eps.contains("0000ff00ff00"));
        QCOMPARE(eps.count("gsave"), eps.count("grestore"));
    }

    // ---- PDF + LaTeX -------------------------------------------------

    void pdfWithLatexText()
    {
        QString error;
        QVERIFY2(write(doc, path("fig.pdf_tex"), Format::PdfTex, Options(), &error), qPrintable(error));
        QVERIFY(QFile::exists(path("fig.pdf")));
        QVERIFY2(!readAll(path("fig.pdf")).contains("/FontDescriptor"), "the text is LaTeX's");
        const QString tex = QString::fromUtf8(readAll(path("fig.pdf_tex")));
        QVERIFY(tex.contains("\\includegraphics[width=\\unitlength,page=1]{fig.pdf}"));
        QVERIFY2(tex.contains("Gain \\& loss: 50\\% \\#1"), qPrintable(tex));
        QVERIFY(tex.contains("\\color[rgb]{0,0.501961,0}"));
        QVERIFY(tex.contains("\\rotatebox{90}"));
        QVERIFY(tex.contains("{}R1}"));
        QVERIFY(tex.contains("\\qucssize{"));

        const QString pdflatex = QStandardPaths::findExecutable("pdflatex");
        if (pdflatex.isEmpty())
            return;
        QFile main(path("main.tex"));
        QVERIFY(main.open(QIODevice::WriteOnly));
        main.write("\\documentclass{article}\n\\usepackage{graphicx}\n\\usepackage{xcolor}\n"
                   "\\begin{document}\n\\def\\svgwidth{\\textwidth}\n\\input{fig.pdf_tex}\n"
                   "\\def\\qucsdocumentfont{}\\input{fig.pdf_tex}\n\\end{document}\n");
        main.close();
        QString log;
        QVERIFY2(run(pdflatex, {"-interaction=nonstopmode", "-halt-on-error", "main.tex"}, dir.path(), &log),
                 qPrintable(log.right(2000)));
    }

    // ---- text --------------------------------------------------------

    void theTextKeptForLatexIsWhereItIsDrawn()
    {
        QImage target(200, 200, QImage::Format_ARGB32);
        target.fill(Qt::white);
        QPainter tp(&target);
        exportdevices::RelayDevice relay(&tp, exportdevices::TextMode::Collect, Colours::Colour);
        {
            QPainter p(&relay);
            p.setPen(Qt::blue);
            p.drawText(QPointF(10, 20), QStringLiteral("flat"));
            p.translate(100, 150);
            p.rotate(-90);
            p.drawText(QPointF(0, 0), QStringLiteral("up"));
        }
        tp.end();
        QCOMPARE(inkBox(target), QRect());   // nothing drawn
        const QList<exportdevices::TextRun> texts = relay.texts();
        QCOMPARE(texts.size(), 2);
        QCOMPARE(texts[0].text, QString("flat"));
        QCOMPARE(texts[0].position, QPointF(10, 20));
        QVERIFY(std::abs(texts[0].angle) < 1e-9);
        QCOMPARE(texts[0].colour, QColor(Qt::blue));
        QCOMPARE(texts[1].text, QString("up"));
        QVERIFY(std::abs(texts[1].position.x() - 100) < 1e-9 && std::abs(texts[1].position.y() - 150) < 1e-9);
        QVERIFY(std::abs(texts[1].angle - 90) < 1e-9);
    }

    void textAsOutlinesLooksLikeText()
    {
        const auto draw = [](exportdevices::TextMode mode) {
            QImage target(300, 60, QImage::Format_ARGB32);
            target.fill(Qt::white);
            QPainter tp(&target);
            exportdevices::RelayDevice relay(&tp, mode, Colours::Colour);
            {
                QPainter p(&relay);
                QFont font = p.font();
                font.setPixelSize(24);
                p.setFont(font);
                p.drawText(QPointF(10, 40), QStringLiteral("Outlines 123"));
            }
            tp.end();
            return target;
        };
        const QRect text = inkBox(draw(exportdevices::TextMode::Text));
        const QRect outlines = inkBox(draw(exportdevices::TextMode::Outlines));
        QVERIFY(!text.isEmpty());
        QVERIFY2(near(text, outlines, 2), qPrintable(describe(text) + " vs " + describe(outlines)));
    }

    // ---- clipboard ---------------------------------------------------

    void theClipboardGetsAnImageAnSvgAndAPdf()
    {
        Options options;
        options.scale = 2.0;
        std::unique_ptr<QMimeData> data(mimeData(doc, options));
        QVERIFY(data->hasImage());
        const QImage picture = qvariant_cast<QImage>(data->imageData());
        QCOMPARE(picture.size(), pixelSize(doc, options));
        QVERIFY(data->data("image/svg+xml").startsWith("<?xml"));
        QVERIFY(!data->data("image/svg+xml").contains("<text"));
        QVERIFY(data->data("application/pdf").startsWith("%PDF-"));
    }

    // ---- the dialog --------------------------------------------------

    void theDialogFollowsTheFormat()
    {
        const QString file = path("out/fig.png");
        ExportDialog dialog(QSize(400, 300), QSize(), file);
        QCOMPARE(dialog.format(), Format::Png);
        QVERIFY(!dialog.selectionBox()->isEnabled());   // nothing selected
        QVERIFY(dialog.scaleBox()->isVisibleTo(&dialog));
        QVERIFY(!dialog.qualityBox()->isVisibleTo(&dialog));
        QVERIFY(!dialog.outlinesBox()->isVisibleTo(&dialog));

        dialog.formatBox()->setCurrentIndex(dialog.formatBox()->findData(int(Format::Svg)));
        QCOMPARE(dialog.fileEdit()->text(), path("out/fig.svg"));
        QVERIFY(!dialog.scaleBox()->isVisibleTo(&dialog));
        QVERIFY(dialog.outlinesBox()->isVisibleTo(&dialog));
        QVERIFY(dialog.outlinesBox()->isEnabled());
        QVERIFY(dialog.options().textAsOutlines);   // the default for SVG

        dialog.formatBox()->setCurrentIndex(dialog.formatBox()->findData(int(Format::Eps)));
        QVERIFY(dialog.outlinesBox()->isChecked() && !dialog.outlinesBox()->isEnabled());
        QVERIFY(dialog.options().textAsOutlines);

        dialog.formatBox()->setCurrentIndex(dialog.formatBox()->findData(int(Format::PdfTex)));
        QCOMPARE(dialog.fileName(), path("out/fig.pdf_tex"));
        QVERIFY(dialog.noteLabel()->text().contains("fig.pdf"));
        QVERIFY(!dialog.options().textAsOutlines);

        dialog.formatBox()->setCurrentIndex(dialog.formatBox()->findData(int(Format::Jpeg)));
        QVERIFY(dialog.qualityBox()->isVisibleTo(&dialog));
        QVERIFY(!dialog.transparentBox()->isEnabled());
        QVERIFY(!dialog.options().transparent);

        // A suffix typed chooses the format (SVG: every build has it; TIFF
        // and WebP need the qtimageformats plugins).
        dialog.fileEdit()->clear();
        QTest::keyClicks(dialog.fileEdit(), path("typed.svg"));
        QCOMPARE(dialog.format(), Format::Svg);
        QCOMPARE(dialog.fileName(), path("typed.svg"));
        dialog.fileEdit()->setText(path("plain"));
        QCOMPARE(dialog.fileName(), path("plain.svg"));
    }

    void theDialogsScaleResolutionAndPixels()
    {
        ExportDialog dialog(QSize(400, 300), QSize(100, 50), path("s.png"));
        QVERIFY(dialog.selectionBox()->isEnabled());
        dialog.dpiBox()->setValue(192);
        QCOMPARE(dialog.options().scale, 2.0);
        QCOMPARE(dialog.widthBox()->value(), 800);
        QCOMPARE(dialog.heightBox()->value(), 600);

        dialog.widthBox()->setValue(1200);
        QCOMPARE(dialog.options().scale, 3.0);
        QCOMPARE(dialog.heightBox()->value(), 900);
        QCOMPARE(dialog.dpiBox()->value(), 288);

        dialog.selectionBox()->setChecked(true);
        QCOMPARE(dialog.widthBox()->value(), 300);
        QCOMPARE(dialog.heightBox()->value(), 150);
        QVERIFY(dialog.options().selectionOnly);

        dialog.coloursBox()->setCurrentIndex(dialog.coloursBox()->findData(int(Colours::Grayscale)));
        dialog.transparentBox()->setChecked(true);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));

        // The next one starts from these choices.
        ExportDialog next(QSize(400, 300), QSize(), path("t.png"));
        QCOMPARE(next.options().scale, 3.0);
        QCOMPARE(next.options().colours, Colours::Grayscale);
        QVERIFY(next.options().transparent);
    }

    void theDialogsDiagramAndItsFolder()
    {
        ExportDialog dialog(QSize(400, 300), QSize(200, 100), path("d.png"));
        dialog.setDiagram();
        QVERIFY(dialog.options().selectionOnly);
        QVERIFY(!dialog.selectionBox()->isEnabled());

        // No such folder: the dialog stays.
        ExportDialog lost(QSize(400, 300), QSize(), path("no/such/folder/x.png"));
        QTimer::singleShot(0, [] {
            if (QWidget* box = QApplication::activeModalWidget())
                box->close();
        });
        lost.accept();
        QVERIFY(lost.result() != QDialog::Accepted);
    }
};

int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestGraphicsExport test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_graphics_export.moc"
