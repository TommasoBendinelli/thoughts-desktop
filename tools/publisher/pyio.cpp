// SPDX-License-Identifier: GPL-2.0-or-later
#include "pyio.h"

#include "pyerror.h"

#include <QStringDecoder>

#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <string>
#endif

namespace Thoughts::Py
{
namespace
{
QByteArray lineEnding()
{
#ifdef Q_OS_WIN
    return QByteArrayLiteral("\r\n");
#else
    return QByteArrayLiteral("\n");
#endif
}

void write(FILE *stream, const QByteArray &text)
{
    QByteArray bytes = text;
    bytes.replace("\n", lineEnding());
#ifdef Q_OS_WIN
    // Python writes Unicode to a console with WriteConsoleW, whatever the code page.
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        std::fflush(stream);
        const std::wstring wide = QString::fromUtf8(bytes).toStdWString();
        DWORD written = 0;
        WriteConsoleW(handle, wide.c_str(), DWORD(wide.size()), &written, nullptr);
        return;
    }
#endif
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
}

QString decodeWide(const QByteArray &bytes, QStringDecoder::Encoding encoding)
{
    QStringDecoder decoder(encoding, QStringDecoder::Flag::Stateless);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        raise(QStringLiteral("UnicodeDecodeError"), QStringLiteral("cannot decode the JSON document"));
    }
    return text;
}
}

Json loadsText(const QString &text, const LoadOptions &givenOptions)
{
    QString error;
    bool duplicate = false;
    LoadOptions options = givenOptions;
    options.duplicateKeyFound = &duplicate;
    std::optional<Json> value = loads(text, &error, options);
    if (!value) {
        // An object_pairs_hook raises a plain ValueError.
        raise(duplicate ? QStringLiteral("ValueError") : jsonDecodeErrorType, error);
    }
    return *value;
}

Json loadsBytes(const QByteArray &bytes, const LoadOptions &options)
{
    // json.detect_encoding, then bytes.decode(encoding, "surrogatepass").
    const auto at = [&bytes](qsizetype index) {
        return uchar(bytes.at(index));
    };
    QString text;
    if (bytes.startsWith(QByteArray("\x00\x00\xFE\xFF", 4)) || bytes.startsWith(QByteArray("\xFF\xFE\x00\x00", 4))) {
        // The decoder drops the byte order mark, as Python's utf-32 codec does.
        text = decodeWide(bytes, bytes.startsWith('\0') ? QStringDecoder::Utf32BE : QStringDecoder::Utf32LE);
    } else if (bytes.startsWith("\xFE\xFF") || bytes.startsWith("\xFF\xFE")) {
        text = decodeWide(bytes, bytes.startsWith("\xFE") ? QStringDecoder::Utf16BE : QStringDecoder::Utf16LE);
    } else if (bytes.size() >= 4 && at(0) == 0) {
        text = decodeWide(bytes, at(1) ? QStringDecoder::Utf16BE : QStringDecoder::Utf32BE);
    } else if (bytes.size() >= 4 && at(1) == 0) {
        text = decodeWide(bytes, at(2) || at(3) ? QStringDecoder::Utf16LE : QStringDecoder::Utf32LE);
    } else if (bytes.size() == 2 && at(0) == 0) {
        text = decodeWide(bytes, QStringDecoder::Utf16BE);
    } else if (bytes.size() == 2 && at(1) == 0) {
        text = decodeWide(bytes, QStringDecoder::Utf16LE);
    } else {
        const bool signature = bytes.startsWith("\xEF\xBB\xBF");
        QString error;
        const std::optional<QString> decoded = decodeUtf8(signature ? bytes.mid(3) : bytes, &error, true);
        if (!decoded) {
            // utf-8-sig reports positions after the signature, as 'utf-8'.
            raise(QStringLiteral("UnicodeDecodeError"), error);
        }
        text = *decoded;
    }
    return loadsText(text, options);
}

QByteArray dumpsChecked(const Json &value, const DumpOptions &options)
{
    QString error;
    std::optional<QByteArray> bytes = dumps(value, options, &error);
    if (!bytes) {
        raiseValueError(error);
    }
    return *bytes;
}

QByteArray dumpsFile(const Json &value, bool sortKeys)
{
    DumpOptions options;
    options.indent = 2;
    options.sortKeys = sortKeys;
    return dumpsChecked(value, options) + '\n';
}

void printOut(const QByteArray &line)
{
    write(stdout, line + '\n');
}

void printError(const QByteArray &line)
{
    write(stderr, line + '\n');
    std::fflush(stderr);
}

void writeOut(const QByteArray &text)
{
    write(stdout, text);
    std::fflush(stdout);
}

void writeError(const QByteArray &text)
{
    write(stderr, text);
    std::fflush(stderr);
}

void printError(const QString &line)
{
    printError(line.toUtf8());
}

void prepareConsole()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

}
