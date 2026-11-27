/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../part/fileidentitycache.h"
#include "../part/thoughtignore.h"

#include <QDir>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/time.h>
#endif

namespace
{
bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

bool replaceFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

// A file's access and modification times at full resolution.
struct FileTimes {
#ifdef Q_OS_WIN
    FILETIME access {};
    FILETIME modification {};
#else
    struct timeval access {};
    struct timeval modification {};
#endif
};

#ifdef Q_OS_WIN
HANDLE openTimes(const QString &path, DWORD access)
{
    return CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()), access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
#endif

bool readFileTimes(const QString &path, FileTimes &times)
{
#ifdef Q_OS_WIN
    const HANDLE handle = openTimes(path, FILE_READ_ATTRIBUTES);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool ok = GetFileTime(handle, nullptr, &times.access, &times.modification);
    CloseHandle(handle);
    return ok;
#else
    struct stat state {};
    if (::stat(QFile::encodeName(path).constData(), &state) != 0) {
        return false;
    }
#ifdef Q_OS_MACOS
    const auto accessed = state.st_atimespec;
    const auto modified = state.st_mtimespec;
#else
    const auto accessed = state.st_atim;
    const auto modified = state.st_mtim;
#endif
    times.access = {accessed.tv_sec, decltype(times.access.tv_usec)(accessed.tv_nsec / 1000)};
    times.modification = {modified.tv_sec, decltype(times.modification.tv_usec)(modified.tv_nsec / 1000)};
    return true;
#endif
}

// Sets the modification time back to what it was, as a copy that preserves it does.
bool restoreModificationTime(const QString &path, const FileTimes &before)
{
#ifdef Q_OS_WIN
    const HANDLE handle = openTimes(path, FILE_WRITE_ATTRIBUTES);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool ok = SetFileTime(handle, nullptr, &before.access, &before.modification);
    CloseHandle(handle);
    return ok;
#else
    const struct timeval times[2] = {before.access, before.modification};
    return ::utimes(QFile::encodeName(path).constData(), times) == 0;
#endif
}
}

class FileIdentityCacheTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void testUnchangedFileIsReadOnce();
    void testChangedFileIsReadAgain();
    void testMissingFileIsNotKept();
    void testFileChangedWhileReadIsNotKept();
    void testIgnorePolicyIsReadOncePerChange();
};

void FileIdentityCacheTest::init()
{
    Thoughts::clearFileIdentityCaches();
}

void FileIdentityCacheTest::testUnchangedFileIsReadOnce()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("index.json"));
    QVERIFY(writeFile(path, R"({"value": 1})"));
    const Thoughts::FileReadCounts before = Thoughts::fileReadCounts();
    QCOMPARE(Thoughts::cachedJsonFile(path).object().value(QStringLiteral("value")).toInt(), 1);
    for (int repeat = 0; repeat < 5; ++repeat) {
        QCOMPARE(Thoughts::cachedJsonFile(path).object().value(QStringLiteral("value")).toInt(), 1);
    }
    const Thoughts::FileReadCounts reads = Thoughts::fileReadCounts() - before;
    QCOMPARE(reads.reads, 1);
    QCOMPARE(reads.hits, 5);
    // The bytes cache is separate and behaves the same.
    QCOMPARE(Thoughts::cachedFileBytes(path).value_or(QByteArray()), QByteArray(R"({"value": 1})"));
    QCOMPARE(Thoughts::cachedFileBytes(path).value_or(QByteArray()), QByteArray(R"({"value": 1})"));
    QCOMPARE((Thoughts::fileReadCounts() - before).reads, 2);
}

void FileIdentityCacheTest::testChangedFileIsReadAgain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("index.json"));
    QVERIFY(writeFile(path, R"({"value": 1})"));
    QCOMPARE(Thoughts::cachedJsonFile(path).object().value(QStringLiteral("value")).toInt(), 1);

    // Edited in place to the same size, its modification time put back: the
    // inode stays, the status-change time does not.
    FileTimes before;
    QVERIFY(readFileTimes(path, before));
    QVERIFY(writeFile(path, R"({"value": 2})"));
    QVERIFY(restoreModificationTime(path, before));
    QCOMPARE(Thoughts::cachedJsonFile(path).object().value(QStringLiteral("value")).toInt(), 2);

    // Replaced atomically, as QSaveFile and Sync write.
    QVERIFY(replaceFile(path, R"({"value": 3})"));
    QCOMPARE(Thoughts::cachedJsonFile(path).object().value(QStringLiteral("value")).toInt(), 3);

    // Damaged: the parse error is what is kept, until it is fixed.
    QVERIFY(replaceFile(path, R"({"value": )"));
    QVERIFY(!Thoughts::cachedJsonFile(path).valid());
    QVERIFY(Thoughts::cachedJsonFile(path).opened);
    QVERIFY(replaceFile(path, R"({"value": 4})"));
    QVERIFY(Thoughts::cachedJsonFile(path).valid());
    QCOMPARE(Thoughts::cachedJsonFile(path).object().value(QStringLiteral("value")).toInt(), 4);
}

void FileIdentityCacheTest::testMissingFileIsNotKept()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("later.json"));
    const Thoughts::FileReadCounts before = Thoughts::fileReadCounts();
    QVERIFY(!Thoughts::cachedJsonFile(path).opened);
    QVERIFY(!Thoughts::cachedFileBytes(path));
    // Nothing was there to read.
    QCOMPARE((Thoughts::fileReadCounts() - before).reads, 0);
    QVERIFY(writeFile(path, R"({"value": 5})"));
    QCOMPARE(Thoughts::cachedJsonFile(path).object().value(QStringLiteral("value")).toInt(), 5);
    QVERIFY(QFile::remove(path));
    QVERIFY(!Thoughts::cachedJsonFile(path).opened);
}

void FileIdentityCacheTest::testFileChangedWhileReadIsNotKept()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("busy.txt"));
    QVERIFY(writeFile(path, "first"));
    Thoughts::FileIdentityCache<QByteArray> cache(16);
    int reads = 0;
    const auto read = [&reads](const QString &file) {
        ++reads;
        QFile input(file);
        const QByteArray contents = input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray();
        if (reads == 1) {
            // Another writer, while this one reads.
            writeFile(file, "second");
        }
        return contents;
    };
    QCOMPARE(cache.value(path, read), QByteArray("first"));
    // Not kept: read again, and kept from then on.
    QCOMPARE(cache.value(path, read), QByteArray("second"));
    QCOMPARE(cache.value(path, read), QByteArray("second"));
    QCOMPARE(reads, 2);
}

void FileIdentityCacheTest::testIgnorePolicyIsReadOncePerChange()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = root.filePath(QStringLiteral("docs-sample"));
    QVERIFY(QDir().mkpath(folder));
    QVERIFY(writeFile(folder + QStringLiteral("/.ThoughtIgnore"), "build/\n*.log\n"));
    const Thoughts::FileReadCounts before = Thoughts::fileReadCounts();
    for (int repeat = 0; repeat < 10; ++repeat) {
        const Thoughts::ThoughtIgnore ignore(folder);
        QVERIFY(ignore.error().isEmpty());
        QVERIFY(ignore.excludes(QStringLiteral("notes.log")));
        QVERIFY(ignore.excludes(QStringLiteral("build"), true));
        QVERIFY(!ignore.excludes(QStringLiteral("notes.tex")));
    }
    QCOMPARE((Thoughts::fileReadCounts() - before).reads, 1);
    // A new rule applies at once.
    QVERIFY(replaceFile(folder + QStringLiteral("/.ThoughtIgnore"), "build/\n*.tex\n"));
    const Thoughts::ThoughtIgnore edited(folder);
    QVERIFY(edited.excludes(QStringLiteral("notes.tex")));
    QVERIFY(!edited.excludes(QStringLiteral("notes.log")));
    // A broken policy is reported as before, and so is its repair.
    QVERIFY(replaceFile(folder + QStringLiteral("/.ThoughtIgnore"), "\\bad\n"));
    QCOMPARE(Thoughts::ThoughtIgnore(folder).error(), QStringLiteral(".ThoughtIgnore contains an invalid rule."));
    QVERIFY(replaceFile(folder + QStringLiteral("/.ThoughtIgnore"), "*.log\n"));
    QVERIFY(Thoughts::ThoughtIgnore(folder).error().isEmpty());
    // Without a policy file, the defaults.
    QVERIFY(QFile::remove(folder + QStringLiteral("/.ThoughtIgnore")));
    const Thoughts::ThoughtIgnore defaults(folder);
    QVERIFY(defaults.excludes(QStringLiteral(".DS_Store")));
    QVERIFY(!defaults.excludes(QStringLiteral("notes.log")));
}

QTEST_GUILESS_MAIN(FileIdentityCacheTest)

#include "fileidentitycachetest.moc"
