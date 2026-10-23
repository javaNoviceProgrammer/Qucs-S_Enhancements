/*
 * pptxdoc.h - a PowerPoint presentation (.pptx) in a tab: its slides one
 *             under the other, their thumbnails beside, the notes of the
 *             slide in sight below; found in, played full screen, printed
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_PPTXDOC_H
#define QUCS_PPTXDOC_H

#include "qucsdoc.h"

#include <QAbstractScrollArea>
#include <QCache>
#include <QFrame>
#include <QImage>
#include <QPointer>

#include <memory>

class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTextBrowser;
class QTimer;
class QToolButton;

namespace qucs_s::pptx {

class Presentation;

/// The slides, one under the other, as large as the zoom says: each drawn
/// when it comes in sight, kept for the zoom it was drawn at.
class SlideView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    /// How the zoom is chosen: as set, or so that a slide fills the width
    /// or the whole of the view.
    enum class Fit { None, Width, Slide };

    explicit SlideView(QWidget* parent = nullptr);
    /// \a presentation shown from its first slide, a slide as wide as the
    /// view (restore() puts a view back).
    void setPresentation(std::shared_ptr<const Presentation> presentation);
    std::shared_ptr<const Presentation> presentation() const { return a_presentation; }

    /// Pixels a point of the slide (1: 72 an inch, 100%).
    qreal zoom() const { return a_zoom; }
    void setZoom(qreal zoom);
    /// To the next of the usual scales, up or down.
    void zoomStep(bool in);
    Fit fit() const { return a_fit; }
    void setFit(Fit fit);
    /// The slide in sight: the one at a third of the view's height.
    int currentSlide() const { return a_current; }
    /// Scrolls to slide \a index, its top at the view's.
    void goToSlide(int index);
    /// Shows \a region (points of the slide) of slide \a index as large as
    /// the view takes it, in its middle.
    void showRegion(int index, const QRectF& region);
    /// Where slide \a index is in the viewport.
    QRect slideRect(int index) const;
    /// The slide at \a pos (viewport), -1 where there is none.
    int slideAt(QPoint pos) const;
    /// The slides drawn since the start (for the tests).
    int slidesDrawn() const { return a_drawn; }
    /// The drawings kept dropped (the file read again).
    void clearCache();

    /// Where the view is: its fit or zoom, the slide in sight, how far down
    /// it the view's top is (a fraction of it), how far across (of what
    /// can be scrolled).
    struct ViewState {
        Fit fit = Fit::Width;
        qreal zoom = 1;
        int slide = 0;
        qreal down = 0;
        qreal across = 0;
    };
    ViewState state() const;
    void restore(const ViewState& state);

    static constexpr qreal MinZoom = 0.1;
    static constexpr qreal MaxZoom = 8.0;

signals:
    void currentSlideChanged(int index);
    void zoomChanged(qreal zoom);
    void menuRequested(QPoint globalPos, int slide);
    void slideDoubleClicked(int slide);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    void relayout();
    void updateCurrent();
    QImage drawn(int index, qreal scale, bool* fresh);
    QRectF contentRect(int index) const;   // in the content's pixels

    std::shared_ptr<const Presentation> a_presentation;
    qreal a_zoom = 1.0;
    Fit a_fit = Fit::Width;
    int a_current = 0;
    QSize a_content;
    QCache<QString, QImage> a_cache;
    int a_drawn = 0;
    bool a_relaying = false;
};

/// The slides played full screen, one at a time on black: on with a
/// click, a space, an arrow or Page Down; back with Backspace, the left
/// arrow or Page Up; Home, End; a number and Return goes to that slide;
/// Escape ends it. Hidden slides are left out.
class SlideShow : public QWidget
{
    Q_OBJECT

public:
    SlideShow(std::shared_ptr<const Presentation> presentation, int from, QWidget* parent = nullptr);
    int current() const { return a_current; }
    /// Past the last slide: the end's black, before it closes.
    bool atEnd() const { return a_done; }
    void next();
    void previous();
    void goTo(int index);
    /// The first and the last slide shown (Home, End).
    void first();
    void last();

signals:
    /// Slide \a index shown (not the end's black).
    void slideShown(int index);
    /// It has ended, at slide \a last.
    void ended(int last);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    int step(int from, int by) const;
    void showSlide(int index);
    std::shared_ptr<const Presentation> a_presentation;
    int a_current = 0;
    QString a_typed;
    QImage a_image;
    int a_imageIndex = -1;
    QSize a_imageSize;
    bool a_done = false;   // the end's black, before it closes
};

} // namespace qucs_s::pptx

class PptxDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    PptxDoc(QucsApp* app, const QString& name);
    ~PptxDoc() override;

    void setName(const QString& name) override;
    bool load() override;
    int save() override;
    void print(QPrinter* printer, QPainter* painter, bool printAll, bool fitToPage) override;
    void becomeCurrent(bool) override;
    double zoomBy(double factor) override;
    void showAll() override;
    void zoomToSelection() override;
    void showNoZoom() override;

    /// What is shown (null while nothing could be read).
    std::shared_ptr<const qucs_s::pptx::Presentation> presentation() const { return a_presentation; }
    int slideCount() const;
    /// The slide in sight (from 0).
    int currentSlide() const;
    void goToSlide(int index);
    qucs_s::pptx::SlideView* view() const { return a_view; }
    /// Why nothing is shown, when nothing is (empty: it is).
    QString problem() const { return a_problem; }
    /// Reads the file again, the slide and the zoom kept.
    bool reload();
    /// Slide \a index drawn \a scale pixels a point.
    QImage slideImage(int index, qreal scale) const;

    /// Searches the slides' text and notes for \a text: the first slide
    /// with it from the one in sight on shown (unless \a go is false). The
    /// slides with it.
    QList<int> find(const QString& text, bool go = true);
    void findNext(bool backwards = false);
    /// The find bar shown with \a text in it, found as if typed there (the
    /// focus left where it is).
    void setFindText(const QString& text);
    bool findShown() const;
    QString findText() const;
    /// The slides with the find's text (from 0), and which of them is shown.
    QList<int> hits() const { return a_hits; }
    int hitShown() const { return a_hit; }
    /// Slide \a index's \a region (points) as large as the view takes it.
    void showRegion(int index, const QRectF& region);

    /// Plays the slides full screen from \a from (-1: the one in sight).
    void startShow(int from = -1);
    /// The slide show playing (shown; one closed is gone a moment later).
    qucs_s::pptx::SlideShow* slideShow() const { return a_show && a_show->isVisible() ? a_show.data() : nullptr; }

    bool notesShown() const;
    void setNotesShown(bool shown);
    bool sidebarShown() const;
    void setSidebarShown(bool shown);

    // For the tests.
    QListWidget* thumbnails() const { return a_thumbnails; }
    QLineEdit* slideField() const { return a_slideField; }
    QLineEdit* searchField() const { return a_searchField; }
    QLabel* searchLabel() const { return a_searchLabel; }
    QTextBrowser* notesView() const { return a_notes; }
    QComboBox* zoomBox() const { return a_zoomBox; }

public slots:
    void nextSlide();
    void previousSlide();
    void firstSlide();
    void lastSlide();
    void showSearch();
    void hideSearch();
    void copySlide();
    void copySlideText();
    void exportSlide();
    void copyPath();
    void openExternally();
    void revealInFileManager();

signals:
    void reloaded();
    void currentSlideChanged(int index);

protected:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void restyle();
    void showPresentation(std::shared_ptr<qucs_s::pptx::Presentation> presentation, bool keepView);
    void showProblem(const QString& text);
    void watch();
    void fillThumbnails();
    void renderSomeThumbnails();
    void updateSlideField();
    void updateZoomBox();
    void updateNotes();
    void showMenu(QPoint globalPos, int slide);
    void fillMenu(QMenu* menu);

    std::shared_ptr<qucs_s::pptx::Presentation> a_presentation;
    QString a_problem;
    QString a_source;   // the file read
    QFileSystemWatcher* a_watcher = nullptr;
    QTimer* a_reloadTimer = nullptr;
    QTimer* a_thumbTimer = nullptr;
    int a_thumbsDone = 0;
    QList<int> a_hits;   // the slides the search found
    int a_hit = -1;
    QPointer<qucs_s::pptx::SlideShow> a_show;

    QStackedWidget* a_stack = nullptr;
    QWidget* a_toolbar = nullptr;
    QToolButton* a_sidebarButton = nullptr;
    QToolButton* a_prevButton = nullptr;
    QToolButton* a_nextButton = nullptr;
    QLineEdit* a_slideField = nullptr;
    QLabel* a_slideCount = nullptr;
    QToolButton* a_zoomOutButton = nullptr;
    QToolButton* a_zoomInButton = nullptr;
    QComboBox* a_zoomBox = nullptr;
    QToolButton* a_notesButton = nullptr;
    QToolButton* a_playButton = nullptr;
    QToolButton* a_findButton = nullptr;
    QToolButton* a_menuButton = nullptr;
    QMenu* a_moreMenu = nullptr;
    QWidget* a_searchBar = nullptr;
    QLineEdit* a_searchField = nullptr;
    QLabel* a_searchLabel = nullptr;
    QSplitter* a_splitter = nullptr;
    QSplitter* a_body = nullptr;
    QListWidget* a_thumbnails = nullptr;
    qucs_s::pptx::SlideView* a_view = nullptr;
    QWidget* a_notesPane = nullptr;
    QLabel* a_notesTitle = nullptr;
    QTextBrowser* a_notes = nullptr;
    QLabel* a_problemLabel = nullptr;
};

#endif // QUCS_PPTXDOC_H
