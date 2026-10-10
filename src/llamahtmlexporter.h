#pragma once

#include <QString>
#include <QVector>

#include "llamatypes.h"

namespace LlamaCpp {

// Exports a conversation as self‑contained HTML whose styling (rounded
// message bubbles, current theme colours, syntax‑highlighted code blocks)
// mirrors the chat view.  The document carries its styles in a <style>
// block, so it can be pasted into any editor that allows editing the HTML
// source.
class HtmlExporter
{
public:
    // A complete HTML document (doctype, <style>, one bubble per message).
    static QString conversationHtml(const QString &title, const QVector<Message> &messages);

    // The bubble markup for a single message.  Returns an empty string for
    // messages that render through their tool bubble instead (mirroring
    // ChatManager::messageToMarkdown()).
    static QString messageToHtml(const Message &msg);
};

} // namespace LlamaCpp
