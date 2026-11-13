// SPDX-License-Identifier: GPL-2.0-or-later
#include "pdflinkoverridedialog.h"

#include "documentidentity.h"
#include "libraryprojectstore.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace Thoughts
{
QList<LibraryDocumentChoice> libraryDocumentChoices(const QString &docsRoot, const QString &excludedDocumentId)
{
    QList<LibraryDocumentChoice> choices;
    QSet<QString> listed;
    const DocumentIdentityStore identities(docsRoot);
    LibraryProjectStore store(docsRoot);
    const QJsonArray projects = store.projects(false);
    for (const QJsonValue &projectValue : projects) {
        const QJsonObject project = projectValue.toObject();
        const QString projectName = project.value(QStringLiteral("name")).toString(project.value(QStringLiteral("id")).toString());
        for (const QJsonValue &documentValue : project.value(QStringLiteral("documents")).toArray()) {
            const QJsonObject document = documentValue.toObject();
            QString id = document.value(QStringLiteral("document_id")).toString();
            const QString filename = document.value(QStringLiteral("attachment_filename")).toString(document.value(QStringLiteral("filename")).toString());
            if (!DocumentIdentityStore::validId(id) && !filename.isEmpty()) id = identities.byFilename(filename).value(QStringLiteral("document_id")).toString();
            if (!DocumentIdentityStore::validId(id) || id == excludedDocumentId || listed.contains(id)) continue;
            QString name = document.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty()) name = QFileInfo(filename).completeBaseName();
            if (name.isEmpty()) name = document.value(QStringLiteral("id")).toString();
            listed.insert(id);
            choices.append({id, name, projectName});
        }
    }
    // Library PDFs that belong to no project (or whose project is not on this computer yet).
    QFile index(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
    if (index.open(QIODevice::ReadOnly)) {
        const QJsonArray pdfs = QJsonDocument::fromJson(index.readAll()).object().value(QStringLiteral("pdfs")).toArray();
        for (const QJsonValue &value : pdfs) {
            const QJsonObject pdf = value.toObject();
            const QString id = pdf.value(QStringLiteral("document_id")).toString();
            if (!DocumentIdentityStore::validId(id) || id == excludedDocumentId || listed.contains(id)) continue;
            QString name = pdf.value(QStringLiteral("display_name")).toString().trimmed();
            if (name.isEmpty()) name = QFileInfo(pdf.value(QStringLiteral("filename")).toString()).completeBaseName();
            listed.insert(id);
            choices.append({id, name, i18n("Other Library PDFs")});
        }
    }
    return choices;
}

PdfLinkOverrideDialog::PdfLinkOverrideDialog(const QList<LibraryDocumentChoice> &choices, const QString &currentDocumentId, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("pdfLinkOverrideDialog"));
    setWindowTitle(i18n("Link to Library Document"));
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(i18n("Clicking this link, and every other link of this PDF to the same place, will open:"), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("pdfLinkOverrideSearch"));
    m_search->setPlaceholderText(i18n("Search Library documents"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_documents = new QTreeWidget(this);
    m_documents->setObjectName(QStringLiteral("pdfLinkOverrideDocuments"));
    m_documents->setHeaderHidden(true);
    m_documents->setRootIsDecorated(true);
    layout->addWidget(m_documents, 1);
    QHash<QString, QTreeWidgetItem *> projects;
    QTreeWidgetItem *current = nullptr;
    for (const LibraryDocumentChoice &choice : choices) {
        QTreeWidgetItem *project = projects.value(choice.project);
        if (!project) {
            project = new QTreeWidgetItem(m_documents, {choice.project});
            project->setFlags(Qt::ItemIsEnabled);
            QFont font = project->font(0);
            font.setBold(true);
            project->setFont(0, font);
            project->setExpanded(true);
            projects.insert(choice.project, project);
        }
        auto *document = new QTreeWidgetItem(project, {choice.name});
        document->setData(0, Qt::UserRole, choice.documentId);
        document->setToolTip(0, choice.name);
        if (choice.documentId == currentDocumentId) current = document;
    }
    m_documents->expandAll();
    if (current) m_documents->setCurrentItem(current);
    if (choices.isEmpty()) {
        auto *empty = new QLabel(i18n("The Library has no other documents."), this);
        layout->addWidget(empty);
    }
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(i18n("Link"));
    layout->addWidget(m_buttons);
    connect(m_search, &QLineEdit::textChanged, this, &PdfLinkOverrideDialog::applyFilter);
    connect(m_documents, &QTreeWidget::currentItemChanged, this, &PdfLinkOverrideDialog::updateButtons);
    connect(m_documents, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty()) accept();
    });
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateButtons();
    m_search->setFocus();
    resize(480, 520);
}

QString PdfLinkOverrideDialog::selectedDocumentId() const
{
    const QTreeWidgetItem *item = m_documents->currentItem();
    return item && !item->isHidden() ? item->data(0, Qt::UserRole).toString() : QString();
}

void PdfLinkOverrideDialog::setFilter(const QString &text)
{
    m_search->setText(text);
}

void PdfLinkOverrideDialog::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    QTreeWidgetItem *firstVisible = nullptr;
    for (int index = 0; index < m_documents->topLevelItemCount(); ++index) {
        QTreeWidgetItem *project = m_documents->topLevelItem(index);
        const bool projectMatches = !needle.isEmpty() && project->text(0).contains(needle, Qt::CaseInsensitive);
        bool anyVisible = false;
        for (int child = 0; child < project->childCount(); ++child) {
            QTreeWidgetItem *document = project->child(child);
            const bool visible = needle.isEmpty() || projectMatches || document->text(0).contains(needle, Qt::CaseInsensitive);
            document->setHidden(!visible);
            anyVisible = anyVisible || visible;
            if (visible && !firstVisible) firstVisible = document;
        }
        project->setHidden(!anyVisible);
    }
    QTreeWidgetItem *current = m_documents->currentItem();
    if ((!current || current->isHidden() || current->data(0, Qt::UserRole).toString().isEmpty()) && firstVisible) m_documents->setCurrentItem(firstVisible);
    updateButtons();
}

void PdfLinkOverrideDialog::updateButtons()
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!selectedDocumentId().isEmpty());
}
}
