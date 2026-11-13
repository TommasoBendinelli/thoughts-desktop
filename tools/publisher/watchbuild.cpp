// SPDX-License-Identifier: GPL-2.0-or-later
// thoughts-watch-build: follow one Fedora build in a terminal (the native
// replacement for thoughts_server.build_watch). It polls the build and its
// renderer output, stamps every line with the local time and summarizes the
// errors of a failed build.

#include "argparse.h"
#include "pyerror.h"
#include "pyfs.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "sourceapi.h"

#include <QCoreApplication>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#ifdef Q_OS_WIN
#include <cwchar>
#include <io.h>
#include <windows.h>

#include <shellapi.h>
#else
#include <unistd.h>
#endif

using namespace Thoughts;
using namespace Thoughts::Publisher;
using Py::Json;

namespace
{
const QSet<QString> terminalStatuses{QStringLiteral("succeeded"), QStringLiteral("attention"), QStringLiteral("failed"), QStringLiteral("cancelled")};
const QSet<QString> failedStatuses{QStringLiteral("attention"), QStringLiteral("failed")};

QString timestamp()
{
    const std::time_t now = std::time(nullptr);
#ifdef Q_OS_WIN
    std::tm parts{};
    localtime_s(&parts, &now);
    wchar_t buffer[160] = {};
    std::wcsftime(buffer, 160, L"[%Y-%m-%d %H:%M:%S %Z]", &parts);
    return QString::fromWCharArray(buffer);
#else
    std::tm parts{};
    localtime_r(&now, &parts);
    char buffer[160] = {};
    std::strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S %Z]", &parts);
    return QString::fromLocal8Bit(buffer);
#endif
}

bool isLineBreak(char32_t c)
{
    return c == U'\n' || c == U'\r' || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x85 || c == 0x2028 || c == 0x2029;
}

// str.splitlines(keepends=True)
QStringList splitLinesKeepingEnds(const QString &text)
{
    QStringList lines;
    const std::u32string points = Py::toCodePoints(text);
    size_t start = 0;
    size_t index = 0;
    while (index < points.size()) {
        if (!isLineBreak(points[index])) {
            ++index;
            continue;
        }
        if (points[index] == U'\r' && index + 1 < points.size() && points[index + 1] == U'\n') {
            ++index;
        }
        ++index;
        lines.append(Py::fromCodePoints(points.substr(start, index - start)));
        start = index;
    }
    if (start < points.size()) {
        lines.append(Py::fromCodePoints(points.substr(start)));
    }
    return lines;
}

// _timestamp_text: every non-empty line gets the time; blank lines do not.
QString timestampText(const QString &text)
{
    QString stamped;
    for (const QString &line : splitLinesKeepingEnds(text)) {
        QString content = line;
        while (content.endsWith(QLatin1Char('\r')) || content.endsWith(QLatin1Char('\n'))) {
            content.chop(1);
        }
        const QString ending = line.mid(content.size());
        stamped += content.isEmpty() ? ending : timestamp() + QLatin1Char(' ') + content + ending;
    }
    return stamped;
}

void printEvent(const QString &message = {}, bool toStderr = false)
{
    const QByteArray text = timestampText(message + QLatin1Char('\n')).toUtf8();
    if (toStderr) {
        Py::writeError(text);
    } else {
        Py::writeOut(text);
    }
}

// _TimestampedLineWriter: renderer output, one stamped line per "\n".
class LineWriter
{
public:
    void write(const QString &text)
    {
        m_pending += text;
        qsizetype newline;
        while ((newline = m_pending.indexOf(QLatin1Char('\n'))) >= 0) {
            QString line = m_pending.left(newline);
            m_pending = m_pending.mid(newline + 1);
            QString ending = QStringLiteral("\n");
            if (line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);
                ending = QStringLiteral("\r\n");
            }
            Py::writeOut(timestampText(line + ending).toUtf8());
        }
        std::fflush(stdout);
    }

    void finish()
    {
        if (!m_pending.isEmpty()) {
            Py::writeOut(timestampText(m_pending + QLatin1Char('\n')).toUtf8());
            m_pending.clear();
        }
        std::fflush(stdout);
    }

private:
    QString m_pending;
};

QString formatSeconds(const char *format, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return QString::fromLatin1(buffer);
}

// _format_duration
QString formatDuration(double milliseconds)
{
    double seconds = milliseconds / 1000.0;
    if (!(seconds > 0.0)) {
        seconds = 0.0;
    }
    if (seconds < 60) {
        return formatSeconds("%.1fs", seconds);
    }
    const double remainder = std::fmod(seconds, 60.0);
    const double minutes = std::floor((seconds - remainder) / 60.0);
    return QStringLiteral("%1m %2").arg(qint64(minutes)).arg(formatSeconds("%04.1fs", remainder));
}

// shlex.quote / shlex.join
QString shellQuote(const QString &text)
{
    if (text.isEmpty()) {
        return QStringLiteral("''");
    }
    bool safe = true;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        const bool ok = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9') || u == u'_' || u == u'@' || u == u'%'
            || u == u'+' || u == u'=' || u == u':' || u == u',' || u == u'.' || u == u'/' || u == u'-';
        safe = safe && ok;
    }
    if (safe) {
        return text;
    }
    QString escaped = text;
    escaped.replace(QStringLiteral("'"), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString shellJoin(const QList<Json> &items)
{
    QStringList quoted;
    for (const Json &item : items) {
        quoted.append(shellQuote(item.toString()));
    }
    return quoted.join(QLatin1Char(' '));
}

bool isStringList(const Json &value)
{
    if (!value.isArray()) {
        return false;
    }
    for (const Json &item : value.items()) {
        if (!item.isString()) {
            return false;
        }
    }
    return true;
}

bool isPdfName(const QString &text)
{
    return !text.contains(QLatin1Char('/')) && !text.contains(QLatin1Char('\\')) && Py::casefold(text).endsWith(QLatin1String(".pdf"));
}

void sleepFor(double seconds)
{
    if (std::isnan(seconds)) {
        Py::raiseValueError(QStringLiteral("Invalid value NaN (not a number)"));
    }
    if (std::isinf(seconds)) {
        Py::raise(QStringLiteral("OverflowError"), QStringLiteral("timestamp too large to convert to C _PyTime_t"));
    }
    QThread::usleep(static_cast<unsigned long>(seconds * 1000000.0));
}

void printTimingSummary(const Json &result)
{
    const Json timings = Py::get(result, QStringLiteral("timings_ms"));
    if (!timings.isObject()) {
        return;
    }
    static const QList<QPair<QString, QString>> ordered{{QStringLiteral("source materialization"), QStringLiteral("source_materialization")},
                                                         {QStringLiteral("build-input materialization"), QStringLiteral("build_input_materialization")},
                                                         {QStringLiteral("annotation export"), QStringLiteral("annotation_export")},
                                                         {QStringLiteral("renderer"), QStringLiteral("renderer")},
                                                         {QStringLiteral("publication"), QStringLiteral("publication")},
                                                         {QStringLiteral("cleanup"), QStringLiteral("cleanup")},
                                                         {QStringLiteral("total"), QStringLiteral("total")}};
    QStringList parts;
    for (const auto &[label, key] : ordered) {
        const Json value = timings.value(key);
        if (value.isNumber() || value.isBool()) {
            const double milliseconds = value.isBool() ? (value.toBool() ? 1.0 : 0.0) : value.toDouble();
            parts.append(label + QLatin1Char(' ') + formatDuration(milliseconds));
        }
    }
    if (!parts.isEmpty()) {
        printEvent(QStringLiteral("Build timings: %1.").arg(parts.join(QStringLiteral(", "))));
    }
}

// _failure_entries
QStringList failureEntries(const Json &build, const QString &target)
{
    const Json result = Py::get(build, QStringLiteral("result"));
    const Json failures = result.isObject() ? result.value(QStringLiteral("failed_documents")) : Json();
    QStringList entries;
    QSet<QString> seen;
    if (failures.isArray()) {
        for (const Json &failure : failures.items()) {
            if (!failure.isObject()) {
                continue;
            }
            Json pdf = failure.value(QStringLiteral("pdf_filename"));
            if (!Py::truthy(pdf)) {
                pdf = failure.value(QStringLiteral("filename"));
            }
            QString pdfText = Py::truthy(pdf) ? Py::strip(Py::pythonStr(pdf)) : target;
            if (pdfText.isEmpty()) {
                pdfText = target;
            }
            const Json source = failure.value(QStringLiteral("source"));
            Json message = failure.value(QStringLiteral("message"));
            if (!Py::truthy(message)) {
                message = failure.value(QStringLiteral("error"));
            }
            const QString sourceText = source.isString() ? Py::strip(source.toString()) : QString();
            const QString messageText = message.isString() ? Py::strip(message.toString()) : QString();
            const QString key = pdfText + QChar(0) + sourceText + QChar(0) + messageText;
            if (seen.contains(key)) {
                continue;
            }
            seen.insert(key);
            QStringList parts{pdfText};
            if (!sourceText.isEmpty() && !messageText.contains(sourceText)) {
                parts.append(sourceText);
            }
            if (!messageText.isEmpty()) {
                parts.append(messageText);
            }
            entries.append(parts.join(QStringLiteral(" — ")));
        }
    }
    if (!entries.isEmpty()) {
        return entries;
    }
    const Json error = Py::get(build, QStringLiteral("error"));
    if (error.isString() && !Py::strip(error.toString()).isEmpty()) {
        return {target + QStringLiteral(" — ") + Py::strip(error.toString())};
    }
    const QString status = build.contains(QStringLiteral("status")) ? Py::pythonStr(build.value(QStringLiteral("status"))) : QStringLiteral("unknown");
    return {target + QStringLiteral(" — Build ended with status %1.").arg(status)};
}

void printFailureSummary(const Json &build, const QString &target)
{
    const QStringList entries = failureEntries(build, target);
    printEvent(QStringLiteral("\nErrors found (%1):").arg(entries.size()));
    for (const QString &entry : entries) {
        printEvent(QStringLiteral("- ") + entry);
    }
    if (Py::get(build, QStringLiteral("status")) == Json(QStringLiteral("attention"))) {
        printEvent(QStringLiteral("\nLast-good PDFs were retained for failed documents."));
    }
}

int watchBuildId(SourceApi &api, const QString &buildId, const QString &target, double pollInterval)
{
    printEvent(QStringLiteral("Build ID: ") + buildId);
    printEvent(QStringLiteral("PDF: ") + target);
    qint64 offset = 0;
    std::optional<QPair<QString, QString>> lastStatus;
    bool printedCommand = false;
    LineWriter rendererOutput;
    for (;;) {
        try {
            const Json build = api.build(buildId);
            if (!printedCommand) {
                const Json command = Py::get(build, QStringLiteral("command"));
                if (isStringList(command)) {
                    printEvent(QStringLiteral("Server renderer:"));
                    printEvent(QStringLiteral("  ") + shellJoin(command.items()) + QLatin1Char('\n'));
                } else {
                    printEvent(QStringLiteral("Server renderer command is not available.\n"));
                }
                printedCommand = true;
            }
            const Json progressValue = Py::get(build, QStringLiteral("progress"));
            const Json progress = progressValue.isObject() ? progressValue : Json::object();
            const QPair<QString, QString> statusKey{
                build.contains(QStringLiteral("status")) ? Py::pythonStr(build.value(QStringLiteral("status"))) : QStringLiteral("unknown"),
                progress.contains(QStringLiteral("detail")) ? Py::pythonStr(progress.value(QStringLiteral("detail"))) : QString()};
            if (statusKey != lastStatus) {
                const QString detail = statusKey.second.isEmpty() ? QString() : QStringLiteral(" — ") + statusKey.second;
                printEvent(QStringLiteral("[%1]%2").arg(statusKey.first, detail));
                lastStatus = statusKey;
            }
            const Json chunk = api.buildOutput(buildId, offset);
            const Json text = Py::get(chunk, QStringLiteral("text"));
            if (text.isString() && !text.toString().isEmpty()) {
                rendererOutput.write(text.toString());
            }
            offset = Py::toPythonInt(Py::get(chunk, QStringLiteral("next_offset"), Json(offset)));
            if (Py::truthy(Py::get(chunk, QStringLiteral("complete"))) && terminalStatuses.contains(statusKey.first)) {
                rendererOutput.finish();
                const Json resultValue = Py::get(build, QStringLiteral("result"));
                const Json result = resultValue.isObject() ? resultValue : Json::object();
                const Json logSha = result.value(QStringLiteral("log_sha256"));
                if (logSha.isString() && !logSha.toString().isEmpty()) {
                    printEvent(QStringLiteral("Log object: ") + logSha.toString());
                }
                printTimingSummary(result);
                printEvent(QStringLiteral("Build %1.").arg(statusKey.first));
                if (failedStatuses.contains(statusKey.first)) {
                    printFailureSummary(build, target);
                }
                return statusKey.first == QLatin1String("succeeded") ? 0 : 1;
            }
        } catch (const Py::Exception &error) {
            if (error.type != Py::apiErrorType) {
                throw;
            }
            if (error.status == 401 || error.status == 403 || error.status == 404) {
                printEvent(QStringLiteral("Build output is unavailable: ") + error.message, true);
                return 1;
            }
            printEvent(QStringLiteral("Build connection interrupted; retrying: ") + error.message, true);
        }
        sleepFor(pollInterval);
    }
}

Json readRequest(const Py::Path &runDirectory)
{
    Json value;
    try {
        value = Py::loadsText(Py::readText(runDirectory / QStringLiteral("request.json")));
    } catch (const Py::Exception &error) {
        if (Py::isOsError(error) || error.type == Py::jsonDecodeErrorType) {
            Py::raiseValueError(QStringLiteral("could not read the build request: ") + error.message);
        }
        throw;
    }
    if (Py::get(value, QStringLiteral("schema_version")) != Json(1) || !value.value(QStringLiteral("target")).isString()) {
        Py::raiseValueError(QStringLiteral("the build request has an unsupported schema"));
    }
    if (!isStringList(value.value(QStringLiteral("submission_command")))) {
        Py::raiseValueError(QStringLiteral("the build request does not contain a valid submission command"));
    }
    return value;
}

QString waitForBuildId(const Py::Path &runDirectory, double pollInterval)
{
    const Py::Path buildIdPath = runDirectory / QStringLiteral("build-id");
    const Py::Path statusPath = runDirectory / QStringLiteral("submission-status.json");
    QString lastMessage;
    for (;;) {
        QString buildId;
        try {
            buildId = Py::strip(Py::readText(buildIdPath));
        } catch (const Py::Exception &error) {
            if (!Py::isOsError(error)) {
                throw;
            }
        }
        if (!buildId.isEmpty()) {
            return buildId;
        }
        Json status = Json::object();
        try {
            status = Py::loadsText(Py::readText(statusPath));
        } catch (const Py::Exception &error) {
            if (!Py::isOsError(error) && error.type != Py::jsonDecodeErrorType) {
                throw;
            }
        }
        const Json message = Py::get(status, QStringLiteral("message"));
        if (message.isString() && !message.toString().isEmpty() && message.toString() != lastMessage) {
            printEvent(QStringLiteral("[source publication] ") + message.toString());
            lastMessage = message.toString();
        }
        if (Py::get(status, QStringLiteral("state")) == Json(QStringLiteral("failed"))) {
            throw std::runtime_error(Py::truthy(message) ? Py::pythonStr(message).toStdString() : std::string("source publication failed"));
        }
        sleepFor(pollInterval);
    }
}

int watchBuild(SourceApi &api, const Py::Path &runDirectory, double pollInterval)
{
    const Json request = readRequest(runDirectory);
    printEvent(QStringLiteral("Local source publication:"));
    printEvent(QStringLiteral("  THOUGHTS_TOKEN=<macOS Keychain> ") + shellJoin(request.value(QStringLiteral("submission_command")).items()));
    const QString target = request.value(QStringLiteral("target")).toString();
    printEvent(QStringLiteral("\nWaiting for the server to accept %1...").arg(target));
    QString buildId;
    try {
        buildId = waitForBuildId(runDirectory, pollInterval);
    } catch (const std::runtime_error &error) {
        printEvent(QStringLiteral("\nSource publication failed: ") + QString::fromStdString(error.what()), true);
        return 1;
    }
    return watchBuildId(api, buildId, target, pollInterval);
}

QStringList commandLineArguments(int argc, char **argv)
{
    QStringList arguments;
#ifdef Q_OS_WIN
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    int count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int index = 1; index < count; ++index) {
        arguments.append(QString::fromWCharArray(wide[index]));
    }
    LocalFree(wide);
#else
    for (int index = 1; index < argc; ++index) {
        arguments.append(Py::decodeFileName(QByteArray(argv[index])));
    }
#endif
    return arguments;
}
}

int main(int argc, char **argv)
{
    Py::prepareConsole();
    const ArgParse::Program program = ArgParse::watchProgram();
    const ArgParse::Outcome outcome = ArgParse::parse(program, ArgParse::watchOptions(), commandLineArguments(argc, argv));
    if (!outcome.arguments) {
        return outcome.exitCode;
    }
    const ArgParse::Arguments &arguments = *outcome.arguments;
    const QString token = arguments.has(QStringLiteral("--token")) ? arguments.value(QStringLiteral("--token")) : qEnvironmentVariable("THOUGHTS_TOKEN");
    if (token.isEmpty()) {
        return ArgParse::programError(program, QStringLiteral("--token or THOUGHTS_TOKEN is required"));
    }
    const double pollInterval = arguments.has(QStringLiteral("--poll-interval")) ? arguments.value(QStringLiteral("--poll-interval")).toDouble() : 0.5;
    if (pollInterval <= 0) {
        return ArgParse::programError(program, QStringLiteral("--poll-interval must be positive"));
    }
    QCoreApplication application(argc, argv);
    try {
        SourceApi api(arguments.value(QStringLiteral("--server")), token);
        const QString buildId = arguments.value(QStringLiteral("--build-id"));
        if (!buildId.isEmpty()) {
            const QString target = arguments.value(QStringLiteral("--target"));
            if (target.isEmpty() || !isPdfName(target)) {
                return ArgParse::programError(program, QStringLiteral("--target must be a PDF filename when --build-id is used"));
            }
            return watchBuildId(api, buildId, target, pollInterval);
        }
        if (!arguments.has(QStringLiteral("--run-directory"))) {
            Py::raise(QStringLiteral("AttributeError"), QStringLiteral("'NoneType' object has no attribute 'resolve'"));
        }
        return watchBuild(api, Py::resolve(Py::Path(arguments.value(QStringLiteral("--run-directory")))), pollInterval);
    } catch (const Py::Exception &error) {
        Py::printError(error.line());
        return 1;
    }
}
