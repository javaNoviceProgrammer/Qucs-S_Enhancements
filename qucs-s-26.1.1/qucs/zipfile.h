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
#include <QList>
#include <QString>

namespace qucs_s::zip {

/// A file of an archive: its path in it ("xl/workbook.xml") and its bytes.
struct Entry {
    QString name;
    QByteArray data;
};

/// The files of a ZIP archive, in the order of its directory: stored or
/// deflated, as an archive of Office's files has them. Empty, with
/// \a error said, for what is not one (or one of ZIP64's sizes, split,
/// encrypted, or compressed otherwise).
QList<Entry> read(const QByteArray& archive, QString* error = nullptr);

/// A ZIP archive of \a entries, each deflated (stored when that is not
/// smaller), their names in UTF-8.
QByteArray write(const QList<Entry>& entries);

/// DEFLATE data (RFC 1951, as a ZIP archive has it: no zlib header)
/// inflated; \a ok false for data that is not.
QByteArray inflate(const QByteArray& deflated, bool* ok = nullptr);
/// \a data deflated (raw, as inflate() reads it).
QByteArray deflate(const QByteArray& data);

/// The CRC-32 a ZIP archive checks its files by.
quint32 crc32(const QByteArray& data);

} // namespace qucs_s::zip

#endif // QUCS_ZIPFILE_H
