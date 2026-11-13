// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// Paths and file access as Python's pathlib and os modules see them. Qt
// normalises macOS file names to NFC and sorts directory listings; Python
// keeps the bytes the file system returns and its order, and the published
// manifests record those names. Failures throw Py::Exception with Python's
// OSError text.
namespace Thoughts::Py
{

class Path
{
public:
    // Path("."); pathlib's empty path.
    Path() = default;
    // Path(text): drops empty and "." segments, keeps "..".
    explicit Path(const QString &text);

    // str(path), with the platform's separator.
    QString str() const;
    QString posix() const;
    // Path.parts: the anchor (if any), then each segment.
    QStringList parts() const;
    const QStringList &segments() const { return m_segments; }
    QString anchor() const { return m_drive + m_root; }
    QString name() const;
    QString suffix() const;
    QString stem() const;
    Path parent() const;
    bool isAbsolute() const;
    // path / "a/b"
    Path operator/(const QString &relative) const;
    Path operator/(const Path &relative) const;
    // path / name for one file or folder name that this program did not
    // choose. Unlike operator/, which like pathlib lets an absolute path
    // replace this one and keeps "..", it raises ValueError unless `name` is
    // a single plain segment: the result is always directly inside this path.
    Path child(const QString &name) const;
    // relative_to(base).as_posix(), or nothing where Python raises ValueError.
    std::optional<QString> relativeTo(const Path &base) const;
    bool isRelativeTo(const Path &base) const { return relativeTo(base).has_value(); }

    // Python Path equality and ordering (case-insensitive on Windows).
    bool operator==(const Path &other) const;
    bool operator!=(const Path &other) const { return !(*this == other); }
    static bool less(const Path &left, const Path &right);

private:
    QString m_drive;
    QString m_root;
    QStringList m_segments;
    QString normalizedCase() const;
};

struct FileStatus {
    enum class Kind { Missing, File, Directory, Symlink, Other };
    Kind kind = Kind::Missing;
    qint64 size = 0;
    quint64 device = 0;
    quint64 inode = 0;
    qint64 modifiedNs = 0;
    qint64 changedNs = 0;
    int error = 0;
};

// os.stat / os.lstat. kind is Missing (with error) when the call fails.
FileStatus status(const Path &path);
FileStatus linkStatus(const Path &path);
// Path.is_file(), is_dir(), is_symlink(), exists(); False on any error.
bool isFile(const Path &path);
bool isDir(const Path &path);
bool isSymlink(const Path &path);
bool exists(const Path &path);
// os.path.lexists
bool linkExists(const Path &path);

// Path.resolve(strict=...). Non-strict resolves what exists and appends the
// rest; strict raises the OSError realpath raises.
Path resolve(const Path &path, bool strict = false);
Path currentDirectory();

struct DirectoryEntry {
    QString name;
    // DirEntry.is_dir() (following symlinks; False on error).
    bool isDirectory = false;
    // DirEntry.is_dir(follow_symlinks=False)
    bool isRealDirectory = false;
};
// os.scandir order. Nothing when the directory cannot be listed.
std::optional<QList<DirectoryEntry>> scanDirectory(const Path &directory);

struct WalkStep {
    Path directory;
    QStringList subdirectories;
    QStringList files;
};
// os.walk(top, followlinks=False), top-down. The visitor may prune or reorder
// subdirectories, as callers of os.walk assign to dirnames[:].
template<typename Visitor>
void walk(const Path &top, Visitor &&visit);

// Path(root).rglob("*") in Python 3.14's order: the root's entries, then
// depth-first by a stack, listing each directory's entries as it is found.
// Linked directories are listed but not entered.
QList<Path> recursiveGlob(const Path &root);

// Path.read_bytes(); raises OSError.
QByteArray readBytes(const Path &path);
// Path.read_text(encoding="utf-8"): strict UTF-8, universal newlines.
QString readText(const Path &path);
// The source publisher's atomic_write: a 0600 temporary file in the same
// directory, fsync, then os.replace onto path.
void atomicWrite(const Path &path, const QByteArray &content);
// Path.mkdir(parents=True, exist_ok=True)
void makeDirectories(const Path &path);

// The OSError Python raises for errno (or a Windows error) on filename.
[[noreturn]] void raiseOsError(int error, const QString &filename, const QString &secondFilename = {});

// The raw file-system representation of a path and back (UTF-8 with
// surrogateescape on POSIX).
QByteArray encodeFileName(const QString &name);
QString decodeFileName(const QByteArray &name);

template<typename Visitor>
void walk(const Path &top, Visitor &&visit)
{
    QList<Path> stack{top};
    while (!stack.isEmpty()) {
        const Path directory = stack.takeLast();
        const std::optional<QList<DirectoryEntry>> entries = scanDirectory(directory);
        if (!entries) {
            continue;
        }
        WalkStep step{directory, {}, {}};
        for (const DirectoryEntry &entry : *entries) {
            (entry.isDirectory ? step.subdirectories : step.files).append(entry.name);
        }
        visit(step);
        for (auto it = step.subdirectories.crbegin(); it != step.subdirectories.crend(); ++it) {
            const Path child = directory / *it;
            if (!isSymlink(child)) {
                stack.append(child);
            }
        }
    }
}

}
