/*
 * How a text file's bytes are characters, found when it is read and kept
 * for when it is written back: the text editor's documents, CSV files.
 *
 * A file is read as its byte order mark says (UTF-8, UTF-16, UTF-32);
 * without one as UTF-16 when every other byte of its start is NUL, as
 * UTF-8 when it is valid UTF-8, and otherwise - bytes that are not of the
 * encoding they seemed to be in, too - as Windows-1252: the ANSI
 * code page of Western Windows (vendor model libraries, old netlists,
 * CSV files of Windows programs), Latin-1 with a few more printable
 * characters. In Windows-1252 every byte is one character and back, so a
 * file read that way is written back byte for byte as it was, but for
 * what was edited.
 */
#ifndef QUCS_TEXTCODEC_H
#define QUCS_TEXTCODEC_H

#include <QByteArray>
#include <QString>

namespace qucs_s::textcodec {

struct Encoding {
    enum class Kind { Utf8, Utf16LE, Utf16BE, Utf32LE, Utf32BE, Windows1252 };
    Kind kind = Kind::Utf8;
    bool bom = false;   ///< it starts with a byte order mark (written back)
    /// "UTF-8", "UTF-16 LE", ..., "Windows-1252".
    QString name() const;
    bool operator==(const Encoding&) const = default;
};

/// \a bytes as text, and in \a found how they were (the file header says
/// which way). A byte order mark is not part of the text.
QString decode(const QByteArray& bytes, Encoding* found = nullptr);

/// \a text as bytes in \a encoding, its byte order mark first when it has
/// one. False when a character has no bytes in \a encoding (only
/// Windows-1252 lacks any: it has "°", "µ" and "€", not "Ω"): \a bytes is
/// then left empty and \a missing, when given, set to the first such
/// character.
bool encode(const QString& text, const Encoding& encoding, QByteArray* bytes, QString* missing = nullptr);

} // namespace qucs_s::textcodec

#endif
