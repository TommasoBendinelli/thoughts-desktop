// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

// The Python publisher reports a failure as the uncaught exception's last
// traceback line, "<Type>: <message>", and exits with status 1. The native
// tools throw Exception at the same points and print that line.
namespace Thoughts::Py
{

struct Exception {
    QString type;
    QString message;
    // The HTTP status of an ApiError (0 for a network failure).
    int status = 0;

    // The traceback's last line: the message is omitted when empty.
    QString line() const
    {
        return message.isEmpty() ? type : type + QStringLiteral(": ") + message;
    }
};

inline const QString renderErrorType = QStringLiteral("thoughts_server.render_types.RenderError");
inline const QString apiErrorType = QStringLiteral("thoughts_server.source_agent.ApiError");
inline const QString sourceInfrastructureErrorType = QStringLiteral("thoughts_server.sources.SourceInfrastructureError");
inline const QString sourceApplyErrorType = QStringLiteral("thoughts_server.sources.SourceApplyError");
inline const QString projectAttachmentErrorType = QStringLiteral("thoughts_server.project_attachments.ProjectAttachmentError");
inline const QString jsonDecodeErrorType = QStringLiteral("json.decoder.JSONDecodeError");

[[noreturn]] inline void raise(const QString &type, const QString &message)
{
    throw Exception{type, message};
}

[[noreturn]] inline void raiseValueError(const QString &message)
{
    raise(QStringLiteral("ValueError"), message);
}

[[noreturn]] inline void raiseRenderError(const QString &message)
{
    raise(renderErrorType, message);
}

[[noreturn]] inline void raiseApiError(int status, const QString &detail)
{
    throw Exception{apiErrorType, detail, status};
}

// isinstance(error, ValueError)
inline bool isValueError(const Exception &error)
{
    return error.type == QLatin1String("ValueError") || error.type == QLatin1String("UnicodeDecodeError")
        || error.type == QLatin1String("UnicodeEncodeError") || error.type == jsonDecodeErrorType || error.type == projectAttachmentErrorType
        || error.type == sourceApplyErrorType;
}

// isinstance(error, OSError)
inline bool isOsError(const Exception &error)
{
    static const char *const names[] = {"OSError",        "FileNotFoundError", "FileExistsError",   "IsADirectoryError", "NotADirectoryError",
                                        "PermissionError", "InterruptedError",  "TimeoutError",      "BlockingIOError",   "BrokenPipeError"};
    for (const char *name : names) {
        if (error.type == QLatin1String(name)) {
            return true;
        }
    }
    return false;
}

// error.__class__.__name__
inline QString className(const Exception &error)
{
    return error.type.mid(error.type.lastIndexOf(QLatin1Char('.')) + 1);
}

}
