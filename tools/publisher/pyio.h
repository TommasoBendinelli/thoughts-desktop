// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyjson.h"

#include <QByteArray>
#include <QString>

// JSON and console output as the Python publisher's code paths produce them,
// raising Py::Exception where Python raises.
namespace Thoughts::Py
{

// json.loads(str): a leading U+FEFF is an error.
Json loadsText(const QString &text, const LoadOptions &options = {});
// json.loads(bytes): detects UTF-8 (with or without BOM), UTF-16 and UTF-32.
Json loadsBytes(const QByteArray &bytes, const LoadOptions &options = {});
// json.dumps; raises ValueError where Python does (NaN with allow_nan=False).
QByteArray dumpsChecked(const Json &value, const DumpOptions &options = {});
// json.dumps(value, indent=2) + "\n", the publisher's file format.
QByteArray dumpsFile(const Json &value, bool sortKeys = false);

// print(line) and print(line, file=sys.stderr, flush=True). Python's text
// streams write "\r\n" on Windows.
void printOut(const QByteArray &line);
void printError(const QByteArray &line);
void printError(const QString &line);
// Writes text as a Python text stream does ("\n" becomes "\r\n" on Windows).
void writeOut(const QByteArray &text);
void writeError(const QByteArray &text);
// Puts stdout and stderr in binary mode so line endings are explicit.
void prepareConsole();

}
