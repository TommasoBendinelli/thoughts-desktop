#include "closedialoghelper.h"

#include <QApplication>
#include <QDialog>
#include <QPushButton>
#include <QTest>
#include <QTimer>

namespace TestingUtils
{
CloseDialogHelper::CloseDialogHelper(Okular::Part *p, QDialogButtonBox::StandardButton b)
    : m_widget(p->widget())
    , m_button(b)
    , m_clicked(false)
{
    QTimer::singleShot(0, this, &CloseDialogHelper::closeDialog);
}

CloseDialogHelper::CloseDialogHelper(QWidget *w, QDialogButtonBox::StandardButton b)
    : m_widget(w)
    , m_button(b)
    , m_clicked(false)
{
    QTimer::singleShot(0, this, &CloseDialogHelper::closeDialog);
}

CloseDialogHelper::CloseDialogHelper(QDialogButtonBox::StandardButton b)
    : m_widget(nullptr)
    , m_button(b)
    , m_clicked(false)
{
    QTimer::singleShot(0, this, &CloseDialogHelper::closeDialog);
}

CloseDialogHelper::~CloseDialogHelper()
{
    QVERIFY(m_clicked);
}

void CloseDialogHelper::closeDialog()
{
    QWidget *dialog = (m_widget) ? m_widget->findChild<QDialog *>() : qApp->activeModalWidget();
    if (!dialog || !QTest::qWaitForWindowExposed(dialog)) {
        QTimer::singleShot(0, this, &CloseDialogHelper::closeDialog);
        return;
    }
    QDialogButtonBox *buttonBox = dialog->findChild<QDialogButtonBox *>();
    QPushButton *button = buttonBox ? buttonBox->button(m_button) : nullptr;
    if (!button) {
        // Fail in the destructor instead of crashing, and let exec() return.
        qWarning() << "The dialog" << dialog->windowTitle() << "has no button" << m_button;
        if (auto *modal = qobject_cast<QDialog *>(dialog)) {
            modal->reject();
        }
        return;
    }
    button->click();
    m_clicked = true;
}
}
