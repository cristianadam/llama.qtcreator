#include "autoscrollarea.h"

#include <QScrollBar>

namespace LlamaCpp {

AutoScrollArea::AutoScrollArea(QWidget *parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    m_scrollbar = verticalScrollBar();

    connect(m_scrollbar,
            &QScrollBar::rangeChanged,
            this,
            [this](int, int) {
                // The range changes whenever the scrolled content grows or
                // shrinks. Follow the bottom while we are pinned.
                maybeFollow();
            });

    connect(m_scrollbar, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_updating)
            return; // programmatic change, not the user
        m_following = value >= m_scrollbar->maximum() - 1;
    });
}

void AutoScrollArea::followToBottom()
{
    m_following = true;
    m_updating = true;
    m_scrollbar->setValue(m_scrollbar->maximum());
    m_updating = false;
    m_max = m_scrollbar->maximum();
}

void AutoScrollArea::maybeFollow()
{
    if (!m_following)
        return;

    const int max = m_scrollbar->maximum();
    if (max < m_max) {
        // The content shrank (e.g. a spinner line was replaced by the final
        // answer) and the scrollbar value was clamped to the bottom – lower
        // the high-water mark so following resumes when it grows again.
        m_max = max;
        return;
    }
    if (max <= m_max)
        return;

    m_max = max;
    m_updating = true;
    m_scrollbar->setValue(max);
    m_updating = false;
}

} // namespace LlamaCpp
