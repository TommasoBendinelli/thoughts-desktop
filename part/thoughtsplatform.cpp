// SPDX-License-Identifier: GPL-2.0-or-later
#include "thoughtsplatform.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUuid>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <wincred.h>
#include <winioctl.h>
#include <string>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <SystemConfiguration/SystemConfiguration.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>
#endif
#ifndef Q_OS_WIN
#include <pwd.h>
#endif

namespace Thoughts::Platform
{

namespace
{
#ifdef Q_OS_WIN
std::wstring nativePath(const QString &path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
}

// Opens path for querying its attributes only. Directories need backup
// semantics; links are opened themselves rather than their targets.
HANDLE openForQuery(const QString &path, bool followLinks = true)
{
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (!followLinks) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    return CreateFileW(nativePath(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
}

QString handleIdentity(HANDLE handle)
{
    FILE_ID_INFO id {};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        const QByteArray fileId(reinterpret_cast<const char *>(id.FileId.Identifier), sizeof(id.FileId.Identifier));
        return QStringLiteral("%1:%2").arg(static_cast<qulonglong>(id.VolumeSerialNumber), 0, 16).arg(QString::fromLatin1(fileId.toHex()));
    }
    // Older file systems (FAT, some network shares) only have 64-bit indexes.
    BY_HANDLE_FILE_INFORMATION information {};
    if (GetFileInformationByHandle(handle, &information)) {
        const quint64 index = (quint64(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
        return QStringLiteral("%1:%2").arg(static_cast<qulonglong>(information.dwVolumeSerialNumber), 0, 16).arg(index, 16, 16, QLatin1Char('0'));
    }
    return {};
}

QString credentialTarget(const QString &service, const QString &account)
{
    return service + QLatin1Char('/') + account;
}

// The mount-point member of the kernel's REPARSE_DATA_BUFFER, which the
// user-mode SDK headers do not declare.
struct MountPointReparseBuffer {
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    WCHAR PathBuffer[1];
};
constexpr DWORD mountPointHeaderSize = 8;

bool setJunctionTarget(const QString &link, const QString &target, QString *error)
{
    const QString printName = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath());
    const QString substituteName = QStringLiteral("\\??\\") + printName;
    const std::wstring substitute = substituteName.toStdWString();
    const std::wstring print = printName.toStdWString();
    const DWORD substituteBytes = DWORD(substitute.size() * sizeof(WCHAR));
    const DWORD printBytes = DWORD(print.size() * sizeof(WCHAR));
    const DWORD pathBytes = substituteBytes + sizeof(WCHAR) + printBytes + sizeof(WCHAR);
    const DWORD dataLength = 4 * sizeof(WORD) + pathBytes;
    if (mountPointHeaderSize + dataLength > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        if (error) {
            *error = QStringLiteral("The Library folder path is too long for a Windows junction.");
        }
        return false;
    }
    QByteArray buffer(int(mountPointHeaderSize + dataLength + sizeof(WCHAR)), '\0');
    auto *reparse = reinterpret_cast<MountPointReparseBuffer *>(buffer.data());
    reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->ReparseDataLength = WORD(dataLength);
    reparse->SubstituteNameOffset = 0;
    reparse->SubstituteNameLength = WORD(substituteBytes);
    reparse->PrintNameOffset = WORD(substituteBytes + sizeof(WCHAR));
    reparse->PrintNameLength = WORD(printBytes);
    memcpy(reparse->PathBuffer, substitute.c_str(), substituteBytes);
    memcpy(reinterpret_cast<char *>(reparse->PathBuffer) + reparse->PrintNameOffset, print.c_str(), printBytes);

    HANDLE handle = CreateFileW(nativePath(link).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = QStringLiteral("The Library folder link could not be opened (Windows error %1).").arg(GetLastError());
        }
        return false;
    }
    DWORD returned = 0;
    // Replacing the data of an existing junction retargets it atomically.
    const bool set = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), mountPointHeaderSize + dataLength, nullptr, 0, &returned, nullptr);
    const DWORD lastError = GetLastError();
    CloseHandle(handle);
    if (!set && error) {
        *error = QStringLiteral("The Library folder link could not be pointed at the new generation (Windows error %1).").arg(lastError);
    }
    return set;
}
#endif
}

namespace
{
// macOS takes the data folders from the account's home (or CFFIXED_USER_HOME),
// never from HOME, so a process given a private HOME (a test, an isolated
// thoughtsctl) would still use the real folders. Keep them under HOME there,
// as Linux does.
QString followHome(const QString &location)
{
#ifdef Q_OS_MACOS
    if (qEnvironmentVariableIsSet("CFFIXED_USER_HOME")) {
        return location;
    }
    const QString account = accountHomeDirectory();
    const QString home = QDir::homePath();
    if (account.isEmpty() || home == account || QFileInfo(home).canonicalFilePath() == QFileInfo(account).canonicalFilePath()) {
        return location;
    }
    if (location == account || location.startsWith(account + QLatin1Char('/'))) {
        return home + location.mid(account.size());
    }
#endif
    return location;
}
}

QString accountHomeDirectory()
{
#ifdef Q_OS_WIN
    return {};
#else
    const passwd *entry = getpwuid(getuid());
    return entry && entry->pw_dir ? QDir::cleanPath(QFile::decodeName(entry->pw_dir)) : QString();
#endif
}

bool isAccountDataRoot(const QString &root)
{
#ifdef Q_OS_WIN
    Q_UNUSED(root)
    return false;
#else
    const QString account = accountHomeDirectory();
    const QString path = QDir::cleanPath(root);
    return !account.isEmpty() && !QStandardPaths::isTestModeEnabled() && (path == account || path.startsWith(account + QLatin1Char('/')));
#endif
}

QString applicationDataRoot()
{
#ifdef Q_OS_WIN
    return thoughtsDataRoot();
#else
    return followHome(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
#endif
}

QString thoughtsDataRoot()
{
    return QDir(followHome(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))).filePath(QStringLiteral("Thoughts"));
}

QByteArray cleanSecret(QByteArray secret)
{
    if (secret.startsWith("\xEF\xBB\xBF")) {
        secret.remove(0, 3);
    }
    return secret.trimmed();
}

QByteArray readSecret(const QString &service, const QString &account)
{
#if defined(Q_OS_MACOS)
    QProcess process;
    process.start(QStringLiteral("/usr/bin/security"), {QStringLiteral("find-generic-password"), QStringLiteral("-s"), service, QStringLiteral("-a"), account, QStringLiteral("-w")});
    if (process.waitForStarted(1000) && process.waitForFinished(2000) && process.exitCode() == 0) {
        return cleanSecret(process.readAllStandardOutput());
    }
    return {};
#elif defined(Q_OS_WIN)
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(credentialTarget(service, account).toStdWString().c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        return {};
    }
    QByteArray secret(reinterpret_cast<const char *>(credential->CredentialBlob), int(credential->CredentialBlobSize));
    CredFree(credential);
    // cmdkey and PowerShell store the secret as UTF-16; Thoughts stores UTF-8.
    bool utf16 = secret.size() >= 2 && secret.size() % 2 == 0;
    for (qsizetype index = 1; utf16 && index < secret.size(); index += 2) {
        utf16 = secret.at(index) == '\0';
    }
    if (utf16) {
        secret = QString::fromUtf16(reinterpret_cast<const char16_t *>(secret.constData()), secret.size() / 2).toUtf8();
    }
    // Earlier versions of store-secret kept a byte order mark piped in by PowerShell.
    return cleanSecret(secret);
#else
    Q_UNUSED(service)
    Q_UNUSED(account)
    return {};
#endif
}

bool writeSecret(const QString &service, const QString &account, const QByteArray &secret, QString *error)
{
#if defined(Q_OS_MACOS)
    // A secret, service and account of plain token characters go to
    // `security -i` on its standard input, so the secret never appears in a
    // process list; anything else is passed as an argument, as it always was.
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9._~+/=:@-]+$"));
    const bool viaInput = plain.match(QString::fromUtf8(secret)).hasMatch() && plain.match(service).hasMatch() && plain.match(account).hasMatch();
    QProcess process;
    if (viaInput) {
        process.start(QStringLiteral("/usr/bin/security"), {QStringLiteral("-i")});
    } else {
        process.start(QStringLiteral("/usr/bin/security"),
                      {QStringLiteral("add-generic-password"), QStringLiteral("-U"), QStringLiteral("-s"), service, QStringLiteral("-a"), account, QStringLiteral("-w"), QString::fromUtf8(secret)});
    }
    if (process.waitForStarted(1000)) {
        if (viaInput) {
            process.write("add-generic-password -U -s \"" + service.toUtf8() + "\" -a \"" + account.toUtf8() + "\" -w \"" + secret + "\"\n");
            // Written now, so the end of input reaches `security`, which then exits.
            process.waitForBytesWritten(2000);
            process.closeWriteChannel();
        }
        // `security -i` exits 0 whatever its commands did: check what is stored.
        if (process.waitForFinished(5000) && process.exitCode() == 0 && (!viaInput || readSecret(service, account) == secret)) {
            return true;
        }
    }
    if (error) {
        const QString message = QString::fromUtf8(process.readAllStandardError()).trimmed();
        *error = message.isEmpty() ? QStringLiteral("The Keychain did not store the secret.") : message;
    }
    return false;
#elif defined(Q_OS_WIN)
    const std::wstring target = credentialTarget(service, account).toStdWString();
    std::wstring user = account.toStdWString();
    QByteArray blob = secret;
    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.UserName = user.data();
    credential.CredentialBlobSize = DWORD(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (CredWriteW(&credential, 0)) {
        return true;
    }
    if (error) {
        *error = QStringLiteral("Windows Credential Manager refused the secret (error %1).").arg(GetLastError());
    }
    return false;
#else
    Q_UNUSED(service)
    Q_UNUSED(account)
    Q_UNUSED(secret)
    if (error) {
        *error = QStringLiteral("This system has no supported secret store.");
    }
    return false;
#endif
}

bool deleteSecret(const QString &service, const QString &account, QString *error)
{
#if defined(Q_OS_MACOS)
    QProcess process;
    process.start(QStringLiteral("/usr/bin/security"), {QStringLiteral("delete-generic-password"), QStringLiteral("-s"), service, QStringLiteral("-a"), account});
    if (process.waitForStarted(1000) && process.waitForFinished(5000)) {
        // 44: errSecItemNotFound, nothing to delete.
        if (process.exitCode() == 0 || process.exitCode() == 44) {
            return true;
        }
    }
    if (error) {
        *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    }
    return false;
#elif defined(Q_OS_WIN)
    if (CredDeleteW(credentialTarget(service, account).toStdWString().c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) {
        return true;
    }
    if (error) {
        *error = QStringLiteral("Windows Credential Manager did not delete the secret (error %1).").arg(GetLastError());
    }
    return false;
#else
    Q_UNUSED(service)
    Q_UNUSED(account)
    if (error) {
        *error = QStringLiteral("This system has no supported secret store.");
    }
    return false;
#endif
}

QString storeSecretCommand(const QString &service, const QString &account)
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("security add-generic-password -U -s %1 -a %2 -w").arg(service, account);
#elif defined(Q_OS_WIN)
    return QStringLiteral("thoughtsctl store-secret --service %1 --account %2").arg(service, account);
#else
    return QStringLiteral("export THOUGHTS_TOKEN=...");
#endif
}

QString desktopDeviceAccount()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("thoughts-mac");
#elif defined(Q_OS_WIN)
    return QStringLiteral("thoughts-windows");
#else
    return QStringLiteral("thoughts-linux");
#endif
}

QString computerName()
{
#if defined(Q_OS_MACOS)
    // The name in System Settings › General › Sharing, e.g. "Alex's MacBook Pro".
    if (CFStringRef name = SCDynamicStoreCopyComputerName(nullptr, nullptr)) {
        const QString text = QString::fromCFString(name).trimmed();
        CFRelease(name);
        if (!text.isEmpty()) {
            return text;
        }
    }
#elif defined(Q_OS_WIN)
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &size) && size > 0) {
        return QString::fromWCharArray(name, int(size));
    }
#endif
    QString host = QSysInfo::machineHostName().trimmed();
    if (host.endsWith(QLatin1String(".local"), Qt::CaseInsensitive)) {
        host.chop(6);
    }
    return host;
}

QString desktopOriginId()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("thoughts-mac");
#elif defined(Q_OS_WIN)
    return QStringLiteral("thoughts-windows");
#else
    return QStringLiteral("thoughts-linux");
#endif
}

QString reviewOriginId()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("mac-okular");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows-okular");
#else
    return QStringLiteral("linux-okular");
#endif
}

QString physicalFileIdentity(const QString &path)
{
#ifdef Q_OS_WIN
    HANDLE handle = openForQuery(path);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    const QString identity = handleIdentity(handle);
    CloseHandle(handle);
    return identity;
#else
    struct stat state;
    if (::stat(QFile::encodeName(path).constData(), &state) != 0) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(static_cast<qulonglong>(state.st_dev)).arg(static_cast<qulonglong>(state.st_ino));
#endif
}

QString fileStamp(const QString &path)
{
#ifdef Q_OS_WIN
    HANDLE handle = openForQuery(path);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    FILE_BASIC_INFO basic {};
    FILE_STANDARD_INFO standard {};
    const bool ok = GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) && GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) && !standard.Directory;
    const QString identity = ok ? handleIdentity(handle) : QString();
    CloseHandle(handle);
    if (identity.isEmpty()) {
        return {};
    }
    // ChangeTime is NTFS's status-change time: it moves on any rewrite,
    // rename or attribute change, like ctime on Unix.
    return path + QStringLiteral(":%1:%2:%3:%4")
                      .arg(identity)
                      .arg(static_cast<qlonglong>(standard.EndOfFile.QuadPart))
                      .arg(static_cast<qlonglong>(basic.LastWriteTime.QuadPart))
                      .arg(static_cast<qlonglong>(basic.ChangeTime.QuadPart));
#else
    struct stat s;
    if (::stat(QFile::encodeName(path).constData(), &s) || !S_ISREG(s.st_mode)) {
        return {};
    }
#ifdef Q_OS_MACOS
    const auto mt = s.st_mtimespec;
    const auto ct = s.st_ctimespec;
#else
    const auto mt = s.st_mtim;
    const auto ct = s.st_ctim;
#endif
    return path + QStringLiteral(":%1:%2:%3:%4:%5:%6:%7").arg(s.st_dev).arg(s.st_ino).arg(s.st_size).arg(mt.tv_sec).arg(mt.tv_nsec).arg(ct.tv_sec).arg(ct.tv_nsec);
#endif
}

bool replaceFileContents(const QString &path, const QByteArray &bytes)
{
    for (int attempt = 0;; ++attempt) {
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) {
            return true;
        }
#ifdef Q_OS_WIN
        if (attempt < 20) {
            QThread::msleep(25);
            continue;
        }
#endif
        return false;
    }
}

bool flushToDisk(int fileDescriptor)
{
#ifdef Q_OS_WIN
    return _commit(fileDescriptor) == 0;
#else
    return ::fsync(fileDescriptor) == 0;
#endif
}

bool flushDirectory(const QString &path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    return true;
#else
    const int descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY);
    if (descriptor < 0) {
        return false;
    }
    const bool success = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return success;
#endif
}

bool cloneOrLinkFile(const QString &source, const QString &destination)
{
#if defined(Q_OS_MACOS)
    // An APFS clone shares the unchanged bytes but stays an independent file.
    return ::clonefile(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData(), CLONE_NOFOLLOW) == 0;
#elif defined(Q_OS_WIN)
    return CreateHardLinkW(nativePath(destination).c_str(), nativePath(source).c_str(), nullptr);
#else
    return ::link(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData()) == 0;
#endif
}

bool isDirectoryLink(const QFileInfo &info)
{
#ifdef Q_OS_WIN
    return info.isSymbolicLink() || info.isJunction();
#else
    return info.isSymLink();
#endif
}

QString directoryLinkTarget(const QFileInfo &info)
{
#ifdef Q_OS_WIN
    if (info.isJunction()) {
        return QDir::cleanPath(QDir::fromNativeSeparators(info.junctionTarget()));
    }
    if (info.isSymbolicLink()) {
        return QFileInfo(info.symLinkTarget()).absoluteFilePath();
    }
    return {};
#else
    return info.isSymLink() ? QFileInfo(info.symLinkTarget()).absoluteFilePath() : QString();
#endif
}

bool replaceDirectoryLink(const QString &link, const QString &target, QString *error)
{
#ifdef Q_OS_WIN
    const QFileInfo info(link);
    if (info.exists() || info.isJunction()) {
        if (!info.isJunction()) {
            if (error) {
                *error = QStringLiteral("%1 is a folder, not a link.").arg(link);
            }
            return false;
        }
        if (setJunctionTarget(link, target, error)) {
            return true;
        }
        // A file system that refuses to retarget in place: replace the link.
        // Removing a junction never touches the folder it points to.
        if (!RemoveDirectoryW(nativePath(link).c_str())) {
            return false;
        }
    }
    if (!CreateDirectoryW(nativePath(link).c_str(), nullptr)) {
        if (error) {
            *error = QStringLiteral("The Library folder link could not be created (Windows error %1).").arg(GetLastError());
        }
        return false;
    }
    if (!setJunctionTarget(link, target, error)) {
        RemoveDirectoryW(nativePath(link).c_str());
        return false;
    }
    return true;
#else
    const QString temporary = link + QStringLiteral(".next-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QByteArray encodedTemporary = QFile::encodeName(temporary);
    QFile::remove(temporary);
    if (::symlink(QFile::encodeName(target).constData(), encodedTemporary.constData()) != 0) {
        if (error) {
            *error = QString::fromLocal8Bit(strerror(errno));
        }
        return false;
    }
    if (::rename(encodedTemporary.constData(), QFile::encodeName(link).constData()) == 0) {
        return true;
    }
    if (error) {
        *error = QString::fromLocal8Bit(strerror(errno));
    }
    QFile::remove(temporary);
    return false;
#endif
}

bool directoryHasFilesInUse(const QString &directory)
{
#ifdef Q_OS_WIN
    QDirIterator iterator(directory, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        // An exclusive open fails while any other handle has the file open,
        // which is exactly when Windows would refuse to delete it.
        HANDLE handle = CreateFileW(nativePath(path).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD lastError = GetLastError();
            if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND) {
                return true;
            }
            continue;
        }
        CloseHandle(handle);
    }
    return false;
#else
    Q_UNUSED(directory)
    return false;
#endif
}

QString nativeServerTool(const QString &name)
{
#ifdef Q_OS_WIN
    const QString file = name + QStringLiteral(".exe");
#else
    const QString &file = name;
#endif
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(file);
    return QFileInfo(path).isExecutable() ? path : QString();
}

void keepStandardStreamsPrivate()
{
#ifdef Q_OS_WIN
    for (const DWORD stream : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        const HANDLE handle = GetStdHandle(stream);
        if (handle && handle != INVALID_HANDLE_VALUE) {
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        }
    }
#endif
}

bool startInNewConsole(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment, const QString &workingDirectory)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    if (!workingDirectory.isEmpty()) {
        process.setWorkingDirectory(workingDirectory);
    }
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags &= ~DWORD(CREATE_NO_WINDOW | DETACHED_PROCESS);
        arguments->flags |= CREATE_NEW_CONSOLE;
    });
#endif
    return process.startDetached();
}

bool processIsRunning(const QString &pid)
{
    bool ok = false;
    const qint64 id = pid.toLongLong(&ok);
    if (!ok || id <= 0) {
        return false;
    }
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(id));
    if (!process) {
        return false;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    return ::kill(pid_t(id), 0) == 0 || errno == EPERM;
#endif
}

bool focusWindowTitled(const QString &title)
{
#ifdef Q_OS_WIN
    struct Search {
        std::wstring title;
        HWND found = nullptr;
    } search {title.toStdWString(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto *search = reinterpret_cast<Search *>(parameter);
            wchar_t text[512] = {};
            if (IsWindowVisible(window) && GetWindowTextW(window, text, 512) > 0 && search->title == text) {
                search->found = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
        return false;
    }
    if (IsIconic(search.found)) {
        ShowWindow(search.found, SW_RESTORE);
    }
    return SetForegroundWindow(search.found);
#else
    Q_UNUSED(title)
    return false;
#endif
}

bool revealInFileManager(const QString &path)
{
#if defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path});
#elif defined(Q_OS_WIN)
    const QFileInfo info(path);
    QStringList arguments;
    if (!info.isDir()) {
        arguments << QStringLiteral("/select,");
    }
    arguments << QDir::toNativeSeparators(info.absoluteFilePath());
    return QProcess::startDetached(QStringLiteral("explorer.exe"), arguments);
#else
    const QFileInfo info(path);
    return QProcess::startDetached(QStringLiteral("xdg-open"), {info.isDir() ? info.absoluteFilePath() : info.absolutePath()});
#endif
}

bool markDownloaded(const QString &path)
{
#if defined(Q_OS_MACOS)
    // flags;time;agent;event: 0081 is what a browser writes for a download
    // the user has not approved yet. A mark that is already there stays (it
    // may record that approval).
    if (isMarkedDownloaded(path)) {
        return true;
    }
    const QByteArray value = "0081;" + QByteArray::number(QDateTime::currentSecsSinceEpoch(), 16) + ";Thoughts;";
    return ::setxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", value.constData(), size_t(value.size()), 0, XATTR_NOFOLLOW) == 0;
#elif defined(Q_OS_WIN)
    QFile zone(path + QStringLiteral(":Zone.Identifier"));
    const QByteArray value("[ZoneTransfer]\r\nZoneId=3\r\n");
    return zone.open(QIODevice::WriteOnly | QIODevice::Truncate) && zone.write(value) == value.size();
#else
    Q_UNUSED(path)
    return true;
#endif
}

bool isMarkedDownloaded(const QString &path)
{
#if defined(Q_OS_MACOS)
    return ::getxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", nullptr, 0, 0, XATTR_NOFOLLOW) > 0;
#elif defined(Q_OS_WIN)
    QFile zone(path + QStringLiteral(":Zone.Identifier"));
    return zone.open(QIODevice::ReadOnly) && zone.readAll().contains("ZoneId=");
#else
    Q_UNUSED(path)
    return false;
#endif
}

bool openWithApplication(const QString &application, const QString &path)
{
#if defined(Q_OS_MACOS)
    return application.isEmpty() ? QProcess::startDetached(QStringLiteral("/usr/bin/open"), {path})
                                 : QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), application, path});
#elif defined(Q_OS_WIN)
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    return application.isEmpty() ? QProcess::startDetached(QStringLiteral("explorer.exe"), {native}) : QProcess::startDetached(application, {native});
#else
    return application.isEmpty() ? QProcess::startDetached(QStringLiteral("xdg-open"), {path}) : QProcess::startDetached(application, {path});
#endif
}

}
