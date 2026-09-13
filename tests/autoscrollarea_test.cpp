#include <QApplication>
#include <QScrollBar>
#include <QTest>
#include <QWidget>

#include <autoscrollarea.h>

using namespace LlamaCpp;

static void pump(int ms = 20)
{
    for (int i = 0; i < 3; ++i) {
        QApplication::processEvents();
        QTest::qWait(ms);
    }
}

class AutoScrollAreaTest : public QObject
{
    Q_OBJECT

private:
    AutoScrollArea m_area;
    QWidget *m_content{nullptr};

    void setupContent()
    {
        m_content = new QWidget;
        m_area.setWidget(m_content);
        m_area.resize(300, 300);
        m_area.show();
    }

    void setHeight(int h)
    {
        m_content->setMinimumHeight(h);
        pump();
    }

    int value() const { return m_area.verticalScrollBar()->value(); }
    int maximum() const { return m_area.verticalScrollBar()->maximum(); }

private slots:
    void followsGrowth();
    void stopsFollowingAfterUserScrollUp();
    void resumesOnFollowToBottom();
    void atBottomAgainEnablesFollowing();
    void followsAgainAfterShrink();
};

void AutoScrollAreaTest::followsGrowth()
{
    setupContent();
    setHeight(600);
    QVERIFY2(maximum() > 0, "content should overflow the viewport");

    m_area.followToBottom();
    QCOMPARE(value(), maximum());

    // Simulate streaming: the content keeps growing. The viewport must stay
    // pinned to the bottom without any further explicit call.
    setHeight(1200);
    QVERIFY(m_area.following());
    QCOMPARE(value(), maximum());
}

void AutoScrollAreaTest::stopsFollowingAfterUserScrollUp()
{
    setupContent();
    setHeight(600);
    m_area.followToBottom();
    QCOMPARE(value(), maximum());

    // User scrolls to the top.
    m_area.verticalScrollBar()->setValue(0);
    pump();
    QVERIFY(!m_area.following());

    // More content arrives: we must NOT yank the user back to the bottom.
    setHeight(1200);
    QCOMPARE(value(), 0);
    QVERIFY(!m_area.following());
}

void AutoScrollAreaTest::resumesOnFollowToBottom()
{
    setupContent();
    setHeight(600);
    m_area.verticalScrollBar()->setValue(0);
    pump();
    QVERIFY(!m_area.following());

    // The user sends a new message: following resumes and we jump down.
    m_area.followToBottom();
    QVERIFY(m_area.following());
    QCOMPARE(value(), maximum());

    setHeight(1200);
    QCOMPARE(value(), maximum());
}

void AutoScrollAreaTest::atBottomAgainEnablesFollowing()
{
    setupContent();
    setHeight(600);
    m_area.followToBottom();

    m_area.verticalScrollBar()->setValue(0);
    pump();
    QVERIFY(!m_area.following());

    // User scrolls back down to the very bottom.
    m_area.verticalScrollBar()->setValue(m_area.verticalScrollBar()->maximum());
    pump();
    QVERIFY(m_area.following());
}

void AutoScrollAreaTest::followsAgainAfterShrink()
{
    setupContent();
    setHeight(600);
    m_area.followToBottom();
    QCOMPARE(value(), maximum());

    // Streaming grows the content; the view stays pinned to the bottom.
    setHeight(1200);
    QCOMPARE(value(), maximum());
    QVERIFY(m_area.following());

    // A tool update / final answer removes content, so the range shrinks.
    setHeight(900);
    pump();
    // The scrollbar value is clamped to the new bottom; we are still pinned.
    QVERIFY(m_area.following());

    // More content arrives, but the range is still below the earlier peak.
    // Following must resume instead of being blocked by the high-water mark.
    setHeight(1050);
    QCOMPARE(value(), maximum());
    QVERIFY(m_area.following());
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    AutoScrollAreaTest tc;
    return QTest::qExec(&tc);
}

#include "autoscrollarea_test.moc"
