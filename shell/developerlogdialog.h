// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QVariantMap>
#include <functional>
class QWidget;
namespace Thoughts { void showDeveloperLogs(QWidget *parent, std::function<QVariantMap()> sourceTracking = {}); }
