// Reproduction for the crash when opening the Sandbox settings page:
// constructing the page and showing it (the way the options dialog does
// when the tab becomes current) crashed in QWidget::setEnabled, called
// from SandboxSettingsWidget::updateBackendWarning() during the
// constructor's first showMxc() call - the backend check ran while the
// widgets were not in a layout yet. The fix is to run the check after
// the layout is attached; this test guards that the page can be
// constructed and shown (and re-shown) without crashing.
#include "../src/sandboxsettingswidget.h"

#include <QApplication>
#include <QTest>

namespace LlamaCpp {

class SandboxWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void constructAndShow();
};

void SandboxWidgetTest::constructAndShow()
{
    for (int i = 0; i < 2; ++i) {
        auto *widget = new SandboxSettingsWidget;
        widget->show();
        qApp->processEvents();
        widget->close();
        qApp->processEvents();
        delete widget;
    }
}

} // namespace LlamaCpp

QTEST_MAIN(LlamaCpp::SandboxWidgetTest)
#include "sandboxwidget_test.moc"
