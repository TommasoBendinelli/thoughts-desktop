// SPDX-License-Identifier: GPL-2.0-or-later
#include "dlgagentprompts.h"

#include "agentpromptsettings.h"
#include "agentsessionsettings.h"
#include "claudecode.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

using namespace Thoughts;

namespace
{
QPlainTextEdit *templateEditor(QWidget *parent, const QString &objectName)
{
    auto *editor = new QPlainTextEdit(parent);
    editor->setObjectName(objectName);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor->setTabChangesFocus(true);
    editor->setMinimumHeight(140);
    return editor;
}
}

DlgAgentPrompts::DlgAgentPrompts(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *editors = new QVBoxLayout;
    layout->addLayout(editors, 3);

    auto *intro = new QLabel(i18n("What the review card ⋮ menu copies to paste into an agent such as Claude or Codex."), this);
    intro->setWordWrap(true);
    editors->addWidget(intro);

    const auto addTemplate = [&](const QString &title, const QString &name, AgentPrompts::Kind kind) {
        auto *box = new QGroupBox(title, this);
        auto *boxLayout = new QVBoxLayout(box);
        auto *editor = templateEditor(box, name);
        boxLayout->addWidget(editor);
        auto *reset = new QPushButton(i18n("Reset to default"), box);
        reset->setObjectName(name + QStringLiteral("Reset"));
        connect(reset, &QPushButton::clicked, editor, [editor, kind] { editor->setPlainText(AgentPrompts::defaultTemplate(kind)); });
        boxLayout->addWidget(reset, 0, Qt::AlignRight);
        connect(editor, &QPlainTextEdit::textChanged, this, &DlgAgentPrompts::changed);
        editors->addWidget(box, 1);
        return editor;
    };
    m_agentTemplate = addTemplate(i18n("Copy for agent"), QStringLiteral("agentPromptTemplate"), AgentPrompts::Kind::Agent);

    // The phone's "Start a new study session": a session of the owner's own Claude Code or Codex,
    // started by Thoughts on this computer (shell/sessionstarter.h).
    auto *sessions = new QGroupBox(i18n("Study sessions from the phone"), this);
    auto *sessionsLayout = new QFormLayout(sessions);
    m_phoneSessions = new QComboBox(sessions);
    m_phoneSessions->setObjectName(QStringLiteral("phoneSessionStart"));
    m_phoneSessions->addItem(i18n("Ask each time"));
    m_phoneSessions->addItem(i18n("Always"));
    m_phoneSessions->addItem(i18n("Never"));
    sessionsLayout->addRow(i18n("Start agent sessions from my phone on this computer:"), m_phoneSessions);
    auto *sessionsHelp = new QLabel(i18n("When you tap Start on the phone's “Start a new study session”, Thoughts here starts Claude Code (or Codex) "
                                         "with your own sign-in and the Thoughts tools, in the folder %1.",
                                         QDir::toNativeSeparators(AgentSessions::sessionsFolder())),
                                    sessions);
    sessionsHelp->setWordWrap(true);
    sessionsLayout->addRow(sessionsHelp);
    connect(m_phoneSessions, &QComboBox::currentIndexChanged, this, &DlgAgentPrompts::changed);
    editors->addWidget(sessions);

    // Claude Code on this computer, where this installation brings the Thoughts tools (Windows):
    // the tools registered for it, and its sessions woken by Thoughts (no helper to install).
    if (ClaudeCode::bundledTools().hasTools()) {
        auto *claude = new QGroupBox(i18n("Claude Code on this computer"), this);
        claude->setObjectName(QStringLiteral("claudeCodeHere"));
        auto *claudeLayout = new QVBoxLayout(claude);
        auto *setUpRow = new QHBoxLayout;
        m_setUpClaudeCode = new QPushButton(i18n("Set up Claude Code"), claude);
        m_setUpClaudeCode->setObjectName(QStringLiteral("setUpClaudeCode"));
        m_setUpClaudeCode->setToolTip(i18n("Gives Claude Code on this computer the Thoughts tools, which use your Thoughts sign-in."));
        setUpRow->addWidget(m_setUpClaudeCode);
        setUpRow->addStretch(1);
        claudeLayout->addLayout(setUpRow);
        m_claudeCodeState = new QLabel(claude);
        m_claudeCodeState->setObjectName(QStringLiteral("claudeCodeState"));
        m_claudeCodeState->setWordWrap(true);
        m_claudeCodeState->setTextInteractionFlags(Qt::TextSelectableByMouse);
        claudeLayout->addWidget(m_claudeCodeState);
        m_answerHere = new QCheckBox(i18n("Let Claude Code sessions on this computer answer"), claude);
        m_answerHere->setObjectName(QStringLiteral("answerOnThisComputer"));
        claudeLayout->addWidget(m_answerHere);
        auto *answerHelp = new QLabel(i18n("Thoughts lists them in Send to Agent and Ask Agent, brings them the pages you send them and your replies to "
                                           "their notes, and starts a closed one again to read them. It runs while Thoughts runs."),
                                      claude);
        answerHelp->setWordWrap(true);
        claudeLayout->addWidget(answerHelp);
        connect(m_setUpClaudeCode, &QPushButton::clicked, this, &DlgAgentPrompts::setUpClaudeCode);
        connect(m_answerHere, &QCheckBox::toggled, this, &DlgAgentPrompts::changed);
        editors->addWidget(claude);
        showClaudeCodeState();
    }

    // The placeholders, next to the editors.
    QString help = QStringLiteral("<p><b>%1</b></p><table cellspacing=\"0\" cellpadding=\"2\">").arg(i18n("Placeholders").toHtmlEscaped());
    for (const auto &placeholder : AgentPrompts::placeholders())
        help += QStringLiteral("<tr><td nowrap><code>{%1}</code></td><td>%2</td></tr>").arg(placeholder.name, placeholder.description.toHtmlEscaped());
    help += QStringLiteral("</table><p>%1</p>")
                .arg(i18n("A line is left out when one of its placeholders has no value, e.g. \"Note:\" for a card without a note. "
                          "Other text in braces is copied as written.")
                         .toHtmlEscaped());
    auto *placeholders = new QLabel(help, this);
    placeholders->setObjectName(QStringLiteral("agentPromptPlaceholders"));
    placeholders->setTextFormat(Qt::RichText);
    placeholders->setWordWrap(true);
    placeholders->setTextInteractionFlags(Qt::TextSelectableByMouse);
    placeholders->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    placeholders->setContentsMargins(6, 0, 6, 6);
    // Scrolls rather than making the whole Settings dialog as tall as the list.
    auto *scroll = new QScrollArea(this);
    scroll->setWidget(placeholders);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(340);
    layout->addWidget(scroll, 2);

    load();
}

void DlgAgentPrompts::load()
{
    const QSignalBlocker agent(m_agentTemplate);
    m_agentTemplate->setPlainText(AgentPrompts::savedTemplate(AgentPrompts::Kind::Agent));
    const QSignalBlocker sessions(m_phoneSessions);
    m_phoneSessions->setCurrentIndex(int(AgentSessions::phoneStartChoice()));
    if (m_answerHere) {
        const QSignalBlocker answer(m_answerHere);
        m_answerHere->setChecked(AgentSessions::answerHere());
    }
}

void DlgAgentPrompts::showClaudeCodeState()
{
    if (!m_claudeCodeState) return;
    m_claudeCodeState->setText(AgentSessions::registeredTools().isEmpty() ? i18n("Claude Code does not have the Thoughts tools yet.")
                                                                           : i18n("Claude Code has the Thoughts tools."));
}

void DlgAgentPrompts::setUpClaudeCode()
{
    m_setUpClaudeCode->setEnabled(false);
    m_claudeCodeState->setText(i18n("Setting up Claude Code…"));
    const bool answerFollowedSetUp = AgentSessions::answerHere() == AgentSessions::answerHereByDefault() && m_answerHere->isChecked() == AgentSessions::answerHere();
    // The app's, not the dialog's: closing Settings meanwhile must not stop claude half way.
    auto *setup = new ClaudeCode::Setup(QCoreApplication::instance());
    const ClaudeCode::BundledTools tools = ClaudeCode::bundledTools();
    const QPointer<DlgAgentPrompts> page(this);
    connect(setup, &ClaudeCode::Setup::finished, setup, [page, setup, tools, answerFollowedSetUp](const ClaudeCode::Result &result) {
        setup->deleteLater();
        const bool registered = result.outcome == ClaudeCode::Outcome::Done || result.outcome == ClaudeCode::Outcome::NotSignedIn;
        if (registered) AgentSessions::setRegisteredTools(tools.fingerprint());
        if (!page) return;
        page->m_setUpClaudeCode->setEnabled(true);
        // The sessions answer from now on, unless the person said otherwise.
        if (registered && answerFollowedSetUp) {
            const QSignalBlocker answer(page->m_answerHere);
            page->m_answerHere->setChecked(AgentSessions::answerHere());
        }
        page->m_claudeCodeState->setText(ClaudeCode::describe(result));
    });
    setup->start();
}

void DlgAgentPrompts::save()
{
    AgentPrompts::saveTemplate(AgentPrompts::Kind::Agent, m_agentTemplate->toPlainText());
    AgentSessions::setPhoneStartChoice(AgentSessions::PhoneStart(qBound(0, m_phoneSessions->currentIndex(), 2)));
    if (m_answerHere) AgentSessions::setAnswerHere(m_answerHere->isChecked());
}

void DlgAgentPrompts::setDefaults()
{
    m_agentTemplate->setPlainText(AgentPrompts::defaultTemplate(AgentPrompts::Kind::Agent));
    m_phoneSessions->setCurrentIndex(int(AgentSessions::PhoneStart::Ask));
    if (m_answerHere) m_answerHere->setChecked(AgentSessions::answerHereByDefault());
}

bool DlgAgentPrompts::hasChanged() const
{
    const QString text = m_agentTemplate->toPlainText();
    return (text.trimmed().isEmpty() ? AgentPrompts::defaultTemplate(AgentPrompts::Kind::Agent) : text)
        != AgentPrompts::savedTemplate(AgentPrompts::Kind::Agent)
        || m_phoneSessions->currentIndex() != int(AgentSessions::phoneStartChoice()) || (m_answerHere && m_answerHere->isChecked() != AgentSessions::answerHere());
}

bool DlgAgentPrompts::isDefault() const
{
    return m_agentTemplate->toPlainText() == AgentPrompts::defaultTemplate(AgentPrompts::Kind::Agent)
        && m_phoneSessions->currentIndex() == int(AgentSessions::PhoneStart::Ask) && (!m_answerHere || m_answerHere->isChecked() == AgentSessions::answerHereByDefault());
}
