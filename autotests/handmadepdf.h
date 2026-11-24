// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A PDF written by hand for tests that need what QPdfWriter cannot write: a crop box away from
// the media box's origin, a /Rotate, a page many screens tall. Text is Helvetica, drawn at
// user-space points (the baseline's left end).

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace HandMadePdf
{
struct Text {
    QPointF at;
    QString text;
    double size = 14;
};

struct Page {
    QRectF media;
    QRectF crop; // none: the media box
    int rotate = 0;
    QList<Text> texts;
};

inline QByteArray box(const QRectF &rect)
{
    return QByteArray("[") + QByteArray::number(rect.left()) + ' ' + QByteArray::number(rect.top()) + ' ' + QByteArray::number(rect.right()) + ' '
        + QByteArray::number(rect.bottom()) + ']';
}

inline bool write(const QString &path, const QList<Page> &pages)
{
    QList<QByteArray> objects;
    QByteArray kids;
    for (int index = 0; index < pages.size(); ++index) {
        kids += QByteArray::number(4 + 2 * index) + " 0 R ";
    }
    objects << "<< /Type /Catalog /Pages 2 0 R >>";
    objects << "<< /Type /Pages /Kids [" + kids + "] /Count " + QByteArray::number(pages.size()) + " >>";
    objects << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    for (int index = 0; index < pages.size(); ++index) {
        const Page &page = pages.at(index);
        QByteArray content;
        for (const Text &text : page.texts) {
            content += "BT /F1 " + QByteArray::number(text.size) + " Tf 1 0 0 1 " + QByteArray::number(text.at.x()) + ' ' + QByteArray::number(text.at.y()) + " Tm ("
                + text.text.toLatin1() + ") Tj ET\n";
        }
        QByteArray object = "<< /Type /Page /Parent 2 0 R /MediaBox " + box(page.media);
        if (!page.crop.isNull()) object += " /CropBox " + box(page.crop);
        if (page.rotate) object += " /Rotate " + QByteArray::number(page.rotate);
        object += " /Resources << /Font << /F1 3 0 R >> >> /Contents " + QByteArray::number(5 + 2 * index) + " 0 R >>";
        objects << object;
        objects << "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream";
    }
    QByteArray pdf = "%PDF-1.4\n";
    QList<qsizetype> offsets;
    for (int index = 0; index < objects.size(); ++index) {
        offsets << pdf.size();
        pdf += QByteArray::number(index + 1) + " 0 obj\n" + objects.at(index) + "\nendobj\n";
    }
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const qsizetype offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(pdf) == pdf.size();
}
}
