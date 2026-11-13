// SPDX-License-Identifier: GPL-2.0-or-later
// Differential check against Python's json module: for every case directory
// entry NAME.json (input text) and NAME.opts (one option per line), writes
// NAME.out with json.dumps(json.loads(text), **options) or "ERROR: message".
#include "../pyjson.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QDir dir(QString::fromLocal8Bit(argv[1]));
    for (const QString &name : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        const QString base = name.chopped(5);
        QFile input(dir.filePath(name));
        QFile optionsFile(dir.filePath(base + QStringLiteral(".opts")));
        if (!input.open(QIODevice::ReadOnly) || !optionsFile.open(QIODevice::ReadOnly)) {
            return 2;
        }
        Thoughts::Py::DumpOptions options;
        Thoughts::Py::LoadOptions loadOptions;
        for (const QByteArray &line : optionsFile.readAll().split('\n')) {
            const qsizetype equals = line.indexOf('=');
            const QByteArray key = line.left(equals);
            const QByteArray value = equals < 0 ? QByteArray() : line.mid(equals + 1);
            if (key == "indent" && value != "None") options.indent = value.toInt();
            if (key == "sort_keys") options.sortKeys = value == "True";
            if (key == "ensure_ascii") options.ensureAscii = value == "True";
            if (key == "allow_nan") options.allowNan = value == "True";
            if (key == "separators" && !value.isEmpty()) {
                const QList<QByteArray> parts = value.split('|');
                options.separators = QPair<QString, QString>(QString::fromUtf8(parts.value(0)), QString::fromUtf8(parts.value(1)));
            }
            if (key == "reject_duplicates") loadOptions.rejectDuplicateKeys = value == "True";
        }
        QString error;
        QByteArray out;
        const auto decoded = Thoughts::Py::decodeUtf8(input.readAll(), &error);
        const auto loaded = decoded ? Thoughts::Py::loads(*decoded, &error, loadOptions) : std::nullopt;
        if (!loaded) {
            out = "ERROR: " + error.toUtf8();
        } else if (const auto dumped = Thoughts::Py::dumps(*loaded, options, &error)) {
            out = *dumped;
        } else {
            out = "ERROR: " + error.toUtf8();
        }
        QFile output(dir.filePath(base + QStringLiteral(".out")));
        if (!output.open(QIODevice::WriteOnly) || output.write(out) != out.size()) {
            return 3;
        }
    }
    return 0;
}
