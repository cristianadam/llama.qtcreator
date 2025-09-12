#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/dialogs/ioptionspage.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/icore.h>
#include <coreplugin/messagemanager.h>
#include <coreplugin/statusbarmanager.h>
#include <extensionsystem/pluginmanager.h>
#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>
#include <texteditor/textmark.h>
#include <texteditor/textsuggestion.h>
#include <utils/fileutils.h>
#include <utils/hostosinfo.h>
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
#include <QTranslator>

#include "llamaplugin.h"
#include "llamachateditor.h"
#include "llamaconversationsview.h"
#include "llamaconstants.h"
#include "llamaicons.h"
#include "llamalocatorfilter.h"
#include "llamaprojectpanel.h"
#include "llamasettings.h"
#include "llamatr.h"


using namespace Core;
using namespace TextEditor;
using namespace Utils;
using namespace ProjectExplorer;

Q_LOGGING_CATEGORY(llamaLog, "llama.cpp", QtWarningMsg)
Q_LOGGING_CATEGORY(llamaNetwork, "llama.cpp.network", QtWarningMsg)

namespace LlamaCpp {

QRegularExpression LlamaPlugin::s_whitespace_regex("^\\s*$");

LlamaPlugin::LlamaPlugin()
    : m_ringUpdateTimer(new QTimer(this))
    , m_positionChangedTimer(new QTimer(this))
    , m_networkManager(new QNetworkAccessManager(this))
{
    connect(m_ringUpdateTimer, &QTimer::timeout, this, &LlamaPlugin::ring_update);
    m_positionChangedTimer->setSingleShot(true);
    connect(m_positionChangedTimer,
            &QTimer::timeout,
            this,
            &LlamaPlugin::handleCursorPositionChangedDelayed);

    QFontDatabase::addApplicationFont(":/images/heroicons_outline.ttf");
}

LlamaPlugin::~LlamaPlugin()
{
    disconnect(m_ringUpdateTimer, &QTimer::timeout, this, &LlamaPlugin::ring_update);
    disconnect(m_positionChangedTimer,
               &QTimer::timeout,
               this,
               &LlamaPlugin::handleCursorPositionChangedDelayed);
}

FilePath LlamaPlugin::getTranslationFilePath(const QString &translationFile)
{
    const FilePath pluginLocation = ExtensionSystem::PluginManager::specForPlugin(this)->location();
    const FilePath translationDir = HostOsInfo::isMacHost()
                                        ? (pluginLocation / "../../Resources/translations")
                                        : (pluginLocation / "../../../share/qtcreator/translations")
                                              .cleanPath();
    return translationDir / translationFile;
}

void LlamaPlugin::initialize()
{
    // Translations
    auto translator = new QTranslator(this);
    QString locale = ICore::userInterfaceLanguage();
    locale = locale.contains("zh_") ? locale : locale.left(locale.indexOf("_"));
    const QString languageFile = "llamacpp_" + locale + ".qm";
    const QString llamaCppTranslation = getTranslationFilePath(languageFile).path();
    if (translator->load(llamaCppTranslation))
        QCoreApplication::installTranslator(translator);

    IOptionsPage::registerCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY,
                                   Constants::LLAMACPP_GENERAL_OPTIONS_DISPLAY_CATEGORY,
                                   ":/images/settingscategory_llama.png");

    MenuBuilder(Constants::LLAMACPP_MENU_ID)
        .setTitle(Tr::tr("llama.cpp"))
        .setIcon(LlamaCpp::LLAMACPP_ICON.icon())
        .setOnAllDisabledBehavior(ActionContainer::Show)
        .addToContainer(Core::Constants::M_TOOLS);

    Core::Command *newConversationCmd
        = ActionManager::registerAction(&m_newConversation, Constants::LLAMACPP_NEW_CONVERSATION);
    connect(&m_newConversation, &QAction::triggered, this, [this] {
        QString title("llama.cpp coversation");
        Core::EditorManager::openEditorWithContents(Constants::LLAMACPP_VIEWER_ID, &title);
    });
    ActionManager::actionContainer(Constants::LLAMACPP_MENU_ID)->addAction(newConversationCmd);

    ActionBuilder requestAction(this, Constants::LLAMACPP_REQUEST_SUGGESTION);
    requestAction.setText(Tr::tr("Request llama.cpp Suggestion"));
    requestAction.setToolTip(
        Tr::tr("Request llama.cpp suggestion at the current editor's cursor position."));
    requestAction.addOnTriggered(this, [this] {
        if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget()) {
            QTextCursor cursor = editor->textCursor();
            int pos_x = cursor.positionInBlock();
            int pos_y = cursor.blockNumber() + 1;
            fim(pos_x, pos_y, false);
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

    ActionBuilder toggleAutoFimAction(this, Constants::LLAMACPP_TOGGLE_AUTOFIM);
    toggleAutoFimAction.setText(Tr::tr("Toggle Auto FIM"));
    toggleAutoFimAction.setCheckable(true);
    toggleAutoFimAction.setChecked(settings().autoFim());
    toggleAutoFimAction.addOnTriggered(this, [this](bool checked) {
        qCInfo(llamaLog) << "Toggle Auto FIM" << checked;

        settings().autoFim.setValue(checked);
        settings().apply();

        if (checked) {
            // Show the llama.cpp text hint
            fim(-1, -1, false);
        } else {
            hideCompletionHint();
        }
    });
    toggleAutoFimAction.setDefaultKeySequence(Tr::tr("Ctrl+Shift+G"));

    QAction *toggleAct = toggleAction.contextAction();
    QAction *requestAct = requestAction.contextAction();
    QAction *toogleAutoFimAct = toggleAutoFimAction.contextAction();
    auto updateActions = [toggleAct, requestAct, toogleAutoFimAct] {
        const bool enabled = settings().enableLlamaCpp();
        toggleAct->setToolTip(enabled ? Tr::tr("Disable llama.cpp.") : Tr::tr("Enable llama.cpp."));
        toggleAct->setChecked(enabled);
        requestAct->setEnabled(enabled);
        toogleAutoFimAct->setEnabled(enabled);
    };

    settings().enableLlamaCpp.addOnChanged(this, updateActions);

    updateActions();

    auto toggleButton = new QToolButton;
    toggleButton->setDefaultAction(toggleAction.contextAction());
    StatusBarManager::addStatusBarWidget(toggleButton, StatusBarManager::RightCorner);

    setupLlamaCppProjectPanel();
    setupConversationViewWidgetFactory();
    setupLocatorFilter();

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
        if (qApp->clipboard()->text().isEmpty() || !settings().enableLlamaCpp())
            return;
        pick_chunk(qApp->clipboard()->text().split("\n"), false, true);
    });

    setupChatEditor();
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

    m_cacheData.setMaxCost(settings().maxCacheKeys.value());

    if (!settings().enableLlamaCpp()) {
        hideCompletionHint();

        if (m_fimReply && m_fimReply->isRunning())
            m_fimReply->abort();

        m_cacheData.clear();
        m_lastEditLineHash.clear();
        m_ringChunks.clear();
        m_ringQueued.clear();
    }
}

ExtensionSystem::IPlugin::ShutdownFlag LlamaPlugin::aboutToShutdown()
{
    return SynchronousShutdown;
}

void LlamaPlugin::handleCurrentEditorChanged(Core::IEditor *editor)
{
    if (!editor || !settings().enableLlamaCpp())
        return;

    TextEditorWidget *editorWidget = TextEditorWidget::fromEditor(editor);
    if (!isValid(editorWidget))
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
    if (!editor || !settings().enableLlamaCpp())
        return;

    TextEditorWidget *editorWidget = TextEditorWidget::fromEditor(editor);
    if (!isValid(editorWidget))
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
    if (!settings().enableLlamaCpp())
        return;

    hideCompletionHint();
    m_positionChangedTimer->start(100);
}

void LlamaPlugin::handleCursorPositionChangedDelayed()
{
    TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget();
    if (!isValid(editor))
        return;

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
    if (!isValid(editor) || !document || !settings().enableLlamaCpp())
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

void LlamaPlugin::fim(int pos_x, int pos_y, bool isAuto, const QStringList &prev)
{
    TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget();
    if (!isValid(editor))
        return;

    QTextDocument *currentDocument = editor->document();
    if (!currentDocument)
        return;

    if (pos_x < 0 && pos_y < 0) {
        QTextCursor cursor = editor->textCursor();
        pos_x = cursor.positionInBlock();
        pos_y = cursor.blockNumber() + 1;
    }

    qCInfo(llamaLog) << "fim:" << pos_x << pos_y << (prev.isEmpty() ? "" : "previous content!");

    // avoid sending repeated requests too fast
    if (m_fimReply && m_fimReply->isRunning()) {
        qCInfo(llamaLog) << "fim:" << pos_x << pos_y
                         << "There is a fim network request is in progress, re-trying in 100ms";
        QTimer::singleShot(100, [this, isAuto, prev]() {
            // If the cursor has been moved, use the the actual cursor postion
            // for a more up to date fim call.
            fim(-1, -1, isAuto, prev);
        });
        return;
    }

    // Get local context
    auto [prefix, middle, suffix, line_cur, line_cur_prefix, line_cur_suffix]
        = fim_ctx_local(editor, pos_x, pos_y, prev);

    if (isAuto && line_cur_suffix.size() > settings().maxLineSuffix.value())
        return;

    // Check cache first
    const QByteArray hash = QCryptographicHash::hash((prefix + middle + "Î" + suffix).toUtf8(),
                                                     QCryptographicHash::Sha256)
                                .toHex();

    QByteArrayList hashes{hash};

    // compute multiple hashes that can be used to generate a completion for which the first few lines
    // are missing. this happens when we have scrolled down a bit from where the original generation was done
    static QRegularExpression re("^[^\n]*\n");
    QString prefix_trim = prefix;
    for (int i = 0; i < 3; ++i) {
        prefix_trim = prefix_trim.replace(re, "");
        if (prefix_trim.isEmpty())
            break;
        hashes << QCryptographicHash::hash((prefix_trim + middle + "Î" + suffix).toUtf8(),
                                           QCryptographicHash::Sha256)
                      .toHex();
    }

    // if we already have a cached completion for one of the hashes, don't send a request
    for (const QByteArray &h : std::as_const(hashes)) {
        if (m_cacheData.contains(h)) {
            // On explicit Ctrl+G fim call, display the suggestion
            if (!isAuto)
                fim_try_hint(pos_x, pos_y);
            return;
        }
    }

    // Create JSON request
    QJsonObject request;

    QStringList stopStrings = settings().stopStrings.value().isEmpty()
                                  ? QStringList()
                                  : settings().stopStrings.value().split(";");
    request["prompt"] = middle;
    request["n_predict"] = settings().nPredict.value();
    request["stop"] = QJsonArray::fromStringList(stopStrings);
    request["top_k"] = 40;
    request["top_p"] = 0.9;
    request["stream"] = false;
    request["samplers"] = QJsonArray::fromStringList({"top_k", "top_p", "infill"});
    request["cache_prompt"] = true;
    request["t_max_prompt_ms"] = settings().tMaxPromptMs.value();
    // the first request is quick - we will launch a speculative request after this one is displayed
    request["t_max_predict_ms"] = prev.isEmpty() ? 250 : settings().tMaxPredictMs.value();
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

    // evict chunks that are very similar to the current context
    // this is needed because such chunks usually distort the completion to repeat what was already there
    // for (int i = m_ringChunks.size() - 1; i >= 0; --i) {
    //     if (chunk_sim(m_ringChunks[i].data, chunk) > 0.5) {
    //         m_ringChunks.removeAt(i);
    //         m_ringNEvict++;
    //     }
    // }

    // Add extra context
    QJsonArray extraContext;
    for (const Chunk &chunk : std::as_const(m_ringChunks)) {
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

    qCInfo(llamaLog) << "fim:" << pos_x << pos_y << "Sending network request.";
    qCDebug(llamaNetwork).noquote() << "fim: post request:\n" << doc.toJson();

    QElapsedTimer replyTimer;
    replyTimer.start();
    m_fimReply.reset(m_networkManager->post(req, jsonData));

    // Connect to response
    connect(m_fimReply.get(), &QNetworkReply::finished, [this, hash, pos_x, pos_y, replyTimer]() {
        qCInfo(llamaLog) << "fim_on_response: received reply after:" << replyTimer.elapsed()
                         << "ms";
        if (m_fimReply->error() == QNetworkReply::NoError) {
            fim_on_response(pos_x, pos_y, hash, m_fimReply->readAll());
        } else {
            Core::MessageManager::writeSilently(
                Tr::tr("[llama.cpp] Error fetching fim completion from %1: %2")
                    .arg(settings().endpoint.value())
                    .arg(m_fimReply->errorString()));
        }
        m_fimReply.release()->deleteLater();
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

void LlamaPlugin::fim_on_response(int pos_x,
                                  int pos_y,
                                  const QByteArray &hash,
                                  const QByteArray &response)
{
    qCInfo(llamaLog) << "fim_on_response:" << pos_x << pos_y;

    // Cache the result
    m_cacheData.insert(hash, new QByteArray(response));

    // if nothing is currently displayed - show the hint directly
    if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget()) {
        QTextCursor cursor = editor->textCursor();
        int cursor_pos_x = cursor.positionInBlock();
        int cursor_pos_y = cursor.blockNumber() + 1;

        if (cursor_pos_x == pos_x && cursor_pos_y == pos_y) {
            fim_try_hint(pos_x, pos_y);
        } else {
            qCInfo(llamaLog) << "fim_on_response:" << "Received response for:" << pos_x << pos_y
                             << "and the cursor is at" << cursor_pos_x << cursor_pos_y
                             << "ignoring.";
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

    QString label = QString("llama.cpp | %1 ms").arg(t_prompt_ms + t_predict_ms, 0, 'f', 2);

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
    if (!isValid(editor))
        return;

    auto [prefix, middle, suffix, line_cur, line_cur_prefix, line_cur_suffix]
        = fim_ctx_local(editor, pos_x, pos_y);

    QString context = prefix + middle + "Î" + suffix;
    QByteArray hash = QCryptographicHash::hash(context.toUtf8(), QCryptographicHash::Sha256).toHex();

    QByteArray raw;
    if (m_cacheData.contains(hash)) {
        raw = *m_cacheData[hash];
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
                QByteArray response_cached = *m_cacheData[hash_new];
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
            fim(pos_x, pos_y, true, m_suggestionContent);
        }
    }
}

// render a suggestion at the current cursor location
void LlamaPlugin::fim_render(TextEditorWidget *editor,
                             int pos_x,
                             int pos_y,
                             const QByteArray &response)
{
    // do not show a suggestion if we have a selection
    if (!editor->selectedText().isEmpty())
        return;

    // Parse JSON response
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(response, &error);

    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON parse error:" << error.errorString();
        return;
    }

    QJsonObject obj = doc.object();

    QString content_str = obj.value("content").toString();
    QStringList content = content_str.split("\n");

    qCInfo(llamaLog) << "fim_render:" << pos_x << pos_y << "Raw suggestion:" << content_str;
    qDebug(llamaNetwork()).noquote() << "fim_render: response:\n" << doc.toJson();

    bool can_accept = true;

    // Remove trailing new lines
    while (!content.isEmpty() && content.last().isEmpty()) {
        content.removeLast();
    }
    if (content.isEmpty()) {
        content << "";
        can_accept = false;
    }

    QString line_cur = getline(editor, pos_y - 1);

    // if the current has too much whitespace, more than the current indent, trim
    // so that we don't have double indentation.
    auto line_cur_match = s_whitespace_regex.match(line_cur);
    if (line_cur_match.hasMatch()) {
        int lead = qMin(line_cur_match.capturedLength(), line_cur.size());
        if (lead > pos_x) {
            line_cur = line_cur.left(pos_x);
            content[0] = content[0].mid(pos_x);
        }
    }
    QString line_cur_prefix = line_cur.left(pos_x);
    QString line_cur_suffix = line_cur.mid(pos_x);

    // Logic for discarding predictions that repeat existing text
    // truncate the suggestion if the first line is empty
    if (content.size() == 1 && content[0].isEmpty()) {
        content = {};
    }

    // or the suggestion repeats the previous line
    int cmp_y = pos_y - 2;
    while (cmp_y > 1 && getline(editor, cmp_y).trimmed().isEmpty()) {
        cmp_y--;
    }
    if (cmp_y > 1 && content.size() == 1
        && (line_cur_prefix + content[0]) == getline(editor, cmp_y)) {
        content = {};
    }

    // ... and the next lines are repeated
    if (content.size() > 1 && content[0].isEmpty()
        && content.mid(1) == getlines(editor, pos_y, pos_y + content.size() - 1)) {
        content = {};
    }

    // truncate the suggestion if it repeats the suffix
    if (content.size() == 1 && content[0] == line_cur_suffix) {
        content = {};
    }

    // Find the first non-empty line
    cmp_y = pos_y;
    while (cmp_y < editor->document()->lineCount() && getline(editor, cmp_y).trimmed().isEmpty()) {
        cmp_y++;
    }

    if (!content.isEmpty() && (line_cur_prefix + content[0]) == getline(editor, cmp_y)) {
        // truncate the suggestion if it repeats the next line
        if (content.size() == 1) {
            content = {};
        }

        // ... or if the second line of the suggestion is the prefix of line cmp_y + 1
        if (content.size() == 2
            && content.back() == getline(editor, cmp_y + 1).left(content.back().length())) {
            content = {};
        }

        // ... or if the middle chunk of lines of the suggestion is the same as [cmp_y + 1, cmp_y + content.size() - 1)
        if (content.size() > 2
            && content.mid(1).join("\n")
                   == getlines(editor, cmp_y + 1, cmp_y + content.size() - 1).join("\n")) {
            content = {};
        }
    }

    // if only whitespaces - do not accept
    QString combined_content = content.join("\n");
    if (s_whitespace_regex.match(combined_content).hasMatch())
        can_accept = false;

    if (can_accept) {
        // Text:positionInText has 1 based line and column values
        int currentIntPos = Text::positionInText(editor->document(), pos_y, pos_x + 1);
        TextSuggestion::Data data;
        Text::Position currentPos = Text::Position::fromPositionInDocument(editor->document(),
                                                                           currentIntPos);
        data.range.begin = currentPos;
        data.range.end = currentPos;
        data.position = currentPos;

        int separator = combined_content.indexOf("\n");
        data.range.end.column = separator != -1 ? combined_content.size() - separator - 1
                                                : combined_content.size();
        data.text = combined_content;

        if (m_suggestionContent != content) {
            auto suggestion = std::make_unique<TextEditor::TextSuggestion>(data, editor->document());
            suggestion->replacementDocument()->setPlainText(line_cur_prefix + combined_content
                                                            + line_cur_suffix);

            editor->insertSuggestion(std::move(suggestion));

            qCInfo(llamaLog) << "fim_render:" << pos_x << pos_y
                             << "Prepared suggestion:" << combined_content;
            if (!m_suggestionContent.isEmpty())
                qCInfo(llamaLog) << "The replaced suggestion:" << m_suggestionContent.join("\n");

            m_suggestionContent = content;
        }
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

    if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget()) {
        editor->clearSuggestion();
        m_suggestionContent.clear();
    }
}

bool LlamaPlugin::isValid(TextEditor::TextEditorWidget *editor)
{
    if (!editor || editor->isReadOnly() || editor->multiTextCursor().hasMultipleCursors())
        return false;
    return true;
}

LlamaPlugin::FimContext LlamaPlugin::fim_ctx_local(TextEditorWidget *editor,
                                                   int pos_x,
                                                   int pos_y,
                                                   const QStringList &prev)
{
    QTextDocument *document = editor->document();
    int max_y = document->lineCount();

    QString lineCur;
    QString lineCurPrefix;
    QString lineCurSuffix;
    QStringList linesPrefix;
    QStringList linesSuffix;

    if (prev.isEmpty()) {
        // No previous completion
        lineCur = getline(editor, pos_y - 1);

        lineCurPrefix = lineCur.left(pos_x);
        lineCurSuffix = lineCur.mid(pos_x);

        int startLine = qMax(1, pos_y - settings().nPrefix.value());
        for (int i = startLine; i < pos_y; ++i)
            linesPrefix << getline(editor, i - 1);

        int endLine = qMin(max_y, pos_y + settings().nSuffix.value());
        for (int i = pos_y + 1; i <= endLine; ++i)
            linesSuffix << getline(editor, i - 1);
    } else {
        // With previous completion
        if (prev.size() == 1)
            lineCur = getline(editor, pos_y - 1) + prev.first();
        else
            lineCur = prev.last(); // Use the last item of prev as current line

        lineCurPrefix = lineCur;
        lineCurSuffix.clear();

        int startLine = qMax(1, pos_y - settings().nPrefix.value() + prev.size() - 1);
        for (int i = startLine; i < pos_y; ++i)
            linesPrefix << getline(editor, i - 1);

        // Add modified previous lines to prefix
        if (prev.size() > 1) {
            linesPrefix << getline(editor, pos_y - 1) + prev.first();
            for (int i = 1; i < prev.size() - 1; ++i) {
                linesPrefix << prev[i];
            }
        }

        int endLine = qMin(max_y, pos_y + settings().nSuffix.value());
        for (int i = pos_y + 1; i <= endLine; ++i)
            linesSuffix << getline(editor, i - 1);
    }

    const QString prefix = linesPrefix.join("\n") + "\n";
    const QString middle = lineCurPrefix;
    const QString suffix = lineCurSuffix + "\n" + linesSuffix.join("\n") + "\n";

    FimContext res;
    res.prefix = prefix;
    res.middle = middle;
    res.suffix = suffix;
    res.line_cur = lineCur;
    res.line_cur_prefix = lineCurPrefix;
    res.line_cur_suffix = lineCurSuffix;

    return res;
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

QString LlamaPlugin::getline(TextEditorWidget *editor, int line)
{
    QTextBlock block = editor->document()->findBlockByNumber(line);
    return block.isValid() ? block.text() : QString();
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

    qCInfo(llamaLog) << "ring_update: ring chunks:" << m_ringChunks.size() << "ring queued"
                     << m_ringQueued.size();

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

} // namespace LlamaCpp
