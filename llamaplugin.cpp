#include "llamaplugin.h"
#include "llamaconstants.h"
#include "llamaicons.h"
#include "llamaprojectpanel.h"
#include "llamasettings.h"
#include "llamatr.h"

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/dialogs/ioptionspage.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/statusbarmanager.h>
#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>
#include <texteditor/textmark.h>
#include <texteditor/textsuggestion.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <QToolButton>

using namespace Core;
using namespace TextEditor;
using namespace Utils;
using namespace ProjectExplorer;

Q_LOGGING_CATEGORY(llamaLog, "llama.cpp", QtWarningMsg)

namespace LlamaCpp::Internal {

LlamaPlugin::LlamaPlugin()
    : m_ringUpdateTimer(new QTimer(this))
    , m_networkManager(new QNetworkAccessManager(this))
{
    connect(m_ringUpdateTimer, &QTimer::timeout, this, &LlamaPlugin::ring_update);
}

LlamaPlugin::~LlamaPlugin()
{
    disconnect(m_ringUpdateTimer, &QTimer::timeout, this, &LlamaPlugin::ring_update);
}

void LlamaPlugin::initialize()
{
    IOptionsPage::registerCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY,
                                   Constants::LLAMACPP_GENERAL_OPTIONS_DISPLAY_CATEGORY,
                                   ":/images/settingscategory_llama.png");

    ActionBuilder requestAction(this, Constants::LLAMACPP_REQUEST_SUGGESTION);
    requestAction.setText(Tr::tr("Request llama.cpp Suggestion"));
    requestAction.setToolTip(
        Tr::tr("Request llama.cpp suggestion at the current editor's cursor position."));
    requestAction.addOnTriggered(this, [this] {
        if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget()) {
            QTextCursor cursor = editor->textCursor();
            int pos_x = cursor.positionInBlock();
            int pos_y = cursor.blockNumber() + 1;
            fim(pos_x, pos_y, true);
        }
    });
    requestAction.setDefaultKeySequence(Tr::tr("Ctrl+G"));

    ActionBuilder toggleAction(this, Constants::LLAMACPP_TOGGLE_ENABLE_DISABLE);
    toggleAction.setText(Tr::tr("Toggle enable/disable llama.cpp"));
    toggleAction.setCheckable(true);
    toggleAction.setChecked(settings().enableLlamaCpp());
    toggleAction.setIcon(LLAMACPP_ICON.icon());
    toggleAction.addOnTriggered(this, [](bool checked) {
        settings().enableLlamaCpp.setValue(checked);
        settings().apply();
    });

    QAction *toggleAct = toggleAction.contextAction();
    QAction *requestAct = requestAction.contextAction();
    auto updateActions = [toggleAct, requestAct] {
        const bool enabled = settings().enableLlamaCpp();
        toggleAct->setToolTip(enabled ? Tr::tr("Disable llama.cpp.") : Tr::tr("Enable llama.cpp."));
        toggleAct->setChecked(enabled);
        requestAct->setEnabled(enabled);
    };

    settings().enableLlamaCpp.addOnChanged(this, updateActions);

    updateActions();

    auto toggleButton = new QToolButton;
    toggleButton->setDefaultAction(toggleAction.contextAction());
    StatusBarManager::addStatusBarWidget(toggleButton, StatusBarManager::RightCorner);

    setupLlamaCppProjectPanel();

    // Connect to editor manager signals
    connect(EditorManager::instance(),
            &EditorManager::currentEditorChanged,
            this,
            &LlamaPlugin::handleCurrentEditorChanged);
    connect(EditorManager::instance(),
            &EditorManager::editorAboutToClose,
            this,
            &LlamaPlugin::handleEditorAboutToClose);
    connect(EditorManager::instance(),
            &EditorManager::saved,
            this,
            &LlamaPlugin::handleDocumentSaved);

    connect(qApp->clipboard(), &QClipboard::dataChanged, this, [this] {
        if (qApp->clipboard()->text().isEmpty())
            return;
        pick_chunk(qApp->clipboard()->text().split("\n"), false, true);
    });
}

bool LlamaPlugin::delayedInitialize()
{
    settingsUpdated();
    connect(&settings(), &AspectContainer::applied, this, &LlamaPlugin::settingsUpdated);

    return true;
}

void LlamaPlugin::settingsUpdated()
{
    // Set up timer for context gathering
    if (settings().ringNChunks.value() > 0 && !m_ringUpdateTimer->isActive()) {
        m_ringUpdateTimer->start(settings().ringUpdateMs.value());
    } else if (settings().ringNChunks.value() == 0 && m_ringUpdateTimer->isActive()) {
        m_ringUpdateTimer->stop();
    }
}

ExtensionSystem::IPlugin::ShutdownFlag LlamaPlugin::aboutToShutdown()
{
    return SynchronousShutdown;
}

void LlamaPlugin::handleCurrentEditorChanged(Core::IEditor *editor)
{
    if (!editor)
        return;

    TextEditorWidget *editorWidget = TextEditorWidget::fromEditor(editor);
    if (!editorWidget)
        return;

    if (editorWidget->textDocument()
        && !m_lastEditLineHash.contains(editorWidget->textDocument()->filePath())) {
        m_lastEditLineHash[editorWidget->textDocument()->filePath()] = -9999;
    }

    pick_chunk_at_cursor(editorWidget);

    // Connect to cursor position changes
    connect(editorWidget,
            &TextEditorWidget::cursorPositionChanged,
            this,
            &LlamaPlugin::handleCursorPositionChanged);
}

void LlamaPlugin::handleEditorAboutToClose(Core::IEditor *editor)
{
    if (!editor)
        return;

    TextEditorWidget *editorWidget = TextEditorWidget::fromEditor(editor);
    if (!editorWidget)
        return;

    pick_chunk_at_cursor(editorWidget);

    disconnect(editorWidget,
               &TextEditorWidget::cursorPositionChanged,
               this,
               &LlamaPlugin::handleCursorPositionChanged);

    hideCompletionHint();
}

void LlamaPlugin::handleCursorPositionChanged()
{
    TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget();
    if (!editor)
        return;

    hideCompletionHint();

    QTextCursor cursor = editor->textCursor();
    int pos_x = cursor.positionInBlock();
    int pos_y = cursor.blockNumber() + 1;

    if (settings().autoFim.value()) {
        fim(pos_x, pos_y, true);
    }

    fim_try_hint(pos_x, pos_y);
}

void LlamaPlugin::pick_chunk_at_cursor(TextEditorWidget *editor)
{
    if (!editor)
        return;

    QTextCursor cursor = editor->textCursor();
    int pos_y = cursor.blockNumber() + 1;
    int max_y = editor->document()->lineCount();

    QStringList lines = getlines(editor,
                                 qMax(1, pos_y - settings().ringChunkSize.value() / 2),
                                 qMin(pos_y + settings().ringChunkSize.value() / 2, max_y));
    pick_chunk(lines, true, true);
}

void LlamaPlugin::handleDocumentSaved(Core::IDocument *document)
{
    TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget();
    if (!editor || !document)
        return;

    if (editor->textDocument()->filePath() != document->filePath())
        return;

    pick_chunk_at_cursor(editor);
}

// compute how similar two chunks of text are
// 0 - no similarity, 1 - high similarity
// TODO: figure out something better
static double chunk_sim(const QStringList &c0, const QStringList &c1)
{
    if (c0.isEmpty() && c1.isEmpty())
        return 0.0;

    int common = 0;

    // Count common lines
    for (const auto &line0 : c0) {
        for (const auto &line1 : c1) {
            if (line0 == line1) {
                common++;
                break;
            }
        }
    }

    return 2.0 * common / (c0.size() + c1.size());
}

void LlamaPlugin::fim(int pos_x, int pos_y, bool isAuto)
{
    TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget();
    if (!editor)
        return;

    MultiTextCursor cursor = editor->multiTextCursor();
    if (cursor.hasMultipleCursors() || cursor.hasSelection() || editor->suggestionVisible())
        return;

    QTextDocument *currentDocument = editor->document();
    if (!currentDocument)
        return;

    // Get local context
    auto [prefix, middle, suffix] = fim_ctx_local(editor, pos_x, pos_y);

    // Check cache first
    const QByteArray hash = QCryptographicHash::hash((prefix + middle + "Î" + suffix).toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toHex();

    QByteArrayList hashes{hash};

    // compute multiple hashes that can be used to generate a completion for which the first few lines
    // are missing. this happens when we have scrolled down a bit from where the original generation was done
    QString prefix_trim = prefix;
    for (int i = 0; i < 3; ++i) {
        QRegularExpression re("^[^\n]*\n");
        prefix_trim = prefix_trim.replace(re, "");
        if (prefix_trim.isEmpty())
            break;
        hashes << QCryptographicHash::hash((prefix_trim + middle + "Î" + suffix).toUtf8(),
                                           QCryptographicHash::Sha256)
                      .toHex();
    }

    // if we already have a cached completion for one of the hashes, don't send a request
    for (const QByteArray &h : hashes) {
        if (m_cacheData.contains(h)) {
            return;
        }
    }

    // Create JSON request
    QJsonObject request;

    QStringList stopStrings = settings().stopStrings.value().isEmpty()
                                  ? QStringList()
                                  : settings().stopStrings.value().split(" ");
    request["prompt"] = middle;
    request["n_predict"] = settings().nPredict.value();
    request["stop"] = QJsonArray::fromStringList(stopStrings);
    request["top_k"] = 40;
    request["top_p"] = 0.9;
    request["stream"] = false;
    request["samplers"] = QJsonArray::fromStringList({"top_k", "top_p", "infill"});
    request["cache_prompt"] = true;
    request["t_max_prompt_ms"] = settings().tMaxPromptMs.value();
    request["t_max_predict_ms"]
        = isAuto ? 250 : settings().tMaxPredictMs.value(); // Faster for auto completion
    request["response_fields"] = QJsonArray::fromStringList({"content",
                                                             "timings/prompt_n",
                                                             "timings/prompt_ms",
                                                             "timings/prompt_per_token_ms",
                                                             "timings/prompt_per_second",
                                                             "timings/predicted_n",
                                                             "timings/predicted_ms",
                                                             "timings/predicted_per_token_ms",
                                                             "timings/predicted_per_second",
                                                             "truncated",
                                                             "tokens_cached"});

    // evict chunks that are very similar to the current context
    // this is needed because such chunks usually distort the completion to repeat what was already there

    int startLine = 1;
    int endLine = currentDocument->lineCount();
    if (currentDocument->lineCount() > settings().ringChunkSize.value()) {
        startLine = qMax(startLine, pos_y - settings().ringChunkSize.value() / 2);
        endLine = qMin(endLine, pos_y + settings().ringChunkSize.value() / 2);
    }
    const QStringList lines = getlines(editor, startLine, endLine);
    if (lines.size() < 3)
        return;

    QStringList chunk;
    if (currentDocument->lineCount() > settings().ringChunkSize.value()) {
        // Pick a random chunk
        int l0 = QRandomGenerator::global()->bounded(lines.size()
                                                     - settings().ringChunkSize.value() / 2);
        int l1 = qMin(l0 + settings().ringChunkSize.value() / 2, lines.size());

        chunk = lines.mid(l0, l1 - l0);
    } else {
        chunk = lines;
    }

    for (int i = m_ringChunks.size() - 1; i >= 0; --i) {
        if (chunk_sim(m_ringChunks[i].data, chunk) > 0.5) {
            m_ringChunks.removeAt(i);
            m_ringNEvict++;
        }
    }

    // Add extra context
    QJsonArray extraContext;
    for (const Chunk &chunk : m_ringChunks) {
        QJsonObject chunkObj;
        chunkObj["text"] = chunk.str;
        chunkObj["time"] = chunk.time.toString(Qt::ISODate);
        chunkObj["filename"] = chunk.filename.path();
        extraContext.append(chunkObj);
    }
    request["input_prefix"] = prefix;
    request["input_suffix"] = suffix;
    request["input_extra"] = extraContext;

    // Create JSON document
    QJsonDocument doc(request);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    // Send request
    QNetworkRequest req(QUrl(settings().endpoint.value()));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().apiKey.value().isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + settings().apiKey.value().toUtf8());
    }

    QNetworkReply *reply = m_networkManager->post(req, jsonData);

    // Connect to response
    connect(reply, &QNetworkReply::finished, [this, reply, hash]() {
        if (reply->error() == QNetworkReply::NoError) {
            fim_on_response(hash, reply->readAll());
        } else {
            qCDebug(llamaLog) << "Error fetching completion:" << reply->errorString();
        }
        reply->deleteLater();
    });

    // gather some extra context nearby and process it in the background
    // only gather chunks if the cursor has moved a lot
    // TODO: something more clever? reranking?
    int delta_y = qAbs(pos_y - m_lastEditLineHash[editor->textDocument()->filePath()]);
    if (delta_y > 32) {
        // expand the prefix even further
        QStringList lines = getlines(editor,
                                     qMax(1, pos_y - settings().ringScope.value()),
                                     qMax(1, pos_y - settings().nPrefix.value()));
        pick_chunk(lines, false, false);

        // pick a suffix chunk
        const int max_y = currentDocument->lineCount();
        lines = getlines(editor,
                         qMin(max_y, pos_y + settings().nSuffix.value()),
                         qMin(max_y,
                              pos_y + settings().nSuffix.value()
                                  + settings().ringChunkSize.value()));
        pick_chunk(lines, false, false);

        m_lastEditLineHash[editor->textDocument()->filePath()] = pos_y;
    }
}

void LlamaPlugin::fim_on_response(const QByteArray &hash, const QByteArray &response)
{
    // TODO: Currently the cache uses a random eviction policy. A more clever policy could be implemented (eg. LRU).
    if (m_cacheData.size() > settings().maxCacheKeys.value()) {
        int randomIndex = QRandomGenerator::global()->bounded(m_cacheData.size());
        m_cacheData.removeIf([randomIndex](const auto &) {
            static int index = 0;
            return index++ == randomIndex;
        });
    }

    // Cache the result
    m_cacheData[hash] = response;

    // if nothing is currently displayed - show the hint directly
    if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget()) {
        if (!editor->suggestionVisible()) {
            QTextCursor cursor = editor->textCursor();
            int pos_x = cursor.positionInBlock();
            int pos_y = cursor.blockNumber() + 1;
            fim_try_hint(pos_x, pos_y);
        }
    }
}

LlamaPlugin::ThreeQStrings LlamaPlugin::getShowInfoStats(const QJsonObject &response)
{
    int n_cached = response["tokens_cached"].toInt();
    bool truncated = response["truncated"].toBool();

    int n_prompt = response["timings/prompt_n"].toInt();
    double t_prompt_ms = response["timings/prompt_ms"].toDouble(1.0);
    double s_prompt = response["timings/prompt_per_second"].toDouble();
    int n_predict = response["timings/predicted_n"].toInt();
    double t_predict_ms = response["timings/predicted_ms"].toDouble(1.0);
    double s_predict = response["timings/predicted_per_second"].toDouble();

    static QChar chars[4]{'|', '/', '-', '\\'};
    static int rotation = 0;
    QString label = QString("llama.cpp %1 %2 ms")
                        .arg(m_ringQueued.size() == 0 ? '|' : chars[rotation++ % 4])
                        .arg(t_prompt_ms + t_predict_ms, 0, 'f', 2);

    QString warningTooltip;
    if (truncated) {
        warningTooltip = QString(
                             "llama.cpp | WARNING: the context is full: %1, increase the server "
                             "context size or reduce ring_n_chunks %2 value in settings.")
                             .arg(n_cached)
                             .arg(settings().ringNChunks.value());
    }

    QString tooltip = QString(
                          "llama.cpp | c: %1, r: %2/%3, e: %4, q: %5/16, C: %6/%7 | p: %8 (%9 ms, "
                          "%10 t/s) | g: %11 (%12 ms, %13 t/s)")
                          .arg(n_cached)
                          .arg(m_ringChunks.size())
                          .arg(settings().ringNChunks.value())
                          .arg(m_ringNEvict)
                          .arg(m_ringQueued.size())
                          .arg(m_cacheData.size())
                          .arg(settings().maxCacheKeys.value())
                          .arg(n_prompt)
                          .arg(t_prompt_ms, 0, 'f', 2)
                          .arg(s_prompt, 0, 'f', 2)
                          .arg(n_predict)
                          .arg(t_predict_ms, 0, 'f', 2)
                          .arg(s_predict, 0, 'f', 2);

    return {label, tooltip, warningTooltip};
}

// try to generate a suggestion using the data in the cache
void LlamaPlugin::fim_try_hint(int pos_x, int pos_y)
{
    TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget();

    if (!editor || editor->isReadOnly() || editor->multiTextCursor().hasMultipleCursors())
        return;

    auto [prefix, middle, suffix] = fim_ctx_local(editor, pos_x, pos_y, {});

    QString context = prefix + middle + "Î" + suffix;
    QByteArray hash = QCryptographicHash::hash(context.toUtf8(), QCryptographicHash::Sha256).toHex();

    QByteArray raw;
    if (m_cacheData.contains(hash)) {
        raw = m_cacheData[hash];
    } else {
        QString pm = prefix + middle;
        int best = 0;
        QByteArray best_raw;

        for (int i = 0; i < 128; ++i) {
            if (pm.length() <= i)
                break;

            QString removed = pm.mid(pm.length() - (1 + i)); // last i+1 chars
            QString ctx_new = pm.left(pm.length() - (2 + i)) + "Î" + suffix;
            QByteArray hash_new
                = QCryptographicHash::hash(ctx_new.toUtf8(), QCryptographicHash::Sha256).toHex();

            if (m_cacheData.contains(hash_new)) {
                QByteArray response_cached = m_cacheData[hash_new];
                if (response_cached.isEmpty())
                    continue;

                QJsonParseError error;
                QJsonDocument doc = QJsonDocument::fromJson(response_cached, &error);
                if (error.error != QJsonParseError::NoError)
                    continue;

                QJsonObject obj = doc.object();
                QString content = obj["content"].toString();

                if (content.length() <= i)
                    continue;
                QString prefix_match = content.left(i + 1);

                if (prefix_match != removed)
                    continue;

                QString remaining_content = content.mid(i + 1);
                if (!remaining_content.isEmpty()) {
                    if (raw.isNull() || remaining_content.length() > best) {
                        best = remaining_content.length();
                        best_raw = response_cached;
                    }
                }
            }
        }

        raw = best_raw;
    }

    if (!raw.isNull() && !raw.isEmpty()) {
        fim_render(editor, pos_x, pos_y, raw);

        if (editor->suggestionVisible()) {
            // Call speculative FIM
            fim(pos_x, pos_y, true);
        }
    }
}

// render a suggestion at the current cursor location
void LlamaPlugin::fim_render(TextEditorWidget *editor,
                             int pos_x,
                             int pos_y,
                             const QByteArray &response)
{
    // do not show if there is a completion in progress
    if (editor->suggestionVisible())
        return;

    // Parse JSON response
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(response, &error);

    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON parse error:" << error.errorString();
        return;
    }

    QJsonObject obj = doc.object();
    qCDebug(llamaLog) << Q_FUNC_INFO << obj;

    QString content_str = obj.value("content").toString();
    QStringList content = content_str.split("\n", Qt::SkipEmptyParts);

    // Remove trailing new lines
    while (!content.isEmpty() && content.last().isEmpty()) {
        content.removeLast();
    }

    bool can_accept = !content_str.isEmpty() && !content.isEmpty();
    bool has_info = false;

    if (can_accept) {
        const int end = int(content_str.size()) - 1;
        int delta = 0;
        while (delta <= end && content_str[end - delta].isSpace())
            ++delta;

        if (delta > 0)
            content_str = content_str.chopped(delta);
    }

    // Get cursor position for hint

    QString nextText = Text::textAt(editor->document(),
                                    editor->textCursor().position(),
                                    content_str.size() * 2);

    if (nextText.contains(content_str)) {
        can_accept = false;
    } else if (nextText.contains(content.join("\n"))) {
        can_accept = false;
    } else if (content.size() > 1 && nextText.contains(content.mid(1).join("\n"))) {
        can_accept = false;
    }

    if (can_accept) {
        TextSuggestion::Data data;
        Text::Position currentPos = Text::Position::fromCursor(editor->textCursor());

        data.range.begin = currentPos;
        data.range.end = currentPos;
        data.position = currentPos;

        data.range.begin.column -= pos_x;
        int separator = content_str.indexOf("\n");
        data.range.end.column = pos_x
                                + (separator != -1 ? content_str.size() - separator - 1
                                                   : content_str.size());

        // Workaround for QTCREATORBUG-33303
        data.position.column -= pos_x;
        data.text = editor->textCursor().block().text() + content_str;

        editor->insertSuggestion(
            std::make_unique<TextEditor::TextSuggestion>(data, editor->document()));

        m_hintShown = true;
    }

    if (settings().showInfo.value() > 0) {
        m_textMark = std::make_unique<TextEditor::TextMark>(
            TextEditorWidget::currentTextEditorWidget()->textDocument(),
            pos_y,
            TextMarkCategory{"llama", "llama.cpp"});

        auto [label, tooltip, warningTooltip] = getShowInfoStats(obj);
        m_textMark->setLineAnnotation(label);
        m_textMark->setToolTip(warningTooltip.isEmpty() ? tooltip : warningTooltip);
        m_textMark->setColor(warningTooltip.isEmpty()
                                 ? Utils::Theme::CodeModel_Info_TextMarkColor
                                 : Utils::Theme::CodeModel_Warning_TextMarkColor);
    }
}

void LlamaPlugin::hideCompletionHint()
{
    m_textMark.reset({});

    if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget())
        editor->clearSuggestion();
}

LlamaPlugin::ThreeQStrings LlamaPlugin::fim_ctx_local(TextEditorWidget *editor,
                                                      int pos_x,
                                                      int pos_y,
                                                      const QByteArray &prev)
{
    QTextDocument *document = editor->document();

    QTextBlock block = document->findBlockByNumber(pos_y - 1);
    if (!block.isValid())
        return {};

    QString lineCur = block.text();
    QString lineCurPrefix = lineCur.left(pos_x);
    QString lineCurSuffix = lineCur.mid(pos_x);

    if (lineCurSuffix.size() > settings().maxLineSuffix.value())
        return {};

    // Get prefix lines
    QStringList linesPrefix;
    int startLine = qMax(1, pos_y - settings().nPrefix.value());
    for (int i = startLine; i < pos_y; ++i) {
        QTextBlock b = document->findBlockByNumber(i - 1);
        if (b.isValid()) {
            linesPrefix.append(b.text());
        }
    }

    // Get suffix lines
    QStringList linesSuffix;
    int endLine = qMin(document->lineCount(), pos_y + settings().nSuffix.value());
    for (int i = pos_y + 1; i <= endLine; ++i) {
        QTextBlock b = document->findBlockByNumber(i - 1);
        if (b.isValid()) {
            linesSuffix.append(b.text());
        }
    }

    const QString prefix = linesPrefix.join("\n") + "\n";
    const QString middle = lineCurPrefix;
    const QString suffix = lineCurSuffix + "\n" + linesSuffix.join("\n") + "\n";

    return {prefix, middle, suffix};
}

QStringList LlamaPlugin::getlines(TextEditorWidget *editor, int startLine, int endLine)
{
    QStringList lines;
    for (int i = startLine; i <= endLine; ++i) {
        QTextBlock block = editor->document()->findBlockByNumber(i - 1);
        if (block.isValid())
            lines.append(block.text());
    }
    return lines;
}

void LlamaPlugin::pick_chunk(const QStringList &text, bool noModifiedState, bool doEviction)
{
    if (settings().ringNChunks.value() <= 0)
        return;

    TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget();
    if (!editor)
        return;

    if (noModifiedState && editor->textDocument()->isModified())
        return;

    if (text.size() < 3)
        return;

    QStringList chunk;
    if (text.size() > settings().ringChunkSize.value()) {
        // Pick a random chunk
        int l0 = QRandomGenerator::global()->bounded(text.size()
                                                     - settings().ringChunkSize.value() / 2);
        int l1 = qMin(l0 + settings().ringChunkSize.value() / 2, text.size());

        chunk = text.mid(l0, l1 - l0);
    } else {
        chunk = text;
    }
    const QString chunkStr = chunk.join("\n") + "\n";

    // Check if already exists
    for (const Chunk &c : std::as_const(m_ringChunks)) {
        if (c.data == chunk)
            return;
    }
    for (const Chunk &c : std::as_const(m_ringQueued)) {
        if (c.data == chunk)
            return;
    }

    // Evict similar chunks
    for (int i = m_ringQueued.size() - 1; i >= 0; --i) {
        if (chunk_sim(m_ringQueued[i].data, chunk) > 0.9) {
            if (doEviction) {
                m_ringQueued.removeAt(i);
                m_ringNEvict++;
            } else {
                return;
            }
        }
    }
    for (int i = m_ringChunks.size() - 1; i >= 0; --i) {
        if (chunk_sim(m_ringChunks[i].data, chunk) > 0.9) {
            if (doEviction) {
                m_ringChunks.removeAt(i);
                m_ringNEvict++;
            } else {
                return;
            }
        }
    }

    // Add to queued
    Chunk newChunk;
    newChunk.data = chunk;
    newChunk.str = chunkStr;
    newChunk.time = QDateTime::currentDateTime();
    newChunk.filename = editor->textDocument()->filePath();
    m_ringQueued.append(newChunk);

    if (m_ringQueued.size() > 16)
        m_ringQueued.removeFirst();
}

void LlamaPlugin::ring_update()
{
    if (m_ringQueued.isEmpty())
        return;

    // Move first queued chunk to ring buffer
    if (m_ringChunks.size() >= settings().ringNChunks.value()) {
        m_ringChunks.removeFirst();
    }

    Chunk chunk = m_ringQueued.takeFirst();
    m_ringChunks.append(chunk);

    // Send request to update context on server
    QJsonObject request;
    request["input_prefix"] = "";
    request["input_suffix"] = "";
    request["prompt"] = "";
    request["n_predict"] = 0;
    request["temperature"] = 0.0;
    request["stream"] = false;
    request["samplers"] = QJsonArray::fromStringList({});
    request["cache_prompt"] = true;
    request["t_max_prompt_ms"] = 1;
    request["t_max_predict_ms"] = 1;
    request["response_fields"] = QJsonArray::fromStringList({});

    // Add extra context
    QJsonArray extraContext;
    for (const Chunk &c : std::as_const(m_ringChunks)) {
        QJsonObject chunkObj;
        chunkObj["text"] = c.str;
        chunkObj["time"] = c.time.toString(Qt::ISODate);
        chunkObj["filename"] = c.filename.path();
        extraContext.append(chunkObj);
    }
    request["input_extra"] = extraContext;

    // Create JSON document
    QJsonDocument doc(request);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    // Send request to server
    QNetworkRequest req(QUrl(settings().endpoint.value()));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().apiKey.value().isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + settings().apiKey.value().toUtf8());
    }

    m_networkManager->post(req, jsonData);
}

} // namespace LlamaCpp::Internal
