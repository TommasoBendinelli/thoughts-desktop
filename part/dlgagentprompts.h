// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef DLGAGENTPROMPTS_H
#define DLGAGENTPROMPTS_H

#include <QWidget>

class QComboBox;
class QPlainTextEdit;

// Settings > Agent prompts: the template of the review card ⋮ menu's "Copy for agent", with
// Reset to default, and the placeholders it can use; and whether this computer starts the study
// sessions the phone asks for (agentsessionsettings.h). Saved through PreferencesDialog's OK/Apply
// (agentpromptsettings.h).
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
    QPlainTextEdit *m_agentTemplate;
    // Ask, Always, Never (AgentSessions::PhoneStart, in that order).
    QComboBox *m_phoneSessions;
};

#endif
