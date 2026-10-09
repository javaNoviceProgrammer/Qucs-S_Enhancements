/*
 * The image viewer (imagedoc.h): a picture opened in a tab of its own -
 * one tab for one file - from the menus, the panels and a drop; shown
 * whole at first, zoomed as the View menu, the toolbar, the wheel and a
 * double click do, turned; its pixels read under the pointer; a part of
 * it selected and copied; an SVG drawn anew at every scale; an
 * animation's frames played and gone through, a file of several pictures'
 * too; a photo upright as its camera said; read again when its file
 * changes; saved under another name - copied, or converted; printed; a
 * file that is no picture refused; and every command of the Edit,
 * Positioning, Insert, Simulation and View menus used with it in front -
 * none of them may take it for a schematic.
 */
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QImageReader>
#include <QImageWriter>
#include <QLineEdit>
#include <QMenuBar>
#include <QMimeData>
#include <QPainter>
#include <QPrinter>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolButton>

#include <functional>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "filebrowser.h"
#include "imagedoc.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "textdoc.h"

using qucs_s::image::ImageView;

namespace {

const QColor kRed(0xff, 0x00, 0x00);
const QColor kGreen(0x00, 0xc0, 0x00);
const QColor kBlue(0x00, 0x00, 0xff);

// A picture of 40 × 30: red, green, blue quarters and a transparent one
// (top left, top right, bottom left, bottom right), a black pixel at 5, 5.
QImage probe(const QColor& first = kRed)
{
    QImage image(40, 30, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 40; ++x) {
            if (x < 20 && y < 15) image.setPixelColor(x, y, first);
            else if (x >= 20 && y < 15) image.setPixelColor(x, y, kGreen);
            else if (x < 20) image.setPixelColor(x, y, kBlue);
        }
    image.setPixelColor(5, 5, Qt::black);
    return image;
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// An SVG of 100 × 50: red on the left half, blue on the right.
const char* const kSvg = "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='50' viewBox='0 0 100 50'>"
                         "<rect x='0' y='0' width='50' height='50' fill='#ff0000'/>"
                         "<rect x='50' y='0' width='50' height='50' fill='#0000ff'/></svg>";

void le16(QByteArray& out, int v)
{
    out += char(v & 0xff);
    out += char((v >> 8) & 0xff);
}

// An animated GIF of \a frames (a few colours each), \a delays in ms -
// written here: Qt reads GIFs, it does not write them. The pixels as
// literal codes of 8 bits (a minimum code size of 7, the table cleared
// every 100 codes, so the code size never grows): no compression, and
// nothing to get wrong.
QByteArray gif(const QList<QImage>& frames, const QList<int>& delays)
{
    QList<QRgb> palette;
    for (const QImage& f : frames)
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (!palette.contains(f.pixel(x, y))) palette << f.pixel(x, y);
    while (palette.size() < 128) palette << qRgb(0, 0, 0);
    const QSize size = frames.first().size();
    QByteArray out("GIF89a");
    le16(out, size.width());
    le16(out, size.height());
    out += char(0xf6);   // a global table of 128 colours
    out += char(0);
    out += char(0);
    for (QRgb c : palette) {
        out += char(qRed(c));
        out += char(qGreen(c));
        out += char(qBlue(c));
    }
    out += QByteArray::fromHex("21ff0b") + "NETSCAPE2.0" + QByteArray::fromHex("03010000") + char(0);   // looped
    for (int i = 0; i < frames.size(); ++i) {
        out += QByteArray::fromHex("21f904");
        out += char(0x04);   // left in place
        le16(out, delays.value(i) / 10);
        out += char(0);
        out += char(0);
        out += char(0x2c);
        le16(out, 0);
        le16(out, 0);
        le16(out, size.width());
        le16(out, size.height());
        out += char(0);
        out += char(7);   // the minimum code size
        QByteArray codes;
        codes += char(128);   // clear
        int n = 0;
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                codes += char(palette.indexOf(frames.at(i).pixel(x, y)));
                if (++n % 100 == 0) codes += char(128);
            }
        codes += char(129);   // the end
        for (int at = 0; at < codes.size(); at += 255) {
            const QByteArray block = codes.mid(at, 255);
            out += char(block.size());
            out += block;
        }
        out += char(0);
    }
    out += char(0x3b);
    return out;
}

// An icon of several sizes: each written by Qt alone, put together.
QByteArray icon(const QList<int>& sides)
{
    QList<QByteArray> entries, datas;
    for (int side : sides) {
        QImage image(side, side, QImage::Format_ARGB32);
        image.fill(side == sides.last() ? kBlue : kRed);
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        if (!QImageWriter(&buffer, "ico").write(image)) return {};
        const QByteArray one = buffer.data();
        entries << one.mid(6, 16);
        datas << one.mid(22);
    }
    QByteArray out = QByteArray::fromHex("00000100");
    le16(out, int(sides.size()));
    qint64 offset = 6 + 16 * sides.size();
    for (int i = 0; i < entries.size(); ++i) {
        QByteArray entry = entries.at(i);
        const quint32 at = quint32(offset);
        entry[12] = char(at & 0xff);
        entry[13] = char((at >> 8) & 0xff);
        entry[14] = char((at >> 16) & 0xff);
        entry[15] = char((at >> 24) & 0xff);
        out += entry;
        offset += datas.at(i).size();
    }
    for (const QByteArray& d : datas) out += d;
    return out;
}

// A JPEG of 40 × 20 (red left, blue right) whose camera was held on its
// side: an Exif orientation of 6, "turn it a quarter clockwise".
QByteArray sidewaysJpeg()
{
    QImage image(40, 20, QImage::Format_RGB32);
    image.fill(kBlue);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x) image.setPixelColor(x, y, kRed);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(100);
    if (!writer.write(image)) return {};
    QByteArray exif = QByteArray("Exif") + char(0) + char(0)
                      + QByteArray::fromHex("4d4d002a00000008"
                                            "0001"
                                            "011200030000000100060000"
                                            "00000000");
    QByteArray segment = QByteArray::fromHex("ffe1");
    segment += char(((exif.size() + 2) >> 8) & 0xff);
    segment += char((exif.size() + 2) & 0xff);
    segment += exif;
    const QByteArray jpeg = buffer.data();
    return jpeg.left(2) + segment + jpeg.mid(2);
}

bool near(const QColor& a, const QColor& b, int tolerance = 24)
{
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
           && std::abs(a.blue() - b.blue()) <= tolerance;
}

QString describe(const QColor& c)
{
    return QStringLiteral("%1 (%2, %3, %4, %5)").arg(c.name()).arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

// The view as the screen shows it.
QImage shot(ImageView* view)
{
    view->viewport()->repaint();
    return view->viewport()->grab().toImage();
}

void mouse(QWidget* w, QEvent::Type type, QPoint pos, Qt::MouseButton button = Qt::NoButton,
           Qt::MouseButtons buttons = Qt::NoButton, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent e(type, QPointF(pos), w->mapToGlobal(QPointF(pos)), button, buttons, modifiers);
    QApplication::sendEvent(w, &e);
}

// The system's viewer is not started: the files it is given end here.
class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls << url; }
};

// Whatever dialog comes up is closed: the commands are used, not answered.
class DialogCloser : public QObject
{
public:
    explicit DialogCloser(QObject* parent) : QObject(parent)
    {
        startTimer(40);
    }

protected:
    void timerEvent(QTimerEvent*) override
    {
        if (QWidget* w = QApplication::activeModalWidget()) {
            if (auto* d = qobject_cast<QDialog*>(w)) d->reject();
            else w->close();
        }
    }
};

} // namespace

class TestImageDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QString png, svg;
    UrlCatcher catcher;

    ImageDoc* doc() const { return qobject_cast<ImageDoc*>(app->DocumentTab->currentWidget()); }

    // Some pictures, as the panes show them, for a look (QUCS_TEST_GRAB).
    void grab(const QString& name)
    {
        const QString to = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (to.isEmpty() || doc() == nullptr) return;
        QApplication::processEvents();   // (its bar's labels laid out)
        QDir().mkpath(to);
        doc()->grab().save(to + QStringLiteral("/image-%1.png").arg(name));
    }

    ImageDoc* open(const QString& path)
    {
        if (!app->gotoPage(path)) return nullptr;
        ImageDoc* d = doc();
        if (d != nullptr) QApplication::processEvents();
        return d;
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
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &catcher, "open");
        png = dir.filePath("ws/probe.png");
        QVERIFY(probe().save(png));
        svg = dir.filePath("ws/halves.svg");
        QVERIFY(writeBytes(svg, kSvg));
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 900);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
    }

    void cleanupTestCase()
    {
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        for (QucsDoc* d : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(d)) sch->setChanged(false);
            d->setDocChanged(false);
        }
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    // The suffixes it opens: what this Qt reads, SVG; not a PDF, not a
    // schematic.
    void theKindsItOpens()
    {
        for (const char* name : {"a.png", "A.PNG", "b.jpg", "b.jpeg", "c.svg", "c.svgz", "d.gif", "e.bmp", "f.ico", "g.ppm"})
            QVERIFY2(QucsApp::isImageFile(QString::fromLatin1(name)), name);
        for (const char* name : {"a.pdf", "a.sch", "a.txt", "a.dat", "a.png.txt", "png"})
            QVERIFY2(!QucsApp::isImageFile(QString::fromLatin1(name)), name);
        for (const char* written : {"png", "jpg", "jpeg", "bmp"})
            QVERIFY2(qucs_s::image::writableSuffixes().contains(QLatin1String(written)), written);
        QVERIFY(!qucs_s::image::writableSuffixes().contains(QStringLiteral("svg")));
    }

    // A tab of its own - once - known as a document by the window and by
    // Claude's tools; shown whole, in the middle, at 100% (it is small).
    void aPictureOpensInATab()
    {
        ImageDoc* d = open(png);
        QVERIFY(d != nullptr);
        QVERIFY(QucsApp::isImageDocument(d));
        QVERIFY(!QucsApp::isTextDocument(d));
        QVERIFY(!QucsApp::isPdfDocument(d));
        QVERIFY(QucsApp::schematicIn(d) == nullptr);
        QCOMPARE(app->getDoc(), static_cast<QucsDoc*>(d));
        QCOMPARE(app->DocumentTab->tabText(app->DocumentTab->currentIndex()).remove('&'), QStringLiteral("probe.png"));
        QVERIFY(d->getDataDisplay().isEmpty());   // not simulated
        QCOMPARE(d->format(), QStringLiteral("png"));
        QCOMPARE(d->imageSize(), QSize(40, 30));
        QCOMPARE(d->frameCount(), 1);
        QVERIFY(!d->isVector());
        QVERIFY(d->problem().isEmpty());
        QVERIFY2(d->facts().contains("PNG") && d->facts().contains(QStringLiteral("40 × 30 pixels")) && d->facts().contains("RGBA"),
                 qPrintable(d->facts()));
        ImageView* view = d->view();
        QCOMPARE(view->fit(), ImageView::Fit::Auto);
        QCOMPARE(view->zoom(), 1.0);
        const QPointF middle = view->pictureRect().center();
        QVERIFY(std::abs(middle.x() - view->viewport()->width() / 2.0) <= 1);
        QVERIFY(std::abs(middle.y() - view->viewport()->height() / 2.0) <= 1);
        QVERIFY(!d->frameBar()->isVisible());   // one picture: no frames

        const int tabs = app->DocumentTab->count();
        QVERIFY(app->gotoPage(png));
        QCOMPARE(app->DocumentTab->count(), tabs);
        QCOMPARE(app->findDoc(png), static_cast<QucsDoc*>(d));

        auto* control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        QVERIFY(QucsControl::textOf(control->callNow("get_state", {})).remove(' ').contains("\"kind\":\"picture\""));
        QVERIFY(control->callNow("get_schematic", {}).value("isError").toBool());   // not a schematic
        grab("small");
    }

    // From the Content panel and the File Browser, and a drop - an SVG,
    // which is text, too: the same tab, not the system's viewer nor the
    // text editor.
    void itOpensFromThePanels()
    {
        const int tabs = app->DocumentTab->count();
        app->openFileFromProjectView(QFileInfo(png), QString());
        QCOMPARE(app->DocumentTab->count(), tabs);
        QVERIFY(QucsApp::isImageDocument(app->DocumentTab->currentWidget()));
        app->openDroppedFile(svg);
        QCOMPARE(app->DocumentTab->count(), tabs + 1);
        QVERIFY(QucsApp::isImageDocument(app->DocumentTab->currentWidget()));
        QVERIFY(doc()->isVector());
        app->slotFileClose(app->DocumentTab->currentIndex());
        QVERIFY(catcher.urls.isEmpty());   // not the system's viewer
        // Unless asked for.
        open(png)->openExternally();
        QCOMPARE(catcher.urls, QList<QUrl>{QUrl::fromLocalFile(QFileInfo(png).absoluteFilePath())});
        catcher.urls.clear();
    }

    // Its pixels under the pointer, in the file's coordinates: where, its
    // colour, how transparent.
    void itsPixelsAreRead()
    {
        ImageDoc* d = open(png);
        ImageView* view = d->view();
        view->setZoom(4.0);
        QWidget* vp = view->viewport();
        mouse(vp, QEvent::MouseMove, view->toViewport(QPointF(5.5, 5.5)).toPoint());
        QVERIFY2(d->pointerText().contains("x 5") && d->pointerText().contains("y 5") && d->pointerText().contains("#000000"),
                 qPrintable(d->pointerText()));
        mouse(vp, QEvent::MouseMove, view->toViewport(QPointF(25.5, 2.5)).toPoint());
        QVERIFY2(d->pointerText().contains("#00C000") && d->pointerText().contains("(0, 192, 0)"), qPrintable(d->pointerText()));
        grab("pointer");
        QVERIFY(!d->pointerText().contains("alpha"));
        mouse(vp, QEvent::MouseMove, view->toViewport(QPointF(30.5, 20.5)).toPoint());
        QVERIFY2(d->pointerText().contains("alpha 0%"), qPrintable(d->pointerText()));
        // Off it, and out of the view: nothing.
        mouse(vp, QEvent::MouseMove, view->toViewport(QPointF(-3, -3)).toPoint());
        QVERIFY(d->pointerText().isEmpty());
        QCOMPARE(view->pixelAt(view->toViewport(QPointF(39.5, 29.5))), QPoint(39, 29));
        QCOMPARE(view->pixelAt(view->toViewport(QPointF(40.5, 29.5))), QPoint(-1, -1));
    }

    // Zoomed by the toolbar, the View menu, a typed scale, the wheel about
    // the pointer, a double click; fitted.
    void itIsZoomed()
    {
        ImageDoc* d = open(png);
        ImageView* view = d->view();
        view->setFit(ImageView::Fit::Auto);
        QCOMPARE(view->zoom(), 1.0);
        view->zoomStep(true);
        QCOMPARE(view->zoom(), 1.25);
        view->zoomStep(false);
        QCOMPARE(view->zoom(), 1.0);
        view->zoomStep(false);
        QCOMPARE(view->zoom(), 0.75);
        // The View menu.
        view->setZoom(1.0);
        app->magPlus->trigger();
        QCOMPARE(view->zoom(), 1.25);
        app->magMinus->trigger();
        QCOMPARE(view->zoom(), 1.0);
        app->magAll->trigger();   // View All: the whole, as large as it goes
        QCOMPARE(view->fit(), ImageView::Fit::Window);
        QVERIFY(view->zoom() > 10);
        QVERIFY(view->pictureRect().width() <= view->viewport()->width());
        QVERIFY(view->pictureRect().height() <= view->viewport()->height());
        QVERIFY2(d->zoomBox()->currentText().startsWith(QStringLiteral("Fit Window")), qPrintable(d->zoomBox()->currentText()));
        app->magOne->trigger();   // View 1:1
        QCOMPARE(view->fit(), ImageView::Fit::None);
        QCOMPARE(view->zoom(), 1.0);
        // A scale typed.
        d->zoomBox()->lineEdit()->setText(QStringLiteral("250%"));
        QTest::keyClick(d->zoomBox()->lineEdit(), Qt::Key_Return);
        QCOMPARE(view->zoom(), 2.5);
        QCOMPARE(d->zoomBox()->currentText(), QStringLiteral("250%"));
        d->zoomBox()->lineEdit()->setText(QStringLiteral("Best Fit"));
        QTest::keyClick(d->zoomBox()->lineEdit(), Qt::Key_Return);
        QCOMPARE(view->fit(), ImageView::Fit::Auto);
        // The wheel with Ctrl: the point under the pointer stays there
        // (the picture larger than the view: else it is in the middle).
        view->setZoom(32.0);
        const QPoint at = view->toViewport(QPointF(30, 10)).toPoint();
        const QPointF before = view->toPicture(QPointF(at));
        QWheelEvent wheel(QPointF(at), view->viewport()->mapToGlobal(QPointF(at)), QPoint(), QPoint(0, 240), Qt::NoButton,
                          Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &wheel);
        QVERIFY(view->zoom() > 32.0);
        const QPointF after = view->toPicture(QPointF(at));
        QVERIFY2(std::abs(after.x() - before.x()) < 0.3 && std::abs(after.y() - before.y()) < 0.3,
                 qPrintable(QStringLiteral("%1,%2 -> %3,%4").arg(before.x()).arg(before.y()).arg(after.x()).arg(after.y())));
        // A double click: 100% there, and back to the whole.
        mouse(view->viewport(), QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(view->zoom(), 1.0);
        QCOMPARE(view->fit(), ImageView::Fit::None);
        mouse(view->viewport(), QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(view->fit(), ImageView::Fit::Auto);
    }

    // Larger: sharp pixels, their grid from 800%; smaller: smooth.
    void itIsDrawnSharpOrSmooth()
    {
        // Columns of one pixel, black and white.
        QImage stripes(64, 64, QImage::Format_RGB32);
        for (int x = 0; x < 64; ++x)
            for (int y = 0; y < 64; ++y) stripes.setPixelColor(x, y, x % 2 ? Qt::white : Qt::black);
        const QString file = dir.filePath("ws/stripes.png");
        QVERIFY(stripes.save(file));
        ImageDoc* d = open(file);
        ImageView* view = d->view();
        d->view()->setPixelGrid(false);
        const qreal dpr = view->viewport()->devicePixelRatioF();
        view->setZoom(4.0);
        QImage screen = shot(view);
        const auto at = [&](QPointF picture) {
            const QPointF p = view->toViewport(picture) * dpr;
            return screen.pixelColor(int(p.x()), int(p.y()));
        };
        QVERIFY2(at(QPointF(10.5, 10.5)) == QColor(Qt::black), qPrintable(describe(at(QPointF(10.5, 10.5)))));
        QVERIFY2(at(QPointF(11.5, 10.5)) == QColor(Qt::white), qPrintable(describe(at(QPointF(11.5, 10.5)))));
        QVERIFY(at(QPointF(10.1, 10.1)) == QColor(Qt::black));   // the whole of the pixel
        QVERIFY(at(QPointF(10.9, 10.9)) == QColor(Qt::black));
        // Half the size on the screen's pixels: grey (on a screen of twice
        // the pixels, 50% is a pixel for a pixel - sharp).
        view->setZoom(0.5 / dpr);
        screen = shot(view);
        const QColor grey = at(QPointF(32, 32));
        QVERIFY2(grey.red() > 70 && grey.red() < 185, qPrintable(describe(grey)));
        // The grid at 800% when it is on: on the edge between two pixels.
        view->setZoom(8.0);
        screen = shot(view);
        QVERIFY2(at(QPointF(11.0, 10.5)) == QColor(Qt::white), qPrintable(describe(at(QPointF(11.0, 10.5)))));
        view->setPixelGrid(true);
        screen = shot(view);
        QVERIFY2(at(QPointF(11.0, 10.5)) != QColor(Qt::white), qPrintable(describe(at(QPointF(11.0, 10.5)))));
        QVERIFY(at(QPointF(10.5, 10.5)) == QColor(Qt::black));   // the pixel itself as it was
        // Not below 800%.
        view->setZoom(6.0);
        screen = shot(view);
        QVERIFY2(at(QPointF(11.0, 10.5)) == QColor(Qt::white), qPrintable(describe(at(QPointF(11.0, 10.5)))));
        grab("grid");
        app->slotFileClose(app->DocumentTab->currentIndex());
    }

    // A photo of many pixels, at the fit: drawn small from its halves - the
    // right colours where they are.
    void aLargePictureIsDrawn()
    {
        QImage large(4000, 3000, QImage::Format_RGB32);
        large.fill(kBlue);
        {
            QPainter p(&large);
            p.fillRect(0, 0, 2000, 3000, kRed);
        }
        const QString file = dir.filePath("ws/large.png");
        QVERIFY(large.save(file));
        ImageDoc* d = open(file);
        ImageView* view = d->view();
        QCOMPARE(view->fit(), ImageView::Fit::Auto);
        QVERIFY(view->zoom() < 0.5);
        const QImage screen = shot(view);
        const qreal dpr = view->viewport()->devicePixelRatioF();
        const QPointF left = view->toViewport(QPointF(500, 1500)) * dpr, right = view->toViewport(QPointF(3500, 1500)) * dpr;
        QVERIFY(near(screen.pixelColor(left.toPoint()), kRed));
        QVERIFY(near(screen.pixelColor(right.toPoint()), kBlue));
        // Scrolled at 100%: still right.
        view->setZoom(1.0);
        view->horizontalScrollBar()->setValue(view->horizontalScrollBar()->maximum());
        const QImage scrolled = shot(view);
        QVERIFY(near(scrolled.pixelColor(scrolled.width() / 2, scrolled.height() / 2), kBlue));
        app->slotFileClose(app->DocumentTab->currentIndex());
    }

    // An SVG: its own size, drawn anew at every scale - sharp; the part in
    // sight alone at a scale that would be too large whole.
    void anSvgIsDrawnAtEveryScale()
    {
        ImageDoc* d = open(svg);
        QVERIFY(d != nullptr);
        QVERIFY(d->isVector());
        QCOMPARE(d->format(), QStringLiteral("svg"));
        QCOMPARE(d->imageSize(), QSize(100, 50));
        QVERIFY2(d->facts().contains("SVG") && d->facts().contains("vector"), qPrintable(d->facts()));
        ImageView* view = d->view();
        // Whole, as large as it goes: it is as sharp at any size.
        QCOMPARE(view->fit(), ImageView::Fit::Auto);
        QVERIFY(view->zoom() > 5);
        const qreal dpr = view->viewport()->devicePixelRatioF();
        view->setZoom(4.0);
        QImage screen = shot(view);
        QCOMPARE(view->svgScale(), 4.0 * dpr);
        const int renders = view->svgRenders();
        const QPointF edge = view->toViewport(QPointF(50, 25)) * dpr;
        QVERIFY2(screen.pixelColor(int(edge.x()) - 1, int(edge.y())) == kRed, qPrintable(describe(screen.pixelColor(int(edge.x()) - 1, int(edge.y())))));
        QVERIFY2(screen.pixelColor(int(edge.x()) + 1, int(edge.y())) == kBlue, qPrintable(describe(screen.pixelColor(int(edge.x()) + 1, int(edge.y())))));
        // Scrolled: not drawn again.
        shot(view);
        QCOMPARE(view->svgRenders(), renders);
        view->setZoom(8.0);
        shot(view);
        QCOMPARE(view->svgScale(), 8.0 * dpr);
        QVERIFY(view->svgRenders() > renders);
        // 6400%: the part in sight.
        view->setZoom(64.0);
        screen = shot(view);
        QCOMPARE(view->svgScale(), 64.0 * dpr);
        const QPointF red = view->toViewport(view->toPicture(QPointF(view->viewport()->rect().center()))) * dpr;
        const QColor middle = screen.pixelColor(red.toPoint());
        QVERIFY2(middle == kRed || middle == kBlue, qPrintable(describe(middle)));
        view->setFit(ImageView::Fit::Auto);
        grab("svg");
        // Copied: drawn at twice its size, and its text with it.
        d->copyImage();
        const QMimeData* data = QApplication::clipboard()->mimeData();
        QVERIFY(data != nullptr && data->hasImage());
        QCOMPARE(qvariant_cast<QImage>(data->imageData()).size(), QSize(200, 100));
        QVERIFY(data->data("image/svg+xml").contains("<svg"));
    }

    // Turned a quarter: shown so, copied so; its pixels still the file's.
    void itIsTurned()
    {
        ImageDoc* d = open(png);
        ImageView* view = d->view();
        view->setZoom(4.0);
        d->rotateRight();
        QCOMPARE(view->turns(), 1);
        QCOMPARE(view->shownSize(), QSize(30, 40));
        QCOMPARE(view->pictureRect().size(), QSizeF(120, 160));
        // The red quarter (top left in the file) is at the top right.
        const QRectF shown = view->pictureRect();
        const qreal dpr = view->viewport()->devicePixelRatioF();
        const QImage screen = shot(view);
        const QPointF topRight = (shown.topRight() + QPointF(-10, 10)) * dpr;
        QVERIFY2(screen.pixelColor(topRight.toPoint()) == kRed, qPrintable(describe(screen.pixelColor(topRight.toPoint()))));
        // The pointer there is over the file's 2, 2.
        QCOMPARE(view->pixelAt(shown.topRight() + QPointF(-10, 10)), QPoint(2, 2));
        const QImage copy = d->picture(false);
        QCOMPARE(copy.size(), QSize(30, 40));
        QCOMPARE(copy.pixelColor(29, 0), kRed);
        QCOMPARE(copy.pixelColor(0, 0), kBlue);
        d->rotateLeft();
        d->rotateLeft();
        QCOMPARE(view->turns(), 3);
        QCOMPARE(d->picture(false).pixelColor(0, 39), kRed);
        d->rotateRight();
        QCOMPARE(view->turns(), 0);
        QCOMPARE(view->shownSize(), QSize(40, 30));
    }

    // A part of it selected - Shift and a drag, or a drag with the
    // selection on -, copied (Edit > Copy, Copy as Image), zoomed to;
    // Escape lets go of it.
    void aPartIsSelectedAndCopied()
    {
        ImageDoc* d = open(png);
        ImageView* view = d->view();
        view->setZoom(4.0);
        QWidget* vp = view->viewport();
        const QPoint from = view->toViewport(QPointF(2.5, 3.5)).toPoint(), to = view->toViewport(QPointF(11.5, 7.5)).toPoint();
        mouse(vp, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        mouse(vp, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
        mouse(vp, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        QCOMPARE(view->selection(), QRect(2, 3, 10, 5));
        QVERIFY2(d->selectionText().contains(QStringLiteral("10 × 5")), qPrintable(d->selectionText()));
        grab("selection");
        // Copied: those pixels.
        QApplication::clipboard()->clear();
        app->editCopy->trigger();
        QImage copied = QApplication::clipboard()->image();
        QCOMPARE(copied.size(), QSize(10, 5));
        QCOMPARE(copied.pixelColor(3, 2), QColor(Qt::black));   // the file's 5, 5
        QCOMPARE(copied.pixelColor(0, 0), kRed);
        // Copy as Image: the same.
        QApplication::clipboard()->clear();
        app->editCopyImage->trigger();
        QCOMPARE(QApplication::clipboard()->image().size(), QSize(10, 5));
        // Zoomed to (View > Zoom to Selection).
        app->magSel->trigger();
        QVERIFY(view->zoom() > 20);
        QVERIFY(view->pictureRect().contains(view->toViewport(QPointF(7, 5.5))));
        // Escape: gone - before the window's Escape takes it.
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(view->selection().isEmpty());
        // With the selection mode, a drag without Shift.
        view->setZoom(4.0);
        QToolButton* tool = nullptr;
        for (QToolButton* b : d->findChildren<QToolButton*>())
            if (b->isCheckable() && b->toolTip().startsWith("Select")) tool = b;
        QVERIFY(tool != nullptr);
        tool->click();
        QVERIFY(view->selecting());
        const QPoint a = view->toViewport(QPointF(20.5, 0.5)).toPoint(), b = view->toViewport(QPointF(39.9, 14.5)).toPoint();
        mouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(view->selection(), QRect(20, 0, 20, 15));
        QCOMPARE(d->picture().pixelColor(5, 5), kGreen);
        // A click: no selection.
        mouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseButtonRelease, a, Qt::LeftButton, Qt::NoButton);
        QVERIFY(view->selection().isEmpty());
        tool->click();
        QVERIFY(!view->selecting());
        // Edit > Select All: the whole.
        app->selectAll->trigger();
        QCOMPARE(view->selection(), QRect(0, 0, 40, 30));
        view->clearSelection();
        // Nothing selected: the whole picture copied.
        app->editCopy->trigger();
        QCOMPARE(QApplication::clipboard()->image().size(), QSize(40, 30));
    }

    // An animation: played as it opens, the delays its own; paused, gone
    // through frame by frame (the buttons, Page Up and Down), played again
    // with Space.
    void anAnimationIsPlayed()
    {
        QList<QImage> frames;
        for (const QColor& c : {kRed, kGreen, kBlue}) {
            QImage f(12, 8, QImage::Format_RGB32);
            f.fill(c);
            frames << f;
        }
        const QString file = dir.filePath("ws/blink.gif");
        QVERIFY(writeBytes(file, gif(frames, {60, 60, 60})));
        ImageDoc* d = open(file);
        QVERIFY(d != nullptr);
        QCOMPARE(d->format(), QStringLiteral("gif"));
        QCOMPARE(d->frameCount(), 3);
        QVERIFY(d->isAnimated());
        QVERIFY(d->isPlaying());
        QCOMPARE(d->frameDelay(0), 60);
        QVERIFY(d->frameBar()->isVisible());
        QVERIFY2(d->facts().contains("3 frames, 0.2 s") || d->facts().contains("3 frames, 0,2 s"), qPrintable(d->facts()));
        QSignalSpy changed(d, &ImageDoc::frameChanged);
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 4, 5000);   // round and round
        d->setPlaying(false);
        QVERIFY(!d->isPlaying());
        d->setFrame(0);
        QCOMPARE(d->view()->image().pixelColor(1, 1), kRed);
        QTest::qWait(250);
        QCOMPARE(d->currentFrame(), 0);   // paused
        d->stepFrame(1);
        QCOMPARE(d->currentFrame(), 1);
        QCOMPARE(d->view()->image().pixelColor(1, 1), kGreen);
        d->stepFrame(-2);
        QCOMPARE(d->currentFrame(), 2);   // round
        d->view()->setFocus();
        QTest::keyClick(d->view(), Qt::Key_PageDown);
        QCOMPARE(d->currentFrame(), 0);
        QTest::keyClick(d->view(), Qt::Key_PageUp);
        QCOMPARE(d->currentFrame(), 2);
        QTest::keyClick(d->view(), Qt::Key_Space);
        QVERIFY(d->isPlaying());
        QTest::keyClick(d->view(), Qt::Key_Space);
        QVERIFY(!d->isPlaying());
        grab("animation");
        app->slotFileClose(app->DocumentTab->currentIndex());
    }

    // A file of several pictures - an icon's sizes: its largest first,
    // the others gone to; not an animation.
    void aFileOfSeveralPictures()
    {
        const QByteArray ico = icon({16, 32, 48});
        if (ico.isEmpty()) QSKIP("This Qt writes no icons");
        const QString file = dir.filePath("ws/sizes.ico");
        QVERIFY(writeBytes(file, ico));
        ImageDoc* d = open(file);
        QVERIFY(d != nullptr);
        QCOMPARE(d->frameCount(), 3);
        QVERIFY(!d->isAnimated());
        QCOMPARE(d->imageSize(), QSize(48, 48));
        QCOMPARE(d->currentFrame(), 2);
        QVERIFY2(d->facts().contains("3 pictures in the file"), qPrintable(d->facts()));
        d->stepFrame(1);
        QCOMPARE(d->currentFrame(), 0);
        QCOMPARE(d->imageSize(), QSize(16, 16));
        QVERIFY2(d->facts().contains(QStringLiteral("16 × 16")), qPrintable(d->facts()));
        app->slotFileClose(app->DocumentTab->currentIndex());
    }

    // A photo taken on its side: upright, as its camera said - in the
    // viewer and in the File Browser's look at it.
    void aPhotoIsShownUpright()
    {
        const QByteArray jpeg = sidewaysJpeg();
        QVERIFY(!jpeg.isEmpty());
        const QString file = dir.filePath("ws/sideways.jpg");
        QVERIFY(writeBytes(file, jpeg));
        ImageDoc* d = open(file);
        QVERIFY(d != nullptr);
        QCOMPARE(d->format(), QStringLiteral("jpeg"));
        QCOMPARE(d->imageSize(), QSize(20, 40));
        // Red was on the left: turned clockwise, at the top.
        QVERIFY(near(d->filePicture().pixelColor(10, 5), kRed));
        QVERIFY(near(d->filePicture().pixelColor(10, 35), kBlue));
        QSize size;
        const QImage small = qucs_s::image::thumbnail(file, 10, &size);
        QCOMPARE(size, QSize(20, 40));
        QVERIFY(small.width() <= 10 && small.height() <= 10);
        QVERIFY(small.height() > small.width());
        app->slotFileClose(app->DocumentTab->currentIndex());
    }

    // The File Browser's look at a picture: small, its own size said.
    void thumbnails()
    {
        QSize size;
        QImage small = qucs_s::image::thumbnail(png, 20, &size);
        QCOMPARE(size, QSize(40, 30));
        QCOMPARE(small.size(), QSize(20, 15));
        small = qucs_s::image::thumbnail(svg, 300, &size);
        QCOMPARE(size, QSize(100, 50));
        QCOMPARE(small.size(), QSize(300, 150));   // drawn to fit: sharp at any size
        QVERIFY(qucs_s::image::thumbnail(dir.filePath("ws/none.png"), 20).isNull());
    }

    // The File Browser's Columns view: a picture's preview is the picture,
    // small, its size said.
    void theFileBrowserShowsIt()
    {
        FileBrowser fb;
        fb.resize(760, 500);
        fb.show();
        fb.setLocation(dir.filePath("ws"));
        fb.setView(FileBrowser::View::Columns);
        QTRY_VERIFY_WITH_TIMEOUT(fb.indexOf(png).isValid(), 10000);
        auto* facts = fb.findChild<QLabel*>("fbPreviewFacts");
        auto* picture = fb.findChild<QLabel*>("fbPreviewIcon");
        QVERIFY(facts != nullptr && picture != nullptr);
        fb.selectPath(png);
        QTRY_VERIFY_WITH_TIMEOUT(facts->text().contains(QStringLiteral("40 × 30 pixels")), 10000);
        QCOMPARE(picture->pixmap().deviceIndependentSize(), QSizeF(40, 30));
        const QString large = dir.filePath("ws/large.png");
        QVERIFY(QFileInfo::exists(large));
        fb.selectPath(large);
        QTRY_VERIFY_WITH_TIMEOUT(facts->text().contains(QStringLiteral("4000 × 3000 pixels")), 10000);
        QCOMPARE(picture->pixmap().deviceIndependentSize(), QSizeF(128, 96));
        fb.setView(FileBrowser::View::Details);
    }

    // Written again (a plot made anew): read again, the zoom kept.
    void itIsReadAgainWhenTheFileChanges()
    {
        ImageDoc* d = open(png);
        d->view()->setZoom(3.0);
        QSignalSpy reloaded(d, &ImageDoc::reloaded);
        QTest::qWait(1100);   // a newer time on the file
        QVERIFY(probe(kGreen).save(png));
        QTRY_VERIFY_WITH_TIMEOUT(reloaded.count() >= 1, 10000);
        QCOMPARE(d->view()->zoom(), 3.0);
        QCOMPARE(d->filePicture().pixelColor(1, 1), kGreen);
        // Half written, or no picture any more: why, in its place.
        QTest::qWait(1100);
        QVERIFY(writeBytes(png, "not a picture"));
        QTRY_VERIFY_WITH_TIMEOUT(!d->problem().isEmpty(), 10000);
        QTest::qWait(1100);
        QVERIFY(probe().save(png));
        QTRY_VERIFY_WITH_TIMEOUT(d->problem().isEmpty() && d->filePicture().pixelColor(1, 1) == kRed, 10000);
        QCOMPARE(d->view()->zoom(), 3.0);
    }

    // Deleted, then written again a moment later: still followed. Shown as
    // it was meanwhile, and the status bar says why.
    void itFollowsItsFileThroughADelete()
    {
        ImageDoc* d = open(png);
        QSignalSpy reloaded(d, &ImageDoc::reloaded);
        QVERIFY(QFile::remove(png));
        QTRY_VERIFY_WITH_TIMEOUT(app->statusBar()->currentMessage().contains("was deleted"), 5000);
        QCOMPARE(d->imageSize(), QSize(40, 30));   // as it was
        QTest::qWait(1500);
        QVERIFY(probe(kBlue).save(png));
        QTRY_VERIFY_WITH_TIMEOUT(reloaded.count() >= 1 && d->filePicture().pixelColor(1, 1) == kBlue, 10000);
        QTest::qWait(1100);
        QVERIFY(probe().save(png));
        QTRY_VERIFY_WITH_TIMEOUT(d->filePicture().pixelColor(1, 1) == kRed, 10000);
    }

    // Saved as: a copy of the file under its own kind; converted under
    // another - a JPEG on white -, and the tab shows that; refused as a
    // kind that cannot be written.
    void itIsSavedUnderAnotherName()
    {
        ImageDoc* d = open(png);
        const QString copy = dir.filePath("ws/copy.png");
        QVERIFY(app->saveDocumentAs(d, copy));
        QCOMPARE(readBytes(copy), readBytes(png));
        QCOMPARE(d->getDocName(), QFileInfo(copy).absoluteFilePath());
        QVERIFY(app->saveFile(d));   // nothing to write: fine
        const QString jpeg = dir.filePath("ws/converted.jpg");
        QVERIFY(app->saveDocumentAs(d, jpeg));
        QCOMPARE(QImageReader(jpeg).format(), QByteArray("jpeg"));
        QCOMPARE(d->format(), QStringLiteral("jpeg"));
        QCOMPARE(d->getDocName(), QFileInfo(jpeg).absoluteFilePath());
        QVERIFY(near(d->filePicture().pixelColor(30, 20), Qt::white));   // the transparent quarter, on white
        QVERIFY(near(d->filePicture().pixelColor(1, 1), kRed, 40));
        {
            misc::ErrorCapture capture;
            QVERIFY(!app->saveDocumentAs(d, dir.filePath("ws/vector.svg")));
            QVERIFY(!capture.errors().isEmpty());
            QVERIFY2(capture.errors().first().contains("PNG"), qPrintable(capture.errors().first()));
        }
        QVERIFY(!QFileInfo::exists(dir.filePath("ws/vector.svg")));
        QCOMPARE(d->getDocName(), QFileInfo(jpeg).absoluteFilePath());
        // Back to the first: as itself.
        QVERIFY(app->saveDocumentAs(d, dir.filePath("ws/converted.jpeg")));
        QCOMPARE(readBytes(dir.filePath("ws/converted.jpeg")), readBytes(jpeg));
        app->slotFileClose(app->DocumentTab->currentIndex());
    }

    // No picture: refused, said why, no tab left.
    void aFileThatIsNoPictureIsRefused()
    {
        for (const char* name : {"ws/bad.png", "ws/bad.svg", "ws/bad.gif"}) {
            const QString bad = dir.filePath(QString::fromLatin1(name));
            QVERIFY(writeBytes(bad, "this is not a picture at all\n"));
            const int tabs = app->DocumentTab->count();
            misc::ErrorCapture capture;
            QVERIFY2(!app->gotoPage(bad), name);
            QCOMPARE(app->DocumentTab->count(), tabs);
            QVERIFY(!capture.errors().isEmpty());
            QVERIFY2(capture.errors().first().contains("cannot be read as a picture"), qPrintable(capture.errors().first()));
        }
    }

    // An SVG's text in the text editor, in place of its picture.
    void anSvgIsEditedAsText()
    {
        ImageDoc* d = open(svg);
        QVERIFY(d != nullptr);
        const int tabs = app->DocumentTab->count();
        d->editAsText();
        QTRY_VERIFY(qobject_cast<TextDoc*>(app->DocumentTab->currentWidget()) != nullptr);
        auto* text = qobject_cast<TextDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(text->toPlainText().contains("<svg"));
        QCOMPARE(app->DocumentTab->count(), tabs);
        QCOMPARE(app->findDoc(svg), static_cast<QucsDoc*>(text));
        for (int i = 0; i < app->DocumentTab->count(); ++i)
            if (auto* image = qobject_cast<ImageDoc*>(app->DocumentTab->widget(i))) QVERIFY(image->getDocName() != QFileInfo(svg).absoluteFilePath());
        app->slotFileClose(app->DocumentTab->currentIndex());
        QVERIFY(QucsApp::isImageDocument(open(svg)));
    }

    // Every command that works on the document in front, with a picture
    // there: none may take it for a schematic or a text document.
    void everyCommandLeavesItWhole()
    {
        QVERIFY(app->gotoPage(png));
        auto* closer = new DialogCloser(this);
        const QStringList menus = {"Edit", "Positioning", "Insert", "Simulation", "View", "Python"};
        const QStringList skip = {"Terminal", "Python", "Octave", "Claude", "Full Screen", "Exit", "Quit"};
        QList<QAction*> actions;
        std::function<void(QMenu*)> walk = [&](QMenu* menu) {
            for (QAction* a : menu->actions()) {
                if (a->menu() != nullptr) walk(a->menu());
                else if (!a->isSeparator()) actions << a;
            }
        };
        for (QAction* top : app->menuBar()->actions())
            if (top->menu() != nullptr && menus.contains(QString(top->text()).remove('&'))) walk(top->menu());
        actions << app->fileSave << app->fileSaveAs << app->fileSaveAll << app->fileSettings << app->exportAsImage
                << app->editCopyImage << app->symEdit;
        QVERIFY(actions.size() > 60);
        int used = 0;
        for (QAction* a : std::as_const(actions)) {
            const QString text = QString(a->text()).remove('&');
            if (std::any_of(skip.cbegin(), skip.cend(), [&](const QString& s) { return text.contains(s, Qt::CaseInsensitive); }))
                continue;
            QVERIFY(app->gotoPage(png));
            QVERIFY(QucsApp::isImageDocument(app->DocumentTab->currentWidget()));
            if (!a->isEnabled()) continue;
            a->trigger();
            if (a->isCheckable() && a->isChecked() && a != app->select) a->trigger();   // back off a mode
            ++used;
            QTest::qWait(1);
        }
        QVERIFY(used > 40);
        QVERIFY(app->gotoPage(png));
        ImageDoc* d = doc();
        QVERIFY(d != nullptr);
        QCOMPARE(d->imageSize(), QSize(40, 30));
        QVERIFY(!d->getDocChanged());
        d->view()->viewport()->repaint();
        delete closer;
    }

    // Printed: as large as the page goes, or at its own size; turned as
    // it is shown; an SVG drawn on the paper itself.
    void itIsPrinted()
    {
        ImageDoc* d = open(png);
        d->view()->setTurns(0);
        QPrinter printer(QPrinter::HighResolution);
        QImage page(800, 1000, QImage::Format_RGB32);
        page.fill(Qt::white);
        {
            QPainter painter(&page);
            d->print(&printer, &painter, true, true);
        }
        // Fitted: 800 wide, 600 high, at the top.
        QVERIFY2(near(page.pixelColor(60, 60), kRed), qPrintable(describe(page.pixelColor(60, 60))));
        QVERIFY2(page.pixelColor(110, 110).lightness() < 60, qPrintable(describe(page.pixelColor(110, 110))));   // the file's 5, 5
        QVERIFY(near(page.pixelColor(700, 100), kGreen));
        QVERIFY(near(page.pixelColor(100, 500), kBlue));
        QVERIFY(near(page.pixelColor(400, 800), Qt::white));
        // Its own size: 40 pixels at 96 to the inch on a printer of 1200.
        page.fill(Qt::white);
        {
            QPainter painter(&page);
            d->print(&printer, &painter, true, false);
        }
        const int width = int(std::lround(40 * printer.resolution() / 96.0));
        const int left = (800 - width) / 2;
        QVERIFY(near(page.pixelColor(left + 5, 5), kRed));
        QVERIFY(near(page.pixelColor(left - 5, 5), Qt::white));
        // An SVG, turned.
        ImageDoc* v = open(svg);
        v->rotateRight();
        page.fill(Qt::white);
        {
            QPainter painter(&page);
            v->print(&printer, &painter, true, true);
        }
        // 50 wide shown 100 high: 800 × 1600 would not fit - 500 × 1000.
        QVERIFY(near(page.pixelColor(400, 100), kRed));
        QVERIFY(near(page.pixelColor(400, 900), kBlue));
        v->rotateLeft();
    }

    // Closed like any document.
    void itIsClosed()
    {
        QVERIFY(app->gotoPage(png));
        app->slotFileClose(app->DocumentTab->currentIndex());
        QVERIFY(app->findDoc(png) == nullptr);
    }
};

QTEST_MAIN(TestImageDoc)
#include "test_image_doc.moc"
