// SPDX-License-Identifier: GPL-2.0-or-later
#include "pyfs.h"

#include "pyerror.h"
#include "pyjson.h"
#include "pystr.h"

#include <QHash>
#include <QRandomGenerator>

#include <cerrno>
#include <cstring>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Thoughts::Py
{
namespace
{
#ifdef Q_OS_WIN
const QChar separator = QLatin1Char('\\');
#else
const QChar separator = QLatin1Char('/');
#endif

QString errorClass(int error)
{
    switch (error) {
    case ENOENT:
        return QStringLiteral("FileNotFoundError");
    case EEXIST:
        return QStringLiteral("FileExistsError");
    case EISDIR:
        return QStringLiteral("IsADirectoryError");
    case ENOTDIR:
        return QStringLiteral("NotADirectoryError");
    case EACCES:
    case EPERM:
        return QStringLiteral("PermissionError");
    case EINTR:
        return QStringLiteral("InterruptedError");
    case ETIMEDOUT:
        return QStringLiteral("TimeoutError");
    case EAGAIN:
    case EALREADY:
    case EINPROGRESS:
        return QStringLiteral("BlockingIOError");
    case EPIPE:
        return QStringLiteral("BrokenPipeError");
    default:
        return QStringLiteral("OSError");
    }
}

#ifdef Q_OS_WIN
std::wstring wide(const QString &path)
{
    return path.toStdWString();
}

int errnoFromWindows(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    default:
        return EINVAL;
    }
}

[[noreturn]] void raiseWindowsError(DWORD error, const QString &filename, const QString &secondFilename = {})
{
    wchar_t *buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    QString text = buffer ? QString::fromWCharArray(buffer).trimmed() : QString();
    LocalFree(buffer);
    QString message = QStringLiteral("[WinError %1] %2: %3").arg(error).arg(text, repr(filename));
    if (!secondFilename.isNull()) {
        message += QStringLiteral(" -> ") + repr(secondFilename);
    }
    raise(errorClass(errnoFromWindows(error)), message);
}

FileStatus windowsStatus(const Path &path, bool follow)
{
    FileStatus result;
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (!follow) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    HANDLE handle = CreateFileW(wide(path.str()).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = errnoFromWindows(GetLastError());
        return result;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool known = GetFileInformationByHandle(handle, &information);
    const bool tagged = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag));
    CloseHandle(handle);
    if (!known) {
        result.error = EINVAL;
        return result;
    }
    if (!follow && tagged && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && tag.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        result.kind = FileStatus::Kind::Symlink;
    } else if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        result.kind = FileStatus::Kind::Directory;
    } else {
        result.kind = FileStatus::Kind::File;
    }
    result.size = (qint64(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    result.device = information.dwVolumeSerialNumber;
    result.inode = (quint64(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
    const auto nanoseconds = [](FILETIME time) {
        const qint64 ticks = (qint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        return (ticks - 116444736000000000LL) * 100;
    };
    result.modifiedNs = nanoseconds(information.ftLastWriteTime);
    result.changedNs = nanoseconds(information.ftCreationTime);
    return result;
}

QString finalPath(const QString &path, DWORD *error)
{
    HANDLE handle = CreateFileW(wide(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return {};
    }
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), 0);
    if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), 0);
    }
    const DWORD failure = length == 0 ? GetLastError() : 0;
    CloseHandle(handle);
    if (length == 0) {
        *error = failure;
        return {};
    }
    *error = 0;
    return QString::fromWCharArray(buffer.data(), length);
}

QString stripVerbatimPrefix(const QString &path)
{
    const QString prefix = QStringLiteral("\\\\?\\");
    const QString uncPrefix = QStringLiteral("\\\\?\\UNC\\");
    QString stripped;
    if (path.startsWith(uncPrefix)) {
        stripped = QStringLiteral("\\\\") + path.mid(uncPrefix.size());
    } else if (path.startsWith(prefix)) {
        stripped = path.mid(prefix.size());
    } else {
        return path;
    }
    DWORD error = 0;
    return finalPath(stripped, &error) == path ? stripped : path;
}
#else
qint64 timespecNs(const struct timespec &time)
{
    return qint64(time.tv_sec) * 1000000000LL + time.tv_nsec;
}

FileStatus posixStatus(const Path &path, bool follow)
{
    FileStatus result;
    struct stat information;
    const QByteArray name = encodeFileName(path.str());
    if ((follow ? ::stat(name.constData(), &information) : ::lstat(name.constData(), &information)) != 0) {
        result.error = errno;
        return result;
    }
    if (S_ISLNK(information.st_mode)) {
        result.kind = FileStatus::Kind::Symlink;
    } else if (S_ISDIR(information.st_mode)) {
        result.kind = FileStatus::Kind::Directory;
    } else if (S_ISREG(information.st_mode)) {
        result.kind = FileStatus::Kind::File;
    } else {
        result.kind = FileStatus::Kind::Other;
    }
    result.size = information.st_size;
    result.device = quint64(information.st_dev);
    result.inode = quint64(information.st_ino);
#ifdef Q_OS_DARWIN
    result.modifiedNs = timespecNs(information.st_mtimespec);
    result.changedNs = timespecNs(information.st_ctimespec);
#else
    result.modifiedNs = timespecNs(information.st_mtim);
    result.changedNs = timespecNs(information.st_ctim);
#endif
    return result;
}

// posixpath.realpath (Python 3.14) on decoded names.
QString realPath(const QString &filename, bool strict)
{
    const QString sep = QStringLiteral("/");
    QList<std::optional<QString>> rest;
    const QStringList split = filename.split(sep);
    for (auto it = split.crbegin(); it != split.crend(); ++it) {
        rest.append(*it);
    }
    qsizetype partCount = rest.size();
    QString path = filename.startsWith(sep) ? sep : currentDirectory().str();
    QHash<QString, std::optional<QString>> seen;
    while (partCount) {
        std::optional<QString> name = rest.takeLast();
        if (!name) {
            seen[*rest.takeLast()] = path;
            continue;
        }
        --partCount;
        if (name->isEmpty() || *name == QLatin1String(".")) {
            continue;
        }
        if (*name == QLatin1String("..")) {
            path = path.left(path.lastIndexOf(sep));
            if (path.isEmpty()) {
                path = sep;
            }
            continue;
        }
        const QString newPath = path == sep ? QString(path + *name) : QString(path + sep + *name);
        const QByteArray encoded = encodeFileName(newPath);
        struct stat information;
        if (::lstat(encoded.constData(), &information) != 0) {
            if (strict) {
                raiseOsError(errno, newPath);
            }
            path = newPath;
            continue;
        }
        if (!S_ISLNK(information.st_mode)) {
            if (strict && partCount && !S_ISDIR(information.st_mode)) {
                raiseOsError(ENOTDIR, newPath);
            }
            path = newPath;
            continue;
        }
        if (seen.contains(newPath)) {
            const std::optional<QString> known = seen.value(newPath);
            if (known) {
                path = *known;
                continue;
            }
            if (strict) {
                raiseOsError(ELOOP, newPath);
            }
            path = newPath;
            continue;
        }
        QByteArray target(PATH_MAX + 1, Qt::Uninitialized);
        const ssize_t length = ::readlink(encoded.constData(), target.data(), target.size());
        if (length < 0) {
            if (strict) {
                raiseOsError(errno, newPath);
            }
            path = newPath;
            continue;
        }
        target.truncate(length);
        const QString link = decodeFileName(target);
        if (link.startsWith(sep)) {
            path = sep;
        }
        seen.insert(newPath, std::nullopt);
        rest.append(newPath);
        rest.append(std::nullopt);
        const QStringList targetParts = link.split(sep);
        for (auto it = targetParts.crbegin(); it != targetParts.crend(); ++it) {
            rest.append(*it);
        }
        partCount += targetParts.size();
    }
    return path;
}
#endif

QString randomTemporaryName()
{
    static const char characters[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
    QString result;
    for (int index = 0; index < 8; ++index) {
        result += QLatin1Char(characters[QRandomGenerator::global()->bounded(int(sizeof(characters) - 1))]);
    }
    return result;
}
}

Path::Path(const QString &input)
{
#ifdef Q_OS_WIN
    QString text = input;
    text.replace(QLatin1Char('/'), separator);
    qsizetype position = 0;
    if (text.startsWith(QStringLiteral("\\\\"))) {
        // \\server\share is the drive of a UNC path.
        const qsizetype server = text.indexOf(separator, 2);
        const qsizetype share = server < 0 ? -1 : text.indexOf(separator, server + 1);
        position = server < 0 ? text.size() : (share < 0 ? text.size() : share);
        m_drive = text.left(position);
    } else if (text.size() >= 2 && text.at(1) == QLatin1Char(':')) {
        m_drive = text.left(2);
        position = 2;
    }
    if (position < text.size() && text.at(position) == separator) {
        m_root = separator;
    }
    const QString tail = text.mid(position);
#else
    const QString &text = input;
    if (text.startsWith(QLatin1Char('/'))) {
        m_root = text.startsWith(QStringLiteral("//")) && !text.startsWith(QStringLiteral("///")) ? QStringLiteral("//") : QStringLiteral("/");
    }
    const QString &tail = text;
#endif
    for (const QString &segment : tail.split(separator)) {
        if (!segment.isEmpty() && segment != QLatin1String(".")) {
            m_segments.append(segment);
        }
    }
}

QString Path::str() const
{
    const QString result = m_drive + m_root + m_segments.join(separator);
    return result.isEmpty() ? QStringLiteral(".") : result;
}

QString Path::posix() const
{
#ifdef Q_OS_WIN
    QString result = str();
    result.replace(separator, QLatin1Char('/'));
    return result;
#else
    return str();
#endif
}

QStringList Path::parts() const
{
    QStringList result;
    if (!anchor().isEmpty()) {
        result.append(anchor());
    }
    result.append(m_segments);
    return result;
}

QString Path::name() const
{
    return m_segments.isEmpty() ? QString() : m_segments.last();
}

QString Path::suffix() const
{
    return nameSuffix(name());
}

QString Path::stem() const
{
    return nameStem(name());
}

Path Path::parent() const
{
    Path result = *this;
    if (!result.m_segments.isEmpty()) {
        result.m_segments.removeLast();
    }
    return result;
}

bool Path::isAbsolute() const
{
#ifdef Q_OS_WIN
    return m_drive.startsWith(QStringLiteral("\\\\")) || (!m_drive.isEmpty() && !m_root.isEmpty());
#else
    return !m_root.isEmpty();
#endif
}

Path Path::operator/(const QString &relative) const
{
    return *this / Path(relative);
}

Path Path::operator/(const Path &relative) const
{
    if (!relative.m_drive.isEmpty()) {
        if (relative.isAbsolute() || !m_root.isEmpty() || Py::lower(relative.m_drive) != Py::lower(m_drive)) {
            return relative;
        }
    } else if (!relative.m_root.isEmpty()) {
        Path result = relative;
        result.m_drive = m_drive;
        return result;
    }
    Path result = *this;
    result.m_segments.append(relative.m_segments);
    return result;
}

Path Path::child(const QString &name) const
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1Char(':')) || name.contains(QChar(0))) {
        raiseValueError(QStringLiteral("not a single file name: ") + name.left(80));
    }
    Path result = *this;
    result.m_segments.append(name);
    return result;
}

QString Path::normalizedCase() const
{
#ifdef Q_OS_WIN
    return Py::lower(str());
#else
    return str();
#endif
}

std::optional<QString> Path::relativeTo(const Path &base) const
{
#ifdef Q_OS_WIN
    const auto same = [](const QString &left, const QString &right) {
        return Py::lower(left) == Py::lower(right);
    };
#else
    const auto same = [](const QString &left, const QString &right) {
        return left == right;
    };
#endif
    if (!same(anchor(), base.anchor()) || base.m_segments.size() > m_segments.size()) {
        return std::nullopt;
    }
    for (qsizetype index = 0; index < base.m_segments.size(); ++index) {
        if (!same(m_segments.at(index), base.m_segments.at(index))) {
            return std::nullopt;
        }
    }
    const QStringList rest = m_segments.mid(base.m_segments.size());
    return rest.isEmpty() ? QStringLiteral(".") : rest.join(QLatin1Char('/'));
}

bool Path::operator==(const Path &other) const
{
    return normalizedCase() == other.normalizedCase();
}

bool Path::less(const Path &left, const Path &right)
{
    const QStringList leftParts = left.normalizedCase().split(separator);
    const QStringList rightParts = right.normalizedCase().split(separator);
    for (qsizetype index = 0; index < leftParts.size() && index < rightParts.size(); ++index) {
        if (leftParts.at(index) != rightParts.at(index)) {
            return codePointLess(leftParts.at(index), rightParts.at(index));
        }
    }
    return leftParts.size() < rightParts.size();
}

namespace
{
// os functions raise ValueError for these; Path.is_file() and friends answer False.
bool representable(const Path &path)
{
    const QString text = path.str();
    if (text.contains(QChar(0))) {
        return false;
    }
#ifndef Q_OS_WIN
    for (const char32_t codePoint : toCodePoints(text)) {
        if (codePoint >= 0xD800 && codePoint <= 0xDFFF && !(codePoint >= 0xDC80 && codePoint <= 0xDCFF)) {
            return false;
        }
    }
#endif
    return true;
}

void requireRepresentable(const Path &path)
{
    if (path.str().contains(QChar(0))) {
        raiseValueError(QStringLiteral("embedded null byte"));
    }
    if (!representable(path)) {
        raise(QStringLiteral("UnicodeEncodeError"), QStringLiteral("'utf-8' codec can't encode character: surrogates not allowed"));
    }
}
}

FileStatus status(const Path &path)
{
    if (!representable(path)) {
        return FileStatus{FileStatus::Kind::Missing, 0, 0, 0, 0, 0, EINVAL};
    }
#ifdef Q_OS_WIN
    return windowsStatus(path, true);
#else
    return posixStatus(path, true);
#endif
}

FileStatus linkStatus(const Path &path)
{
    if (!representable(path)) {
        return FileStatus{FileStatus::Kind::Missing, 0, 0, 0, 0, 0, EINVAL};
    }
#ifdef Q_OS_WIN
    return windowsStatus(path, false);
#else
    return posixStatus(path, false);
#endif
}

bool isFile(const Path &path)
{
    return status(path).kind == FileStatus::Kind::File;
}

bool isDir(const Path &path)
{
    return status(path).kind == FileStatus::Kind::Directory;
}

bool isSymlink(const Path &path)
{
    return linkStatus(path).kind == FileStatus::Kind::Symlink;
}

bool exists(const Path &path)
{
    return status(path).kind != FileStatus::Kind::Missing;
}

bool linkExists(const Path &path)
{
    return linkStatus(path).kind != FileStatus::Kind::Missing;
}

Path currentDirectory()
{
#ifdef Q_OS_WIN
    const DWORD length = GetCurrentDirectoryW(0, nullptr);
    std::wstring buffer(length, L'\0');
    const DWORD written = GetCurrentDirectoryW(length, buffer.data());
    return Path(QString::fromWCharArray(buffer.data(), written));
#else
    QByteArray buffer(PATH_MAX + 1, Qt::Uninitialized);
    if (!::getcwd(buffer.data(), buffer.size())) {
        raiseOsError(errno, {});
    }
    return Path(decodeFileName(QByteArray(buffer.constData())));
#endif
}

Path resolve(const Path &path, bool strict)
{
#ifdef Q_OS_WIN
    const Path absolute = path.isAbsolute() ? path : currentDirectory() / path;
    // normpath: ".." removes the previous segment.
    QStringList segments;
    for (const QString &segment : absolute.segments()) {
        if (segment == QLatin1String("..")) {
            if (!segments.isEmpty()) {
                segments.removeLast();
            }
        } else {
            segments.append(segment);
        }
    }
    const QString anchorText = absolute.anchor();
    DWORD error = 0;
    QString resolved = finalPath(anchorText + segments.join(QLatin1Char('\\')), &error);
    if (!resolved.isNull()) {
        return Path(stripVerbatimPrefix(resolved));
    }
    if (strict) {
        raiseWindowsError(error, Path(anchorText + segments.join(QLatin1Char('\\'))).str());
    }
    QStringList tail;
    while (!segments.isEmpty()) {
        tail.prepend(segments.takeLast());
        resolved = finalPath(anchorText + segments.join(QLatin1Char('\\')), &error);
        if (!resolved.isNull()) {
            return Path(stripVerbatimPrefix(resolved)) / tail.join(QLatin1Char('\\'));
        }
    }
    return Path(anchorText) / tail.join(QLatin1Char('\\'));
#else
    return Path(realPath(path.str(), strict));
#endif
}

std::optional<QList<DirectoryEntry>> scanDirectory(const Path &directory)
{
    QList<DirectoryEntry> entries;
#ifdef Q_OS_WIN
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(wide(directory.str() + QStringLiteral("\\*")).c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    do {
        const QString name = QString::fromWCharArray(data.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String("..")) {
            continue;
        }
        DirectoryEntry entry{name, false, false};
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && data.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
            entry.isDirectory = isDir(directory / name);
        } else {
            entry.isDirectory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            entry.isRealDirectory = entry.isDirectory;
        }
        entries.append(entry);
    } while (FindNextFileW(handle, &data));
    const DWORD error = GetLastError();
    FindClose(handle);
    if (error != ERROR_NO_MORE_FILES) {
        return std::nullopt;
    }
#else
    DIR *stream = ::opendir(encodeFileName(directory.str()).constData());
    if (!stream) {
        return std::nullopt;
    }
    errno = 0;
    while (const struct dirent *raw = ::readdir(stream)) {
        const QByteArray name(raw->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        DirectoryEntry entry{decodeFileName(name), false, false};
        if (raw->d_type == DT_UNKNOWN || raw->d_type == DT_LNK) {
            entry.isDirectory = isDir(directory / entry.name);
            entry.isRealDirectory = raw->d_type == DT_UNKNOWN && linkStatus(directory / entry.name).kind == FileStatus::Kind::Directory;
        } else {
            entry.isDirectory = raw->d_type == DT_DIR;
            entry.isRealDirectory = entry.isDirectory;
        }
        entries.append(entry);
        errno = 0;
    }
    const int error = errno;
    ::closedir(stream);
    if (error != 0) {
        return std::nullopt;
    }
#endif
    return entries;
}

QList<Path> recursiveGlob(const Path &root)
{
    QList<Path> results;
    if (const std::optional<QList<DirectoryEntry>> entries = scanDirectory(root)) {
        for (const DirectoryEntry &entry : *entries) {
            results.append(root / entry.name);
        }
    }
    QList<Path> stack{root};
    while (!stack.isEmpty()) {
        const Path directory = stack.takeLast();
        const std::optional<QList<DirectoryEntry>> entries = scanDirectory(directory);
        if (!entries) {
            continue;
        }
        for (const DirectoryEntry &entry : *entries) {
            if (!entry.isRealDirectory) {
                continue;
            }
            const Path child = directory / entry.name;
            if (const std::optional<QList<DirectoryEntry>> children = scanDirectory(child)) {
                for (const DirectoryEntry &grandchild : *children) {
                    results.append(child / grandchild.name);
                }
            }
            stack.append(child);
        }
    }
    return results;
}

QByteArray readBytes(const Path &path)
{
    requireRepresentable(path);
#ifdef Q_OS_WIN
    const int descriptor = _wopen(wide(path.str()).c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    const int descriptor = ::open(encodeFileName(path.str()).constData(), O_RDONLY | O_CLOEXEC);
#endif
    if (descriptor < 0) {
        raiseOsError(errno, path.str());
    }
    QByteArray content;
#ifdef Q_OS_WIN
    char buffer[65536];
    for (;;) {
        const int count = _read(descriptor, buffer, sizeof(buffer));
        if (count < 0) {
            const int error = errno;
            _close(descriptor);
            raiseOsError(error, path.str());
        }
        if (count == 0) {
            break;
        }
        content.append(buffer, count);
    }
    _close(descriptor);
#else
    struct stat information;
    if (::fstat(descriptor, &information) == 0 && S_ISDIR(information.st_mode)) {
        ::close(descriptor);
        raiseOsError(EISDIR, path.str());
    }
    // Read straight into the result, sized from fstat and grown if the file
    // turns out longer.
    qsizetype used = 0;
    content.resize(qMax<qsizetype>(qsizetype(information.st_size) + 1, 4096));
    for (;;) {
        if (used == content.size()) {
            content.resize(content.size() * 2);
        }
        const ssize_t count = ::read(descriptor, content.data() + used, size_t(content.size() - used));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            ::close(descriptor);
            raiseOsError(error, path.str());
        }
        if (count == 0) {
            break;
        }
        used += count;
    }
    content.truncate(used);
    ::close(descriptor);
#endif
    return content;
}

QString readText(const Path &path)
{
    QString error;
    const std::optional<QString> text = decodeUtf8(readBytes(path), &error);
    if (!text) {
        raise(QStringLiteral("UnicodeDecodeError"), error);
    }
    return universalNewlines(*text);
}

void makeDirectories(const Path &path)
{
#ifdef Q_OS_WIN
    const auto make = [](const Path &directory) -> int {
        if (CreateDirectoryW(wide(directory.str()).c_str(), nullptr)) {
            return 0;
        }
        return errnoFromWindows(GetLastError());
    };
#else
    const auto make = [](const Path &directory) -> int {
        return ::mkdir(encodeFileName(directory.str()).constData(), 0777) == 0 ? 0 : errno;
    };
#endif
    const int error = make(path);
    if (error == 0) {
        return;
    }
    if (error == ENOENT) {
        if (path.parent() == path) {
            raiseOsError(error, path.str());
        }
        makeDirectories(path.parent());
        const int retry = make(path);
        if (retry == 0 || isDir(path)) {
            return;
        }
        raiseOsError(retry, path.str());
    }
    if (!isDir(path)) {
        raiseOsError(error, path.str());
    }
}

void atomicWrite(const Path &path, const QByteArray &content)
{
    makeDirectories(path.parent());
    // mkstemp names the file in os.path.abspath(dir).
    const Path directory = path.parent().isAbsolute() ? path.parent() : currentDirectory() / path.parent();
    const QString prefix = QLatin1Char('.') + path.name() + QLatin1Char('.');
    Path temporary;
    int descriptor = -1;
    for (int attempt = 0; attempt < 10000 && descriptor < 0; ++attempt) {
        temporary = directory / (prefix + randomTemporaryName());
#ifdef Q_OS_WIN
        descriptor = _wopen(wide(temporary.str()).c_str(), _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
        descriptor = ::open(encodeFileName(temporary.str()).constData(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
        if (descriptor < 0 && errno != EEXIST) {
            raiseOsError(errno, temporary.str());
        }
    }
    if (descriptor < 0) {
        raiseOsError(EEXIST, temporary.str());
    }
    const auto removeTemporary = [&temporary] {
#ifdef Q_OS_WIN
        DeleteFileW(wide(temporary.str()).c_str());
#else
        ::unlink(encodeFileName(temporary.str()).constData());
#endif
    };
    qsizetype written = 0;
    while (written < content.size()) {
#ifdef Q_OS_WIN
        const int count = _write(descriptor, content.constData() + written, unsigned(qMin<qsizetype>(content.size() - written, 1 << 30)));
#else
        const ssize_t count = ::write(descriptor, content.constData() + written, size_t(content.size() - written));
        if (count < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (count < 0) {
            const int error = errno;
#ifdef Q_OS_WIN
            _close(descriptor);
#else
            ::close(descriptor);
#endif
            removeTemporary();
            raiseOsError(error, {});
        }
        written += count;
    }
#ifdef Q_OS_WIN
    const bool synced = _commit(descriptor) == 0;
    const int syncError = errno;
    _close(descriptor);
#else
    const bool synced = ::fsync(descriptor) == 0;
    const int syncError = errno;
    ::close(descriptor);
#endif
    if (!synced) {
        removeTemporary();
        raiseOsError(syncError, {});
    }
#ifdef Q_OS_WIN
    if (!MoveFileExW(wide(temporary.str()).c_str(), wide(path.str()).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD error = GetLastError();
        removeTemporary();
        raiseWindowsError(error, temporary.str(), path.str());
    }
#else
    if (::rename(encodeFileName(temporary.str()).constData(), encodeFileName(path.str()).constData()) != 0) {
        const int error = errno;
        removeTemporary();
        raiseOsError(error, temporary.str(), path.str());
    }
#endif
}

void raiseOsError(int error, const QString &filename, const QString &secondFilename)
{
    QString message = QStringLiteral("[Errno %1] %2").arg(error).arg(QString::fromLocal8Bit(std::strerror(error)));
    if (!filename.isNull()) {
        message += QStringLiteral(": ") + repr(filename);
        if (!secondFilename.isNull()) {
            message += QStringLiteral(" -> ") + repr(secondFilename);
        }
    }
    raise(errorClass(error), message);
}

QByteArray encodeFileName(const QString &name)
{
#ifdef Q_OS_WIN
    return name.toUtf8();
#else
    // Only a lone U+DC80..U+DCFF (an undecodable byte) needs more than UTF-8.
    bool escaped = false;
    for (qsizetype index = 0; index < name.size() && !escaped; ++index) {
        const char16_t unit = name.at(index).unicode();
        if (unit >= 0xDC80 && unit <= 0xDCFF && !(index > 0 && QChar::isHighSurrogate(name.at(index - 1).unicode()))) {
            escaped = true;
        }
    }
    if (!escaped) {
        return name.toUtf8();
    }
    QByteArray result;
    for (const char32_t codePoint : toCodePoints(name)) {
        if (codePoint >= 0xDC80 && codePoint <= 0xDCFF) {
            result.append(char(codePoint - 0xDC00));
        } else {
            result.append(fromCodePoints(std::u32string(1, codePoint)).toUtf8());
        }
    }
    return result;
#endif
}

QString decodeFileName(const QByteArray &name)
{
#ifdef Q_OS_WIN
    return QString::fromUtf8(name);
#else
    bool ascii = true;
    for (const char byte : name) {
        ascii = ascii && uchar(byte) < 0x80;
    }
    if (ascii) {
        return QString::fromLatin1(name);
    }
    // UTF-8 with surrogateescape: each byte of an invalid sequence becomes
    // U+DC80..U+DCFF.
    std::u32string result;
    const auto byte = [&name](qsizetype index) {
        return uchar(name.at(index));
    };
    qsizetype index = 0;
    while (index < name.size()) {
        const uchar lead = byte(index);
        if (lead < 0x80) {
            result.push_back(lead);
            ++index;
            continue;
        }
        int length = 0;
        char32_t codePoint = 0;
        char32_t minimum = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            codePoint = lead & 0x1F;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            codePoint = lead & 0x0F;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            codePoint = lead & 0x07;
            minimum = 0x10000;
        }
        bool valid = length > 0 && index + length <= name.size();
        for (int offset = 1; valid && offset < length; ++offset) {
            const uchar next = byte(index + offset);
            valid = (next & 0xC0) == 0x80;
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        if (valid && (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))) {
            valid = false;
        }
        if (valid) {
            result.push_back(codePoint);
            index += length;
        } else {
            result.push_back(0xDC00 + lead);
            ++index;
        }
    }
    return fromCodePoints(result);
#endif
}

}
