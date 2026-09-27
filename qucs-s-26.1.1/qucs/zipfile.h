/*
 * zipfile.h - ZIP archives read and written (the packages of Office's
 *             files: an .xlsx is one)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ZIPFILE_H
#define QUCS_ZIPFILE_H

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace qucs_s::zip {

/// A file of an archive: its path in it ("xl/workbook.xml") and its bytes.
struct Entry {
    QString name;
    QByteArray data;
};

/// A file (or a folder: its name ends in '/') of an archive, as its
/// directory tells: nothing inflated.
struct Item {
    QString name;
    quint16 method = 0;      ///< 0 stored, 8 deflated; any other: listed, copied, not read
    quint16 flags = 0;       ///< bit 0: encrypted
    quint32 crc = 0;
    quint32 packed = 0;      ///< its bytes in the archive
    quint32 size = 0;        ///< its bytes inflated
    QDateTime modified;      ///< to two seconds, local time (invalid: none)
    quint32 externalAttributes = 0;   ///< (a Unix mode in its high half, made by Unix)
    quint16 madeBy = 0;      ///< the version and system that made it (0: this one)
    QString comment;
    qsizetype dataOffset = -1;   ///< where its packed data starts in the archive

    bool encrypted() const { return flags & 1; }
    bool folder() const { return name.endsWith(QLatin1Char('/')); }
};

/// A file to write into an archive: \c data packed anew - or, \c copied,
/// the packed bytes of an archive's file (\c raw) as they are, its item
/// saying how (method, checksum, sizes). The item gives the name, the
/// time, the comment and the attributes either way.
struct Part {
    Item item;
    QByteArray data;
    QByteArray raw;
    bool copied = false;
};

/// The most a file of an archive may hold, and all of them together, once
/// inflated: read() refuses more (a few kilobytes of DEFLATE can say
/// gigabytes).
constexpr quint32 MaxEntrySize = 128u * 1024 * 1024;
constexpr qint64 MaxArchiveSize = 256ll * 1024 * 1024;

/// The files of a ZIP archive, in the order of its directory: stored or
/// deflated, as an archive of Office's files has them. Empty, with
/// \a error said, for what is not one (or one of ZIP64's sizes, split,
/// encrypted, or compressed otherwise), or holds more than MaxEntrySize
/// or MaxArchiveSize, or a file that inflates past the size the archive
/// gives it.
QList<Entry> read(const QByteArray& archive, QString* error = nullptr);

/// A ZIP archive of \a entries, each deflated (stored when that is not
/// smaller), their names in UTF-8, at a fixed time (the same files make
/// the same archive).
QByteArray write(const QList<Entry>& entries);
/// A ZIP archive of \a parts, in their order, with \a comment.
QByteArray write(const QList<Part>& parts, const QString& comment = QString());

/// The directory of a ZIP archive: its files and folders, nothing
/// inflated. Empty, with \a error said, for what is not one (or ZIP64).
QList<Item> list(const QByteArray& archive, QString* error = nullptr);
/// A file of \a archive (an item of list()) inflated - no further than
/// its size, refused past MaxEntrySize - its checksum checked; \a ok
/// false (and \a error said) for one encrypted, packed in a way not read,
/// or damaged.
QByteArray extract(const QByteArray& archive, const Item& item, QString* error = nullptr, bool* ok = nullptr);
/// Its packed bytes, as they are (for Part::raw).
QByteArray packedData(const QByteArray& archive, const Item& item);
/// The archive's own comment.
QString comment(const QByteArray& archive);

/// DEFLATE data (RFC 1951, as a ZIP archive has it: no zlib header)
/// inflated; \a ok false for data that is not, or that inflates past
/// \a limit bytes (-1: no limit).
QByteArray inflate(const QByteArray& deflated, bool* ok = nullptr, qsizetype limit = -1);
/// \a data deflated (raw, as inflate() reads it).
QByteArray deflate(const QByteArray& data);

/// The CRC-32 a ZIP archive checks its files by.
quint32 crc32(const QByteArray& data);

} // namespace qucs_s::zip

#endif // QUCS_ZIPFILE_H
