// SPDX-License-Identifier: GPL-2.0-or-later
#include "feedbackversion.h"

#include "documentidentity.h"
#include "verifiedartifactcache.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace Thoughts::FeedbackVersion
{
namespace
{
bool validSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar c : value) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    }
    return true;
}
}

QString field()
{
    return QStringLiteral("review_version");
}

QString cachedFileSha256(const QString &path)
{
    static QMutex mutex;
    static QHash<QString, QString> hashes;
    const QString identity = artifactFileIdentity(path);
    if (identity.isEmpty()) return {};
    {
        QMutexLocker lock(&mutex);
        if (const auto found = hashes.constFind(identity); found != hashes.cend()) return *found;
    }
    const QString digest = QString::fromLatin1(artifactFileSha256(path));
    // Changed while it was read: no stamp rather than a wrong one.
    if (digest.size() != 64 || artifactFileIdentity(path) != identity) return {};
    QMutexLocker lock(&mutex);
    if (hashes.size() >= 4096) hashes.clear();
    hashes.insert(identity, digest);
    return digest;
}

QJsonObject make(const QString &pdfSha256, bool sourceBuilt)
{
    if (!validSha256(pdfSha256)) return {};
    return {{QStringLiteral("pdf_sha256"), pdfSha256}, {QStringLiteral("source_built"), sourceBuilt}};
}

bool librarySourceBuilt(const QString &docsRoot, const QString &filename)
{
    // The Library index says how a published PDF is made: "source_linked" is built from a .tex or
    // .md source; an imported PDF ("unlinked", or one only on this computer) never changes.
    return !filename.isEmpty()
        && DocumentIdentityStore(docsRoot).byFilename(filename).value(QStringLiteral("kind")).toString() == QLatin1String("source_linked");
}

QJsonObject forPdf(const QString &docsRoot, const QString &pdfPath, bool sourceBuiltHint, const QString &knownSha256)
{
    QString digest = knownSha256.toLower();
    if (!validSha256(digest)) {
        if (pdfPath.isEmpty() || !QFileInfo(pdfPath).isFile()) return {};
        digest = cachedFileSha256(pdfPath);
    }
    if (!validSha256(digest)) return {};
    return make(digest, sourceBuiltHint || librarySourceBuilt(docsRoot, QFileInfo(pdfPath).fileName()));
}

QString documentPdfPath(const QString &docsRoot, const QString &documentId, const QString &pdfStem)
{
    // Not DocumentIdentityStore::readablePath(): it verifies the bytes against the index, a second
    // hash of the file; the stamp records the hash of the bytes that are there anyway.
    if (const QJsonObject entry = DocumentIdentityStore(docsRoot).byId(documentId); !entry.isEmpty()) {
        const QString filename = entry.value(QStringLiteral("filename")).toString();
        if (!filename.isEmpty() && !filename.contains(QLatin1Char('/')) && !filename.contains(QLatin1Char('\\'))) {
            const QString published = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/") + filename);
            if (QFileInfo(published).isFile()) return published;
        }
        const QString local = entry.value(QStringLiteral("local_path")).toString();
        if (!local.isEmpty() && QFileInfo(local).isFile()) return local;
    }
    if (pdfStem.isEmpty() || pdfStem.contains(QLatin1Char('/')) || pdfStem.contains(QLatin1Char('\\'))) return {};
    const QString path = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(pdfStem));
    return QFileInfo(path).isFile() ? path : QString();
}

QJsonObject stamped(QJsonObject record, const QJsonObject &version)
{
    if (version.isEmpty()) record.remove(field());
    else record.insert(field(), version);
    return record;
}
}
