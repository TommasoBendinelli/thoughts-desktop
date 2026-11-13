// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArrayView>
#include <QString>

// SHA-256 as lowercase hex, using the platform's hardware-accelerated
// implementation (CommonCrypto, CNG); QCryptographicHash elsewhere. Build
// inputs hash every PDF and image of a folder, where Qt's portable code is
// several times slower.
namespace Thoughts::Publisher
{

QString sha256Hex(QByteArrayView data);

}
