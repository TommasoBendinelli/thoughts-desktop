// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyjson.h"

#include <QList>
#include <QString>

// Python operations on json.loads values, raising what Python raises when
// the publisher's code meets an unexpected type. Inputs written by other
// tools fail with the same message as before.
namespace Thoughts::Py
{

// type(value).__name__
QString typeName(const Json &value);
// iter(value): list items, dict keys, str characters; TypeError otherwise.
QList<Json> iterate(const Json &value);
// value.get(key, fallback); AttributeError unless value is a dict.
Json get(const Json &value, const QString &key, const Json &fallback = Json());
// value[key]; KeyError or TypeError like Python.
Json subscript(const Json &value, const QString &key);
// A key that is equal exactly when Python's == (and hash) is, for sets.
// Raises TypeError for unhashable values.
QString hashKey(const Json &value, const char *container = "set element");
// re.fullmatch(..., value): TypeError unless value is a str.
QString requireString(const Json &value);
// bool(value)
bool truthy(const Json &value);
// int(value) within 64 bits; raises ValueError, TypeError or OverflowError.
qint64 toPythonInt(const Json &value);
// str(value) as Python formats a JSON value (repr for containers).
QString pythonStr(const Json &value);

}
