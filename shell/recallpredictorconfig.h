// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>

// The agent recall prediction service (review-phases spec, section 4):
// recall-predictor.json in the application data folder, {"url": …, "token": …}.
// Each request carries the token and a review card's question, answer and
// history, so the address must be https. Plain http is accepted only for a
// service on this computer itself (localhost, 127.0.0.1, ::1), for example
// the near end of an SSH tunnel: to any other host the token and the cards
// would cross the network unencrypted.
namespace Thoughts::RecallPredictorConfig
{

struct Service {
    QUrl url;
    QByteArray token;
};

inline bool isLoopbackHost(const QString &host)
{
    const QString name = host.toLower();
    if (name == QLatin1String("localhost") || name.endsWith(QLatin1String(".localhost"))) {
        return true;
    }
    const QHostAddress address(name);
    return !address.isNull() && address.isLoopback();
}

inline bool urlAllowed(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return false;
    }
    return url.scheme() == QLatin1String("https") || (url.scheme() == QLatin1String("http") && isLoopbackHost(url.host()));
}

// The service a configuration file names, or an empty one (no prediction is
// asked for) when the address or the token is missing or the address is not
// allowed; *refusal then says why, for the log.
inline Service parse(const QByteArray &json, QString *refusal = nullptr)
{
    const QJsonObject config = QJsonDocument::fromJson(json).object();
    const QUrl url(config.value(QStringLiteral("url")).toString());
    const QByteArray token = config.value(QStringLiteral("token")).toString().toUtf8();
    if (refusal) {
        refusal->clear();
    }
    if (url.isEmpty() || token.isEmpty()) {
        return {};
    }
    if (!urlAllowed(url)) {
        if (refusal) {
            *refusal = QStringLiteral("recall-predictor.json: the address must start with https://; http:// is accepted only for this computer itself (localhost, 127.0.0.1, ::1). No recall prediction is asked for.");
        }
        return {};
    }
    return {url, token};
}

}
