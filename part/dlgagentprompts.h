// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef DLGAGENTPROMPTS_H
#define DLGAGENTPROMPTS_H

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

// Settings > Agent prompts: the template of the review card ⋮ menu's "Copy for agent", with
// Reset to default, and the placeholders it can use; whether this computer starts the study
// sessions the phone asks for (agentsessionsettings.h); and, where the installation brings the
// Thoughts tools (Windows), Claude Code on this computer: Set up Claude Code, which registers them
// at once (claudecode.h), and whether this computer's Claude Code sessions answer (the app's reply
// waker, shell/wakersupervisor.h). Saved through PreferencesDialog's OK/Apply (agentpromptsettings.h),
// except Set up Claude Code, which acts when it is chosen.
class DlgAgentPrompts : public QWidget
{
    Q_OBJECT

public:
    explicit DlgAgentPrompts(QWidget *parent = nullptr);

    void load();
    void save();
    void setDefaults();
    bool hasChanged() const;
    bool isDefault() const;

Q_SIGNALS:
    void changed();

private:
    void setUpClaudeCode();
    void showClaudeCodeState();

    QPlainTextEdit *m_agentTemplate;
    // Ask, Always, Never (AgentSessions::PhoneStart, in that order).
    QComboBox *m_phoneSessions;
    // Claude Code on this computer (null where the installation has no tools).
    QPushButton *m_setUpClaudeCode = nullptr;
    QLabel *m_claudeCodeState = nullptr;
    QCheckBox *m_answerHere = nullptr;
};

#endif
