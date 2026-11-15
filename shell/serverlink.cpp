// SPDX-License-Identifier: GPL-2.0-or-later
#include "serverlink.h"

#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace Thoughts
{

void ServerLink::setServer(const QUrl &baseUrl, const QByteArray &token)
{
    m_baseUrl = baseUrl;
    m_token = token;
    m_explicit = true;
}

bool ServerLink::ensure()
{
    if (!m_explicit && (m_token.isEmpty() || !m_baseUrl.isValid() || m_baseUrl.isEmpty())) {
        m_token = qgetenv("THOUGHTS_TOKEN").trimmed();
        if (QStandardPaths::isTestModeEnabled() && (m_token.isEmpty() || !ServerAddress::overriddenByEnvironment())) {
            m_token.clear();
            m_baseUrl.clear();
        } else {
            const QString address = ServerAddress::configured();
            if (m_token.isEmpty()) m_token = ServerAddress::storedToken(Platform::desktopDeviceAccount(), address);
            m_baseUrl = QUrl(ServerAddress::normalized(address));
        }
    }
    return !m_token.isEmpty() && m_baseUrl.isValid() && !m_baseUrl.isEmpty();
}

void ServerLink::tokenRefused()
{
    if (!m_explicit) m_token.clear();
}

QNetworkRequest ServerLink::request(const QString &pathAndQuery, int timeoutMs, const QByteArray &userAgent) const
{
    QUrl url = m_baseUrl;
    QString base = url.path();
    if (base.endsWith(QLatin1Char('/'))) base.chop(1);
    const QUrl relative(pathAndQuery);
    url.setPath(base + relative.path());
    url.setQuery(relative.query());
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setRawHeader("User-Agent", userAgent);
    request.setTransferTimeout(timeoutMs);
    return request;
}

QStringList serverCapabilities(const QByteArray &healthJson)
{
    QStringList capabilities;
    for (const QJsonValue &value : QJsonDocument::fromJson(healthJson).object().value(QLatin1String("capabilities")).toArray()) {
        if (value.isString()) capabilities.append(value.toString());
    }
    return capabilities;
}

}
