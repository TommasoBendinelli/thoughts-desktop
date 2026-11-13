// SPDX-License-Identifier: GPL-2.0-or-later
// Rule N (folder names) and rule P (rewrite) of the source folder naming
// change, against the vectors shared with the server and Android.
#include "../part/sourcefoldernames.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>

using Thoughts::SourceFolderNames;
using Thoughts::SourceFolderRename;
using Thoughts::SourceFolderRenames;

namespace
{
// Fedora's tests/fixtures/source_folder_names_vectors.json, copied verbatim.
QJsonObject vectors()
{
    QFile file(QStringLiteral(KDESRCDIR "data/source-folder-names/source_folder_names_vectors.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

SourceFolderRenames renames(const QJsonArray &values)
{
    SourceFolderRenames result;
    for (const QJsonValue &value : values) {
        const QJsonObject entry = value.toObject();
        result.append({entry.value(QStringLiteral("folder_id")).toString(), entry.value(QStringLiteral("old")).toString(), entry.value(QStringLiteral("new")).toString()});
    }
    return result;
}

const SourceFolderRenames Map{{QStringLiteral("f1"), QStringLiteral("docs-x"), QStringLiteral("docs-x-abc123")},
                              {QStringLiteral("f2"), QStringLiteral("docs-template"), QStringLiteral("docs-template-c8ee33")}};
}

class SourceFolderNamesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRuleP_data()
    {
        QTest::addColumn<QJsonArray>("map");
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("output");
        const QJsonArray groups = vectors().value(QStringLiteral("rule_p")).toArray();
        QVERIFY(!groups.isEmpty());
        int row = 0;
        for (const QJsonValue &group : groups) {
            for (const QJsonValue &value : group.toObject().value(QStringLiteral("cases")).toArray()) {
                const QJsonObject entry = value.toObject();
                QTest::addRow("%d %s", row++, qPrintable(entry.value(QStringLiteral("note")).toString()))
                    << group.toObject().value(QStringLiteral("map")).toArray() << entry.value(QStringLiteral("input")).toString() << entry.value(QStringLiteral("output")).toString();
            }
        }
        // Legacy Project ids (amendment 2026-10-27): "source:docs-x" never changes; scope keys do.
        QTest::addRow("project id") << SourceFolderNames::toJson(Map) << QStringLiteral("source:docs-template") << QStringLiteral("source:docs-template");
        QTest::addRow("project id in a path") << SourceFolderNames::toJson(Map)
                                              << QStringLiteral("{\"id\": \"source:docs-x\", \"path\": \"docs-x/a.tex\", \"scope\": \"project:docs-x\"}")
                                              << QStringLiteral("{\"id\": \"source:docs-x\", \"path\": \"docs-x-abc123/a.tex\", \"scope\": \"project:docs-x-abc123\"}");
        QTest::addRow("project id key") << SourceFolderNames::toJson(Map) << QStringLiteral("project:source:docs-x Source:docs-x")
                                        << QStringLiteral("project:source:docs-x Source:docs-x-abc123");
        // Desktop extras: non-ASCII neighbours are boundaries in text and UTF-8 alike.
        QTest::addRow("non-ASCII neighbours") << SourceFolderNames::toJson(Map) << QStringLiteral("\u00e9 docs-x\u2192docs-x") << QStringLiteral("\u00e9 docs-x-abc123\u2192docs-x-abc123");
    }
    void testRuleP()
    {
        QFETCH(QJsonArray, map);
        QFETCH(QString, input);
        QFETCH(QString, output);
        QCOMPARE(SourceFolderNames::rewrite(input, renames(map)), output);
        // Bytes and text agree (UTF-8 JSON files, SQLite text and blobs).
        QCOMPARE(SourceFolderNames::rewrite(input.toUtf8(), renames(map)), output.toUtf8());
    }

    // Values: keys and single-line strings; contents fields and multi-line strings stay.
    void testRulePValues()
    {
        const QJsonArray groups = vectors().value(QStringLiteral("rule_p_values")).toArray();
        QVERIFY(!groups.isEmpty());
        for (const QJsonValue &group : groups) {
            const SourceFolderRenames map = renames(group.toObject().value(QStringLiteral("map")).toArray());
            for (const QJsonValue &value : group.toObject().value(QStringLiteral("cases")).toArray()) {
                const QJsonObject entry = value.toObject();
                const QJsonValue input = entry.value(QStringLiteral("input"));
                const QJsonValue output = entry.value(QStringLiteral("output"));
                QCOMPARE(SourceFolderNames::rewriteJson(input, map), output);
                // The textual rewrite of the same JSON gives the same value.
                const QByteArray text = input.isObject() ? QJsonDocument(input.toObject()).toJson() : QJsonDocument(input.toArray()).toJson();
                const QJsonDocument rewritten = QJsonDocument::fromJson(SourceFolderNames::rewriteJsonText(text, map));
                QCOMPARE(rewritten.isObject() ? QJsonValue(rewritten.object()) : QJsonValue(rewritten.array()), output);
            }
        }
    }

    // Whole manifests: a legacy Project id source:docs-x keeps its bytes.
    void testRulePManifests()
    {
        const QJsonArray groups = vectors().value(QStringLiteral("rule_p_manifests")).toArray();
        QVERIFY(!groups.isEmpty());
        for (const QJsonValue &group : groups) {
            const SourceFolderRenames map = renames(group.toObject().value(QStringLiteral("map")).toArray());
            for (const QJsonValue &value : group.toObject().value(QStringLiteral("cases")).toArray()) {
                const QJsonObject entry = value.toObject();
                const QString input = entry.value(QStringLiteral("input")).toString();
                const QString output = entry.value(QStringLiteral("output")).toString();
                QCOMPARE(SourceFolderNames::rewrite(input, map), output);
                // The migration's textual JSON rewrite gives the same bytes.
                QCOMPARE(SourceFolderNames::rewriteJsonText(input.toUtf8(), map), output.toUtf8());
            }
        }
    }

    void testSlug_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("slug");
        const QJsonArray cases = vectors().value(QStringLiteral("slug")).toArray();
        QVERIFY(!cases.isEmpty());
        int row = 0;
        for (const QJsonValue &value : cases) {
            const QJsonObject entry = value.toObject();
            QTest::addRow("%d", row++) << entry.value(QStringLiteral("name")).toString() << entry.value(QStringLiteral("slug")).toString();
        }
    }
    void testSlug()
    {
        QFETCH(QString, name);
        QFETCH(QString, slug);
        // A blank name falls back to the old name; here both are the same input.
        QCOMPARE(SourceFolderNames::slug(name, name), slug);
    }

    void testName_data()
    {
        QTest::addColumn<QString>("display");
        QTest::addColumn<QString>("old");
        QTest::addColumn<QString>("folderId");
        QTest::addColumn<QStringList>("taken");
        QTest::addColumn<QString>("expected");
        const QJsonArray cases = vectors().value(QStringLiteral("folder_name")).toArray();
        QVERIFY(!cases.isEmpty());
        int row = 0;
        for (const QJsonValue &value : cases) {
            const QJsonObject entry = value.toObject();
            QStringList taken;
            for (const QJsonValue &name : entry.value(QStringLiteral("taken")).toArray()) {
                taken.append(name.toString());
            }
            QTest::addRow("%d", row++) << entry.value(QStringLiteral("display")).toString() << entry.value(QStringLiteral("old")).toString()
                                       << entry.value(QStringLiteral("folder_id")).toString() << taken << entry.value(QStringLiteral("new")).toString();
        }
    }
    void testName()
    {
        QFETCH(QString, display);
        QFETCH(QString, old);
        QFETCH(QString, folderId);
        QFETCH(QStringList, taken);
        QFETCH(QString, expected);
        QSet<QString> lowered;
        for (const QString &name : std::as_const(taken)) {
            // The folder's own old name never clashes.
            if (name.compare(old, Qt::CaseInsensitive) != 0) {
                lowered.insert(name.toLower());
            }
        }
        const QString name = SourceFolderNames::name(SourceFolderNames::slug(display, old), SourceFolderNames::hexSource(old, folderId), [&lowered](const QString &candidate) {
            return lowered.contains(candidate);
        });
        QCOMPARE(name, expected);
        QVERIFY(SourceFolderNames::isStrictFolderName(name));
        QVERIFY(SourceFolderNames::isRuleNName(name));
    }

    void testImportFolderNameVectors()
    {
        const QJsonArray cases = vectors().value(QStringLiteral("import_folder_name")).toArray();
        QVERIFY(!cases.isEmpty());
        for (const QJsonValue &value : cases) {
            const QJsonObject entry = value.toObject();
            QSet<QString> lowered;
            for (const QJsonValue &name : entry.value(QStringLiteral("taken")).toArray()) {
                lowered.insert(name.toString().toLower());
            }
            QCOMPARE(SourceFolderNames::importFolderName(entry.value(QStringLiteral("display")).toString(), entry.value(QStringLiteral("operation_id")).toString(),
                                                         [&lowered](const QString &candidate) {
                                                             return lowered.contains(candidate);
                                                         }),
                     entry.value(QStringLiteral("new")).toString());
        }
    }

    void testIsRuleNName()
    {
        const QJsonArray cases = vectors().value(QStringLiteral("is_rule_n_name")).toArray();
        QVERIFY(!cases.isEmpty());
        for (const QJsonValue &value : cases) {
            const QJsonObject entry = value.toObject();
            QCOMPARE(SourceFolderNames::isRuleNName(entry.value(QStringLiteral("name")).toString()), entry.value(QStringLiteral("result")).toBool());
        }
    }

    void testImportFolderName()
    {
        const QString operation = QStringLiteral("1ed79cc6-0123-4567-89ab-cdef01234567");
        QCOMPARE(SourceFolderNames::importFolderName(QStringLiteral("Benchmarks"), operation, {}), QStringLiteral("docs-benchmarks-1ed79c"));
        // Same inputs as Fedora's own fallback: a clash lengthens the hex.
        QCOMPARE(SourceFolderNames::importFolderName(QStringLiteral("Benchmarks"), operation,
                                                     [](const QString &name) {
                                                         return name == QLatin1String("docs-benchmarks-1ed79c");
                                                     }),
                 QStringLiteral("docs-benchmarks-1ed79cc6"));
        QCOMPARE(SourceFolderNames::importFolderName(QString(), operation, {}), QStringLiteral("docs-folder-1ed79c"));
        QVERIFY(SourceFolderNames::importFolderName(QStringLiteral("x"), QStringLiteral("not a uuid"), {}).isEmpty());
        QVERIFY(SourceFolderNames::isLegacyImportFolder(QStringLiteral("docs-import-1ed79cc6012345670123456789abcdef")));
        QVERIFY(!SourceFolderNames::isLegacyImportFolder(QStringLiteral("docs-import-notes")));
    }

    void testMapFromJson()
    {
        const QJsonArray values{QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("f1")}, {QStringLiteral("old"), QStringLiteral("docs-x")}, {QStringLiteral("new"), QStringLiteral("docs-x-abc123")}},
                                QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("f2")}, {QStringLiteral("old"), QStringLiteral("../etc")}, {QStringLiteral("new"), QStringLiteral("docs-y")}},
                                QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("f3")}, {QStringLiteral("old"), QStringLiteral("docs-z")}, {QStringLiteral("new"), QStringLiteral("docs-z/..")}},
                                QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("f4")}, {QStringLiteral("old"), QStringLiteral("docs-w")}, {QStringLiteral("new"), QStringLiteral("docs-w")}}};
        const SourceFolderRenames parsed = SourceFolderNames::fromJson(values);
        QCOMPARE(parsed.size(), 1);
        QCOMPARE(parsed.first().newName, QStringLiteral("docs-x-abc123"));
        QCOMPARE(SourceFolderNames::toJson(parsed), QJsonArray{values.first()});
        QCOMPARE(SourceFolderNames::oldNameOf(QStringLiteral("docs-x-abc123"), parsed), QStringLiteral("docs-x"));
        QCOMPARE(SourceFolderNames::newNameOf(QStringLiteral("docs-x"), parsed), QStringLiteral("docs-x-abc123"));
        QCOMPARE(SourceFolderNames::newNameOf(QStringLiteral("docs-q"), parsed), QStringLiteral("docs-q"));
    }

    // State and metadata change; free text (multi-line strings, contents fields) keeps its bytes.
    void testJsonValue()
    {
        const QJsonObject before{{QStringLiteral("bases"), QJsonObject{{QStringLiteral("docs-x/a.tex"), QJsonObject{{QStringLiteral("revision"), 3}}}}},
                                 {QStringLiteral("project"), QStringLiteral("docs-template")},
                                 {QStringLiteral("contents"), QStringLiteral("see docs-x")},
                                 {QStringLiteral("draft_contents"), QJsonObject{{QStringLiteral("path"), QStringLiteral("docs-x/a.tex")}}},
                                 {QStringLiteral("note"), QStringLiteral("line one docs-x\nline two")},
                                 {QStringLiteral("list"), QJsonArray{QStringLiteral("docs-x"), QStringLiteral("docs-templates")}}};
        qsizetype count = 0;
        const QJsonObject after = SourceFolderNames::rewriteJson(before, Map, &count);
        QCOMPARE(count, 3);
        QVERIFY(after.value(QStringLiteral("bases")).toObject().contains(QStringLiteral("docs-x-abc123/a.tex")));
        QCOMPARE(after.value(QStringLiteral("project")).toString(), QStringLiteral("docs-template-c8ee33"));
        QCOMPARE(after.value(QStringLiteral("contents")), before.value(QStringLiteral("contents")));
        QCOMPARE(after.value(QStringLiteral("draft_contents")), before.value(QStringLiteral("draft_contents")));
        QCOMPARE(after.value(QStringLiteral("note")), before.value(QStringLiteral("note")));
        QCOMPARE(after.value(QStringLiteral("list")).toArray(), (QJsonArray{QStringLiteral("docs-x-abc123"), QStringLiteral("docs-templates")}));
    }

    // The textual JSON rewrite keeps every other byte, as Python's and Kotlin's writers format them.
    void testJsonText()
    {
        const QByteArray before = QByteArrayLiteral(
            "{\n  \"bases\": {\n    \"docs-x/a.tex\": {\"revision\": 3, \"sha256\": \"ab\"}\n  },\n"
            "  \"contents\": \"docs-x\",\n  \"comment_contents\": {\"a\": [\"docs-x\"]},\n"
            "  \"note\": \"one docs-x\\ntwo\",\n  \"tab\": \"\\tdocs-x\",\n  \"escaped\": \"docs-x\\/a\",\n"
            "  \"unicode\": \"\\u00e9 docs-template\",\n  \"n\": 1.50\n}\n");
        qsizetype count = 0;
        const QByteArray after = SourceFolderNames::rewriteJsonText(before, Map, &count);
        QCOMPARE(count, 4);
        QVERIFY(after.contains("\"docs-x-abc123/a.tex\": {\"revision\": 3, \"sha256\": \"ab\"}"));
        QVERIFY(after.contains("\"contents\": \"docs-x\","));
        QVERIFY(after.contains("\"comment_contents\": {\"a\": [\"docs-x\"]}"));
        QVERIFY(after.contains("\"note\": \"one docs-x\\ntwo\""));
        QVERIFY(after.contains("\"tab\": \"\\tdocs-x-abc123\""));
        QVERIFY(after.contains("\"escaped\": \"docs-x-abc123\\/a\""));
        QVERIFY(after.contains("docs-template-c8ee33\""));
        QVERIFY(after.contains("\"n\": 1.50\n}\n"));
        // JSON Lines.
        const QByteArray lines = QByteArrayLiteral("{\"path\":\"docs-x/a.tex\",\"contents\":\"docs-x\"}\n{\"path\":\"docs-template/b.md\"}\n");
        QCOMPARE(SourceFolderNames::rewriteJsonText(lines, Map),
                 QByteArrayLiteral("{\"path\":\"docs-x-abc123/a.tex\",\"contents\":\"docs-x\"}\n{\"path\":\"docs-template-c8ee33/b.md\"}\n"));
        // Nothing to do: the same bytes.
        QCOMPARE(SourceFolderNames::rewriteJsonText(QByteArrayLiteral("{\"a\": \"docs-y\"}"), Map), QByteArrayLiteral("{\"a\": \"docs-y\"}"));
    }
};

QTEST_GUILESS_MAIN(SourceFolderNamesTest)
#include "sourcefoldernamestest.moc"
