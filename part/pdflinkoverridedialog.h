// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include "okularpart_export.h"

class QDialogButtonBox;
class QLineEdit;
class QTreeWidget;

namespace Thoughts
{
// A Library document a PDF link can be redirected to.
struct LibraryDocumentChoice {
    QString documentId;
    QString name;
    QString project;
};

// Library documents with a document_id, by project (then the PDFs of no project),
// without @p excludedDocumentId (the PDF that contains the link).
OKULARPART_EXPORT QList<LibraryDocumentChoice> libraryDocumentChoices(const QString &docsRoot, const QString &excludedDocumentId);

// "Link to Library document…": a searchable list of Library documents grouped by project.
class OKULARPART_EXPORT PdfLinkOverrideDialog : public QDialog
{
    Q_OBJECT

public:
    PdfLinkOverrideDialog(const QList<LibraryDocumentChoice> &choices, const QString &currentDocumentId, QWidget *parent = nullptr);

    QString selectedDocumentId() const;
    void setFilter(const QString &text);

private:
    void applyFilter(const QString &text);
    void updateButtons();

    QLineEdit *m_search = nullptr;
    QTreeWidget *m_documents = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
}
