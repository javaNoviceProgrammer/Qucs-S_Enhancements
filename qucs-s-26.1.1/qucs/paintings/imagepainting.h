/***************************************************************************
                              imagepainting.h
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
#ifndef IMAGEPAINTING_H
#define IMAGEPAINTING_H

#include "embeddedimage.h"
#include "rectangle.h"
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

class ImagePainting : public QObject, public qucs::Rectangle {
  Q_OBJECT
public:
  ImagePainting();
  Painting* newOne() override;
  void paint(QPainter* painter) override;
  bool load(const QString& s) override;
  QString save() override;
  QString saveCpp() override;
  QString saveJSON() override;
  bool Dialog(QWidget* parent = nullptr) override;
  static Element* info(QString& Name, char* &BitmapFile, bool getNewOne = false);

  void setImageFromPixmap(const QPixmap& pixmap);
  void setImageFromPath(const QString& path);
  void setImageFromClipboard();

  //! The image itself - its bytes, its format and the turns applied to it.
  const qucs_s::EmbeddedImage& embeddedImage() const { return m_image; }

  //! Put the image in this rectangle, centre worked out as load() does.
  void setPlacement(int left, int top, int right, int bottom);

  // Override selection and interaction methods
  bool getSelected(const QPoint& click, int tolerance) override;
  bool resizeTouched(const QPoint& click, int tolerance) override;
  void MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& cursor) override;
  bool MousePressing(Schematic* sch = nullptr) override;
  void MouseResizeMoving(int x, int y, Schematic* p) override;
  void ResetDragTracking();

  using Element::mirrorX;
  using Element::mirrorY;
  bool rotate() noexcept override;
  bool rotate(int xc, int yc) noexcept override;
  bool mirrorX() noexcept override;
  bool mirrorY() noexcept override;

  int getImageWidth() const;
  int getImageHeight() const;

private:
  qucs_s::EmbeddedImage m_image;
  //! Where the image was read from. Kept to show it, never read again.
  QString m_sourceFile;

  enum DraggedCorner { TopLeft, TopRight, BottomLeft, BottomRight, NotSet };
  DraggedCorner m_draggedCorner = NotSet;
  int m_lastDragX = -1;
  int m_lastDragY = -1;

  // Local pen properties
  QColor penColor;
  int penWidth;
  Qt::PenStyle penStyle;

  // Aspect ratio control
  bool m_keepAspectRatio;
  double m_aspectRatio; // cached aspect ratio

  // Dialog widget members
  QLineEdit* m_pathEdit;
  QLineEdit* m_widthEdit;
  QLineEdit* m_heightEdit;
  QCheckBox* m_aspectRatioCheck;
  QPushButton* m_resetButton;
  QLabel* m_statusLabel;
  //! The image the path in the dialog names, until the dialog is accepted.
  qucs_s::EmbeddedImage m_pendingImage;

  // Dialog handler methods
  void onBrowseClicked();
  void onResetClicked();
  void onAspectRatioToggled(bool checked);
  void onPathChanged(const QString& newPath);
  void updateHeight();
  void showImageState();

  // Helper methods
  void updateAspectRatio();
  //! The image of the dialog if it has one, else the one being painted.
  const qucs_s::EmbeddedImage& dialogImage() const;
};

#endif // IMAGEPAINTING_H
