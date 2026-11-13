// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gitclone.h"

class QWidget;

namespace Thoughts {
// Clones on a worker thread behind a "Cloning repository…" progress dialog and asks
// for sign-ins, key passphrases and new SSH host keys in dialogs. Unless it returns
// Cloned, workDirectory (which contains request.destination) is removed: right away,
// or, when Cancel finds the clone waiting on the network, by the cloning thread once
// the connection gives up.
GitCloneOutcome cloneWithProgress(QWidget *parent, const GitCloneRequest &request, const QString &workDirectory);
}
