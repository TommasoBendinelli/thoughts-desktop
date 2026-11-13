// SPDX-License-Identifier: GPL-2.0-or-later
#include "sourcemerge.h"

#include "projectconfig.h"
#include "pyerror.h"
#include "pyio.h"

#include "xdiff/thoughts-xdiff.h"

#include <QHash>
#include <QSet>

#include <limits>
#include <new>

namespace Thoughts::Publisher
{

namespace
{

// project_configuration._parsed: the validated manifest stored at path.
std::optional<Py::Json> parsedManifest(const QString &path, const QByteArray &content)
{
    if (!isManifestPath(path)) {
        return std::nullopt;
    }
    try {
        Py::LoadOptions options;
        options.rejectDuplicateKeys = true;
        options.duplicateKeyMessage = QStringLiteral("Duplicate Project field");
        Py::Json value = Py::loadsBytes(content, options);
        validateConfiguration(value);
        if (manifestPathOf(value) != path) {
            return std::nullopt;
        }
        return value;
    } catch (const Py::Exception &) {
        return std::nullopt;
    }
}

struct Clash {
};

using Side = std::optional<Py::Json>;

Side memberOf(const Side &object, const QString &key)
{
    if (!object || !object->isObject() || !object->contains(key)) {
        return std::nullopt;
    }
    return object->value(key);
}

// Lists of objects merged entry by entry; any other list must not change on both sides.
QString keyedListIdentity(const QString &key)
{
    if (key == QLatin1String("documents")) {
        return QStringLiteral("document_id");
    }
    if (key == QLatin1String("source_folders")) {
        return QStringLiteral("folder_id");
    }
    return {};
}

Py::Json mergeKeyedList(const Py::Json &base, const Py::Json &current, const Py::Json &incoming, const QString &identity);

// project_configuration._merge_value
Side mergeValue(const Side &base, const Side &current, const Side &incoming, const QString &key = {})
{
    if (current == incoming) {
        return current;
    }
    if (base == current) {
        return incoming;
    }
    if (base == incoming) {
        return current;
    }
    if (base && current && incoming && base->isObject() && current->isObject() && incoming->isObject()) {
        QStringList names = current->keys();
        for (const QString &name : incoming->keys()) {
            if (!current->contains(name)) {
                names.append(name);
            }
        }
        Py::Json result = Py::Json::object();
        for (const QString &name : std::as_const(names)) {
            const Side merged = mergeValue(memberOf(base, name), memberOf(current, name), memberOf(incoming, name), name);
            if (merged) {
                result.insert(name, *merged);
            }
        }
        return result;
    }
    const QString identity = keyedListIdentity(key);
    if (!identity.isEmpty() && base && current && incoming && base->isArray() && current->isArray() && incoming->isArray()) {
        return mergeKeyedList(*base, *current, *incoming, identity);
    }
    throw Clash{};
}

Py::Json mergeKeyedList(const Py::Json &base, const Py::Json &current, const Py::Json &incoming, const QString &identity)
{
    QList<QHash<QString, Py::Json>> entries(3);
    QList<QStringList> orders(3);
    const Py::Json *sides[] = {&base, &current, &incoming};
    for (int side = 0; side < 3; ++side) {
        for (const Py::Json &item : sides[side]->items()) {
            const QString name = item.value(identity).toString();
            entries[side].insert(name, item);
            orders[side].append(name);
        }
    }
    const auto entryOf = [&entries](int side, const QString &name) -> Side {
        const auto found = entries[side].constFind(name);
        return found == entries[side].constEnd() ? Side() : Side(*found);
    };
    QHash<QString, Py::Json> merged;
    QSet<QString> seen;
    for (const QString &name : orders[1] + orders[2] + orders[0]) {
        if (seen.contains(name)) {
            continue;
        }
        seen.insert(name);
        const Side value = mergeValue(entryOf(0, name), entryOf(1, name), entryOf(2, name));
        if (value) {
            merged.insert(name, *value);
        }
    }
    const auto reordered = [&](const QStringList &order) {
        const QSet<QString> present(order.cbegin(), order.cend());
        QStringList common, expected;
        for (const QString &name : order) {
            if (entries[0].contains(name)) {
                common.append(name);
            }
        }
        for (const QString &name : orders[0]) {
            if (entries[0].contains(name) && present.contains(name)) {
                expected.append(name);
            }
        }
        return common != expected;
    };
    const bool currentMoved = reordered(orders[1]);
    const bool incomingMoved = reordered(orders[2]);
    if (currentMoved && incomingMoved) {
        const QSet<QString> currentNames(orders[1].cbegin(), orders[1].cend());
        const QSet<QString> shared = currentNames & QSet<QString>(orders[2].cbegin(), orders[2].cend());
        QStringList currentShared, incomingShared;
        for (const QString &name : orders[1]) {
            if (shared.contains(name)) {
                currentShared.append(name);
            }
        }
        for (const QString &name : orders[2]) {
            if (shared.contains(name)) {
                incomingShared.append(name);
            }
        }
        if (currentShared != incomingShared) {
            throw Clash{};
        }
    }
    // The incoming order wins unless only the current side reordered.
    const QStringList &primary = currentMoved && !incomingMoved ? orders[1] : orders[2];
    const QStringList &secondary = currentMoved && !incomingMoved ? orders[2] : orders[1];
    QStringList result;
    for (const QString &name : primary) {
        if (merged.contains(name)) {
            result.append(name);
        }
    }
    // An entry only the other side has goes before the entry that follows it there, else last.
    for (qsizetype index = secondary.size() - 1; index >= 0; --index) {
        const QString &name = secondary.at(index);
        if (!merged.contains(name) || result.contains(name)) {
            continue;
        }
        qsizetype position = result.size();
        for (qsizetype later = index + 1; later < secondary.size(); ++later) {
            if (result.contains(secondary.at(later))) {
                position = result.indexOf(secondary.at(later));
                break;
            }
        }
        result.insert(position, name);
    }
    Py::Json list = Py::Json::array();
    for (const QString &name : std::as_const(result)) {
        list.append(merged.value(name));
    }
    return list;
}

}

std::optional<QByteArray> mergeManifest(const QString &path, const QByteArray &base, const QByteArray &current, const QByteArray &incoming)
{
    const Side baseValue = parsedManifest(path, base);
    const Side currentValue = parsedManifest(path, current);
    const Side incomingValue = parsedManifest(path, incoming);
    if (!baseValue || !currentValue || !incomingValue) {
        return std::nullopt;
    }
    Side merged;
    try {
        merged = mergeValue(baseValue, currentValue, incomingValue);
        if (!merged) {
            return std::nullopt;
        }
        validateConfiguration(*merged);
    } catch (const Clash &) {
        return std::nullopt;
    } catch (const Py::Exception &) {
        return std::nullopt;
    }
    if (*merged == *currentValue) {
        return current;
    }
    Py::DumpOptions dump;
    dump.indent = 2;
    dump.ensureAscii = false;
    return Py::dumpsChecked(*merged, dump) + QByteArrayLiteral("\n");
}

std::optional<QByteArray> semanticIdentity(const QString &path, const QByteArray &content)
{
    if (!isManifestPath(path)) {
        return std::nullopt;
    }
    try {
        Py::LoadOptions options;
        options.rejectDuplicateKeys = true;
        options.duplicateKeyMessage = QStringLiteral("Duplicate Project field");
        const Py::Json value = Py::loadsBytes(content, options);
        validateConfiguration(value);
        if (manifestPathOf(value) != path) {
            return std::nullopt;
        }
        Py::DumpOptions dump;
        dump.sortKeys = true;
        dump.separators = QPair<QString, QString>(QStringLiteral(","), QStringLiteral(":"));
        dump.allowNan = false;
        return Py::dumpsChecked(value, dump);
    } catch (const Py::Exception &) {
        // ValueError, TypeError and UnicodeError mean "no identity".
        return std::nullopt;
    }
}

QPair<QByteArray, int> mergeFile(const QByteArray &current, const QByteArray &base, const QByteArray &incoming)
{
    static const char *const names[3] = {"current", "base", "incoming"};
    const QByteArray *contents[3] = {&current, &base, &incoming};
    thoughts_xdiff_side sides[3];
    for (int index = 0; index < 3; ++index) {
        // git refuses more than 1023 MiB as binary; long is 32 bits on Windows.
        if (contents[index]->size() > std::numeric_limits<long>::max()) {
            Py::raise(Py::sourceInfrastructureErrorType, QStringLiteral("error: Cannot merge binary files: ") + QLatin1String(names[index]));
        }
        sides[index] = {contents[index]->constData(), static_cast<long>(contents[index]->size()), nullptr};
    }
    sides[0].label = "FEDORA_CURRENT";
    sides[1].label = "COMMON_BASE";
    sides[2].label = "PHONE_INCOMING";
    char *result = nullptr;
    long resultSize = 0;
    int failedSide = -1;
    const char *message = nullptr;
    const int status = thoughts_xdiff_merge_file(&sides[0], &sides[1], &sides[2], &result, &resultSize, &failedSide, &message);
    if (status == THOUGHTS_XDIFF_BINARY) {
        // git's stderr names the file; Fedora's are <tmp>/thoughts-merge-*/current, base, incoming.
        Py::raise(Py::sourceInfrastructureErrorType, QStringLiteral("error: Cannot merge binary files: ") + QLatin1String(names[failedSide]));
    }
    if (status < 0) {
        Py::raise(Py::sourceInfrastructureErrorType, QString::fromUtf8(message).trimmed());
    }
    QByteArray merged;
    try {
        merged = QByteArray(result ? result : "", result ? resultSize : 0);
    } catch (const std::bad_alloc &) {
        thoughts_xdiff_free(result);
        Py::raise(Py::sourceInfrastructureErrorType, QStringLiteral("fatal: Out of memory, malloc failed (tried to allocate %1 bytes)").arg(resultSize));
    }
    thoughts_xdiff_free(result);
    return {merged, status};
}

QPair<QByteArray, int> mergeSources(const QByteArray &base, const QByteArray &current, const QByteArray &incoming, const QString &path)
{
    const std::optional<QByteArray> currentIdentity = semanticIdentity(path, current);
    const std::optional<QByteArray> incomingIdentity = semanticIdentity(path, incoming);
    if (currentIdentity && incomingIdentity) {
        if (*currentIdentity == *incomingIdentity) {
            return {current, 0};
        }
        const std::optional<QByteArray> baseIdentity = semanticIdentity(path, base);
        if (baseIdentity == incomingIdentity) {
            return {current, 0};
        }
        if (baseIdentity == currentIdentity) {
            return {incoming, 0};
        }
        if (const std::optional<QByteArray> merged = mergeManifest(path, base, current, incoming)) {
            return {*merged, 0};
        }
    }
    if (current == base) {
        return {incoming, 0};
    }
    if (incoming == base || incoming == current) {
        return {current, 0};
    }
    return mergeFile(current, base, incoming);
}

}
