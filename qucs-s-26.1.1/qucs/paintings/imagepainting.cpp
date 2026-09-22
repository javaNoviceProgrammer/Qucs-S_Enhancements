/***************************************************************************
                              imagepainting.cpp
                             ---------------
    copyright            : (C) 2025 by Andrés Martínez Mera
    email                : andresmartinezmera@gmail.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "imagepainting.h"
#include "filldialog.h"
#include "geometry/geometry.h"
#include "misc.h"
#include "schematic.h"

#include <QClipboard>
#include <QMimeData>
#include <QTransform>

#include <cmath>
#include "ink.h"

namespace {
//! The placeholder written for a painting that carries no image.
const char* const NoImage = "-";
}

ImagePainting::ImagePainting() :
      Rectangle(false),
      penColor(Qt::black),
      penWidth(1),
      penStyle(Qt::SolidLine),
      m_keepAspectRatio(true),
      m_aspectRatio(1.0)
{
  Name = "ImagePainting ";
}

Painting* ImagePainting::newOne()
{
  return new ImagePainting();
}


void ImagePainting::paint(QPainter* painter) {
  const QRect bounds = boundingRect();

  if (!m_image.isNull() && !bounds.isEmpty()) {
    // Render the image at the size it is about to be shown at, so that a
    // vector image stays sharp however far the schematic is zoomed in.
    const QTransform& t = painter->transform();
    const QSize target(qRound(bounds.width() * std::hypot(t.m11(), t.m12())),
                       qRound(bounds.height() * std::hypot(t.m21(), t.m22())));
    const QPixmap pixmap = m_image.pixmap(target);

    if (!pixmap.isNull()) {
      painter->save();
      painter->setPen(Qt::NoPen);
      painter->setRenderHint(QPainter::SmoothPixmapTransform);
      painter->drawPixmap(QRectF{bounds}, pixmap, QRectF{pixmap.rect()});
      painter->restore();
    }

    // Draw selection handles when selected
    if (isSelected) {
      painter->setPen(qucs_s::ink::on(QPen(Qt::darkGray, penWidth + 5)));
      painter->drawRect(bounds);
      painter->setPen(qucs_s::ink::on(QPen(Qt::white, penWidth, penStyle)));
      painter->drawRect(bounds);

      // Draw resize handles
      const auto handles = bounds.marginsAdded({0, 0, 1, 1});
      misc::draw_resize_handle(painter, handles.topLeft());
      misc::draw_resize_handle(painter, handles.topRight());
      misc::draw_resize_handle(painter, handles.bottomRight());
      misc::draw_resize_handle(painter, handles.bottomLeft());
    }
  } else {
    // Ensure base class is in valid state
    if (x1 == x2) x2 = x1 + 100; // Default width
    if (y1 == y2) y2 = y1 + 100; // Default height
    Rectangle::paint(painter);
  }
}


// "ImagePainting x1 y1 x2 y2 <base64> <format> <turns> <mirrored>"
// Up to 26.1.2 only the first six fields were written and the image was
// always a PNG; those files still load, the missing fields default.
bool ImagePainting::load(const QString& s) {
  QStringList parts = s.split(' ', Qt::SkipEmptyParts);
  if (parts.size() < 5) return false;

  // Extract coordinates
  if (parts[0] != "ImagePainting") return false;

  bool ok;
  x1 = misc::clampCoordinate(parts[1].toInt(&ok));
  if (!ok) return false;

  y1 = misc::clampCoordinate(parts[2].toInt(&ok));
  if (!ok) return false;

  x2 = misc::clampCoordinate(parts[3].toInt(&ok));
  if (!ok) return false;

  y2 = misc::clampCoordinate(parts[4].toInt(&ok));
  if (!ok) return false;

  updateCenter();

  m_image.clear();
  m_sourceFile.clear();
  m_aspectRatio = 1.0;

  const QString imageData = parts.size() > 5 ? parts[5] : QString();
  if (imageData.isEmpty() || imageData == QLatin1String(NoImage))
    return true;   // a frame that lost its image; keep the rest of the file

  QString format = parts.size() > 6 ? parts[6] : QString();
  if (format == QLatin1String(NoImage) || format == QLatin1String("none")) format.clear();

  if (!m_image.loadBase64(imageData, format)) {
    qWarning("Failed to load the image of an ImagePainting");
    return true;
  }

  const int turns = parts.size() > 7 ? parts[7].toInt() : 0;
  const bool mirrored = parts.size() > 8 && parts[8].toInt() != 0;
  m_image.setTransform(turns, mirrored);

  updateAspectRatio();
  return true;
}

QString ImagePainting::save() {
  // The image travels with the document: its bytes are written here, so the
  // file it was read from is not needed again.
  return QString("ImagePainting %1 %2 %3 %4 %5 %6 %7 %8")
      .arg(x1).arg(y1).arg(x2).arg(y2)
      .arg(m_image.isNull() ? QString(NoImage) : m_image.base64())
      .arg(m_image.isNull() ? QString(NoImage) : m_image.format())
      .arg(m_image.quarterTurns())
      .arg(m_image.isFlipped() ? 1 : 0);
}

QString ImagePainting::saveCpp() {
  // Customize as needed; example:
  return QString("new ImagePainting(%1, %2, %3, %4, \"%5\")")
      .arg(x1).arg(y1).arg(x2-x1).arg(y2-y1).arg(m_sourceFile);
}

QString ImagePainting::saveJSON() {
  return QStringLiteral("{\"type\":\"ImagePainting\",\"image\":\"%1\",%2}")
      .arg(m_sourceFile, Rectangle::saveJSON().mid(1)); // Merge with base JSON
}

// Override getSelected to handle image area
bool ImagePainting::getSelected(const QPoint& click, int tolerance) {
  // Always check if click is within bounds (whether filled or not)
  return boundingRect()
      .marginsAdded({tolerance, tolerance, tolerance, tolerance})
      .contains(click);
}

// Override resizeTouched to maintain resize functionality
bool ImagePainting::resizeTouched(const QPoint& click, int tolerance) {
  return Rectangle::resizeTouched(click, tolerance);
}

// Override mouse interaction methods
void ImagePainting::MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& cursor) {
  // Get the cursor coordinates
  x1 = onGrid.x();
  y1 = onGrid.y();
  x2 = x1;
  y2 = y1;

  // Draw a symbol (two mountains) while hovering
  // Draw frame
  sch->PostPaintEvent(_Rect, cursor.x() + 13, cursor.y(), 105, 48, 0, 0, true);

  // Draw the sun (larger, in the top-right corner)
  sch->PostPaintEvent(_Ellipse, cursor.x() + 100, cursor.y() + 8, 12, 12, 0, 0, true); // (x, y, width, height)

  // Draw the mountain on the left
  sch->PostPaintEvent(_Line, cursor.x() + 15, cursor.y() + 44, cursor.x() + 45, cursor.y() + 12, 0, 0, true); // left base to peak
  sch->PostPaintEvent(_Line, cursor.x() + 45, cursor.y() + 12, cursor.x() + 75, cursor.y() + 44, 0, 0, true); // peak to right base

  // Draw the mountain on the right
  sch->PostPaintEvent(_Line, cursor.x() + 45, cursor.y() + 44, cursor.x() + 81, cursor.y() + 4, 0, 0, true); // left base to peak
  sch->PostPaintEvent(_Line, cursor.x() + 81, cursor.y() + 4, cursor.x() + 115, cursor.y() + 44, 0, 0, true); // peak to right base

  // Add a ground line
  sch->PostPaintEvent(_Line, cursor.x() + 15, cursor.y() + 44, cursor.x() + 115, cursor.y() + 44, 0, 0, true); // ground
}

bool ImagePainting::MousePressing(Schematic* sch) {
  if (m_image.isNull()) {
    QWidget* parentWidget = sch ? sch->parentWidget() : nullptr;
    if (!parentWidget) {
      parentWidget = QApplication::activeWindow();
    }

    QString newPath = QFileDialog::getOpenFileName(
        parentWidget,
        QObject::tr("Select Image"),
        QDir::homePath(),
        qucs_s::EmbeddedImage::fileDialogFilter()
        );

    if (newPath.isEmpty()) return false;   // No image selected.

    setImageFromPath(newPath);

    // Set dimensions to actual image size if image loaded successfully
    const QSize size = m_image.isNull() ? QSize(100, 100) : m_image.size();
    setPlacement(x1, y1, x1 + size.width(), y1 + size.height());
  }

  return true;
}

void ImagePainting::MouseResizeMoving(int x, int y, Schematic* p) {
  if (m_keepAspectRatio && m_aspectRatio > 0) {
    // If this is the first call or position jumped significantly, determine the corner
    if (m_draggedCorner == NotSet || abs(x - m_lastDragX) > 50 || abs(y - m_lastDragY) > 50) {
      // Calculate distances to each corner
      int distToTopLeft = abs(x - x1) + abs(y - y1);
      int distToTopRight = abs(x - x2) + abs(y - y1);
      int distToBottomLeft = abs(x - x1) + abs(y - y2);
      int distToBottomRight = abs(x - x2) + abs(y - y2);

      // Find the minimum distance to determine which corner is being dragged
      int minDist = qMin(qMin(distToTopLeft, distToTopRight), qMin(distToBottomLeft, distToBottomRight));

      if (minDist == distToTopLeft) {
        m_draggedCorner = TopLeft;
      } else if (minDist == distToTopRight) {
        m_draggedCorner = TopRight;
      } else if (minDist == distToBottomLeft) {
        m_draggedCorner = BottomLeft;
      } else {
        m_draggedCorner = BottomRight;
      }
    }

    m_lastDragX = x;
    m_lastDragY = y;

    int constrainedX = x;
    int constrainedY = y;

    // Use the stored corner that was determined at drag start
    switch (m_draggedCorner) {
    case TopLeft: {
      int deltaX = x2 - x;
      int deltaY = y2 - y;
      if (deltaX <= 0 || deltaY <= 0) return;

      double scale = (double)deltaX / (x2 - x1);
      int newWidth = qRound((x2 - x1) * scale);
      int newHeight = qRound(newWidth * m_aspectRatio);
      constrainedX = x2 - newWidth;
      constrainedY = y2 - newHeight;
      break;
    }
    case TopRight: {
      int deltaX = x - x1;
      int deltaY = y2 - y;
      if (deltaX <= 0 || deltaY <= 0) return;

      double scale = (double)deltaX / (x2 - x1);
      int newWidth = qRound((x2 - x1) * scale);
      int newHeight = qRound(newWidth * m_aspectRatio);
      constrainedX = x1 + newWidth;
      constrainedY = y2 - newHeight;
      break;
    }
    case BottomLeft: {
      int deltaX = x2 - x;
      int deltaY = y - y1;
      if (deltaX <= 0 || deltaY <= 0) return;

      double scale = (double)deltaX / (x2 - x1);
      int newWidth = qRound((x2 - x1) * scale);
      int newHeight = qRound(newWidth * m_aspectRatio);
      constrainedX = x2 - newWidth;
      constrainedY = y1 + newHeight;
      break;
    }
    case BottomRight: {
      int deltaX = x - x1;
      int deltaY = y - y1;
      if (deltaX <= 0 || deltaY <= 0) return;

      double scale = (double)deltaX / (x2 - x1);
      int newWidth = qRound((x2 - x1) * scale);
      int newHeight = qRound(newWidth * m_aspectRatio);
      constrainedX = x1 + newWidth;
      constrainedY = y1 + newHeight;
      break;
    }
    default:
      break;
    }

    Rectangle::MouseResizeMoving(constrainedX, constrainedY, p);
  } else {
    Rectangle::MouseResizeMoving(x, y, p);
  }
}

void ImagePainting::ResetDragTracking() {
  m_draggedCorner = NotSet;
  m_lastDragX = -1;
  m_lastDragY = -1;
}


bool ImagePainting::Dialog(QWidget* parent) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QObject::tr("Image Properties"));
  auto* layout = new QVBoxLayout(&dialog);

  m_pendingImage = qucs_s::EmbeddedImage();

  // Add image path UI
  auto* imageLayout = new QHBoxLayout;
  auto* pathLabel = new QLabel(QObject::tr("Image file:"));
  m_pathEdit = new QLineEdit(m_sourceFile);
  auto* browseButton = new QPushButton(QObject::tr("Browse..."));

  // The image is always kept inside the document; the path only says where
  // it was read from.
  m_statusLabel = new QLabel();
  showImageState();

  // Connect browse button
  QObject::connect(browseButton, &QPushButton::clicked, this, &ImagePainting::onBrowseClicked);

  imageLayout->addWidget(pathLabel);
  imageLayout->addWidget(m_pathEdit);
  imageLayout->addWidget(browseButton);

  // Add dimensions UI
  auto* dimensionsLayout = new QVBoxLayout;

  // Width input
  auto* widthLayout = new QHBoxLayout;
  auto* widthLabel = new QLabel(QObject::tr("Width:"));
  m_widthEdit = new QLineEdit(QString::number(x2 - x1));
  m_widthEdit->setValidator(new QIntValidator(1, 10000, &dialog));
  widthLayout->addWidget(widthLabel);
  widthLayout->addWidget(m_widthEdit);

  // Height input
  auto* heightLayout = new QHBoxLayout;
  auto* heightLabel = new QLabel(QObject::tr("Height:"));
  m_heightEdit = new QLineEdit(QString::number(y2 - y1));
  m_heightEdit->setValidator(new QIntValidator(1, 10000, &dialog));
  heightLayout->addWidget(heightLabel);
  heightLayout->addWidget(m_heightEdit);

  // Aspect ratio checkbox - initialize with current state
  m_aspectRatioCheck = new QCheckBox(QObject::tr("Keep aspect ratio"));
  m_aspectRatioCheck->setChecked(m_keepAspectRatio);

  // Reset to original button
  m_resetButton = new QPushButton(QObject::tr("Reset to original dimensions"));
  m_resetButton->setEnabled(!m_image.isNull());

  dimensionsLayout->addLayout(widthLayout);
  dimensionsLayout->addLayout(heightLayout);
  dimensionsLayout->addWidget(m_aspectRatioCheck);
  dimensionsLayout->addWidget(m_resetButton);

  // Connect signals to handlers
  QObject::connect(m_resetButton, &QPushButton::clicked, this, &ImagePainting::onResetClicked);
  QObject::connect(m_aspectRatioCheck, &QCheckBox::toggled, this, &ImagePainting::onAspectRatioToggled);
  QObject::connect(m_pathEdit, &QLineEdit::textChanged, this, &ImagePainting::onPathChanged);

  // Connect width change to height calculation when aspect ratio is locked
  QObject::connect(m_widthEdit, &QLineEdit::textChanged, this, [this]() {
    if (m_aspectRatioCheck->isChecked()) {
      updateHeight();
    }
  });

  layout->addWidget(m_statusLabel);
  layout->addLayout(imageLayout);
  layout->addLayout(dimensionsLayout);

  QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  layout->addWidget(&buttons);

  if (dialog.exec() == QDialog::Rejected) {
    m_pendingImage = qucs_s::EmbeddedImage();
    return false;
  }

  // Take over the image the path names, if it is a new one. Clearing the
  // path leaves the image that is already embedded alone.
  const QString newPath = m_pathEdit->text();
  if (newPath != m_sourceFile && !m_pendingImage.isNull()) {
    m_pendingImage.setTransform(m_image.quarterTurns(), m_image.isFlipped());
    m_image = m_pendingImage;
    m_sourceFile = newPath;
    updateAspectRatio();
  }
  m_pendingImage = qucs_s::EmbeddedImage();

  // Update persistent aspect ratio setting
  m_keepAspectRatio = m_aspectRatioCheck->isChecked();

  // Update dimensions
  int newWidth = m_widthEdit->text().toInt();
  int newHeight = m_heightEdit->text().toInt();

  if (newWidth > 0 && newHeight > 0) {
    x2 = x1 + newWidth;
    y2 = y1 + newHeight;
    updateCenter();
  }

  return true;
}


Element* ImagePainting::info(QString& Name, char* &BitmapFile, bool getNewOne) {
  Name = QObject::tr("Image");
  BitmapFile = (char*)"ImagePainting";
  return getNewOne ? new ImagePainting() : nullptr;
}

// Rotates the image a quarter turn counter-clockwise, together with the
// rectangle it is drawn in - a square image turns too.
bool ImagePainting::rotate() noexcept {
  qucs_s::geom::rotate_point_ccw(x1, y1, cx, cy);
  qucs_s::geom::rotate_point_ccw(x2, y2, cx, cy);
  if (x2 < x1) std::swap(x1, x2);
  if (y2 < y1) std::swap(y1, y2);
  updateCenter();

  m_image.rotate();
  updateAspectRatio();
  return true;
}

bool ImagePainting::rotate(int xc, int yc) noexcept {
  if (cx == xc && cy == yc) return rotate();

  qucs_s::geom::rotate_point_ccw(x1, y1, xc, yc);
  qucs_s::geom::rotate_point_ccw(x2, y2, xc, yc);
  if (x2 < x1) std::swap(x1, x2);
  if (y2 < y1) std::swap(y1, y2);
  updateCenter();

  m_image.rotate();
  updateAspectRatio();
  return true;
}

bool ImagePainting::mirrorX() noexcept {
  m_image.mirrorX();
  return true;
}

bool ImagePainting::mirrorY() noexcept {
  m_image.mirrorY();
  return true;
}


void ImagePainting::setPlacement(int left, int top, int right, int bottom) {
  x1 = left;
  y1 = top;
  x2 = right;
  y2 = bottom;
  updateCenter();
}


void ImagePainting::setImageFromPixmap(const QPixmap& pixmap) {
  if (m_image.loadPixmap(pixmap)) {
    m_sourceFile.clear();   // pasted, no file behind it
    updateAspectRatio();
  }
}

void ImagePainting::setImageFromPath(const QString& path) {
  if (path.isEmpty()) return;

  if (m_image.loadFile(path)) {
    m_sourceFile = path;
    updateAspectRatio();
  } else {
    qWarning("Failed to load image: %s", qUtf8Printable(path));
  }
}

void ImagePainting::setImageFromClipboard() {
  QClipboard* clipboard = QApplication::clipboard();
  if (clipboard->mimeData()->hasImage()) {
    QImage clipboardImage = clipboard->image();
    if (!clipboardImage.isNull()) {
      setImageFromPixmap(QPixmap::fromImage(clipboardImage));
    }
  }
}


void ImagePainting::onBrowseClicked() {
  QString path = QFileDialog::getOpenFileName(
      m_pathEdit->parentWidget(),
      QObject::tr("Select Image"),
      m_sourceFile.isEmpty() ? QDir::homePath() : m_sourceFile,
      qucs_s::EmbeddedImage::fileDialogFilter()
      );

  if (!path.isEmpty()) {
    m_pathEdit->setText(path);   // onPathChanged reads the file
  }
}

void ImagePainting::onResetClicked() {
  const QSize original = dialogImage().size();
  if (original.isEmpty()) return;

  m_widthEdit->setText(QString::number(original.width()));
  m_heightEdit->setText(QString::number(original.height()));
}

void ImagePainting::onAspectRatioToggled(bool checked) {
  m_heightEdit->setEnabled(!checked);
  if (checked) {
    updateHeight();
  }
}

void ImagePainting::onPathChanged(const QString& newPath) {
  m_pendingImage = qucs_s::EmbeddedImage();

  if (!newPath.isEmpty() && newPath != m_sourceFile)
    m_pendingImage.loadFile(newPath);

  m_resetButton->setEnabled(!dialogImage().isNull());
  showImageState();

  if (!m_pendingImage.isNull() && m_aspectRatioCheck->isChecked())
    updateHeight();
}

void ImagePainting::showImageState() {
  if (!m_statusLabel) return;

  const QString format = dialogImage().format().toUpper();
  if (!m_pendingImage.isNull()) {
    m_statusLabel->setText(
        QObject::tr("%1 image, will be stored in the document").arg(format));
    m_statusLabel->setStyleSheet("color: green; font-style: italic;");
  } else if (!m_image.isNull()) {
    m_statusLabel->setText(
        QObject::tr("%1 image stored in the document").arg(format));
    m_statusLabel->setStyleSheet("color: green; font-style: italic;");
  } else if (!m_pathEdit->text().isEmpty()) {
    m_statusLabel->setText(QObject::tr("Cannot read this file"));
    m_statusLabel->setStyleSheet("color: red; font-style: italic;");
  } else {
    m_statusLabel->setText(QObject::tr("No image loaded"));
    m_statusLabel->setStyleSheet("color: red; font-style: italic;");
  }
}

void ImagePainting::updateHeight() {
  if (!m_aspectRatioCheck || !m_aspectRatioCheck->isChecked()) return;
  if (!m_widthEdit || !m_heightEdit) return;

  const QSize original = dialogImage().size();
  if (original.isEmpty()) return;

  const int width = m_widthEdit->text().toInt();
  if (width <= 0) return;

  const double aspectRatio = double(original.height()) / double(original.width());
  m_heightEdit->setText(QString::number(qRound(width * aspectRatio)));
}

const qucs_s::EmbeddedImage& ImagePainting::dialogImage() const {
  return m_pendingImage.isNull() ? m_image : m_pendingImage;
}

void ImagePainting::updateAspectRatio() {
  const QSize size = m_image.size();
  m_aspectRatio = size.isEmpty() ? 1.0 : double(size.height()) / double(size.width());
}

// Needed to have the image size at schematic.cpp when drag and dropping
int ImagePainting::getImageWidth() const {
  const QSize size = m_image.size();
  return size.isEmpty() ? 100 : size.width();
}

int ImagePainting::getImageHeight() const {
  const QSize size = m_image.size();
  return size.isEmpty() ? 100 : size.height();
}
