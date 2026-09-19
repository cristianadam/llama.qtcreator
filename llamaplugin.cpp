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
#include <QElapsedTimer>
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
#include <QUrl>

#include "llamachateditor.h"
#include "llamachatmanager.h"
#include "llamaconstants.h"
#include "llamaconversationsview.h"
#include "llamaicons.h"
#include "llamalocatorfilter.h"
#include "llamaplugin.h"
#include "llamaprojectpanel.h"
#include "llamasettings.h"
#include "llamaspinner.h"
#include "llamatr.h"
#include "tools/factory.h"
#include "tools/mcpbridge.h"
#include "tools/mcpclient.h"

using namespace Core;
using namespace TextEditor;
using namespace Utils;
using namespace ProjectExplorer;

Q_LOGGING_CATEGORY(llamaLog, "llama.cpp", QtWarningMsg)
Q_LOGGING_CATEGORY(llamaNetwork, "llama.cpp.network", QtWarningMsg)

Q_IMPORT_PLUGIN(SpinnerPlugin);

namespace LlamaCpp {

QRegularExpression LlamaPlugin::s_whitespace_regex("^\\s*$");
QRegularExpression LlamaPlugin::s_indent_regex("^[ \\t]*");

// the ring-buffer chunks are only processed once the user has been idle for this long
constexpr int kRingIdleDelayMs = 3000;

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
    // Publish the tools served by the Qt Creator MCP server (building,
    // running, opening projects, …) as chat tools.
    ToolFactory::instance().setRemoteToolProvider(&McpBridge::instance());

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
        QString title(Tr::tr("llama.cpp coversation"));
        Conversation c = ChatManager::instance().createConversation(title);
        Core::EditorManager::openEditorWithContents(Constants::LLAMACPP_VIEWER_ID,
                                                    &title,
                                                    c.id.toUtf8(),
                                                    c.id);
    });

    Core::Command *requestCmd
        = ActionManager::registerAction(&m_requestAction, Constants::LLAMACPP_REQUEST_SUGGESTION);
    m_requestAction.setToolTip(
        Tr::tr("Request llama.cpp suggestion at the current editor's cursor position."));
    connect(&m_requestAction, &QAction::triggered, this, [this] {
        if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget()) {
            QTextCursor cursor = editor->textCursor();
            int pos_x = cursor.positionInBlock();
            int pos_y = cursor.blockNumber() + 1;
            fim(pos_x, pos_y, false);
        }
    });
    requestCmd->setDefaultKeySequence(Tr::tr("Ctrl+G"));

    Core::Command *toggleCmd
        = ActionManager::registerAction(&m_toggleAction, Constants::LLAMACPP_TOGGLE_ENABLE_DISABLE);
    m_toggleAction.setCheckable(true);
    m_toggleAction.setChecked(settings().enableLlamaCpp());
    m_toggleAction.setIcon(LLAMACPP_ICON.icon());
    connect(&m_toggleAction, &QAction::triggered, this, [this](bool checked) {
        settings().enableLlamaCpp.setValue(checked);
        settings().apply();
    });

    Core::Command *toggleAutoFimCmd
        = ActionManager::registerAction(&m_toogleAutoFimAction, Constants::LLAMACPP_TOGGLE_AUTOFIM);
    m_toogleAutoFimAction.setCheckable(true);
    m_toogleAutoFimAction.setChecked(settings().autoFim());
    connect(&m_toogleAutoFimAction, &QAction::triggered, this, [this](bool checked) {
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
    toggleAutoFimCmd->setDefaultKeySequence(Tr::tr("Ctrl+Shift+G"));

    Core::Command *nextCompletionCmd =
        ActionManager::registerAction(&m_nextCompletionAction, Constants::LLAMACPP_NEXT_COMPLETION);
    m_nextCompletionAction.setToolTip(Tr::tr(
        "Cycle to the next cached completion candidate at the current position (press Ctrl+G, "
        "then Down)."));
    connect(&m_nextCompletionAction, &QAction::triggered, this, [this] { fim_cycle(1); });
    nextCompletionCmd->setDefaultKeySequence(Tr::tr("Ctrl+G Down"));

    Core::Command *prevCompletionCmd = ActionManager::registerAction(&m_prevCompletionAction,
                                                                     Constants::LLAMACPP_PREV_COMPLETION);
    m_prevCompletionAction.setToolTip(Tr::tr(
        "Cycle to the previous cached completion candidate at the current position (press Ctrl+G, "
        "then Up)."));
    connect(&m_prevCompletionAction, &QAction::triggered, this, [this] { fim_cycle(-1); });
    prevCompletionCmd->setDefaultKeySequence(Tr::tr("Ctrl+G Up"));

    Core::Command *statusCmd
        = ActionManager::registerAction(&m_statusAction, Constants::LLAMACPP_SHOW_SERVER_STATUS);
    m_statusAction.setToolTip(
        Tr::tr("Query the llama.cpp servers and report which models are loaded."));
    connect(&m_statusAction, &QAction::triggered, this, [this] { checkServerStatus(); });

    auto menuContainer = ActionManager::actionContainer(Constants::LLAMACPP_MENU_ID);
    menuContainer->addAction(newConversationCmd);
    menuContainer->addSeparator();
    menuContainer->addAction(requestCmd);
    menuContainer->addAction(toggleCmd);
    menuContainer->addAction(toggleAutoFimCmd);
    menuContainer->addAction(statusCmd);

    auto updateActions = [this] {
        const bool enabled = settings().enableLlamaCpp();
        m_toggleAction.setToolTip(enabled ? Tr::tr("Disable llama.cpp.") : Tr::tr("Enable llama.cpp."));
        m_toggleAction.setChecked(enabled);
        m_requestAction.setEnabled(enabled);
        m_toogleAutoFimAction.setEnabled(enabled);
        m_nextCompletionAction.setEnabled(enabled);
        m_prevCompletionAction.setEnabled(enabled);
    };

    settings().enableLlamaCpp.addOnChanged(this, updateActions);

    updateActions();

    auto toggleButton = new QToolButton;
    toggleButton->setDefaultAction(&m_toggleAction);
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

#ifdef WITH_TESTS
    // Registered so the real usage (builtin Qt Creator MCP server,
    // McpBridge, ToolFactory) can be tested from the command line:
    //   Qt Creator -pluginpath <build dir> -test llamacpp[,McpClientTest]
    addTestCreator(&Internal::createMcpClientTest);
#endif
}

void LlamaPlugin::extensionsInitialized()
{
    // Start the connection only once all plugins are initialized: the
    // builtin MCP server (and the plugins that contribute its tools) is
    // fully up by then, so the handshake does not race against tool
    // registration. (McpClient's tools/list retry covers servers that
    // register their tools even later.)
    McpBridge::instance().start();
}

bool LlamaPlugin::delayedInitialize()
{
    settingsUpdated();
    connect(&settings(), &AspectContainer::applied, this, &LlamaPlugin::settingsUpdated);

    return true;
}

void LlamaPlugin::settingsUpdated()
{
    // The cached completions and the in-flight request are tied to the previous
    // endpoint/model and must not be reused once any of them changes.
    const bool endpointChanged = m_lastEndpoint != settings().endpoint.value()
                                 || m_lastApiKey != settings().apiKey.value()
                                 || m_lastModel != settings().modelFim.value();
    m_lastEndpoint = settings().endpoint.value();
    m_lastApiKey = settings().apiKey.value();
    m_lastModel = settings().modelFim.value();

    if (endpointChanged) {
        m_cacheData.clear();
        m_suggestionResponses.clear();
        m_selectedCompletion = 0;
        if (m_fimReply && m_fimReply->isRunning())
            m_fimReply->abort();
    }

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
    m_lastUserActivity.restart();

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
    auto [prefix, middle, suffix, line_cur, line_cur_prefix, line_cur_suffix, indent]
        = fim_ctx_local(editor, pos_x, pos_y, prev);

    if (isAuto && line_cur_suffix.size() > settings().maxLineSuffix.value())
        return;

    m_indentLast = indent;

    // compute multiple hashes that can be used to generate a completion for which the first few lines
    // are missing. this happens when we have scrolled down a bit from where the original generation was done
    const QByteArrayList hashes = Fim::contextHashes(prefix, middle, suffix);

    // if we already have a cached completion for one of the hashes, don't send a request
    if (m_cacheData.containsAny(hashes)) {
        // On explicit Ctrl+G fim call, display the suggestion
        if (!isAuto)
            fim_try_hint(pos_x, pos_y);
        return;
    }

    // Create JSON request
    QJsonObject request;

    QStringList stopStrings = settings().stopStrings.value().isEmpty()
                                  ? QStringList()
                                  : settings().stopStrings.value().split(";");
    request["prompt"] = middle;
    request["n_predict"] = settings().nPredict.value();
    request["stop"] = QJsonArray::fromStringList(stopStrings);
    request["n_cmpl"] = settings().nCmpl.value();
    request["n_indent"] = indent;
    if (!settings().modelFim.value().isEmpty())
        request["model"] = settings().modelFim.value();
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

    int startLine = 1;
    int endLine = currentDocument->lineCount();
    if (currentDocument->lineCount() > settings().ringChunkSize.value()) {
        startLine = qMax(startLine, pos_y - settings().ringChunkSize.value() / 2);
        endLine = qMin(endLine, pos_y + settings().ringChunkSize.value() / 2);
    }
    const QStringList lines = getlines(editor, startLine, endLine);

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
    // (like llama.vim, the oldest chunk at index 0 is not considered here)
    for (int i = m_ringChunks.size() - 1; i > 0; --i) {
        if (Fim::chunkSim(m_ringChunks[i].data, chunk) > 0.5) {
            m_ringChunks.removeAt(i);
            m_ringNEvict++;
        }
    }

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
    connect(m_fimReply.get(),
            &QNetworkReply::finished,
            [this, hashes, pos_x, pos_y, replyTimer]() {
                qCInfo(llamaLog) << "fim_on_response: received reply after:"
                                 << replyTimer.elapsed() << "ms";
                if (m_fimReply->error() == QNetworkReply::NoError) {
                    fim_on_response(pos_x, pos_y, hashes, m_fimReply->readAll());
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
                                  const QByteArrayList &hashes,
                                  const QByteArray &response)
{
    qCInfo(llamaLog) << "fim_on_response:" << pos_x << pos_y;

    // Cache the results: each key maps to a ring buffer of up to nCmpl entries.
    // Insert under all hashes, so that a lookup with a slightly shorter prefix
    // (cursor moved up a few lines) hits the cache instead of re-requesting.
    m_cacheData.insert(hashes, response, settings().nCmpl.value());

    // if nothing is currently displayed - show the hint directly
    if (auto editor = TextEditor::TextEditorWidget::currentTextEditorWidget()) {
        QTextCursor cursor = editor->textCursor();
        int cursor_pos_x = cursor.positionInBlock();
        int cursor_pos_y = cursor.blockNumber() + 1;

        if (cursor_pos_x == pos_x && cursor_pos_y == pos_y && !editor->suggestionVisible()) {
            fim_try_hint(pos_x, pos_y);
        } else {
            qCInfo(llamaLog) << "fim_on_response:" << "Received response for:" << pos_x << pos_y
                             << "and the cursor is at" << cursor_pos_x << cursor_pos_y
                             << "or a suggestion is already displayed, ignoring.";
        }
    }
}

LlamaPlugin::ThreeQStrings LlamaPlugin::getShowInfoStats(const QJsonObject &response)
{
    int n_cached = response["tokens_cached"].toInt();
    // the server reports truncation under "timings", accept the top-level
    // field and the flat "timings/truncated" field (as returned when
    // response_fields filtering is used) as well
    bool truncated = response.value("timings").toObject().value("truncated").toBool()
                     || response["truncated"].toBool();
    if (!truncated) {
        const QJsonValue v = response["timings/truncated"];
        truncated = v.isBool() ? v.toBool()
                  : v.toString() == QLatin1String("true");
    }

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

    auto [prefix, middle, suffix, line_cur, line_cur_prefix, line_cur_suffix, indent]
        = fim_ctx_local(editor, pos_x, pos_y);

    // Phase 1: exact match at the current position
    const QByteArray hash = Fim::contextHashes(prefix, middle, suffix).first();
    QList<QByteArray> *cached = m_cacheData.lookup(hash);

    if (cached && !cached->isEmpty()) {
        const QList<QByteArray> &responses = *cached;
        // keep the cycling position when the same set of completions is re-displayed
        if (m_suggestionPos != QPoint(pos_x, pos_y) || m_suggestionResponses != responses)
            m_selectedCompletion = 0;
        m_selectedCompletion = qBound(0, m_selectedCompletion, responses.size() - 1);
        fim_render(editor, pos_x, pos_y, responses, m_selectedCompletion);

        if (editor->suggestionVisible()) {
            // Call speculative FIM
            fim(pos_x, pos_y, true, m_suggestionContent);
        }
        return;
    }

    // Phase 2: nearby match - search for a cached completion whose start matches what was typed
    // only pick the single best match, no cycling
    QString pm = prefix + middle;
    int best = 0;
    QByteArray best_raw;

    for (int i = 0; i < 128; ++i) {
        if (pm.length() <= i)
            break;

        const QString removed = pm.mid(pm.length() - (1 + i)); // last i+1 chars
        const QString ctx_new = pm.left(pm.length() - (2 + i)) + Fim::kContextSeparator + suffix;
        const QByteArray hash_new
            = QCryptographicHash::hash(ctx_new.toUtf8(), QCryptographicHash::Sha256).toHex();

        if (QList<QByteArray> *cached = m_cacheData.lookup(hash_new)) {
            for (const QByteArray &response_cached : std::as_const(*cached)) {
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
                    if (best_raw.isNull() || remaining_content.length() > best) {
                        best = remaining_content.length();
                        best_raw = response_cached;
                    }
                }
            }
        }
    }

    if (!best_raw.isNull() && !best_raw.isEmpty()) {
        m_selectedCompletion = 0;
        fim_render(editor, pos_x, pos_y, {best_raw}, 0);

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
                             const QList<QByteArray> &responses,
                             int selected)
{
    // do not show a suggestion if we have a selection
    if (!editor->selectedText().isEmpty())
        return;

    if (responses.isEmpty())
        return;
    if (selected < 0 || selected >= responses.size())
        selected = 0;

    // Parse JSON response
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(responses[selected], &error);

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

    // if the current line is full of whitespaces, trim as much whitespace as
    // possible from the suggestion (but no more than the line itself has) so
    // that we don't end up with double indentation
    if (s_whitespace_regex.match(line_cur).hasMatch()) {
        const int lead
            = qMin(s_indent_regex.match(content.at(0)).capturedLength(0), line_cur.size());
        line_cur = content.at(0).left(lead);
        content[0] = content.at(0).mid(lead);
    }
    QString line_cur_prefix = line_cur.left(pos_x);
    QString line_cur_suffix = line_cur.mid(pos_x);

    // the model sometimes re-generates text that is already on the line
    // (e.g. the // of a comment): drop the repeated copy from the suggestion,
    // the accepted text is the same, the ghost text is less confusing
    if (!line_cur_prefix.isEmpty() && content.at(0).startsWith(line_cur_prefix))
        content[0] = content.at(0).mid(line_cur_prefix.size());

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
        // Text:positionInText has 1 based line and 0 based column values
        int currentIntPos = Text::positionInText(editor->document(), pos_y, pos_x);
        TextSuggestion::Data data;
        Text::Position currentPos = Text::Position::fromPositionInDocument(editor->document(),
                                                                           currentIntPos);
        data.range.begin = currentPos;
        data.range.end = currentPos;
        data.position = currentPos;

        static const QString qtCreatorVersion1800 = "18.0.0";

        if (QCoreApplication::applicationVersion() != qtCreatorVersion1800) {
            combined_content = line_cur_prefix + combined_content;
            if (!combined_content.endsWith(line_cur_suffix))
                combined_content.append(line_cur_suffix);
            data.range.begin.column = 0;
        }

        int separator = combined_content.indexOf("\n");
        data.range.end.column = separator != -1 ? combined_content.size() - separator - 1
                                                : combined_content.size();
        data.text = combined_content;

        if (m_suggestionContent != content) {
            auto suggestion = std::make_unique<TextEditor::TextSuggestion>(data, editor->document());
            if (QCoreApplication::applicationVersion() == qtCreatorVersion1800) {
                suggestion->replacementDocument()->setPlainText(line_cur_prefix + combined_content
                                                                + line_cur_suffix);
            }
            editor->insertSuggestion(std::move(suggestion));

            qCInfo(llamaLog) << "fim_render:" << pos_x << pos_y
                             << "Prepared suggestion:" << combined_content;
            if (!m_suggestionContent.isEmpty())
                qCInfo(llamaLog) << "The replaced suggestion:" << m_suggestionContent.join("\n");

            m_suggestionContent = content;
        }
    }

    // remember the current suggestion so we can cycle through the completions;
    // the state is kept even if this candidate was not rendered (e.g. it is
    // whitespace-only or repeats existing text), so the user can still cycle
    // back to a renderable one
    m_suggestionResponses = responses;
    m_selectedCompletion = selected;
    m_suggestionPos = QPoint(pos_x, pos_y);

    if (settings().showInfo.value() > 0) {
        m_textMark = std::make_unique<TextEditor::TextMark>(
            TextEditorWidget::currentTextEditorWidget()->textDocument(),
            pos_y,
            TextMarkCategory{"llama", "llama.cpp"});

        auto [label, tooltip, warningTooltip] = getShowInfoStats(obj);
        if (responses.size() > 1)
            label += QStringLiteral(" [%1/%2]").arg(selected + 1).arg(responses.size());
        m_textMark->setLineAnnotation(label);
        m_textMark->setToolTip(warningTooltip.isEmpty() ? tooltip : warningTooltip);
        m_textMark->setColor(warningTooltip.isEmpty()
                                 ? Utils::Theme::CodeModel_Info_TextMarkColor
                                 : Utils::Theme::CodeModel_Warning_TextMarkColor);
    }
}

// cycle to the next/previous completion candidate
void LlamaPlugin::fim_cycle(int direction)
{
    if (m_suggestionResponses.size() <= 1)
        return;

    auto *editor = TextEditor::TextEditorWidget::currentTextEditorWidget();

    // only cycle if the suggestion is still visible in the current editor and the
    // remembered position is still valid in its document
    if (!editor || !editor->suggestionVisible() || !editor->document() || m_suggestionPos.y() < 1
        || m_suggestionPos.y() > editor->document()->lineCount())
        return;

    const int n = m_suggestionResponses.size();
    m_selectedCompletion = (m_selectedCompletion + direction + n) % n;

    fim_render(editor,
               m_suggestionPos.x(),
               m_suggestionPos.y(),
               m_suggestionResponses,
               m_selectedCompletion);
}

void LlamaPlugin::hideCompletionHint()
{
    m_textMark.reset({});

    m_suggestionResponses.clear();
    m_selectedCompletion = 0;

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
    int indent = 0;

    if (prev.isEmpty()) {
        // No previous completion
        lineCur = getline(editor, pos_y - 1);

        lineCurPrefix = lineCur.left(pos_x);
        lineCurSuffix = lineCur.mid(pos_x);

        // special handling of lines full of whitespaces - start from the beginning of the line
        if (s_whitespace_regex.match(lineCur).hasMatch()) {
            indent = 0;
            lineCurPrefix.clear();
            lineCurSuffix.clear();
        } else {
            // the indentation of the current line
            indent = s_indent_regex.match(lineCur).capturedLength(0);
        }

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

        // keep the indentation consistent with the previously accepted completion
        indent = m_indentLast;
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
    res.indent = indent;

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

    // Evict similar chunks (like llama.vim, the oldest element at index 0 is
    // not considered here)
    for (int i = m_ringQueued.size() - 1; i > 0; --i) {
        if (Fim::chunkSim(m_ringQueued[i].data, chunk) > 0.9) {
            if (doEviction) {
                m_ringQueued.removeAt(i);
                m_ringNEvict++;
            } else {
                return;
            }
        }
    }
    for (int i = m_ringChunks.size() - 1; i > 0; --i) {
        if (Fim::chunkSim(m_ringChunks[i].data, chunk) > 0.9) {
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
    // skip processing while the user is actively typing or moving the cursor,
    // so we don't burn server prompt-processing time (ref: llama.vim s:ring_update)
    if (m_lastUserActivity.isValid() && m_lastUserActivity.elapsed() < kRingIdleDelayMs)
        return;

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

// query the /v1/models endpoint and report which models are loaded
// (ported from the :LlamaStatus command in llama.vim)
void LlamaPlugin::checkServerStatus()
{
    auto check = [this](const QString &endpoint,
                        const QString &apiKey,
                        const QString &modelName,
                        const QString &label) {
        const QUrl endpointUrl(endpoint);
        if (endpointUrl.isRelative() || (endpointUrl.scheme() != QLatin1String("http")
                                          && endpointUrl.scheme() != QLatin1String("https"))) {
            Core::MessageManager::writeDisrupting(
                Tr::tr("llama.cpp %1 server (model: %2): invalid endpoint %3")
                    .arg(label)
                    .arg(modelName.isEmpty() ? Tr::tr("default") : modelName)
                    .arg(endpoint));
            return;
        }

        QString base = endpoint;
        for (const char *suffix : {"/infill", "/v1/chat/completions"}) {
            const QString s = QLatin1String(suffix);
            if (base.endsWith(s))
                base.chop(s.size());
        }
        while (base.endsWith('/'))
            base.chop(1);

        QNetworkRequest req(QUrl(base + "/v1/models"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        if (!apiKey.isEmpty())
            req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());

        auto *reply = m_networkManager->get(req);
        connect(reply,
                &QNetworkReply::finished,
                [this, reply, modelName, label, endpoint]() {
                    const bool ok = (reply->error() == QNetworkReply::NoError);
                    const QByteArray data = reply->readAll();
                    reply->deleteLater();

                    QString status;
                    if (!ok) {
                        status = Tr::tr("not reachable");
                    } else {
                        const QJsonDocument doc = QJsonDocument::fromJson(data);
                        const QJsonArray models = doc.object().value("data").toArray();

                        QJsonObject match;
                        bool found = false;
                        if (!modelName.isEmpty()) {
                            for (const QJsonValue &value : std::as_const(models)) {
                                const QJsonObject model = value.toObject();
                                if (model.value("id").toString() == modelName
                                    || model.value("tags").toArray().contains(QJsonValue(modelName))
                                    || model.value("aliases").toArray().contains(QJsonValue(modelName))) {
                                    match = model;
                                    found = true;
                                    break;
                                }
                            }
                        } else if (models.size() == 1) {
                            match = models.first().toObject();
                            found = true;
                        }

                        if (!found) {
                            if (modelName.isEmpty())
                                status
                                    = models.isEmpty() ? Tr::tr("no models loaded")
                                                       : Tr::tr("multiple models loaded");
                            else
                                status = Tr::tr("model %1 is not loaded").arg(modelName);
                        } else {
                            const QJsonValue statusValue = match.value("status");
                            QString value = statusValue.isObject()
                                                ? statusValue.toObject().value("value").toString()
                                                : statusValue.toString();
                            if (value.isEmpty())
                                value = QStringLiteral("loaded");
                            status = (value == QStringLiteral("loaded")) ? Tr::tr("ready") : value;
                        }
                    }

                    // the user explicitly asked for the status - surface it in the UI
                    Core::MessageManager::writeDisrupting(
                        Tr::tr("llama.cpp %1 server (%2, model: %3): %4")
                            .arg(label)
                            .arg(endpoint)
                            .arg(modelName.isEmpty() ? Tr::tr("default") : modelName)
                            .arg(status));
                });
    };

    check(settings().endpoint.value(),
          settings().apiKey.value(),
          settings().modelFim.value(),
          Tr::tr("FIM"));
    check(settings().chatEndpoint.value(),
          settings().chatApiKey.value(),
          QString(),
          Tr::tr("Chat"));
}

} // namespace LlamaCpp
