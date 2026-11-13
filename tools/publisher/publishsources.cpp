// SPDX-License-Identifier: GPL-2.0-or-later
// thoughts-publish-sources: the native replacement for thoughts-server's
// Python console script of the same name, with the same command line,
// output, files and exit codes.

#include "argparse.h"
#include "buildinputs.h"
#include "manifestcommands.h"
#include "progress.h"
#include "publishcommand.h"
#include "pyerror.h"
#include "pyfs.h"
#include "pyio.h"
#include "sourcereconciliation.h"

#include <QCoreApplication>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>

#include <shellapi.h>
#endif

using namespace Thoughts;
using namespace Thoughts::Publisher;

namespace
{
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

QByteArray sortedJson(const Py::Json &value)
{
    Py::DumpOptions options;
    options.sortKeys = true;
    return Py::dumpsChecked(value, options);
}

int run(const QString &command, const ArgParse::Arguments &arguments)
{
    if (command == QLatin1String("generate-manifest")) {
        Progress::setActive(arguments.flag(QStringLiteral("--desktop-progress-jsonl")));
        const qsizetype count = generateManifest(Py::Path(arguments.value(QStringLiteral("--docs-root"))), Py::Path(arguments.value(QStringLiteral("--output"))));
        Progress::setActive(false);
        Py::printOut(QByteArrayLiteral("Wrote ") + QByteArray::number(count) + QByteArrayLiteral(" managed source paths."));
        return 0;
    }
    if (command == QLatin1String("refresh-project-manifest")) {
        Progress::setActive(arguments.flag(QStringLiteral("--desktop-progress-jsonl")));
        const qsizetype count = refreshProjectManifest(Py::Path(arguments.value(QStringLiteral("--docs-root"))),
                                                       Py::Path(arguments.value(QStringLiteral("--manifest"))),
                                                       arguments.value(QStringLiteral("--project")),
                                                       Py::Path(arguments.value(QStringLiteral("--output"))));
        Progress::setActive(false);
        Py::printOut(QByteArrayLiteral("Wrote ") + QByteArray::number(count) + QByteArrayLiteral(" managed source paths."));
        return 0;
    }
    if (command == QLatin1String("migrate-manifest")) {
        Py::printOut(sortedJson(migrateSourceManifest(Py::Path(arguments.value(QStringLiteral("--docs-root"))),
                                                      Py::Path(arguments.value(QStringLiteral("--manifest"))))));
        return 0;
    }
    if (command == QLatin1String("list-candidates")) {
        Py::printOut(sortedJson(sourceProjectCandidates(Py::Path(arguments.value(QStringLiteral("--docs-root"))))));
        return 0;
    }
    if (command == QLatin1String("inspect-build-inputs") || command == QLatin1String("inspect-library-build-inputs")) {
        Progress::setActive(arguments.flag(QStringLiteral("--desktop-progress-jsonl")));
        const Py::Path root(arguments.value(QStringLiteral("--root")));
        const Py::Path manifest(arguments.value(QStringLiteral("--manifest")));
        const std::optional<QString> project = arguments.has(QStringLiteral("--project")) ? std::optional<QString>(arguments.value(QStringLiteral("--project"))) : std::nullopt;
        const QStringList alsoTargets = arguments.lists.value(QStringLiteral("--also-target"));
        const Py::Json result = command == QLatin1String("inspect-library-build-inputs")
            ? inspectLibraryBuildInputs(root, manifest)
            : (alsoTargets.isEmpty() ? inspectBuildInputs(root, arguments.value(QStringLiteral("--target")), manifest, project)
                                     : inspectProjectBuildInputs(root, QStringList {arguments.value(QStringLiteral("--target"))} + alsoTargets, manifest, project));
        Progress::setActive(false);
        Py::printOut(sortedJson(result));
        return 0;
    }
    if (command == QLatin1String("publish")) {
        return runPublish(arguments);
    }
    Py::raise(QStringLiteral("NotImplementedError"), QStringLiteral("the native publisher does not provide %1 yet").arg(command));
}
}

int main(int argc, char **argv)
{
    Py::prepareConsole();
    const QStringList arguments = commandLineArguments(argc, argv);
    static const QStringList routed{QStringLiteral("generate-manifest"),    QStringLiteral("refresh-project-manifest"),
                                    QStringLiteral("migrate-manifest"),     QStringLiteral("inspect-build-inputs"),
                                    QStringLiteral("inspect-library-build-inputs"), QStringLiteral("list-candidates")};
    // The desktop runs the editable-source installer as
    // "python -m thoughts_server.source_reconciliation"; this program accepts
    // the same command line in place of that interpreter.
    if (arguments.size() >= 2 && arguments.at(0) == QLatin1String("-m") && arguments.at(1) == QLatin1String("thoughts_server.source_reconciliation")) {
        const ArgParse::Outcome outcome = ArgParse::parse(ArgParse::reconcileProgram(), ArgParse::reconcileOptions(), arguments.mid(2));
        if (!outcome.arguments) {
            return outcome.exitCode;
        }
        QCoreApplication application(argc, argv);
        try {
            return runReconcile(*outcome.arguments);
        } catch (const Py::Exception &error) {
            Py::printError(error.line());
            return 1;
        }
    }
    // Anything but an exact subcommand name is a publication.
    const bool explicitCommand = !arguments.isEmpty() && routed.contains(arguments.first());
    const QString command = explicitCommand ? arguments.first() : QStringLiteral("publish");
    const ArgParse::Outcome outcome = ArgParse::parse(command, ArgParse::commandOptions(command), explicitCommand ? arguments.mid(1) : arguments);
    if (!outcome.arguments) {
        return outcome.exitCode;
    }
    // Network requests need an application object.
    std::unique_ptr<QCoreApplication> application;
    if (command == QLatin1String("publish")) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    try {
        return run(command, *outcome.arguments);
    } catch (const Py::Exception &error) {
        Progress::setActive(false);
        Py::printError(error.line());
        return 1;
    }
}
