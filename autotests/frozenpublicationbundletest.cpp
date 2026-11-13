// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/frozenpublicationbundle.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace Thoughts;

namespace
{

QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString emptySha256()
{
    return digest(QByteArray());
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    return QDir().mkpath(QFileInfo(path).absolutePath()) && [&] {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush();
    }();
}

DesktopTrackedPath tracked(const QString &path, const QByteArray &bytes, const QString &sourceKind, const QStringList &targets, const QString &role = {}, const QString &mediaType = {})
{
    DesktopTrackedPath value;
    value.path = path;
    value.exists = true;
    value.sizeBytes = bytes.size();
    value.sha256 = digest(bytes);
    value.metadata = QJsonObject {{QStringLiteral("kind"), QStringLiteral("file")}, {QStringLiteral("source_kind"), sourceKind}};
    if (!role.isEmpty()) {
        value.metadata.insert(QStringLiteral("role"), role);
    }
    if (!mediaType.isEmpty()) {
        value.metadata.insert(QStringLiteral("media_type"), mediaType);
    }
    value.affectedTargets = targets;
    return value;
}

DesktopPendingFile pending(const DesktopTrackedPath &path, const QString &action, quint64 generation)
{
    DesktopPendingFile value;
    value.path = path.path;
    value.generation = generation;
    value.action = action;
    value.metadata = path.metadata;
    value.affectedTargets = path.affectedTargets;
    value.hasTrackedState = true;
    value.trackedState = path;
    return value;
}

QJsonObject loadManifest(const FrozenPublicationBundle &bundle)
{
    QFile file(bundle.manifestPath);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

}

class FrozenPublicationBundleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void stagesOnlyChangedBytesAndReusesAcceptedClosure();
    void stagesSourceAndBuildInputDeletionsWithoutObjects();
    void rejectsConcurrentEditsAndRequiresLiveScanForUnknownClosure();
    void freshInventoryRecomputesScopedClosureAndFreezesOnlyUnverifiedBytes();
    void freshInventoriesStageSeveralTargetsOfOneProject();
    void projectInventoryResolvesKnownGraphSourceWithoutScanningOtherProjects();
    void globalDiscoveryFreezesRecomputedGraphAndIgnoresNondependencies();
    void globalDiscoveryReusesOnlyMatchingAcceptedInputs_data();
    void globalDiscoveryReusesOnlyMatchingAcceptedInputs();
};

void FrozenPublicationBundleTest::stagesOnlyChangedBytesAndReusesAcceptedClosure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    const QByteArray changedSource("changed source\n");
    const QByteArray changedPlot("changed plot bytes");
    const QByteArray unchangedAsset("large accepted baseline identity");
    QVERIFY(writeBytes(QDir(docsRoot).filePath(QStringLiteral("docs-paper/tex/source/main.tex")), changedSource));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(QStringLiteral("docs-paper/tex/generated/plot.png")), changedPlot));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(QStringLiteral("docs-paper/tex/assets/logo.png")), unchangedAsset));

    const DesktopTrackedPath source = tracked(QStringLiteral("docs-paper/tex/source/main.tex"), changedSource, QStringLiteral("source"), {QStringLiteral("paper.pdf")}, QStringLiteral("source"));
    const DesktopTrackedPath acceptedSource = tracked(QStringLiteral("docs-paper/tex/source/main.tex"), QByteArrayLiteral("accepted source base\n"), QStringLiteral("source"), {QStringLiteral("paper.pdf")}, QStringLiteral("source"));
    const DesktopTrackedPath plot = tracked(QStringLiteral("docs-paper/tex/generated/plot.png"), changedPlot, QStringLiteral("build_input"), {QStringLiteral("paper.pdf")}, QStringLiteral("generated"), QStringLiteral("image/png"));
    const DesktopTrackedPath asset = tracked(QStringLiteral("docs-paper/tex/assets/logo.png"), unchangedAsset, QStringLiteral("build_input"), {QStringLiteral("paper.pdf")}, QStringLiteral("asset"), QStringLiteral("image/png"));
    DesktopSyncSnapshot snapshot;
    snapshot.throughGeneration = 2;
    snapshot.files = {pending(source, QStringLiteral("upsert"), 1), pending(plot, QStringLiteral("upsert"), 2)};

    const FrozenPublicationBundle bundle = FrozenPublicationStager::stageScoped(docsRoot, stateRoot, QStringLiteral("paper.pdf"), snapshot, {acceptedSource, asset});
    QVERIFY2(bundle.ready(), qPrintable(bundle.error));
    QCOMPARE(bundle.frozenBytes, changedSource.size() + changedPlot.size());
    QVERIFY(QFileInfo::exists(QDir(bundle.directoryPath).filePath(QStringLiteral("objects/%1").arg(source.sha256))));
    QVERIFY(QFileInfo::exists(QDir(bundle.directoryPath).filePath(QStringLiteral("objects/%1").arg(plot.sha256))));
    QVERIFY(!QFileInfo::exists(QDir(bundle.directoryPath).filePath(QStringLiteral("objects/%1").arg(asset.sha256))));

    const QJsonObject manifest = loadManifest(bundle);
    QCOMPARE(manifest.value(QStringLiteral("schema_version")).toInt(), 2);
    QCOMPARE(manifest.value(QStringLiteral("sources")).toArray().size(), 1);
    QCOMPARE(manifest.value(QStringLiteral("sources")).toArray().at(0).toObject().value(QStringLiteral("base_sha256")).toString(), acceptedSource.sha256);
    const QJsonArray buildInputs = manifest.value(QStringLiteral("build_inputs")).toObject().value(QStringLiteral("paper.pdf")).toArray();
    QCOMPARE(buildInputs.size(), 2);
    const auto reused = std::find_if(buildInputs.cbegin(), buildInputs.cend(), [&asset](const QJsonValue &value) { return value.toObject().value(QStringLiteral("sha256")).toString() == asset.sha256; });
    QVERIFY(reused != buildInputs.cend());
    QVERIFY((*reused).toObject().value(QStringLiteral("reuse_verified")).toBool());
    const auto changed = std::find_if(buildInputs.cbegin(), buildInputs.cend(), [&plot](const QJsonValue &value) { return value.toObject().value(QStringLiteral("sha256")).toString() == plot.sha256; });
    QVERIFY(changed != buildInputs.cend());
    QVERIFY(!(*changed).toObject().contains(QStringLiteral("reuse_verified")));
    QCOMPARE(bundle.acceptedEntries.size(), 3);

    QString error;
    QVERIFY2(FrozenPublicationStager::discard(bundle, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(bundle.directoryPath));
}

void FrozenPublicationBundleTest::stagesSourceAndBuildInputDeletionsWithoutObjects()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    QVERIFY(QDir().mkpath(docsRoot));
    const QByteArray oldSource("old source\n");
    const QByteArray oldPlot("old plot");
    DesktopTrackedPath source = tracked(QStringLiteral("docs-paper/tex/source/removed.tex"), oldSource, QStringLiteral("source"), {QStringLiteral("paper.pdf")});
    DesktopTrackedPath plot = tracked(QStringLiteral("docs-paper/tex/generated/removed.png"), oldPlot, QStringLiteral("build_input"), {QStringLiteral("paper.pdf")}, QStringLiteral("generated"), QStringLiteral("image/png"));
    const DesktopTrackedPath acceptedSource = source;
    const DesktopTrackedPath acceptedPlot = plot;
    for (DesktopTrackedPath *removed : {&source, &plot}) {
        removed->exists = false;
        removed->sizeBytes = -1;
        removed->sha256.clear();
    }
    DesktopSyncSnapshot snapshot;
    snapshot.throughGeneration = 2;
    snapshot.files = {pending(source, QStringLiteral("delete"), 1), pending(plot, QStringLiteral("delete"), 2)};

    const FrozenPublicationBundle bundle = FrozenPublicationStager::stageScoped(docsRoot, QDir(temporary.path()).filePath(QStringLiteral("state")), QStringLiteral("paper.pdf"), snapshot, {acceptedSource, acceptedPlot});
    QVERIFY2(bundle.ready(), qPrintable(bundle.error));
    QCOMPARE(bundle.frozenBytes, 0);
    QCOMPARE(QDir(QDir(bundle.directoryPath).filePath(QStringLiteral("objects"))).entryList(QDir::Files).size(), 0);
    const QJsonObject manifest = loadManifest(bundle);
    const QJsonObject sourceDelete = manifest.value(QStringLiteral("sources")).toArray().at(0).toObject();
    QCOMPARE(sourceDelete.value(QStringLiteral("action")).toString(), QStringLiteral("delete"));
    QCOMPARE(sourceDelete.value(QStringLiteral("base_sha256")).toString(), acceptedSource.sha256);
    QVERIFY(!sourceDelete.contains(QStringLiteral("sha256")));
    const QJsonObject buildDelete = manifest.value(QStringLiteral("build_inputs")).toObject().value(QStringLiteral("paper.pdf")).toArray().at(0).toObject();
    QCOMPARE(buildDelete.value(QStringLiteral("action")).toString(), QStringLiteral("delete"));
    QCOMPARE(buildDelete.value(QStringLiteral("role")).toString(), QStringLiteral("generated"));
    QVERIFY(!buildDelete.contains(QStringLiteral("sha256")));
    QCOMPARE(bundle.acceptedEntries.size(), 2);
    QVERIFY(FrozenPublicationStager::discard(bundle));
}

void FrozenPublicationBundleTest::rejectsConcurrentEditsAndRequiresLiveScanForUnknownClosure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    const QString sourcePath = QDir(docsRoot).filePath(QStringLiteral("docs-paper/tex/source/main.tex"));
    const QByteArray frozenBytes("first generation\n");
    QVERIFY(writeBytes(sourcePath, frozenBytes));
    const DesktopTrackedPath source = tracked(QStringLiteral("docs-paper/tex/source/main.tex"), frozenBytes, QStringLiteral("source"), {QStringLiteral("paper.pdf")});
    DesktopSyncSnapshot snapshot;
    snapshot.throughGeneration = 1;
    snapshot.files = {pending(source, QStringLiteral("upsert"), 1)};
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("second generation\n")));
    const FrozenPublicationBundle raced = FrozenPublicationStager::stageScoped(docsRoot, stateRoot, QStringLiteral("paper.pdf"), snapshot, {});
    QCOMPARE(raced.disposition, FrozenPublicationDisposition::Failed);
    QVERIFY(raced.error.contains(QStringLiteral("edited")));
    QVERIFY(raced.directoryPath.isEmpty() || !QFileInfo::exists(raced.directoryPath));

    DesktopTrackedPath directory;
    directory.path = QStringLiteral("docs-paper/tex/generated");
    directory.exists = true;
    directory.metadata.insert(QStringLiteral("kind"), QStringLiteral("directory"));
    directory.affectedTargets = {QStringLiteral("paper.pdf")};
    snapshot.files = {pending(directory, QStringLiteral("upsert"), 2)};
    const FrozenPublicationBundle ambiguous = FrozenPublicationStager::stageScoped(docsRoot, stateRoot, QStringLiteral("paper.pdf"), snapshot, {});
    QCOMPARE(ambiguous.disposition, FrozenPublicationDisposition::RequiresLiveScan);
    QVERIFY(ambiguous.directoryPath.isEmpty());
}

void FrozenPublicationBundleTest::freshInventoryRecomputesScopedClosureAndFreezesOnlyUnverifiedBytes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    const QString target = QStringLiteral("paper.pdf");
    const QString sourcePath = QStringLiteral("docs-paper/tex/source/main.tex");
    const QString newPlotPath = QStringLiteral("docs-paper/tex/generated/new.svg");
    const QString oldPlotPath = QStringLiteral("docs-paper/tex/generated/old.svg");
    const QString logoPath = QStringLiteral("docs-paper/tex/assets/logo.png");
    const QString changedAssetPath = QStringLiteral("docs-paper/tex/assets/theme.dat");
    const QByteArray sourceBytes("\\includegraphics{../generated/new.svg}\n");
    const QByteArray newPlotBytes("<svg>new dependency</svg>\n");
    const QByteArray oldPlotAcceptedBytes("<svg>old accepted dependency</svg>\n");
    const QByteArray oldPlotPendingBytes("<svg>changed but no longer referenced</svg>\n");
    const QByteArray logoBytes("verified logo bytes");
    const QByteArray oldThemeBytes("old theme bytes");
    const QByteArray newThemeBytes("new theme bytes");
    QVERIFY(writeBytes(QDir(docsRoot).filePath(sourcePath), sourceBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(newPlotPath), newPlotBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(oldPlotPath), oldPlotPendingBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(logoPath), logoBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(changedAssetPath), newThemeBytes));

    const DesktopTrackedPath source = tracked(sourcePath, sourceBytes, QStringLiteral("source"), {target}, QStringLiteral("source"));
    DesktopTrackedPath newPlot = tracked(newPlotPath, newPlotBytes, QString(), {}, QStringLiteral("generated"), QStringLiteral("image/svg+xml"));
    newPlot.metadata.insert(QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved"));
    const DesktopTrackedPath oldPlotAccepted = tracked(oldPlotPath, oldPlotAcceptedBytes, QStringLiteral("build_input"), {target}, QStringLiteral("generated"), QStringLiteral("image/svg+xml"));
    const DesktopTrackedPath oldPlotPending = tracked(oldPlotPath, oldPlotPendingBytes, QStringLiteral("build_input"), {target}, QStringLiteral("generated"), QStringLiteral("image/svg+xml"));
    const DesktopTrackedPath logo = tracked(logoPath, logoBytes, QStringLiteral("build_input"), {target}, QStringLiteral("asset"), QStringLiteral("image/png"));
    const DesktopTrackedPath oldTheme = tracked(changedAssetPath, oldThemeBytes, QStringLiteral("build_input"), {target}, QStringLiteral("asset"), QStringLiteral("application/octet-stream"));
    const DesktopTrackedPath newTheme = tracked(changedAssetPath, newThemeBytes, QStringLiteral("build_input"), {target}, QStringLiteral("asset"), QStringLiteral("application/octet-stream"));

    DesktopSyncSnapshot snapshot;
    snapshot.throughGeneration = 4;
    snapshot.files = {pending(source, QStringLiteral("upsert"), 1), pending(newPlot, QStringLiteral("upsert"), 2), pending(oldPlotPending, QStringLiteral("upsert"), 3), pending(newTheme, QStringLiteral("upsert"), 4)};
    const auto inventoryEntry = [](const DesktopTrackedPath &path) {
        return QJsonObject {{QStringLiteral("path"), path.path},
                            {QStringLiteral("sha256"), path.sha256},
                            {QStringLiteral("size_bytes"), path.sizeBytes},
                            {QStringLiteral("media_type"), path.metadata.value(QStringLiteral("media_type"))},
                            {QStringLiteral("role"), path.metadata.value(QStringLiteral("role"))}};
    };
    const QJsonObject inventory {{QStringLiteral("schema_version"), 3},
                                 {QStringLiteral("target"), target},
                                 {QStringLiteral("project"), QStringLiteral("docs-paper")},
                                 {QStringLiteral("entries"), QJsonArray {inventoryEntry(newPlot), inventoryEntry(logo), inventoryEntry(newTheme)}}};

    const FrozenPublicationBundle bundle = FrozenPublicationStager::stageScopedWithFreshInventory(docsRoot, stateRoot, target, inventory, snapshot, {oldPlotAccepted, logo, oldTheme});
    QVERIFY2(bundle.ready(), qPrintable(bundle.error));
    QCOMPARE(bundle.frozenBytes, sourceBytes.size() + newPlotBytes.size() + newThemeBytes.size());
    const QString objectsPath = QDir(bundle.directoryPath).filePath(QStringLiteral("objects"));
    QVERIFY(QFileInfo::exists(QDir(objectsPath).filePath(source.sha256)));
    QVERIFY(QFileInfo::exists(QDir(objectsPath).filePath(newPlot.sha256)));
    QVERIFY(QFileInfo::exists(QDir(objectsPath).filePath(newTheme.sha256)));
    QVERIFY(!QFileInfo::exists(QDir(objectsPath).filePath(logo.sha256)));
    QVERIFY(!QFileInfo::exists(QDir(objectsPath).filePath(oldPlotPending.sha256)));

    const QJsonObject manifest = loadManifest(bundle);
    QCOMPARE(manifest.value(QStringLiteral("sources")).toArray().size(), 1);
    QCOMPARE(manifest.value(QStringLiteral("sources")).toArray().at(0).toObject().value(QStringLiteral("base_sha256")).toString(), emptySha256());
    const QJsonArray buildInputs = manifest.value(QStringLiteral("build_inputs")).toObject().value(target).toArray();
    QCOMPARE(buildInputs.size(), 3);
    const auto findBuildInput = [&buildInputs](const QString &path) {
        return std::find_if(buildInputs.cbegin(), buildInputs.cend(), [&path](const QJsonValue &value) { return value.toObject().value(QStringLiteral("path")).toString() == path; });
    };
    const auto reusedLogo = findBuildInput(logoPath);
    QVERIFY(reusedLogo != buildInputs.cend());
    QVERIFY((*reusedLogo).toObject().value(QStringLiteral("reuse_verified")).toBool());
    const auto newPlotEntry = findBuildInput(newPlotPath);
    QVERIFY(newPlotEntry != buildInputs.cend());
    QVERIFY(!(*newPlotEntry).toObject().contains(QStringLiteral("reuse_verified")));
    const auto changedThemeEntry = findBuildInput(changedAssetPath);
    QVERIFY(changedThemeEntry != buildInputs.cend());
    QVERIFY(!(*changedThemeEntry).toObject().contains(QStringLiteral("reuse_verified")));
    QVERIFY(findBuildInput(oldPlotPath) == buildInputs.cend());

    QCOMPARE(bundle.acceptedEntries.size(), 4);
    QStringList acceptedPaths;
    for (const QJsonValue &value : bundle.acceptedEntries) {
        acceptedPaths.append(value.toObject().value(QStringLiteral("path")).toString());
    }
    acceptedPaths.sort();
    QStringList expectedPaths {sourcePath, newPlotPath, logoPath, changedAssetPath};
    expectedPaths.sort();
    QCOMPARE(acceptedPaths, expectedPaths);
    QVERIFY(std::none_of(bundle.acceptedEntries.cbegin(), bundle.acceptedEntries.cend(), [&oldPlotPath](const QJsonValue &value) { return value.toObject().value(QStringLiteral("path")).toString() == oldPlotPath; }));
    QVERIFY(FrozenPublicationStager::discard(bundle));

    QVERIFY(writeBytes(QDir(docsRoot).filePath(sourcePath), QByteArrayLiteral("edited after the frozen generation\n")));
    const FrozenPublicationBundle raced = FrozenPublicationStager::stageScopedWithFreshInventory(docsRoot, stateRoot, target, inventory, snapshot, {oldPlotAccepted, logo, oldTheme});
    QCOMPARE(raced.disposition, FrozenPublicationDisposition::Failed);
    QVERIFY(raced.error.contains(QStringLiteral("edited")));
    QVERIFY(raced.directoryPath.isEmpty() || !QFileInfo::exists(raced.directoryPath));
}

void FrozenPublicationBundleTest::freshInventoriesStageSeveralTargetsOfOneProject()
{
    // One publisher run for several PDFs of a Project (2026-10-14): a pending
    // source is frozen once, each touched PDF gets its own closure, and a PDF
    // no pending path touches is left out.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    const QString sourcePath = QStringLiteral("docs-paper/tex/source/one.tex");
    const QString sharedPath = QStringLiteral("docs-paper/tex/assets/shared.pdf");
    const QString logoPath = QStringLiteral("docs-paper/tex/assets/logo.png");
    const QByteArray sourceBytes("edited one\n");
    const QByteArray sharedBytes("changed shared figure");
    const QByteArray logoBytes("accepted logo");
    QVERIFY(writeBytes(QDir(docsRoot).filePath(sourcePath), sourceBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(sharedPath), sharedBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(logoPath), logoBytes));
    const QStringList touched {QStringLiteral("one.pdf"), QStringLiteral("two.pdf")};
    const DesktopTrackedPath source = tracked(sourcePath, sourceBytes, QStringLiteral("source"), {QStringLiteral("one.pdf")}, QStringLiteral("source"));
    const DesktopTrackedPath shared = tracked(sharedPath, sharedBytes, QStringLiteral("build_input"), touched, QStringLiteral("asset"), QStringLiteral("application/pdf"));
    const DesktopTrackedPath logo = tracked(logoPath, logoBytes, QStringLiteral("build_input"), touched + QStringList {QStringLiteral("three.pdf")}, QStringLiteral("asset"), QStringLiteral("image/png"));
    DesktopSyncSnapshot snapshot;
    snapshot.throughGeneration = 2;
    snapshot.files = {pending(source, QStringLiteral("upsert"), 1), pending(shared, QStringLiteral("upsert"), 2)};
    const auto inventory = [&](const QString &target) {
        const auto entry = [](const DesktopTrackedPath &path) {
            return QJsonObject {{QStringLiteral("path"), path.path},
                                {QStringLiteral("sha256"), path.sha256},
                                {QStringLiteral("size_bytes"), path.sizeBytes},
                                {QStringLiteral("media_type"), path.metadata.value(QStringLiteral("media_type"))},
                                {QStringLiteral("role"), path.metadata.value(QStringLiteral("role"))}};
        };
        // three.pdf does not read the shared figure.
        const QJsonArray entries = target == QLatin1String("three.pdf") ? QJsonArray {entry(logo)} : QJsonArray {entry(logo), entry(shared)};
        return QJsonObject {{QStringLiteral("schema_version"), 3}, {QStringLiteral("target"), target}, {QStringLiteral("project"), QStringLiteral("docs-paper")}, {QStringLiteral("entries"), entries}};
    };
    const QList<QPair<QString, QJsonObject>> inventories {{QStringLiteral("one.pdf"), inventory(QStringLiteral("one.pdf"))},
                                                          {QStringLiteral("two.pdf"), inventory(QStringLiteral("two.pdf"))},
                                                          {QStringLiteral("three.pdf"), inventory(QStringLiteral("three.pdf"))}};
    const FrozenPublicationBundle bundle = FrozenPublicationStager::stageScopedWithFreshInventories(docsRoot, stateRoot, inventories, snapshot, {logo});
    QVERIFY2(bundle.ready(), qPrintable(bundle.error));
    QCOMPARE(bundle.targets, touched);
    QCOMPARE(bundle.target, QStringLiteral("one.pdf"));
    // The source once, the changed figure once; the logo is reused.
    QCOMPARE(bundle.frozenBytes, sourceBytes.size() + sharedBytes.size());
    const QJsonObject manifest = loadManifest(bundle);
    QCOMPARE(manifest.value(QStringLiteral("sources")).toArray().size(), 1);
    const QJsonObject buildInputs = manifest.value(QStringLiteral("build_inputs")).toObject();
    QCOMPARE(QStringList(buildInputs.keys()), touched);
    for (const QString &target : touched) {
        QCOMPARE(buildInputs.value(target).toArray().size(), 2);
    }
    // Accepted entries: the source, then each PDF's build inputs, in the publisher's order.
    QCOMPARE(bundle.acceptedEntries.size(), 1 + 2 + 2);
    QCOMPARE(bundle.acceptedEntries.at(0).toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("build_input"));
    QCOMPARE(bundle.acceptedEntries.at(0).toObject().value(QStringLiteral("target")).toString(), QStringLiteral("one.pdf"));
    QCOMPARE(bundle.acceptedEntries.at(2).toObject().value(QStringLiteral("target")).toString(), QStringLiteral("two.pdf"));
    QCOMPARE(bundle.acceptedEntries.at(4).toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("source"));
    QVERIFY(FrozenPublicationStager::discard(bundle));

    // The single-target call stages exactly what a one-PDF batch stages.
    const FrozenPublicationBundle single = FrozenPublicationStager::stageScopedWithFreshInventory(docsRoot, stateRoot, QStringLiteral("two.pdf"), inventory(QStringLiteral("two.pdf")), snapshot, {logo});
    const FrozenPublicationBundle batchOfOne = FrozenPublicationStager::stageScopedWithFreshInventories(docsRoot, stateRoot, {{QStringLiteral("two.pdf"), inventory(QStringLiteral("two.pdf"))}}, snapshot, {logo});
    QVERIFY2(single.ready() && batchOfOne.ready(), qPrintable(single.error + batchOfOne.error));
    QCOMPARE(single.acceptedEntries, batchOfOne.acceptedEntries);
    QCOMPARE(loadManifest(single).value(QStringLiteral("build_inputs")), loadManifest(batchOfOne).value(QStringLiteral("build_inputs")));
    QVERIFY(FrozenPublicationStager::discard(single));
    QVERIFY(FrozenPublicationStager::discard(batchOfOne));

    // Nothing pending touches three.pdf alone.
    const FrozenPublicationBundle untouched = FrozenPublicationStager::stageScopedWithFreshInventories(docsRoot, stateRoot, {{QStringLiteral("three.pdf"), inventory(QStringLiteral("three.pdf"))}}, snapshot, {logo});
    QCOMPARE(untouched.disposition, FrozenPublicationDisposition::NoChanges);
}

void FrozenPublicationBundleTest::projectInventoryResolvesKnownGraphSourceWithoutScanningOtherProjects()
{
    const QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    const QString target = QStringLiteral("paper.pdf");
    const QString project = QStringLiteral("docs-paper");
    const QString sourcePath = QStringLiteral("docs-paper/tex/source/new.tex");
    const QByteArray sourceBytes("\\input{fragment}\n");
    QVERIFY(writeBytes(QDir(docsRoot).filePath(sourcePath), sourceBytes));
    QVERIFY(QDir().mkpath(stateRoot));

    const QString refreshedManifestPath = QDir(temporary.path()).filePath(QStringLiteral("project-manifest.json"));
    const QJsonObject refreshedManifest {
        {QStringLiteral("schema_version"), 3},
        {QStringLiteral("shared_files"), QJsonArray()},
        {QStringLiteral("projects"),
         QJsonArray {
             QJsonObject {{QStringLiteral("project"), project},
                          {QStringLiteral("targets"), QJsonArray {target}},
                          {QStringLiteral("files"), QJsonArray {QJsonObject {{QStringLiteral("path"), sourcePath}, {QStringLiteral("role"), QStringLiteral("source")}}}}},
             QJsonObject {{QStringLiteral("project"), QStringLiteral("docs-other")},
                          {QStringLiteral("targets"), QJsonArray()},
                          {QStringLiteral("files"), QJsonArray {QJsonObject {{QStringLiteral("path"), QStringLiteral("docs-other/not-read.tex")}, {QStringLiteral("role"), QStringLiteral("source")}}}}},
         }},
    };
    QVERIFY(writeBytes(refreshedManifestPath, QJsonDocument(refreshedManifest).toJson(QJsonDocument::Compact)));

    DesktopTrackedPath unresolved = tracked(sourcePath, sourceBytes, QString(), {target});
    unresolved.metadata.insert(QStringLiteral("project"), project);
    unresolved.metadata.insert(QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved"));
    DesktopSyncSnapshot snapshot;
    snapshot.throughGeneration = 1;
    snapshot.files = {pending(unresolved, QStringLiteral("upsert"), 1)};
    const QJsonObject inventory {{QStringLiteral("schema_version"), 3}, {QStringLiteral("target"), target}, {QStringLiteral("project"), project}, {QStringLiteral("entries"), QJsonArray()}};

    FrozenPublicationBundle bundle = FrozenPublicationStager::stageProjectTargetWithFreshInventory(docsRoot, stateRoot, project, target, refreshedManifestPath, inventory, snapshot, {});
    QVERIFY2(bundle.ready(), qPrintable(bundle.error));
    QCOMPARE(bundle.frozenBytes, sourceBytes.size());
    QCOMPARE(bundle.acceptedEntries.size(), 1);
    QCOMPARE(bundle.acceptedEntries.at(0).toObject().value(QStringLiteral("path")).toString(), sourcePath);
    QCOMPARE(bundle.acceptedEntries.at(0).toObject().value(QStringLiteral("base_sha256")).toString(), emptySha256());
    QFile frozenManifest(bundle.sourceManifestPath);
    QVERIFY(frozenManifest.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(frozenManifest.readAll()).object(), refreshedManifest);
    // Windows cannot delete a file that is still open.
    frozenManifest.close();
    QVERIFY(FrozenPublicationStager::discard(bundle));

    // A newly discovered file in this project is not necessarily present in
    // the watcher's pending generation (the requirements.txt Sync regression).
    const QString discoveredPath = project + QStringLiteral("/requirements.txt");
    const QByteArray discoveredBytes("example-package==1.0\n");
    QVERIFY(writeBytes(QDir(docsRoot).filePath(discoveredPath), discoveredBytes));
    QJsonObject expandedManifest = refreshedManifest;
    QJsonArray expandedProjects = expandedManifest.value(QStringLiteral("projects")).toArray();
    QJsonObject expandedProject = expandedProjects.first().toObject();
    QJsonArray expandedFiles = expandedProject.value(QStringLiteral("files")).toArray();
    expandedFiles.append(QJsonObject {{QStringLiteral("path"), discoveredPath}, {QStringLiteral("role"), QStringLiteral("source")}});
    expandedProject.insert(QStringLiteral("files"), expandedFiles);
    expandedProjects.replace(0, expandedProject);
    expandedManifest.insert(QStringLiteral("projects"), expandedProjects);
    QVERIFY(writeBytes(refreshedManifestPath, QJsonDocument(expandedManifest).toJson(QJsonDocument::Compact)));
    bundle = FrozenPublicationStager::stageProjectTargetWithFreshInventory(docsRoot, stateRoot, project, target, refreshedManifestPath, inventory, snapshot, {});
    QCOMPARE(bundle.disposition, FrozenPublicationDisposition::RequiresLiveScan);
    QVERIFY(bundle.error.contains(discoveredPath));
    QVERIFY(bundle.directoryPath.isEmpty());

    // Existing accepted sources need no new upload, while a pending addition
    // must actually be frozen before scoped publication can proceed.
    const DesktopTrackedPath discovered = tracked(discoveredPath, discoveredBytes, QStringLiteral("source"), {target}, QStringLiteral("source"));
    bundle = FrozenPublicationStager::stageProjectTargetWithFreshInventory(docsRoot, stateRoot, project, target, refreshedManifestPath, inventory, snapshot, {discovered});
    QVERIFY2(bundle.ready(), qPrintable(bundle.error));
    QCOMPARE(bundle.acceptedEntries.size(), 1);
    QVERIFY(FrozenPublicationStager::discard(bundle));
    snapshot.files.append(pending(discovered, QStringLiteral("upsert"), 1));
    bundle = FrozenPublicationStager::stageProjectTargetWithFreshInventory(docsRoot, stateRoot, project, target, refreshedManifestPath, inventory, snapshot, {});
    QVERIFY2(bundle.ready(), qPrintable(bundle.error));
    QCOMPARE(bundle.acceptedEntries.size(), 2);
    QCOMPARE(bundle.frozenBytes, sourceBytes.size() + discoveredBytes.size());
    QVERIFY(FrozenPublicationStager::discard(bundle));
    snapshot.files.removeLast();
    QVERIFY(writeBytes(refreshedManifestPath, QJsonDocument(refreshedManifest).toJson(QJsonDocument::Compact)));

    // Empty targets are valid for unrelated connected folders, but cannot
    // establish ownership of the PDF requested from the selected project.
    QJsonObject missingTargetManifest = refreshedManifest;
    QJsonArray missingTargetProjects = missingTargetManifest.value(QStringLiteral("projects")).toArray();
    QJsonObject selectedProject = missingTargetProjects.at(0).toObject();
    selectedProject.insert(QStringLiteral("targets"), QJsonArray());
    missingTargetProjects.replace(0, selectedProject);
    missingTargetManifest.insert(QStringLiteral("projects"), missingTargetProjects);
    QVERIFY(writeBytes(refreshedManifestPath, QJsonDocument(missingTargetManifest).toJson(QJsonDocument::Compact)));
    bundle = FrozenPublicationStager::stageProjectTargetWithFreshInventory(docsRoot, stateRoot, project, target, refreshedManifestPath, inventory, snapshot, {});
    QCOMPARE(bundle.disposition, FrozenPublicationDisposition::Failed);
    QVERIFY(bundle.error.contains(QStringLiteral("not owned by the selected project")));
    QVERIFY(writeBytes(refreshedManifestPath, QJsonDocument(refreshedManifest).toJson(QJsonDocument::Compact)));

    unresolved.path = QStringLiteral("docs-paper/tex/source/ambiguous.dat");
    unresolved.metadata.insert(QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved"));
    unresolved.sha256 = digest(QByteArrayLiteral("ambiguous"));
    unresolved.sizeBytes = 9;
    snapshot.files = {pending(unresolved, QStringLiteral("upsert"), 2)};
    snapshot.throughGeneration = 2;
    bundle = FrozenPublicationStager::stageProjectTargetWithFreshInventory(docsRoot, stateRoot, project, target, refreshedManifestPath, inventory, snapshot, {});
    QCOMPARE(bundle.disposition, FrozenPublicationDisposition::RequiresLiveScan);
}

void FrozenPublicationBundleTest::globalDiscoveryFreezesRecomputedGraphAndIgnoresNondependencies()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    const QString sourcePath = QStringLiteral("docs-paper/tex/support/manual_style.sty");
    const QString unrelatedSourcePath = QStringLiteral("docs-paper/tex/source/unrelated.tex");
    const QString newlyManagedSourcePath = QStringLiteral("docs-paper/tex/source/newly-managed.tex");
    const QString newPlotPath = QStringLiteral("docs-paper/tex/generated/new.svg");
    const QString unrelatedPath = QStringLiteral("docs-paper/tex/generated/scratch.svg");
    const QByteArray sourceBytes("% selected shared style generation\n");
    const QByteArray unrelatedSourceBytes("unrelated local TeX draft\n");
    const QByteArray newlyManagedSourceBytes("new managed source\n");
    const QByteArray plotBytes("<svg>new</svg>\n");
    const QByteArray unrelatedBytes("<svg>scratch</svg>\n");
    QVERIFY(writeBytes(QDir(docsRoot).filePath(sourcePath), sourceBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(unrelatedSourcePath), unrelatedSourceBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(newlyManagedSourcePath), newlyManagedSourceBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(newPlotPath), plotBytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(unrelatedPath), unrelatedBytes));
    const QString manifestPath = QDir(temporary.path()).filePath(QStringLiteral("source-manifest.json"));
    const QJsonObject manifest {
        {QStringLiteral("schema_version"), 3},
        {QStringLiteral("projects"),
         QJsonArray {QJsonObject {{QStringLiteral("project"), QStringLiteral("docs-paper")},
                                  {QStringLiteral("targets"), QJsonArray {QStringLiteral("paper.pdf")}},
                                  {QStringLiteral("files"),
                                   QJsonArray {
                                       QJsonObject {{QStringLiteral("path"), sourcePath}, {QStringLiteral("role"), QStringLiteral("support")}},
                                       QJsonObject {{QStringLiteral("path"), unrelatedSourcePath}, {QStringLiteral("role"), QStringLiteral("source")}},
                                       QJsonObject {{QStringLiteral("path"), newlyManagedSourcePath}, {QStringLiteral("role"), QStringLiteral("source")}},
                                   }}}}},
    };
    QVERIFY(writeBytes(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact)));
    const QJsonObject plot {{QStringLiteral("path"), newPlotPath},
                            {QStringLiteral("sha256"), digest(plotBytes)},
                            {QStringLiteral("size_bytes"), plotBytes.size()},
                            {QStringLiteral("media_type"), QStringLiteral("image/svg+xml")},
                            {QStringLiteral("role"), QStringLiteral("generated")}};
    const QJsonObject inventory {{QStringLiteral("schema_version"), 3}, {QStringLiteral("target"), QStringLiteral("paper.pdf")}, {QStringLiteral("project"), QStringLiteral("docs-paper")}, {QStringLiteral("entries"), QJsonArray {plot}}};
    DesktopTrackedPath unresolved = tracked(unrelatedPath, unrelatedBytes, QString(), {QStringLiteral("paper.pdf")});
    unresolved.metadata.insert(QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved"));
    DesktopSyncSnapshot snapshot;
    snapshot.throughGeneration = 3;
    const DesktopTrackedPath selectedSource = tracked(sourcePath, sourceBytes, QStringLiteral("source"), {QStringLiteral("paper.pdf")}, QStringLiteral("support"));
    snapshot.files = {pending(selectedSource, QStringLiteral("upsert"), 2), pending(unresolved, QStringLiteral("upsert"), 3)};
    const DesktopTrackedPath acceptedSource = tracked(sourcePath, QByteArrayLiteral("accepted global source base\n"), QStringLiteral("source"), {QStringLiteral("paper.pdf")}, QStringLiteral("support"));
    const DesktopTrackedPath unrelatedSource = tracked(unrelatedSourcePath, unrelatedSourceBytes, QStringLiteral("source"), {QStringLiteral("paper.pdf")}, QStringLiteral("source"));
    const DesktopTrackedPath acceptedUnrelatedSource = tracked(unrelatedSourcePath, QByteArrayLiteral("accepted unrelated TeX base\n"), QStringLiteral("source"), {QStringLiteral("paper.pdf")}, QStringLiteral("source"));

    FrozenPublicationBundle added = FrozenPublicationStager::stageGlobal(docsRoot, stateRoot, manifestPath, QJsonArray {inventory}, snapshot, {acceptedSource, acceptedUnrelatedSource});
    QVERIFY2(added.ready(), qPrintable(added.error));
    const QJsonObject addedManifest = loadManifest(added);
    const QJsonArray addedSources = addedManifest.value(QStringLiteral("sources")).toArray();
    QCOMPARE(addedSources.size(), 2);
    QSet<QString> addedSourcePaths;
    for (const QJsonValue &value : addedSources) {
        addedSourcePaths.insert(value.toObject().value(QStringLiteral("path")).toString());
    }
    QCOMPARE(addedSourcePaths, QSet<QString>({sourcePath, newlyManagedSourcePath}));
    QVERIFY(!addedSourcePaths.contains(unrelatedSourcePath));
    const QJsonArray addedInputs = addedManifest.value(QStringLiteral("build_inputs")).toObject().value(QStringLiteral("paper.pdf")).toArray();
    QCOMPARE(addedInputs.size(), 1);
    QCOMPARE(addedInputs.at(0).toObject().value(QStringLiteral("path")).toString(), newPlotPath);
    QVERIFY(std::none_of(added.acceptedEntries.cbegin(), added.acceptedEntries.cend(), [&unrelatedPath](const QJsonValue &value) { return value.toObject().value(QStringLiteral("path")).toString() == unrelatedPath; }));
    QVERIFY(FrozenPublicationStager::discard(added));

    DesktopSyncSnapshot completeSnapshot = snapshot;
    completeSnapshot.files.append(pending(unrelatedSource, QStringLiteral("upsert"), 4));
    completeSnapshot.throughGeneration = 4;
    FrozenPublicationBundle complete = FrozenPublicationStager::stageGlobal(docsRoot, stateRoot, manifestPath, QJsonArray {inventory}, completeSnapshot, {acceptedSource, acceptedUnrelatedSource});
    QVERIFY2(complete.ready(), qPrintable(complete.error));
    const QJsonArray completeSources = loadManifest(complete).value(QStringLiteral("sources")).toArray();
    QCOMPARE(completeSources.size(), 3);
    QVERIFY(std::any_of(completeSources.cbegin(), completeSources.cend(), [&unrelatedSourcePath](const QJsonValue &value) { return value.toObject().value(QStringLiteral("path")).toString() == unrelatedSourcePath; }));
    QVERIFY(FrozenPublicationStager::discard(complete));

    QJsonObject removedInventory = inventory;
    removedInventory.insert(QStringLiteral("entries"), QJsonArray());
    FrozenPublicationBundle removed = FrozenPublicationStager::stageGlobal(docsRoot, stateRoot, manifestPath, QJsonArray {removedInventory}, {}, {acceptedSource, acceptedUnrelatedSource});
    QVERIFY2(removed.ready(), qPrintable(removed.error));
    QCOMPARE(loadManifest(removed).value(QStringLiteral("build_inputs")).toObject().value(QStringLiteral("paper.pdf")).toArray().size(), 0);
    QVERIFY(FrozenPublicationStager::discard(removed));
}

void FrozenPublicationBundleTest::globalDiscoveryReusesOnlyMatchingAcceptedInputs_data()
{
    QTest::addColumn<QString>("variation");
    QTest::addColumn<bool>("reuse");
    QTest::addColumn<bool>("ready");
    QTest::newRow("unchanged") << QString() << true << true;
    QTest::newRow("changed-content") << QStringLiteral("content") << false << true;
    QTest::newRow("changed-size") << QStringLiteral("size") << false << true;
    QTest::newRow("changed-role") << QStringLiteral("role") << false << true;
    QTest::newRow("changed-media-type") << QStringLiteral("media") << false << true;
    QTest::newRow("new-target") << QStringLiteral("target") << false << true;
    QTest::newRow("source-is-not-build-input") << QStringLiteral("source") << false << true;
    QTest::newRow("previously-deleted") << QStringLiteral("deleted") << false << true;
    QTest::newRow("duplicate-baseline") << QStringLiteral("duplicate") << false << false;
    QTest::newRow("changed-during-preparation") << QStringLiteral("race") << false << false;
}

void FrozenPublicationBundleTest::globalDiscoveryReusesOnlyMatchingAcceptedInputs()
{
    QFETCH(QString, variation);
    QFETCH(bool, reuse);
    QFETCH(bool, ready);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    const QString path = QStringLiteral("docs-paper/tex/generated/plot.svg");
    const QString addedPath = QStringLiteral("docs-paper/tex/generated/new.svg");
    const QByteArray bytes("<svg>accepted plot</svg>");
    const QByteArray addedBytes("<svg>new dependency</svg>");
    QVERIFY(writeBytes(QDir(docsRoot).filePath(path), bytes));
    QVERIFY(writeBytes(QDir(docsRoot).filePath(addedPath), variation == QLatin1String("race") ? QByteArrayLiteral("later edit") : addedBytes));
    const QString manifestPath = QDir(temporary.path()).filePath(QStringLiteral("source-manifest.json"));
    const QJsonObject manifest {{QStringLiteral("schema_version"), 3},
        {QStringLiteral("projects"), QJsonArray {QJsonObject {{QStringLiteral("project"), QStringLiteral("docs-paper")},
            {QStringLiteral("files"), QJsonArray()}}}}};
    QVERIFY(writeBytes(manifestPath, QJsonDocument(manifest).toJson()));
    const auto identity = [](const QString &filePath, const QByteArray &content) {
        return QJsonObject {{QStringLiteral("path"), filePath}, {QStringLiteral("sha256"), digest(content)},
            {QStringLiteral("size_bytes"), content.size()}, {QStringLiteral("role"), QStringLiteral("generated")},
            {QStringLiteral("media_type"), QStringLiteral("image/svg+xml")}};
    };
    const QJsonObject inventory {{QStringLiteral("schema_version"), 3}, {QStringLiteral("target"), QStringLiteral("paper.pdf")},
        {QStringLiteral("project"), QStringLiteral("docs-paper")},
        {QStringLiteral("entries"), QJsonArray {identity(path, bytes), identity(addedPath, addedBytes)}}};
    DesktopTrackedPath baseline = tracked(path, bytes, QStringLiteral("build_input"), {QStringLiteral("paper.pdf"), QStringLiteral("second.pdf")}, QStringLiteral("generated"), QStringLiteral("image/svg+xml"));
    if (variation == QLatin1String("content")) {
        baseline.sha256 = digest(QByteArrayLiteral("old contents"));
    }
    if (variation == QLatin1String("size")) {
        ++baseline.sizeBytes;
    }
    if (variation == QLatin1String("role")) {
        baseline.metadata.insert(QStringLiteral("role"), QStringLiteral("asset"));
    }
    if (variation == QLatin1String("media")) {
        baseline.metadata.insert(QStringLiteral("media_type"), QStringLiteral("image/png"));
    }
    if (variation == QLatin1String("target")) {
        baseline.affectedTargets = {QStringLiteral("unrelated.pdf")};
    }
    if (variation == QLatin1String("source")) {
        baseline.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    }
    if (variation == QLatin1String("deleted")) {
        baseline.exists = false;
    }
    QList<DesktopTrackedPath> baselines {baseline};
    // An old dependency must not reappear merely because its baseline exists.
    baselines.append(tracked(QStringLiteral("docs-paper/tex/generated/removed.svg"), QByteArrayLiteral("removed"), QStringLiteral("build_input"), {QStringLiteral("paper.pdf")}, QStringLiteral("generated"), QStringLiteral("image/svg+xml")));
    if (variation == QLatin1String("duplicate")) {
        baselines.append(baseline);
    }
    QJsonObject secondInventory = inventory;
    secondInventory.insert(QStringLiteral("target"), QStringLiteral("second.pdf"));
    const FrozenPublicationBundle bundle = FrozenPublicationStager::stageGlobal(docsRoot, stateRoot, manifestPath, QJsonArray {inventory, secondInventory}, {}, baselines);
    QCOMPARE(bundle.ready(), ready);
    if (!ready) {
        QVERIFY(!bundle.error.isEmpty());
        QVERIFY(!QFileInfo::exists(bundle.directoryPath));
        return;
    }
    QCOMPARE(bundle.frozenBytes, addedBytes.size() + (reuse ? 0 : bytes.size()));
    const QJsonObject buildInputs = loadManifest(bundle).value(QStringLiteral("build_inputs")).toObject();
    QCOMPARE(buildInputs.size(), 2);
    for (const QString &target : {QStringLiteral("paper.pdf"), QStringLiteral("second.pdf")}) {
        const QJsonArray entries = buildInputs.value(target).toArray();
        QCOMPARE(entries.size(), 2);
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            QCOMPARE(entry.value(QStringLiteral("reuse_verified")).toBool(), reuse && entry.value(QStringLiteral("path")).toString() == path);
        }
    }
    QCOMPARE(bundle.acceptedEntries.size(), 4);
    QCOMPARE(QFileInfo::exists(QDir(bundle.directoryPath).filePath(QStringLiteral("objects/") + digest(bytes))), !reuse);
    QVERIFY(FrozenPublicationStager::discard(bundle));

    if (reuse) {
        // A repeat run with no new dependencies freezes no object bytes.
        baselines.append(tracked(addedPath, addedBytes, QStringLiteral("build_input"), {QStringLiteral("paper.pdf"), QStringLiteral("second.pdf")}, QStringLiteral("generated"), QStringLiteral("image/svg+xml")));
        const FrozenPublicationBundle repeat = FrozenPublicationStager::stageGlobal(docsRoot, stateRoot, manifestPath, QJsonArray {inventory, secondInventory}, {}, baselines);
        QVERIFY2(repeat.ready(), qPrintable(repeat.error));
        QCOMPARE(repeat.frozenBytes, 0);
        QCOMPARE(repeat.acceptedEntries.size(), 4);
        QVERIFY(FrozenPublicationStager::discard(repeat));
    }
}

QTEST_GUILESS_MAIN(FrozenPublicationBundleTest)
#include "frozenpublicationbundletest.moc"
