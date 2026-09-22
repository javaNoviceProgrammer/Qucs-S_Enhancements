/*
 * Images in a schematic and on a subcircuit symbol: which formats are
 * taken, that the bytes of the file travel inside the document, and that
 * an image on a symbol turns and mirrors with the component drawn from it.
 */
#include <QtTest>
#include <QPainter>
#include <QTemporaryDir>

#include <memory>

#include "config.h"
#include "element.h"
#include "embeddedimage.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "paintings/imagepainting.h"
#include "components/subcircuit.h"
#include "schematic.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

// Four quadrants, each its own colour: red green / blue yellow. Which
// colour is in which corner says how the picture has been turned.
const QColor TopLeft(220, 0, 0);
const QColor TopRight(0, 160, 0);
const QColor BottomLeft(0, 0, 220);
const QColor BottomRight(230, 200, 0);

QImage quadrants(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    QPainter painter(&image);
    painter.fillRect(0, 0, width / 2, height / 2, TopLeft);
    painter.fillRect(width / 2, 0, width - width / 2, height / 2, TopRight);
    painter.fillRect(0, height / 2, width / 2, height - height / 2, BottomLeft);
    painter.fillRect(width / 2, height / 2, width - width / 2, height - height / 2, BottomRight);
    return image;
}

// The colour a quarter of the way into a corner of `rect`.
QColor corner(const QImage& canvas, const QRectF& rect, double fx, double fy)
{
    return canvas.pixelColor(qRound(rect.x() + fx * rect.width()),
                             qRound(rect.y() + fy * rect.height()));
}

// Near enough: scaling and resampling shift a colour by a few counts.
bool same(const QColor& a, const QColor& b)
{
    return qAbs(a.red() - b.red()) < 24 && qAbs(a.green() - b.green()) < 24
           && qAbs(a.blue() - b.blue()) < 24;
}

QString describe(const QColor& c)
{
    return QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

#define COMPARE_COLOR(got, want) \
    QVERIFY2(same((got), (want)), qPrintable(describe(got) + " != " + describe(want)))

// The four corners of an image drawn into `rect`.
struct Corners {
    QColor topLeft, topRight, bottomLeft, bottomRight;
};

Corners cornersOf(const QImage& canvas, const QRectF& rect)
{
    return {corner(canvas, rect, 0.25, 0.25), corner(canvas, rect, 0.75, 0.25),
            corner(canvas, rect, 0.25, 0.75), corner(canvas, rect, 0.75, 0.75)};
}

// The picture a painting makes, and where in it the painting sits.
QImage paintingCanvas(Painting* painting, int size = 300)
{
    QImage canvas(size, size, QImage::Format_ARGB32);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    painter.translate(size / 2 - painting->cx, size / 2 - painting->cy);
    painting->paint(&painter);
    painter.end();
    return canvas;
}

QRectF placeOf(Painting* painting, int size = 300)
{
    return painting->boundingRect().translated(size / 2 - painting->cx, size / 2 - painting->cy);
}

QImage componentCanvas(Component* component, int size = 300)
{
    QImage canvas(size, size, QImage::Format_ARGB32);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    painter.translate(size / 2 - component->cx, size / 2 - component->cy);
    component->paint(&painter);
    painter.end();
    return canvas;
}

QRectF placeOf(qucs::Image* image, int size = 300)
{
    return QRectF{double(size / 2) + image->x, double(size / 2) + image->y, image->w, image->h};
}

// A painting as a document holds it: placed, and with its centre worked
// out from its corners, which is what load() does.
std::unique_ptr<ImagePainting> placed(const QString& file, int x1, int y1, int x2, int y2)
{
    ImagePainting source;
    source.setImageFromPath(file);
    source.x1 = x1;
    source.y1 = y1;
    source.x2 = x2;
    source.y2 = y2;

    auto painting = std::make_unique<ImagePainting>();
    if (!painting->load(source.save())) return nullptr;
    return painting;
}

} // namespace

class TestSymbolImage : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString png, jpg, svg, text;

    static void write(const QString& path, const QByteArray& bytes)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }

    // A subcircuit whose symbol is nothing but one image and one port.
    QString subcircuitWithImage(const QString& name, const QString& imageFile,
                                int x1, int y1, int x2, int y2)
    {
        QFile source(imageFile);
        if (!source.open(QIODevice::ReadOnly)) return QString();
        const QString data = QString::fromLatin1(source.readAll().toBase64());
        const QString format = QFileInfo(imageFile).suffix() == "svg" ? "svg" : "png";

        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return QString();
        QTextStream(&f)
            << "<Qucs Schematic " PACKAGE_VERSION ">\n"
            << "<Symbol>\n"
            << QStringLiteral("  <ImagePainting %1 %2 %3 %4 %5 %6 0 0>\n")
                   .arg(x1).arg(y1).arg(x2).arg(y2).arg(data, format)
            << "  <.PortSym -50 0 1 0>\n"
            << "</Symbol>\n"
            << "<Components>\n</Components>\n"
            << "<Wires>\n</Wires>\n"
            << "<Diagrams>\n</Diagrams>\n"
            << "<Paintings>\n</Paintings>\n";
        return path;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();

        png = dir.filePath("quadrants.png");
        QVERIFY(quadrants(60, 40).save(png, "PNG"));
        jpg = dir.filePath("quadrants.jpg");
        QVERIFY(quadrants(60, 40).save(jpg, "JPG"));

        // Two halves, so that a rescaled render can be told from a sharp one.
        svg = dir.filePath("quadrants.svg");
        write(svg,
              "<?xml version=\"1.0\"?>\n"
              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"60\" height=\"40\">\n"
              "  <rect x=\"0\" y=\"0\" width=\"30\" height=\"20\" fill=\"#dc0000\"/>\n"
              "  <rect x=\"30\" y=\"0\" width=\"30\" height=\"20\" fill=\"#00a000\"/>\n"
              "  <rect x=\"0\" y=\"20\" width=\"30\" height=\"20\" fill=\"#0000dc\"/>\n"
              "  <rect x=\"30\" y=\"20\" width=\"30\" height=\"20\" fill=\"#e6c800\"/>\n"
              "</svg>\n");

        text = dir.filePath("notes.txt");
        write(text, "not an image\n");
    }

    // ---- the formats that are offered -------------------------------

    void theFormatsTakeVectorsAndPhotographs()
    {
        const QString filter = qucs_s::EmbeddedImage::fileDialogFilter();
        for (const char* pattern : {"*.png", "*.svg", "*.svgz", "*.jpg", "*.bmp", "*.gif"})
            QVERIFY2(filter.contains(QLatin1String(pattern)), pattern);

        QVERIFY(qucs_s::EmbeddedImage::isSupportedFile(png));
        QVERIFY(qucs_s::EmbeddedImage::isSupportedFile(jpg));
        QVERIFY(qucs_s::EmbeddedImage::isSupportedFile(svg));
        QVERIFY(!qucs_s::EmbeddedImage::isSupportedFile(text));
        QVERIFY(!qucs_s::EmbeddedImage::isSupportedFile(dir.filePath("nothing-here.png")));
    }

    void everyOfferedFormatCanBePlaced()
    {
        for (const QString& file : {png, jpg, svg}) {
            ImagePainting painting;
            painting.setImageFromPath(file);
            QVERIFY2(!painting.embeddedImage().isNull(), qPrintable(file));
            QCOMPARE(painting.getImageWidth(), 60);
            QCOMPARE(painting.getImageHeight(), 40);
        }
    }

    // ---- the image is inside the document ---------------------------

    void theImageTravelsInsideTheDocument()
    {
        const QString copy = dir.filePath("gone-tomorrow.png");
        QVERIFY(QFile::copy(png, copy));

        ImagePainting painting;
        painting.setImageFromPath(copy);
        painting.x1 = painting.y1 = 0;
        painting.x2 = 60;
        painting.y2 = 40;
        const QString saved = painting.save();

        QVERIFY(QFile::remove(copy));   // the file is not needed again

        ImagePainting reloaded;
        QVERIFY(reloaded.load(saved));
        QCOMPARE(reloaded.getImageWidth(), 60);
        QCOMPARE(reloaded.getImageHeight(), 40);

        const Corners c = cornersOf(paintingCanvas(&reloaded), placeOf(&reloaded));
        COMPARE_COLOR(c.topLeft, TopLeft);
        COMPARE_COLOR(c.topRight, TopRight);
        COMPARE_COLOR(c.bottomLeft, BottomLeft);
        COMPARE_COLOR(c.bottomRight, BottomRight);
    }

    void aVectorImageStaysAVector()
    {
        ImagePainting painting;
        painting.setImageFromPath(svg);
        QCOMPARE(painting.embeddedImage().format(), QStringLiteral("svg"));

        ImagePainting reloaded;
        QVERIFY(reloaded.load(painting.save()));
        QCOMPARE(reloaded.embeddedImage().format(), QStringLiteral("svg"));

        // Blown up to ten times its size it is still drawn from the source,
        // so the edge between two colours stays one pixel wide.
        const QPixmap big = reloaded.embeddedImage().pixmap(QSize(600, 400));
        QCOMPARE(big.size(), QSize(600, 400));
        const QImage image = big.toImage();
        COMPARE_COLOR(image.pixelColor(298, 100), TopLeft);
        COMPARE_COLOR(image.pixelColor(302, 100), TopRight);
    }

    // A document written by 26.1.2 carries five fields and a PNG; one
    // whose image cannot be read must not take the document with it.
    void theOlderDocumentFormatStillLoads()
    {
        QFile file(png);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString data = QString::fromLatin1(file.readAll().toBase64());

        ImagePainting old;
        QVERIFY(old.load("ImagePainting 0 0 60 40 " + data));
        QCOMPARE(old.getImageWidth(), 60);
        QCOMPARE(old.embeddedImage().quarterTurns(), 0);
        COMPARE_COLOR(cornersOf(paintingCanvas(&old), placeOf(&old)).topLeft, TopLeft);

        ImagePainting damaged;
        QVERIFY(damaged.load("ImagePainting 0 0 60 40 " + data.left(40)));
        QVERIFY(damaged.embeddedImage().isNull());

        ImagePainting empty;
        QVERIFY(empty.load("ImagePainting 0 0 60 40"));
        QVERIFY(empty.embeddedImage().isNull());

        ImagePainting nonsense;
        QVERIFY(!nonsense.load("ImagePainting 0 0"));
    }

    // ---- turning and mirroring a painting ---------------------------

    void aTurnedPaintingTurnsItsPicture()
    {
        auto painting = placed(png, 0, 0, 60, 40);
        QVERIFY(painting);

        QVERIFY(painting->rotate());   // a quarter turn counter-clockwise
        QCOMPARE(painting->boundingRect().size(), QSize(40, 60));

        const Corners c = cornersOf(paintingCanvas(painting.get()), placeOf(painting.get()));
        COMPARE_COLOR(c.topLeft, TopRight);       // the right edge is now on top
        COMPARE_COLOR(c.bottomLeft, TopLeft);
        COMPARE_COLOR(c.bottomRight, BottomLeft);
        COMPARE_COLOR(c.topRight, BottomRight);
    }

    void aTurnedPaintingStillSaves()
    {
        // Up to 26.1.2 turning an image that had been read back from a
        // document emptied it, and the document would not load again.
        auto embedded = placed(png, 0, 0, 60, 40);
        QVERIFY(embedded);
        QVERIFY(embedded->rotate());

        const QString saved = embedded->save();
        ImagePainting reloaded;
        QVERIFY2(reloaded.load(saved), qPrintable(saved.left(60)));
        QVERIFY(!reloaded.embeddedImage().isNull());
        QCOMPARE(reloaded.embeddedImage().quarterTurns(), 1);

        const Corners c = cornersOf(paintingCanvas(&reloaded), placeOf(&reloaded));
        COMPARE_COLOR(c.topLeft, TopRight);
    }

    void aMirroredPaintingMirrorsItsPicture()
    {
        auto painting = placed(png, 0, 0, 60, 40);
        QVERIFY(painting);

        QVERIFY(painting->mirrorX());   // about the horizontal axis
        Corners c = cornersOf(paintingCanvas(painting.get()), placeOf(painting.get()));
        COMPARE_COLOR(c.topLeft, BottomLeft);
        COMPARE_COLOR(c.bottomRight, TopRight);

        QVERIFY(painting->mirrorX());   // and back
        c = cornersOf(paintingCanvas(painting.get()), placeOf(painting.get()));
        COMPARE_COLOR(c.topLeft, TopLeft);

        QVERIFY(painting->mirrorY());   // about the vertical axis
        c = cornersOf(paintingCanvas(painting.get()), placeOf(painting.get()));
        COMPARE_COLOR(c.topLeft, TopRight);
        COMPARE_COLOR(c.bottomRight, BottomLeft);
    }

    // ---- the image on an instantiated subcircuit ---------------------

    void theSubcircuitSymbolShowsItsImage()
    {
        const QString file = subcircuitWithImage("picture.sch", png, -30, -20, 30, 20);
        QVERIFY(!file.isEmpty());

        Subcircuit sub;
        sub.Props.front()->Value = file;
        sub.recreate();

        QCOMPARE(sub.Images.count(), 1);
        qucs::Image* image = sub.Images.first();
        QCOMPARE(QRectF(image->x, image->y, image->w, image->h), QRectF(-30, -20, 60, 40));

        const Corners c = cornersOf(componentCanvas(&sub), placeOf(image));
        COMPARE_COLOR(c.topLeft, TopLeft);
        COMPARE_COLOR(c.topRight, TopRight);
        COMPARE_COLOR(c.bottomLeft, BottomLeft);
        COMPARE_COLOR(c.bottomRight, BottomRight);
    }

    void theSymbolImageTurnsWithTheComponent()
    {
        const QString file = subcircuitWithImage("turning.sch", png, -30, -20, 30, 20);
        QVERIFY(!file.isEmpty());

        Subcircuit sub;
        sub.Props.front()->Value = file;
        sub.recreate();
        QVERIFY(sub.rotate());

        qucs::Image* image = sub.Images.first();
        QCOMPARE(QRectF(image->x, image->y, image->w, image->h), QRectF(-20, -30, 40, 60));

        const Corners c = cornersOf(componentCanvas(&sub), placeOf(image));
        COMPARE_COLOR(c.topLeft, TopRight);
        COMPARE_COLOR(c.bottomLeft, TopLeft);
        COMPARE_COLOR(c.bottomRight, BottomLeft);
        COMPARE_COLOR(c.topRight, BottomRight);
    }

    void theSymbolImageMirrorsWithTheComponent()
    {
        const QString file = subcircuitWithImage("mirroring.sch", png, -30, -20, 30, 20);
        QVERIFY(!file.isEmpty());

        Subcircuit sub;
        sub.Props.front()->Value = file;
        sub.recreate();

        QVERIFY(sub.mirrorX());
        Corners c = cornersOf(componentCanvas(&sub), placeOf(sub.Images.first()));
        COMPARE_COLOR(c.topLeft, BottomLeft);
        COMPARE_COLOR(c.topRight, BottomRight);

        QVERIFY(sub.mirrorY());   // mirrored both ways is turned by 180
        c = cornersOf(componentCanvas(&sub), placeOf(sub.Images.first()));
        COMPARE_COLOR(c.topLeft, BottomRight);
        COMPARE_COLOR(c.bottomRight, TopLeft);
    }

    // The component is rebuilt from its file whenever anything about it
    // changes; the turns it had must come back with it.
    void theTurnedSymbolSurvivesARebuild()
    {
        const QString file = subcircuitWithImage("rebuilt.sch", png, -30, -20, 30, 20);
        QVERIFY(!file.isEmpty());

        Subcircuit sub;
        sub.Props.front()->Value = file;
        sub.recreate();
        QVERIFY(sub.rotate());
        sub.recreate();   // as a reload of the symbol file does it

        QCOMPARE(sub.Images.count(), 1);
        qucs::Image* image = sub.Images.first();
        QCOMPARE(QRectF(image->x, image->y, image->w, image->h), QRectF(-20, -30, 40, 60));
        COMPARE_COLOR(cornersOf(componentCanvas(&sub), placeOf(image)).topLeft, TopRight);
    }

    // The symbol editor writes the turned image back into the document,
    // which has to load again. Up to 26.1.2 turning an image emptied it and
    // the document was refused on the next open.
    void aTurnedSymbolImageLeavesALoadableDocument()
    {
        const QString file = subcircuitWithImage("symbolmode.sch", png, -30, -20, 30, 20);
        QVERIFY(!file.isEmpty());

        auto* doc = new Schematic(nullptr, file);
        QVERIFY(doc->load());

        ImagePainting* image = nullptr;
        for (Painting* painting : doc->a_SymbolPaints)
            if (auto* found = dynamic_cast<ImagePainting*>(painting)) image = found;
        QVERIFY(image);
        QVERIFY(image->rotate());
        QVERIFY(doc->save() >= 0);
        delete doc;

        auto* reloaded = new Schematic(nullptr, file);
        QVERIFY2(reloaded->load(), "the document with the turned image did not load");

        ImagePainting* back = nullptr;
        for (Painting* painting : reloaded->a_SymbolPaints)
            if (auto* found = dynamic_cast<ImagePainting*>(painting)) back = found;
        QVERIFY(back);
        QCOMPARE(back->embeddedImage().quarterTurns(), 1);
        COMPARE_COLOR(cornersOf(paintingCanvas(back), placeOf(back)).topLeft, TopRight);
        delete reloaded;

        // And the subcircuit drawn from it is turned too.
        Subcircuit sub;
        sub.Props.front()->Value = file;
        sub.recreate();
        QCOMPARE(sub.Images.count(), 1);
        COMPARE_COLOR(cornersOf(componentCanvas(&sub), placeOf(sub.Images.first())).topLeft,
                      TopRight);
    }

    void aVectorSymbolIsDrawnFromItsSource()
    {
        const QString file = subcircuitWithImage("vector.sch", svg, -30, -20, 30, 20);
        QVERIFY(!file.isEmpty());

        Subcircuit sub;
        sub.Props.front()->Value = file;
        sub.recreate();

        QCOMPARE(sub.Images.count(), 1);
        QCOMPARE(sub.Images.first()->image.format(), QStringLiteral("svg"));
        COMPARE_COLOR(cornersOf(componentCanvas(&sub), placeOf(sub.Images.first())).topLeft, TopLeft);
    }
};

int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestSymbolImage test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_symbol_image.moc"
