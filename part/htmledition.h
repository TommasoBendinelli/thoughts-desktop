// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// HTML editions on the desktop (thoughts-html-view-SPEC.md, server docs/html-edition.md):
// the server's state of a PDF's edition, the manifest, and the local cache the HTML view serves.
//
// Cache layout (<root> = <Application Support>/thoughts-server/html-editions):
//   editions/<edition sha256>/manifest.json + every file of the manifest at its path, verified
//   pdf/<pdf sha256>.json      {pdf_sha256, filename, html_status, html_edition_sha256?, html_error?, checked_at}
// An edition directory exists only complete (built in editions/.tmp-* and renamed).
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace Thoughts::HtmlEdition
{
enum class Status { None, Pending, Ready, Failed };

struct State {
    Status status = Status::None;
    QString editionSha256; // lower-case hex, Ready only
    QString error;         // Failed only

    // From a Library entry / render-state document / GET /v1/html-editions reply: html_status,
    // html_edition_sha256, html_error (flat or under "html").
    static State fromFields(const QJsonObject &fields);
    // nullopt when the object carries no html_status key at all (older server or listing).
    static std::optional<State> fromListedFields(const QJsonObject &fields);
    QString statusName() const;
    // Final for its PDF version: a ready edition (named) or a failed conversion.
    bool isFinal() const
    {
        return status == Status::Failed || (status == Status::Ready && !editionSha256.isEmpty());
    }
    bool operator==(const State &other) const
    {
        return status == other.status && editionSha256 == other.editionSha256 && error == other.error;
    }
};

bool isSha256(const QString &text);
// A manifest file path: relative, '/'-separated, no empty/'.'/'..' segments, no '\\', ':' or controls.
bool isValidPath(const QString &path);

struct Manifest {
    QJsonObject raw;
    QString entry = QStringLiteral("index.html");
    QHash<QString, QString> files; // path -> sha256
    QSet<QString> anchors;         // marker keys present in the HTML (empty: unknown)
    QJsonObject items;             // {cards: {id: {anchors, question_anchors}}, comments: {id: [keys]}}
    QJsonObject sourceFiles;       // Markdown: file -> sha256
    QString sourceMap = QStringLiteral("none");
    QString sourceFormat;
    QString pdfSha256;
    QString documentId;

    static std::optional<Manifest> parse(const QByteArray &bytes, QString *error = nullptr);
    // Keys of a card's answer segments / question segments from `items` (empty when unknown).
    QStringList cardAnchors(const QString &cardId) const;
    QStringList cardQuestionAnchors(const QString &cardId) const;
    QStringList commentAnchors(const QString &commentId) const;
};

// What the HTML view can show for a PDF version.
struct Availability {
    enum Kind { Ready, Building, NotDownloaded, Unavailable, Unknown } kind = Unknown; // Unknown: ask the server
    QString reason;     // Unavailable
    QString editionSha; // Ready / NotDownloaded
};

class Store
{
public:
    explicit Store(const QString &root);
    static QString defaultRoot(); // <AppDataLocation>/thoughts-server/html-editions
    QString root() const
    {
        return m_root;
    }

    QString editionDir(const QString &editionSha) const;
    bool isInstalled(const QString &editionSha) const;
    std::optional<Manifest> manifest(const QString &editionSha) const;

    std::optional<State> record(const QString &pdfSha) const;
    QDateTime checkedAt(const QString &pdfSha) const;
    bool writeRecord(const QString &pdfSha, const QString &filename, const State &state);
    // Records the state a stored Library listing gives for a PDF version. The listing's
    // html_* fields are hints the server keeps out of library_revision, so they can be older
    // than a record from GET /v1/html-editions: a final record is kept, and "none" or
    // "pending" never replace a record. Returns whether the record changed.
    bool adoptListed(const QString &pdfSha, const QString &filename, const State &listed);

    // Installs an edition from its objects: `fetch(sha)` returns the object's bytes (or an empty
    // optional with *error set). Every object is verified against its SHA-256.
    using Fetch = std::function<std::optional<QByteArray>(const QString &sha, QString *error)>;
    bool install(const QString &editionSha, const Fetch &fetch, QString *error);
    // Installs from bytes already fetched (manifest + files by sha), the async path.
    bool installFromObjects(const QString &editionSha, const QByteArray &manifestBytes, const QHash<QString, QByteArray> &objects, QString *error);

    Availability availability(const QString &pdfSha, bool hasSource) const;

    // Deletes editions not named by any record and records of PDFs not in `keepPdfShas`.
    void retain(const QSet<QString> &keepPdfShas);

private:
    QString m_root;
};
}

#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace Thoughts::HtmlEdition
{
// What the Library says about one opened PDF (current_pdfs/.thoughts-library-index.json and the
// generation manifest next to it).
struct LibraryInfo {
    bool inLibrary = false;
    bool projectReading = false;
    bool sourceLinked = false;
    QString filename;
    QString pdfSha256;
    QString documentId;
    std::optional<State> listed; // html_* of the generation's publication, when it lists them
};
LibraryInfo libraryInfo(const QString &pdfPath);

// Server requests for editions: GET /v1/html-editions/{pdf_sha256} and /v1/objects/{sha256}
// (Bearer token as FedoraRepository). Installs into a Store. One request chain per edition.
class Fetcher : public QObject
{
    Q_OBJECT
public:
    explicit Fetcher(Store *store, QObject *parent = nullptr);
    // Defaults: THOUGHTS_TOKEN or the desktop device secret, ServerAddress::configured(), read
    // again after ServerAddress::connectionChanged().
    void setServer(const QUrl &baseUrl, const QByteArray &token);
    // Asks the server for the PDF's edition state and records it; `done(state, error)`.
    void checkState(const QString &pdfSha, const QString &filename, QObject *context, std::function<void(std::optional<State>, QString)> done);
    // Downloads and installs an edition; `done(ok, error)`.
    void install(const QString &editionSha, QObject *context, std::function<void(bool, QString)> done);

private:
    bool ensureServer(QString *error);
    QNetworkReply *get(const QString &path);
    Store *m_store;
    QNetworkAccessManager *m_network = nullptr;
    QUrl m_baseUrl;
    QByteArray m_token;
    bool m_serverSet = false;
    // setServer() gave the server (tests): it is kept whatever happens.
    bool m_serverGiven = false;
    quint64 m_serverGeneration = 0;
    QHash<QString, QList<std::pair<QPointer<QObject>, std::function<void(bool, QString)>>>> m_installs;
};
}
