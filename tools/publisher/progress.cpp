// SPDX-License-Identifier: GPL-2.0-or-later
#include "progress.h"

#include "pyio.h"
#include "pystr.h"

namespace Thoughts::Publisher::Progress
{
namespace
{
bool active = false;
}

void setActive(bool value)
{
    active = value;
}

bool isActive()
{
    return active;
}

void report(const char *operation, const Py::Path &docsRoot, const Py::Path &path)
{
    if (!active) {
        return;
    }
    const std::optional<QString> relative = path.relativeTo(docsRoot);
    if (!relative) {
        return;
    }
    const Py::Path candidate(*relative);
    if (candidate.isAbsolute() || candidate.segments().contains(QStringLiteral("..")) || candidate.segments().isEmpty()) {
        return;
    }
    const Py::Json payload = Py::Json::object({{QStringLiteral("schema_version"), Py::Json(1)},
                                               {QStringLiteral("event"), Py::Json(QStringLiteral("local_source_access"))},
                                               {QStringLiteral("operation"), Py::Json(QString::fromLatin1(operation))},
                                               {QStringLiteral("path"), Py::Json(Py::leftCodePoints(candidate.segments().join(QLatin1Char('/')), 512))}});
    Py::DumpOptions options;
    options.sortKeys = true;
    options.separators = QPair<QString, QString>(QStringLiteral(","), QStringLiteral(":"));
    Py::printError(QByteArrayLiteral("THOUGHTS_LOCAL_SOURCE_PROGRESS ") + Py::dumpsChecked(payload, options));
}

}
