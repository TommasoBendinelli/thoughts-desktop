// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmledition.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include "thoughtsplatform.h"
#include "thoughtsserveraddress.h"

namespace Thoughts::HtmlEdition
{
namespace
{
constexpr qsizetype MaxManifestBytes = 4 * 1024 * 1024;
constexpr qsizetype MaxFileBytes = 64 * 1024 * 1024;
constexpr qsizetype MaxEditionBytes = 256 * 1024 * 1024;
constexpr int MaxFiles = 5000;

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QStringList stringList(const QJsonValue &value)
{
    QStringList out;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
        if (item.isString() && !item.toString().isEmpty() && !out.contains(item.toString())) {
            out.append(item.toString());
        }
    }
    return out;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}

bool isSha256(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return re.match(text).hasMatch();
}

bool isValidPath(const QString &path)
{
    if (path.isEmpty() || path.size() > 512 || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))) {
        return false;
    }
    for (const QChar c : path) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            return false;
        }
    }
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return false;
        }
    }
    return true;
}

std::optional<State> State::fromListedFields(const QJsonObject &fields)
{
    const QJsonObject source = fields.contains(QStringLiteral("html_status")) || !fields.value(QStringLiteral("html")).isObject() ? fields : fields.value(QStringLiteral("html")).toObject();
    if (!source.contains(QStringLiteral("html_status"))) {
        return std::nullopt;
    }
    return fromFields(source);
}

State State::fromFields(const QJsonObject &input)
{
    const QJsonObject fields = input.contains(QStringLiteral("html_status")) || !input.value(QStringLiteral("html")).isObject() ? input : input.value(QStringLiteral("html")).toObject();
    State state;
    const QString status = fields.value(QStringLiteral("html_status")).toString();
    if (status == QLatin1String("ready")) {
        const QString sha = fields.value(QStringLiteral("html_edition_sha256")).toString().toLower();
        if (isSha256(sha)) {
            state.status = Status::Ready;
            state.editionSha256 = sha;
        } else {
            state.status = Status::Failed;
            state.error = QStringLiteral("the server named no valid HTML edition");
        }
    } else if (status == QLatin1String("pending") || status == QLatin1String("running")) {
        state.status = Status::Pending;
    } else if (status == QLatin1String("failed")) {
        state.status = Status::Failed;
        state.error = fields.value(QStringLiteral("html_error")).toString().trimmed().left(500);
    }
    return state;
}

QString State::statusName() const
{
    switch (status) {
    case Status::Ready:
        return QStringLiteral("ready");
    case Status::Pending:
        return QStringLiteral("pending");
    case Status::Failed:
        return QStringLiteral("failed");
    case Status::None:
        break;
    }
    return QString();
}

std::optional<Manifest> Manifest::parse(const QByteArray &bytes, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Manifest> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    if (bytes.size() > MaxManifestBytes) {
        return fail(QStringLiteral("the edition manifest is too large"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        return fail(QStringLiteral("the edition manifest is not JSON"));
    }
    Manifest manifest;
    manifest.raw = document.object();
    if (manifest.raw.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts_html_edition") || manifest.raw.value(QStringLiteral("schema")).toInt() != 1) {
        return fail(QStringLiteral("not an HTML edition manifest (schema 1)"));
    }
    const QJsonObject files = manifest.raw.value(QStringLiteral("files")).toObject();
    if (files.isEmpty() || files.size() > MaxFiles) {
        return fail(QStringLiteral("the edition lists no files"));
    }
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QString sha = it.value().toString().toLower();
        if (!isValidPath(it.key()) || !isSha256(sha) || it.key() == QLatin1String("manifest.json")) {
            return fail(QStringLiteral("the edition names an invalid file: %1").arg(it.key()));
        }
        manifest.files.insert(it.key(), sha);
    }
    manifest.entry = manifest.raw.value(QStringLiteral("entry")).toString(QStringLiteral("index.html"));
    if (!manifest.files.contains(manifest.entry) || !manifest.entry.endsWith(QLatin1String(".html"))) {
        return fail(QStringLiteral("the edition has no entry page"));
    }
    const QStringList anchors = stringList(manifest.raw.value(QStringLiteral("anchors")));
    manifest.anchors = QSet<QString>(anchors.begin(), anchors.end());
    manifest.items = manifest.raw.value(QStringLiteral("items")).toObject();
    manifest.sourceFiles = manifest.raw.value(QStringLiteral("source_files")).toObject();
    manifest.sourceMap = manifest.raw.value(QStringLiteral("source_map")).toString(QStringLiteral("none"));
    manifest.sourceFormat = manifest.raw.value(QStringLiteral("source_format")).toString();
    manifest.pdfSha256 = manifest.raw.value(QStringLiteral("pdf_sha256")).toString().toLower();
    manifest.documentId = manifest.raw.value(QStringLiteral("document_id")).toString();
    return manifest;
}

QStringList Manifest::cardAnchors(const QString &cardId) const
{
    return stringList(items.value(QStringLiteral("cards")).toObject().value(cardId).toObject().value(QStringLiteral("anchors")));
}

QStringList Manifest::cardQuestionAnchors(const QString &cardId) const
{
    return stringList(items.value(QStringLiteral("cards")).toObject().value(cardId).toObject().value(QStringLiteral("question_anchors")));
}

QStringList Manifest::commentAnchors(const QString &commentId) const
{
    return stringList(items.value(QStringLiteral("comments")).toObject().value(commentId));
}

Store::Store(const QString &root)
    : m_root(root)
{
}

QString Store::defaultRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/thoughts-server/html-editions");
}

QString Store::editionDir(const QString &editionSha) const
{
    return m_root + QStringLiteral("/editions/") + editionSha.toLower();
}

std::optional<Manifest> Store::manifest(const QString &editionSha) const
{
    if (!isSha256(editionSha)) {
        return std::nullopt;
    }
    QFile file(editionDir(editionSha) + QStringLiteral("/manifest.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return Manifest::parse(file.readAll());
}

bool Store::isInstalled(const QString &editionSha) const
{
    const auto parsed = manifest(editionSha);
    if (!parsed) {
        return false;
    }
    const QString dir = editionDir(editionSha);
    for (auto it = parsed->files.cbegin(); it != parsed->files.cend(); ++it) {
        if (!QFileInfo(dir + QLatin1Char('/') + it.key()).isFile()) {
            return false;
        }
    }
    return true;
}

std::optional<State> Store::record(const QString &pdfSha) const
{
    if (!isSha256(pdfSha)) {
        return std::nullopt;
    }
    QFile file(m_root + QStringLiteral("/pdf/") + pdfSha.toLower() + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return State::fromFields(QJsonDocument::fromJson(file.readAll()).object());
}

QDateTime Store::checkedAt(const QString &pdfSha) const
{
    QFile file(m_root + QStringLiteral("/pdf/") + pdfSha.toLower() + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QDateTime::fromString(QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("checked_at")).toString(), Qt::ISODateWithMs);
}

bool Store::adoptListed(const QString &pdfSha, const QString &filename, const State &listed)
{
    if (const auto recorded = record(pdfSha)) {
        if (*recorded == listed || recorded->isFinal() || listed.status == Status::None
            || (listed.status == Status::Pending && recorded->status == Status::Pending)) {
            return false;
        }
    }
    return writeRecord(pdfSha, filename, listed);
}

bool Store::writeRecord(const QString &pdfSha, const QString &filename, const State &state)
{
    if (!isSha256(pdfSha)) {
        return false;
    }
    QJsonObject record{{QStringLiteral("pdf_sha256"), pdfSha.toLower()},
                       {QStringLiteral("filename"), filename},
                       {QStringLiteral("html_status"), state.status == Status::None ? QJsonValue() : QJsonValue(state.statusName())},
                       {QStringLiteral("checked_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if (state.status == Status::Ready) {
        record.insert(QStringLiteral("html_edition_sha256"), state.editionSha256);
    }
    if (state.status == Status::Failed) {
        record.insert(QStringLiteral("html_error"), state.error);
    }
    return writeFile(m_root + QStringLiteral("/pdf/") + pdfSha.toLower() + QStringLiteral(".json"), QJsonDocument(record).toJson(QJsonDocument::Compact));
}

bool Store::install(const QString &editionSha, const Fetch &fetch, QString *error)
{
    if (isInstalled(editionSha)) {
        return true;
    }
    QString fetchError;
    const auto manifestBytes = fetch(editionSha.toLower(), &fetchError);
    if (!manifestBytes) {
        if (error) {
            *error = fetchError;
        }
        return false;
    }
    QString parseError;
    const auto parsed = Manifest::parse(*manifestBytes, &parseError);
    if (!parsed) {
        if (error) {
            *error = parseError;
        }
        return false;
    }
    QHash<QString, QByteArray> objects;
    for (const QString &sha : parsed->files) {
        if (objects.contains(sha)) {
            continue;
        }
        const auto bytes = fetch(sha, &fetchError);
        if (!bytes) {
            if (error) {
                *error = fetchError;
            }
            return false;
        }
        objects.insert(sha, *bytes);
    }
    return installFromObjects(editionSha, *manifestBytes, objects, error);
}

bool Store::installFromObjects(const QString &editionSha, const QByteArray &manifestBytes, const QHash<QString, QByteArray> &objects, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!isSha256(editionSha) || sha256Hex(manifestBytes) != editionSha.toLower()) {
        return fail(QStringLiteral("the edition manifest failed verification"));
    }
    QString parseError;
    const auto parsed = Manifest::parse(manifestBytes, &parseError);
    if (!parsed) {
        return fail(parseError);
    }
    if (isInstalled(editionSha)) {
        return true;
    }
    const QString tmp = m_root + QStringLiteral("/editions/.tmp-") + QUuid::createUuid().toString(QUuid::Id128);
    qsizetype total = 0;
    for (auto it = parsed->files.cbegin(); it != parsed->files.cend(); ++it) {
        const QByteArray bytes = objects.value(it.value());
        total += bytes.size();
        if (bytes.size() > MaxFileBytes || total > MaxEditionBytes) {
            QDir(tmp).removeRecursively();
            return fail(QStringLiteral("the edition is too large"));
        }
        if (sha256Hex(bytes) != it.value()) {
            QDir(tmp).removeRecursively();
            return fail(QStringLiteral("%1 failed verification").arg(it.key()));
        }
        if (!writeFile(tmp + QLatin1Char('/') + it.key(), bytes)) {
            QDir(tmp).removeRecursively();
            return fail(QStringLiteral("could not write the edition"));
        }
    }
    if (!writeFile(tmp + QStringLiteral("/manifest.json"), manifestBytes)) {
        QDir(tmp).removeRecursively();
        return fail(QStringLiteral("could not write the edition"));
    }
    const QString target = editionDir(editionSha);
    QDir(target).removeRecursively();
    if (!QDir().rename(tmp, target)) {
        QDir(tmp).removeRecursively();
        return fail(QStringLiteral("could not write the edition"));
    }
    return true;
}

Availability Store::availability(const QString &pdfSha, bool hasSource) const
{
    Availability result;
    if (!hasSource) {
        result.kind = Availability::Unavailable;
        result.reason = QStringLiteral("imported PDFs have no HTML edition");
        return result;
    }
    if (!isSha256(pdfSha)) {
        result.kind = Availability::Unavailable;
        result.reason = QStringLiteral("this PDF is not a Library version");
        return result;
    }
    const auto state = record(pdfSha);
    if (!state) {
        result.kind = Availability::Unknown;
        return result;
    }
    switch (state->status) {
    case Status::Ready:
        result.editionSha = state->editionSha256;
        result.kind = isInstalled(state->editionSha256) ? Availability::Ready : Availability::NotDownloaded;
        break;
    case Status::Pending:
        result.kind = Availability::Building;
        break;
    case Status::Failed:
        result.kind = Availability::Unavailable;
        result.reason = state->error.isEmpty() ? QStringLiteral("the conversion failed") : state->error;
        break;
    case Status::None:
        result.kind = Availability::Unavailable;
        result.reason = QStringLiteral("the server has no HTML edition of this version");
        break;
    }
    return result;
}

void Store::retain(const QSet<QString> &keepPdfShas)
{
    QSet<QString> keepEditions;
    QDir pdfDir(m_root + QStringLiteral("/pdf"));
    const QStringList records = pdfDir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &name : records) {
        const QString pdfSha = name.chopped(5);
        if (!keepPdfShas.contains(pdfSha)) {
            pdfDir.remove(name);
            continue;
        }
        const auto state = record(pdfSha);
        if (state && state->status == Status::Ready) {
            keepEditions.insert(state->editionSha256);
        }
    }
    QDir editions(m_root + QStringLiteral("/editions"));
    const QStringList dirs = editions.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString &name : dirs) {
        if (!keepEditions.contains(name)) {
            QDir(editions.filePath(name)).removeRecursively();
        }
    }
}
}

namespace Thoughts::HtmlEdition
{
LibraryInfo libraryInfo(const QString &pdfPath)
{
    LibraryInfo info;
    const QFileInfo file(pdfPath);
    info.filename = file.fileName();
    if (file.suffix() == QLatin1String("thoughts-project")) {
        info.projectReading = true;
        return info;
    }
    const QString canonical = file.canonicalFilePath();
    const QDir dir = QFileInfo(canonical.isEmpty() ? file.absoluteFilePath() : canonical).absoluteDir();
    QFile indexFile(dir.filePath(QStringLiteral(".thoughts-library-index.json")));
    if (!indexFile.open(QIODevice::ReadOnly)) {
        return info;
    }
    const QJsonObject index = QJsonDocument::fromJson(indexFile.readAll()).object();
    for (const QJsonValue &value : index.value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("filename")).toString() != info.filename) {
            continue;
        }
        info.inLibrary = true;
        info.pdfSha256 = entry.value(QStringLiteral("pdf_sha256")).toString().toLower();
        info.documentId = entry.value(QStringLiteral("document_id")).toString();
        info.sourceLinked = entry.value(QStringLiteral("kind")).toString() == QLatin1String("source_linked");
        if (const auto listed = State::fromListedFields(entry)) {
            info.listed = listed;
        }
        break;
    }
    if (!info.inLibrary) {
        return info;
    }
    QFile generationFile(dir.filePath(QStringLiteral(".thoughts-generation.json")));
    if (generationFile.open(QIODevice::ReadOnly)) {
        const QJsonObject generation = QJsonDocument::fromJson(generationFile.readAll()).object();
        for (const QJsonValue &value : generation.value(QStringLiteral("publications")).toArray()) {
            const QJsonObject publication = value.toObject();
            if (publication.value(QStringLiteral("filename")).toString() == info.filename
                && publication.value(QStringLiteral("sha256")).toString().toLower() == info.pdfSha256) {
                if (const auto listed = State::fromListedFields(publication)) {
                    info.listed = listed;
                }
                break;
            }
        }
    }
    return info;
}

Fetcher::Fetcher(Store *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
{
}

void Fetcher::setServer(const QUrl &baseUrl, const QByteArray &token)
{
    m_baseUrl = baseUrl;
    m_token = token;
    m_serverSet = true;
    m_serverGiven = true;
}

bool Fetcher::ensureServer(QString *error)
{
    if (m_serverSet && !m_serverGiven && m_serverGeneration != ServerAddress::connectionGeneration()) {
        // Signed in or out since: read the address and the token again.
        m_serverSet = false;
    }
    if (!m_serverSet) {
        m_serverGeneration = ServerAddress::connectionGeneration();
        const QString address = ServerAddress::configured();
        m_token = qgetenv("THOUGHTS_TOKEN").trimmed();
        if (m_token.isEmpty()) {
            m_token = ServerAddress::storedToken(Platform::desktopDeviceAccount(), address);
        }
        m_baseUrl = QUrl(ServerAddress::normalized(address));
        m_serverSet = true;
    }
    if (m_token.isEmpty() || !m_baseUrl.isValid() || m_baseUrl.isEmpty()) {
        if (error) {
            *error = QStringLiteral("this computer is not connected to a Thoughts server");
        }
        return false;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    return true;
}

QNetworkReply *Fetcher::get(const QString &path)
{
    QUrl url = m_baseUrl;
    QString base = url.path();
    if (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    url.setPath(base + path);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token);
    // The token is this server's: a redirect to another origin is not followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(60000);
    return m_network->get(request);
}

void Fetcher::checkState(const QString &pdfSha, const QString &filename, QObject *context, std::function<void(std::optional<State>, QString)> done)
{
    QString error;
    if (!isSha256(pdfSha)) {
        done(std::nullopt, QStringLiteral("this PDF is not a Library version"));
        return;
    }
    if (!ensureServer(&error)) {
        done(std::nullopt, error);
        return;
    }
    QNetworkReply *reply = get(QStringLiteral("/v1/html-editions/") + pdfSha.toLower());
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [this, reply, pdfSha, filename, guard, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        std::optional<State> state;
        QString error;
        if (status == 404) {
            state = State{};
        } else if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            error = status ? QStringLiteral("the server answered %1").arg(status) : reply->errorString();
        } else {
            const QByteArray body = reply->read(8 * 1024 * 1024 + 1);
            const QJsonObject object = QJsonDocument::fromJson(body).object();
            if (object.isEmpty()) {
                error = QStringLiteral("the server sent no edition state");
            } else {
                state = State::fromFields(object);
            }
        }
        if (state) {
            m_store->writeRecord(pdfSha, filename, *state);
        }
        if (guard) {
            done(state, error);
        }
    });
}

void Fetcher::install(const QString &editionSha, QObject *context, std::function<void(bool, QString)> done)
{
    const QString sha = editionSha.toLower();
    if (m_store->isInstalled(sha)) {
        done(true, QString());
        return;
    }
    QString error;
    if (!ensureServer(&error)) {
        done(false, error);
        return;
    }
    const bool running = m_installs.contains(sha);
    m_installs[sha].append({QPointer<QObject>(context), std::move(done)});
    if (running) {
        return;
    }
    struct Job {
        QByteArray manifest;
        QHash<QString, QByteArray> objects;
        int pending = 0;
        QString error;
    };
    auto job = std::make_shared<Job>();
    auto finish = [this, sha, job] {
        bool ok = job->error.isEmpty();
        QString error = job->error;
        if (ok) {
            ok = m_store->installFromObjects(sha, job->manifest, job->objects, &error);
        }
        const auto waiting = m_installs.take(sha);
        for (const auto &[guard, callback] : waiting) {
            if (guard) {
                callback(ok, error);
            }
        }
    };
    auto fetchObject = [this, job](const QString &objectSha, std::function<void(QByteArray)> then) {
        QNetworkReply *reply = get(QStringLiteral("/v1/objects/") + objectSha);
        connect(reply, &QNetworkReply::finished, this, [reply, job, then] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                if (job->error.isEmpty()) {
                    job->error = status ? QStringLiteral("downloading the edition failed (%1)").arg(status) : reply->errorString();
                }
                then(QByteArray());
                return;
            }
            then(reply->readAll());
        });
    };
    fetchObject(sha, [job, sha, fetchObject, finish](const QByteArray &manifestBytes) {
        if (!job->error.isEmpty()) {
            finish();
            return;
        }
        job->manifest = manifestBytes;
        QString parseError;
        const auto manifest = Manifest::parse(manifestBytes, &parseError);
        if (!manifest) {
            job->error = parseError;
            finish();
            return;
        }
        QSet<QString> shas;
        for (const QString &fileSha : manifest->files) {
            shas.insert(fileSha);
        }
        job->pending = int(shas.size());
        for (const QString &fileSha : std::as_const(shas)) {
            fetchObject(fileSha, [job, fileSha, finish](const QByteArray &bytes) {
                if (job->error.isEmpty()) {
                    job->objects.insert(fileSha, bytes);
                }
                if (--job->pending == 0) {
                    finish();
                }
            });
        }
    });
}
}
