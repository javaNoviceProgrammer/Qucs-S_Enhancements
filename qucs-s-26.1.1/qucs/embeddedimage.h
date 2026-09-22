/***************************************************************************
                              embeddedimage.h
                             -----------------
    An image kept by value: the bytes of the file it was read from, the
    format they are in and the quarter turns and flip applied to it.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef EMBEDDEDIMAGE_H
#define EMBEDDEDIMAGE_H

#include <QByteArray>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QStringList>

namespace qucs_s {

/*!
 * \brief An image carried by the document that shows it.
 *
 * The file an image comes from is read once, when it is picked, and its
 * bytes are kept here; everything afterwards - drawing, saving, loading,
 * rotating - works on those bytes, so the file itself may be moved or
 * deleted. A vector image (SVG) keeps its source and is rendered at the
 * size it is drawn at, so it stays sharp at any zoom; every other format
 * keeps the file's own bytes and is scaled.
 */
class EmbeddedImage {
public:
  //! Every format that can be read, as a QFileDialog filter string.
  static QString fileDialogFilter();
  //! Every readable suffix, lower case and without the dot.
  static QStringList supportedSuffixes();
  //! Whether this path names a file in one of those formats.
  static bool isSupportedFile(const QString& path);

  //! Read a file. Keeps its bytes as they are.
  bool loadFile(const QString& path);
  //! Take the bytes of an image file. An empty format is sniffed.
  bool loadEncoded(const QByteArray& data, const QString& format = QString());
  //! Take base64 of the above, as a document stores it.
  bool loadBase64(const QString& base64, const QString& format = QString());
  //! Take a pixmap that has no file behind it (the clipboard); kept as PNG.
  bool loadPixmap(const QPixmap& pixmap);

  bool isNull() const { return m_data.isEmpty() || !m_sourceSize.isValid(); }
  void clear();

  const QByteArray& data() const { return m_data; }
  const QString& format() const { return m_format; }
  QString base64() const { return QString::fromLatin1(m_data.toBase64()); }
  bool isVector() const { return m_format == QLatin1String("svg"); }

  //! The size the image would like to be drawn at, turns applied.
  QSize size() const;

  int  quarterTurns() const { return m_turns; }   //!< counter-clockwise, 0..3
  bool isFlipped() const { return m_flipped; }    //!< about the horizontal axis
  void setTransform(int quarterTurns, bool flipped);

  void rotate();   //!< a quarter turn counter-clockwise, as every other element
  void mirrorX();  //!< about the horizontal axis
  void mirrorY();  //!< about the vertical axis

  //! The image drawn into a rectangle of \a target pixels, turns applied.
  QPixmap pixmap(const QSize& target) const;

private:
  bool decode();
  QPixmap render(const QSize& target) const;

  QByteArray m_data;
  QString m_format;
  QSize m_sourceSize;
  int m_turns = 0;
  bool m_flipped = false;

  mutable QPixmap m_cache;
  mutable QSize m_cacheSize;
  mutable int m_cacheTurns = -1;
  mutable bool m_cacheFlipped = false;
};

} // namespace qucs_s

#endif // EMBEDDEDIMAGE_H
