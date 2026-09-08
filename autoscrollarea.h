#pragma once

#include <QScrollArea>

class QScrollBar;

namespace LlamaCpp {

// A QScrollArea that keeps the viewport pinned to the bottom while the
// scrolled content keeps growing (e.g. a chat stream or tool output).
//
// As soon as the user scrolls up (i.e. is no longer at the bottom) the
// automatic following stops. Calling followToBottom() re-enables it and
// jumps to the bottom immediately.
class AutoScrollArea : public QScrollArea
{
    Q_OBJECT

public:
    explicit AutoScrollArea(QWidget *parent = nullptr);

    // Snap the viewport to the very bottom and resume following.
    void followToBottom();

    // Whether the viewport is currently pinned to the bottom.
    bool following() const { return m_following; }

private:
    void maybeFollow();

    QScrollBar *m_scrollbar{nullptr};
    bool m_following{true};
    bool m_updating{false}; // guard while we move the scrollbar ourselves
    int m_max{-1};
};

} // namespace LlamaCpp
