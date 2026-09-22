/***************************************************************************
                             embeddedimage.cpp
                            -------------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "embeddedimage.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QObject>
#include <QPainter>
#include <QSvgRenderer>
#include <QTransform>

namespace {

// No image is rendered larger than this, however far the schematic is
// zoomed in.
constexpr int MaxRenderedPixels = 4096;

// The only vector format, under its one name: an .svgz is gzipped SVG and
// QSvgRenderer takes it as it comes.
const char* const VectorFormat = "svg";

bool looksLikeSvg(const QByteArray& data)
{
  if (data.startsWith("\x1f\x8b")) return true;   // gzip, hence .svgz
  return data.left(1024).contains("<svg");
}

QString sniffFormat(const QByteArray& data)
{
  if (looksLikeSvg(data)) return QString::fromLatin1(VectorFormat);

  QBuffer buffer;
  buffer.setData(data);
  if (!buffer.open(QIODevice::ReadOnly)) return QString();
  const QByteArray format = QImageReader(&buffer).format();
  return QString::fromLatin1(format).toLower();
}

} // namespace

QStringList qucs_s::EmbeddedImage::supportedSuffixes()
{
  QStringList suffixes;
  const QList<QByteArray> formats = QImageReader::supportedImageFormats();
  for (const QByteArray& format : formats) {
    const QString suffix = QString::fromLatin1(format).toLower();
    if (!suffixes.contains(suffix)) suffixes.append(suffix);
  }
  // Qucs-S renders SVG itself, so it is offered whether or not Qt's own
  // SVG image plugin is installed next to the application.
  for (const QString& vector : {QStringLiteral("svg"), QStringLiteral("svgz")})
    if (!suffixes.contains(vector)) suffixes.append(vector);

  suffixes.sort();
  return suffixes;
}

QString qucs_s::EmbeddedImage::fileDialogFilter()
{
  QStringList patterns;
  const QStringList suffixes = supportedSuffixes();
  patterns.reserve(suffixes.size());
  for (const QString& suffix : suffixes) patterns.append("*." + suffix);

  return QObject::tr("Images") + " (" + patterns.join(' ') + ");;"
       + QObject::tr("All files") + " (*)";
}

bool qucs_s::EmbeddedImage::isSupportedFile(const QString& path)
{
  if (path.isEmpty()) return false;

  const QFileInfo info(path);
  if (!info.exists() || !info.isFile()) return false;

  return supportedSuffixes().contains(info.suffix().toLower());
}

void qucs_s::EmbeddedImage::clear()
{
  m_data.clear();
  m_format.clear();
  m_sourceSize = QSize();
  m_turns = 0;
  m_flipped = false;
  m_cache = QPixmap();
  m_cacheTurns = -1;
}

bool qucs_s::EmbeddedImage::loadFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;

  const QByteArray data = file.readAll();
  file.close();

  const QString suffix = QFileInfo(path).suffix().toLower();
  const QString format = (suffix == QLatin1String("svg") || suffix == QLatin1String("svgz"))
                             ? QString::fromLatin1(VectorFormat)
                             : QString();
  return loadEncoded(data, format);
}

bool qucs_s::EmbeddedImage::loadEncoded(const QByteArray& data, const QString& format)
{
  if (data.isEmpty()) return false;

  const QByteArray previousData = m_data;
  const QString previousFormat = m_format;

  m_data = data;
  m_format = format.isEmpty() ? sniffFormat(data) : format.toLower();
  if (m_format == QLatin1String("svgz")) m_format = QString::fromLatin1(VectorFormat);

  if (!decode()) {
    m_data = previousData;
    m_format = previousFormat;
    decode();
    return false;
  }

  m_cache = QPixmap();
  m_cacheTurns = -1;
  return true;
}

bool qucs_s::EmbeddedImage::loadBase64(const QString& base64, const QString& format)
{
  const QByteArray data =
      QByteArray::fromBase64(base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  return loadEncoded(data, format);
}

bool qucs_s::EmbeddedImage::loadPixmap(const QPixmap& pixmap)
{
  if (pixmap.isNull()) return false;

  QByteArray data;
  QBuffer buffer(&data);
  if (!buffer.open(QIODevice::WriteOnly)) return false;
  if (!pixmap.save(&buffer, "PNG")) return false;
  buffer.close();

  return loadEncoded(data, QStringLiteral("png"));
}

bool qucs_s::EmbeddedImage::decode()
{
  m_sourceSize = QSize();
  if (m_data.isEmpty()) return false;

  if (isVector()) {
    QSvgRenderer renderer(m_data);
    if (!renderer.isValid()) return false;
    m_sourceSize = renderer.defaultSize();
    if (m_sourceSize.isEmpty()) m_sourceSize = QSize(100, 100);
    return true;
  }

  QBuffer buffer;
  buffer.setData(m_data);
  if (!buffer.open(QIODevice::ReadOnly)) return false;

  QImageReader reader(&buffer);
  m_sourceSize = reader.size();
  if (m_sourceSize.isEmpty()) {   // a format whose reader cannot tell up front
    const QImage image = reader.read();
    if (image.isNull()) return false;
    m_sourceSize = image.size();
  }
  return !m_sourceSize.isEmpty();
}

QSize qucs_s::EmbeddedImage::size() const
{
  if (!m_sourceSize.isValid()) return QSize();
  return (m_turns & 1) ? QSize(m_sourceSize.height(), m_sourceSize.width()) : m_sourceSize;
}

void qucs_s::EmbeddedImage::setTransform(int quarterTurns, bool flipped)
{
  m_turns = ((quarterTurns % 4) + 4) % 4;
  m_flipped = flipped;
}

// The transform is kept as "flip about the horizontal axis, then n quarter
// turns counter-clockwise"; every operation is folded back into that pair.
void qucs_s::EmbeddedImage::rotate()
{
  m_turns = (m_turns + 1) & 3;
}

void qucs_s::EmbeddedImage::mirrorX()
{
  m_flipped = !m_flipped;
  m_turns = (4 - m_turns) & 3;
}

void qucs_s::EmbeddedImage::mirrorY()
{
  m_flipped = !m_flipped;
  m_turns = (6 - m_turns) & 3;
}

QPixmap qucs_s::EmbeddedImage::render(const QSize& target) const
{
  if (isVector()) {
    QSvgRenderer renderer(m_data);
    if (!renderer.isValid()) return QPixmap();

    QPixmap pixmap(target);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(target)));
    return pixmap;
  }

  QPixmap pixmap;
  if (!pixmap.loadFromData(m_data)) return QPixmap();
  if (pixmap.size() == target) return pixmap;
  return pixmap.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QPixmap qucs_s::EmbeddedImage::pixmap(const QSize& target) const
{
  if (isNull()) return QPixmap();

  QSize wanted = target;
  if (wanted.width() < 1 || wanted.height() < 1) wanted = size();
  wanted = wanted.boundedTo(QSize(MaxRenderedPixels, MaxRenderedPixels));
  if (wanted.isEmpty()) return QPixmap();

  if (!m_cache.isNull() && m_cacheSize == wanted && m_cacheTurns == m_turns
      && m_cacheFlipped == m_flipped)
    return m_cache;

  // The turns are applied after rendering, so an odd number of them means
  // the image is rendered with its sides the other way round.
  const QSize renderSize =
      (m_turns & 1) ? QSize(wanted.height(), wanted.width()) : wanted;

  QPixmap pixmap = render(renderSize);
  if (pixmap.isNull()) return QPixmap();

  if (m_flipped || m_turns) {
    // Row vectors: a point is mapped by the flip first, then by the turns.
    QTransform transform;
    if (m_flipped) transform = QTransform(1, 0, 0, -1, 0, 0);
    for (int turn = 0; turn < m_turns; ++turn)
      transform = transform * QTransform(0, -1, 1, 0, 0, 0);
    pixmap = pixmap.transformed(transform);
  }

  m_cache = pixmap;
  m_cacheSize = wanted;
  m_cacheTurns = m_turns;
  m_cacheFlipped = m_flipped;
  return pixmap;
}
