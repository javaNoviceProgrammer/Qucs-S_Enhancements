/*
 * The PDF viewer (pdfdoc.h): a PDF document opened in a tab of its own -
 * one tab for one file - its pages laid out to the width, drawn, gone
 * through by page, zoomed as the View menu and the toolbar do, fitted;
 * its text searched, the matches gone through, selected (a word, a page)
 * and copied; its links followed; read again when the file changes; its
 * pages at the side; saved under another name; a file that is not a PDF
 * refused; and every command of the Edit, Positioning, Insert, Simulation
 * and View menus, and the document's commands of the File menu, used with
 * it in front - none of them may take it for a schematic.
 */
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QPdfSearchModel>
#include <QPdfWriter>
#include <QPushButton>
#include <QTimer>
#include <QPrinter>
#include <QScrollBar>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTreeView>

#include "multiphysicsview.h"
#include "gpurendering.h"
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "pdfdoc.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"

using qucs_s::pdf::PageView;

namespace {

// A PDF of \a pages pages: "Page n" at the top of each, "needle" twice on
// the third, a link on the first. Written by Qt, as a report would be.
bool writePdf(const QString& path, int pages)
{
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setTitle(QStringLiteral("The probe"));
    QTextDocument doc;
    QFont font(QStringLiteral("Helvetica"));
    font.setPointSize(28);
    doc.setDefaultFont(font);
    QString html = QStringLiteral("<p>Page 1. Hello world, see <a href='https://example.org/datasheet'>the datasheet</a>.</p>");
    for (int i = 2; i <= pages; ++i) {
        html += QStringLiteral("<p style='page-break-before:always'>Page %1.</p>").arg(i);
        if (i == 3) html += QStringLiteral("<p>Here is the needle, and another needle.</p>");
    }
    doc.setHtml(html);
    doc.print(&writer);
    return QFileInfo(path).size() > 0;
}

// A PDF of three pages written by hand, with what Qt's writer leaves out:
// an outline ("1 Introduction" on page 1, "2.2.1 Modulation Efficiency"
// lower on page 3) and page labels (i, then S-1, S-2). "Figure 2.15:" on
// pages 2 and 3.
bool writeOutlinedPdf(const QString& path)
{
    const QStringList texts{QStringLiteral("Front matter"), QStringLiteral("Introduction. Figure 2.15: a sketch"),
                            QStringLiteral("Modulation. Figure 2.15: the bands")};
    QList<QByteArray> objects;
    objects << "<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R /PageLabels << /Nums [0 << /S /r >> 1 << /P (S-) /S /D >>] >> >>";
    objects << "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>";
    for (int i = 0; i < 3; ++i)
        objects << QStringLiteral("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 6 0 R >> >> /Contents %1 0 R >>")
                       .arg(7 + i)
                       .toLatin1();
    objects << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    for (int i = 0; i < 3; ++i) {
        const QByteArray stream = QStringLiteral("BT /F1 24 Tf 72 700 Td (%1) Tj ET BT /F1 24 Tf 72 380 Td (Section on page %2) Tj ET")
                                      .arg(texts.at(i))
                                      .arg(i + 1)
                                      .toLatin1();
        objects << "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream";
    }
    objects << "<< /Type /Outlines /First 11 0 R /Last 12 0 R /Count 2 >>";
    objects << "<< /Title (1 Introduction) /Parent 10 0 R /Next 12 0 R /Dest [3 0 R /XYZ 0 792 0] >>";
    objects << "<< /Title (2.2.1 Modulation Efficiency) /Parent 10 0 R /Prev 11 0 R /Dest [5 0 R /XYZ 0 420 0] >>";
    QByteArray out = "%PDF-1.4\n";
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets << int(out.size());
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const int xref = int(out.size());
    out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int o : offsets) out += QByteArray::number(o).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

// The dark pixels of a picture.
int darkPixels(const QImage& image)
{
    int dark = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).lightness() < 128) ++dark;
    return dark;
}

// What a page view shows, painted and its tiles all drawn: two grabs
// alike. (An OpenGL viewport's grab is its last picture, painted when the
// window was; a plain one's is painted for the grab.)
QImage shown(PageView* view)
{
    QImage last;
    for (int k = 0; k < 100; ++k) {
        QTest::qWait(40);
        const QImage now = view->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
        if (!last.isNull() && now == last) return now;
        last = now;
    }
    return last;
}

// The time a viewport's paint takes, its painting sent on and timed.
class PaintTimer : public QObject
{
public:
    explicit PaintTimer(QWidget* viewport) : QObject(viewport) { viewport->installEventFilter(this); }
    qint64 worst = 0;   // µs
    int paints = 0;
    QList<qint64> times;   // µs, each paint's
    qint64 median() const
    {
        QList<qint64> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        return sorted.isEmpty() ? 0 : sorted.at(sorted.size() / 2);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::Paint || a_inside) return false;
        a_inside = true;
        QElapsedTimer t;
        t.start();
        QCoreApplication::sendEvent(watched, event);
        times << t.nsecsElapsed() / 1000;
        worst = std::max(worst, times.last());
        ++paints;
        a_inside = false;
        return true;
    }

private:
    bool a_inside = false;
};

// Whatever dialog comes up is closed: the commands are used, not answered.
class DialogCloser : public QObject
{
public:
    explicit DialogCloser(QObject* parent) : QObject(parent)
    {
        startTimer(40);
    }
    int closed = 0;

protected:
    void timerEvent(QTimerEvent*) override
    {
        if (QWidget* w = QApplication::activeModalWidget()) {
            if (auto* d = qobject_cast<QDialog*>(w)) d->reject();
            else w->close();
            ++closed;
        }
    }
};

// Links to the web end here, not in a browser.
class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls << url; }
};

} // namespace

class TestPdfViewer : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QString file;
    UrlCatcher catcher;

    PdfDoc* pdf() const { return qobject_cast<PdfDoc*>(app->DocumentTab->currentWidget()); }

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
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &catcher, "open");
        file = dir.filePath("ws/probe.pdf");
        QVERIFY(writePdf(file, 3));
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 900);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
    }

    void cleanupTestCase()
    {
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    // A tab of its own - once - known as a document by the window and by
    // Claude's tools.
    void aPdfOpensInATab()
    {
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr);
        QVERIFY(QucsApp::isPdfDocument(doc));
        QVERIFY(!QucsApp::isTextDocument(doc));
        QVERIFY(QucsApp::schematicIn(doc) == nullptr);
        QCOMPARE(app->getDoc(), static_cast<QucsDoc*>(doc));
        QCOMPARE(QucsApp::documentWidget(app->getDoc()), static_cast<QWidget*>(doc));
        QCOMPARE(doc->pageCount(), 3);
        QCOMPARE(doc->title(), QStringLiteral("The probe"));
        QVERIFY(doc->problem().isEmpty());
        QCOMPARE(app->DocumentTab->tabText(app->DocumentTab->currentIndex()).remove('&'), QStringLiteral("probe.pdf"));
        QVERIFY(doc->getDataDisplay().isEmpty());   // not simulated
        const int tabs = app->DocumentTab->count();
        QVERIFY(app->gotoPage(file));
        QCOMPARE(app->DocumentTab->count(), tabs);
        QCOMPARE(app->findDoc(file), static_cast<QucsDoc*>(doc));
        QVERIFY(app->allDocuments().contains(doc));

        auto* control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        QVERIFY(QucsControl::textOf(control->callNow("get_state", {})).remove(' ').contains("\"kind\":\"pdf\""));
        QVERIFY(control->callNow("get_schematic", {}).value("isError").toBool());   // not a schematic
    }

    // From the Content panel and the File Browser (and a drop): the same
    // tab, not the system's viewer.
    void itOpensFromThePanels()
    {
        const int tabs = app->DocumentTab->count();
        app->openFileFromProjectView(QFileInfo(file), QString());
        QCOMPARE(app->DocumentTab->count(), tabs);
        QVERIFY(QucsApp::isPdfDocument(app->DocumentTab->currentWidget()));
    }

    // Laid out to the width, drawn, gone through by page.
    void pagesAreShownAndGoneThrough()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        QCOMPARE(view->fit(), PageView::Fit::Width);
        QTRY_VERIFY(view->pageRect(0).width() > 100);
        QVERIFY(std::abs(view->pageRect(0).width() - (view->viewport()->width() - 32)) <= 2);
        QCOMPARE(view->currentPage(), 0);
        QCOMPARE(doc->pageField()->text(), QStringLiteral("1"));
        // Drawn: the paper white, the text dark.
        const QImage shot = shown(view);
        const QRect page = view->pageRect(0).intersected(view->viewport()->rect());
        int dark = 0, white = 0;
        for (int y = page.top(); y < page.bottom(); y += 3)
            for (int x = page.left(); x < page.right(); x += 3) {
                const QColor c = shot.pixelColor(QPoint(x, y) * shot.devicePixelRatio());
                if (c.lightness() < 100) ++dark;
                else if (c.lightness() > 245) ++white;
            }
        QVERIFY2(dark > 20 && white > dark * 5, qPrintable(QStringLiteral("%1 %2").arg(dark).arg(white)));
        QVERIFY(view->tilesRendered() > 0);

        doc->nextPage();
        QCOMPARE(view->currentPage(), 1);
        QCOMPARE(doc->pageField()->text(), QStringLiteral("2"));
        doc->lastPage();
        QCOMPARE(view->currentPage(), 2);
        doc->previousPage();
        QCOMPARE(view->currentPage(), 1);
        doc->firstPage();
        QCOMPARE(view->currentPage(), 0);
        doc->pageField()->setText(QStringLiteral("3"));
        QMetaObject::invokeMethod(doc->pageField(), "returnPressed");
        QCOMPARE(view->currentPage(), 2);
        doc->pageField()->setText(QStringLiteral("99"));   // no such page: stays
        QMetaObject::invokeMethod(doc->pageField(), "returnPressed");
        QCOMPARE(view->currentPage(), 2);
        // The keys: Home, End.
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Home);
        QCOMPARE(view->currentPage(), 0);
        QTest::keyClick(view, Qt::Key_End);
        QCOMPARE(view->currentPage(), 2);
        doc->firstPage();
    }

    // Zoomed by the View menu and the toolbar; fitted; the point in sight kept.
    void itIsZoomed()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        app->magOne->trigger();   // View > View 1:1
        QCOMPARE(view->zoom(), 1.0);
        QCOMPARE(view->fit(), PageView::Fit::None);
        const QSizeF points = doc->document()->pagePointSize(0);
        QVERIFY(std::abs(view->pageRect(0).width() - points.width() * view->pixelsPerPoint()) <= 2);
        app->magPlus->trigger();   // View > Zoom In
        QCOMPARE(view->zoom(), 1.25);
        QVERIFY(!app->magPlus->isChecked());   // not a mode, for a PDF
        app->magMinus->trigger();
        QCOMPARE(view->zoom(), 1.0);
        app->magAll->trigger();   // View All: the page whole
        QCOMPARE(view->fit(), PageView::Fit::Page);
        QVERIFY(view->pageRect(0).height() <= view->viewport()->height() - 30);
        app->magSel->trigger();   // Zoom to Selection: the width
        QCOMPARE(view->fit(), PageView::Fit::Width);
        view->setZoom(100);   // clamped
        QCOMPARE(view->zoom(), PageView::MaxZoom);
        // At 1600% only the tiles in sight are drawn.
        const int before = view->tilesRendered();
        view->viewport()->repaint();
        QVERIFY(view->tilesRendered() - before < 40);
        view->setZoom(0.001);
        QCOMPARE(view->zoom(), PageView::MinZoom);
        view->setFit(PageView::Fit::Width);
    }

    // Found, gone through, wrapped round; the menu's Find brings the bar.
    void itIsSearched()
    {
        PdfDoc* doc = pdf();
        doc->find(QStringLiteral("needle"));
        QTRY_COMPARE(doc->searchResultCount(), 2);
        doc->findNext();
        QCOMPARE(doc->currentSearchResult(), 0);
        QTRY_COMPARE(doc->view()->currentPage(), 2);
        doc->findNext();
        QCOMPARE(doc->currentSearchResult(), 1);
        doc->findNext();
        QCOMPARE(doc->currentSearchResult(), 0);
        doc->findNext(true);
        QCOMPARE(doc->currentSearchResult(), 1);
        doc->find(QStringLiteral("nowhere to be found"));
        QTest::qWait(300);
        QCOMPARE(doc->searchResultCount(), 0);
        doc->hideSearch();
        QVERIFY(!doc->searchField()->isVisible());
        app->editFind->trigger();   // Edit > Find
        QVERIFY(doc->searchField()->isVisible());
        doc->hideSearch();
        doc->firstPage();
    }

    // A page's text, a word, a stretch: selected, copied.
    void textIsSelectedAndCopied()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        QApplication::clipboard()->clear();
        app->selectAll->trigger();   // Edit > Select All: the page's text
        QVERIFY(view->hasSelection());
        QVERIFY2(view->selectedText().contains("Hello world"), qPrintable(view->selectedText()));
        app->editCopy->trigger();   // Edit > Copy
        QVERIFY(QApplication::clipboard()->text().contains("Hello world"));

        // A word: where the search finds it, double clicked.
        doc->find(QStringLiteral("Hello"));
        QTRY_COMPARE(doc->searchResultCount(), 1);
        doc->findNext();
        QTRY_VERIFY(doc->currentSearchResult() == 0);
        auto* model = doc->findChild<QPdfSearchModel*>();
        QVERIFY(model != nullptr);
        const QList<QRectF> rects = model->resultAtIndex(0).rectangles();
        QVERIFY(!rects.isEmpty());
        QRectF word = rects.first();
        for (const QRectF& r : rects) word = word.united(r);
        doc->hideSearch();
        const QPointF centre = word.center();
        const QPoint at = (view->pageRect(0).topLeft() + centre * view->pixelsPerPoint()).toPoint();
        QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(view->selectedText().trimmed(), QStringLiteral("Hello"));
        // A stretch, dragged across: from the word's start past its end.
        view->clearSelection();
        QVERIFY(view->selectText(0, QPointF(word.left() + 0.5, centre.y()), QPointF(word.right() + 40, centre.y())));
        QVERIFY2(view->selectedText().startsWith("Hello"), qPrintable(view->selectedText()));
        // Begun beside the text (the page's corner): from the nearest character.
        QVERIFY(view->selectText(0, QPointF(1, 1), QPointF(word.right() + 2, centre.y())));
        QVERIFY2(view->selectedText().contains("Hello"), qPrintable(view->selectedText()));
        view->clearSelection();
        QVERIFY(!view->hasSelection());
    }

    // A link to the web: opened (here, caught); the hand over it.
    void linksAreFollowed()
    {
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        doc->firstPage();
        QPdfLinkModel links;
        links.setDocument(doc->document());
        links.setPage(0);
        QVERIFY(links.rowCount(QModelIndex()) >= 1);   // a rectangle for each line it takes
        const QRectF rect = links.data(links.index(0), int(QPdfLinkModel::Role::Rectangle)).toRectF();
        const QPoint at = (view->pageRect(0).topLeft() + rect.center() * view->pixelsPerPoint()).toPoint();
        QMouseEvent hover(QEvent::MouseMove, QPointF(at), view->viewport()->mapToGlobal(QPointF(at)), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &hover);
        QCOMPARE(view->viewport()->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTRY_COMPARE(catcher.urls.size(), 1);
        QCOMPARE(catcher.urls.first(), QUrl("https://example.org/datasheet"));
    }

    // A link of a PDF to a program is not handed to the system (it ran -
    // bug hunt 2026-09-26, B2): asked about, shown in the file manager at
    // most.
    void aLinkToAProgramIsNotRun()
    {
        const QString evil = dir.filePath("ws/evil.command");
        {
            QFile f(evil);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\necho ran > ran.txt\n");
        }
        QFile::setPermissions(evil, QFile::permissions(evil) | QFileDevice::ExeOwner);
        const QString bait = dir.filePath("ws/bait.pdf");
        {
            QPdfWriter writer(bait);
            QTextDocument doc;
            QFont font(QStringLiteral("Helvetica"));
            font.setPointSize(28);
            doc.setDefaultFont(font);
            doc.setHtml(QStringLiteral("<p><a href='%1'>Open the schematic</a></p>").arg(QUrl::fromLocalFile(evil).toString()));
            doc.print(&writer);
        }
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &catcher, "open");
        catcher.urls.clear();
        QVERIFY(app->gotoPage(bait));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr && doc->getDocName().endsWith("bait.pdf"));
        PageView* view = doc->view();
        QPdfLinkModel links;
        links.setDocument(doc->document());
        links.setPage(0);
        QVERIFY(links.rowCount(QModelIndex()) >= 1);
        QCOMPARE(links.data(links.index(0), int(QPdfLinkModel::Role::Url)).toUrl(), QUrl::fromLocalFile(evil));
        const QRectF rect = links.data(links.index(0), int(QPdfLinkModel::Role::Rectangle)).toRectF();
        const QPoint at = (view->pageRect(0).topLeft() + rect.center() * view->pixelsPerPoint()).toPoint();
        QString said;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr || !box->isVisible()) return;
            said = box->objectName() + ": " + box->text();
            box->button(QMessageBox::Cancel) != nullptr ? box->button(QMessageBox::Cancel)->click() : box->reject();
        });
        timer.start(20);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTRY_VERIFY(!said.isEmpty());
        timer.stop();
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        QVERIFY2(said.startsWith("linkQuestion") && said.contains("evil.command"), qPrintable(said));
        QVERIFY(catcher.urls.isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath("ws/ran.txt")));
        QVERIFY(QMetaObject::invokeMethod(app, "slotFileClose", Q_ARG(int, app->DocumentTab->currentIndex())));
        QVERIFY(app->gotoPage(file));
    }

    // Written again (a report made anew): read again, the page kept.
    void itIsReadAgainWhenTheFileChanges()
    {
        PdfDoc* doc = pdf();
        doc->nextPage();
        QCOMPARE(doc->view()->currentPage(), 1);
        QSignalSpy reloaded(doc, &PdfDoc::reloaded);
        QTest::qWait(1100);   // a newer time on the file
        QVERIFY(writePdf(file, 5));
        QTRY_VERIFY_WITH_TIMEOUT(reloaded.count() >= 1, 10000);
        QCOMPARE(doc->pageCount(), 5);
        QCOMPARE(doc->view()->currentPage(), 1);
    }

    // Deleted, then written again a moment later ("rm report.pdf && make
    // report"): still followed - read again then, and at every later
    // version (bug hunt 2026-09-26, E3: nothing watched the path any more).
    // Meanwhile it is shown as it was, and the status bar says why.
    void itFollowsItsFileThroughADelete()
    {
        PdfDoc* doc = pdf();
        QSignalSpy reloaded(doc, &PdfDoc::reloaded);
        QVERIFY(QFile::remove(file));
        QTRY_VERIFY_WITH_TIMEOUT(app->statusBar()->currentMessage().contains("was deleted"), 5000);
        QCOMPARE(doc->pageCount(), 5);   // as it was
        QTest::qWait(1500);
        QVERIFY(writePdf(file, 3));
        QTRY_VERIFY_WITH_TIMEOUT(reloaded.count() >= 1 && doc->pageCount() == 3, 10000);
        QTest::qWait(1100);
        const int before = reloaded.count();
        QVERIFY(writePdf(file, 4));
        QTRY_VERIFY_WITH_TIMEOUT(reloaded.count() > before && doc->pageCount() == 4, 10000);
        QTest::qWait(1100);   // (as the next tests have it: 5 pages)
        QVERIFY(writePdf(file, 5));
        QTRY_VERIFY_WITH_TIMEOUT(doc->pageCount() == 5, 10000);
    }

    // The pages at the side, drawn small; one clicked is gone to. No
    // outline in this document: its tab hidden.
    void itsPagesAreAtTheSide()
    {
        PdfDoc* doc = pdf();
        doc->setSidebarShown(true);
        QVERIFY(doc->sidebar()->isVisible());
        QCOMPARE(doc->thumbnails()->count(), 5);
        QTRY_VERIFY([&] {
            // The first page's text, some grey among the white.
            const QImage img = doc->thumbnails()->item(0)->icon().pixmap(QSize(104, 150)).toImage();
            int grey = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (img.pixelColor(x, y).lightness() < 200) ++grey;
            return grey > 10;
        }());
        doc->thumbnails()->setCurrentRow(3);
        QCOMPARE(doc->view()->currentPage(), 3);
        doc->setSidebarShown(false);
        QVERIFY(!doc->sidebar()->isVisible());
    }

    // Saved as: a copy, the tab named after it.
    void itIsSavedUnderAnotherName()
    {
        PdfDoc* doc = pdf();
        const QString copy = dir.filePath("ws/copy.pdf");
        QVERIFY(app->saveDocumentAs(doc, copy));
        QVERIFY(QFileInfo::exists(copy));
        QFile a(file), b(copy);
        QVERIFY(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly));
        QCOMPARE(a.readAll(), b.readAll());
        QCOMPARE(doc->getDocName(), QFileInfo(copy).absoluteFilePath());
        QVERIFY(app->saveFile(doc));   // nothing to write: fine
        file = copy;
    }

    // Saved as itself under another spelling - a path through a link to
    // its folder, another case where the file system has none: nothing to
    // write, the file whole. It was deleted, and the copy then failed (bug
    // hunt 2026-09-26, A1).
    void itIsSavedAsItselfUnderAnotherSpelling()
    {
        PdfDoc* doc = pdf();
        QFile original(file);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray before = original.readAll();
        original.close();
        const auto whole = [&](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) && f.readAll() == before;
        };
#ifndef Q_OS_WIN
        const QString link = dir.filePath("wslink");
        QVERIFY(QFile::link(QFileInfo(file).absolutePath(), link));
        QVERIFY(app->saveDocumentAs(doc, link + "/" + QFileInfo(file).fileName()));
        QVERIFY(whole(file));
#endif
        const QString upper = QFileInfo(file).absolutePath() + "/" + QFileInfo(file).fileName().toUpper();
        if (QFileInfo::exists(upper)) {   // (a file system without case: macOS, Windows)
            QVERIFY(app->saveDocumentAs(doc, upper));
            QVERIFY(whole(upper));
        }
        QVERIFY(app->saveDocumentAs(doc, file));
        QVERIFY(whole(file));
        QCOMPARE(doc->getDocName(), QFileInfo(file).absoluteFilePath());
    }

    // Not a PDF: refused, said why, no tab left.
    void aFileThatIsNotAPdfIsRefused()
    {
        const QString bad = dir.filePath("ws/bad.pdf");
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not a PDF at all\n");
        f.close();
        const int tabs = app->DocumentTab->count();
        misc::ErrorCapture capture;
        QVERIFY(!app->gotoPage(bad));
        QCOMPARE(app->DocumentTab->count(), tabs);
        QVERIFY(!capture.errors().isEmpty());
    }

    // Every command that works on the document in front, with a PDF
    // there: none may take it for a schematic or a text document.
    void everyCommandLeavesItWhole()
    {
        QVERIFY(app->gotoPage(file));
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
            if (top->menu() != nullptr && menus.contains(QString(top->text()).remove('&')))
                walk(top->menu());
        // (Not File > Print: the system's print panel is no dialog of
        // Qt's to close; printing is tested on its own.)
        actions << app->fileSave << app->fileSaveAs << app->fileSaveAll << app->fileSettings
                << app->exportAsImage << app->editCopyImage << app->symEdit;
        QVERIFY(actions.size() > 60);
        int used = 0;
        for (QAction* a : std::as_const(actions)) {
            const QString text = QString(a->text()).remove('&');
            if (std::any_of(skip.cbegin(), skip.cend(), [&](const QString& s) { return text.contains(s, Qt::CaseInsensitive); }))
                continue;
            QVERIFY(app->gotoPage(file));
            QVERIFY(QucsApp::isPdfDocument(app->DocumentTab->currentWidget()));
            if (!a->isEnabled()) continue;
            a->trigger();
            if (a->isCheckable() && a->isChecked() && a != app->select) a->trigger();   // back off a mode
            ++used;
            QTest::qWait(1);
        }
        QVERIFY(used > 40);
        // Still there and whole.
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr);
        QVERIFY(doc->pageCount() == 5);
        doc->view()->viewport()->repaint();
        delete closer;
    }

    // Printed: every page on a sheet (here to a PDF file).
    void itIsPrinted()
    {
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        const QString out = dir.filePath("printed.pdf");
        {
            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(out);
            QPainter painter(&printer);
            doc->print(&printer, &painter, true, true);
        }
        QPdfDocument printed;
        QCOMPARE(printed.load(out), QPdfDocument::Error::None);
        QCOMPARE(printed.pageCount(), doc->pageCount());
        // Each sheet the page's picture: some text on white.
        const QImage sheet = printed.render(0, QSize(300, 424));
        int dark = 0;
        for (int y = 0; y < sheet.height(); ++y)
            for (int x = 0; x < sheet.width(); ++x)
                if (sheet.pixelColor(x, y).alpha() > 0 && sheet.pixelColor(x, y).lightness() < 128) ++dark;
        QVERIFY2(dark > 20, qPrintable(QString::number(dark)));
    }

    // Claude's tools show a page: by number, by a hit of a search (every
    // hit's page said), zoomed, a region of it as large as the tab takes it.
    void claudesToolsShowAPage()
    {
        auto* control = app->findChild<QucsControl*>();
        auto call = [&](const char* tool, const QJsonObject& args) { return control->callNow(QString::fromLatin1(tool), args); };
        auto json = [](const QJsonObject& r) { return QJsonDocument::fromJson(QucsControl::textOf(r).toUtf8()).object(); };
        QJsonObject r = call("show_pdf", {{"path", file}, {"page", 2}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr);
        QCOMPARE(json(r).value("page").toInt(), 2);
        QCOMPARE(json(r).value("pages").toInt(), doc->pageCount());   // (5: written again by a test before)
        QCOMPARE(doc->view()->currentPage(), 1);
        // A search: every hit's page, the first shown and marked.
        doc->firstPage();
        r = call("show_pdf", {{"find", "needle"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        const QJsonObject found = json(r).value("found").toObject();
        QCOMPARE(found.value("hits").toInt(), 2);
        QCOMPARE(found.value("pages").toArray(), QJsonArray({3}));
        QCOMPARE(json(r).value("page").toInt(), 3);
        QCOMPARE(doc->view()->currentPage(), 2);
        QVERIFY(doc->currentSearchResult() >= 0);
        QCOMPARE(doc->searchField()->text(), QStringLiteral("needle"));
        QVERIFY(doc->searchField()->isVisible());
        r = call("show_pdf", {{"find", "nowhere to be found"}});
        QCOMPARE(json(r).value("found").toObject().value("hits").toInt(), 0);
        QVERIFY(json(r).value("note").toString().contains("not in its text"));
        doc->hideSearch();
        // Zoomed: a percentage, a fit.
        r = call("show_pdf", {{"page", 1}, {"zoom", 150}});
        QCOMPARE(json(r).value("zoom").toString(), QStringLiteral("150 %"));
        QVERIFY(std::abs(doc->view()->zoom() - 1.5) < 1e-9);
        QCOMPARE(doc->view()->currentPage(), 0);
        r = call("show_pdf", {{"zoom", "fit width"}});
        QCOMPARE(doc->view()->fit(), PageView::Fit::Width);
        // Found and zoomed at once: the hit still in sight (where a search
        // of its own finds it).
        doc->firstPage();
        r = call("show_pdf", {{"find", "needle"}, {"zoom", 800}});
        QCOMPARE(doc->view()->currentPage(), 2);
        {
            QPdfSearchModel own;
            own.setDocument(doc->document());
            own.setSearchString(QStringLiteral("needle"));
            const QList<QPdfLink> links = own.resultsOnPage(2);
            QVERIFY(!links.isEmpty() && !links.first().rectangles().isEmpty());
            const QRectF hit = links.first().rectangles().first();
            const qreal ppp = doc->view()->pixelsPerPoint();
            const QRectF shown(doc->view()->pageRect(2).topLeft() + hit.topLeft() * ppp, hit.size() * ppp);
            QVERIFY2(QRectF(doc->view()->viewport()->rect()).contains(shown), qPrintable(QStringLiteral("%1, %2").arg(shown.x()).arg(shown.y())));
        }
        doc->hideSearch();
        r = call("show_pdf", {{"zoom", "fit width"}});
        // A region - the third page's top left quarter, as fractions -:
        // zoomed to fit it, all of it in sight, as large as the tab takes it.
        const QRect vp = doc->view()->viewport()->rect();
        auto inSight = [&](int page, const QRectF& region) {
            const qreal ppp = doc->view()->pixelsPerPoint();
            const QRect p = doc->view()->pageRect(page);
            const QRectF shown(p.topLeft() + region.topLeft() * ppp, region.size() * ppp);
            const bool fills = std::abs(shown.width() - (vp.width() - 32)) < 2 || std::abs(shown.height() - (vp.height() - 32)) < 2;
            return QRectF(vp).adjusted(-1, -1, 1, 1).contains(shown) && fills;
        };
        r = call("show_pdf", {{"page", 3}, {"region", QJsonArray{0, 0, 0.5, 0.5}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        const QSizeF size = doc->document()->pagePointSize(2);
        QCOMPARE(doc->view()->currentPage(), 2);
        QVERIFY(inSight(2, QRectF(0, 0, size.width() / 2, size.height() / 2)));
        const qreal fits = std::min((vp.width() - 32) / (size.width() / 2), (vp.height() - 32) / (size.height() / 2)) / (doc->view()->logicalDpiX() / 72.0);
        QVERIFY2(std::abs(doc->view()->zoom() - fits) < 1e-6, qPrintable(QString::number(doc->view()->zoom())));
        // In points too - a small one, its middle the view's; out of the
        // document, refused.
        r = call("show_pdf", {{"page", 1}, {"region", QJsonArray{72, 72, 172, 122}}});
        QVERIFY(inSight(0, QRectF(72, 72, 100, 50)));
        QPointF at;
        QCOMPARE(doc->view()->pageAt(vp.center(), &at), 0);
        QVERIFY2(std::abs(at.x() - 122) < 2 && std::abs(at.y() - 97) < 2, qPrintable(QStringLiteral("%1, %2").arg(at.x()).arg(at.y())));
        QVERIFY(call("show_pdf", {{"page", doc->pageCount() + 1}}).value("isError").toBool());
        QVERIFY(call("show_pdf", {{"zoom", "bigger"}}).value("isError").toBool());
        doc->view()->setFit(PageView::Fit::Width);
    }

    // An outline's destination, a page's label; opened at a page or a hit.
    void claudesToolsFindDestinations()
    {
        auto* control = app->findChild<QucsControl*>();
        auto call = [&](const char* tool, const QJsonObject& args) { return control->callNow(QString::fromLatin1(tool), args); };
        auto json = [](const QJsonObject& r) { return QJsonDocument::fromJson(QucsControl::textOf(r).toUtf8()).object(); };
        const QString outlined = dir.filePath("ws/outlined.pdf");
        QVERIFY(writeOutlinedPdf(outlined));
        QJsonObject r = call("open_document", {{"path", outlined}, {"page", "2.2.1 Modulation"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        QVERIFY2(QucsControl::textOf(r).contains("At page 3 of 3") && QucsControl::textOf(r).contains("2.2.1 Modulation Efficiency"),
                 qPrintable(QucsControl::textOf(r)));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr && doc->getDocName() == outlined);
        QCOMPARE(doc->view()->currentPage(), 2);
        r = call("show_pdf", {{"page", "introduction"}});
        QCOMPARE(json(r).value("page").toInt(), 1);
        QCOMPARE(json(r).value("destination").toString(), QStringLiteral("1 Introduction"));
        r = call("show_pdf", {{"page", "Modulation"}});
        QCOMPARE(json(r).value("page").toInt(), 3);
        // Its labels: i, S-1, S-2 - a label finds its page; a number is the
        // file's.
        r = call("show_pdf", {{"page", "S-2"}});
        QCOMPARE(json(r).value("page").toInt(), 3);
        QCOMPARE(json(r).value("label").toString(), QStringLiteral("S-2"));
        QVERIFY(!json(r).contains("destination"));
        r = call("show_pdf", {{"page", "2"}});
        QCOMPARE(json(r).value("page").toInt(), 2);
        QCOMPARE(json(r).value("label").toString(), QStringLiteral("S-1"));
        r = call("show_pdf", {{"page", "Conclusions"}});
        QVERIFY(r.value("isError").toBool());
        QVERIFY2(QucsControl::textOf(r).contains("1 Introduction; 2.2.1 Modulation Efficiency"), qPrintable(QucsControl::textOf(r)));
        // A figure named: its pages, the first shown - the PDF brought to
        // the front from behind another.
        QVERIFY(app->gotoPage(file));
        QVERIFY(pdf() != doc);
        r = call("show_pdf", {{"path", outlined}, {"find", "Figure 2.15:"}});
        QCOMPARE(pdf(), doc);
        QCOMPARE(json(r).value("found").toObject().value("pages").toArray(), QJsonArray({2, 3}));
        QCOMPARE(doc->view()->currentPage(), 1);
        doc->hideSearch();
        // Opened again at a hit: brought to the front there.
        QVERIFY(app->gotoPage(file));
        r = call("open_document", {{"path", outlined}, {"find", "the bands"}});
        QVERIFY2(QucsControl::textOf(r).contains("1 hits, on pages 3"), qPrintable(QucsControl::textOf(r)));
        QCOMPARE(pdf(), doc);
        QCOMPARE(doc->view()->currentPage(), 2);
        doc->hideSearch();
        QVERIFY(call("open_document", {{"path", outlined}, {"page", 7}}).value("isError").toBool());
        QVERIFY(app->gotoPage(outlined));
        app->slotFileClose(app->DocumentTab->currentIndex());
        QVERIFY(app->findDoc(outlined) == nullptr);
    }

    // A page drawn - all of it, or a region -, the tab not moved; read_pdf's
    // pages as pictures too.
    void claudesToolsDrawAPage()
    {
        auto* control = app->findChild<QucsControl*>();
        auto call = [&](const char* tool, const QJsonObject& args) { return control->callNow(QString::fromLatin1(tool), args); };
        auto image = [](const QJsonObject& r) {
            for (const QJsonValue& v : r.value("content").toArray())
                if (v.toObject().value("type").toString() == QLatin1String("image"))
                    return QImage::fromData(QByteArray::fromBase64(v.toObject().value("data").toString().toLatin1()));
            return QImage();
        };
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        doc->firstPage();
        const int scrolled = doc->view()->verticalScrollBar()->value();
        // The third page's top fifth: "Page 3." in it, drawn sharp.
        QJsonObject r = call("screenshot", {{"page", 3}, {"region", QJsonArray{0, 0, 1, 0.2}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        QImage part = image(r);
        const QSizeF size = doc->document()->pagePointSize(2);
        QVERIFY(!part.isNull());
        QVERIFY2(std::abs(double(part.width()) / part.height() - size.width() / (0.2 * size.height())) < 0.02, qPrintable(QStringLiteral("%1 x %2").arg(part.width()).arg(part.height())));
        QVERIFY(part.width() >= 1000);
        QVERIFY2(darkPixels(part) > 100, qPrintable(QString::number(darkPixels(part))));
        QVERIFY(QucsControl::textOf(r).contains("its tab left as it is"));
        QCOMPARE(doc->view()->currentPage(), 0);
        QCOMPARE(doc->view()->verticalScrollBar()->value(), scrolled);
        // Its middle, blank (its number is at its foot).
        r = call("screenshot", {{"page", 3}, {"region", QJsonArray{0, 0.45, 1, 0.55}}});
        QCOMPARE(darkPixels(image(r)), 0);
        // A whole page.
        r = call("screenshot", {{"page", 2}});
        const QImage whole = image(r);
        QVERIFY(std::abs(double(whole.width()) / whole.height() - size.width() / size.height()) < 0.01);
        QVERIFY(call("screenshot", {{"page", doc->pageCount() + 1}}).value("isError").toBool());
        // read_pdf: the text and the pictures.
        r = call("read_pdf", {{"path", file}, {"pages", "2-3"}, {"image", true}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        int images = 0;
        for (const QJsonValue& v : r.value("content").toArray()) images += v.toObject().value("type").toString() == QLatin1String("image");
        QCOMPARE(images, 2);
        QVERIFY(QucsControl::textOf(r).contains("needle"));
        r = call("read_pdf", {{"path", file}, {"pages", 3}, {"image", true}, {"region", QJsonArray{0, 0, 1, 0.2}}});
        QVERIFY(darkPixels(image(r)) > 100);
    }

    // The tab's own bar through get_ui and set_ui: a page typed into its
    // page field is shown.
    void claudesToolsUseItsBar()
    {
        auto* control = app->findChild<QucsControl*>();
        auto call = [&](const char* tool, const QJsonObject& args) { return control->callNow(QString::fromLatin1(tool), args); };
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        doc->firstPage();
        QJsonObject r = call("get_ui", {{"area", "document"}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        const QJsonObject ui = QJsonDocument::fromJson(QucsControl::textOf(r).toUtf8()).object();
        QString pageId;
        for (const QJsonValue& v : ui.value("controls").toArray())
            if (v.toObject().value("value").toString() == QLatin1String("1") && v.toObject().value("kind").toString() == QLatin1String("field"))
                pageId = v.toObject().value("id").toString();
        QVERIFY2(!pageId.isEmpty(), qPrintable(QucsControl::textOf(r)));
        r = call("set_ui", {{"area", "document"}, {"set", QJsonArray{QJsonObject{{"control", pageId}, {"value", "3"}}}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        QTRY_COMPARE(doc->view()->currentPage(), 2);
        QVERIFY(call("get_ui", {}).value("isError").toBool() == false);
        QVERIFY(QucsControl::textOf(call("get_ui", {})).contains("\"document\""));
        doc->firstPage();
    }

    // Drawn by the GPU as by the CPU (gpurendering.h: an OpenGL viewport,
    // the page's tiles textures), the setting switching the view open; a
    // scroll's paint timed each way.
    void theGpuDrawsAsTheCpu()
    {
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        QVERIFY(app->gotoPage(file));
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        QVERIFY(view->gpu());   // (chosen by default)
        QVERIFY(qucs_s::gpu::hasGlViewport(view));
        doc->firstPage();
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Pdf, false);
        QVERIFY(!view->gpu());
        QCOMPARE(QucsSettingsFile().value("Rendering/pdf").toString(), QString("cpu"));
        const QImage cpu = shown(view);
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Pdf, true);
        QVERIFY(view->gpu());
        const QImage gpu = shown(view);
        QCOMPARE(gpu.size(), cpu.size());
        int differ = 0, dark = 0;
        for (int y = 0; y < cpu.height(); ++y)
            for (int x = 0; x < cpu.width(); ++x) {
                const QColor a = cpu.pixelColor(x, y), b = gpu.pixelColor(x, y);
                dark += a.lightness() < 100;
                differ += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) > 24;
            }
        const QString shots = qEnvironmentVariable("QUCS_TEST_SHOTS");
        if (!shots.isEmpty()) {
            cpu.save(shots + "/pdf_cpu.png");
            gpu.save(shots + "/pdf_gpu.png");
        }
        qInfo("%d of %d pixels differ; %d dark (the text)", differ, cpu.width() * cpu.height(), dark);
        QVERIFY(dark > 500);
        QVERIFY2(differ < cpu.width() * cpu.height() / 200, qPrintable(QString::number(differ)));
        // A scroll's paint, each way.
        qint64 worst[2] = {0, 0}, median[2] = {0, 0};
        for (bool onGpu : {false, true}) {
            view->setGpu(onGpu);
            (void)shown(view);
            auto* timer = new PaintTimer(view->viewport());
            for (int k = 0; k < 20; ++k) {
                view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() + 37);
                QTest::qWait(20);
            }
            QTRY_VERIFY(timer->paints > 5);
            worst[onGpu] = timer->worst;
            median[onGpu] = timer->median();
            delete timer;
            doc->firstPage();
        }
        qInfo("a scroll's paint: the CPU %lld µs (%lld at most), the GPU %lld µs (%lld)", median[0], worst[0], median[1], worst[1]);
        QVERIFY(view->gpu());
    }

    // A page full of text: its first selection at once. (Each of its
    // characters' boxes was asked of pdfium one by one, some 2 ms each - a
    // paper's page of 6000 characters 11 s at its first selection, the
    // program not answering; now pdfium's own hit test, a few calls.) What
    // is selected: from the character under the press to the one under the
    // mouse, past its middle.
    void aFullPageIsSelectedAtOnce()
    {
        const QString dense = dir.filePath("ws/dense.pdf");
        {
            QPdfWriter writer(dense);
            writer.setPageSize(QPageSize(QPageSize::A4));
            QTextDocument text;
            QFont font(QStringLiteral("Helvetica"));
            font.setPointSize(7);
            text.setDefaultFont(font);
            QString html;
            for (int i = 0; i < 70; ++i)
                html += QStringLiteral("<p>Line %1: the quick brown fox jumps over the lazy dog, again and again, until the page is full.</p>")
                            .arg(i, 2, 10, QLatin1Char('0'));
            text.setHtml(html);
            text.print(&writer);
        }
        QVERIFY(app->gotoPage(dense));
        PdfDoc* doc = pdf();
        QVERIFY(doc != nullptr);
        PageView* view = doc->view();
        QPdfDocument pdf;
        QCOMPARE(pdf.load(dense), QPdfDocument::Error::None);
        const QPdfSelection all = pdf.getAllText(0);
        QVERIFY2(all.endIndex() > 3000, qPrintable(QString::number(all.endIndex())));
        const QRectF page = all.boundingRectangle();
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer)) || !defined(NDEBUG)
        const qint64 slower = 10;   // (a build with sanitizers, or for debugging)
#else
        const qint64 slower = 1;
#endif
        QElapsedTimer t;
        t.start();
        QVERIFY(view->selectText(0, page.topLeft() + QPointF(2, 2), page.bottomRight() - QPointF(2, 2)));
        const qint64 first = t.elapsed();
        t.restart();
        QVERIFY(view->selectText(0, page.topLeft() + QPointF(2, 2), page.center()));
        const qint64 next = t.elapsed();
        qInfo("a page of %d characters: its first selection %lld ms, the next %lld ms", all.endIndex(), first, next);
        QVERIFY2(first < 1000 * slower, qPrintable(QString::number(first)));
        QVERIFY2(next < 200 * slower, qPrintable(QString::number(next)));
        // From the middle of "quick" to past the middle of "fox"'s x.
        const QString line = QStringLiteral("Line 10: the quick brown fox");
        const int at = int(all.text().indexOf(line));
        QVERIFY(at >= 0);
        const QRectF q = pdf.getSelectionAtIndex(0, at + int(line.indexOf("quick")) + 2, 1).boundingRectangle();
        const QRectF x = pdf.getSelectionAtIndex(0, at + int(line.indexOf("fox")) + 2, 1).boundingRectangle();
        QVERIFY(!q.isEmpty() && !x.isEmpty());
        QVERIFY(view->selectText(0, QPointF(q.left() + 0.2, q.center().y()), QPointF(x.right() - 0.2, x.center().y())));
        QCOMPARE(view->selectedText(), QStringLiteral("ick brown fox"));
        // Past the middle of the 'i', short of the middle of the 'x': each
        // left out.
        QVERIFY(view->selectText(0, QPointF(q.center().x() + 0.3 * q.width(), q.center().y()),
                                 QPointF(x.center().x() - 0.3 * x.width(), x.center().y())));
        QCOMPARE(view->selectedText(), QStringLiteral("ck brown fo"));
        // Begun in the margin, left of the line: from its first character.
        QVERIFY(view->selectText(0, QPointF(page.left() - 20, q.center().y()), QPointF(x.right() - 0.2, x.center().y())));
        QCOMPARE(view->selectedText(), line);
        view->clearSelection();
        // (Closed: the probe in front, as the tests after have it.)
        app->slotFileClose(app->DocumentTab->indexOf(doc));
        QVERIFY(app->findDoc(dense) == nullptr);
        QVERIFY(app->gotoPage(file));
    }

    // Closed like any document.
    void itIsClosed()
    {
        QVERIFY(app->gotoPage(file));
        app->slotFileClose(app->DocumentTab->currentIndex());
        QVERIFY(app->findDoc(file) == nullptr);   // (the last tab closed: an untitled one in its place)
        for (int i = 0; i < app->DocumentTab->count(); ++i) QVERIFY(!QucsApp::isPdfDocument(app->DocumentTab->widget(i)));
    }
};

int main(int argc, char** argv)
{
    // (As the application: the core profile's OpenGL, before the
    // application - the GPU's drawing where there is one.)
    qucs_s::fem::GraphicsView::prepareGpu();
    QApplication app(argc, argv);
    TestPdfViewer test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_pdf_viewer.moc"
