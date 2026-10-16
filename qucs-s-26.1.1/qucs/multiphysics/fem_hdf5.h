/*
 * fem_hdf5.h - a read-only view of an HDF5 file: enough of it for the
 *              project files of other tools (Lumerical's .ldev)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_HDF5_H
#define QUCS_FEM_HDF5_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cmath>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace qucs_s::fem {

/*!
 * An HDF5 file read whole: a version 2 or 3 superblock, version 2 object
 * headers and their continuations, groups' links and objects' attributes
 * compact or dense (a fractal heap's managed objects - its direct and
 * indirect blocks -, indexed by a version 2 B-tree), compact or contiguous
 * datasets of fixed-point,
 * floating-point and fixed-length string elements, unfiltered. Old-style
 * groups (symbol tables), shared messages and chunked data are not read.
 */
class Hdf5File
{
public:
    /// An attribute's or a dataset's elements: numbers (fixed or floating
    /// point) or texts (fixed-length strings), its bytes as stored.
    struct Value {
        std::vector<double> numbers;
        QStringList texts;
        QByteArray raw;
        std::vector<quint64> dims;
        bool text = false;
        double number(double otherwise = NAN) const { return numbers.empty() ? otherwise : numbers.front(); }
        QString string() const { return texts.isEmpty() ? QString() : texts.front(); }
    };

    /// Reads \a path; false (and why) when it is no HDF5 file this reads.
    bool open(const QString& path, QString* error = nullptr);
    bool openData(const QByteArray& data, QString* error = nullptr);
    quint64 root() const { return a_root; }
    /// The object at \a path ("a/b/c" from the root); 0 when there is none.
    quint64 find(const QString& path, quint64 from = 0) const;
    /// A group's hard links in their stored order: name and object.
    std::vector<std::pair<QString, quint64>> links(quint64 object) const;
    /// An object's attributes.
    std::map<QString, Value> attributes(quint64 object) const;
    /// A dataset's elements; none when it is not one this reads.
    std::optional<Value> dataset(quint64 object) const;
    /// What went wrong reading (the last).
    QString lastError() const { return a_error; }

private:
    struct Message {
        int type = 0;
        qint64 at = 0, size = 0;
    };
    quint64 u(qint64 at, int bytes) const;
    // (Not at + bytes: a damaged file's offset near 2^63 overflowed it.)
    bool fits(qint64 at, qint64 bytes) const { return at >= 0 && bytes >= 0 && at <= a_data.size() && bytes <= a_data.size() - at; }
    std::vector<Message> messages(quint64 object) const;
    std::optional<std::pair<QString, quint64>> link(qint64 at, qint64 size) const;
    std::optional<std::pair<QString, Value>> attribute(qint64 at, qint64 size) const;
    std::optional<Value> decode(qint64 type, qint64 space, qint64 data, qint64 available) const;
    // Dense storage: the fractal heap's object of an ID; a version 2
    // B-tree's records.
    QByteArray heapObject(quint64 heap, const QByteArray& id) const;
    std::vector<QByteArray> treeRecords(quint64 tree) const;
    QByteArray a_data;
    int a_O = 8, a_L = 8;
    quint64 a_root = 0;
    mutable QString a_error;
};

} // namespace qucs_s::fem

#endif // QUCS_FEM_HDF5_H
