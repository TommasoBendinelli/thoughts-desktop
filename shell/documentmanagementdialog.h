// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../part/documentbuildselection.h"
#include "../part/libraryprojectstore.h"
#include <KLocalizedString>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace Thoughts {

// Reserve a separate hit target for dragging, away from the inclusion checkbox.
class DocumentHandleDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static QRect contentRect(QRect rect) { return rect.adjusted(24, 0, 0, 0); }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto content = option;
        content.rect = contentRect(option.rect);
        QStyledItemDelegate::paint(painter, content, index);
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.palette.color(QPalette::Text));
        for (int x : {9, 14}) for (int y : {-4, 0, 4})
            painter->drawEllipse(QPointF(option.rect.left() + x, option.rect.center().y() + y), 1.2, 1.2);
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto size = QStyledItemDelegate::sizeHint(option, index);
        return QSize(size.width() + 24, qMax(32, size.height()));
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override {
        auto content = option;
        content.rect = contentRect(option.rect);
        return QStyledItemDelegate::editorEvent(event, model, content, index);
    }
};

class DocumentManagementList : public QListWidget {
public:
    explicit DocumentManagementList(QWidget *parent) : QListWidget(parent) {
        setItemDelegate(new DocumentHandleDelegate(this));
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
        setDragDropOverwriteMode(false);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }
protected:
    void mousePressEvent(QMouseEvent *event) override {
        const auto rect = visualItemRect(itemAt(event->position().toPoint()));
        m_handlePressed = event->button() == Qt::LeftButton && rect.contains(event->position().toPoint())
            && event->position().x() < rect.left() + 24;
        QListWidget::mousePressEvent(event);
    }
    void startDrag(Qt::DropActions actions) override {
        if (m_handlePressed) QListWidget::startDrag(actions);
    }
private:
    bool m_handlePressed = false;
};

// Keep the exact original bytes so a failed Apply can restore all touched files.
class DocumentSettingsTransaction {
public:
    QString watch(const QString &path) {
        QFile file(path);
        if (QFileInfo(path).isSymLink() || (file.exists() && !file.open(QIODevice::ReadOnly)))
            return i18n("Cannot read %1.", path);
        m_original.insert(path, {file.exists(), file.isOpen() ? file.readAll() : QByteArray()});
        return {};
    }
    QString commit(const QMap<QString, QJsonObject> &changes) const {
        for (auto it = m_original.cbegin(); it != m_original.cend(); ++it) {
            QFile file(it.key());
            if (QFileInfo(it.key()).isSymLink() || file.exists() != it->exists
                || (it->exists && (!file.open(QIODevice::ReadOnly) || file.readAll() != it->bytes)))
                return i18n("The document settings changed. Cancel and reopen Manage documents.");
        }
        QStringList written;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            if (!m_original.contains(it.key())) return i18n("The document settings changed. Cancel and reopen Manage documents.");
        }
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            const auto error = DocumentBuildSelection::write(it.key(), it.value());
            if (error.isEmpty()) { written.prepend(it.key()); continue; }
            QStringList failed;
            for (const auto &path : written) {
                const auto original = m_original.value(path);
                if (!original.exists) { if (!QFile::remove(path)) failed.append(path); continue; }
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(original.bytes) != original.bytes.size() || !file.commit()) failed.append(path);
            }
            return failed.isEmpty() ? error : error + QLatin1Char('\n') + i18n("Could not restore: %1", failed.join(QStringLiteral(", ")));
        }
        return {};
    }
private:
    struct Original { bool exists; QByteArray bytes; };
    QMap<QString, Original> m_original;
};

class DocumentManagementDialog : public QDialog {
public:
    DocumentManagementDialog(const QString &root, const QJsonObject &project, QWidget *parent = nullptr)
        : QDialog(parent), m_root(root), m_project(project), m_owners(DocumentBuildSelection::owners(project)) {
        setObjectName(QStringLiteral("thoughtsManageDocumentsDialog"));
        setWindowTitle(i18n("Manage documents"));
        resize(720, 480);
        auto *layout = new QVBoxLayout(this);
        auto *hint = new QLabel(m_owners.isEmpty() ? i18n("Drag the handles to arrange the document order. Apply saves the order. Sync updates the project on other devices.") : i18n("Choose which documents to build and drag their handles to arrange the order. Apply saves both changes. Sync builds the updated PDF."), this);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        m_list = new DocumentManagementList(this);
        m_list->setObjectName(QStringLiteral("manageDocumentsList"));
        layout->addWidget(m_list);
        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        layout->addWidget(m_status);
        auto *moves = new QHBoxLayout;
        for (const auto &move : {qMakePair(i18n("Move up"), -1), qMakePair(i18n("Move down"), 1)}) {
            auto *button = new QPushButton(move.first, this);
            button->setObjectName(move.second < 0 ? QStringLiteral("documentOrderUp") : QStringLiteral("documentOrderDown"));
            button->setAutoDefault(false);
            moves->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, offset = move.second] {
                const int row = m_list->currentRow();
                if (row < 0 || row + offset < 0 || row + offset >= m_list->count()) return;
                auto *item = m_list->takeItem(row);
                m_list->insertItem(row + offset, item);
                m_list->setCurrentItem(item);
            });
        }
        moves->addStretch();
        layout->addLayout(moves);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        m_apply = buttons->button(QDialogButtonBox::Apply);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_apply, &QPushButton::clicked, this, [this] { apply(); });
        load(project);
        connect(m_list, &QListWidget::itemChanged, this, [this] { validate(); });
        validate();
    }
    // Ticks and selects the row of the source file @p absolutePath (Add as Document…); false when the
    // folder offers no such document (not a complete LaTeX or Markdown document).
    bool selectSource(const QString &absolutePath) {
        const QString wanted = QFileInfo(absolutePath).canonicalFilePath();
        for (int i = 0; !wanted.isEmpty() && i < m_list->count(); ++i) {
            auto *row = m_list->item(i);
            const auto entry = row->data(Qt::UserRole).toJsonObject();
            // A folder's candidate names its owner and source; a Project configuration's row has the
            // document's Library path (its alias) as its tooltip.
            const QString owner = entry.value(QStringLiteral("owner")).toString();
            const QJsonValue source = entry.value(QStringLiteral("source"));
            const QString relative = !owner.isEmpty() && source.isString() ? owner + QLatin1Char('/') + source.toString()
                                                                           : row->toolTip().section(QLatin1Char('\n'), 0, 0);
            if (relative.isEmpty() || QFileInfo(QDir(m_root).filePath(relative)).canonicalFilePath() != wanted) continue;
            if (row->flags() & Qt::ItemIsUserCheckable) row->setCheckState(Qt::Checked);
            m_list->setCurrentItem(row);
            m_list->scrollToItem(row);
            return true;
        }
        return false;
    }
private:
    static constexpr int RemovedRole = Qt::UserRole + 1;
    static QString orderPath(const QString &root, const QString &owner) {
        return QDir(root).filePath(owner + QStringLiteral("/tex/support/thoughts_document_order.txt"));
    }
    void loadConfiguration() {
        m_error = m_transaction.watch(m_configurationPath);
        if (!m_error.isEmpty()) return;
        m_configuration = ProjectConfiguration::read(m_configurationPath, &m_error);
        if (!m_error.isEmpty()) return;
        // The default folder declares Markdown and LaTeX documents like a connected folder.
        const auto defaultOwner = DocumentBuildSelection::defaultFolder(m_configuration);
        if (!defaultOwner.isEmpty() && !m_owners.contains(defaultOwner)) m_owners.append(defaultOwner);
        QSet<QString> declared;
        const auto append = [&](QJsonObject document, const QString &label) {
            const auto alias = ProjectConfiguration::documentAlias(m_configuration, document);
            declared.insert(alias);
            auto *row = new QListWidgetItem(label, m_list);
            row->setData(Qt::UserRole, document);
            row->setToolTip(alias);
            row->setCheckState(document.value(QStringLiteral("included")).toBool() ? Qt::Checked : Qt::Unchecked);
        };
        for (const auto &raw : m_configuration.value(QStringLiteral("documents")).toArray()) {
            const auto document = raw.toObject();
            const auto id = document.value(QStringLiteral("document_id")).toString();
            const auto source = document.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString();
            append(document, m_configuration.value(QStringLiteral("document_names")).toObject().value(id).toString(QFileInfo(source).completeBaseName()));
        }
        auto rendering = m_configuration.value(QStringLiteral("rendering")).toObject();
        const auto removed = m_project.value(QStringLiteral("removed_identities")).toObject();
        QSet<QString> identities;
        for (const auto &raw : m_configuration.value(QStringLiteral("documents")).toArray()) identities.insert(raw.toObject().value(QStringLiteral("document_id")).toString());
        for (const auto &owner : m_owners) {
            const auto entries = DocumentBuildSelection::candidates(m_root, owner, &m_error);
            if (!m_error.isEmpty()) return;
            m_candidates.insert(owner, entries);
            QString folderId;
            for (const auto &raw : m_configuration.value(QStringLiteral("source_folders")).toArray())
                if (raw.toObject().value(QStringLiteral("path")).toString() == owner) folderId = raw.toObject().value(QStringLiteral("folder_id")).toString();
            if (folderId.isEmpty()) { m_error = i18n("The connected source folder has no identity."); return; }
            for (const auto &raw : entries) {
                const auto entry = raw.toObject();
                if (declared.contains(entry.value(QStringLiteral("id")).toString())) continue;
                // A removed document keeps its identity: Fedora refuses its PDF name for any other.
                auto identifier = removed.value(entry.value(QStringLiteral("id")).toString()).toString();
                if (!DocumentIdentityStore::validId(identifier) || identities.contains(identifier)) identifier = QUuid::createUuid().toString(QUuid::WithoutBraces);
                identities.insert(identifier);
                QJsonObject options;
                for (const auto &key : {QStringLiteral("output"), QStringLiteral("stylesheet"), QStringLiteral("working_directory")})
                    if (entry.contains(key)) options.insert(key, entry.value(key));
                if (!options.isEmpty()) rendering.insert(identifier, options);
                const auto source = entry.value(QStringLiteral("source")).toString();
                append({{QStringLiteral("document_id"), identifier}, {QStringLiteral("format"), entry.value(QStringLiteral("format"))},
                    {QStringLiteral("included"), false}, {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), source}}}},
                    entry.value(QStringLiteral("title")).toString(QFileInfo(source).completeBaseName()));
                if (identifier == removed.value(entry.value(QStringLiteral("id")).toString()).toString()) m_list->item(m_list->count() - 1)->setData(RemovedRole, true);
            }
        }
        m_configuration.insert(QStringLiteral("rendering"), rendering);
        if (m_list->count()) m_list->setCurrentRow(0);
    }
    void load(const QJsonObject &project) {
        m_configurationPath = ProjectConfiguration::path(m_root, project.value(QStringLiteral("id")).toString());
        if (QFileInfo::exists(m_configurationPath)) { loadConfiguration(); return; }
        m_configurationPath.clear();
        // Pre-v5 settings exist only in connected folders.
        m_owners.removeIf([](const QString &owner) { return !SourceOwner::isConnectedFolder(owner); });
        QMap<QString, QListWidgetItem *> rows;
        QMap<QString, QString> documentNames;
        QStringList initialOrder;
        for (const auto &value : project.value(QStringLiteral("documents")).toArray()) {
            const auto document = value.toObject();
            const auto id = document.value(QStringLiteral("id")).toString();
            initialOrder.append(id);
            const auto name = document.value(QStringLiteral("name")).toString().trimmed();
            if (!id.isEmpty() && !name.isEmpty()) documentNames.insert(id, name);
        }
        QStringList savedOrder;
        QStringList legacyOrder;
        for (const auto &owner : m_owners) {
            QFile legacy(QDir(m_root).filePath(owner + QStringLiteral("/tex/docs_order_manifest.txt")));
            if (legacy.open(QIODevice::ReadOnly)) {
                for (const auto &line : QString::fromUtf8(legacy.readAll()).split(QLatin1Char('\n'))) {
                    const auto name = line.trimmed();
                    if (!name.isEmpty() && !name.startsWith(QLatin1Char('#')))
                        legacyOrder.append(owner + QStringLiteral("/tex/source/") + name);
                }
            }
            for (const auto &path : {DocumentBuildSelection::optionsPath(m_root, owner), orderPath(m_root, owner), QDir(m_root).filePath(owner + QStringLiteral("/thoughts-project.json"))}) {
                m_error = m_transaction.watch(path);
                if (!m_error.isEmpty()) return;
            }
            const auto order = DocumentBuildSelection::read(orderPath(m_root, owner), &m_error);
            if (!m_error.isEmpty()) return;
            if (savedOrder.isEmpty()) for (const auto &id : order.value(QStringLiteral("documents")).toArray()) savedOrder.append(id.toString());
            const auto options = DocumentBuildSelection::read(DocumentBuildSelection::optionsPath(m_root, owner), &m_error);
            if (!m_error.isEmpty()) return;
            const auto entries = DocumentBuildSelection::candidates(m_root, owner, &m_error);
            if (!m_error.isEmpty()) return;
            m_candidates.insert(owner, entries);
            for (const auto &value : entries) {
                const auto entry = value.toObject();
                const auto source = entry.value(QStringLiteral("source")).toString();
                const auto id = entry.value(QStringLiteral("id")).toString();
                const auto title = entry.value(QStringLiteral("title")).toString(QFileInfo(source).completeBaseName());
                const auto label = documentNames.value(id, m_owners.size() > 1 ? owner + QStringLiteral(" / ") + title : title);
                auto *row = new QListWidgetItem(label, m_list);
                row->setToolTip(source + QLatin1Char('\n') + i18n("Use the checkbox to include this document; drag the handle to change its order."));
                row->setData(Qt::UserRole, entry);
                row->setCheckState(DocumentBuildSelection::selected(options, entry) ? Qt::Checked : Qt::Unchecked);
                rows.insert(entry.value(QStringLiteral("id")).toString(), row);
            }
        }
        // Imported PDFs can be ordered alongside source documents, without a build checkbox.
        for (const auto &value : project.value(QStringLiteral("documents")).toArray()) {
            const auto doc = value.toObject();
            const auto id = doc.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || rows.contains(id)) continue;
            // The assembled project PDF is an output, not another build input.
            if (!m_owners.isEmpty() && id == project.value(QStringLiteral("target")).toString() && !doc.value(QStringLiteral("imported")).toBool()) continue;
            auto *row = new QListWidgetItem(doc.value(QStringLiteral("name")).toString(id), m_list);
            row->setData(Qt::UserRole, QJsonObject{{QStringLiteral("id"), id}});
            row->setFlags(row->flags() & ~Qt::ItemIsUserCheckable);
            rows.insert(id, row);
        }
        int position = 0;
        QSet<QString> placed;
        for (const auto &id : savedOrder + initialOrder + legacyOrder) {
            if (!rows.contains(id) || placed.contains(id)) continue;
            auto *row = m_list->takeItem(m_list->row(rows.value(id)));
            m_list->insertItem(position++, row);
            placed.insert(id);
        }
        if (m_list->count()) m_list->setCurrentRow(0);
    }
    void validate() {
        QSet<QString> selectedOwners;
        for (int i = 0; i < m_list->count(); ++i) {
            const auto *row = m_list->item(i);
            if (row->checkState() == Qt::Checked) selectedOwners.insert(row->data(Qt::UserRole).toJsonObject().value(QStringLiteral("owner")).toString());
        }
        const bool valid = m_error.isEmpty() && (!m_configurationPath.isEmpty() || m_owners.isEmpty() || selectedOwners.size() == m_owners.size());
        m_apply->setEnabled(valid);
        m_status->setText(!m_error.isEmpty() ? m_error : valid ? QString() : i18n("Keep at least one document selected in each source folder."));
    }
    void apply() {
        if (!m_configurationPath.isEmpty()) {
            for (const auto &owner : m_owners) {
                QString error;
                if (DocumentBuildSelection::candidates(m_root, owner, &error) != m_candidates.value(owner) || !error.isEmpty()) {
                    m_status->setText(i18n("The source list changed. Cancel and reopen Manage documents.")); return;
                }
            }
            QJsonArray documents;
            auto rendering = m_configuration.value(QStringLiteral("rendering")).toObject();
            for (int i = 0; i < m_list->count(); ++i) {
                const auto *row = m_list->item(i);
                auto document = row->data(Qt::UserRole).toJsonObject();
                // A removed document returns only when checked: declared again, even
                // unchecked, its kept PDF would be live and a build target once more.
                if (row->data(RemovedRole).toBool() && row->checkState() != Qt::Checked) {
                    rendering.remove(document.value(QStringLiteral("document_id")).toString());
                    continue;
                }
                document.insert(QStringLiteral("included"), row->checkState() == Qt::Checked);
                documents.append(document);
            }
            auto value = m_configuration;
            value.insert(QStringLiteral("documents"), documents);
            value.insert(QStringLiteral("rendering"), rendering);
            const auto error = m_transaction.commit({{m_configurationPath, value}});
            if (!error.isEmpty()) { m_status->setText(error); return; }
            accept(); return;
        }
        QMap<QString, QJsonArray> selected;
        QJsonArray order;
        for (int i = 0; i < m_list->count(); ++i) {
            const auto *row = m_list->item(i);
            const auto entry = row->data(Qt::UserRole).toJsonObject();
            order.append(entry.value(QStringLiteral("id")));
            if (row->checkState() == Qt::Checked) selected[entry.value(QStringLiteral("owner")).toString()].append(entry.value(QStringLiteral("id")));
        }
        QMap<QString, QJsonObject> changes;
        if (m_owners.isEmpty()) {
            const auto error = LibraryProjectStore(m_root).enqueue(QStringLiteral("order"), m_project, {{QStringLiteral("documents"), order}});
            if (!error.isEmpty()) { m_status->setText(error); return; }
            accept();
            return;
        }
        for (const auto &owner : m_owners) {
            QString error;
            if (DocumentBuildSelection::candidates(m_root, owner, &error) != m_candidates.value(owner) || !error.isEmpty()) {
                m_status->setText(i18n("The source list changed. Cancel and reopen Manage documents.")); return;
            }
            error = DocumentBuildSelection::prepareSelection(m_root, owner, m_candidates.value(owner), selected.value(owner), changes);
            if (!error.isEmpty()) { m_status->setText(error); return; }
            const auto path = DocumentBuildSelection::manifestPath(m_root, owner);
            changes[path] = DocumentBuildSelection::reordered(changes.value(path), order);
        }
        const auto error = m_transaction.commit(changes);
        if (!error.isEmpty()) { m_status->setText(error); return; }
        accept();
    }
    QString m_root;
    QString m_configurationPath;
    QJsonObject m_configuration;
    QJsonObject m_project;
    QStringList m_owners;
    QString m_error;
    DocumentManagementList *m_list;
    QLabel *m_status;
    QPushButton *m_apply;
    QMap<QString, QJsonArray> m_candidates;
    DocumentSettingsTransaction m_transaction;
};
}
