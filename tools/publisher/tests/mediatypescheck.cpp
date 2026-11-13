// SPDX-License-Identifier: GPL-2.0-or-later
// Prints guessMediaType() for each line of stdin (UTF-8), one per line.
#include "../mediatypes.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile input;
    input.open(stdin, QIODevice::ReadOnly);
    QFile output;
    output.open(stdout, QIODevice::WriteOnly);
    for (const QByteArray &line : input.readAll().split('\n')) {
        output.write(Thoughts::Publisher::guessMediaType(QString::fromUtf8(line)).toUtf8() + '\n');
    }
    return 0;
}
