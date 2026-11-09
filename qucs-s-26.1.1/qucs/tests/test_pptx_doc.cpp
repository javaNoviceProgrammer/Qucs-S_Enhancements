/*
 * PowerPoint presentations read and drawn (pptx.h), in their tab
 * (pptxdoc.h), and for Claude (read_pptx, show_pptx): decks made here part
 * by part - a theme, a master with its text styles, a layout with its
 * placeholders, slides with shapes, text, pictures, tables, charts, notes
 * - each drawn and looked at pixel by pixel where it should be.
 */
#include <QSplitter>
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "pptx.h"
#include "pptxdoc.h"
#include "menu_owner_close.h"
#include "qucs.h"
#include "settings.h"
#include "qucscontrol.h"
#include "svgguard.h"
#include "zipfile.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif

using namespace qucs_s;
namespace markup = qucs_s::markup;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

const char* const kNs = "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                        "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
                        "xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
                        "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
                        "xmlns:a14=\"http://schemas.microsoft.com/office/drawing/2010/main\" "
                        "xmlns:dsp=\"http://schemas.microsoft.com/office/drawing/2008/diagram\" "
                        "xmlns:dgm=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\"";

QByteArray ns()
{
    return QByteArray(kNs);
}

QByteArray emu(double points)
{
    return QByteArray::number(qint64(std::llround(points * 12700)));
}

/// A shape's position, in points.
QByteArray xfrm(double x, double y, double w, double h, const QByteArray& extra = QByteArray())
{
    return "<a:xfrm" + extra + "><a:off x=\"" + emu(x) + "\" y=\"" + emu(y) + "\"/><a:ext cx=\"" + emu(w) + "\" cy=\"" + emu(h) + "\"/></a:xfrm>";
}

/// A shape: its geometry, fill, line, text and more as given.
QByteArray shape(int id, const QByteArray& spPr, const QByteArray& txBody = QByteArray(), const QByteArray& nvPr = QByteArray(),
                 const QByteArray& style = QByteArray())
{
    return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + QByteArray::number(id) + "\" name=\"s" + QByteArray::number(id) + "\"/><p:cNvSpPr/><p:nvPr>" + nvPr
           + "</p:nvPr></p:nvSpPr><p:spPr>" + spPr + "</p:spPr>" + style + txBody + "</p:sp>";
}

QByteArray text(const QByteArray& paragraphs, const QByteArray& bodyPr = "<a:bodyPr/>")
{
    return "<p:txBody>" + bodyPr + "<a:lstStyle/>" + paragraphs + "</p:txBody>";
}

QByteArray para(const QByteArray& runs, const QByteArray& pPr = QByteArray())
{
    return "<a:p>" + pPr + runs + "</a:p>";
}

QByteArray run(const QString& t, const QByteArray& rPr = QByteArray())
{
    return "<a:r>" + (rPr.isEmpty() ? QByteArray() : rPr) + "<a:t>" + t.toHtmlEscaped().toUtf8() + "</a:t></a:r>";
}

QByteArray solid(const QByteArray& rgb)
{
    return "<a:solidFill><a:srgbClr val=\"" + rgb + "\"/></a:solidFill>";
}

/// A deck: slides of a 960 x 540 point (16:9) presentation, with a theme
/// (accent1 4472C4, accent2 ED7D31; a solid fill of the style's colour
/// first in its fills), a master (a dark-blue title style of 44 points in
/// Arial, a body style of 28 points with "•" bullets) and a layout with a
/// title at (48, 30, 864, 80) and a body at (48, 130, 864, 360).
struct Deck {
    struct SlideSpec {
        QByteArray tree;   // the shapes
        QByteArray attrs;   // the p:sld element's (show="0")
        QByteArray bg;     // a p:bg
        QByteArray rels;   // more relationships
        QString notes;
        QByteArray clrMapOvr;
    };
    QList<SlideSpec> slides;
    QList<zip::Entry> extra;   // media, charts ...
    QByteArray masterTree;     // shapes on the master (beside its placeholders)
    QByteArray masterBg = "<p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg>";
    QByteArray layoutBg;
    QByteArray layoutTree;   // the layout's shapes, in place of its title and body
    QByteArray clrMap = "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
                        "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>";
    QByteArray tableStyles;
    QByteArray effectStyles = "<a:effectStyle><a:effectLst/></a:effectStyle>";   // the theme's
    QByteArray minorFont = "Arial";   // the theme's body font
    QString title;

    QByteArray build() const
    {
        QList<zip::Entry> e;
        const auto add = [&e](const QString& name, const QByteArray& data) { e << zip::Entry{name, data}; };
        add(QStringLiteral("[Content_Types].xml"),
            "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/></Types>");
        add(QStringLiteral("_rels/.rels"),
            "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/></Relationships>");
        if (!title.isEmpty())
            add(QStringLiteral("docProps/core.xml"), "<?xml version=\"1.0\"?><cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                                                     "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + title.toUtf8() + "</dc:title></cp:coreProperties>");
        QByteArray ids, rels;
        for (int i = 0; i < slides.size(); ++i) {
            ids += "<p:sldId id=\"" + QByteArray::number(256 + i) + "\" r:id=\"rIdS" + QByteArray::number(i + 1) + "\"/>";
            rels += "<Relationship Id=\"rIdS" + QByteArray::number(i + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" "
                    "Target=\"slides/slide" + QByteArray::number(i + 1) + ".xml\"/>";
        }
        add(QStringLiteral("ppt/presentation.xml"),
            "<?xml version=\"1.0\"?><p:presentation " + ns() + "><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rIdM\"/></p:sldMasterIdLst>"
            "<p:sldIdLst>" + ids + "</p:sldIdLst><p:sldSz cx=\"12192000\" cy=\"6858000\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
            "<p:defaultTextStyle><a:lvl1pPr><a:defRPr sz=\"1800\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mn-lt\"/></a:defRPr></a:lvl1pPr></p:defaultTextStyle>"
            "</p:presentation>");
        add(QStringLiteral("ppt/_rels/presentation.xml.rels"),
            "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + rels +
            "<Relationship Id=\"rIdM\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>"
            "<Relationship Id=\"rIdT\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/></Relationships>");
        add(QStringLiteral("ppt/theme/theme1.xml"),
            "<?xml version=\"1.0\"?><a:theme " + ns() + " name=\"T\"><a:themeElements><a:clrScheme name=\"C\">"
            "<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
            "<a:dk2><a:srgbClr val=\"1F2A44\"/></a:dk2><a:lt2><a:srgbClr val=\"EEECE1\"/></a:lt2>"
            "<a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1><a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2>"
            "<a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
            "<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5><a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6>"
            "<a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink><a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink></a:clrScheme>"
            "<a:fontScheme name=\"F\"><a:majorFont><a:latin typeface=\"Arial\"/></a:majorFont><a:minorFont><a:latin typeface=\"" + minorFont + "\"/></a:minorFont></a:fontScheme>"
            "<a:fmtScheme name=\"S\"><a:fillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"><a:tint val=\"50000\"/></a:schemeClr></a:solidFill>"
            "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:fillStyleLst>"
            "<a:lnStyleLst><a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln><a:ln w=\"25400\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
            "<a:ln w=\"38100\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln></a:lnStyleLst>"
            "<a:effectStyleLst>" + effectStyles + "</a:effectStyleLst>"
            "<a:bgFillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:bgFillStyleLst>"
            "</a:fmtScheme></a:themeElements></a:theme>");
        add(QStringLiteral("ppt/slideMasters/slideMaster1.xml"),
            "<?xml version=\"1.0\"?><p:sldMaster " + ns() + "><p:cSld>" + masterBg + "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
            + shape(2, xfrm(48, 30, 864, 80) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", text(para(run("Master title style")), "<a:bodyPr anchor=\"b\"/>"), "<p:ph type=\"title\"/>")
            + shape(3, xfrm(48, 130, 864, 360) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", text(para(run("Master body"))), "<p:ph type=\"body\" idx=\"1\"/>")
            + masterTree +
            "</p:spTree></p:cSld>" + clrMap +
            "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rIdL\"/></p:sldLayoutIdLst>"
            "<p:txStyles><p:titleStyle><a:lvl1pPr algn=\"l\"><a:defRPr sz=\"4400\" b=\"1\"><a:solidFill><a:schemeClr val=\"tx2\"/></a:solidFill><a:latin typeface=\"+mj-lt\"/></a:defRPr></a:lvl1pPr></p:titleStyle>"
            "<p:bodyStyle><a:lvl1pPr marL=\"228600\" indent=\"-228600\"><a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/><a:defRPr sz=\"2800\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mn-lt\"/></a:defRPr></a:lvl1pPr>"
            "<a:lvl2pPr marL=\"685800\" indent=\"-228600\"><a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8211;\"/><a:defRPr sz=\"2400\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:defRPr></a:lvl2pPr></p:bodyStyle>"
            "<p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill></a:defRPr></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>");
        add(QStringLiteral("ppt/slideMasters/_rels/slideMaster1.xml.rels"),
            "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rIdL\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
            "<Relationship Id=\"rIdT\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/></Relationships>");
        add(QStringLiteral("ppt/slideLayouts/slideLayout1.xml"),
            "<?xml version=\"1.0\"?><p:sldLayout " + ns() + "><p:cSld>" + layoutBg + "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
            + (layoutTree.isEmpty() ? shape(2, QByteArray(), text(para(run("Click to add title"))), "<p:ph type=\"title\"/>")
                                          + shape(3, QByteArray(), text(para(run("Click to add text"))), "<p:ph idx=\"1\"/>")
                                    : layoutTree) +
            "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>");
        add(QStringLiteral("ppt/slideLayouts/_rels/slideLayout1.xml.rels"),
            "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rIdM\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>");
        if (!tableStyles.isEmpty()) add(QStringLiteral("ppt/tableStyles.xml"), tableStyles);
        for (int i = 0; i < slides.size(); ++i) {
            const SlideSpec& s = slides.at(i);
            const QByteArray n = QByteArray::number(i + 1);
            add(QStringLiteral("ppt/slides/slide%1.xml").arg(i + 1),
                "<?xml version=\"1.0\"?><p:sld " + ns() + " " + s.attrs + "><p:cSld>" + s.bg + "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
                + s.tree + "</p:spTree></p:cSld>" + (s.clrMapOvr.isEmpty() ? QByteArray("<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>") : s.clrMapOvr) + "</p:sld>");
            QByteArray r = "<Relationship Id=\"rIdL\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>" + s.rels;
            if (!s.notes.isEmpty()) {
                r += "<Relationship Id=\"rIdN\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide\" Target=\"../notesSlides/notesSlide" + n + ".xml\"/>";
                QByteArray notesParas;
                for (const QString& line : s.notes.split(QLatin1Char('\n'))) notesParas += para(run(line));
                add(QStringLiteral("ppt/notesSlides/notesSlide%1.xml").arg(i + 1),
                    "<?xml version=\"1.0\"?><p:notes " + ns() + "><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
                    + shape(2, QByteArray(), text(para(run("slide image"))), "<p:ph type=\"sldImg\"/>")
                    + shape(3, QByteArray(), text(notesParas), "<p:ph type=\"body\" idx=\"1\"/>") + "</p:spTree></p:cSld></p:notes>");
            }
            add(QStringLiteral("ppt/slides/_rels/slide%1.xml.rels").arg(i + 1),
                "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + r + "</Relationships>");
        }
        e << extra;
        return zip::write(e);
    }
};

QByteArray png(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

std::shared_ptr<pptx::Presentation> readDeck(const Deck& deck, QString* why = nullptr)
{
    return pptx::Presentation::read(deck.build(), why);
}

/// The colour of slide \a image at \a point (points; the image 1 pixel a
/// point unless \a scale).
QColor at(const QImage& image, QPointF point, qreal scale = 1)
{
    return image.pixelColor(QPoint(int(point.x() * scale), int(point.y() * scale)));
}

/// Of what a slide says it does not draw as PowerPoint does, the deck's
/// own: a font this machine has not got is the machine's, and whether it
/// is said at all depends on the host (CI's Linux runner has no Arial,
/// where macOS does, and the note came before the deck's own).
QStringList ofTheDeck(const QStringList& notShown)
{
    QStringList kept;
    for (const QString& said : notShown)
        if (!said.contains(QStringLiteral("not installed"))) kept << said;
    return kept;
}

bool near(const QColor& a, const QColor& b, int tolerance = 12)
{
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance && std::abs(a.blue() - b.blue()) <= tolerance;
}

/// How many pixels of \a region (points) are far from \a background.
int inked(const QImage& image, const QRectF& region, const QColor& background = Qt::white)
{
    int n = 0;
    const QRect r = region.toRect().intersected(image.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (!near(image.pixelColor(x, y), background, 40)) ++n;
    return n;
}

/// The rows (points from the top) of \a region that have ink, first and last.
std::pair<int, int> inkRows(const QImage& image, const QRectF& region, const QColor& background = Qt::white)
{
    int first = -1, last = -1;
    const QRect r = region.toRect().intersected(image.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (!near(image.pixelColor(x, y), background, 60)) {
                if (first < 0) first = y;
                last = y;
                break;
            }
    return {first, last};
}

/// The memory the process holds now, in MB (0 where it is not known).
long residentMB()
{
#ifdef Q_OS_MACOS
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &count) == KERN_SUCCESS) return long(info.resident_size / (1024 * 1024));
#elif defined(Q_OS_LINUX)
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (statm.open(QIODevice::ReadOnly)) return statm.readAll().split(' ').value(1).toLong() * 4 / 1024;
#endif
    return 0;
}

QString str(const QColor& c)
{
    return c.name();
}

} // namespace

class TestPptxDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");   // (no box: "not found")
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // Which files are presentations.
    // The tab timed on decks of one's own (QUCS_PPTX_BENCH, separated by
    // ';'): opening, the first slide shown, going from slide to slide,
    // zooming, scrolling, the thumbnails, the memory. Each step painted
    // there and then (repaint()), as the screen would show it.
    void benchDecksGiven()
    {
        const QStringList files = qEnvironmentVariable("QUCS_PPTX_BENCH").split(QLatin1Char(';'), Qt::SkipEmptyParts);
        if (files.isEmpty()) QSKIP("QUCS_PPTX_BENCH names decks to time");
        for (const QString& file : files) {
            QucsApp app(false);
            MainGuard guard(&app);
            app.resize(1400, 900);
            app.show();
            QVERIFY(QTest::qWaitForWindowExposed(&app));
            const long before = residentMB();
            QElapsedTimer clock;
            clock.start();
            QVERIFY(app.gotoPage(file));
            const qint64 openMs = clock.elapsed();
            auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
            QVERIFY(doc != nullptr);
            QWidget* port = doc->view()->viewport();
            clock.restart();
            port->repaint();
            const qint64 firstMs = clock.elapsed();
            const long opened = residentMB();
            // The thumbnails: how long till all are drawn, and the longest
            // the event loop was held meanwhile.
            const int n = doc->slideCount();
            QListWidget* thumbs = doc->thumbnails();
            const QImage blank = thumbs->item(n - 1)->icon().pixmap(thumbs->iconSize()).toImage();
            qint64 stall = 0;
            QElapsedTimer beat;
            beat.start();
            QTimer ticker;
            QObject::connect(&ticker, &QTimer::timeout, [&] {
                stall = std::max(stall, beat.elapsed());
                beat.restart();
            });
            ticker.start(5);
            clock.restart();
            while (clock.elapsed() < 120000 && thumbs->item(n - 1)->icon().pixmap(thumbs->iconSize()).toImage() == blank) QTest::qWait(10);
            const qint64 thumbsMs = clock.elapsed();
            ticker.stop();
            // From slide to slide, each new to the view; then back to one seen.
            qint64 switchAll = 0, switchWorst = 0;
            const int steps = std::min(20, n - 1);
            for (int k = 1; k <= steps; ++k) {
                clock.restart();
                doc->goToSlide(k * (n - 1) / std::max(1, steps));
                port->repaint();
                const qint64 ms = clock.elapsed();
                switchAll += ms;
                switchWorst = std::max(switchWorst, ms);
            }
            clock.restart();
            doc->goToSlide(0);
            port->repaint();
            const qint64 backMs = clock.elapsed();
            // On to the next slide, a person's pace apart (as the arrow keys
            // are pressed): 15 steps from a slide not yet shown.
            qint64 paceAll = 0, paceWorst = 0;
            const int paced = std::min(15, n - 1);
            doc->goToSlide(std::max(0, n / 2 - paced));
            port->repaint();
            QTest::qWait(120);
            for (int k = 0; k < paced; ++k) {
                clock.restart();
                doc->nextSlide();
                port->repaint();
                const qint64 ms = clock.elapsed();
                paceAll += ms;
                paceWorst = std::max(paceWorst, ms);
                QTest::qWait(120);
            }
            // Zoomed in step by step to 800 %, then out to 25 %.
            qint64 zoomAll = 0, zoomWorst = 0;
            int zooms = 0;
            doc->goToSlide(std::min(1, n - 1));
            doc->view()->setZoom(1);
            port->repaint();
            for (bool in : {true, false})
                for (int k = 0; k < 7; ++k) {
                    clock.restart();
                    doc->view()->zoomStep(in);
                    port->repaint();
                    const qint64 ms = clock.elapsed();
                    zoomAll += ms;
                    zoomWorst = std::max(zoomWorst, ms);
                    ++zooms;
                }
            // Scrolled at 800 % (what a hand on a trackpad does), and at the
            // width of the view through the deck.
            const auto scroll = [&](int by, int count) {
                qint64 all = 0, worst = 0;
                for (int k = 0; k < count; ++k) {
                    clock.restart();
                    doc->view()->verticalScrollBar()->setValue(doc->view()->verticalScrollBar()->value() + by);
                    port->repaint();
                    const qint64 ms = clock.elapsed();
                    all += ms;
                    worst = std::max(worst, ms);
                }
                return std::make_pair(all, worst);
            };
            doc->goToSlide(std::min(1, n - 1));
            doc->view()->setZoom(8);
            port->repaint();
            const auto [deepAll, deepWorst] = scroll(60, 40);
            doc->view()->setFit(pptx::SlideView::Fit::Width);
            doc->goToSlide(0);
            port->repaint();
            const auto [wideAll, wideWorst] = scroll(150, 60);
            qInfo("%s: %d slides, open %lld ms, first slide %lld ms, slide switch %lld ms average (%lld worst), back %lld ms, next slide at a person's pace %lld ms average (%lld worst), zoom %lld ms average (%lld "
                  "worst), scroll at 800%% %lld ms a step (%lld worst), scroll at fit width %lld ms a step (%lld worst), thumbnails %lld ms (event loop held "
                  "%lld ms at most), memory %ld MB opened, %ld MB at the end, dpr %.1f",
                  qPrintable(QFileInfo(file).fileName()), n, openMs, firstMs, switchAll / std::max(1, steps), switchWorst, backMs, paceAll / std::max(1, paced), paceWorst, zoomAll / zooms, zoomWorst,
                  deepAll / 40, deepWorst, wideAll / 60, wideWorst, thumbsMs, stall, opened - before, residentMB() - before, doc->devicePixelRatioF());
            app.closeAllFiles();
        }
    }

    void presentationFiles()
    {
        for (const char* name : {"a.pptx", "b.PPTX", "c.pptm", "d.ppsx", "e.potx", "f.ppsm", "g.potm"}) QVERIFY2(pptx::isPresentationFile(QString::fromLatin1(name)), name);
        for (const char* name : {"a.ppt", "b.zip", "c.docx", "d.xlsx", "e.pdf", "pptx"}) QVERIFY2(!pptx::isPresentationFile(QString::fromLatin1(name)), name);
        QVERIFY(QucsApp::isPresentationFile("x/y.pptx"));
    }

    // What is not a presentation: said why.
    void notPresentationsSaid()
    {
        QString why;
        QVERIFY(pptx::Presentation::read(QByteArray(), &why) == nullptr);
        QVERIFY2(why.contains("empty"), qPrintable(why));
        QVERIFY(pptx::Presentation::read("not a zip at all", &why) == nullptr);
        QVERIFY(!why.isEmpty());
        // A workbook: a ZIP archive, no presentation in it.
        const QByteArray workbook = zip::write({{QStringLiteral("xl/workbook.xml"), "<workbook/>"},
                                                {QStringLiteral("_rels/.rels"), "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"/>"}});
        QVERIFY(pptx::Presentation::read(workbook, &why) == nullptr);
        QVERIFY2(why.contains("no presentation"), qPrintable(why));
        // One with no slides: read, with none.
        Deck empty;
        const auto deck = readDeck(empty, &why);
        QVERIFY2(deck != nullptr, qPrintable(why));
        QCOMPARE(deck->slideCount(), 0);
        QVERIFY(deck->render(0, 1).isNull());
        QCOMPARE(deck->slide(5).title, QString());
    }

    // The slides in order, their size; a slide's title, text and notes; a
    // hidden one; the file's title; finding in text and notes.
    void slidesTextAndNotesRead()
    {
        Deck d;
        d.title = QStringLiteral("A deck");
        Deck::SlideSpec one;
        one.tree = shape(2, QByteArray(), text(para(run("Hello, phase shifter"))), "<p:ph type=\"title\"/>")
                   + shape(3, QByteArray(), text(para(run("First point")) + para(run("Second point")) + para(QByteArray(), QByteArray())), "<p:ph idx=\"1\"/>");
        one.notes = QStringLiteral("Say this first.\nThen the measurement.");
        Deck::SlideSpec two;
        two.tree = shape(2, xfrm(100, 100, 300, 50) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", text(para(run("A text box"))));
        Deck::SlideSpec three;
        three.attrs = "show=\"0\"";
        three.tree = shape(2, QByteArray(), text(para(run("Hidden backup"))), "<p:ph type=\"title\"/>");
        d.slides << one << two << three;
        QString why;
        const auto deck = readDeck(d, &why);
        QVERIFY2(deck != nullptr, qPrintable(why));
        QCOMPARE(deck->slideCount(), 3);
        QCOMPARE(deck->slideSize(), QSizeF(960, 540));
        QCOMPARE(deck->title(), QStringLiteral("A deck"));
        const pptx::SlideInfo& s1 = deck->slide(0);
        QCOMPARE(s1.title, QStringLiteral("Hello, phase shifter"));
        QCOMPARE(s1.paragraphs, (QStringList{"Hello, phase shifter", "• First point", "• Second point"}));
        QCOMPARE(s1.notes, QStringLiteral("Say this first.\nThen the measurement."));
        QVERIFY(!s1.hidden);
        QCOMPARE(deck->slide(1).title, QString());
        QCOMPARE(deck->slide(1).paragraphs, QStringList{"A text box"});
        QVERIFY(deck->slide(2).hidden);
        QCOMPARE(deck->find(QStringLiteral("POINT")), (QList<std::pair<int, int>>{{0, 2}}));
        QCOMPARE(deck->find(QStringLiteral("measurement")), (QList<std::pair<int, int>>{{0, 1}}));
        QVERIFY(deck->find(QStringLiteral("absent")).isEmpty());
        // Drawn: the title where the layout's placeholder is - the master's,
        // the layout saying nothing - in the master's title style (dark
        // blue, tx2), the text box where it says; nothing of the layout's
        // prompts ("Click to add title").
        const QImage image = deck->render(0, 1);
        QCOMPARE(image.size(), QSize(960, 540));
        QVERIFY(inked(image, QRectF(48, 30, 864, 80)) > 300);
        QVERIFY(inked(image, QRectF(0, 0, 960, 28)) == 0);
        // (Its colour: the darkest ink in the title's box is tx2's 1F2A44.)
        QColor darkest = Qt::white;
        for (int y = 30; y < 110; ++y)
            for (int x = 48; x < 912; ++x)
                if (image.pixelColor(x, y).lightness() < darkest.lightness()) darkest = image.pixelColor(x, y);
        QVERIFY2(near(darkest, QColor(0x1F, 0x2A, 0x44), 20), qPrintable(str(darkest)));
        const QImage two_ = deck->render(1, 1);
        QVERIFY(inked(two_, QRectF(100, 100, 300, 50)) > 50);
        QCOMPARE(inked(two_, QRectF(48, 30, 864, 80).adjusted(0, 0, 0, 0).intersected(QRectF(0, 0, 960, 95))), 0);
        // Twice the scale: twice the pixels.
        QCOMPARE(deck->render(0, 2).size(), QSize(1920, 1080));
    }

    // Shapes: their fills (an RGB, a theme colour with modifiers, the
    // style's), outlines (an ellipse, a triangle by its adjustment, a
    // custom path), turned and flipped, and lines with arrowheads.
    void shapesDrawn()
    {
        Deck d;
        Deck::SlideSpec s;
        s.tree = shape(2, xfrm(50, 50, 200, 100) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF0000"))
                 + shape(3, xfrm(300, 50, 200, 100) + "<a:prstGeom prst=\"ellipse\"><a:avLst/></a:prstGeom><a:solidFill><a:schemeClr val=\"accent1\"><a:lumMod val=\"50000\"/></a:schemeClr></a:solidFill>")
                 // (Its style's fill, the theme's first: the style's colour, accent2.)
                 + shape(4, xfrm(550, 50, 100, 100) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", QByteArray(), QByteArray(),
                         "<p:style><a:lnRef idx=\"0\"><a:schemeClr val=\"accent2\"/></a:lnRef><a:fillRef idx=\"1\"><a:schemeClr val=\"accent2\"/></a:fillRef>"
                         "<a:effectRef idx=\"0\"><a:schemeClr val=\"accent2\"/></a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr val=\"lt1\"/></a:fontRef></p:style>")
                 // A triangle with its apex at the left (adj 0).
                 + shape(5, xfrm(700, 50, 200, 100) + "<a:prstGeom prst=\"triangle\"><a:avLst><a:gd name=\"adj\" fmla=\"val 0\"/></a:avLst></a:prstGeom>" + solid("00AA00"))
                 // Turned a quarter: 40 wide and 200 tall, centred at (150, 300).
                 + shape(6, xfrm(50, 280, 200, 40, " rot=\"5400000\"") + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("0000FF"))
                 // A custom path: a right triangle, its right angle at the bottom left.
                 + shape(7, xfrm(300, 200, 200, 200) + "<a:custGeom><a:pathLst><a:path w=\"100\" h=\"100\"><a:moveTo><a:pt x=\"0\" y=\"0\"/></a:moveTo>"
                                                       "<a:lnTo><a:pt x=\"100\" y=\"100\"/></a:lnTo><a:lnTo><a:pt x=\"0\" y=\"100\"/></a:lnTo><a:close/></a:path></a:pathLst></a:custGeom>" + solid("AA00AA"))
                 // A line with an arrowhead at its end, flipped: from top right to bottom left.
                 + "<p:cxnSp><p:nvCxnSpPr><p:cNvPr id=\"8\" name=\"l\"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>" + xfrm(550, 220, 300, 200, " flipH=\"1\"")
                 + "<a:prstGeom prst=\"straightConnector1\"><a:avLst/></a:prstGeom><a:ln w=\"38100\">" + solid("000000") + "<a:tailEnd type=\"triangle\" w=\"lg\" len=\"lg\"/></a:ln></p:spPr></p:cxnSp>"
                 // Hidden: not drawn.
                 + "<p:sp><p:nvSpPr><p:cNvPr id=\"9\" name=\"h\" hidden=\"1\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>" + xfrm(50, 450, 100, 60)
                 + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF0000") + "</p:spPr></p:sp>"
                 // A kind not drawn exactly: its box, said.
                 + shape(10, xfrm(700, 450, 100, 60) + "<a:prstGeom prst=\"noSmoking\"><a:avLst/></a:prstGeom>" + solid("808080"));
        d.slides << s;
        QString why;
        const auto deck = readDeck(d, &why);
        QVERIFY2(deck != nullptr, qPrintable(why));
        const QImage image = deck->render(0, 1);
        QVERIFY2(near(at(image, {150, 100}), QColor(255, 0, 0)), qPrintable(str(at(image, {150, 100}))));
        // accent1 4472C4, its luminance halved.
        QColor accent(0x44, 0x72, 0xC4);
        float h, sat, l, a;
        accent.getHslF(&h, &sat, &l, &a);
        const QColor darker = QColor::fromHslF(h, sat, l / 2);
        QVERIFY2(near(at(image, {400, 100}), darker), qPrintable(str(at(image, {400, 100})) + " for " + str(darker)));
        QVERIFY(near(at(image, {303, 53}), Qt::white));   // (the ellipse's box's corner: outside it)
        QVERIFY2(near(at(image, {600, 100}), QColor(0xED, 0x7D, 0x31)), qPrintable(str(at(image, {600, 100}))));
        // The triangle: its apex at the left - the top left filled, the top right not.
        QVERIFY2(near(at(image, {705, 70}), QColor(0, 0xAA, 0)), qPrintable(str(at(image, {705, 70}))));
        QVERIFY(near(at(image, {890, 60}), Qt::white));
        // Turned: tall, not wide.
        QVERIFY2(near(at(image, {150, 220}), QColor(0, 0, 255)), qPrintable(str(at(image, {150, 220}))));
        QVERIFY(near(at(image, {60, 300}), Qt::white));
        // The custom triangle: its bottom left filled, its top right not.
        QVERIFY2(near(at(image, {320, 380}), QColor(0xAA, 0, 0xAA)), qPrintable(str(at(image, {320, 380}))));
        QVERIFY(near(at(image, {480, 220}), Qt::white));
        // The line flipped: from (850, 220) to (550, 420), its arrowhead at
        // the end (bottom left) - ink near there, none at the other corners.
        QVERIFY(inked(image, QRectF(550, 400, 25, 20)) > 30);
        QVERIFY2(inked(image, QRectF(550, 220, 30, 30)) == 0, "the line runs from top right to bottom left");
        QVERIFY(inked(image, QRectF(840, 220, 10, 10)) > 0);
        QVERIFY(near(at(image, {100, 480}), Qt::white));   // hidden
        QVERIFY2(near(at(image, {750, 480}), QColor(0x80, 0x80, 0x80)), qPrintable(str(at(image, {750, 480}))));
        QCOMPARE(deck->slide(0).notShown, QStringList{"shapes of the kinds noSmoking, drawn as rectangles"});
    }

    // A group: its children in its own coordinates (chOff, chExt), scaled
    // and placed; a group turned turns them.
    void groupsDrawn()
    {
        Deck d;
        Deck::SlideSpec s;
        // Children space 0..1000 x 0..1000 points, shown at (100, 100) as 200 x 200.
        s.tree = "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"2\" name=\"g\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"" + emu(100) + "\" y=\"" + emu(100)
                 + "\"/><a:ext cx=\"" + emu(200) + "\" cy=\"" + emu(200) + "\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"" + emu(1000) + "\" cy=\"" + emu(1000) + "\"/></a:xfrm></p:grpSpPr>"
                 + shape(3, xfrm(0, 0, 500, 500) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF0000"))
                 + shape(4, xfrm(500, 500, 500, 500) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("0000FF")) + "</p:grpSp>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 1);
        QVERIFY2(near(at(image, {150, 150}), QColor(255, 0, 0)), qPrintable(str(at(image, {150, 150}))));
        QVERIFY2(near(at(image, {250, 250}), QColor(0, 0, 255)), qPrintable(str(at(image, {250, 250}))));
        QVERIFY(near(at(image, {250, 150}), Qt::white));
        QVERIFY(near(at(image, {350, 350}), Qt::white));   // (not drawn at its children's own size)
    }

    // Pictures: drawn in their box, cropped (srcRect), in a rounded outline;
    // one in EMF a box in its place, said; one in SVG drawn.
    void picturesDrawn()
    {
        Deck d;
        QImage halves(100, 50, QImage::Format_RGB32);
        halves.fill(Qt::red);
        {
            QPainter p(&halves);
            p.fillRect(50, 0, 50, 50, Qt::blue);
        }
        d.extra << zip::Entry{QStringLiteral("ppt/media/image1.png"), png(halves)};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image2.emf"), QByteArray(100, '\0')};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image3.svg"),
                              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\" fill=\"#00ff00\"/></svg>"};
        const auto pic = [](int id, const QByteArray& rel, const QByteArray& xf, const QByteArray& src = QByteArray(), const QByteArray& geom = "rect") {
            return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + QByteArray::number(id) + "\" name=\"p\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"" + rel + "\"/>" + src
                   + "<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + xf + "<a:prstGeom prst=\"" + geom + "\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        };
        Deck::SlideSpec s;
        s.tree = pic(2, "rIdP1", xfrm(50, 50, 200, 100))
                 // Its left half cropped away: blue alone, stretched over the box.
                 + pic(3, "rIdP1", xfrm(300, 50, 200, 100), "<a:srcRect l=\"50000\"/>")
                 + pic(4, "rIdP2", xfrm(550, 50, 200, 100))
                 + pic(5, "rIdP3", xfrm(50, 250, 100, 100))
                 + pic(6, "rIdP1", xfrm(300, 250, 200, 200), QByteArray(), "ellipse");
        s.rels = "<Relationship Id=\"rIdP1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image1.png\"/>"
                 "<Relationship Id=\"rIdP2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image2.emf\"/>"
                 "<Relationship Id=\"rIdP3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image3.svg\"/>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 1);
        QVERIFY(near(at(image, {80, 100}), QColor(255, 0, 0)));
        QVERIFY(near(at(image, {220, 100}), QColor(0, 0, 255)));
        QVERIFY2(near(at(image, {320, 100}), QColor(0, 0, 255)), qPrintable(str(at(image, {320, 100}))));
        QVERIFY(near(at(image, {480, 100}), QColor(0, 0, 255)));
        // The EMF: a light box, not the picture. Sampled inside the box and
        // clear of its caption, which is drawn centred: whether the middle
        // pixel falls on a glyph or between two is the host's font's to
        // decide, and CI's Linux runner - which has no Helvetica - read the
        // middle as dark where this machine reads it as paper.
        for (const QPointF& inside : {QPointF(565, 62), QPointF(735, 62), QPointF(565, 138), QPointF(735, 138)}) {
            QVERIFY2(at(image, inside).lightness() > 200, qPrintable(str(at(image, inside))));
            QVERIFY2(!near(at(image, inside), Qt::white, 2), qPrintable(str(at(image, inside))));
        }
        QVERIFY2(near(at(image, {100, 300}), QColor(0, 255, 0)), qPrintable(str(at(image, {100, 300}))));
        // In an ellipse: its middle drawn, its box's corner not.
        QVERIFY(near(at(image, {330, 350}), QColor(255, 0, 0)));
        QVERIFY(near(at(image, {305, 255}), Qt::white));
        QCOMPARE(deck->slide(0).pictures, 5);
        // What the slide does not draw as PowerPoint does holds two kinds of
        // thing: what the deck has and Qucs-S cannot draw, and what this
        // machine has not got (a font). Only the first is the deck's.
        QCOMPARE(ofTheDeck(deck->slide(0).notShown), QStringList{"pictures in EMF, not drawn (a box in their place)"});
    }

    // What a slide does not draw as PowerPoint does is of two kinds: what
    // the deck holds and Qucs-S cannot draw (an EMF picture), and what this
    // machine has not got (a font). They go in one list, the font's first
    // where a title is laid out before a picture - so a reader must look
    // through the list rather than at its head. CI's Linux runner, which
    // has no Arial, is where that came from; this deck asks for a font no
    // machine has, so the two kinds are here whatever the host.
    void whatTheDeckLacksAndWhatThisMachineLacks()
    {
        Deck d;
        d.minorFont = "Nonesuch Sans";
        d.extra << zip::Entry{QStringLiteral("ppt/media/image1.emf"), QByteArray(64, '\0')};
        Deck::SlideSpec s;
        s.tree = shape(2, xfrm(50, 20, 500, 60) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", text(para(run("Results"))))
                 + "<p:pic><p:nvPicPr><p:cNvPr id=\"5\" name=\"p\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rIdP\"/></p:blipFill><p:spPr>"
                 + xfrm(550, 50, 200, 100) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        s.rels = "<Relationship Id=\"rIdP\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image1.emf\"/>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 1);
        const QStringList said = deck->slide(0).notShown;
        // Both are said, and the font's comes first - the title before the
        // picture - which is why the head of the list proves nothing.
        QVERIFY2(said.size() == 2, qPrintable(said.join(QStringLiteral(" | "))));
        QVERIFY2(said.first().contains(QStringLiteral("Nonesuch Sans")) && said.first().contains(QStringLiteral("not installed")),
                 qPrintable(said.join(QStringLiteral(" | "))));
        QCOMPARE(ofTheDeck(said), QStringList{"pictures in EMF, not drawn (a box in their place)"});
        // And the box for it is light whatever font its caption is drawn in.
        for (const QPointF& inside : {QPointF(565, 62), QPointF(735, 62), QPointF(565, 138), QPointF(735, 138)}) {
            QVERIFY2(at(image, inside).lightness() > 200, qPrintable(str(at(image, inside))));
            QVERIFY2(!near(at(image, inside), Qt::white, 2), qPrintable(str(at(image, inside))));
        }
    }

    // Text: the master's body style (28 points, a bullet at marL + indent),
    // a second level's own bullet, numbered paragraphs counting by level, a
    // run's own size and colour, a box shrunk to fit (normAutofit's
    // fontScale), anchored in the middle and at the bottom, a slide number.
    void textFormatted()
    {
        Deck d;
        Deck::SlideSpec s;
        s.tree = shape(2, QByteArray(), text(para(run("Level one")) + para(run("Level two"), "<a:pPr lvl=\"1\"/>")), "<p:ph idx=\"1\"/>")
                 + shape(3, xfrm(500, 130, 400, 200) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                         text(para(run("First"), "<a:pPr marL=\"342900\" indent=\"-342900\"><a:buAutoNum type=\"arabicPeriod\"/></a:pPr>")
                              + para(run("Second"), "<a:pPr marL=\"342900\" indent=\"-342900\"><a:buAutoNum type=\"arabicPeriod\"/></a:pPr>")
                              + para(run("Inner"), "<a:pPr lvl=\"1\" marL=\"742950\" indent=\"-285750\"><a:buAutoNum type=\"alphaLcParenR\"/></a:pPr>")
                              + para(run("Third"), "<a:pPr marL=\"342900\" indent=\"-342900\"><a:buAutoNum type=\"arabicPeriod\"/></a:pPr>")))
                 // The same text, its own size, red: then shrunk to half.
                 + shape(4, xfrm(50, 380, 200, 100) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                         text(para(run("Big", "<a:rPr sz=\"4000\">" + solid("FF0000") + "</a:rPr>")), "<a:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\"/>"))
                 + shape(5, xfrm(300, 380, 200, 100) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                         text(para(run("Big", "<a:rPr sz=\"4000\">" + solid("FF0000") + "</a:rPr>")),
                              "<a:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\"><a:normAutofit fontScale=\"50000\"/></a:bodyPr>"))
                 + shape(6, xfrm(550, 380, 150, 140) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                         text(para(run("Mid", "<a:rPr sz=\"1400\"/>")), "<a:bodyPr anchor=\"ctr\" tIns=\"0\" bIns=\"0\"/>"))
                 + shape(7, xfrm(750, 380, 150, 140) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                         text(para("<a:fld id=\"{1}\" type=\"slidenum\"><a:rPr sz=\"1400\"/><a:t>&#8249;#&#8250;</a:t></a:fld>"), "<a:bodyPr anchor=\"b\" tIns=\"0\" bIns=\"0\"/>"))
                 // A bullet hanging an inch before its text.
                 + shape(8, xfrm(50, 260, 400, 60) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                         text(para(run("Wide"), "<a:pPr marL=\"914400\" indent=\"-914400\"><a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/></a:pPr>"),
                              "<a:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\"/>"));
        Deck::SlideSpec blank;
        d.slides << blank << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const pptx::SlideInfo& info = deck->slide(1);
        QCOMPARE(info.paragraphs, (QStringList{"• Level one", "– Level two", "1. First", "2. Second", "a) Inner", "3. Third", "Big", "Big", "Mid", "2", "• Wide"}));
        const QImage image = deck->render(1, 2);
        // The bullet at marL + indent (0 points: at the box's left inset),
        // the text from marL (18 points) on; 28-point text some 20 points tall.
        const auto [top1, bottom1] = inkRows(image, QRectF(2 * 48, 2 * 130, 2 * 400, 2 * 32));
        QVERIFY2(top1 > 0 && (bottom1 - top1) / 2 > 15 && (bottom1 - top1) / 2 < 32, qPrintable(QString("%1..%2").arg(top1).arg(bottom1)));
        QVERIFY(inked(image, QRectF(2 * (48 + 7.2), top1, 2 * 12, bottom1 - top1)) > 10);   // the bullet
        // An inch between the bullet and its text, nothing between them (the
        // bullet not set before the text, as a character of it).
        QVERIFY(inked(image, QRectF(2 * 50, 2 * 260, 2 * 15, 2 * 40)) > 10);
        QCOMPARE(inked(image, QRectF(2 * 70, 2 * 260, 2 * 45, 2 * 40)), 0);
        QVERIFY(inked(image, QRectF(2 * 122, 2 * 260, 2 * 60, 2 * 40)) > 10);
        // The shrunk text: half as tall.
        const auto [a0, a1] = inkRows(image, QRectF(2 * 50, 2 * 380, 2 * 200, 2 * 100));
        const auto [b0, b1] = inkRows(image, QRectF(2 * 300, 2 * 380, 2 * 200, 2 * 100));
        QVERIFY(a0 >= 0 && b0 >= 0);
        const double ratio = double(b1 - b0) / std::max(1, a1 - a0);
        QVERIFY2(ratio > 0.4 && ratio < 0.6, qPrintable(QString::number(ratio)));
        QVERIFY(near(image.pixelColor(QPoint(2 * 50 + 10, (a0 + a1) / 2)), QColor(255, 0, 0), 60) || inked(image, QRectF(2 * 50, a0, 60, a1 - a0), Qt::white) > 0);
        // Anchored: in the middle, at the bottom.
        const auto [m0, m1] = inkRows(image, QRectF(2 * 550, 2 * 380, 2 * 150, 2 * 140));
        QVERIFY2(std::abs((m0 + m1) / 2 - 2 * 450) < 12, qPrintable(QString("%1..%2").arg(m0).arg(m1)));
        const auto [n0, n1] = inkRows(image, QRectF(2 * 750, 2 * 380, 2 * 150, 2 * 140));
        QVERIFY2(n1 > 2 * 500, qPrintable(QString("%1..%2").arg(n0).arg(n1)));
    }

    // A slide's placeholder where its layout's of the same index is: of two
    // content placeholders (no type, so "obj"), the second's place for idx
    // 2 - not the first of the kind.
    void placeholdersFollowTheirIndex()
    {
        Deck d;
        d.layoutTree = shape(2, QByteArray(), text(para(run("Click to add title"))), "<p:ph type=\"title\"/>")
                       + shape(3, xfrm(48, 130, 420, 360), text(para(run("Left"))), "<p:ph sz=\"half\" idx=\"1\"/>")
                       + shape(4, xfrm(492, 130, 420, 360), text(para(run("Right"))), "<p:ph sz=\"half\" idx=\"2\"/>");
        Deck::SlideSpec s;
        s.tree = shape(3, QByteArray(), text(para(run("On the right"))), "<p:ph sz=\"half\" idx=\"2\"/>");
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        QCOMPARE(deck->slide(0).paragraphs, QStringList{"• On the right"});
        const QImage image = deck->render(0, 1);
        QVERIFY(inked(image, QRectF(492, 130, 420, 60)) > 50);
        QCOMPARE(inked(image, QRectF(0, 120, 480, 380)), 0);
    }

    // Tables: a cell's own fill and lines; the built-in style (Medium Style
    // 2 - Accent 1) where the table names none: its header row accent1, its
    // banded rows tinted; rows as tall as their text; merged cells; the
    // text told row by row.
    void tablesDrawn()
    {
        Deck d;
        const auto cell = [](const QString& t, const QByteArray& tcPr = QByteArray(), const QByteArray& attrs = QByteArray()) {
            return "<a:tc" + attrs + "><a:txBody><a:bodyPr/><a:lstStyle/>" + para(run(t)) + "</a:txBody><a:tcPr" + (tcPr.isEmpty() ? QByteArray("/>") : ">" + tcPr + "</a:tcPr>") + "</a:tc>";
        };
        Deck::SlideSpec s;
        s.tree = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"2\" name=\"t\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" + emu(50) + "\" y=\"" + emu(50)
                 + "\"/><a:ext cx=\"" + emu(400) + "\" cy=\"" + emu(120) + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl>"
                   "<a:tblPr firstRow=\"1\" bandRow=\"1\"/><a:tblGrid><a:gridCol w=\"" + emu(200) + "\"/><a:gridCol w=\"" + emu(200) + "\"/></a:tblGrid>"
                   "<a:tr h=\"" + emu(40) + "\">" + cell("Quantity") + cell("Value") + "</a:tr>"
                   "<a:tr h=\"" + emu(40) + "\">" + cell("Phase") + cell("8.9", solid("00FF00")) + "</a:tr>"
                   "<a:tr h=\"" + emu(40) + "\">" + cell("Merged", QByteArray(), " gridSpan=\"2\"") + cell(QString(), QByteArray(), " hMerge=\"1\"") + "</a:tr>"
                   "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const pptx::SlideInfo& info = deck->slide(0);
        QCOMPARE(info.tables, 1);
        QCOMPARE(info.paragraphs, (QStringList{"Quantity | Value", "Phase | 8.9", "Merged"}));
        const QImage image = deck->render(0, 1);
        // The header: accent1 (4472C4), its text white.
        QVERIFY2(near(at(image, {300, 55}), QColor(0x44, 0x72, 0xC4)), qPrintable(str(at(image, {300, 55}))));
        // The second row: banded (accent1 tinted 40 %), the cell of its own green.
        const QColor band = at(image, {60, 125});
        QVERIFY2(band.blue() > band.red() && band.lightness() > 150, qPrintable(str(band)));
        QVERIFY2(near(at(image, {440, 125}), QColor(0, 255, 0)), qPrintable(str(at(image, {440, 125}))));
        // The third row not banded: lighter (the whole table's tint, 20 %).
        const QColor banded = at(image, {150, 125}), plain = at(image, {420, 165});
        QVERIFY2(banded.lightness() + 8 < plain.lightness(), qPrintable(str(banded) + " " + str(plain)));
        // The merged cell runs the whole width, no line in its middle.
        const QColor left = at(image, {200, 165}), middle = at(image, {250, 165});
        QVERIFY2(near(left, middle, 6), qPrintable(str(left) + " " + str(middle)));
    }

    // Charts from their cached values: a scatter chart's series in their
    // colours, its legend, a log axis; a bar chart's bars; a line chart of
    // categories of several levels; a pie's slices.
    void chartsDrawn()
    {
        const auto chartPart = [](const QByteArray& plot, const QByteArray& axes, const QByteArray& legend = "<c:legend><c:legendPos val=\"r\"/></c:legend>") {
            return "<?xml version=\"1.0\"?><c:chartSpace " + ns() + "><c:chart><c:title><c:tx><c:rich><a:bodyPr/><a:p><a:r><a:t>The chart</a:t></a:r></a:p></c:rich></c:tx></c:title>"
                   "<c:autoTitleDeleted val=\"0\"/><c:plotArea>" + plot + axes + "</c:plotArea>" + legend + "</c:chart></c:chartSpace>";
        };
        const auto nums = [](const QList<double>& v) {
            QByteArray out = "<c:numRef><c:numCache><c:ptCount val=\"" + QByteArray::number(v.size()) + "\"/>";
            for (int i = 0; i < v.size(); ++i) out += "<c:pt idx=\"" + QByteArray::number(i) + "\"><c:v>" + QByteArray::number(v.at(i)) + "</c:v></c:pt>";
            return out + "</c:numCache></c:numRef>";
        };
        const QByteArray scatter =
            "<c:scatterChart><c:scatterStyle val=\"lineMarker\"/>"
            "<c:ser><c:idx val=\"0\"/><c:tx><c:strRef><c:strCache><c:ptCount val=\"1\"/><c:pt idx=\"0\"><c:v>Measured</c:v></c:pt></c:strCache></c:strRef></c:tx>"
            "<c:spPr><a:ln w=\"38100\">" + solid("FF0000") + "</a:ln></c:spPr><c:marker><c:symbol val=\"none\"/></c:marker>"
            "<c:xVal>" + nums({1, 2, 3, 4}) + "</c:xVal><c:yVal>" + nums({1, 10, 100, 1000}) + "</c:yVal></c:ser>"
            "<c:axId val=\"1\"/><c:axId val=\"2\"/></c:scatterChart>";
        const QByteArray valAxes = "<c:valAx><c:axId val=\"1\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"b\"/><c:crossAx val=\"2\"/></c:valAx>"
                                   "<c:valAx><c:axId val=\"2\"/><c:scaling><c:logBase val=\"10\"/><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"l\"/>"
                                   "<c:majorGridlines/><c:crossAx val=\"1\"/></c:valAx>";
        const QByteArray bar =
            "<c:barChart><c:barDir val=\"col\"/><c:grouping val=\"clustered\"/>"
            "<c:ser><c:idx val=\"0\"/><c:spPr>" + solid("0000FF") + "</c:spPr>"
            "<c:cat><c:strRef><c:strCache><c:ptCount val=\"2\"/><c:pt idx=\"0\"><c:v>A</c:v></c:pt><c:pt idx=\"1\"><c:v>B</c:v></c:pt></c:strCache></c:strRef></c:cat>"
            "<c:val>" + nums({4, 8}) + "</c:val></c:ser><c:axId val=\"3\"/><c:axId val=\"4\"/></c:barChart>";
        const QByteArray catAxes = "<c:catAx><c:axId val=\"3\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"b\"/><c:crossAx val=\"4\"/></c:catAx>"
                                   "<c:valAx><c:axId val=\"4\"/><c:scaling><c:orientation val=\"minMax\"/><c:min val=\"0\"/><c:max val=\"10\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"l\"/><c:crossAx val=\"3\"/></c:valAx>";
        const QByteArray line =
            "<c:lineChart><c:grouping val=\"standard\"/><c:ser><c:idx val=\"0\"/><c:spPr><a:ln w=\"28575\">" + solid("00AA00") + "</a:ln></c:spPr>"
            "<c:cat><c:multiLvlStrRef><c:multiLvlStrCache><c:ptCount val=\"3\"/><c:lvl><c:pt idx=\"0\"><c:v>x0</c:v></c:pt><c:pt idx=\"1\"><c:v>x1</c:v></c:pt><c:pt idx=\"2\"><c:v>x2</c:v></c:pt></c:lvl>"
            "</c:multiLvlStrCache></c:multiLvlStrRef></c:cat><c:val>" + nums({1, 2, 3}) + "</c:val></c:ser><c:axId val=\"5\"/><c:axId val=\"6\"/></c:lineChart>";
        const QByteArray lineAxes = "<c:catAx><c:axId val=\"5\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"b\"/><c:crossAx val=\"6\"/></c:catAx>"
                                    "<c:valAx><c:axId val=\"6\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"l\"/><c:crossAx val=\"5\"/></c:valAx>";
        const QByteArray pie = "<c:pieChart><c:varyColors val=\"1\"/><c:ser><c:idx val=\"0\"/><c:val>" + nums({1, 1}) + "</c:val></c:ser></c:pieChart>";
        Deck d;
        const char* parts[] = {"chart1", "chart2", "chart3", "chart4"};
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart1.xml"), chartPart(scatter, valAxes)};
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart2.xml"), chartPart(bar, catAxes, QByteArray())};
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart3.xml"), chartPart(line, lineAxes, QByteArray())};
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart4.xml"), chartPart(pie, QByteArray(), QByteArray())};
        Deck::SlideSpec s;
        const QRectF boxes[] = {QRectF(20, 20, 440, 250), QRectF(500, 20, 440, 250), QRectF(20, 280, 440, 250), QRectF(500, 280, 440, 250)};
        for (int i = 0; i < 4; ++i) {
            s.tree += "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" + QByteArray::number(i + 2) + "\" name=\"c\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\""
                      + emu(boxes[i].x()) + "\" y=\"" + emu(boxes[i].y()) + "\"/><a:ext cx=\"" + emu(boxes[i].width()) + "\" cy=\"" + emu(boxes[i].height())
                      + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart r:id=\"rIdC" + QByteArray::number(i + 1)
                      + "\"/></a:graphicData></a:graphic></p:graphicFrame>";
            s.rels += "<Relationship Id=\"rIdC" + QByteArray::number(i + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" Target=\"../charts/"
                      + QByteArray(parts[i]) + ".xml\"/>";
        }
        d.slides << s;
        // The bar chart again, its bars across (barDir "bar"; its category
        // axis at the left, its values' at the bottom), on a slide of its own.
        const QByteArray across = QByteArray(bar).replace("<c:barDir val=\"col\"/>", "<c:barDir val=\"bar\"/>");
        const QByteArray acrossAxes = QByteArray(catAxes)
                                          .replace("<c:axPos val=\"b\"/><c:crossAx val=\"4\"/>", "<c:axPos val=\"l\"/><c:crossAx val=\"4\"/>")
                                          .replace("<c:axPos val=\"l\"/><c:crossAx val=\"3\"/>", "<c:axPos val=\"b\"/><c:crossAx val=\"3\"/>");
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart5.xml"), chartPart(across, acrossAxes, QByteArray())};
        Deck::SlideSpec s2;
        s2.tree = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"2\" name=\"c\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\""
                  + emu(boxes[1].x()) + "\" y=\"" + emu(boxes[1].y()) + "\"/><a:ext cx=\"" + emu(boxes[1].width()) + "\" cy=\"" + emu(boxes[1].height())
                  + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart r:id=\"rIdC5\"/></a:graphicData></a:graphic></p:graphicFrame>";
        s2.rels = "<Relationship Id=\"rIdC5\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" Target=\"../charts/chart5.xml\"/>";
        d.slides << s2;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        QCOMPARE(deck->slide(0).charts, 4);
        QVERIFY(deck->slide(0).paragraphs.contains("Chart: The chart"));
        const QImage image = deck->render(0, 2);
        const auto count = [&image](const QRectF& box, const QColor& c) {
            int n = 0;
            const QRect r = QRectF(box.x() * 2, box.y() * 2, box.width() * 2, box.height() * 2).toRect();
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x)
                    if (near(image.pixelColor(x, y), c, 30)) ++n;
            return n;
        };
        // The scatter's red line: on a log axis a straight line - red in the
        // plot's middle band, rising from left to right.
        QVERIFY(count(boxes[0], QColor(255, 0, 0)) > 400);
        const QRectF plot0 = boxes[0];
        int leftRow = -1, rightRow = -1;
        for (int y = int(plot0.top() * 2); y < int(plot0.bottom() * 2); ++y) {
            for (int x = int(plot0.left() * 2); x < int(plot0.left() * 2 + plot0.width()); ++x)
                if (near(image.pixelColor(x, y), QColor(255, 0, 0), 30) && leftRow < 0) leftRow = y;
        }
        for (int y = int(plot0.top() * 2); y < int(plot0.bottom() * 2); ++y)
            for (int x = int(plot0.left() * 2 + plot0.width()); x < int(plot0.right() * 2); ++x)
                if (near(image.pixelColor(x, y), QColor(255, 0, 0), 30) && rightRow < 0) rightRow = y;
        QVERIFY2(leftRow > rightRow && rightRow > 0, qPrintable(QString("%1 %2").arg(leftRow).arg(rightRow)));
        // Bars: blue, the second twice the first - upright in the column
        // chart, across in the bar chart: the longest blue run of each column
        // (or row) of the box, a bar's length where it is one.
        QVERIFY(count(boxes[1], QColor(0, 0, 255)) > 2000);
        const auto runs = [](const QImage& im, const QRectF& box, bool rows) {
            QList<int> out;
            const QRect r = QRectF(box.x() * 2, box.y() * 2, box.width() * 2, box.height() * 2).toRect();
            for (int i = rows ? r.top() : r.left(); i <= (rows ? r.bottom() : r.right()); ++i) {
                int best = 0, run = 0;
                for (int j = rows ? r.left() : r.top(); j <= (rows ? r.right() : r.bottom()); ++j) {
                    run = near(rows ? im.pixelColor(j, i) : im.pixelColor(i, j), QColor(0, 0, 255), 30) ? run + 1 : 0;
                    best = std::max(best, run);
                }
                if (best > 0) out << best;
            }
            return out;
        };
        const auto halfAndWhole = [](const QList<int>& lengths) {
            const int most = lengths.isEmpty() ? 0 : *std::max_element(lengths.begin(), lengths.end());
            return most > 0 && std::any_of(lengths.begin(), lengths.end(), [most](int n) { return n > 0.4 * most && n < 0.6 * most; });
        };
        const auto longest = [](const QList<int>& lengths) { return lengths.isEmpty() ? 0 : *std::max_element(lengths.begin(), lengths.end()); };
        QVERIFY(halfAndWhole(runs(image, boxes[1], false)));
        QVERIFY2(longest(runs(image, boxes[1], true)) < 0.3 * 2 * boxes[1].width(), qPrintable(QString::number(longest(runs(image, boxes[1], true)))));
        const QImage bars = deck->render(1, 2);
        QVERIFY(halfAndWhole(runs(bars, boxes[1], true)));
        QVERIFY2(longest(runs(bars, boxes[1], false)) < 0.3 * 2 * boxes[1].height(), qPrintable(QString::number(longest(runs(bars, boxes[1], false)))));
        // The line chart's green; the pie's two accents.
        QVERIFY(count(boxes[2], QColor(0, 0xAA, 0)) > 200);
        QVERIFY(count(boxes[3], QColor(0x44, 0x72, 0xC4)) > 2000);
        QVERIFY(count(boxes[3], QColor(0xED, 0x7D, 0x31)) > 2000);
    }

    // Backgrounds: the slide's own, else the layout's, else the master's
    // (bgRef: the theme's background fill of the colour named); a slide's
    // colour map overriding the master's (bg1 dark): its text light.
    void backgroundsAndColourMaps()
    {
        Deck d;
        d.layoutBg = "<p:bg><p:bgPr>" + solid("00FF00") + "<a:effectLst/></p:bgPr></p:bg>";
        Deck::SlideSpec own;
        own.bg = "<p:bg><p:bgPr>" + solid("102030") + "<a:effectLst/></p:bgPr></p:bg>";
        Deck::SlideSpec layouts;
        Deck::SlideSpec mapped;
        mapped.bg = "<p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg>";
        mapped.clrMapOvr = "<p:clrMapOvr><a:overrideClrMapping bg1=\"dk1\" tx1=\"lt1\" bg2=\"dk2\" tx2=\"lt2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
                           "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/></p:clrMapOvr>";
        mapped.tree = shape(2, xfrm(100, 100, 300, 60) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", text(para(run("Light on dark", "<a:rPr sz=\"4000\"/>"))));
        d.slides << own << layouts << mapped;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        QVERIFY(near(at(deck->render(0, 1), {5, 5}), QColor(0x10, 0x20, 0x30)));
        QVERIFY(near(at(deck->render(1, 1), {5, 5}), QColor(0, 255, 0)));
        const QImage dark = deck->render(2, 1);
        QVERIFY2(near(at(dark, {5, 5}), Qt::black), qPrintable(str(at(dark, {5, 5}))));
        // Its text in tx1 - now lt1, white.
        QColor lightest = Qt::black;
        for (int y = 100; y < 160; ++y)
            for (int x = 100; x < 400; ++x)
                if (dark.pixelColor(x, y).lightness() > lightest.lightness()) lightest = dark.pixelColor(x, y);
        QVERIFY2(lightest.lightness() > 230, qPrintable(str(lightest)));
    }

    // The master's own shapes (a logo bar) under every slide's, unless the
    // slide says not (showMasterSp="0"); its placeholders never drawn.
    void mastersShapesDrawn()
    {
        Deck d;
        d.masterTree = shape(9, xfrm(0, 500, 960, 40) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF00FF"));
        Deck::SlideSpec with, without;
        without.attrs = "showMasterSp=\"0\"";
        d.slides << with << without;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        QVERIFY(near(at(deck->render(0, 1), {480, 520}), QColor(255, 0, 255)));
        QVERIFY(near(at(deck->render(1, 1), {480, 520}), Qt::white));
        // The master's placeholder text ("Master title style") not drawn.
        QCOMPARE(inked(deck->render(0, 1), QRectF(48, 30, 864, 80)), 0);
    }

    // What an older reader takes of mc:AlternateContent: its fallback (an
    // equation's picture), not its choice.
    void alternateContentsFallback()
    {
        Deck d;
        Deck::SlideSpec s;
        s.tree = "<mc:AlternateContent><mc:Choice Requires=\"a14\">" + shape(2, xfrm(100, 100, 200, 100) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF0000"))
                 + "</mc:Choice><mc:Fallback>" + shape(3, xfrm(100, 100, 200, 100) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("0000FF"))
                 + "</mc:Fallback></mc:AlternateContent>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        QVERIFY(near(at(deck->render(0, 1), {200, 150}), QColor(0, 0, 255)));
    }

    // SmartArt: the drawing PowerPoint keeps of it, its shapes in the frame.
    void smartArtDrawn()
    {
        Deck d;
        d.extra << zip::Entry{QStringLiteral("ppt/diagrams/data1.xml"),
                              "<?xml version=\"1.0\"?><dgm:dataModel " + ns() + "><dgm:extLst><a:ext uri=\"x\"><dsp:dataModelExt relId=\"rIdD\" minVer=\"1\"/></a:ext></dgm:extLst></dgm:dataModel>"};
        d.extra << zip::Entry{QStringLiteral("ppt/diagrams/drawing1.xml"),
                              "<?xml version=\"1.0\"?><dsp:drawing " + ns() + "><dsp:spTree><dsp:nvGrpSpPr><dsp:cNvPr id=\"0\" name=\"\"/><dsp:cNvGrpSpPr/></dsp:nvGrpSpPr><dsp:grpSpPr/>"
                              "<dsp:sp modelId=\"{1}\"><dsp:nvSpPr><dsp:cNvPr id=\"0\" name=\"\"/><dsp:cNvSpPr/></dsp:nvSpPr><dsp:spPr>" + xfrm(10, 10, 100, 50)
                              + "<a:prstGeom prst=\"roundRect\"><a:avLst/></a:prstGeom>" + solid("00AAFF") + "</dsp:spPr></dsp:sp></dsp:spTree></dsp:drawing>"};
        Deck::SlideSpec s;
        s.tree = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"2\" name=\"d\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" + emu(200) + "\" y=\"" + emu(200)
                 + "\"/><a:ext cx=\"" + emu(300) + "\" cy=\"" + emu(200) + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\">"
                   "<dgm:relIds r:dm=\"rIdDm\" r:lo=\"rIdLo\" r:qs=\"rIdQs\" r:cs=\"rIdCs\"/></a:graphicData></a:graphic></p:graphicFrame>";
        s.rels = "<Relationship Id=\"rIdDm\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/diagramData\" Target=\"../diagrams/data1.xml\"/>"
                 "<Relationship Id=\"rIdD\" Type=\"http://schemas.microsoft.com/office/2007/relationships/diagramDrawing\" Target=\"../diagrams/drawing1.xml\"/>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        // At the frame's corner plus its own place: (210, 210) to (310, 260).
        const QImage image = deck->render(0, 1);
        QVERIFY2(near(at(image, {260, 235}), QColor(0, 0xAA, 0xFF)), qPrintable(str(at(image, {260, 235}))));
        QVERIFY(near(at(image, {400, 350}), Qt::white));
    }

    // The cloud as PowerPoint draws it (presetShapeDefinitions.xml's arcs,
    // their angles true ones on their ellipses): its notches inside its
    // ellipse white, its bumps outside it filled; a callout's bubbles out
    // to its point; text in the cloud's own box, an ellipse's, a rounded
    // rectangle's - not their whole boxes.
    void cloudsAndTextBoxes()
    {
        Deck d;
        Deck::SlideSpec s;
        const QByteArray noLine = "<a:ln><a:noFill/></a:ln>";
        s.tree = shape(2, xfrm(50, 50, 400, 300) + "<a:prstGeom prst=\"cloud\"><a:avLst/></a:prstGeom>" + solid("FF0000") + noLine)
                 + shape(3, xfrm(520, 40, 200, 150) + "<a:prstGeom prst=\"cloudCallout\"><a:avLst/></a:prstGeom>" + solid("0000FF") + noLine)
                 // Text, no insets: where each shape's text box starts.
                 + shape(4, xfrm(520, 300, 200, 100) + "<a:prstGeom prst=\"ellipse\"><a:avLst/></a:prstGeom>",
                         text(para(run("E", "<a:rPr sz=\"2000\"/>")), "<a:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" anchor=\"t\"/>"))
                 + shape(5, xfrm(750, 300, 200, 100) + "<a:prstGeom prst=\"cloud\"><a:avLst/></a:prstGeom>",
                         text(para(run("C", "<a:rPr sz=\"2000\"/>")), "<a:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" anchor=\"t\"/>"));
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 1);
        // In the ellipse, between two bumps: white; out of it, in a bump: red.
        QVERIFY2(near(at(image, {50 + 272, 50 + 276}), Qt::white), qPrintable(str(at(image, {50 + 272, 50 + 276}))));
        QVERIFY2(near(at(image, {50 + 328, 50 + 20}), QColor(255, 0, 0)), qPrintable(str(at(image, {50 + 328, 50 + 20}))));
        QVERIFY(near(at(image, {250, 200}), QColor(255, 0, 0)));
        // The callout: its smallest bubble at its point (adj1 -20833, adj2
        // 62500: below the box, left of its middle).
        QVERIFY2(near(at(image, {520 + 100 - 0.20833 * 200, 40 + 75 + 0.625 * 150}), QColor(0, 0, 255)),
                 qPrintable(str(at(image, {520 + 100 - 0.20833 * 200, 40 + 75 + 0.625 * 150}))));
        QVERIFY(near(at(image, {520 + 100 - 0.20833 * 200 + 20, 40 + 75 + 0.625 * 150}), Qt::white));
        // Text from the ellipse's box (14.6 % in) and the cloud's (13.8 %).
        const auto firstInk = [&image](const QRectF& r) {
            for (int x = int(r.left()); x <= int(r.right()); ++x)
                for (int y = int(r.top()); y <= int(r.bottom()); ++y)
                    if (!near(image.pixelColor(x, y), Qt::white, 60)) return x;
            return -1;
        };
        const int ellipseText = firstInk(QRectF(520, 300, 200, 60));
        const int cloudText = firstInk(QRectF(750, 300, 200, 60));
        QVERIFY2(ellipseText >= 520 + 0.146 * 200 - 1 && ellipseText < 520 + 0.146 * 200 + 8, qPrintable(QString::number(ellipseText)));
        QVERIFY2(cloudText >= 750 + 0.138 * 200 - 1 && cloudText < 750 + 0.138 * 200 + 8, qPrintable(QString::number(cloudText)));
        for (const QString& n : deck->slide(0).notShown) QVERIFY2(!n.contains("cloud"), qPrintable(n));
        // A custom geometry: a whole circle by one arcTo (its sweep a full
        // turn), and a square path not filled - stroked alone, its corners
        // left white.
        Deck d2;
        Deck::SlideSpec s2;
        s2.tree = shape(2, xfrm(100, 100, 200, 200)
                               + "<a:custGeom><a:pathLst><a:path w=\"100\" h=\"100\"><a:moveTo><a:pt x=\"100\" y=\"50\"/></a:moveTo>"
                                 "<a:arcTo wR=\"50\" hR=\"50\" stAng=\"0\" swAng=\"21600000\"/><a:close/></a:path>"
                                 "<a:path w=\"100\" h=\"100\" fill=\"none\"><a:moveTo><a:pt x=\"0\" y=\"0\"/></a:moveTo><a:lnTo><a:pt x=\"100\" y=\"0\"/></a:lnTo>"
                                 "<a:lnTo><a:pt x=\"100\" y=\"100\"/></a:lnTo><a:lnTo><a:pt x=\"0\" y=\"100\"/></a:lnTo><a:close/></a:path></a:pathLst></a:custGeom>"
                               + solid("00AA00") + "<a:ln w=\"12700\">" + solid("000000") + "</a:ln>");
        d2.slides << s2;
        const auto deck2 = readDeck(d2);
        QVERIFY(deck2 != nullptr);
        const QImage custom = deck2->render(0, 1);
        QVERIFY2(near(at(custom, {200, 200}), QColor(0, 0xAA, 0)), qPrintable(str(at(custom, {200, 200}))));
        QVERIFY2(near(at(custom, {110, 110}), Qt::white), qPrintable(str(at(custom, {110, 110}))));   // (the square's corner, inside it, outside the circle)
        QVERIFY(!near(at(custom, {100, 200}), Qt::white, 60));   // (the square's stroke)
    }

    // Outer shadows: the theme's effect style a shape's style names (soft,
    // under it), none where its own effect list is empty, its own (blue, to
    // its right, not blurred).
    // A paragraph of thousands of runs - a listing with its colours, text
    // pasted from a page - is laid out once, not again at every run: 5,000
    // of them took 36 s a slide before (bug hunt 2026-10-23 A5).
    void manyRunsAreLaidOutOnce()
    {
        Deck d;
        QByteArray runs;
        for (int i = 0; i < 5000; ++i)
            runs += "<a:r><a:rPr b=\"" + QByteArray::number(i % 2) + "\"/><a:t>x</a:t></a:r>";
        Deck::SlideSpec s;
        s.tree = shape(2, xfrm(20, 20, 900, 500) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                       "<p:txBody><a:bodyPr wrap=\"square\"/><a:lstStyle/><a:p>" + runs + "</a:p></p:txBody>");
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        QElapsedTimer clock;
        clock.start();
        const QImage image = deck->render(0, 1);
        const qint64 drawn = clock.elapsed();
        QVERIFY(!image.isNull());
        // Generous: it was 36,000 ms, and is some 140 ms here.
        QVERIFY2(drawn < 10000, qPrintable(QString("%1 ms").arg(drawn)));
        // The text is all there (5,000 x's, in one paragraph).
        QCOMPARE(deck->slide(0).paragraphs.size(), 1);
        QCOMPARE(deck->slide(0).paragraphs.first().size(), 5000);
    }

    // Numbers a deck has no business holding: a chart cache saying it has
    // two thousand million points, a point's index and a paragraph's level
    // past what a whole number holds. Each is held within what is read,
    // and none is converted out of range (bug hunt 2026-10-23 A4 and B5;
    // under UndefinedBehaviorSanitizer this test is the check).
    void absurdNumbersAreHeld()
    {
        Deck d;
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart1.xml"),
                              "<?xml version=\"1.0\"?><c:chartSpace " + ns() + "><c:chart><c:plotArea>"
                              "<c:barChart><c:barDir val=\"col\"/><c:grouping val=\"clustered\"/>"
                              "<c:gapWidth val=\"99999999999\"/><c:overlap val=\"-1e300\"/>"
                              "<c:ser><c:idx val=\"99999999999\"/>"
                              "<c:val><c:numRef><c:numCache><c:ptCount val=\"2000000000\"/>"
                              "<c:pt idx=\"0\"><c:v>4</c:v></c:pt><c:pt idx=\"99999999999\"><c:v>8</c:v></c:pt>"
                              "</c:numCache></c:numRef></c:val></c:ser>"
                              "<c:axId val=\"3\"/><c:axId val=\"4\"/></c:barChart>"
                              "<c:catAx><c:axId val=\"3\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/>"
                              "<c:axPos val=\"b\"/><c:crossAx val=\"4\"/></c:catAx>"
                              "<c:valAx><c:axId val=\"4\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/>"
                              "<c:axPos val=\"l\"/><c:crossAx val=\"3\"/></c:valAx>"
                              "</c:plotArea></c:chart></c:chartSpace>"};
        Deck::SlideSpec s;
        s.rels = "<Relationship Id=\"rIdC1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" "
                 "Target=\"../charts/chart1.xml\"/>";
        s.tree = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"2\" name=\"c\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>"
                 "<p:xfrm><a:off x=\"" + emu(20) + "\" y=\"" + emu(20) + "\"/><a:ext cx=\"" + emu(400) + "\" cy=\"" + emu(200)
                 + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">"
                   "<c:chart r:id=\"rIdC1\"/></a:graphicData></a:graphic></p:graphicFrame>"
                 // A paragraph's level, a table's spans and a run's baseline too.
                 + shape(3, xfrm(500, 20, 400, 200) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                         "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:pPr lvl=\"99999999999\" marL=\"-99999999999\"/>"
                         "<a:r><a:rPr baseline=\"99999999999\"/><a:t>Deep</a:t></a:r></a:p></p:txBody>");
        d.slides << s;
        QString why;
        QElapsedTimer clock;
        clock.start();
        const auto deck = readDeck(d, &why);
        const qint64 absurd = clock.elapsed();
        QVERIFY2(deck != nullptr, qPrintable(why));
        QCOMPARE(deck->slideCount(), 1);
        QVERIFY(!deck->render(0, 1).isNull());
        QVERIFY(deck->slide(0).paragraphs.contains("Deep"));
        // The same deck with three points, to measure this machine by: a
        // cache of two thousand million points is read as a million at
        // most, so it costs about what three do, not the thousands of
        // times more that filling it would (15 s where this is 0.03 s).
        Deck plain = d;
        for (zip::Entry& e : plain.extra)
            if (e.name.endsWith(QLatin1String("chart1.xml"))) e.data.replace("<c:ptCount val=\"2000000000\"/>", "<c:ptCount val=\"3\"/>");
        clock.restart();
        const auto few = readDeck(plain, &why);
        const qint64 three = clock.elapsed();
        QVERIFY2(few != nullptr, qPrintable(why));
        QVERIFY2(absurd <= three * 20 + 2000, qPrintable(QStringLiteral("%1 ms against %2 ms for three points").arg(absurd).arg(three)));
    }

    // A deck is read by walking its elements: one nesting them deeper than
    // any PowerPoint writes is refused, and says so, rather than taking the
    // reader's stack with it (bug hunt 2026-10-23 A2).
    void deepNestingIsRefused()
    {
        const QByteArray group = "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"9\" name=\"g\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
                                 "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"914400\" cy=\"914400\"/>"
                                 "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"914400\" cy=\"914400\"/></a:xfrm></p:grpSpPr>";
        const QByteArray rect = "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>";
        // Groups in groups: as deep as a hand-made deck ever is, and far past it.
        for (const auto [depth, read] : {std::pair{40, true}, std::pair{5000, false}}) {
            Deck d;
            Deck::SlideSpec s;
            for (int i = 0; i < depth; ++i) s.tree += group;
            s.tree += shape(100, xfrm(10, 10, 50, 50) + rect + solid("00AA00"));
            for (int i = 0; i < depth; ++i) s.tree += "</p:grpSp>";
            d.slides << s;
            QString why;
            const auto deck = readDeck(d, &why);
            QCOMPARE(deck != nullptr, read);
            if (read) QVERIFY(!deck->render(0, 1).isNull());
            else QVERIFY2(why.contains("nests its elements more than"), qPrintable(why));
        }
        // Elements of no kind the reader knows, nested the same way: the
        // depth alone decides.
        Deck plain;
        Deck::SlideSpec deep;
        for (int i = 0; i < 400; ++i) deep.tree += "<a:x>";
        for (int i = 0; i < 400; ++i) deep.tree += "</a:x>";
        plain.slides << deep;
        QString why;
        QVERIFY(readDeck(plain, &why) == nullptr);
        QVERIFY2(why.contains("256"), qPrintable(why));
    }

    // An SVG picture whose <use> references ask for more than is drawn
    // here: counted before QtSvg reads it (which expands each reference as
    // it goes), a box in its place, and said (bug hunt 2026-10-23 A3).
    void svgAskingTooMuchIsNotDrawn()
    {
        // Fifteen levels of four: 1.8 kB that would draw 10^9 nodes.
        QByteArray defs = "<rect id=\"r0\" width=\"1\" height=\"1\" fill=\"#ff0000\"/>";
        for (int i = 1; i <= 15; ++i) {
            defs += "<g id=\"r" + QByteArray::number(i) + "\">";
            for (int k = 0; k < 4; ++k) defs += "<use xlink:href=\"#r" + QByteArray::number(i - 1) + "\"/>";
            defs += "</g>";
        }
        const QByteArray bomb = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"100\" height=\"100\">"
                                "<defs>" + defs + "</defs><use xlink:href=\"#r15\"/></svg>";
        QVERIFY(qucs_s::svg::expandsTooFar(bomb));
        // One that names the same square a few times is drawn as it is.
        const QByteArray fine = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"10\" height=\"10\">"
                                "<defs><rect id=\"r\" width=\"10\" height=\"10\" fill=\"#00ff00\"/></defs>"
                                "<use xlink:href=\"#r\"/><use xlink:href=\"#r\"/></svg>";
        QVERIFY(!qucs_s::svg::expandsTooFar(fine));
        // A reference that leads round to itself, and one nothing defines:
        // neither counts (QtSvg stops at those too).
        QVERIFY(!qucs_s::svg::expandsTooFar("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
                                            "<defs><g id=\"a\"><use xlink:href=\"#b\"/></g><g id=\"b\"><use xlink:href=\"#a\"/></g></defs>"
                                            "<use xlink:href=\"#a\"/><use xlink:href=\"#nothing\"/></svg>"));
        QVERIFY(!qucs_s::svg::expandsTooFar("<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>"));
        QVERIFY(!qucs_s::svg::expandsTooFar(QByteArray("\x89PNG\r\n\x1a\n", 8)));
        // The forms QtSvg honours besides <use xlink:href>: SVG 2's plain
        // href, and a prefixed name (<svg:use> expands as <use> does).
        QByteArray plain = bomb, prefixed = bomb;
        plain.replace("xlink:href", "href");
        prefixed.replace("<use", "<svg:use").replace("<g ", "<svg:g ").replace("</g>", "</svg:g>").replace("<defs>", "<svg:defs>")
            .replace("</defs>", "</svg:defs>")
            .replace("<svg ", "<svg xmlns:svg=\"http://www.w3.org/2000/svg\" ");
        QVERIFY(qucs_s::svg::expandsTooFar(plain));
        QVERIFY(qucs_s::svg::expandsTooFar(prefixed));
        // And whatever stands before the root: a comment of its own longer
        // than any window the bytes might be looked at through.
        QVERIFY(qucs_s::svg::expandsTooFar("<!-- " + QByteArray(6000, 'x') + " -->\n" + bomb));
        // Deep enough that the count would run past what a whole number
        // holds if it did not stop at the limit.
        QByteArray far = "<rect id=\"d0\" width=\"1\" height=\"1\"/>";
        for (int i = 1; i <= 40; ++i) {
            far += "<g id=\"d" + QByteArray::number(i) + "\">";
            for (int k = 0; k < 4; ++k) far += "<use xlink:href=\"#d" + QByteArray::number(i - 1) + "\"/>";
            far += "</g>";
        }
        QVERIFY(qucs_s::svg::expandsTooFar("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
                                           "<defs>" + far + "</defs><use xlink:href=\"#d40\"/></svg>"));
        QCOMPARE(qucs_s::svg::nodes("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
                                    "<defs>" + far + "</defs><use xlink:href=\"#d40\"/></svg>"),
                 qucs_s::svg::kMaxNodes);

        // In a deck: whatever the picture is called, since the reader
        // decides by what the bytes are.
        for (const char* name : {"ppt/media/image1.svg", "ppt/media/image1.png"}) {
            Deck d;
            d.extra << zip::Entry{QString::fromLatin1(name), bomb};
            d.extra << zip::Entry{QStringLiteral("ppt/media/image2.svg"), fine};
            Deck::SlideSpec s;
            s.rels = "<Relationship Id=\"rIdP1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
                     "Target=\"../media/" + QByteArray(QFileInfo(QString::fromLatin1(name)).fileName().toLatin1()) + "\"/>"
                     "<Relationship Id=\"rIdP2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
                     "Target=\"../media/image2.svg\"/>";
            s.tree = "<p:pic><p:nvPicPr><p:cNvPr id=\"2\" name=\"p\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rIdP1\"/>"
                     "<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + xfrm(50, 50, 100, 100)
                     + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>"
                       "<p:pic><p:nvPicPr><p:cNvPr id=\"3\" name=\"q\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rIdP2\"/>"
                       "<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + xfrm(300, 50, 100, 100)
                     + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
            d.slides << s;
            QString why;
            QElapsedTimer clock;
            clock.start();
            const auto deck = readDeck(d, &why);
            QVERIFY2(deck != nullptr, qPrintable(why));
            const QImage image = deck->render(0, 1);
            QVERIFY(!image.isNull());
            QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
            const QStringList said = deck->slide(0).notShown;
            QVERIFY2(said.filter("<use>").size() == 1, qPrintable(said.join(" | ") + QLatin1String(" for ") + QLatin1String(name)));
            // The one that is fine is still drawn, in green.
            QVERIFY2(near(at(image, {350, 100}), QColor(0, 0xFF, 0), 10), qPrintable(str(at(image, {350, 100}))));
        }
    }

    // An SVG picture as PowerPoint keeps it: a PNG as the blip's own
    // picture, for readers that know no SVG, and the SVG in the blip's
    // extension list (asvg:svgBlip) - or, for an icon inserted on a Mac,
    // the SVG alone, the blip with no picture of its own at all (which is
    // what the user's deck held: "a picture not in the file" in its place).
    // The SVG is drawn where it can be - sharp at any zoom -, else its PNG.
    void svgPicturesDrawnAsPowerPointKeepsThem()
    {
        const QByteArray halves = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"96\" height=\"96\" viewBox=\"0 0 96 96\">"
                                  "<rect width=\"48\" height=\"96\" fill=\"#ff0000\"/><rect x=\"48\" width=\"48\" height=\"96\" fill=\"#0000ff\"/></svg>";
        QImage grey(96, 96, QImage::Format_RGB32);
        grey.fill(QColor(128, 128, 128));
        // A thin black line down the middle of white, 1 of 100 units wide.
        const QByteArray line = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\"><rect width=\"100\" height=\"100\" fill=\"#ffffff\"/>"
                                "<rect x=\"49.5\" width=\"1\" height=\"100\" fill=\"#000000\"/></svg>";
        // Fifteen levels of four <use>s: too much to draw.
        QByteArray defs = "<rect id=\"r0\" width=\"1\" height=\"1\"/>";
        for (int i = 1; i <= 15; ++i) {
            defs += "<g id=\"r" + QByteArray::number(i) + "\">";
            for (int k = 0; k < 4; ++k) defs += "<use xlink:href=\"#r" + QByteArray::number(i - 1) + "\"/>";
            defs += "</g>";
        }
        const QByteArray bomb = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"100\" height=\"100\">"
                                "<defs>" + defs + "</defs><use xlink:href=\"#r15\"/></svg>";
        Deck d;
        d.extra << zip::Entry{QStringLiteral("ppt/media/image1.png"), png(grey)};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image2.svg"), halves};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image3.svg"), "<svg xmlns='http://www.w3.org/2000/svg'><rect width='10'"};   // cut short
        d.extra << zip::Entry{QStringLiteral("ppt/media/image4.svg"), bomb};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image5.svg"), line};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image6.svg"), "<svg xmlns='http://www.w3.org/2000/svg' width='0' height='0'/>"};
        const auto svgBlip = [](const QByteArray& rel) {
            return "<a:extLst><a:ext uri=\"{96DAC541-7B7A-43D3-8B79-37D633B846F1}\"><asvg:svgBlip "
                   "xmlns:asvg=\"http://schemas.microsoft.com/office/drawing/2016/SVG/main\" r:embed=\"" + rel + "\"/></a:ext></a:extLst>";
        };
        // A picture whose blip has its own picture \a own (none: empty) and
        // the SVG \a svg in its extensions (none: empty).
        const auto pic = [&svgBlip](int id, const QByteArray& own, const QByteArray& svg, const QByteArray& xf, const QByteArray& src = QByteArray()) {
            const QByteArray blip = "<a:blip" + (own.isEmpty() ? QByteArray() : " r:embed=\"" + own + "\"") + ">" + (svg.isEmpty() ? QByteArray() : svgBlip(svg)) + "</a:blip>";
            return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + QByteArray::number(id) + "\" name=\"Graphic " + QByteArray::number(id) + "\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>"
                   "<p:blipFill>" + blip + src + "<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + xf
                   + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        };
        const QByteArray rels = "<Relationship Id=\"rPng\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image1.png\"/>"
                                "<Relationship Id=\"rSvg\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image2.svg\"/>"
                                "<Relationship Id=\"rCut\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image3.svg\"/>"
                                "<Relationship Id=\"rBomb\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image4.svg\"/>"
                                "<Relationship Id=\"rLine\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image5.svg\"/>"
                                "<Relationship Id=\"rEmpty\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image6.svg\"/>"
                                "<Relationship Id=\"rGone\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image9.svg\"/>"
                                "<Relationship Id=\"rFar\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"http://example.invalid/a.svg\" TargetMode=\"External\"/>";
        Deck::SlideSpec one;
        one.rels = rels;
        one.tree = pic(2, "rPng", "rSvg", xfrm(20, 20, 100, 100))            // PowerPoint 365: the SVG over its PNG
                   + pic(3, QByteArray(), "rSvg", xfrm(140, 20, 100, 100))   // PowerPoint for Mac's icon: the SVG alone
                   + pic(4, "rSvg", QByteArray(), xfrm(260, 20, 100, 100))   // another tool's: the SVG as the blip's own
                   + pic(5, "rPng", "rCut", xfrm(380, 20, 100, 100))         // an SVG cut short: its PNG
                   + pic(6, "rPng", "rBomb", xfrm(500, 20, 100, 100))        // one asking too much: its PNG
                   + pic(7, "rPng", "rGone", xfrm(620, 20, 100, 100))        // one not in the file: its PNG
                   + pic(8, "rPng", "rFar", xfrm(740, 20, 100, 100))         // one linked from outside: its PNG
                   + pic(9, "rPng", "rEmpty", xfrm(20, 140, 100, 100))       // one of no size: its PNG
                   + pic(10, "rPng", "rSvg", xfrm(140, 140, 100, 100), "<a:srcRect l=\"50000\"/>");   // cropped: its right half
        Deck::SlideSpec two;
        two.rels = rels;
        two.tree = pic(2, QByteArray(), "rCut", xfrm(20, 20, 100, 100))       // an SVG cut short, alone: a box
                   + pic(3, QByteArray(), "rBomb", xfrm(140, 20, 100, 100));  // asking too much, alone: a box, and why
        Deck::SlideSpec three;
        three.rels = rels;
        three.tree = pic(2, "rPng", "rLine", xfrm(0, 0, 100, 100));
        // As a shape's picture fill, clipped to its ellipse, and as the
        // slide's background.
        Deck::SlideSpec four;
        four.rels = rels;
        four.bg = "<p:bg><p:bgPr><a:blipFill><a:blip>" + svgBlip("rSvg") + "</a:blip><a:stretch><a:fillRect/></a:stretch></a:blipFill><a:effectLst/></p:bgPr></p:bg>";
        four.tree = shape(2, xfrm(300, 100, 200, 200) + "<a:prstGeom prst=\"ellipse\"><a:avLst/></a:prstGeom><a:blipFill><a:blip r:embed=\"rPng\">" + svgBlip("rLine")
                                 + "</a:blip><a:stretch><a:fillRect/></a:stretch></a:blipFill>");
        d.slides << one << two << three << four;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 1);
        const QColor red(255, 0, 0), blue(0, 0, 255), greyed(128, 128, 128);
        for (const qreal x : {20.0, 140.0, 260.0}) {
            QVERIFY2(near(at(image, {x + 20, 70}), red), qPrintable(QStringLiteral("at %1: %2").arg(x).arg(str(at(image, {x + 20, 70})))));
            QVERIFY2(near(at(image, {x + 80, 70}), blue), qPrintable(QStringLiteral("at %1: %2").arg(x).arg(str(at(image, {x + 80, 70})))));
        }
        for (const qreal x : {380.0, 500.0, 620.0, 740.0})
            QVERIFY2(near(at(image, {x + 50, 70}), greyed), qPrintable(QStringLiteral("at %1: %2").arg(x).arg(str(at(image, {x + 50, 70})))));
        QVERIFY2(near(at(image, {70, 190}), greyed), qPrintable(str(at(image, {70, 190}))));
        // Cropped to its right half, stretched over the box: blue throughout.
        QVERIFY2(near(at(image, {150, 190}), blue), qPrintable(str(at(image, {150, 190}))));
        QVERIFY2(near(at(image, {230, 190}), blue), qPrintable(str(at(image, {230, 190}))));
        QCOMPARE(deck->slide(0).pictures, 9);
        QCOMPARE(ofTheDeck(deck->slide(0).notShown), QStringList());
        // Alone and not drawable: a box, and what it is.
        const QImage boxes = deck->render(1, 1);
        QVERIFY(!near(at(boxes, {25, 25}), Qt::white, 2) && at(boxes, {25, 25}).lightness() > 200);
        const QStringList said = ofTheDeck(deck->slide(1).notShown);
        QVERIFY2(said.join(QLatin1Char('|')).contains(QStringLiteral("pictures in SVG, not drawn")), qPrintable(said.join(QLatin1Char('|'))));
        QVERIFY2(said.join(QLatin1Char('|')).contains(QStringLiteral("an SVG picture is not drawn")), qPrintable(said.join(QLatin1Char('|'))));
        // Sharp at any zoom: at 8 pixels a point the line is 8 pixels wide,
        // black in its middle and white 8 pixels off it - drawn from the
        // SVG, not the grey PNG scaled up.
        const QImage sharp = deck->render(2, 8);
        QVERIFY2(near(at(sharp, {50, 50}, 8), Qt::black, 20), qPrintable(str(at(sharp, {50, 50}, 8))));
        QVERIFY2(near(sharp.pixelColor(400 - 9, 400), Qt::white, 20), qPrintable(str(sharp.pixelColor(400 - 9, 400))));
        QVERIFY2(near(sharp.pixelColor(400 + 9, 400), Qt::white, 20), qPrintable(str(sharp.pixelColor(400 + 9, 400))));
        // Where a slide is drawn in tiles, the line where it is.
        const QImage tile = deck->render(2, 8, QRect(380, 380, 40, 40));
        QCOMPARE(tile.size(), QSize(40, 40));
        QVERIFY2(near(tile.pixelColor(20, 20), Qt::black, 20), qPrintable(str(tile.pixelColor(20, 20))));
        QVERIFY(near(tile.pixelColor(2, 20), Qt::white, 20));
        // The fill: the line in the ellipse, the background's halves round it.
        const QImage filled = deck->render(3, 1);
        QVERIFY2(near(at(filled, {400, 200}), Qt::black, 60), qPrintable(str(at(filled, {400, 200}))));
        QVERIFY2(near(at(filled, {350, 200}), Qt::white, 20), qPrintable(str(at(filled, {350, 200}))));
        QVERIFY2(near(at(filled, {305, 105}), red), qPrintable(str(at(filled, {305, 105}))));   // the ellipse's corner: the background
        QVERIFY2(near(at(filled, {900, 500}), blue), qPrintable(str(at(filled, {900, 500}))));
    }

    // Pictures are decoded when drawn, no larger than drawn: a slide drawn
    // small and then large gets its picture right at both; a picture whose
    // header reads but whose pixels do not is a light box, not a crash.
    void picturesDecodedAsLargeAsDrawn()
    {
        QImage quarters(1600, 1200, QImage::Format_RGB32);
        quarters.fill(Qt::red);
        {
            QPainter p(&quarters);
            p.fillRect(800, 0, 800, 600, Qt::blue);
            p.fillRect(0, 600, 800, 600, Qt::green);
            p.fillRect(800, 600, 800, 600, Qt::black);
        }
        QByteArray broken = png(quarters);
        broken.truncate(200);   // its header whole, its pixels cut off
        QByteArray jpeg;
        {
            QBuffer buffer(&jpeg);
            buffer.open(QIODevice::WriteOnly);
            quarters.save(&buffer, "JPEG", 90);
        }
        Deck d;
        d.extra << zip::Entry{QStringLiteral("ppt/media/image1.png"), png(quarters)};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image2.png"), broken};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image3.jpeg"), jpeg};
        const auto pic = [](int id, const QByteArray& rel, const QByteArray& xf) {
            return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + QByteArray::number(id) + "\" name=\"p\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"" + rel
                   + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + xf + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        };
        Deck::SlideSpec s;
        s.tree = pic(2, "rP1", xfrm(40, 40, 400, 300)) + pic(3, "rP2", xfrm(480, 40, 200, 150)) + pic(4, "rP3", xfrm(500, 300, 400, 200));
        s.rels = "<Relationship Id=\"rP1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image1.png\"/>"
                 "<Relationship Id=\"rP2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image2.png\"/>"
                 "<Relationship Id=\"rP3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image3.jpeg\"/>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        for (const qreal scale : {0.1, 0.5, 1.0, 4.0, 0.25}) {
            const QImage image = deck->render(0, scale);
            QVERIFY(!image.isNull());
            const struct {
                QPointF at;
                QColor colour;
            } expected[] = {{{140, 115}, Qt::red}, {{340, 115}, Qt::blue}, {{140, 265}, Qt::green}, {{340, 265}, Qt::black},
                            {{600, 350}, Qt::red}, {{800, 350}, Qt::blue}, {{600, 450}, Qt::green}, {{800, 450}, Qt::black}};
            for (const auto& e : expected)
                QVERIFY2(near(at(image, e.at, scale), e.colour, 40),
                         qPrintable(QStringLiteral("at %1, %2 at scale %3: %4").arg(e.at.x()).arg(e.at.y()).arg(scale).arg(str(at(image, e.at, scale)))));
            // The broken one: a light box (whatever its header promised).
            QVERIFY2(at(image, {580, 115}, scale).lightness() > 200, qPrintable(str(at(image, {580, 115}, scale))));
            // Decoded large enough for the scale: the edge between red and
            // blue (240 points across) sharp within a few pixels of it.
            const int edge = int(240 * scale), row = int(115 * scale);
            QVERIFY2(near(image.pixelColor(edge - 4, row), Qt::red, 40), qPrintable(QStringLiteral("scale %1: %2").arg(scale).arg(str(image.pixelColor(edge - 4, row)))));
            QVERIFY2(near(image.pixelColor(edge + 4, row), Qt::blue, 40), qPrintable(QStringLiteral("scale %1: %2").arg(scale).arg(str(image.pixelColor(edge + 4, row)))));
        }
        // Drawn on another thread at the same time as here (the tab decodes
        // the next slides' pictures so), the same pictures come out.
        const QImage alone = deck->render(0, 2);
        std::atomic<bool> stop{false};
        QThreadPool pool;
        for (int t = 0; t < 4; ++t)
            pool.start([&deck, &stop, t] {
                for (int k = 0; !stop && k < 40; ++k) deck->prepare(0, 0.1 + 0.37 * ((k + t) % 9));
            });
        for (int k = 0; k < 10; ++k) QCOMPARE(deck->render(0, 2), alone);
        stop = true;
        pool.waitForDone();
    }

    // A part of a slide drawn alone (a tile) is that part of the slide
    // drawn whole; and what a tile leaves out is left out.
    void aTileIsThatPartOfTheSlide()
    {
        Deck d;
        Deck::SlideSpec s;
        s.tree = shape(2, xfrm(100, 100, 300, 200) + "<a:prstGeom prst=\"ellipse\"><a:avLst/></a:prstGeom>" + solid("FF0000"), text(para(run("A title, long enough to wrap"))))
                 + shape(3, xfrm(600, 50, 300, 400) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("00AA00"))
                 + shape(4, xfrm(50, 400, 860, 2) + "<a:prstGeom prst=\"line\"><a:avLst/></a:prstGeom><a:ln w=\"38100\"><a:solidFill><a:srgbClr val=\"0000FF\"/></a:solidFill>"
                               "<a:tailEnd type=\"triangle\"/></a:ln>");
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        QCOMPARE(deck->pixels(2), QSize(1920, 1080));
        QCOMPARE(deck->pixels(0.001), QSize(1, 1));
        QVERIFY(!deck->pixels(0).isValid());
        const QImage whole = deck->render(0, 2);
        for (const QRect& part : {QRect(0, 0, 512, 512), QRect(512, 0, 512, 512), QRect(1024, 512, 512, 512), QRect(1536, 512, 512, 512), QRect(100, 700, 1700, 200)}) {
            const QImage tile = deck->render(0, 2, part);
            const QRect kept = part & whole.rect();
            QCOMPARE(tile.size(), kept.size());
            int differ = 0;
            for (int y = 0; y < kept.height(); y += 3)
                for (int x = 0; x < kept.width(); x += 3)
                    if (!near(tile.pixelColor(x, y), whole.pixelColor(kept.x() + x, kept.y() + y), 24)) ++differ;
            // (Antialiased edges may differ by a hair where a tile's own
            // clip cuts them.)
            QVERIFY2(differ < kept.width() * kept.height() / 9 / 200, qPrintable(QStringLiteral("%1 pixels differ in %2,%3").arg(differ).arg(part.x()).arg(part.y())));
        }
        QVERIFY(deck->render(0, 2, QRect(5000, 5000, 10, 10)).isNull());
        QVERIFY(deck->render(0, 2, QRect()).isNull());
        QVERIFY(deck->render(7, 2, QRect(0, 0, 10, 10)).isNull());
    }

    // The layouts of text are kept for the slides drawn last, the others'
    // let go and made again: a slide drawn after many others comes out
    // the same, and so do its parts.
    void textLaidOutAgainComesOutTheSame()
    {
        Deck d;
        for (int i = 0; i < 40; ++i) {
            Deck::SlideSpec s;
            for (int k = 0; k < 150; ++k)
                s.tree += shape(10 + k, xfrm((k % 15) * 62 + 8, (k / 15) * 52 + 10, 58, 46) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>",
                                text(para(run(QStringLiteral("s%1 k%2 words to wrap").arg(i).arg(k)))));
            d.slides << s;
        }
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage first = deck->render(0, 1);
        const QList<pptx::SlidePart> parts = deck->parts(0);
        for (int i = 1; i < 40; ++i) (void)deck->render(i, 1);
        for (int i = 1; i < 40; ++i) (void)deck->render(i, 0.1);   // (thumbnails)
        QCOMPARE(deck->render(0, 1), first);
        const QList<pptx::SlidePart> again = deck->parts(0);
        QCOMPARE(again.size(), parts.size());
        for (int k = 0; k < parts.size(); ++k) {
            QCOMPARE(again.at(k).text, parts.at(k).text);
            QCOMPARE(again.at(k).outline, parts.at(k).outline);
        }
    }

    void shadowsDrawn()
    {
        Deck d;
        d.effectStyles = "<a:effectStyle><a:effectLst/></a:effectStyle>"
                         "<a:effectStyle><a:effectLst><a:outerShdw blurRad=\"40000\" dist=\"23000\" dir=\"5400000\" rotWithShape=\"0\"><a:srgbClr val=\"000000\"><a:alpha val=\"35000\"/></a:srgbClr></a:outerShdw></a:effectLst></a:effectStyle>"
                         "<a:effectStyle><a:effectLst/></a:effectStyle>";
        const QByteArray style = "<p:style><a:lnRef idx=\"0\"><a:schemeClr val=\"accent1\"/></a:lnRef><a:fillRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:fillRef>"
                                 "<a:effectRef idx=\"2\"><a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr val=\"lt1\"/></a:fontRef></p:style>";
        const QByteArray rect = "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>";
        Deck::SlideSpec s;
        s.tree = shape(2, xfrm(100, 100, 200, 100) + rect, QByteArray(), QByteArray(), style)
                 + shape(3, xfrm(400, 100, 200, 100) + rect + "<a:effectLst/>", QByteArray(), QByteArray(), style)
                 + shape(4, xfrm(700, 100, 100, 100) + rect + solid("00AA00")
                                + "<a:effectLst><a:outerShdw dist=\"127000\" dir=\"0\"><a:srgbClr val=\"0000FF\"/></a:outerShdw></a:effectLst>");
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 2);
        const QColor under = at(image, {200, 201}, 2), clear = at(image, {200, 212}, 2);
        QVERIFY2(under.lightness() < 240 && under.lightness() > 120, qPrintable(str(under)));
        QVERIFY2(near(clear, Qt::white, 3), qPrintable(str(clear)));
        // Soft: past its edge (the shape's offset by 1.8 points) still a little dark.
        const QColor fading = at(image, {200, 202.5}, 2);
        QVERIFY2(fading.lightness() < 252 && fading.lightness() > under.lightness(), qPrintable(str(fading) + " " + str(under)));
        QVERIFY2(near(at(image, {500, 201}, 2), Qt::white, 3), qPrintable(str(at(image, {500, 201}, 2))));
        QVERIFY2(near(at(image, {805, 150}, 2), QColor(0, 0, 255), 10), qPrintable(str(at(image, {805, 150}, 2))));
        QVERIFY(near(at(image, {813, 150}, 2), Qt::white, 3));
        QVERIFY(near(at(image, {750, 150}, 2), QColor(0, 0xAA, 0), 10));   // (the shape over its shadow)
    }

    // A shadow of a shape as large as the slide, drawn where its mask
    // would be more pixels than are worth holding: it is built smaller and
    // drawn scaled up, not left out altogether (bug hunt 2026-10-23 B1 -
    // a slide's own shape lost its shadow at 8 pixels a point).
    void aBigShadowIsStillDrawn()
    {
        Deck d;
        Deck::SlideSpec s;
        // Nearly the whole of the 960 x 540 slide, its shadow 30 points
        // down and to the right, in red so it is told from the shape. At
        // 7 pixels a point its silhouette is 21 million pixels, past what
        // a mask is allowed, so the picture built smaller is what is drawn.
        s.tree = shape(2, xfrm(10, 10, 900, 480) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("1F4E9C")
                              + "<a:effectLst><a:outerShdw blurRad=\"0\" dist=\"381000\" dir=\"2700000\" algn=\"tl\" rotWithShape=\"0\">"
                                "<a:srgbClr val=\"FF0000\"/></a:outerShdw></a:effectLst>");
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        // In the shadow, past the shape's corner at 910, 490.
        const QPointF inShadow(920, 500), inShape(400, 200);
        for (const qreal scale : {1.0, 4.0, 7.0}) {
            const QImage image = deck->render(0, scale);
            QVERIFY(!image.isNull());
            // At 7 pixels a point the mask would be 21 million pixels,
            // past what is held: built smaller, and still drawn.
            const QColor shadow = at(image, inShadow, scale);
            const QColor shape = at(image, inShape, scale);
            QVERIFY2(near(shadow, QColor(0xFF, 0, 0), 24), qPrintable(QStringLiteral("at %1: %2").arg(scale).arg(str(shadow))));
            QVERIFY2(near(shape, QColor(0x1F, 0x4E, 0x9C), 10), qPrintable(QStringLiteral("at %1: %2").arg(scale).arg(str(shape))));
        }
    }

    // A font not installed: drawn by a stand-in as wide as the font asked
    // in each style (Calibri's widths measured on Calibri), its lines as far
    // apart, and said; one whose widths are not known said so.
    void fontsStandIn()
    {
        const QString sample = QStringLiteral("The quick brown fox jumps over the lazy dog. Measured loss 3.2 dB at 1550 nm; ABCDEFGHIJ 0123456789");
        Deck d;
        Deck::SlideSpec s;
        const QByteArray noInsets = "<a:bodyPr wrap=\"none\" lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" anchor=\"t\"/>";
        const QByteArray rect = "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>";
        s.tree = shape(2, xfrm(20, 40, 920, 40) + rect, text(para(run(sample, "<a:rPr sz=\"2000\"><a:latin typeface=\"Calibri\"/></a:rPr>")), noInsets))
                 + shape(3, xfrm(20, 100, 920, 40) + rect, text(para(run(sample, "<a:rPr sz=\"2000\" i=\"1\"><a:latin typeface=\"Calibri\"/></a:rPr>")), noInsets))
                 + shape(4, xfrm(20, 160, 300, 200) + rect,
                         text("<a:p>" + run("Hxg", "<a:rPr sz=\"4000\"><a:latin typeface=\"Calibri\"/></a:rPr>") + "<a:br><a:rPr sz=\"4000\"><a:latin typeface=\"Calibri\"/></a:rPr></a:br>"
                                  + run("Hxg", "<a:rPr sz=\"4000\"><a:latin typeface=\"Calibri\"/></a:rPr>") + "</a:p>",
                              noInsets))
                 + shape(5, xfrm(400, 160, 300, 60) + rect, text(para(run("Unknown", "<a:rPr sz=\"2000\"><a:latin typeface=\"Qucs Missing Font\"/></a:rPr>")), noInsets));
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 1);
        // Its width: Calibri's, 42.83 ems of 20 points (italic 42.66).
        const auto inkSpan = [&image](const QRectF& r) {
            int lo = -1, hi = -1;
            for (int x = int(r.left()); x <= int(r.right()); ++x)
                for (int y = int(r.top()); y <= int(r.bottom()); ++y)
                    if (!near(image.pixelColor(x, y), Qt::white, 60)) {
                        if (lo < 0) lo = x;
                        hi = x;
                        break;
                    }
            return hi - lo;
        };
        const int regular = inkSpan(QRectF(20, 40, 930, 35)), italic = inkSpan(QRectF(20, 100, 930, 35));
        QVERIFY2(std::abs(regular - 42.832 * 20) < 0.03 * 42.832 * 20, qPrintable(QString::number(regular)));
        QVERIFY2(std::abs(italic - 42.659 * 20) < 0.03 * 42.659 * 20, qPrintable(QString::number(italic)));
        // Its lines 1.221 ems apart (the H's tops).
        QList<int> tops;
        bool inked = false;
        for (int y = 160; y < 300; ++y) {
            bool any = false;
            for (int x = 20; x < 40; ++x) any = any || !near(image.pixelColor(x, y), Qt::white, 60);
            if (any && !inked) tops << y;
            inked = any;
        }
        QVERIFY2(tops.size() >= 2 && std::abs((tops.at(1) - tops.at(0)) - 1.221 * 40) < 0.03 * 1.221 * 40, qPrintable(QString::number(tops.value(1) - tops.value(0))));
        // Said: the stand-in (as wide), and the one whose widths are not known.
        const QStringList notes = deck->slide(0).notShown;
        if (!QFontDatabase::families().contains(QStringLiteral("Calibri")))
            QVERIFY2(notes.join("|").contains(QRegularExpression("the font Calibri, not installed: drawn as .+, as wide")), qPrintable(notes.join(" | ")));
        QVERIFY2(notes.join("|").contains("the font Qucs Missing Font, not installed: drawn as") && notes.join("|").contains("(lines may break elsewhere)"),
                 qPrintable(notes.join(" | ")));
    }

    // A chart's axes as PowerPoint's own, the review's charts again (titled,
    // text of 18 points, a frame of 302 by 202 points): steps for the axis's
    // length - 0 to 4 as 0 to 6 by 2 (the tallest bar two thirds up); a
    // scatter's 0 to 1.4 across as 0 to 1.5 by 0.5.
    void chartAxesStepped()
    {
        const QByteArray magenta = "<c:majorGridlines><c:spPr><a:ln w=\"12700\"><a:solidFill><a:srgbClr val=\"FF00FF\"/></a:solidFill></a:ln></c:spPr></c:majorGridlines>";
        const auto nums = [](const QList<double>& v) {
            QByteArray out = "<c:numRef><c:numCache><c:ptCount val=\"" + QByteArray::number(v.size()) + "\"/>";
            for (int i = 0; i < v.size(); ++i) out += "<c:pt idx=\"" + QByteArray::number(i) + "\"><c:v>" + QByteArray::number(v.at(i)) + "</c:v></c:pt>";
            return out + "</c:numCache></c:numRef>";
        };
        const QByteArray cats = "<c:cat><c:strRef><c:strCache><c:ptCount val=\"4\"/><c:pt idx=\"0\"><c:v>A</c:v></c:pt><c:pt idx=\"1\"><c:v>B</c:v></c:pt>"
                                "<c:pt idx=\"2\"><c:v>C</c:v></c:pt><c:pt idx=\"3\"><c:v>D</c:v></c:pt></c:strCache></c:strRef></c:cat>";
        const QByteArray text18 = "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"1800\"/></a:pPr><a:endParaRPr lang=\"en-US\"/></a:p></c:txPr>";
        const QByteArray columns =
            "<?xml version=\"1.0\"?><c:chartSpace " + ns() + "><c:chart><c:title><c:tx><c:rich><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Columns</a:t></a:r></a:p></c:rich></c:tx>"
            "<c:overlay val=\"0\"/></c:title><c:autoTitleDeleted val=\"0\"/><c:plotArea><c:barChart><c:barDir val=\"col\"/><c:grouping val=\"clustered\"/>"
            "<c:ser><c:idx val=\"0\"/><c:spPr>" + solid("0000FF") + "</c:spPr>" + cats + "<c:val>" + nums({1, 3, 2, 4}) + "</c:val></c:ser>"
            "<c:axId val=\"1\"/><c:axId val=\"2\"/></c:barChart>"
            "<c:catAx><c:axId val=\"1\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"b\"/><c:crossAx val=\"2\"/></c:catAx>"
            "<c:valAx><c:axId val=\"2\"/><c:scaling/><c:delete val=\"0\"/><c:axPos val=\"l\"/>" + magenta + "<c:crossAx val=\"1\"/></c:valAx></c:plotArea>"
            "<c:legend><c:legendPos val=\"b\"/></c:legend></c:chart>" + text18 + "</c:chartSpace>";
        const QByteArray scatter =
            "<?xml version=\"1.0\"?><c:chartSpace " + ns() + "><c:chart><c:title><c:tx><c:rich><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Scatter L-I</a:t></a:r></a:p></c:rich></c:tx>"
            "<c:overlay val=\"0\"/></c:title><c:autoTitleDeleted val=\"0\"/><c:plotArea><c:scatterChart><c:scatterStyle val=\"lineMarker\"/>"
            "<c:ser><c:idx val=\"0\"/><c:xVal>" + nums({0, 0.05, 0.5, 1, 1.4}) + "</c:xVal><c:yVal>" + nums({0, 0, 0.27, 0.57, 0.8}) + "</c:yVal></c:ser>"
            "<c:axId val=\"3\"/><c:axId val=\"4\"/></c:scatterChart>"
            "<c:valAx><c:axId val=\"3\"/><c:scaling/><c:delete val=\"0\"/><c:axPos val=\"b\"/>" + magenta + "<c:crossAx val=\"4\"/></c:valAx>"
            "<c:valAx><c:axId val=\"4\"/><c:scaling/><c:delete val=\"0\"/><c:axPos val=\"l\"/>" + magenta + "<c:crossAx val=\"3\"/></c:valAx></c:plotArea>"
            "<c:legend><c:legendPos val=\"r\"/></c:legend></c:chart>" + text18 + "</c:chartSpace>";
        Deck d;
        d.minorFont = "Calibri";   // (the review's deck's)
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart1.xml"), columns} << zip::Entry{QStringLiteral("ppt/charts/chart2.xml"), scatter};
        Deck::SlideSpec s;
        const QRectF boxes[] = {QRectF(20, 20, 302.4, 201.6), QRectF(20, 300, 432, 201.6)};
        for (int i = 0; i < 2; ++i) {
            s.tree += "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" + QByteArray::number(i + 2) + "\" name=\"c\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\""
                      + emu(boxes[i].x()) + "\" y=\"" + emu(boxes[i].y()) + "\"/><a:ext cx=\"" + emu(boxes[i].width()) + "\" cy=\"" + emu(boxes[i].height())
                      + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart r:id=\"rIdC" + QByteArray::number(i + 1)
                      + "\"/></a:graphicData></a:graphic></p:graphicFrame>";
            s.rels += "<Relationship Id=\"rIdC" + QByteArray::number(i + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" Target=\"../charts/chart"
                      + QByteArray::number(i + 1) + ".xml\"/>";
        }
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QImage image = deck->render(0, 2);
        // The gridlines across a box (rows, or columns), each once.
        const auto lines = [&image](const QRectF& box, bool rows) {
            QList<int> found;
            const QRect r = QRectF(box.x() * 2, box.y() * 2, box.width() * 2, box.height() * 2).toRect();
            int was = -10;
            for (int i = rows ? r.top() : r.left(); i <= (rows ? r.bottom() : r.right()); ++i) {
                int n = 0;
                for (int j = rows ? r.left() : r.top(); j <= (rows ? r.right() : r.bottom()); ++j)
                    if (near(rows ? image.pixelColor(j, i) : image.pixelColor(i, j), QColor(255, 0, 255), 40)) ++n;
                if (n > 40) {
                    if (i > was + 1) found << i;
                    was = i;
                }
            }
            return found;
        };
        // The column chart: the tallest bar's top two thirds of the way from
        // its foot to the top gridline (6); 2, 4 and 6 above the foot. (Its
        // longest run of blue down a column: not the legend's swatch.)
        const QList<int> rows = lines(boxes[0], true);
        int foot = -1, top = 100000;
        const QRect b = QRectF(boxes[0].x() * 2, boxes[0].y() * 2, boxes[0].width() * 2, boxes[0].height() * 2).toRect();
        for (int x = b.left(); x <= b.right(); ++x) {
            int start = -1;
            for (int y = b.top(); y <= b.bottom() + 1; ++y) {
                const bool blue = y <= b.bottom() && near(image.pixelColor(x, y), QColor(0, 0, 255), 30);
                if (blue && start < 0) start = y;
                if (!blue && start >= 0) {
                    if (y - 1 - start > foot - top) {
                        top = start;
                        foot = y - 1;
                    }
                    start = -1;
                }
            }
        }
        QVERIFY2(!rows.isEmpty() && foot > 0, qPrintable(QString::number(rows.size())));
        int above = 0;
        for (int y : rows) above += y < foot - 2 ? 1 : 0;
        QCOMPARE(above, 3);
        const double reach = double(foot - top) / (foot - rows.first());
        QVERIFY2(std::abs(reach - 4.0 / 6.0) < 0.04, qPrintable(QString::number(reach)));
        // The title 1.2 times the labels' text, as PowerPoint's: its capitals
        // ("Columns") against the top label's digit ("6"), any font.
        const auto inkHeight = [&image](const QRect& r) {
            int first = -1, last = -1;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    if (c.lightness() < 160 && !near(c, QColor(255, 0, 255), 60) && !near(c, QColor(0, 0, 255), 60)) {
                        if (first < 0) first = y;
                        last = y;
                        break;
                    }
                }
            return first < 0 ? 0 : last - first + 1;
        };
        int plotLeft = b.right();
        for (int x = b.left(); x <= b.right(); ++x)
            if (near(image.pixelColor(x, rows.first()), QColor(255, 0, 255), 40)) {
                plotLeft = x;
                break;
            }
        const int title = inkHeight(QRect(QPoint(b.left(), b.top()), QPoint(b.right(), rows.first() - 30)));
        const int digit = inkHeight(QRect(QPoint(b.left(), rows.first() - 30), QPoint(plotLeft - 2, rows.first() + 30)));
        QVERIFY2(digit > 0 && double(title) / digit > 1.08 && double(title) / digit < 1.3, qPrintable(QString("%1 / %2").arg(title).arg(digit)));
        // The scatter: 0 to 1.5 by 0.5 across (four lines, or three where the
        // axis covers 0), 0 to 1 by 0.5 up.
        const int across = int(lines(boxes[1], false).size()), up = int(lines(boxes[1], true).size());
        QVERIFY2(across >= 3 && across <= 4, qPrintable(QString::number(across)));
        QVERIFY2(up >= 2 && up <= 3, qPrintable(QString::number(up)));
    }

    // The tab: opened by the window as a presentation - its slides, their
    // thumbnails (a hidden one said), the slide field, next and previous;
    // the zoom box and its fits; the notes of the slide in sight and what
    // of it is not drawn; finding; the slide and its text copied; read
    // again when its file changes (the slide kept), a problem shown when it
    // cannot be read, gone when it can; saved under another name, a copy;
    // printed, a page a slide; played full screen, hidden slides left out.
    // A zoom keeps the slide it was on in sight: it used to open a hair
    // above a slide standing at the view's top, on the end of the slide
    // before, which was then the slide shown (bug hunt 2026-10-23 B2).
    void aZoomKeepsTheSlideItWasOn()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        Deck d;
        for (int i = 0; i < 7; ++i) {
            Deck::SlideSpec s;
            s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide %1").arg(i + 1)))), "<p:ph type=\"title\"/>");
            d.slides << s;
        }
        const QString file = dir.filePath("workspace/zooming.pptx");
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly) && f.write(d.build()) > 0);
        f.close();
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        // From "fit width", where a slide goes to the view's very top.
        doc->view()->setFit(pptx::SlideView::Fit::Width);
        for (const int slide : {4, 6, 2, 0}) {
            doc->goToSlide(slide);
            QCOMPARE(doc->currentSlide(), slide);
            for (const qreal zoom : {1.0, 2.5, 0.5}) {
                doc->view()->setZoom(zoom);
                QApplication::processEvents();
                QVERIFY2(doc->currentSlide() == slide,
                         qPrintable(QStringLiteral("slide %1 at zoom %2 became %3").arg(slide + 1).arg(zoom).arg(doc->currentSlide() + 1)));
            }
            doc->view()->setFit(pptx::SlideView::Fit::Width);
        }
        // And through the tool, which is how it was found: "fit width"
        // again, then a slide and a zoom in one call.
        doc->view()->setFit(pptx::SlideView::Fit::Width);
        doc->goToSlide(0);
        auto* control = app.findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const QJsonObject said = control->callNow(QStringLiteral("show_pptx"),
                                                  QJsonObject{{QStringLiteral("slide"), 5}, {QStringLiteral("zoom"), 100}}, 30000);
        QVERIFY2(!said.value(QStringLiteral("isError")).toBool(), qPrintable(QucsControl::textOf(said)));
        QCOMPARE(doc->currentSlide(), 4);
        app.closeAllFiles();
    }

    // Zoomed out on the last slide, in a view taller than the deck's end
    // can be scrolled to: the view stops at the end, where a third of the
    // way down lies the slide before - and that one used to become the
    // slide shown. Seen on the screen ("slide 7 at zoom 0.5 became 6",
    // 2026-11-07), not offscreen, whose view was shorter.
    void aZoomOutAtTheLastSlideKeepsIt()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 1500);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        Deck d;
        for (int i = 0; i < 7; ++i) {
            Deck::SlideSpec s;
            s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide %1").arg(i + 1)))), "<p:ph type=\"title\"/>");
            d.slides << s;
        }
        const QString file = dir.filePath("workspace/zooming-tall.pptx");
        QVERIFY(write(file, d.build()));
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        // (Tall enough that the end of the deck at 50 % stops short.)
        QVERIFY2(doc->view()->viewport()->height() > 500, qPrintable(QString::number(doc->view()->viewport()->height())));
        for (const int slide : {6, 5}) {
            doc->view()->setFit(pptx::SlideView::Fit::Width);
            doc->goToSlide(slide);
            QCOMPARE(doc->currentSlide(), slide);
            for (const qreal zoom : {1.0, 0.5, 0.25, 0.1, 2.0}) {
                doc->view()->setZoom(zoom);
                QApplication::processEvents();
                QVERIFY2(doc->currentSlide() == slide,
                         qPrintable(QStringLiteral("slide %1 at zoom %2 became %3").arg(slide + 1).arg(zoom).arg(doc->currentSlide() + 1)));
                QCOMPARE(doc->slideField()->text(), QString::number(slide + 1));
            }
            // And stepped, as Ctrl and the wheel step it.
            doc->view()->zoomStep(false);
            QCOMPARE(doc->currentSlide(), slide);
        }
        app.closeAllFiles();
    }

    // A slide too large for one image at the zoom shown - PowerPoint's
    // largest, 56 inches square, at 800 % is a billion pixels; a 16:9 slide
    // at 800 % on a screen of two pixels a point is 133 million - is drawn
    // in tiles, those in sight. It was left white, and asked to be drawn
    // again every frame.
    void aSlideTooLargeForOneImageIsDrawnInTiles()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        Deck d;
        Deck::SlideSpec s;
        // A red square in the middle of a 4032-point slide, a blue one at
        // its top left.
        s.tree = shape(2, xfrm(1916, 1916, 200, 200) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF0000"))
                 + shape(3, xfrm(0, 0, 40, 40) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("0000FF"));
        d.slides << s << s;
        QByteArray bytes = d.build();
        // (The deck's size: 51206400 EMU a side.)
        QList<zip::Entry> entries = zip::read(bytes);
        for (zip::Entry& e : entries)
            if (e.name == QLatin1String("ppt/presentation.xml")) e.data.replace("<p:sldSz cx=\"12192000\" cy=\"6858000\"/>", "<p:sldSz cx=\"51206400\" cy=\"51206400\"/>");
        const QString file = dir.filePath("workspace/huge-slide.pptx");
        QVERIFY(write(file, zip::write(entries)));
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->presentation()->slideSize(), QSizeF(4032, 4032));
        doc->view()->setZoom(pptx::SlideView::MaxZoom);
        doc->view()->centerOn(0, QPointF(2016, 2016));
        QWidget* port = doc->view()->viewport();
        // Drawn within a few frames (a quarter second of drawing a frame).
        QImage seen;
        QTRY_VERIFY_WITH_TIMEOUT((seen = port->grab().toImage(), near(seen.pixelColor(seen.width() / 2, seen.height() / 2), Qt::red)), 10000);
        // And then left alone: no slide drawn again frame after frame.
        QTest::qWait(300);
        const int drawn = doc->view()->slidesDrawn();
        QTest::qWait(300);
        QCOMPARE(doc->view()->slidesDrawn(), drawn);
        // Scrolled to its top left corner: the blue square.
        doc->view()->centerOn(0, QPointF(30, 30));
        QTRY_VERIFY_WITH_TIMEOUT((seen = port->grab().toImage(), !seen.isNull()), 5000);
        const QPoint corner = doc->view()->toViewport(0, QPointF(20, 20)).toPoint();
        QTRY_VERIFY_WITH_TIMEOUT((seen = port->grab().toImage(), near(seen.pixelColor(corner * seen.devicePixelRatio()), Qt::blue)), 10000);
        // The pen on it, where it is drawn.
        markup::Stroke stroke;
        stroke.on = pptx::SlideView::partOf(0);
        stroke.color = Qt::red;
        stroke.width = 2;
        stroke.points = {QPointF(25, 25), QPointF(35, 35)};
        doc->view()->addStroke(stroke);
        QCOMPARE(doc->view()->drawing().on(pptx::SlideView::partOf(0)).size(), 1);
        app.closeAllFiles();
    }

    // The tab used fast and every which way - slides, zooms, fits, regions,
    // scrolling, the pen, the find bar, a reload - on a deck of pictures,
    // shapes and text: nothing lost, nothing out of range.
    void theTabHoldsUnderFastUse()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1100, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QImage photo(1200, 800, QImage::Format_RGB32);
        photo.fill(QColor(30, 120, 200));
        Deck d;
        d.extra << zip::Entry{QStringLiteral("ppt/media/image1.png"), png(photo)};
        d.extra << zip::Entry{QStringLiteral("ppt/media/image2.svg"),
                              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"><circle cx=\"5\" cy=\"5\" r=\"4\" fill=\"#ff8800\"/></svg>"};
        for (int i = 0; i < 30; ++i) {
            Deck::SlideSpec s;
            s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide %1 of the stress").arg(i + 1)))), "<p:ph type=\"title\"/>");
            for (int k = 0; k < 20; ++k)
                s.tree += shape(10 + k, xfrm((k % 10) * 90 + 20, 300 + (k / 10) * 100, 80, 80) + "<a:prstGeom prst=\"roundRect\"><a:avLst/></a:prstGeom>" + solid("70AD47"),
                                text(para(run(QStringLiteral("box %1").arg(k)))));
            s.tree += "<p:pic><p:nvPicPr><p:cNvPr id=\"50\" name=\"p\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rP\"><a:extLst>"
                      "<a:ext uri=\"{96DAC541-7B7A-43D3-8B79-37D633B846F1}\"><asvg:svgBlip xmlns:asvg=\"http://schemas.microsoft.com/office/drawing/2016/SVG/main\" "
                      "r:embed=\"" + QByteArray(i % 2 ? "rS" : "rP") + "\"/></a:ext></a:extLst></a:blip><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>"
                      + xfrm(600, 120, 300, 160) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
            s.rels = "<Relationship Id=\"rP\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image1.png\"/>"
                     "<Relationship Id=\"rS\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image2.svg\"/>";
            s.notes = QStringLiteral("Notes of slide %1").arg(i + 1);
            d.slides << s;
        }
        const QString file = dir.filePath("workspace/stress.pptx");
        QVERIFY(write(file, d.build()));
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        pptx::SlideView* view = doc->view();
        QRandomGenerator random(2611);
        for (int step = 0; step < 400; ++step) {
            const int n = doc->slideCount();
            switch (random.bounded(12)) {
            case 0: doc->goToSlide(random.bounded(n)); break;
            case 1: doc->nextSlide(); break;
            case 2: doc->previousSlide(); break;
            case 3: view->zoomStep(random.bounded(2) == 0); break;
            case 4: view->setZoom(pptx::SlideView::MinZoom + random.generateDouble() * (pptx::SlideView::MaxZoom - pptx::SlideView::MinZoom)); break;
            case 5: view->setFit(random.bounded(2) ? pptx::SlideView::Fit::Width : pptx::SlideView::Fit::Slide); break;
            case 6: view->verticalScrollBar()->setValue(random.bounded(view->verticalScrollBar()->maximum() + 1)); break;
            case 7: view->horizontalScrollBar()->setValue(random.bounded(view->horizontalScrollBar()->maximum() + 1)); break;
            case 8: view->showRegion(random.bounded(n), QRectF(random.bounded(900), random.bounded(500), 1 + random.bounded(400), 1 + random.bounded(300))); break;
            case 9: {
                markup::Stroke stroke;
                stroke.on = pptx::SlideView::partOf(random.bounded(n));
                stroke.color = Qt::red;
                stroke.width = 2;
                stroke.points = {QPointF(random.bounded(960), random.bounded(540)), QPointF(random.bounded(960), random.bounded(540))};
                if (random.bounded(3) == 0) view->undoDrawing();
                else view->addStroke(stroke);
                break;
            }
            case 10: doc->find(QStringLiteral("slide %1").arg(random.bounded(n) + 1)); break;
            case 11:
                if (step % 97 == 0) QVERIFY(doc->reload());
                else doc->zoomBy(random.bounded(2) ? 1.25 : 0.8);
                break;
            }
            if (step % 3 == 0) view->viewport()->repaint();
            if (step % 7 == 0) QApplication::processEvents();
            QVERIFY(doc->currentSlide() >= 0 && doc->currentSlide() < doc->slideCount());
            QVERIFY(view->zoom() >= pptx::SlideView::MinZoom - 1e-9 && view->zoom() <= pptx::SlideView::MaxZoom + 1e-9);
            QCOMPARE(doc->slideField()->text(), QString::number(doc->currentSlide() + 1));
        }
        app.closeAllFiles();
    }

    // Tabs closed, and the file read again, while their thumbnails are
    // still being drawn and their pictures decoded on another thread; many
    // decks open at once, closed in another order.
    void tabsClosedWhileTheirPicturesAreDecoded()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1100, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QStringList files;
        for (int f = 0; f < 6; ++f) {
            Deck d;
            for (int i = 0; i < 60; ++i) {
                QImage photo(900, 600, QImage::Format_RGB32);
                photo.fill(QColor::fromHsv((i * 37 + f * 11) % 360, 200, 200));
                d.extra << zip::Entry{QStringLiteral("ppt/media/image%1.png").arg(i + 1), png(photo)};
                Deck::SlideSpec s;
                s.tree = "<p:pic><p:nvPicPr><p:cNvPr id=\"5\" name=\"p\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rP\"/><a:stretch><a:fillRect/></a:stretch>"
                         "</p:blipFill><p:spPr>" + xfrm(100, 50, 760, 440) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
                s.rels = "<Relationship Id=\"rP\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image"
                         + QByteArray::number(i + 1) + ".png\"/>";
                d.slides << s;
            }
            files << dir.filePath(QStringLiteral("workspace/many-%1.pptx").arg(f));
            QVERIFY(write(files.last(), d.build()));
        }
        // Opened and closed at once, again and again.
        for (int k = 0; k < 4; ++k) {
            QVERIFY(app.gotoPage(files.at(k % files.size())));
            QApplication::processEvents();
            app.closeAllFiles();
        }
        // All open; the file of one replaced while its thumbnails are drawn;
        // the tabs gone through and closed, the last first.
        for (const QString& f : files) QVERIFY(app.gotoPage(f));
        QCOMPARE(app.DocumentTab->count() >= files.size(), true);
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QVERIFY(doc->reload());
        QVERIFY(doc->reload());
        for (int i = 0; i < app.DocumentTab->count(); ++i) {
            app.DocumentTab->setCurrentIndex(i);
            if (auto* d = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget())) {
                d->goToSlide(30);
                d->view()->viewport()->repaint();
            }
        }
        // (The window keeps one tab: the last is closed with the rest.)
        while (app.DocumentTab->count() > 1) {
            app.DocumentTab->setCurrentIndex(app.DocumentTab->count() - 1);
            QApplication::processEvents();
            const int before = app.DocumentTab->count();
            menu_owner_close::clickClose(app.DocumentTab, app.DocumentTab->widget(before - 1));
            QTRY_COMPARE(app.DocumentTab->count(), before - 1);
        }
        app.closeAllFiles();
        QThreadPool::globalInstance()->waitForDone();
    }

    // Slide sizes past anything PowerPoint makes - huge, tiny, of no size -
    // in the tab: held within what the view can scroll, every zoom drawn.
    void absurdSlideSizesInTheTab()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        int k = 0;
        for (const QByteArray& size : QList<QByteArray>{"cx=\"99999999999999\" cy=\"99999999999999\"", "cx=\"1\" cy=\"1\"", "cx=\"0\" cy=\"-5\"",
                                                       "cx=\"51206400\" cy=\"1\"", "cx=\"1\" cy=\"51206400\"", "cx=\"1e300\" cy=\"NaN\""}) {
            Deck d;
            for (int i = 0; i < 40; ++i) {
                Deck::SlideSpec s;
                s.tree = shape(2, xfrm(10, 10, 100, 50) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF0000"), text(para(run("x"))));
                d.slides << s;
            }
            QList<zip::Entry> entries = zip::read(d.build());
            for (zip::Entry& e : entries)
                if (e.name == QLatin1String("ppt/presentation.xml")) e.data.replace("cx=\"12192000\" cy=\"6858000\"", size);
            const QString file = dir.filePath(QStringLiteral("workspace/size-%1.pptx").arg(k++));
            QVERIFY(write(file, zip::write(entries)));
            QVERIFY2(app.gotoPage(file), size.constData());
            auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
            QVERIFY(doc != nullptr);
            const QSizeF s = doc->presentation()->slideSize();
            QVERIFY2(s.width() >= 1 && s.width() <= 40320 && s.height() >= 1 && s.height() <= 40320, size.constData());
            // Its thumbnails no taller than four times their width (a slide 1
            // point by 40,320 made icons of 150 x 6 million pixels, 3.6 GB
            // each), the slide drawn inside.
            QVERIFY2(doc->thumbnails()->iconSize().height() <= 600, size.constData());
            for (const qreal zoom : {pptx::SlideView::MinZoom, 1.0, pptx::SlideView::MaxZoom}) {
                doc->view()->setZoom(zoom);
                doc->goToSlide(39);
                doc->view()->viewport()->repaint();
                QCOMPARE(doc->currentSlide(), 39);
                doc->view()->verticalScrollBar()->setValue(doc->view()->verticalScrollBar()->maximum());
                doc->view()->viewport()->repaint();
            }
            doc->view()->setFit(pptx::SlideView::Fit::Slide);
            doc->view()->viewport()->repaint();
            app.closeAllFiles();
        }
    }

    // The window closed while a tab's own dialog is open: the tab is freed
    // once its own call has returned, not under the dialog - which is a
    // local object of that call, and freeing it killed the program (bug
    // hunt 2026-10-23 A6).
    void aTabIsNotFreedUnderItsOwnDialog()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        Deck d;
        Deck::SlideSpec s;
        s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("One")))), "<p:ph type=\"title\"/>");
        d.slides << s;
        const QString file = dir.filePath("workspace/closing.pptx");
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly) && f.write(d.build()) > 0);
        f.close();
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QVERIFY(!doc->inADialog());
        // Inside the dialog's own loop: the documents are closed, then the
        // dialog is answered. Nothing is freed under it.
        bool closed = false;
        QTimer::singleShot(0, &app, [&app, &closed] {
            QVERIFY(app.closeAllFiles());
            closed = true;
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<QFileDialog*>(w)) dialog->reject();
        });
        doc->exportSlide();   // opens the dialog, and returns once it is answered
        QVERIFY(closed);
        // The tab is gone, and the document is freed as the loop returns to
        // the top (nothing of it is touched meanwhile).
        QTRY_COMPARE(app.DocumentTab->count(), 1);   // (an untitled schematic in its place)
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // The tab closed by its close button while the slide's menu is up: the
    // menu goes with it and nothing is freed twice (docs/bug_hunts/
    // 2026-11-07-layout-close-crash.md: a QMenu on the stack of the slot,
    // shown with exec(), freed as if allocated when the tab closed in
    // exec()'s loop - the abort of 27.0.7 in the layout viewer).
    void aTabClosedWithItsMenuUpTakesTheMenuWithIt()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(900, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        Deck d;
        Deck::SlideSpec s;
        s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("One")))), "<p:ph type=\"title\"/>");
        d.slides << s;
        const QString file = dir.filePath("workspace/menu.pptx");
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly) && f.write(d.build()) > 0);
        f.close();
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        ContextMenuTabWidget* pane = app.paneOf(doc);
        QVERIFY(pane != nullptr);
        const QString wrong = menu_owner_close::closeWithMenuUp(
            doc, [doc] { emit doc->view()->menuRequested(doc->view()->mapToGlobal(QPoint(20, 20)), 0); },
            [pane, doc] { menu_owner_close::clickClose(pane, doc); });
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        app.closeAllFiles();
    }

    void opensInATab()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        const QString file = dir.filePath("workspace/talk.pptx");
        const auto deckOf = [](int slides) {
            Deck d;
            for (int i = 0; i < slides; ++i) {
                Deck::SlideSpec s;
                s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide title %1").arg(i + 1)))), "<p:ph type=\"title\"/>")
                         + shape(3, QByteArray(), text(para(run(i == 1 ? QStringLiteral("The second point") : QStringLiteral("A point")))), "<p:ph idx=\"1\"/>");
                if (i == 1) s.notes = QStringLiteral("Say this about the measurement.");
                if (i == 2) s.attrs = "show=\"0\"";
                d.slides << s;
            }
            return d.build();
        };
        QVERIFY(write(file, deckOf(3)));
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QVERIFY(QucsApp::isPresentationDocument(doc));
        QCOMPARE(doc->slideCount(), 3);
        QCOMPARE(doc->thumbnails()->count(), 3);
        QCOMPARE(doc->thumbnails()->item(2)->text(), QStringLiteral("3 (hidden)"));
        QCOMPARE(doc->currentSlide(), 0);
        QCOMPARE(doc->slideField()->text(), QStringLiteral("1"));
        QVERIFY(doc->notesView()->toPlainText().contains("No speaker notes"));
        doc->nextSlide();
        QCOMPARE(doc->currentSlide(), 1);
        QCOMPARE(doc->slideField()->text(), QStringLiteral("2"));
        QVERIFY(doc->notesView()->toPlainText().contains("Say this about the measurement."));
        QCOMPARE(doc->thumbnails()->currentRow(), 1);
        doc->thumbnails()->setCurrentRow(2);
        QCOMPARE(doc->currentSlide(), 2);
        doc->previousSlide();
        QCOMPARE(doc->currentSlide(), 1);
        // The thumbnails drawn, a few at a time.
        QTRY_VERIFY(!doc->thumbnails()->item(2)->icon().pixmap(QSize(150, 84)).toImage().isNull());
        // Zoom: a scale, the fits.
        doc->view()->setZoom(1.5);
        QCOMPARE(doc->zoomBox()->currentText(), QStringLiteral("150%"));
        QCOMPARE(doc->currentSlide(), 1);   // (kept in sight)
        doc->showAll();
        QCOMPARE(doc->view()->fit(), pptx::SlideView::Fit::Slide);
        QCOMPARE(doc->zoomBox()->currentIndex(), 1);
        doc->zoomToSelection();
        QCOMPARE(doc->view()->fit(), pptx::SlideView::Fit::Width);
        // Found: the slides with it, the first from the one in sight on.
        doc->goToSlide(0);
        QCOMPARE(doc->find(QStringLiteral("second point")), QList<int>{1});
        QCOMPARE(doc->currentSlide(), 1);
        QVERIFY(doc->searchLabel()->text().contains("slide 2"));
        QCOMPARE(doc->find(QStringLiteral("measurement")), QList<int>{1});   // (in its notes)
        QVERIFY(doc->find(QStringLiteral("nowhere")).isEmpty());
        QCOMPARE(doc->searchLabel()->text(), QStringLiteral("Not found"));
        // Copied: the slide as a picture 1920 pixels wide, its text and notes.
        doc->copySlide();
        QCOMPARE(QApplication::clipboard()->image().width(), 1920);
        doc->copySlideText();
        QVERIFY(QApplication::clipboard()->text().contains("Slide title 2") && QApplication::clipboard()->text().contains("Notes:"));
        // Its file written again: read again, the slide and the zoom kept.
        QSignalSpy reloaded(doc, &PptxDoc::reloaded);
        doc->view()->setZoom(2.5);
        QCOMPARE(doc->currentSlide(), 1);
        QVERIFY(write(file, deckOf(4)));
        QTRY_COMPARE(doc->slideCount(), 4);
        QCOMPARE(doc->currentSlide(), 1);
        QCOMPARE(doc->view()->zoom(), 2.5);
        QCOMPARE(doc->view()->fit(), pptx::SlideView::Fit::None);
        QCOMPARE(doc->thumbnails()->count(), 4);
        // The window resized at that zoom: the slide in sight kept.
        const QSize wide = doc->view()->viewport()->size();
        app.resize(900, 650);
        QTRY_VERIFY(doc->view()->viewport()->width() < wide.width());
        QCOMPARE(doc->currentSlide(), 1);
        app.resize(1200, 800);
        QTRY_COMPARE(doc->view()->viewport()->size(), wide);
        QCOMPARE(doc->currentSlide(), 1);
        doc->zoomToSelection();   // (fit width again)
        // As wide as the view, its middle in sight: a resize keeps that place
        // in the slide (not its top at the view's top).
        doc->view()->restore({pptx::SlideView::Fit::Width, 1, 1, 0.5, 0});
        app.resize(1000, 800);
        QTRY_VERIFY(doc->view()->viewport()->width() < wide.width());
        QCOMPARE(doc->view()->state().slide, 1);
        QVERIFY2(std::abs(doc->view()->state().down - 0.5) < 0.02, qPrintable(QString::number(doc->view()->state().down)));
        app.resize(1200, 800);
        QTRY_COMPARE(doc->view()->viewport()->width(), wide.width());   // (no scroll bar across now: taller)
        doc->goToSlide(1);
        // Not readable: said; readable again: shown.
        QVERIFY(write(file, "not a presentation"));
        QTRY_VERIFY(!doc->problem().isEmpty());
        QVERIFY(write(file, deckOf(3)));
        QTRY_VERIFY(doc->problem().isEmpty());
        QCOMPARE(doc->slideCount(), 3);
        // Saved under another name: a copy of the file.
        const QString copy = dir.filePath("workspace/copy.pptx");
        doc->setName(copy);
        QCOMPARE(doc->save(), 0);
        QCOMPARE(read(copy), read(file));
        // Printed: a page a slide.
        const QString pdf = dir.filePath("printed.pdf");
        {
            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(pdf);
            QPainter p(&printer);
            doc->print(&printer, &p, true, true);
        }
        QVERIFY(QFileInfo(pdf).size() > 2000);
        QCOMPARE(read(pdf).count("/Type /Page") - read(pdf).count("/Type /Pages"), 3);
        // Played: from the first slide; on, past the hidden third to the end,
        // which a key more leaves - the tab then at the last slide shown.
        doc->startShow(0);
        QPointer<pptx::SlideShow> show = doc->slideShow();
        QVERIFY(show != nullptr);
        QCOMPARE(show->current(), 0);
        QTest::keyClick(show, Qt::Key_Right);
        QCOMPARE(show->current(), 1);
        QTest::keyClick(show, Qt::Key_Left);
        QCOMPARE(show->current(), 0);
        QTest::keyClick(show, Qt::Key_2);
        QTest::keyClick(show, Qt::Key_Return);
        QCOMPARE(show->current(), 1);
        QTest::keyClick(show, Qt::Key_Space);   // (the third hidden: the end)
        QCOMPARE(show->current(), 1);
        QVERIFY(show != nullptr);
        QTest::keyClick(show, Qt::Key_Space);
        QTRY_VERIFY(show == nullptr);
        QCOMPARE(doc->currentSlide(), 1);
        // Escape ends it at once.
        doc->startShow(-1);
        show = doc->slideShow();
        QVERIFY(show != nullptr);
        QTest::keyClick(show, Qt::Key_Escape);
        QTRY_VERIFY(show == nullptr);
        // Its keys, pressed in the tab: Ctrl+F finds, F5 plays from the first
        // slide, Shift+F5 from the one in sight - not the window's own F5 and
        // Ctrl+F in their place, nor neither for keys taken twice.
        app.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&app));
        doc->hideSearch();
        doc->view()->setFocus();
        QTest::keyClick(doc->view(), Qt::Key_F, Qt::ControlModifier);
        QVERIFY(doc->searchField()->isVisible());
        doc->hideSearch();
        QApplication::clipboard()->clear();
        doc->view()->setFocus();
        QTest::keyClick(doc->view(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->image().width(), 1920);
        doc->goToSlide(1);
        doc->view()->setFocus();
        QTest::keyClick(doc->view(), Qt::Key_F5);
        show = doc->slideShow();
        QVERIFY(show != nullptr);
        QCOMPARE(show->current(), 0);
        QVERIFY(QTest::qWaitForWindowActive(show));   // (the window's keys - Escape its select - then not taken)
        QTest::keyClick(show, Qt::Key_Escape);
        QTRY_VERIFY(show == nullptr);
        doc->goToSlide(1);
        doc->view()->setFocus();
        QTest::keyClick(doc->view(), Qt::Key_F5, Qt::ShiftModifier);
        show = doc->slideShow();
        QVERIFY(show != nullptr);
        QCOMPARE(show->current(), 1);
        QVERIFY(QTest::qWaitForWindowActive(show));
        QTest::keyClick(show, Qt::Key_Escape);
        QTRY_VERIFY(show == nullptr);
        app.closeAllFiles();
    }

    // For Claude: read_pptx reads a deck (open or not) - titles, text,
    // notes, hidden ones, what is not drawn; finds in text and notes; draws
    // slides; refuses what is no presentation. show_pptx opens it at a slide
    // (by number or title), a search's hit, a zoom, its notes, plays it. Its
    // tools in every turn while one is open; a file outside the workspace
    // asked about (the guard).
    void claudesTools()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        auto* control = app.findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const auto call = [control](const QString& tool, const QJsonObject& args) { return control->callNow(tool, args, 30000); };
        const auto json = [](const QJsonObject& r) { return QJsonDocument::fromJson(QucsControl::textOf(r).section(QLatin1Char('\n'), 0, 0).toUtf8()).object(); };
        const auto failed = [](const QJsonObject& r) { return r.value("isError").toBool(); };
        Deck d;
        d.title = QStringLiteral("The review");
        d.extra << zip::Entry{QStringLiteral("ppt/media/image1.emf"), QByteArray(64, '\0')};
        Deck::SlideSpec one, two, three;
        one.tree = shape(2, QByteArray(), text(para(run("Introduction and background"))), "<p:ph type=\"title\"/>") + shape(3, QByteArray(), text(para(run("A first point"))), "<p:ph idx=\"1\"/>");
        two.tree = shape(2, QByteArray(), text(para(run("Results"))), "<p:ph type=\"title\"/>")
                   + "<p:pic><p:nvPicPr><p:cNvPr id=\"5\" name=\"p\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rIdP\"/></p:blipFill><p:spPr>" + xfrm(100, 100, 100, 100)
                   + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        two.rels = "<Relationship Id=\"rIdP\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image1.emf\"/>";
        two.notes = QStringLiteral("The measurement agrees within 3 %.");
        three.tree = shape(2, QByteArray(), text(para(run("Backup"))), "<p:ph type=\"title\"/>");
        three.attrs = "show=\"0\"";
        d.slides << one << two << three;
        const QString file = dir.filePath("workspace/review.pptx");
        QVERIFY(write(file, d.build()));
        // Not open: read from its file.
        QJsonObject r = call("read_pptx", {{"path", "review.pptx"}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("slides in all").toInt(), 3);
        QCOMPARE(o.value("title").toString(), QStringLiteral("The review"));
        QCOMPARE(o.value("slide size").toString(), QStringLiteral("960 x 540 points"));
        const QJsonArray slides = o.value("slides").toArray();
        QCOMPARE(slides.size(), 3);
        QCOMPARE(slides.at(0).toObject().value("title").toString(), QStringLiteral("Introduction and background"));
        QVERIFY(slides.at(0).toObject().value("text").toString().contains("A first point"));
        QCOMPARE(slides.at(1).toObject().value("notes").toString(), QStringLiteral("The measurement agrees within 3 %."));
        QCOMPARE(slides.at(1).toObject().value("pictures").toInt(), 1);
        QStringList notDrawn;
        for (const QJsonValue& v : slides.at(1).toObject().value("not drawn").toArray()) notDrawn << v.toString();
        // (Not the first of them: a machine without the deck's fonts says so
        // in the same list, and the title is laid out before the picture.)
        QCOMPARE(ofTheDeck(notDrawn).size(), 1);
        QVERIFY2(ofTheDeck(notDrawn).first().contains(QStringLiteral("EMF")), qPrintable(notDrawn.join(QStringLiteral(" | "))));
        QVERIFY(slides.at(2).toObject().value("hidden").toBool());
        QVERIFY(!slides.at(0).toObject().contains("hidden"));
        QVERIFY(app.findDoc(file) == nullptr);   // (read, not opened)
        // Searched; some slides; drawn.
        o = json(call("read_pptx", {{"path", "review.pptx"}, {"search", "MEASUREMENT"}}));
        QCOMPARE(o.value("found").toArray().size(), 1);
        QCOMPARE(o.value("found").toArray().first().toObject().value("slide").toInt(), 2);
        QVERIFY(o.value("found").toArray().first().toObject().contains("notes"));
        o = json(call("read_pptx", {{"path", "review.pptx"}, {"slides", "2-3"}}));
        QCOMPARE(o.value("slides").toArray().size(), 2);
        QCOMPARE(o.value("slides").toArray().first().toObject().value("slide").toInt(), 2);
        r = call("read_pptx", {{"path", "review.pptx"}, {"slides", 1}, {"image", true}});
        bool picture = false;
        for (const QJsonValue& v : r.value("content").toArray()) picture = picture || v.toObject().value("type").toString() == "image";
        QVERIFY(picture);
        QVERIFY(failed(call("read_pptx", {{"path", "review.pptx"}, {"slides", 9}})));
        QVERIFY(QucsControl::textOf(call("read_pptx", {{"path", "missing.pptx"}})).contains("There is no file"));
        QVERIFY(write(dir.filePath("workspace/notes.txt"), "text"));
        QVERIFY(QucsControl::textOf(call("read_pptx", {{"path", "notes.txt"}})).contains("is no presentation"));
        QVERIFY(QucsControl::textOf(call("read_pptx", {})).contains("Which presentation?"));
        // Shown: opened at slide 2; by a title; a search's hit; a zoom; the notes; played.
        r = call("show_pptx", {{"path", "review.pptx"}, {"slide", 2}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->currentSlide(), 1);
        o = json(r);
        QCOMPARE(o.value("slide shown").toInt(), 2);
        QCOMPARE(o.value("its title").toString(), QStringLiteral("Results"));
        QCOMPARE(json(call("show_pptx", {{"slide", "Back"}})).value("slide shown").toInt(), 3);   // (its start before slide 1's "background")
        QVERIFY(QucsControl::textOf(call("show_pptx", {{"slide", "Nowhere"}})).contains("No slide is titled"));
        o = json(call("show_pptx", {{"find", "first point"}}));
        QCOMPARE(o.value("slide shown").toInt(), 1);
        QCOMPARE(o.value("hits").toArray().size(), 1);
        QCOMPARE(json(call("show_pptx", {{"zoom", "fit slide"}})).value("zoom").toString(), QStringLiteral("fit slide"));
        QCOMPARE(json(call("show_pptx", {{"zoom", 150}})).value("zoom").toString(), QStringLiteral("150%"));
        QCOMPARE(json(call("show_pptx", {{"notes", false}})).value("notes shown").toBool(), false);
        QVERIFY(!doc->notesShown());
        o = json(call("show_pptx", {{"slide", 1}, {"play", true}}));
        QCOMPARE(o.value("playing").toObject().value("slide").toInt(), 1);
        QVERIFY(doc->slideShow() != nullptr);
        call("show_pptx", {{"play", false}});
        QTRY_VERIFY(doc->slideShow() == nullptr);
        // Open: read as its tab shows it, the one in front by default.
        o = json(call("read_pptx", {}));
        QCOMPARE(o.value("slides in all").toInt(), 3);
        // Its tools in every turn while it is open; the guard of files outside.
        QVERIFY(control->inEveryTurn().contains("read_pptx") && control->inEveryTurn().contains("show_pptx"));
        QVERIFY(control->readOnlyTools().contains("read_pptx"));
        QVERIFY(control->autonomousTools().contains("show_pptx"));
        const QString outside = dir.filePath("outside/deck.pptx");
        QVERIFY(write(outside, d.build()));
        QVERIFY2(control->askedEachTime("read_pptx", {{"path", outside}}).contains("reads"), qPrintable(control->askedEachTime("read_pptx", {{"path", outside}})));
        app.closeAllFiles();
        QVERIFY(!control->inEveryTurn().contains("read_pptx"));
    }

    // Claude drives the tab as the user does: slides by number, title and
    // step; the find bar and its hits; the zoom and a region; the panes; the
    // file read again; the slide copied; the show played and stepped to its
    // end; the bar through get_ui/set_ui (its ⋯ menu listed, not left open,
    // an entry chosen); the right-click menu; slides drawn and written
    // (pictures, numbered; a PDF; a deck not open); opened at a slide;
    // listed among the files.
    void claudeDrivesTheTab()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        auto* control = app.findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const auto call = [control](const QString& tool, const QJsonObject& args) { return control->callNow(tool, args, 30000); };
        const auto json = [](const QJsonObject& r) { return QJsonDocument::fromJson(QucsControl::textOf(r).section(QLatin1Char('\n'), 0, 0).toUtf8()).object(); };
        const auto failed = [](const QJsonObject& r) { return r.value("isError").toBool(); };
        const auto picture = [](const QJsonObject& r) {
            for (const QJsonValue& v : r.value("content").toArray())
                if (v.toObject().value("type").toString() == "image")
                    return QImage::fromData(QByteArray::fromBase64(v.toObject().value("data").toString().toLatin1()), "PNG");
            return QImage();
        };
        // Five slides, the fifth hidden; a red box on slide 2's lower right.
        const auto deckOf = [](int slides) {
            Deck d;
            const char* titles[] = {"Introduction", "Method", "Results", "Discussion", "Backup", "Appendix"};
            for (int i = 0; i < slides; ++i) {
                Deck::SlideSpec s;
                s.tree = shape(2, QByteArray(), text(para(run(titles[i]))), "<p:ph type=\"title\"/>");
                if (i == 1 || i == 3 || i == 4) s.tree += shape(3, QByteArray(), text(para(run("A point"))), "<p:ph idx=\"1\"/>");
                if (i == 1) s.tree += shape(4, xfrm(600, 300, 300, 200) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + solid("FF0000"));
                if (i == 2) s.notes = QStringLiteral("The measured loss is 3 dB.");
                if (i == 4) s.attrs = "show=\"0\"";
                d.slides << s;
            }
            return d.build();
        };
        const QString file = dir.filePath("workspace/drive.pptx");
        QVERIFY(write(file, deckOf(5)));
        QJsonObject o = json(call("show_pptx", {{"path", "drive.pptx"}}));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QCOMPARE(o.value("slides in all").toInt(), 5);
        QCOMPARE(o.value("slide shown").toInt(), 1);
        QCOMPARE(o.value("zoom").toString(), QStringLiteral("fit width"));
        QVERIFY(o.value("thumbnails shown").toBool());
        // Slides: steps, a title's start, the hidden one said.
        QCOMPARE(json(call("show_pptx", {{"slide", "next"}})).value("slide shown").toInt(), 2);
        o = json(call("show_pptx", {{"slide", "last"}}));
        QCOMPARE(o.value("slide shown").toInt(), 5);
        QVERIFY(o.contains("it is hidden"));
        QCOMPARE(json(call("show_pptx", {{"slide", "previous"}})).value("slide shown").toInt(), 4);
        QCOMPARE(json(call("show_pptx", {{"slide", "first"}})).value("slide shown").toInt(), 1);
        QCOMPARE(json(call("show_pptx", {{"slide", "Res"}})).value("slide shown").toInt(), 3);
        QCOMPARE(doc->currentSlide(), 2);
        // A wrong argument: nothing done.
        QVERIFY(failed(call("show_pptx", {{"slide", 4}, {"zoom", "huge"}})));
        QVERIFY(failed(call("show_pptx", {{"slide", 9}})));
        QVERIFY(failed(call("show_pptx", {{"slide", QJsonObject{}}})));
        QVERIFY(failed(call("show_pptx", {{"slide", 1}, {"copy", "bogus"}})));
        QCOMPARE(doc->currentSlide(), 2);
        // The find bar: from the slide shown on; on, back; closed.
        QVERIFY(failed(call("show_pptx", {{"find_next", "next"}})));
        o = json(call("show_pptx", {{"find", "point"}}));
        QVERIFY(doc->findShown());
        QCOMPARE(doc->findText(), QStringLiteral("point"));
        QCOMPARE(o.value("slide shown").toInt(), 4);
        QCOMPARE(o.value("hits").toArray().size(), 3);
        QCOMPARE(o.value("find bar").toObject().value("slides with it").toArray(), (QJsonArray{2, 4, 5}));
        QCOMPARE(json(call("show_pptx", {{"find_next", "next"}})).value("slide shown").toInt(), 5);
        QCOMPARE(json(call("show_pptx", {{"find_next", "next"}})).value("slide shown").toInt(), 2);   // (round again)
        QCOMPARE(json(call("show_pptx", {{"find_next", "previous"}})).value("slide shown").toInt(), 5);
        QCOMPARE(json(call("show_pptx", {{"find", "measured loss"}})).value("slide shown").toInt(), 3);   // (its notes)
        o = json(call("show_pptx", {{"find", ""}}));
        QVERIFY(!doc->findShown());
        QVERIFY(!o.contains("find bar"));
        // Zoom; a region as large as the tab takes it - the red box filling it.
        QCOMPARE(json(call("show_pptx", {{"zoom", 150}})).value("zoom").toString(), QStringLiteral("150%"));
        o = json(call("show_pptx", {{"slide", 2}, {"region", QJsonArray{620, 320, 880, 480}}}));
        QCOMPARE(o.value("slide shown").toInt(), 2);
        QVERIFY(doc->view()->zoom() > 2);
        const QImage seen = doc->view()->viewport()->grab().toImage();
        QVERIFY2(near(seen.pixelColor(seen.width() / 2, seen.height() / 2), QColor(255, 0, 0), 30), qPrintable(str(seen.pixelColor(seen.width() / 2, seen.height() / 2))));
        QVERIFY(failed(call("show_pptx", {{"region", QJsonArray{1, 2}}})));
        call("show_pptx", {{"zoom", "fit width"}});
        // The panes.
        o = json(call("show_pptx", {{"notes", false}, {"sidebar", false}}));
        QVERIFY(!doc->notesShown() && !doc->sidebarShown());
        QVERIFY(!o.value("notes shown").toBool() && !o.value("thumbnails shown").toBool());
        call("show_pptx", {{"notes", true}, {"sidebar", true}});
        QVERIFY(doc->notesShown() && doc->sidebarShown());
        // Copied: a picture, the text.
        QApplication::clipboard()->clear();
        call("show_pptx", {{"slide", 3}, {"copy", "picture"}});
        QCOMPARE(QApplication::clipboard()->image().width(), 1920);
        call("show_pptx", {{"copy", "text"}});
        QVERIFY(QApplication::clipboard()->text().contains("Results") && QApplication::clipboard()->text().contains("measured loss"));
        // Played: from slide 2; the show stepped (the hidden fifth left
        // out: past the fourth its end), ended by a step more; from the first.
        o = json(call("show_pptx", {{"slide", 2}, {"play", true}}));
        QPointer<pptx::SlideShow> show = doc->slideShow();
        QVERIFY(show != nullptr);
        QCOMPARE(o.value("playing").toObject().value("slide").toInt(), 2);
        QCOMPARE(json(call("show_pptx", {{"slide", "next"}})).value("playing").toObject().value("slide").toInt(), 3);
        QCOMPARE(show->current(), 2);
        QCOMPARE(doc->currentSlide(), 2);   // (the tab follows the show)
        QCOMPARE(json(call("show_pptx", {{"slide", "last"}})).value("playing").toObject().value("slide").toInt(), 4);   // (the fifth hidden)
        o = json(call("show_pptx", {{"slide", "first"}}));
        QCOMPARE(o.value("playing").toObject().value("slide").toInt(), 1);
        QCOMPARE(o.value("slide shown").toInt(), 1);
        QCOMPARE(json(call("show_pptx", {{"slide", "Discussion"}})).value("playing").toObject().value("slide").toInt(), 4);
        o = json(call("show_pptx", {{"slide", "next"}}));
        QVERIFY(o.value("playing").toObject().contains("at the end"));
        o = json(call("show_pptx", {{"slide", "next"}}));
        QVERIFY(!o.contains("playing"));
        QTRY_VERIFY(show == nullptr);
        QCOMPARE(json(call("show_pptx", {{"play", "first"}})).value("playing").toObject().value("slide").toInt(), 1);
        show = doc->slideShow();
        QVERIFY(failed(call("show_pptx", {{"play", "later"}})));
        call("show_pptx", {{"play", false}});
        QTRY_VERIFY(show == nullptr);
        // Read again: what its file holds now - the view kept, a region's
        // zoom and the slide (in sight still, the red box filling it).
        call("show_pptx", {{"slide", 2}, {"region", QJsonArray{620, 320, 880, 480}}});
        const qreal zoomed = doc->view()->zoom();
        QVERIFY(write(file, deckOf(6)));
        o = json(call("show_pptx", {{"reload", true}}));
        QCOMPARE(o.value("slides in all").toInt(), 6);
        QVERIFY(o.value("done").toString().contains("again"));
        QCOMPARE(o.value("slide shown").toInt(), 2);
        QCOMPARE(doc->view()->zoom(), zoomed);
        const QImage kept = doc->view()->viewport()->grab().toImage();
        QVERIFY2(near(kept.pixelColor(kept.width() / 2, kept.height() / 2), QColor(255, 0, 0), 30), qPrintable(str(kept.pixelColor(kept.width() / 2, kept.height() / 2))));
        call("show_pptx", {{"zoom", "fit width"}});
        QVERIFY(write(file, deckOf(5)));
        call("show_pptx", {{"reload", true}});
        QCOMPARE(doc->slideCount(), 5);
        // read_pptx: the tab's state; a region of a slide drawn.
        o = json(call("read_pptx", {{"slides", 1}}));
        QVERIFY(o.value("its tab").toObject().contains("slide shown"));
        QJsonObject r = call("read_pptx", {{"slides", 2}, {"image", true}, {"region", QJsonArray{620, 320, 880, 480}}});
        QImage part = picture(r);
        QVERIFY(!part.isNull());
        QVERIFY2(near(part.pixelColor(part.width() / 2, part.height() / 2), QColor(255, 0, 0), 20), qPrintable(str(part.pixelColor(part.width() / 2, part.height() / 2))));
        // screenshot: a slide, a region of one (in fractions), the tab as it was.
        call("show_pptx", {{"slide", 1}});
        r = call("screenshot", {{"slide", 2}, {"region", QJsonArray{0.625, 0.556, 0.917, 0.889}}});
        part = picture(r);
        QVERIFY(!part.isNull() && part.width() > 800);
        QVERIFY(near(part.pixelColor(part.width() / 2, part.height() / 2), QColor(255, 0, 0), 20));
        QCOMPARE(doc->currentSlide(), 0);
        QVERIFY(QucsControl::textOf(r).contains("slide 2 of 5"));
        QVERIFY(failed(call("screenshot", {{"slide", 9}})));
        // get_ui: the bar named; its ⋯ menu listed.
        o = json(call("get_ui", {{"area", "document"}}));
        QStringList labels;
        QJsonObject more;
        for (const QJsonValue& v : o.value("controls").toArray()) {
            labels << v.toObject().value("label").toString();
            if (v.toObject().value("label").toString() == "More") more = v.toObject();
        }
        QVERIFY2(labels.contains("Slide") && labels.contains("Zoom") && labels.contains("Slides"), qPrintable(labels.join(" | ")));
        QVERIFY2(more.value("menu").toArray().contains("Reload"), qPrintable(QJsonDocument(more).toJson()));
        // set_ui: the slide field, the zoom, a thumbnail; the ⋯ pressed - its
        // entries said, at once, no menu left open -, an entry chosen.
        QVERIFY(!failed(call("set_ui", {{"area", "document"}, {"set", QJsonArray{QJsonObject{{"control", "Slide"}, {"value", "4"}}}}})));
        QCOMPARE(doc->currentSlide(), 3);
        call("set_ui", {{"area", "document"}, {"set", QJsonArray{QJsonObject{{"control", "Zoom"}, {"value", "200%"}}}}});
        QCOMPARE(qRound(doc->view()->zoom() * 100), 200);
        call("set_ui", {{"area", "document"}, {"set", QJsonArray{QJsonObject{{"control", "Slides"}, {"value", "2"}}}}});
        QCOMPARE(doc->currentSlide(), 1);
        QElapsedTimer clock;
        clock.start();
        r = call("set_ui", {{"area", "document"}, {"press", "More"}});
        QVERIFY(clock.elapsed() < 5000);
        QVERIFY(QApplication::activePopupWidget() == nullptr);
        QVERIFY2(QucsControl::textOf(r).contains("Copy the Slide's Text") && QucsControl::textOf(r).contains("More > "), qPrintable(QucsControl::textOf(r)));
        QApplication::clipboard()->clear();
        r = call("set_ui", {{"area", "document"}, {"press", "More > Copy the Slide's Text"}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        QTRY_VERIFY(QApplication::clipboard()->text().contains("Method"));
        QVERIFY(QucsControl::textOf(call("set_ui", {{"area", "document"}, {"press", "More > Nothing"}})).contains("Reload"));
        QVERIFY(failed(call("set_ui", {{"area", "document"}, {"press", "Zoom in > Reload"}})));
        // The right-click menu on a slide: all the ⋯ has; an entry chosen.
        o = json(call("context_menu", {{"on", QJsonObject{{"slide", 3}}}}));
        QStringList entries;
        for (const QJsonValue& v : o.value("menu").toArray()) entries << v.toObject().value("entry").toString();
        for (const char* e : {"Copy the Slide as a Picture", "Copy the Slide's Text", "Play from This Slide", "Play from the First Slide", "Reload", "Show in File Manager", "Copy Path"})
            QVERIFY2(entries.contains(QString::fromLatin1(e)), qPrintable(entries.join(" | ")));
        QApplication::clipboard()->clear();
        r = call("context_menu", {{"on", QJsonObject{{"slide", 3}}}, {"choose", "Copy the Slide's Text"}});
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        QTRY_VERIFY(QApplication::clipboard()->text().contains("Results"));
        QVERIFY(failed(call("context_menu", {{"on", QJsonObject{{"slide", 9}}}})));
        // export_image: the slide shown; two, numbered; all as a PDF; in
        // grey; a deck not open; what is not a slide's format refused.
        QDir().mkpath(dir.filePath("workspace/out"));
        call("show_pptx", {{"slide", 2}});
        o = json(call("export_image", {{"save_as", "out/slide.png"}}));
        QCOMPARE(o.value("slides").toArray(), QJsonArray{2});
        QCOMPARE(QImage(dir.filePath("workspace/out/slide.png")).size(), QSize(1920, 1080));
        QVERIFY(near(QImage(dir.filePath("workspace/out/slide.png")).pixelColor(1500, 800), QColor(255, 0, 0)));
        QVERIFY(!control->irreversible("export_image", {{"slides", "2-3"}, {"save_as", "out/deck.png"}}));
        o = json(call("export_image", {{"slides", "2-3"}, {"save_as", "out/deck.png"}, {"scale", 1}}));
        QCOMPARE(o.value("written").toArray().size(), 2);
        QVERIFY(QFileInfo::exists(dir.filePath("workspace/out/deck-2.png")) && QFileInfo::exists(dir.filePath("workspace/out/deck-3.png")));
        QCOMPARE(QImage(dir.filePath("workspace/out/deck-3.png")).width(), 960);
        QVERIFY(control->irreversible("export_image", {{"slides", "2-3"}, {"save_as", "out/deck.png"}}));
        o = json(call("export_image", {{"slides", "all"}, {"save_as", "out/deck.pdf"}, {"scale", 1}}));
        QCOMPARE(read(dir.filePath("workspace/out/deck.pdf")).count("/Type /Page") - read(dir.filePath("workspace/out/deck.pdf")).count("/Type /Pages"), 5);
        QCOMPARE(o.value("hidden slides among them").toString(), QStringLiteral("5"));
        call("export_image", {{"save_as", "out/grey.png"}, {"colours", "grayscale"}});
        QVERIFY(QImage(dir.filePath("workspace/out/grey.png")).isGrayscale());
        QVERIFY(QucsControl::textOf(call("export_image", {{"save_as", "out/slide.svg"}})).contains("pdf"));
        QVERIFY(failed(call("export_image", {{"save_as", "out/x.png"}, {"slides", 9}})));
        QVERIFY(failed(call("export_image", {{"save_as", "out/x.png"}, {"diagram", 1}})));
        QVERIFY(write(dir.filePath("workspace/closed.pptx"), deckOf(3)));
        o = json(call("export_image", {{"path", "closed.pptx"}, {"slides", 3}, {"save_as", "out/closed.jpg"}}));
        QVERIFY(QFileInfo::exists(dir.filePath("workspace/out/closed.jpg")));
        QVERIFY(app.findDoc(dir.filePath("workspace/closed.pptx")) == nullptr);
        QVERIFY(control->askedEachTime("export_image", {{"save_as", dir.filePath("outside/x.png")}}).contains("writes"));
        QDir().mkpath(dir.filePath("outside"));
        QVERIFY2(control->askedEachTime("export_image", {{"slides", "2-3"}, {"save_as", dir.filePath("outside/x.png")}}).contains("x-2.png"),
                 qPrintable(control->askedEachTime("export_image", {{"slides", "2-3"}, {"save_as", dir.filePath("outside/x.png")}})));
        // Opened at a slide; at a find's first hit.
        app.closeAllFiles();
        QVERIFY(QucsControl::textOf(call("open_document", {{"path", "drive.pptx"}, {"page", 3}})).contains("At slide 3 of 5"));
        doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->currentSlide(), 2);
        QVERIFY(QucsControl::textOf(call("open_document", {{"path", "drive.pptx"}, {"find", "Discussion"}})).contains("on slides 4"));
        QCOMPARE(doc->currentSlide(), 3);
        // Listed among the workspace's files.
        o = json(call("list_documents", {}));
        bool listed = false;
        for (const QJsonValue& v : o.value("files").toArray())
            listed = listed || (v.toObject().value("kind").toString() == "presentation" && v.toObject().value("path").toString() == "drive.pptx");
        QVERIFY2(listed, qPrintable(QJsonDocument(o).toJson()));
        app.closeAllFiles();
    }

    // A deck of one's own in its tab, the window's picture taken: the first
    // file of QUCS_PPTX_RENDER, at slide QUCS_PPTX_SLIDE, into QUCS_PPTX_OUT.
    // A drawing (markup.h): the strokes of a part, the parts drawn on; an
    // eraser takes the latest stroke within its reach (its own half width
    // more) and no other part's; each stroke added, an eraser's drag and a
    // clearing undone in turn, back to nothing. A stroke's box is its
    // points' grown by half its width. A stroke begun is as broad as its
    // pen at the zoom it is drawn at, in the measures drawn on; a
    // highlighter's its own width, its colour let through.
    void aDrawingIsUndoneChangeByChange()
    {
        markup::Pen hand;
        hand.color = QColor(10, 200, 30);
        hand.width = 4;
        hand.highlighterWidth = 20;
        const markup::Stroke inked = markup::begin(hand, QStringLiteral("7"), QPointF(1, 2), 8);
        QVERIFY(inked.on == QStringLiteral("7") && inked.points == QPolygonF({QPointF(1, 2)}) && !inked.highlighter);
        QCOMPARE(inked.width, 0.5);
        QCOMPARE(inked.color, hand.color);
        hand.tool = markup::Tool::Highlighter;
        const markup::Stroke band = markup::begin(hand, QStringLiteral("7"), QPointF(1, 2), 8);
        QVERIFY(band.highlighter);
        QCOMPARE(band.width, 2.5);
        QCOMPARE(band.color.rgb(), hand.color.rgb());
        QVERIFY2(band.color.alpha() > 80 && band.color.alpha() < 125, qPrintable(QString::number(band.color.alpha())));
        markup::Drawing d;
        markup::Stroke a, b, c;
        a.on = b.on = QStringLiteral("1");
        c.on = QStringLiteral("2");
        a.points = {QPointF(0, 0), QPointF(10, 0)};
        b.points = {QPointF(0, 5), QPointF(10, 5)};
        c.points = {QPointF(3, 3)};
        a.width = b.width = 2;
        c.width = 4;
        d.add(a);
        d.add(b);
        d.add(c);
        QCOMPARE(d.count(QStringLiteral("1")), 2);
        QCOMPARE(d.count(QStringLiteral("2")), 1);
        QCOMPARE(d.parts(), QStringList({QStringLiteral("1"), QStringLiteral("2")}));
        QCOMPARE(c.bounds(), QRectF(1, 1, 4, 4));
        const QList<markup::Stroke> before = d.strokes();
        QVERIFY(!d.eraseAt(QStringLiteral("1"), QPointF(5, 2.4), 0.3));   // 2.4 and 2.6 off, 1.3 its reach
        QVERIFY(d.eraseAt(QStringLiteral("1"), QPointF(5, 4), 0.5));       // b, the latest, 1 off
        QCOMPARE(d.count(QStringLiteral("1")), 1);
        QCOMPARE(d.strokes().first().points.first(), QPointF(0, 0));
        d.changed(before);
        QVERIFY(!d.eraseAt(QStringLiteral("2"), QPointF(5, 0), 1));        // another part's stroke there
        QVERIFY(d.clear(QStringLiteral("1")));
        QCOMPARE(d.count(QStringLiteral("1")), 0);
        QCOMPARE(d.count(QStringLiteral("2")), 1);
        QVERIFY(!d.clear(QStringLiteral("3")));
        QVERIFY(d.undo());
        QCOMPARE(d.count(QStringLiteral("1")), 1);   // the clearing
        QVERIFY(d.undo());
        QCOMPARE(d.count(QStringLiteral("1")), 2);   // the eraser's drag
        for (int i = 0; i < 3; ++i) QVERIFY(d.undo());
        QVERIFY(d.isEmpty() && !d.canUndo() && !d.undo());
    }

    // The pen in hand, picked from the toolbar pen's menu - its tool, a
    // colour, how broad, any colour the chooser gives - kept in the settings
    // and the same in every viewer: another button's menu knows it at once.
    // Picking takes the pen up; a colour picked with the eraser in hand
    // takes the pen. The highlighter's width is its own. The icon is drawn
    // in the pen's colour; the menu's undo and clearing are as the drawing.
    void thePenIsPickedFromItsMenuAndKept()
    {
        markup::setCurrentPen(markup::Pen());
        markup::PenButton one(QStringLiteral("Slide")), two(QStringLiteral("Cell"));
        const auto entry = [](markup::PenButton& b, const QString& text) -> QAction* {
            for (QAction* a : b.penMenu()->actions())
                if (a->text() == text) return a;
            return nullptr;
        };
        for (const QString& text : {QStringLiteral("Pen"), QStringLiteral("Highlighter"), QStringLiteral("Eraser"), QStringLiteral("Blue"),
                                    QStringLiteral("Other Colour…"), QStringLiteral("Fine"), QStringLiteral("Undo the Last Change"),
                                    QStringLiteral("Clear the Drawings on This Slide"), QStringLiteral("Clear All the Drawings")})
            QVERIFY2(entry(one, text) != nullptr, qPrintable(text));
        QVERIFY(!one.isChecked());
        entry(one, QStringLiteral("Highlighter"))->trigger();
        QVERIFY(one.isChecked());
        QCOMPARE(markup::currentPen().tool, markup::Tool::Highlighter);
        QVERIFY(entry(two, QStringLiteral("Highlighter"))->isChecked());
        entry(one, QStringLiteral("Blue"))->trigger();
        QCOMPARE(markup::currentPen().color, QColor(0x1c, 0x71, 0xd8));
        entry(one, QStringLiteral("Bold"))->trigger();
        QCOMPARE(markup::currentPen().highlighterWidth, 24.0);
        QCOMPARE(markup::currentPen().width, 3.0);
        one.setColorChooser([](const QColor&) { return QColor(10, 20, 30); });
        entry(one, QStringLiteral("Other Colour…"))->trigger();
        QCOMPARE(markup::currentPen().color, QColor(10, 20, 30));
        one.setColorChooser([](const QColor&) { return QColor(); });
        entry(one, QStringLiteral("Other Colour…"))->trigger();
        QCOMPARE(markup::currentPen().color, QColor(10, 20, 30));
        {
            const QucsSettingsFile settings;
            QCOMPARE(settings.value(QStringLiteral("Viewers/penTool")).toString(), QStringLiteral("highlighter"));
            QCOMPARE(settings.value(QStringLiteral("Viewers/penColor")).toString(), QStringLiteral("#0a141e"));
            QCOMPARE(settings.value(QStringLiteral("Viewers/highlighterWidth")).toDouble(), 24.0);
        }
        entry(one, QStringLiteral("Eraser"))->trigger();
        QVERIFY(!entry(one, QStringLiteral("Fine"))->isEnabled());
        entry(one, QStringLiteral("Red"))->trigger();
        QCOMPARE(markup::currentPen().tool, markup::Tool::Pen);
        QVERIFY(entry(two, QStringLiteral("Red"))->isChecked());
        const QImage icon = markup::PenButton::icon(markup::currentPen()).pixmap(32, 32).toImage();
        int red = 0;
        for (int y = 0; y < icon.height(); ++y)
            for (int x = 0; x < icon.width(); ++x)
                if (icon.pixelColor(x, y).alpha() > 200 && near(icon.pixelColor(x, y), QColor(0xe0, 0x1b, 0x24), 30)) ++red;
        QVERIFY2(red > 10, qPrintable(QString::number(red)));
        QVERIFY(!entry(one, QStringLiteral("Undo the Last Change"))->isEnabled());
        one.setDrawingState(true, 2, 5);
        QVERIFY(entry(one, QStringLiteral("Undo the Last Change"))->isEnabled()
                && entry(one, QStringLiteral("Clear the Drawings on This Slide"))->isEnabled());
        one.setDrawingState(false, 0, 5);
        QVERIFY(!entry(one, QStringLiteral("Clear the Drawings on This Slide"))->isEnabled()
                && entry(one, QStringLiteral("Clear All the Drawings"))->isEnabled());
        markup::setCurrentPen(markup::Pen());
    }

    // The pen on a slide. Taken up from the toolbar, a drag over slide 2
    // draws on it in the pen's colour: in points of the slide, so that it
    // zooms with it, as broad as the pen was at the zoom it was drawn at,
    // and no further than its edges - on screen, in the slide's picture at
    // twice the size, copied, printed where the slide lands on the page.
    // A double click draws dots and plays nothing; Ctrl+Z undoes, pressed;
    // the eraser takes the line away; Escape puts the pen down. Kept when
    // the file is read again; the pen's menu clears this slide's, then all.
    void aPenDrawsOnTheSlide()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1100, 800);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        Deck d;
        for (int i = 0; i < 3; ++i) {
            Deck::SlideSpec s;
            s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide %1").arg(i + 1)))), "<p:ph type=\"title\"/>");
            d.slides << s;
        }
        const QString file = dir.filePath("workspace/pen.pptx");
        QDir().mkpath(QFileInfo(file).absolutePath());
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly) && f.write(d.build()) > 0);
        }
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr && doc->penButton() != nullptr);
        pptx::SlideView* view = doc->view();
        QWidget* port = view->viewport();
        const auto send = [port](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons held) {
            QMouseEvent e(type, at, port->mapToGlobal(at), button, held, Qt::NoModifier);
            QCoreApplication::sendEvent(port, &e);
        };
        const auto drag = [&send](QPointF from, QPointF to) {
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 8; ++i) send(QEvent::MouseMove, from + (to - from) * (i / 8.0), Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        };
        const QColor green(0, 160, 0);
        markup::Pen pen;
        pen.color = green;
        pen.width = 6;
        markup::setCurrentPen(pen);
        view->setZoom(1.0);
        doc->goToSlide(1);
        QApplication::processEvents();
        doc->penButton()->click();
        QVERIFY(view->penActive() && doc->penButton()->isChecked());
        const QRect r = view->slideRect(1);
        QCOMPARE(r.width(), 960);   // (a point a pixel)
        drag(r.topLeft() + QPointF(100, 100), r.topLeft() + QPointF(300, 150));
        QCOMPARE(view->drawing().strokes().size(), 1);
        const markup::Stroke s = view->drawing().strokes().first();
        QCOMPARE(s.on, QStringLiteral("2"));
        QVERIFY(QLineF(s.points.first(), QPointF(100, 100)).length() < 1e-9 && QLineF(s.points.last(), QPointF(300, 150)).length() < 1e-9);
        QCOMPARE(s.width, 6.0);
        // On screen, where it was drawn.
        QApplication::processEvents();
        const QImage screen = port->grab().toImage();
        const qreal dpr = screen.devicePixelRatio();
        QVERIFY(near(screen.pixelColor(((r.topLeft() + QPointF(200, 125)) * dpr).toPoint()), green, 30));
        // In the slide's picture at twice the size: where it is, as broad -
        // 3 points either side of the line, measured across it.
        const QImage twice = doc->slideImage(1, 2);
        const QPointF mid(200, 125), across = QPointF(-50, 200) / QLineF(0, 0, -50, 200).length();
        QVERIFY(near(at(twice, mid, 2), green, 30));
        QVERIFY(near(at(twice, mid + 2.2 * across, 2), green, 40));
        QVERIFY(!near(at(twice, mid + 4.5 * across, 2), green, 60));
        int greenOnOne = 0;
        const QImage first = doc->slideImage(0, 1);
        for (int y = 0; y < first.height(); y += 2)
            for (int x = 0; x < first.width(); x += 2)
                if (near(first.pixelColor(x, y), green, 30)) ++greenOnOne;
        QCOMPARE(greenOnOne, 0);
        // Past the slide's right edge a stroke goes on, drawn as far as the
        // edge: none of it in the gap beside the slide (at 40 %, the slide
        // whole in the view).
        {
            view->setZoom(0.4);
            doc->goToSlide(1);
            QApplication::processEvents();
            const QRect half = view->slideRect(1);
            QVERIFY2(half.right() + 12 < port->width(), qPrintable(QStringLiteral("%1 of %2").arg(half.right()).arg(port->width())));
            const qreal y = half.top() + 100;
            drag(QPointF(half.right() - 60, y), QPointF(half.right() + 12, y));
            QApplication::processEvents();
            const QImage edge = port->grab().toImage();
            QVERIFY(near(edge.pixelColor((QPointF(half.right() - 20, y) * dpr).toPoint()), green, 30));
            QVERIFY(!near(edge.pixelColor((QPointF(half.right() + 8, y) * dpr).toPoint()), green, 60));
            QVERIFY(view->undoDrawing());
            view->setZoom(1.0);
            doc->goToSlide(1);
            QApplication::processEvents();
            QCOMPARE(view->slideRect(1), r);
        }
        // A double click draws, and plays nothing: what a widget is given
        // of one, a press, a release, the double click and its release.
        const QPointF dot = r.topLeft() + QPointF(500, 300);
        send(QEvent::MouseButtonPress, dot, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, dot, Qt::LeftButton, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, dot, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, dot, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
        QVERIFY(doc->slideShow() == nullptr);
        QCOMPARE(view->drawing().strokes().size(), 3);
        // Ctrl+Z, pressed: the window's Undo does not take it.
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(view->drawing().strokes().size(), 1);
        // The eraser across the line, and the erasing undone.
        pen.tool = markup::Tool::Eraser;
        markup::setCurrentPen(pen);
        drag(r.topLeft() + QPointF(200, 105), r.topLeft() + QPointF(200, 145));
        QCOMPARE(view->drawing().strokes().size(), 0);
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(view->drawing().strokes().size(), 1);
        // Copied - 1920 pixels wide, two a point - and printed, with it.
        doc->copySlide();
        QVERIFY(near(at(QApplication::clipboard()->image(), mid, 2), green, 30));
        {
            QPrinter printer;
            QImage page(1200, 900, QImage::Format_ARGB32_Premultiplied);
            page.fill(Qt::white);
            QPainter painter(&page);
            doc->print(&printer, &painter, false, true);
            painter.end();
            // (The slide 1200 by 675 in the middle of the page: 1.25 a point.)
            QVERIFY(near(page.pixelColor(QPointF(mid.x() * 1.25, 112.5 + mid.y() * 1.25).toPoint()), green, 40));
        }
        // Escape puts the pen down.
        pen.tool = markup::Tool::Pen;
        markup::setCurrentPen(pen);
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(!view->penActive() && !doc->penButton()->isChecked());
        // Kept when the file is read again.
        QVERIFY(doc->reload());
        QCOMPARE(view->drawing().strokes().size(), 1);
        // The pen's menu: this slide's cleared, then all.
        markup::Stroke other = s;
        other.on = QStringLiteral("1");
        view->addStroke(other);
        QAction* clearHere = nullptr;
        QAction* clearAll = nullptr;
        for (QAction* a : doc->penButton()->penMenu()->actions()) {
            if (a->text() == QStringLiteral("Clear the Drawings on This Slide")) clearHere = a;
            if (a->text() == QStringLiteral("Clear All the Drawings")) clearAll = a;
        }
        QVERIFY(clearHere != nullptr && clearAll != nullptr && clearHere->isEnabled());
        doc->goToSlide(1);
        clearHere->trigger();
        QCOMPARE(view->drawing().strokes().size(), 1);
        QCOMPARE(view->drawing().strokes().first().on, QStringLiteral("1"));
        QVERIFY(!clearHere->isEnabled() && clearAll->isEnabled());
        clearAll->trigger();
        QVERIFY(view->drawing().isEmpty());
        markup::setCurrentPen(markup::Pen());
        app.closeAllFiles();
    }

    // A stroke read as one reads it (markup.h): a dot; a ring, back near
    // where it began; a line, straight or nearly; an arrow drawn at one go,
    // its tip where it turns back, but not a pen lifted with a wobble; a
    // line with a short stroke drawn just after it at one of its ends, an
    // arrow pointing there, the short one its head; a tick, a mark. Its
    // points simplified; how much of a box a ring holds. On a page: a ring
    // round a paragraph whole, or round some of its words; a line under
    // words, or through them; a highlighter over words; an arrow from a
    // word pointing at a picture it stops short of; a dot on a word; a mark
    // on a chart; a dot on nothing. A reading in a sentence, the latest so
    // many of them.
    void strokesAreReadAsDrawn()
    {
        const auto stroke = [](const QPolygonF& points, double width = 2) {
            markup::Stroke s;
            s.on = QStringLiteral("1");
            s.points = points;
            s.width = width;
            s.color = QColor(0xe0, 0x1b, 0x24);
            return s;
        };
        // An ellipse of 23 points round c, a twelfth of it left open.
        const auto ring = [&stroke](QPointF c, double rx, double ry) {
            QPolygonF p;
            for (int k = 0; k < 23; ++k) p << c + QPointF(rx * std::cos(2 * M_PI * k / 24), ry * std::sin(2 * M_PI * k / 24));
            return stroke(p);
        };
        const auto line = [&stroke](QPointF a, QPointF b, int n = 12) {
            QPolygonF p;
            for (int k = 0; k <= n; ++k) p << a + (b - a) * (double(k) / n);
            return stroke(p);
        };
        QPointF tip;
        QCOMPARE(markup::gestureOf(stroke({QPointF(5, 5)})), markup::Gesture::Dot);
        QCOMPARE(markup::gestureOf(stroke({QPointF(5, 5), QPointF(6, 6)})), markup::Gesture::Dot);
        QCOMPARE(markup::gestureOf(ring(QPointF(100, 100), 60, 30)), markup::Gesture::Ring);
        QCOMPARE(markup::gestureOf(line(QPointF(0, 0), QPointF(200, 20)), &tip), markup::Gesture::Line);
        QCOMPARE(tip, QPointF(200, 20));
        markup::Stroke wobbly = line(QPointF(0, 0), QPointF(200, 0));
        wobbly.points[6].ry() += 9;
        QCOMPARE(markup::gestureOf(wobbly), markup::Gesture::Line);
        markup::Stroke oneGo = line(QPointF(0, 0), QPointF(200, 100));
        oneGo.points << QPointF(185, 82) << QPointF(200, 100) << QPointF(180, 103);
        QCOMPARE(markup::gestureOf(oneGo, &tip), markup::Gesture::Arrow);
        QCOMPARE(tip, QPointF(200, 100));
        markup::Stroke lifted = line(QPointF(0, 0), QPointF(200, 0));
        lifted.points << QPointF(198, 1.5);
        QCOMPARE(markup::gestureOf(lifted), markup::Gesture::Line);
        QCOMPARE(markup::gestureOf(stroke({QPointF(0, 50), QPointF(20, 80), QPointF(70, 0)})), markup::Gesture::Mark);
        // One-stroke arrows of the other ways a hand draws them (the RF 2.5D
        // assessment of 2 November: "one-stroke arrows as marks"): both sides
        // of the V out and back from the point, a closed triangle - each its
        // tip at the point. A V of long sides is no arrow but a corner, its
        // point where its sides meet; an L a corner, its point the bend.
        markup::Stroke bothSides = line(QPointF(0, 0), QPointF(200, 0));
        bothSides.points << QPointF(160, 30) << QPointF(200, 0) << QPointF(160, -30);
        QCOMPARE(markup::gestureOf(bothSides, &tip), markup::Gesture::Arrow);
        QCOMPARE(tip, QPointF(200, 0));
        markup::Stroke triangle = line(QPointF(0, 0), QPointF(0, 200));
        triangle.points << QPointF(-25, 160) << QPointF(25, 160) << QPointF(0, 200);
        QCOMPARE(markup::gestureOf(triangle, &tip), markup::Gesture::Arrow);
        QCOMPARE(tip, QPointF(0, 200));
        markup::Stroke vee = line(QPointF(0, 0), QPointF(50, 100));
        vee.points << QPointF(75, 50) << QPointF(100, 0);
        QCOMPARE(markup::gestureOf(vee, &tip), markup::Gesture::Corner);
        QCOMPARE(tip, QPointF(50, 100));
        markup::Stroke ell = line(QPointF(0, 0), QPointF(0, 100));
        ell.points << QPointF(30, 100) << QPointF(60, 100);
        QCOMPARE(markup::gestureOf(ell, &tip), markup::Gesture::Corner);
        QCOMPARE(tip, QPointF(0, 100));
        // Neither a hairpin, its arms 10 degrees apart (the shorter ending
        // far from where it began, or it is a ring), nor a bend of 150
        // degrees: marks.
        markup::Stroke hairpin = line(QPointF(0, 0), QPointF(100, 0));
        hairpin.points << QPointF(70.46, 5.21) << QPointF(40.91, 10.42);
        QCOMPARE(markup::gestureOf(hairpin), markup::Gesture::Mark);
        markup::Stroke open = line(QPointF(0, 0), QPointF(100, 0));
        open.points << QPointF(143.3, 25) << QPointF(186.6, 50);
        QCOMPARE(markup::gestureOf(open), markup::Gesture::Mark);
        // A head drawn after its line, at its end; a short stroke far from
        // either end of the next line, a stroke of its own - a caret, a
        // corner.
        const QList<markup::Stroke> four{line(QPointF(0, 0), QPointF(200, 0)), stroke({QPointF(185, -12), QPointF(200, 0), QPointF(185, 12)}),
                                         line(QPointF(0, 50), QPointF(200, 50)), stroke({QPointF(90, 40), QPointF(100, 50), QPointF(90, 60)})};
        QList<QPointF> tips;
        QList<int> heads;
        QCOMPARE(markup::gesturesOf(four, &tips, &heads),
                 (QList<markup::Gesture>{markup::Gesture::Arrow, markup::Gesture::Arrowhead, markup::Gesture::Line, markup::Gesture::Corner}));
        QCOMPARE(tips.at(0), QPointF(200, 0));
        QCOMPARE(heads, (QList<int>{-1, 0, -1, -1}));
        // A dot at a line's end: a pin, not its head.
        const QList<markup::Stroke> pinned{line(QPointF(0, 0), QPointF(200, 0)), stroke({QPointF(201, 1)})};
        QCOMPARE(markup::gesturesOf(pinned, &tips, &heads), (QList<markup::Gesture>{markup::Gesture::Line, markup::Gesture::Dot}));
        // Drawn from its tip back to its tail, its head at its start.
        const QList<markup::Stroke> backwards{line(QPointF(200, 0), QPointF(0, 0)), stroke({QPointF(185, -12), QPointF(200, 0), QPointF(185, 12)})};
        QCOMPARE(markup::gesturesOf(backwards, &tips, &heads).first(), markup::Gesture::Arrow);
        QCOMPARE(tips.at(0), QPointF(200, 0));
        // Simplified; what a ring holds.
        QCOMPARE(markup::simplified(line(QPointF(0, 0), QPointF(200, 20), 50).points, 0.5), (QPolygonF{QPointF(0, 0), QPointF(200, 20)}));
        const QPolygonF square{QPointF(0, 0),     QPointF(50, 0),  QPointF(100, 0), QPointF(100, 50),
                               QPointF(100, 100), QPointF(50, 100), QPointF(0, 100), QPointF(0, 50)};
        QCOMPARE(markup::simplified(square, 1), (QPolygonF{QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100), QPointF(0, 50)}));
        QCOMPARE(markup::insideShare(square, QRectF(20, 20, 30, 30)), 1.0);
        QCOMPARE(markup::insideShare(square, QRectF(60, 20, 100, 30)), 0.4);
        QVERIFY(markup::insideShare(square, QRectF(200, 200, 10, 10)) == 0);

        // A page: a paragraph of five words, a picture, a chart.
        const auto box = [](double l, double t, double r, double b) { return QPolygonF(QRectF(QPointF(l, t), QPointF(r, b))); };
        markup::Target paragraph{QStringLiteral("paragraph"), QStringLiteral("The gain rises with frequency"), box(100, 100, 450, 130), {}};
        const std::pair<double, double> spans[] = {{100, 140}, {150, 200}, {210, 270}, {280, 330}, {340, 450}};
        const QStringList words = paragraph.text.split(QLatin1Char(' '));
        for (int i = 0; i < 5; ++i) paragraph.words << std::make_pair(words.at(i), box(spans[i].first, 100, spans[i].second, 130));
        const QList<markup::Target> page{paragraph, {QStringLiteral("picture"), QStringLiteral("Bode plot"), box(600, 100, 900, 400), {}},
                                         {QStringLiteral("chart"), QStringLiteral("Noise figure"), box(100, 300, 400, 500), {}}};
        markup::Stroke band = line(QPointF(98, 115), QPointF(202, 115));
        band.width = 14;
        band.highlighter = true;
        band.color = QColor(0xf6, 0xd3, 0x2d, 102);
        markup::Stroke arrow = line(QPointF(440, 118), QPointF(585, 200));
        arrow.points << QPointF(570, 197) << QPointF(585, 200) << QPointF(575, 186);
        markup::Stroke orange = stroke({QPointF(50, 50)});
        orange.color = QColor(0xff, 0x78, 0x00);
        const QList<markup::Stroke> drawn{ring(QPointF(275, 115), 200, 40),
                                          ring(QPointF(210, 115), 80, 25),
                                          line(QPointF(205, 133), QPointF(335, 134)),
                                          line(QPointF(335, 116), QPointF(455, 114)),
                                          band,
                                          arrow,
                                          stroke({QPointF(300, 115)}),
                                          stroke({QPointF(200, 400), QPointF(220, 430), QPointF(270, 350)}),
                                          orange};
        const QJsonArray read = markup::readPage(drawn, page, true);
        QCOMPARE(read.size(), drawn.size());
        const auto reading = [&read](int stroke) { return read.at(stroke - 1).toObject(); };
        const QJsonObject whole{{"paragraph", "The gain rises with frequency"}};
        const auto some = [](const QString& words) { return QJsonObject{{"words", words}, {"in paragraph", "The gain rises with frequency"}}; };
        QCOMPARE(reading(1).value("drawn as").toString(), QStringLiteral("ring"));
        QCOMPARE(reading(1).value("around").toArray(), QJsonArray{whole});
        QCOMPARE(reading(2).value("around").toArray(), QJsonArray{some("gain rises")});   // (a tenth of "The" and "with" in it)
        QCOMPARE(reading(3).value("drawn as").toString(), QStringLiteral("line"));
        QCOMPARE(reading(3).value("underlines").toArray(), QJsonArray{some("rises with")});
        QVERIFY(!reading(3).contains("strikes through") && !reading(3).contains("points at"));
        QCOMPARE(reading(4).value("strikes through").toArray(), QJsonArray{some("frequency")});
        QVERIFY(!reading(4).contains("underlines"));
        QCOMPARE(reading(5).value("tool").toString(), QStringLiteral("highlighter"));
        QCOMPARE(reading(5).value("colour").toString(), QStringLiteral("#f6d32d"));
        QCOMPARE(reading(5).value("highlights").toArray(), QJsonArray{some("The gain")});
        QCOMPARE(reading(6).value("drawn as").toString(), QStringLiteral("arrow"));
        QCOMPARE(reading(6).value("from").toObject(), some("frequency"));
        QCOMPARE(reading(6).value("points at").toObject(), (QJsonObject{{"picture", "Bode plot"}}));
        QCOMPARE(reading(7).value("drawn as").toString(), QStringLiteral("dot"));
        QCOMPARE(reading(7).value("on").toObject(), some("with"));
        QCOMPARE(reading(7).value("box").toArray(), (QJsonArray{299, 114, 301, 116}));
        QCOMPARE(reading(8).value("drawn as").toString(), QStringLiteral("mark"));
        QCOMPARE(reading(8).value("on").toArray(), (QJsonArray{QJsonObject{{"chart", "Noise figure"}}}));
        QVERIFY(!reading(9).contains("on"));
        for (int i = 1; i <= read.size(); ++i) QCOMPARE(reading(i).value("stroke").toInt(), i);
        // Its points: a line's two ends, a ring's few.
        QCOMPARE(reading(3).value("points").toArray(), (QJsonArray{QJsonArray{205, 133}, QJsonArray{335, 134}}));
        const int few = int(reading(1).value("points").toArray().size());
        QVERIFY2(few >= 6 && few <= 32, qPrintable(QString::number(few)));
        QVERIFY(!markup::readPage(drawn, page, false).at(0).toObject().contains("points"));
        // In sentences: on slide 3, the latest three, the earlier counted.
        QJsonArray onSlide;
        for (const QJsonValue& v : read) {
            QJsonObject o = v.toObject();
            o.insert("slide", 3);
            onSlide.append(o);
        }
        const QStringList lines = markup::sentencesOf(onSlide, true).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), drawn.size());
        QVERIFY2(lines.at(0).startsWith("slide 3, stroke 1: a red pen ring around the paragraph \"The gain rises with frequency\" (its box "), qPrintable(lines.at(0)));
        QVERIFY2(lines.at(0).endsWith(" points)"), qPrintable(lines.at(0)));
        QVERIFY2(lines.at(1).contains("ring around \"gain rises\" in the paragraph \"The gain rises with frequency\""), qPrintable(lines.at(1)));
        QVERIFY2(lines.at(2).contains("line under \"rises with\" in the paragraph"), qPrintable(lines.at(2)));
        QVERIFY2(lines.at(3).contains("line through \"frequency\""), qPrintable(lines.at(3)));
        QVERIFY2(lines.at(4).contains("a yellow highlighter line over \"The gain\""), qPrintable(lines.at(4)));
        QVERIFY2(lines.at(5).contains("arrow from \"frequency\" in the paragraph \"The gain rises with frequency\" pointing at the picture \"Bode plot\""),
                 qPrintable(lines.at(5)));
        QVERIFY2(lines.at(7).contains("mark on the chart \"Noise figure\""), qPrintable(lines.at(7)));
        QVERIFY2(lines.at(8).startsWith("slide 3, stroke 9: an orange pen dot (its box"), qPrintable(lines.at(8)));
        const QStringList latest = markup::sentencesOf(onSlide, true, 3).split(QLatin1Char('\n'));
        QCOMPARE(latest.size(), 4);
        QVERIFY2(latest.at(0).contains("6 earlier stroke(s) left out"), qPrintable(latest.at(0)));
        QVERIFY(latest.at(1).startsWith("slide 3, stroke 7:"));
        // A corner, its point on a word: at that word, and its point given.
        markup::Stroke bend = line(QPointF(240, 60), QPointF(240, 115));
        bend.points << QPointF(265, 115) << QPointF(290, 115);
        QJsonObject corner = markup::readPage({bend}, page, false).at(0).toObject();
        QCOMPARE(corner.value("drawn as").toString(), QStringLiteral("corner"));
        QCOMPARE(corner.value("at").toObject(), some("rises"));
        QCOMPARE(corner.value("point").toArray(), (QJsonArray{240, 115}));
        corner.insert("slide", 3);
        const QString said = markup::sentencesOf(QJsonArray{corner}, true);
        QVERIFY2(said.contains("a red pen corner at \"rises\" in the paragraph \"The gain rises with frequency\""), qPrintable(said));
    }

    // A slide's parts where they are drawn (Presentation::parts()), what a
    // mark on it is read against: its paragraphs and their words, under
    // which its text's ink lies and none outside them - the title's, a text
    // box's two, a text turned with its shape, one in a group where the
    // group puts it, a table's cells; a picture by its alt text, else its
    // name; a chart by its title.
    void slidePartsAreWhereTheyAreDrawn()
    {
        Deck d;
        QImage red(20, 20, QImage::Format_RGB32);
        red.fill(Qt::red);
        d.extra << zip::Entry{QStringLiteral("ppt/media/image1.png"), png(red)};
        d.extra << zip::Entry{QStringLiteral("ppt/charts/chart1.xml"),
                              "<?xml version=\"1.0\"?><c:chartSpace " + ns() + "><c:chart><c:title><c:tx><c:rich><a:bodyPr/><a:p><a:r><a:t>Noise figure</a:t></a:r></a:p>"
                              "</c:rich></c:tx></c:title><c:autoTitleDeleted val=\"0\"/><c:plotArea><c:pieChart><c:varyColors val=\"1\"/><c:ser><c:idx val=\"0\"/><c:val>"
                              "<c:numRef><c:numCache><c:ptCount val=\"2\"/><c:pt idx=\"0\"><c:v>1</c:v></c:pt><c:pt idx=\"1\"><c:v>1</c:v></c:pt></c:numCache></c:numRef>"
                              "</c:val></c:ser></c:pieChart></c:plotArea></c:chart></c:chartSpace>"};
        const QByteArray bare = "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln>";
        const auto pic = [](int id, const QByteArray& cNvPr, const QRectF& at) {
            return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + QByteArray::number(id) + "\" " + cNvPr + "/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rIdP1\"/>"
                   "<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + xfrm(at.x(), at.y(), at.width(), at.height()) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        };
        const auto cell = [](const QString& t) { return "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/>" + para(run(t)) + "</a:txBody><a:tcPr/></a:tc>"; };
        Deck::SlideSpec s;
        s.tree = shape(2, QByteArray(), text(para(run("Results of the run"))), "<p:ph type=\"title\"/>")
                 + shape(3, xfrm(60, 140, 400, 110) + bare, text(para(run("alpha beta gamma")) + para(run("delta"))))
                 + pic(4, "name=\"Picture 4\" descr=\"Bode plot\"", QRectF(520, 140, 200, 120)) + pic(5, "name=\"Picture 5\"", QRectF(760, 140, 120, 120))
                 + "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"6\" name=\"t\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" + emu(60)
                 + "\" y=\"" + emu(280) + "\"/><a:ext cx=\"" + emu(400) + "\" cy=\"" + emu(60) + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\">"
                   "<a:tbl><a:tblPr/><a:tblGrid><a:gridCol w=\"" + emu(200) + "\"/><a:gridCol w=\"" + emu(200) + "\"/></a:tblGrid><a:tr h=\"" + emu(60) + "\">" + cell("a1 x")
                 + cell("b1") + "</a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>"
                 + "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"7\" name=\"c\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" + emu(520)
                 + "\" y=\"" + emu(290) + "\"/><a:ext cx=\"" + emu(200) + "\" cy=\"" + emu(120) + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">"
                   "<c:chart r:id=\"rIdC1\"/></a:graphicData></a:graphic></p:graphicFrame>"
                 + shape(8, xfrm(760, 330, 160, 40, " rot=\"5400000\"") + bare, text(para(run("turned"))))
                 + "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"9\" name=\"g\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"" + emu(60) + "\" y=\"" + emu(380)
                 + "\"/><a:ext cx=\"" + emu(200) + "\" cy=\"" + emu(100) + "\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"" + emu(400) + "\" cy=\"" + emu(200)
                 + "\"/></a:xfrm></p:grpSpPr>" + shape(10, xfrm(0, 0, 400, 200) + bare, text(para(run("grouped words")))) + "</p:grpSp>"
                 + shape(11, xfrm(280, 430, 220, 50) + bare,
                         text(para(run("one two"), "<a:pPr marL=\"0\" indent=\"0\"><a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/></a:pPr>")));
        s.rels = "<Relationship Id=\"rIdP1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image1.png\"/>"
                 "<Relationship Id=\"rIdC1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" Target=\"../charts/chart1.xml\"/>";
        d.slides << s;
        const auto deck = readDeck(d);
        QVERIFY(deck != nullptr);
        const QList<pptx::SlidePart> parts = deck->parts(0);
        QStringList said;
        for (const pptx::SlidePart& p : parts) said << QStringLiteral("%1 %2").arg(int(p.kind)).arg(p.text);
        QCOMPARE(said, (QStringList{"0 Results of the run", "0 alpha beta gamma", "0 delta", "2 Bode plot", "2 Picture 5", "1 a1 x", "1 b1", "3 Noise figure",
                                    "0 turned", "0 grouped words", "0 one two"}));
        QCOMPARE(parts.at(3).outline.boundingRect(), QRectF(520, 140, 200, 120));
        QCOMPARE(parts.at(7).outline.boundingRect(), QRectF(520, 290, 200, 120));
        QCOMPARE(parts.at(5).outline.boundingRect(), QRectF(60, 280, 200, 60));
        // The words, each with ink under it, in turn along their line.
        const QImage image = deck->render(0, 1);
        for (const int i : {0, 1, 2, 5, 6, 8, 9, 10}) {
            const pptx::SlidePart& p = parts.at(i);
            QCOMPARE(p.words.size(), p.text.split(QLatin1Char(' ')).size());
            for (int w = 0; w < p.words.size(); ++w) {
                QCOMPARE(p.words.at(w).first, p.text.split(QLatin1Char(' ')).at(w));
                QVERIFY2(inked(image, p.words.at(w).second.boundingRect()) > 3, qPrintable(p.words.at(w).first));
                if (w > 0 && i != 8)
                    QVERIFY2(p.words.at(w - 1).second.boundingRect().right() <= p.words.at(w).second.boundingRect().left() + 0.01, qPrintable(p.words.at(w).first));
            }
        }
        // Turned with its shape: the word down the slide.
        const QRectF turned = parts.at(8).words.first().second.boundingRect();
        QVERIFY2(turned.height() > 2 * turned.width() && QRectF(820, 270, 40, 160).contains(turned.center()), qPrintable(QStringLiteral("%1 %2 %3 %4").arg(turned.x()).arg(turned.y()).arg(turned.width()).arg(turned.height())));
        // A text's ink, none of it outside its words (a point and a half more).
        const auto outside = [&image](const QRectF& region, const QList<pptx::SlidePart>& of) {
            int n = 0;
            const QRect r = region.toRect().intersected(image.rect());
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    if (near(image.pixelColor(x, y), Qt::white, 40)) continue;
                    bool under = false;
                    for (const pptx::SlidePart& p : of)
                        for (const auto& [word, at] : p.words) under = under || at.boundingRect().adjusted(-1.5, -1.5, 1.5, 1.5).contains(QPointF(x + 0.5, y + 0.5));
                    if (!under) ++n;
                }
            return n;
        };
        // ... nor its words much broader than its ink: a word's box is its
        // advance, its line's height.
        const auto fits = [&image](const QRectF& region, const QList<pptx::SlidePart>& of) {
            QRect ink;
            const QRect r = region.toRect().intersected(image.rect());
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x)
                    if (!near(image.pixelColor(x, y), Qt::white, 40)) ink |= QRect(x, y, 1, 1);
            QRectF words;
            for (const pptx::SlidePart& p : of)
                for (const auto& [word, at] : p.words) words |= at.boundingRect();
            return std::max(ink.width(), ink.height()) >= 0.8 * std::max(words.width(), words.height())
                   && std::min(ink.width(), ink.height()) >= 0.5 * std::min(words.width(), words.height());
        };
        const std::pair<QRectF, QList<pptx::SlidePart>> texts[] = {{QRectF(48, 30, 864, 80), {parts.at(0)}},
                                                                    {QRectF(60, 140, 400, 110), {parts.at(1), parts.at(2)}},
                                                                    {QRectF(820, 270, 40, 160), {parts.at(8)}},
                                                                    {QRectF(60, 380, 200, 100), {parts.at(9)}}};
        for (const auto& [region, of] : texts) {
            QCOMPARE(outside(region, of), 0);
            QVERIFY2(fits(region, of), qPrintable(of.first().text));
        }
        // A bullet before its text, not one of its words.
        QCOMPARE(parts.at(10).words.first().first, QStringLiteral("one"));
    }

    // What the user drew with the pen, as Claude's tools have it, with no
    // picture: read_pptx's and show_pptx's tab, stroke by stroke - its
    // slide, how it was drawn, its tool, colour and box in points from the
    // slide's top left, what it marks (a ring round slide 2's title); and
    // read_drawings, every open deck's and layout's with each stroke's
    // points, or one document's, refused for one not open or not a deck or
    // a layout. Its pictures too: screenshot's of a slide with it, and
    // read_pptx's. show_pptx takes the pen up and puts it down, and clears
    // the drawings; a 'pen' or 'drawings' of another kind is refused,
    // nothing done.
    void claudeSeesWhatThePenDrew()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        auto* control = app.findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const auto call = [control](const QString& tool, const QJsonObject& args) { return control->callNow(tool, args, 30000); };
        const auto json = [](const QJsonObject& r) { return QJsonDocument::fromJson(QucsControl::textOf(r).section(QLatin1Char('\n'), 0, 0).toUtf8()).object(); };
        const auto failed = [](const QJsonObject& r) { return r.value("isError").toBool(); };
        const auto picture = [](const QJsonObject& r) {
            for (const QJsonValue& v : r.value("content").toArray())
                if (v.toObject().value("type").toString() == "image")
                    return QImage::fromData(QByteArray::fromBase64(v.toObject().value("data").toString().toLatin1()), "PNG");
            return QImage();
        };
        Deck d;
        for (int i = 0; i < 3; ++i) {
            Deck::SlideSpec s;
            s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide %1").arg(i + 1)))), "<p:ph type=\"title\"/>");
            d.slides << s;
        }
        const QString file = dir.filePath("workspace/drawn.pptx");
        QVERIFY(write(file, d.build()));
        QJsonObject o = json(call("show_pptx", {{"path", "drawn.pptx"}}));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QVERIFY(!o.contains("drawings") && !o.contains("pen"));
        QVERIFY(json(call("read_drawings", {})).value("drawings").toString().contains(QStringLiteral("Nothing is drawn")));
        QVERIFY(!control->inEveryTurn().contains("read_drawings"));
        // A line and a ring round the title on slide 2, a highlighter's on
        // slide 3.
        markup::Stroke line;
        line.on = QStringLiteral("2");
        line.points = {QPointF(100, 100), QPointF(300, 150)};
        line.color = QColor(0, 160, 0);
        line.width = 8;
        doc->view()->addStroke(line);
        markup::Stroke ring;
        ring.on = QStringLiteral("2");
        ring.points = {QPointF(40, 20),  QPointF(480, 18),  QPointF(920, 20), QPointF(922, 70),
                       QPointF(920, 120), QPointF(480, 122), QPointF(40, 120), QPointF(38, 40)};
        ring.color = QColor(0xe0, 0x1b, 0x24);
        ring.width = 3;
        doc->view()->addStroke(ring);
        markup::Stroke band = line;
        band.on = QStringLiteral("3");
        band.color = QColor(246, 211, 45, 102);
        band.highlighter = true;
        doc->view()->addStroke(band);
        o = json(call("read_pptx", {{"path", "drawn.pptx"}, {"slides", 1}})).value("its tab").toObject();
        const QJsonArray drawn = o.value("drawings").toArray();
        QCOMPARE(drawn.size(), 3);
        const QJsonObject first = drawn.at(0).toObject();
        QCOMPARE(first.value("slide").toInt(), 2);
        QCOMPARE(first.value("stroke").toInt(), 1);
        QCOMPARE(first.value("drawn as").toString(), QStringLiteral("line"));
        QCOMPARE(first.value("tool").toString(), QStringLiteral("pen"));
        QCOMPARE(first.value("colour").toString(), QStringLiteral("#00a000"));
        QCOMPARE(first.value("box").toArray(), (QJsonArray{96, 96, 304, 154}));
        QVERIFY(!first.contains("points"));
        QCOMPARE(drawn.at(1).toObject().value("drawn as").toString(), QStringLiteral("ring"));
        QCOMPARE(drawn.at(1).toObject().value("around").toArray(), (QJsonArray{QJsonObject{{"paragraph", "Slide 2"}}}));
        QCOMPARE(drawn.at(2).toObject().value("slide").toInt(), 3);
        QCOMPARE(drawn.at(2).toObject().value("tool").toString(), QStringLiteral("highlighter"));
        QCOMPARE(drawn.at(2).toObject().value("colour").toString(), QStringLiteral("#f6d32d"));
        // read_drawings: each stroke's points too, every deck's or this one's.
        const QJsonArray documents = json(call("read_drawings", {})).value("documents").toArray();
        QCOMPARE(documents.size(), 1);
        QCOMPARE(documents.at(0).toObject().value("document").toString(), QStringLiteral("drawn.pptx"));
        QVERIFY(documents.at(0).toObject().value("measures").toString().contains(QStringLiteral("points")));
        const QJsonArray strokes = documents.at(0).toObject().value("strokes").toArray();
        QCOMPARE(strokes.size(), 3);
        QCOMPARE(strokes.at(0).toObject().value("points").toArray(), (QJsonArray{QJsonArray{100, 100}, QJsonArray{300, 150}}));
        QCOMPARE(strokes.at(1).toObject().value("around"), drawn.at(1).toObject().value("around"));
        const QJsonObject brief = json(call("read_drawings", {{"path", "drawn.pptx"}, {"points", false}}));
        QCOMPARE(brief.value("documents").toArray().at(0).toObject().value("strokes").toArray().size(), 3);
        QVERIFY(!brief.value("documents").toArray().at(0).toObject().value("strokes").toArray().at(0).toObject().contains("points"));
        QVERIFY(failed(call("read_drawings", {{"path", "elsewhere.pptx"}})));
        QVERIFY(control->inEveryTurn().contains("read_drawings"));
        // What a prompt from the Claude Code panel tells of it: the
        // drawing's revision, a stroke a line.
        const auto [revision, told] = app.drawingsOf(doc->getDocName());
        QVERIFY(revision > 0 && revision == doc->view()->drawing().revision());
        QCOMPARE(told.count(QLatin1Char('\n')), 2);
        QVERIFY2(told.section(QLatin1Char('\n'), 1, 1).startsWith(QStringLiteral("slide 2, stroke 2: a red pen ring around the paragraph \"Slide 2\"")), qPrintable(told));
        QCOMPARE(app.drawingsOf(dir.filePath("workspace/elsewhere.pptx")), std::make_pair(quint64(0), QString()));
        QCOMPARE(app.drawingsOf(doc->getDocName(), revision), std::make_pair(revision, QString()));   // (unchanged: not read)
        // Its pictures with it: screenshot's of slide 2, read_pptx's.
        const QJsonObject shot = call("screenshot", {{"slide", 2}});
        QVERIFY(!failed(shot));
        const QImage seen = picture(shot);
        QVERIFY(!seen.isNull());
        const qreal scale = seen.width() / 960.0;
        QVERIFY(near(seen.pixelColor(QPointF(200 * scale, 125 * scale).toPoint()), QColor(0, 160, 0), 30));
        QVERIFY(near(picture(call("screenshot", {{"slide", 1}})).pixelColor(QPointF(200 * scale, 125 * scale).toPoint()), Qt::white, 30));
        const QImage read = picture(call("read_pptx", {{"path", "drawn.pptx"}, {"slides", 2}, {"image", true}}));
        QVERIFY(near(read.pixelColor(QPointF(200 * read.width() / 960.0, 125 * read.width() / 960.0).toPoint()), QColor(0, 160, 0), 30));
        // The pen taken up and put down; refused of another kind.
        o = json(call("show_pptx", {{"pen", true}}));
        QVERIFY(doc->view()->penActive() && doc->penButton()->isChecked());
        QVERIFY(o.value("pen").toString().contains(QStringLiteral("taken up")));
        QVERIFY(failed(call("show_pptx", {{"pen", "yes"}, {"slide", 3}})));
        QCOMPARE(doc->currentSlide(), 0);
        QVERIFY(failed(call("show_pptx", {{"drawings", "all"}})));
        QCOMPARE(doc->view()->drawing().strokes().size(), 3);
        o = json(call("show_pptx", {{"pen", false}, {"drawings", "clear"}}));
        QVERIFY(!doc->view()->penActive() && doc->view()->drawing().isEmpty());
        QVERIFY(!o.contains("drawings") && !o.contains("pen"));
        QVERIFY2(o.value("done").toString().contains(QStringLiteral("taken away")), qPrintable(o.value("done").toString()));
        QVERIFY(!control->inEveryTurn().contains("read_drawings"));
        // Many strokes: a tab's answer has the latest 40, read_drawings the
        // latest 200, the earlier counted.
        for (int i = 0; i < 245; ++i) {
            markup::Stroke dot = line;
            dot.on = QStringLiteral("1");
            dot.points = {QPointF(400 + i, 400)};
            doc->view()->addStroke(dot);
        }
        o = json(call("read_pptx", {{"path", "drawn.pptx"}, {"slides", 1}})).value("its tab").toObject();
        QCOMPARE(o.value("drawings").toArray().size(), 40);
        QCOMPARE(o.value("drawings").toArray().first().toObject().value("stroke").toInt(), 206);
        QVERIFY2(o.value("drawings left out").toString().startsWith(QStringLiteral("205 earlier")), qPrintable(o.value("drawings left out").toString()));
        const QJsonObject many = json(call("read_drawings", {{"points", false}})).value("documents").toArray().at(0).toObject();
        QCOMPARE(many.value("strokes").toArray().size(), 200);
        QVERIFY2(many.value("left out").toString().startsWith(QStringLiteral("45 earlier")), qPrintable(many.value("left out").toString()));
        // read_drawings of a document the pen does not draw on: refused.
        const QString notes = dir.filePath("workspace/notes.txt");
        QVERIFY(write(notes, "a note\n"));
        QVERIFY(app.gotoPage(notes));
        const QJsonObject refused = call("read_drawings", {{"path", "notes.txt"}});
        QVERIFY2(failed(refused) && QucsControl::textOf(refused).contains(QStringLiteral("not a layout or a presentation")), qPrintable(QucsControl::textOf(refused)));
        app.closeAllFiles();
    }

    // The pen drawn with as the user's mouse draws, through Claude's raw
    // input (the check of G8, G9 and the pens of 2 November: "send_input
    // has no target for a layout or slide view ... Wanted: a layout/slide
    // target for send_input"): 'target' slide, its points the slide's from
    // its top left - 'drag_through' a ring round slide 2's title, drawn by
    // the mouse's events and read in the answer as read_drawings reads it;
    // 'drag_to' a line on slide 3, scrolled to; its keys to the slides,
    // Ctrl+Z undoing and Escape putting the pen down; with the pen down a
    // drag draws nothing; a slide that is none and two drags refused.
    void theRawInputDrawsOnASlide()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1200, 800);
        app.show();
        auto* control = app.findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const auto call = [control](const QString& tool, const QJsonObject& args) { return control->callNow(tool, args, 30000); };
        const auto json = [](const QJsonObject& r) { return QJsonDocument::fromJson(QucsControl::textOf(r).section(QLatin1Char('\n'), 0, 0).toUtf8()).object(); };
        const auto failed = [](const QJsonObject& r) { return r.value("isError").toBool(); };
        Deck d;
        for (int i = 0; i < 3; ++i) {
            Deck::SlideSpec s;
            s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide %1").arg(i + 1)))), "<p:ph type=\"title\"/>");
            d.slides << s;
        }
        QVERIFY(write(dir.filePath("workspace/input.pptx"), d.build()));
        QVERIFY(!failed(call("show_pptx", {{"path", "input.pptx"}, {"pen", true}})));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr && doc->view()->penActive());
        QJsonArray ring;
        for (int k = 1; k <= 36; ++k) ring.append(QJsonArray{480 + 450 * std::cos(2 * M_PI * k / 36), 70 + 56 * std::sin(2 * M_PI * k / 36)});
        // The view changed after the points are mapped and before the
        // mouse's events go - as on Linux, where Qt shows the scroll bar by
        // a queued call and the slides, fitted to the width, are fitted
        // again to the narrower viewport (CI of d7facd8: the stroke's first
        // point at 953 for 930): they go where the points are then.
        qucs_s::pptx::SlideView* view = doc->view();
        const qreal before = view->zoom();
        QMetaObject::invokeMethod(view, [view] { view->setZoom(view->zoom() * 0.8); }, Qt::QueuedConnection);
        QJsonObject r = call("send_input", {{"target", "slide"}, {"slide", 2}, {"click", QJsonArray{930, 70}}, {"drag_through", ring}});
        QVERIFY2(view->zoom() < 0.9 * before, qPrintable(QStringLiteral("%1 then %2").arg(before).arg(view->zoom())));
        QVERIFY2(!failed(r), qPrintable(QucsControl::textOf(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("on").toString(), QStringLiteral("slide 2 of input.pptx"));
        QVERIFY2(QucsControl::textOf(r).contains(QStringLiteral("points of it, from its top left")), qPrintable(QucsControl::textOf(r)));
        QCOMPARE(o.value("drawn").toArray().size(), 1);
        const QJsonObject drawn = o.value("drawn").toArray().at(0).toObject();
        QCOMPARE(drawn.value("slide").toInt(), 2);
        QCOMPARE(drawn.value("drawn as").toString(), QStringLiteral("ring"));
        QCOMPARE(drawn.value("around").toArray(), (QJsonArray{QJsonObject{{"paragraph", "Slide 2"}}}));
        // Where the mouse went: the stroke's points the ones asked, to a
        // pixel's worth of the slide.
        QCOMPARE(doc->view()->drawing().count(QStringLiteral("2")), 1);
        const markup::Stroke stroke = doc->view()->drawing().on(QStringLiteral("2")).first();
        const double pixel = 1.0 / doc->view()->zoom();
        QVERIFY2(QLineF(stroke.points.first(), QPointF(930, 70)).length() <= pixel, qPrintable(QString::number(stroke.points.first().x())));
        double far = 0;
        for (const QPointF& p : stroke.points) far = std::max(far, std::abs(std::hypot((p.x() - 480) / 450, (p.y() - 70) / 56) - 1));
        QVERIFY2(far < 0.05, qPrintable(QString::number(far)));
        // A line on slide 3, which is scrolled to; Ctrl+Z takes it away.
        r = call("send_input", {{"target", "slide"}, {"slide", 3}, {"click", QJsonArray{100, 300}}, {"drag_to", QJsonArray{500, 300}}});
        QCOMPARE(json(r).value("drawn").toArray().at(0).toObject().value("drawn as").toString(), QStringLiteral("line"));
        QCOMPARE(doc->view()->drawing().count(QStringLiteral("3")), 1);
        r = call("send_input", {{"target", "slide"}, {"keys", "Ctrl+Z"}});
        QVERIFY2(json(r).value("taken away").toString().startsWith(QStringLiteral("1 of the pen's")), qPrintable(QucsControl::textOf(r)));
        QCOMPARE(doc->view()->drawing().count(QStringLiteral("3")), 0);
        // Escape puts the pen down: a drag then draws nothing.
        QVERIFY(!failed(call("send_input", {{"target", "slide"}, {"keys", "Escape"}})));
        QVERIFY(!doc->view()->penActive());
        r = call("send_input", {{"target", "slide"}, {"slide", 1}, {"click", QJsonArray{100, 300}}, {"drag_to", QJsonArray{500, 300}}});
        QVERIFY2(!failed(r) && !json(r).contains("drawn"), qPrintable(QucsControl::textOf(r)));
        QCOMPARE(doc->view()->drawing().strokes().size(), 1);
        // Refused, nothing sent: a slide that is none, two drags at once.
        r = call("send_input", {{"target", "slide"}, {"slide", 4}, {"click", QJsonArray{1, 1}}});
        QVERIFY2(failed(r) && QucsControl::textOf(r).contains(QStringLiteral("'slide' is a slide's number, from 1 to 3")), qPrintable(QucsControl::textOf(r)));
        QJsonArray one;   // (a list of one point: appended, as a braced one is a copy on some compilers)
        one.append(QJsonArray{3, 3});
        QCOMPARE(one.at(0).toArray().size(), 2);
        QVERIFY(failed(call("send_input", {{"target", "slide"}, {"click", QJsonArray{1, 1}}, {"drag_to", QJsonArray{2, 2}}, {"drag_through", one}})));
        app.closeAllFiles();
    }

    void showDeckGiven()
    {
        const QString file = qEnvironmentVariable("QUCS_PPTX_RENDER").section(QLatin1Char(';'), 0, 0);
        const QString out = qEnvironmentVariable("QUCS_PPTX_OUT");
        if (file.isEmpty() || out.isEmpty() || out == QLatin1String("-")) QSKIP("QUCS_PPTX_RENDER and QUCS_PPTX_OUT name a deck and where its picture goes");
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1400, 900);
        app.show();
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        doc->goToSlide(qEnvironmentVariable("QUCS_PPTX_SLIDE").toInt() - 1);
        QTest::qWait(400);   // (the thumbnails drawn)
        QDir().mkpath(out);
        QVERIFY(app.grab().save(QDir(out).filePath(QStringLiteral("window.png"))));
        app.closeAllFiles();
    }

    // Decks of one's own drawn into a folder, to be looked at: the files in
    // QUCS_PPTX_RENDER (separated by ';'), each slide into QUCS_PPTX_OUT
    // ("-": drawn, not kept).
    void renderDecksGiven()
    {
        const QStringList files = qEnvironmentVariable("QUCS_PPTX_RENDER").split(QLatin1Char(';'), Qt::SkipEmptyParts);
        const QString out = qEnvironmentVariable("QUCS_PPTX_OUT");
        if (files.isEmpty() || out.isEmpty()) QSKIP("QUCS_PPTX_RENDER and QUCS_PPTX_OUT name decks to draw and where");
        if (out != QLatin1String("-")) QDir().mkpath(out);
        const qreal scale = qEnvironmentVariableIsSet("QUCS_PPTX_SCALE") ? qEnvironmentVariable("QUCS_PPTX_SCALE").toDouble() : 1.0;
        const bool save = out != QLatin1String("-");
        for (const QString& file : files) {
            qInfo("deck: %s", qPrintable(file));
            QElapsedTimer clock;
            clock.start();
            QString why;
            const auto deck = pptx::Presentation::read(read(file), &why);
            if (deck == nullptr) {
                qInfo("%s: not read: %s", qPrintable(QFileInfo(file).fileName()), qPrintable(why));
                continue;
            }
            const qint64 readMs = clock.elapsed();
            qint64 drawMs = 0;
            for (int i = 0; i < deck->slideCount(); ++i) {
                clock.restart();
                const QImage image = deck->render(i, scale);
                drawMs += clock.elapsed();
                QVERIFY(!image.isNull());
                if (save) image.save(QDir(out).filePath(QStringLiteral("%1-%2.png").arg(QFileInfo(file).completeBaseName()).arg(i + 1, 2, 10, QLatin1Char('0'))));
                const pptx::SlideInfo& s = deck->slide(i);
                if (!s.notShown.isEmpty()) qInfo("%s %d: not shown: %s", qPrintable(QFileInfo(file).fileName()), i + 1, qPrintable(s.notShown.join(" | ")));
            }
            qInfo("%s: %d slides, read %lld ms, drawn %lld ms", qPrintable(QFileInfo(file).fileName()), deck->slideCount(), readMs, drawMs);
        }
    }

    // Opened, the slides start where the thumbnails end, and at "fit
    // width" a slide is as wide as the view, a gap each side. The
    // thumbnails' pane used to be as wide as a list's size hint (256) and
    // the list no wider than its maximum (210): a strip of nothing between
    // them and the slides, until the splitter was moved (seen on the
    // screen, 2026-11-08).
    void opensAsWideAsTheView()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1300, 850);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        Deck d;
        for (int i = 0; i < 2; ++i) {
            Deck::SlideSpec s;
            s.tree = shape(2, QByteArray(), text(para(run(QStringLiteral("Slide %1").arg(i + 1)))), "<p:ph type=\"title\"/>");
            d.slides << s;
        }
        const QString file = dir.filePath("workspace/wide.pptx");
        QVERIFY(write(file, d.build()));
        QVERIFY(app.gotoPage(file));
        auto* doc = qobject_cast<PptxDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        auto* view = doc->view();
        auto* list = doc->thumbnails();
        auto* splitter = qobject_cast<QSplitter*>(list->parentWidget());
        QVERIFY(splitter != nullptr);
        const auto check = [&](const QString& when) {
            for (int i = 0; i < 5; ++i) QApplication::processEvents();
            QWidget* body = splitter->widget(1);
            if (list->isVisible()) {
                const int gap = body->x() - list->geometry().right() - 1;
                if (gap > splitter->handleWidth())
                    return QStringLiteral("%1: %2 pixels between the thumbnails (%3 wide) and the slides").arg(when).arg(gap).arg(list->width());
            } else if (body->x() > splitter->handleWidth()) {
                return QStringLiteral("%1: the slides start %2 pixels in, the thumbnails hidden").arg(when).arg(body->x());
            }
            const QRect r = view->slideRect(0);
            const int w = view->viewport()->width();
            if (std::abs(r.left() - 16) > 1 || std::abs(r.right() + 1 - (w - 16)) > 1)
                return QStringLiteral("%1: slide %2..%3 in a view %4 wide, zoom %5").arg(when).arg(r.left()).arg(r.right()).arg(w).arg(view->zoom());
            return QString();
        };
        QString wrong = check(QStringLiteral("opened"));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        doc->setSidebarShown(false);
        wrong = check(QStringLiteral("thumbnails hidden"));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        doc->setSidebarShown(true);
        wrong = check(QStringLiteral("thumbnails shown again"));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        app.resize(1000, 700);
        wrong = check(QStringLiteral("the window made smaller"));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        app.closeAllFiles();
    }

    // Symbol fonts' characters as what they stand for: Symbol's "a" an alpha
    // and its private-use U+F061 too (in the font a run's <a:sym> names),
    // drawn as the Greek is, and said so to read_pptx; Symbol's bullet a
    // bullet. Wingdings drawn by its font where it is here, else as what it
    // stands for - and said so either way. The Symbol macOS ships is a
    // Unicode font: "abg" in it was drawn "abg", U+F061 as a box (2026-11-09).
    void symbolFontsReadAsWhatTheyStandFor()
    {
        const auto box = [](const QByteArray& paras) {
            return shape(2, xfrm(40, 40, 880, 460) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", text(paras, "<a:bodyPr wrap=\"square\"/>"));
        };
        const QByteArray none = "<a:pPr><a:buNone/></a:pPr>";
        const auto bullet = [](const QByteArray& font, const QString& ch) {
            return "<a:pPr marL=\"342900\" indent=\"-342900\"><a:buFont typeface=\"" + font + "\"/><a:buChar char=\"" + ch.toHtmlEscaped().toUtf8()
                   + "\"/></a:pPr>";
        };
        const QByteArray symbol = "<a:rPr lang=\"en-US\"><a:latin typeface=\"Symbol\"/><a:sym typeface=\"Symbol\"/></a:rPr>";
        const QByteArray calibriSym = "<a:rPr lang=\"en-US\"><a:latin typeface=\"Helvetica\"/><a:sym typeface=\"Symbol\"/></a:rPr>";
        const QByteArray symOnly = "<a:rPr lang=\"en-US\"><a:sym typeface=\"Symbol\"/></a:rPr>";
        const QByteArray wingdings = "<a:rPr lang=\"en-US\"><a:latin typeface=\"Wingdings\"/><a:sym typeface=\"Wingdings\"/></a:rPr>";
        Deck d;
        Deck::SlideSpec one;
        one.tree = box(para(run(QStringLiteral("abgW"), symbol), none)                                                     // αβγΩ
                       + para(run(QStringLiteral("x"), "<a:rPr lang=\"en-US\"/>") + run(QStringLiteral(""), calibriSym), none)   // xα≥√
                       + para(run(QStringLiteral("ÒÓ"), symOnly), none)                                          // <a:sym> is for U+F0xx: ÒÓ
                       + para(run(QStringLiteral("dot")), bullet("Symbol", QStringLiteral("·")))                      // • dot
                       + para(run(QStringLiteral("pua")), bullet("Symbol", QStringLiteral("")))                      // • pua
                       + para(run(QStringLiteral("J"), wingdings), none)                                             // ☺✓
                       + para(run(QStringLiteral("arrow")), bullet("Wingdings", QStringLiteral("Ø"))));               // ➢ arrow
        Deck::SlideSpec greek;   // the same, in Unicode
        greek.tree = box(para(run(QStringLiteral("αβγΩ"), "<a:rPr lang=\"en-US\"/>"), none));
        Deck::SlideSpec inSymbol;
        inSymbol.tree = box(para(run(QStringLiteral("abgW"), symbol), none));
        Deck::SlideSpec inPua;
        inPua.tree = box(para(run(QStringLiteral(""), symOnly), none));
        d.slides << one << greek << inSymbol << inPua;
        QString why;
        const auto deck = readDeck(d, &why);
        QVERIFY2(deck != nullptr, qPrintable(why));
        QCOMPARE(deck->slide(0).paragraphs, (QStringList{QStringLiteral("αβγΩ"), QStringLiteral("xα≥√"), QStringLiteral("ÒÓ"), QStringLiteral("• dot"),
                                                         QStringLiteral("• pua"), QStringLiteral("☺✓"), QStringLiteral("➢ arrow")}));
        const QString notes = deck->slide(0).notShown.join(QStringLiteral(" | "));
        QVERIFY2(!notes.contains(QStringLiteral("Symbol")) && !notes.contains(QStringLiteral("Wingdings")), qPrintable(notes));
        // Drawn as the Greek is: Symbol's letters and its private-use
        // characters alike.
        const QImage want = deck->render(1, 1.0);
        QVERIFY(!want.isNull());
        QCOMPARE(deck->render(2, 1.0), want);
        QCOMPARE(deck->render(3, 1.0), want);
        QCOMPARE(deck->find(QStringLiteral("β")).size(), 4);
    }
};

QTEST_MAIN(TestPptxDoc)
#include "test_pptx_doc.moc"
