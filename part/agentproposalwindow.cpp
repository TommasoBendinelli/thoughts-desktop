/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "agentproposalwindow.h"
#include "agentconversationopener.h"
#include "proposalprovenance.h"

#include <QCloseEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextOption>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include <KLocalizedString>

// QPlainTextEdit only overrides application shortcuts when editable. These
// review fields must retain Copy and Select All while remaining read-only.
class ProposalTextEdit : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ShortcutOverride) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->matches(QKeySequence::Copy) || key->matches(QKeySequence::SelectAll)) {
                event->accept();
                return true;
            }
        }
        return QPlainTextEdit::event(event);
    }
};

static QStringList proposalTextLines(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (text.isEmpty()) {
        return {};
    }
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (text.endsWith(QLatin1Char('\n')) && !lines.isEmpty() && lines.constLast().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

static QString proposalLineDiff(const QString &beforeText, const QString &afterText)
{
    const QStringList before = proposalTextLines(beforeText);
    const QStringList after = proposalTextLines(afterText);
    const int beforeCount = before.size();
    const int afterCount = after.size();
    QVector<QVector<int>> common(beforeCount + 1, QVector<int>(afterCount + 1));
    for (int beforeIndex = beforeCount - 1; beforeIndex >= 0; --beforeIndex) {
        for (int afterIndex = afterCount - 1; afterIndex >= 0; --afterIndex) {
            common[beforeIndex][afterIndex] = before.at(beforeIndex) == after.at(afterIndex)
                ? common.at(beforeIndex + 1).at(afterIndex + 1) + 1
                : std::max(common.at(beforeIndex + 1).at(afterIndex), common.at(beforeIndex).at(afterIndex + 1));
        }
    }

    QStringList diff;
    int beforeIndex = 0;
    int afterIndex = 0;
    while (beforeIndex < beforeCount || afterIndex < afterCount) {
        if (beforeIndex < beforeCount && afterIndex < afterCount && before.at(beforeIndex) == after.at(afterIndex)) {
            ++beforeIndex;
            ++afterIndex;
        } else if (beforeIndex < beforeCount && (afterIndex == afterCount || common.at(beforeIndex + 1).at(afterIndex) >= common.at(beforeIndex).at(afterIndex + 1))) {
            diff.append(QStringLiteral("- ") + before.at(beforeIndex));
            ++beforeIndex;
        } else {
            diff.append(QStringLiteral("+ ") + after.at(afterIndex));
            ++afterIndex;
        }
    }
    return diff.isEmpty() ? i18n("No line changes.") : diff.join(QLatin1Char('\n'));
}

AgentProposalWindow::AgentProposalWindow(QWidget *parent)
    : QFrame(parent, Qt::Window)
{
    setObjectName(QStringLiteral("AgentProposalWindow"));
    setWindowTitle(i18n("Agent Edit Proposal"));
    setAutoFillBackground(true);
    setFrameStyle(QFrame::Panel | QFrame::Raised);
    setMinimumSize(700, 480);
    resize(960, 640);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName(QStringLiteral("agentProposalTitleBar"));
    auto *titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(2, 0, 0, 0);
    m_title = new QLabel(i18n("Agent Edit Proposal"), m_titleBar);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    titleLayout->addWidget(m_title);
    titleLayout->addStretch(1);
    m_closeButton = new QToolButton(m_titleBar);
    m_closeButton->setObjectName(QStringLiteral("agentProposalClose"));
    m_closeButton->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    m_closeButton->setText(i18n("Close"));
    m_closeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_closeButton->setToolTip(i18n("Close"));
    m_closeButton->setAutoRaise(true);
    titleLayout->addWidget(m_closeButton);
    layout->addWidget(m_titleBar);

    const auto addField = [this, layout](const QString &label, const QString &objectName, int minimumHeight) {
        auto *title = new QLabel(label, this);
        QFont font = title->font();
        font.setBold(true);
        title->setFont(font);
        layout->addWidget(title);
        auto *field = new ProposalTextEdit(this);
        field->setObjectName(objectName);
        field->setReadOnly(true);
        field->setMinimumHeight(minimumHeight);
        field->setTabChangesFocus(true);
        layout->addWidget(field);
        return field;
    };
    m_motivation = addField(i18n("Motivation"), QStringLiteral("agentProposalMotivation"), 54);
    m_motivation->setMaximumHeight(110);

    auto *diffTitle = new QLabel(i18n("Source change"), this);
    QFont diffTitleFont = diffTitle->font();
    diffTitleFont.setBold(true);
    diffTitle->setFont(diffTitleFont);
    layout->addWidget(diffTitle);
    m_diff = new ProposalTextEdit(this);
    m_diff->setObjectName(QStringLiteral("agentProposalDiff"));
    m_diff->setReadOnly(true);
    m_diff->setMinimumHeight(180);
    m_diff->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_diff->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_diff->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_diff->setTabChangesFocus(true);
    layout->addWidget(m_diff, 3);

    m_locationIndicator = new QWidget(this);
    m_locationIndicator->setObjectName(QStringLiteral("agentProposalLocationIndicator"));
    auto *locationLayout = new QHBoxLayout(m_locationIndicator);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->setSpacing(4);
    auto *locationIcon = new QLabel(m_locationIndicator);
    locationIcon->setPixmap(QIcon::fromTheme(QStringLiteral("dialog-information")).pixmap(style()->pixelMetric(QStyle::PM_SmallIconSize)));
    locationLayout->addWidget(locationIcon);
    m_locationStatus = new QLabel(i18n("No visible PDF location"), m_locationIndicator);
    m_locationStatus->setObjectName(QStringLiteral("agentProposalLocationStatus"));
    QFont locationFont = m_locationStatus->font();
    if (locationFont.pointSizeF() > 1.0) {
        locationFont.setPointSizeF(locationFont.pointSizeF() - 1.0);
    }
    m_locationStatus->setFont(locationFont);
    locationLayout->addWidget(m_locationStatus);
    locationLayout->addStretch(1);
    const QString locationToolTip = i18n("This proposal does not produce visible PDF content, so there is nowhere in the document to move to.");
    m_locationIndicator->setToolTip(locationToolTip);
    m_locationStatus->setToolTip(locationToolTip);
    m_locationIndicator->hide();
    layout->addWidget(m_locationIndicator);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("agentProposalStatus"));
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);

    auto *actions = new QHBoxLayout;
    m_openSource = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Open Source in VS Code"), this);
    m_openSource->setObjectName(QStringLiteral("agentProposalOpenSource"));
    m_openSource->setAutoDefault(false);
    m_openConversation = new QPushButton(QIcon::fromTheme(QStringLiteral("external-link")), i18n("Open Conversation"), this);
    m_openConversation->setObjectName(QStringLiteral("agentProposalOpenConversation"));
    m_openConversation->setAutoDefault(false);
    m_openConversation->setEnabled(false);
    m_reject = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), i18n("Reject Proposal"), this);
    m_reject->setObjectName(QStringLiteral("agentProposalReject"));
    m_reject->setAutoDefault(false);
    m_accept = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18n("Accept Edit"), this);
    m_accept->setObjectName(QStringLiteral("agentProposalAccept"));
    m_accept->setAutoDefault(false);
    actions->addWidget(m_openSource);
    actions->addWidget(m_openConversation);
    actions->addStretch(1);
    actions->addWidget(m_reject);
    actions->addWidget(m_accept);
    layout->addLayout(actions);

    connect(m_closeButton, &QToolButton::clicked, this, &QWidget::close);
    connect(m_openSource, &QPushButton::clicked, this, [this] { Q_EMIT openSourceRequested(m_proposal.sourcePath, m_proposal.sourceLine); });
    connect(m_openConversation, &QPushButton::clicked, this, [this] {
        if (!m_conversationUrl.isEmpty() && !Thoughts::openAgentConversationUrl(m_conversationUrl)) {
            m_status->setText(m_conversationUrl.startsWith(QLatin1String("codex:")) ? i18n("Could not open the conversation in Codex. Check that Codex is installed.")
                                                                                    : i18n("Could not open the conversation."));
            m_status->show();
        }
    });
    connect(m_reject, &QPushButton::clicked, this, [this] { Q_EMIT resolutionRequested(m_proposal.id, QStringLiteral("disregard")); });
    connect(m_accept, &QPushButton::clicked, this, [this] { Q_EMIT resolutionRequested(m_proposal.id, QStringLiteral("accept")); });
}

void AgentProposalWindow::setProposal(const AgentProposalSidebarEntry &proposal)
{
    m_proposal = proposal;
    m_operationPending = false;
    m_motivation->setPlainText(proposal.motivation);
    m_conversationUrl = Thoughts::proposalConversationLink(proposal.conversationUrl, proposal.conversationId);
    const bool codex = m_conversationUrl.startsWith(QLatin1String("codex:"));
    m_openConversation->setEnabled(!m_conversationUrl.isEmpty());
    m_openConversation->setText(m_conversationUrl.isEmpty() ? i18n("Open Conversation") : codex ? i18n("Open in Codex") : i18n("Open in Claude"));
    m_openConversation->setToolTip(m_conversationUrl.isEmpty() ? i18n("No link to the conversation that created this proposal is recorded.")
                                   : codex ? i18n("Open the Codex conversation that created this proposal.")
                                           : i18n("Open the Claude conversation that created this proposal."));
    m_diff->setPlainText(proposalLineDiff(proposal.before, proposal.after));
    setLocationUnavailable(false);
    updateDiffFormatting();
    m_accept->setText(proposal.after.trimmed().isEmpty() ? i18n("Accept Removal") : i18n("Accept Edit"));
    if (proposal.resolutionError.isEmpty()) {
        m_status->clear();
        m_status->hide();
    } else {
        m_status->setText(proposal.resolutionError);
        m_status->show();
    }
    updateActions();
}

void AgentProposalWindow::setLocationUnavailable(bool unavailable)
{
    if (unavailable) {
        const QString message = i18n("No visible PDF location");
        const QString toolTip = i18n("This proposal does not produce visible PDF content, so there is nowhere in the document to move to.");
        m_locationStatus->setText(message);
        m_locationIndicator->setToolTip(toolTip);
        m_locationStatus->setToolTip(toolTip);
    }
    m_locationIndicator->setVisible(unavailable);
}

void AgentProposalWindow::setPdfBuildRequired(bool required)
{
    if (required) {
        const QString message = i18n("Proposed text is not in the current PDF. Build is required.");
        const QString toolTip = i18n("Thoughts moved to the current PDF text. Run Build to compile the proposed text into the PDF.");
        m_locationStatus->setText(message);
        m_locationIndicator->setToolTip(toolTip);
        m_locationStatus->setToolTip(toolTip);
    }
    m_locationIndicator->setVisible(required);
}

void AgentProposalWindow::setOperationPending(bool pending, const QString &message)
{
    m_operationPending = pending;
    if (!message.isEmpty()) {
        m_status->setText(message);
        m_status->show();
    }
    updateActions();
}

void AgentProposalWindow::showResolutionError(const QString &message)
{
    m_operationPending = false;
    m_status->setText(message);
    m_status->show();
    updateActions();
}

void AgentProposalWindow::showResolutionSuccess(const QString &message)
{
    m_operationPending = true;
    m_status->setText(message);
    m_status->show();
    updateActions();
}

QString AgentProposalWindow::proposalId() const
{
    return m_proposal.id;
}

void AgentProposalWindow::updateDiffFormatting()
{
    QList<QTextEdit::ExtraSelection> selections;
    for (QTextBlock block = m_diff->document()->firstBlock(); block.isValid(); block = block.next()) {
        const QString text = block.text();
        if (!text.startsWith(QLatin1Char('-')) && !text.startsWith(QLatin1Char('+'))) {
            continue;
        }
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(block);
        selection.cursor.select(QTextCursor::BlockUnderCursor);
        if (text.startsWith(QLatin1Char('-'))) {
            selection.format.setForeground(QColor(145, 25, 25));
            selection.format.setBackground(QColor(255, 228, 228));
        } else {
            selection.format.setForeground(QColor(20, 105, 45));
            selection.format.setBackground(QColor(224, 247, 231));
        }
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selections.append(selection);
    }
    m_diff->setExtraSelections(selections);
    m_diff->moveCursor(QTextCursor::Start);
}

void AgentProposalWindow::closeEvent(QCloseEvent *event)
{
    QFrame::closeEvent(event);
    if (event->isAccepted()) {
        Q_EMIT closed();
    }
}

void AgentProposalWindow::updateActions()
{
    const bool canResolveLocally = m_proposal.liveSource && m_proposal.sourceSha256.size() == 64 && m_proposal.resolutionError.isEmpty();
    const bool canResolveArtifact = !m_proposal.localSourcePresent && m_proposal.bundleId.size() == 64 && m_proposal.resolutionError.isEmpty();
    const bool canResolve = !m_operationPending && !m_proposal.id.isEmpty() && (canResolveLocally || canResolveArtifact);
    m_accept->setEnabled(canResolve);
    m_reject->setEnabled(canResolve);
    m_openSource->setEnabled(!m_operationPending && !m_proposal.sourcePath.isEmpty() && m_proposal.sourceLine > 0);
}
