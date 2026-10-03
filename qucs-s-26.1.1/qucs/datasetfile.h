/*
 * datasetfile.h - a dataset's file: written as text or binary, a binary
 *                 one read, and the header of a simulator's raw file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DATASETFILE_H
#define QUCS_DATASETFILE_H

#include <QByteArray>
#include <QByteArrayView>
#include <QFile>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

class QFileDevice;
class QIODevice;

/*
 * A dataset in text is "<Qucs Dataset VERSION>" and blocks of values, one a
 * line: <indep name count> ... </indep>, <dep name over ...> ... </dep>. A
 * binary one has the same blocks, their values as doubles:
 *
 *   <Qucs Dataset VERSION binary>         the first line, then zeros to
 *                                         byte 64
 *   the values                            each block's, one after the
 *                                         other: little-endian doubles, a
 *                                         complex one's real and imaginary
 *                                         parts in turn
 *   <index>                               a line a block: its header as
 *   <indep time 1001> 64 1001 r           text has it, where its values
 *   <dep tran.v(out) time> 8072 1001 c    begin, how many, real or complex
 *   </index>
 *   QDSINDEX <where> <length>             the last line: where the index
 *                                         begins and how long it is, 16
 *                                         hex digits each
 *
 * A block's values are together, so one is read without the others; the
 * numbers are the simulator's own, every digit.
 */
namespace qucs_s::datasetfile {

/// Whether the dataset at \a path is a binary one (its first line says so).
bool isBinary(const QString& path);
/// Whether a dataset beginning with \a head is a binary one.
bool isBinaryHead(QByteArrayView head);

/// A number as a dataset's text has it: as many digits as it takes to read
/// back the same double ("1.5e-03", "1.0000010000000001e+01").
QByteArray numberText(double v);
/// "+1.5e-3", "+1.5e-3+j2e-4", "-j2e-4": the real and imaginary parts of a
/// value as text has it; false when it is no number.
bool readValue(QByteArrayView text, double* re, double* im, bool* complex);

/// Where a dataset's blocks go, one after the other.
class Writer
{
public:
    virtual ~Writer() = default;
    /// A block, its header as text has it without the brackets: "indep time
    /// 1001", "dep tran.v(out) time".
    virtual void begin(const QString& header) = 0;
    virtual void real(double v) = 0;
    virtual void complex(double re, double im) = 0;
    /// A value as text has it (a sweep's parameter: "1e3").
    virtual void text(QByteArrayView value) = 0;
    virtual void end() = 0;
    /// Blocks written as text (a Monte Carlo's, an operating point's).
    virtual void blocks(const QString& text);
    /// Whether no block was written.
    virtual bool empty() const = 0;
    /// The last of it written; false and why when anything failed.
    virtual bool finish(QString* error) = 0;
};

/// A dataset in text, into \a out.
class TextWriter : public Writer
{
public:
    explicit TextWriter(QIODevice* out);
    void begin(const QString& header) override;
    void real(double v) override;
    void complex(double re, double im) override;
    void text(QByteArrayView value) override;
    void end() override;
    /// (As they are: what text has.)
    void blocks(const QString& text) override;
    bool empty() const override { return a_blocks == 0; }
    bool finish(QString* error) override;

private:
    void spill();
    QIODevice* a_out;
    QByteArray a_buffer;
    QByteArray a_close;   // "</indep>\n" or "</dep>\n" of the block open
    int a_blocks = 0;
    bool a_failed = false;
};

/// A binary dataset, into \a out (empty, open for writing; seeking in it).
class BinaryWriter : public Writer
{
public:
    explicit BinaryWriter(QFileDevice* out);
    void begin(const QString& header) override;
    void real(double v) override;
    void complex(double re, double im) override;
    void text(QByteArrayView value) override;
    void end() override;
    bool empty() const override { return a_index.isEmpty(); }
    bool finish(QString* error) override;
    /// Room for a block of \a count values, written later with writeAt() -
    /// in pieces, in any order; where they begin.
    qint64 reserve(const QString& header, qint64 count, bool complex);
    /// \a count doubles (little-endian ones made of native ones) at \a
    /// offset of the file.
    bool writeAt(qint64 offset, const double* values, qint64 count);

private:
    struct Entry {
        QString header;
        qint64 offset;
        qint64 count;
        bool complex;
    };
    QFileDevice* a_out;
    qint64 a_end = 0;   // where the next block's values go
    QList<Entry> a_index;
    // The block open: its values, held until it ends (one block's).
    QString a_header;
    bool a_open = false;
    QVector<double> a_re, a_im;
    bool a_complex = false;
    bool a_failed = false;
    QString a_error;
};

/// A block of a binary dataset, as its index has it.
struct Block {
    QString header;      // "dep tran.v(out) time"
    QStringList words;   // its words
    qint64 offset = 0;
    qint64 count = 0;
    bool complex = false;
    QString name() const { return words.value(1); }
    bool independent() const { return words.value(0) == QLatin1String("indep"); }
    QStringList dependencies() const { return independent() ? QStringList() : words.mid(2); }
};

/// A binary dataset read: its blocks, and their values when asked for. The
/// file is mapped while it is open (or read when it cannot be).
class BinaryReader
{
public:
    BinaryReader() = default;
    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;
    /// False, and why in \a error, when \a path is no binary dataset or a
    /// damaged one: an index or a block that does not fit the file.
    bool open(const QString& path, QString* error = nullptr);
    void close();
    const QList<Block>& blocks() const { return a_blocks; }
    /// The block of a name, or -1: the last of that name, as a dataset is
    /// read (dataset.h) - or the first, as a diagram's graph finds it.
    int find(const QString& name, bool first = false) const;
    /// A block's values: its real parts, and its imaginary parts when it is
    /// complex (\a im empty otherwise; it may be null).
    bool values(const Block& block, QVector<double>* re, QVector<double>* im) const;
    /// The first \a count values of a block into \a out, real and imaginary
    /// parts in turn (2 * count doubles; 0 for a real one's imaginary parts).
    bool pairs(const Block& block, double* out, qint64 count) const;

private:
    bool read(qint64 offset, void* into, qint64 bytes) const;
    mutable QFile a_file;
    const uchar* a_map = nullptr;
    qint64 a_size = 0;
    QList<Block> a_blocks;
    QHash<QString, int> a_index;   // a name's last block
    QHash<QString, int> a_first;   // and its first
};

/// A binary dataset written out as text, into \a out.
bool writeText(const QString& binaryPath, QIODevice* out, QString* error = nullptr);
/// The dependent variables' names of a binary dataset, from its index.
QStringList dependentNames(const QString& path);

/// The header of a simulator's raw file (ngspice's, Xyce's format=raw):
/// its first plot.
struct RawHeader {
    QStringList variables;   // the names, as the file has them
    bool complex = false;
    bool binary = false;
    bool dims = false;       // a variable with dims= (an XSPICE digital node, a scalar)
    qint64 points = 0;
    qint64 dataOffset = 0;   // where the values begin (after "Binary:")
    /// The bytes of one point's values.
    qint64 pointBytes() const { return qint64(variables.size()) * (complex ? 16 : 8); }
};
/// Reads the header of the raw file \a path; false when it has none.
bool readRawHeader(const QString& path, RawHeader* header);
/// Whether \a header is of a binary raw file that holds just one whole plot
/// of \a fileSize bytes, and none of dims: one a converter can take block by
/// block.
bool isPlainBinaryRaw(const RawHeader& header, qint64 fileSize);
/// The values of such a raw file into blocks of \a out reserved for them,
/// one a variable in the file's order (\a offsets; the first, the
/// independent one, real): \a bufferBytes at a time, not the whole of it in
/// memory - as many variables as that holds, else a few points.
bool transposeRaw(const QString& rawPath, const RawHeader& header, BinaryWriter& out, const QList<qint64>& offsets,
                  QString* error = nullptr, qint64 bufferBytes = qint64(64) << 20);

} // namespace qucs_s::datasetfile

#endif // QUCS_DATASETFILE_H
