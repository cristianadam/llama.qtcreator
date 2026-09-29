#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QProcess>
#include <QTimer>

#include <optional>

#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/textdocument.h>

#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>

#include "llamachatmanager.h"
#include "llamaconstants.h"
#include "llamasettings.h"
#include "projectinstructions.h"
#include "skills.h"
#include "llamastorage.h"
#include "markdownrenderer.h"
#include "llamathinkingsectionparser.h"
#include "llamatr.h"
#include "tools/factory.h"
#include "tools/mcpbridge.h"
#include "tools/tool.h"
#include "tools/tool_utils.h"
#include "tools/websearch_tool.h"

Q_LOGGING_CATEGORY(llamaChatNetwork, "llama.cpp.chat.network", QtWarningMsg)
Q_LOGGING_CATEGORY(llamaChatTools, "llama.cpp.chat.tools", QtWarningMsg)

using namespace ProjectExplorer;
using namespace Utils;

namespace LlamaCpp {

// Returns the thinking‑control kwarg of the chat template
// ("enable_thinking", "reasoning_effort" or "thinking"), or an empty string
// when the template does not expose a known one (or does not support
// thinking at all, see chatTemplateSupportsThinking()).
static QString chatTemplateThinkingKwarg(const QString &tmpl)
{
    if (tmpl.isEmpty())
        return QString();

    static const QStringList kwargs{QLatin1String("enable_thinking"),
                                    QLatin1String("reasoning_effort"),
                                    QLatin1String("thinking")};
    for (const QString &kwarg : std::as_const(kwargs)) {
        const QRegularExpression re(
            QStringLiteral("(\\{\\{[^{}]*\\b%1\\b[^{}]*\\}\\}|\\{%[^{}]*\\b%1\\b[^{}]*%\\})")
                .arg(QRegularExpression::escape(kwarg)),
            QRegularExpression::CaseInsensitiveOption);
        if (re.match(tmpl).hasMatch())
            return kwarg;
    }
    return QString();
}

static void addCommonPayloadParams(QJsonObject &payload, const QString &model)
{
    payload["samplers"] = settings().samplers.value();
    payload["temperature"] = settings().temperature.value();
    payload["dynatemp_range"] = settings().dynatemp_range.value();
    payload["dynatemp_exponent"] = settings().dynatemp_exponent.value();
    payload["top_k"] = settings().top_k.value();
    payload["top_p"] = settings().top_p.value();
    payload["min_p"] = settings().min_p.value();
    payload["typical_p"] = settings().typical_p.value();
    payload["xtc_probability"] = settings().xtc_probability.value();
    payload["xtc_threshold"] = settings().xtc_threshold.value();
    payload["repeat_last_n"] = settings().repeat_last_n.value();
    payload["repeat_penalty"] = settings().repeat_penalty.value();
    payload["presence_penalty"] = settings().presence_penalty.value();
    payload["frequency_penalty"] = settings().frequency_penalty.value();
    payload["dry_multiplier"] = settings().dry_multiplier.value();
    payload["dry_base"] = settings().dry_base.value();
    payload["dry_allowed_length"] = settings().dry_allowed_length.value();
    // Newer llama.cpp servers reject negative values; the -1 default
    // ("fall back to repeat_last_n") is what the server assumes when the
    // field is omitted.
    if (settings().dry_penalty_last_n.value() >= 0)
        payload["dry_penalty_last_n"] = settings().dry_penalty_last_n.value();
    payload["max_tokens"] = settings().max_tokens.value();
    // Progress and timings are always requested so the context-usage label in
    // the status bar can be populated, even when "Show tokens per second" is
    // disabled. The speed label itself is only shown when that setting is on.
    payload["timings_per_token"] = true;
    payload["return_progress"] = true;
    // Thinking level for reasoning models, sent as the OAI "reasoning_effort"
    // field (the llama.cpp server maps it onto the chat template kwargs).
    // "off" maps to "none"; "default" (or empty) omits the field entirely so
    // the server/model default applies. Unknown values are ignored and the
    // field is not sent for templates without thinking support.
    const QString thinkingLevel = settings().thinkingLevel.value();
    static const QStringList knownLevels{QLatin1String("off"),
                                         QLatin1String("low"),
                                         QLatin1String("medium"),
                                         QLatin1String("high"),
                                         QLatin1String("max")};
    if (knownLevels.contains(thinkingLevel) && ChatManager::instance().serverSupportsThinking()) {
        payload["reasoning_effort"] =
            thinkingLevel == QLatin1String("off") ? QStringLiteral("none") : thinkingLevel;
    }
    // Select the active model; required by router-mode servers, ignored by
    // single-model ones.
    if (!model.isEmpty())
        payload["model"] = model;
}

// Auxiliary requests (conversation title, follow‑up suggestions) are
// short housekeeping calls that fire right after every chat completion.
// Sending them with the full chat settings – especially an uncapped
// max_tokens and a high reasoning level – makes them occupy the server's
// slot for a long time and delays the user's next prompt. Keep them cheap:
// - no user sampling settings (a low temperature is enough here)
// - a hard max_tokens cap
// - thinking disabled when the chat template supports it (see the
//   title generation in llama.cpp's tools/ui, which sends
//   chat_template_kwargs: {enable_thinking: false})
void ChatManager::addAuxiliaryPayloadParams(QJsonObject &payload, int maxTokens) const
{
    payload["max_tokens"] = maxTokens;
    payload["temperature"] = 0.2;
    payload["top_p"] = 0.9;
    // Disable thinking in the way the template understands it: boolean
    // kwargs (Qwen‑style "enable_thinking", DeepSeek‑0528‑style "thinking")
    // go into chat_template_kwargs, while "reasoning_effort" templates take
    // the OAI field with the value "none" (cf. the title generation in
    // llama.cpp's tools/ui, which sends chat_template_kwargs with
    // enable_thinking: false).
    const QString thinkingKwarg = chatTemplateThinkingKwarg(m_serverProps.chat_template);
    if (thinkingKwarg == QLatin1String("reasoning_effort"))
        payload["reasoning_effort"] = QStringLiteral("none");
    else if (!thinkingKwarg.isEmpty())
        payload["chat_template_kwargs"] = QJsonObject{{thinkingKwarg, false}};

    // An optional dedicated (typically smaller) model for these tasks, cf.
    // the "task model" in Open WebUI. Empty falls back to the chat model.
    const QString model =
        settings().utilityModel.value().isEmpty() ? m_selectedModel : settings().utilityModel.value();
    if (!model.isEmpty())
        payload["model"] = model;
}

// Local tools are enabled when listed in EnabledToolsList. Tools served by
// MCP servers are disabled by default and only included when the user
// explicitly enabled them (EnabledMcpToolsList).
static bool isToolEnabled(const QString &toolName)
{
    if (McpBridge::instance().isMcpTool(toolName))
        return settings().enabledMcpToolsList().contains(toolName);
    // effectiveEnabledTools() also enables tools that were registered after
    // the user's stored list was written (new plugin version).
    return effectiveEnabledTools().contains(toolName);
}

static void addToolsToPayload(QJsonObject &payload, const QStringList *allowedTools = nullptr)
{
    QJsonArray toolsArr;
    const QStringList creatorsList = ToolFactory::instance().creatorsList();
    for (const QString &toolName : std::as_const(creatorsList)) {
        std::unique_ptr<Tool> tool = ToolFactory::instance().create(toolName);
        if (!tool)
            continue; // e.g. a remote tool that disappeared mid‑flight

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(tool->toolDefinition().toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "Invalid tool JSON:" << err.errorString();
            continue;
        }

        const QJsonObject functionObj = doc.object().value(QStringLiteral("function")).toObject();
        const QString name = functionObj.value(QStringLiteral("name")).toString();

        if (allowedTools && !allowedTools->contains(name)) {
            // Not part of the conversation‑specific tool whitelist (task
            // sub‑agent) – it must not be advertised either.  An empty
            // whitelist means no tools at all.
            continue;
        }

        if (!isToolEnabled(name)) {
            // Skip disabled tools – they must never be advertised to the server.
            qCInfo(llamaChatTools).nospace()
                << "Tool '" << name << "' is disabled, not adding it to payload.";
            continue;
        }

        if (name == QLatin1String("skill") && Skills::enabledSkills().isEmpty()) {
            // No skills to load – advertising the tool would only cost
            // context. It reappears as soon as a skill is enabled.
            continue;
        }
        if (name == QLatin1String("websearch")
                && !Tools::WebSearchConfig::fromSettings().isConfigured()) {
            // No search backend configured – every call would fail.
            continue;
        }

        QJsonObject root;
        root[QStringLiteral("type")] = QStringLiteral("function");
        root[QStringLiteral("function")] = functionObj;
        toolsArr.append(root);
    }

    if (!toolsArr.isEmpty())
        payload["tools"] = toolsArr;
}

ChatManager &ChatManager::instance()
{
    static ChatManager inst;
    return inst;
}

ChatManager::ChatManager(QObject *parent)
    : QObject(parent)
    , m_storage(&Storage::instance())
{
    initServerProps();
    refreshModels();

    connect(m_storage, &Storage::messageAppended, this, &ChatManager::messageAppended);
    connect(m_storage, &Storage::conversationCreated, [this](const QString &convId) {
        if (!m_taskConvCreationPending && !m_taskConversations.contains(convId))
            m_activeConvId = convId;
        emit conversationCreated(convId);
    });
    connect(m_storage, &Storage::conversationRenamed, this, &ChatManager::conversationRenamed);
    connect(m_storage, &Storage::conversationDeleted, this, &ChatManager::conversationDeleted);
    connect(m_storage, &Storage::messageExtraUpdated, this, &ChatManager::messageExtraUpdated);

    // "Human Editor" documents: saving the split text editor commits its
    // content as a chat message (a new assistant reply, or the in‑place
    // edit of a stored message).
    connect(Core::EditorManager::instance(),
            &Core::EditorManager::aboutToSave,
            this,
            [this](Core::IDocument *document, Core::IDocument::SaveOption) {
                auto it = m_humanEditorSessions.find(document);
                if (it == m_humanEditorSessions.end())
                    return;
                // A pending reply is committed at most once (a later save is a
                // plain file save); an in‑place edit re‑updates the message on
                // every save.
                if (it->committed && it->msgId <= 0)
                    return;
                it->committed = true;

                Core::BaseTextDocument *textDocument =
                    qobject_cast<Core::BaseTextDocument *>(document);
                const QString content = textDocument ? textDocument->plainText() : QString();

                if (it->msgId > 0)
                    updateMessageContent(it->convId, it->msgId, content);
                else
                    commitHumanEditorMessage(it->convId, content);

                // Note: the throw‑away file is removed when the editor closes
                // (not here – the actual file write happens after this signal).
            });

    // Closing a "Human Editor" editor without saving aborts the pending
    // reply (an in‑place edit simply keeps the old content).
    connect(Core::EditorManager::instance(),
            &Core::EditorManager::editorAboutToClose,
            this,
            [this](Core::IEditor *editor) {
                for (auto it = m_humanEditorSessions.begin();
                     it != m_humanEditorSessions.end();
                     ++it) {
                    if (it->editor != editor)
                        continue;
                    // A pending reply that was never saved is discarded; an
                    // in‑place edit that was never saved keeps the old text.
                    if (it->msgId <= 0 && !it->committed)
                        abortHumanEditorMessage(it->convId);
                    if (!it->filePath.isEmpty())
                        QFile::remove(it->filePath);
                    m_humanEditorSessions.erase(it);
                    return;
                }
            });
}

static QNetworkReply *getServerProps(QNetworkAccessManager *manager,
                                     const QString &baseUrl,
                                     const QString &apiKey)
{
    QUrl url(baseUrl + "/props");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!apiKey.isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());

    return manager->get(req);
}

void ChatManager::refreshServerProps()
{
    initServerProps();
}

void ChatManager::initServerProps()
{
    QNetworkReply *reply = getServerProps(&m_network,
                                          settings().chatEndpoint.value(),
                                          settings().chatApiKey.value());
    QObject::connect(reply, &QNetworkReply::finished, [reply, this]() {
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(llamaChatNetwork) << "Failed to fetch server props:" << reply->errorString();
            reply->deleteLater();
            return;
        }
        QByteArray b = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(b);
        QJsonObject obj = doc.object();
        m_serverProps.build_info = obj.value("build_info").toString();
        m_serverProps.model_path = obj.value("model_path").toString();
        QJsonObject genSettings = obj.value("default_generation_settings").toObject();
        m_serverProps.n_ctx = genSettings.value("n_ctx").toInt();
        QJsonObject mod = obj.value("modalities").toObject();
        m_serverProps.modalities.vision = mod.value("vision").toBool();
        m_serverProps.modalities.audio = mod.value("audio").toBool();
        m_serverProps.chat_template = obj.value("chat_template").toString();
        reply->deleteLater();

        emit serverPropsUpdated();
    });
}

// Heuristics mirroring the llama.cpp web UI's chat-template thinking detector:
// look for thinking-control Jinja variables, thinking conditionals and paired
// thinking tag markers in the model's chat template.
static bool chatTemplateSupportsThinking(const QString &tmpl)
{
    if (tmpl.isEmpty())
        return false;

    static const QStringList kwargVars{QLatin1String("enable_thinking"),
                                       QLatin1String("reasoning_effort"),
                                       QLatin1String("thinking_budget")};
    for (const QString &kwarg : std::as_const(kwargVars)) {
        const QRegularExpression re(
            QStringLiteral("(\\{\\{[^{}]*\\b%1\\b[^{}]*\\}\\}|\\{%[^{}]*\\b%1\\b[^{}]*%\\})")
                .arg(QRegularExpression::escape(kwarg)),
            QRegularExpression::CaseInsensitiveOption);
        if (re.match(tmpl).hasMatch())
            return true;
    }

    static const QRegularExpression conditionals[] = {
        QRegularExpression(QStringLiteral("\\{%-?\\s*if\\s+\\(?\\s*\\w*enable[\\s_]+\\w*(thinking|think|reasoning)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("\\{%-?\\s*if\\s+\\w*(thinking|reasoning)\\s*(is not|==|!=)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("\\{%-?\\s*if\\s+not\\s+\\w*enable"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("\\{%-?\\s*if\\s+ns\\.enable_thinking"),
                           QRegularExpression::CaseInsensitiveOption),
    };
    for (const auto &re : conditionals) {
        if (re.match(tmpl).hasMatch())
            return true;
    }

    struct TagPair
    {
        QString start;
        std::optional<QString> end; // self-closing tag when unset
    };
    static const TagPair tagPatterns[] = {
        {QLatin1String("<think"), QStringLiteral("</think")},
        {QLatin1String("<|channel>thought"), QStringLiteral("<|channel|>")},
        {QLatin1String("<|think|>"), std::nullopt},
        {QLatin1String("<seed:think|>"), std::nullopt},
    };
    for (const auto &pair : tagPatterns) {
        if (tmpl.contains(pair.start)
            && (!pair.end.has_value() || tmpl.contains(*pair.end)))
            return true;
    }

    return false;
}

bool ChatManager::serverSupportsThinking() const
{
    return chatTemplateSupportsThinking(m_serverProps.chat_template);
}

QList<ChatManager::ModelEntry> ChatManager::models() const
{
    return m_models;
}

QString ChatManager::selectedModel() const
{
    return m_selectedModel;
}

void ChatManager::refreshModels()
{
    QUrl url(settings().chatEndpoint.value() + "/models");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.get(req);
    connect(reply, &QNetworkReply::finished, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonArray arr = QJsonDocument::fromJson(reply->readAll())
                                       .object()
                                       .value("data")
                                       .toArray();
            QList<ModelEntry> entries;
            for (const auto &v : arr) {
                const QJsonObject obj = v.toObject();
                const ModelEntry entry{obj.value("id").toString(),
                                       obj.value("status").toObject().value("value").toString()};
                if (!entry.id.isEmpty())
                    entries.append(entry);
            }
            m_models = entries;
            if (!m_models.isEmpty()) {
                bool selected = false;
                for (const auto &m : std::as_const(m_models))
                    if (m.id == m_selectedModel)
                        selected = true;
                if (!selected && !isHumanEditor()) {
                    // Fall back to the loaded model, or the first one if none
                    // is loaded yet.  The "Human Editor" pseudo‑model is not
                    // in the server's list, so it must not be clobbered.
                    m_selectedModel = m_models.first().id;
                    for (const auto &m : std::as_const(m_models))
                        if (m.status == QLatin1String("loaded")) {
                            m_selectedModel = m.id;
                            break;
                        }
                }
            }
        } else {
            qCWarning(llamaChatNetwork) << "Failed to fetch model list:" << reply->errorString();
        }
        reply->deleteLater();

        emit modelsUpdated();
        updateModelPolling();
    });
}

void ChatManager::selectModel(const QString &id)
{
    if (id.isEmpty() || id == m_selectedModel)
        return;
    m_selectedModel = id;

    // The "Human Editor" endpoint is not a server model: no load request.
    if (isHumanEditor()) {
        emit modelsUpdated();
        return;
    }

    // Router mode: ask the server to load the model when it is not loaded
    // yet. The load endpoint returns before loading completes, so the
    // polling started by updateModelPolling() tracks the progress.
    for (const auto &m : std::as_const(m_models)) {
        if (m.id != id || m.status.isEmpty()
            || m.status == QLatin1String("loaded") || m.status == QLatin1String("loading"))
            continue;
        QUrl url(settings().chatEndpoint.value() + "/models/load");
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        if (!settings().chatApiKey.value().isEmpty())
            req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());
        QJsonObject body;
        body["model"] = id;
        QNetworkReply *reply = m_network.post(req, QJsonDocument(body).toJson());
        connect(reply, &QNetworkReply::finished, [reply]() {
            if (reply->error() != QNetworkReply::NoError)
                qCWarning(llamaChatNetwork) << "Failed to load model:" << reply->errorString();
            reply->deleteLater();
        });
        break;
    }

    emit modelsUpdated();
    updateModelPolling();
}

void ChatManager::updateModelPolling()
{
    bool anyLoading = false;
    for (const auto &m : std::as_const(m_models))
        if (m.status == QLatin1String("loading")) {
            anyLoading = true;
            break;
        }

    if (anyLoading && !m_modelPollTimer) {
        m_modelPollTimer = new QTimer(this);
        m_modelPollTimer->setInterval(1000);
        connect(m_modelPollTimer, &QTimer::timeout, this, [this]() { refreshModels(); });
    }
    if (m_modelPollTimer) {
        if (anyLoading && !m_modelPollTimer->isActive())
            m_modelPollTimer->start();
        else if (!anyLoading && m_modelPollTimer->isActive())
            m_modelPollTimer->stop();
    }
}

bool ChatManager::isGenerating(const QString &convId) const
{
    return m_pendingMessages.contains(convId) || m_runningTools.value(convId) > 0;
}

ViewingChat ChatManager::getViewingChat(const QString &convId) const
{
    ViewingChat vc{m_storage->getOneConversation(convId), m_storage->getMessages(convId)};
    return vc;
}

// Renders a tool call and its result as a collapsible Markdown block,
// mirroring the tool bubble shown in the chat UI.  Returns an empty
// string when the message carries no tool call.
static QString toolCallToMarkdown(const Message &msg)
{
    QString functionName;
    QString argumentsJson;
    QString functionResult;
    QString toolStatus;

    for (const QVariantMap &e : msg.extra) {
        if (e.contains("tool_calls")) {
            const QJsonArray calls = e.value("tool_calls").toJsonArray();
            if (!calls.isEmpty()) {
                const QJsonObject callObj = calls.first().toObject();
                functionName = callObj.value("function").toObject().value("name").toString();
                argumentsJson
                    = callObj.value("function").toObject().value("arguments").toString();
            }
        }
        if (e.contains("tool_result"))
            functionResult = toolResultText(e.value("tool_result").toJsonObject().value("content"));
        if (e.contains("tool_status"))
            toolStatus = e.value("tool_status").toString(); // "success" / "failed"
        // NB: tool_calls, tool_result and tool_status may live in the same
        // extra entry, so these must be independent checks.
    }

    if (functionName.isEmpty())
        return {};

    QJsonObject args;
    QString formattedArgs = argumentsJson;
    const QJsonDocument argDoc = QJsonDocument::fromJson(argumentsJson.toUtf8());
    if (argDoc.isObject()) {
        args = argDoc.object();
        formattedArgs = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented));
    }

    // Reuse the tool's one‑line summary so the export matches what the UI
    // shows in the collapsed tool bubble.
    QString summary = QStringLiteral("Tool: %1").arg(functionName);
    if (const std::unique_ptr<Tool> tool = ToolFactory::instance().create(functionName))
        summary = tool->oneLineSummary(args);
    if (!toolStatus.isEmpty())
        summary += QStringLiteral(" (%1)").arg(toolStatus);

    if (functionResult.endsWith('\n'))
        functionResult.chop(1);

    QString md = QStringLiteral("<details>\n<summary>%1</summary>\n").arg(summary);
    md += QStringLiteral("\n**Arguments**\n\n```json\n") + formattedArgs + QStringLiteral("\n```\n");
    if (!functionResult.isEmpty())
        md += QStringLiteral("\n**Result**\n\n```\n") + functionResult + QStringLiteral("\n```\n");
    md += QStringLiteral("\n</details>\n\n");

    return md;
}

QString ChatManager::messageToMarkdown(const Message &msg)
{
    if (msg.role == "tool")
        return toolCallToMarkdown(msg);

    // Embed the persisted diagram SVGs so the export shows the rendered
    // pictures instead of re-computing them (or losing them in viewers
    // without mermaid/KaTeX support).
    const QMap<QString, MarkdownRenderer::DiagramSvg> diagrams
        = MarkdownRenderer::diagramSvgsFromExtra(msg.extra);

    if (msg.role == "user")
        return QStringLiteral("### User\n\n")
               + MarkdownRenderer::embedDiagramSvgs(msg.content, diagrams)
               + QStringLiteral("\n\n");

    // Assistant (or anything else).
    QString processedContent = msg.content;
    processedContent.replace(ThinkingSectionParser::startToken(),
                             "<details><summary>Thought</summary>\n");
    processedContent.replace(ThinkingSectionParser::endToken(),
                             "\n</details>\n\n");

    // A tool‑call‑only assistant message renders through its tool bubble,
    // whose call + result are exported with the tool message instead.
    if (processedContent.trimmed().isEmpty()) {
        for (const QVariantMap &e : msg.extra) {
            if (e.contains("tool_calls"))
                return {};
        }
    }

    return QStringLiteral("### Assistant\n\n")
           + MarkdownRenderer::embedDiagramSvgs(processedContent, diagrams)
           + QStringLiteral("\n\n");
}

void ChatManager::saveDiagramSvg(const Message &msg, const QVariantMap &entry)
{
    if (msg.id < 0 || msg.convId.isEmpty())
        return;

    if (auto it = m_pendingMessages.find(msg.convId);
        it != m_pendingMessages.end() && it->id == msg.id) {
        // Still streaming: stash the entry in the pending message, it is
        // committed to the database with the message.
        for (QVariantMap &e : it->extra) {
            if (e.value("key").toString() == entry.value("key").toString()) {
                e = entry;
                return;
            }
        }
        it->extra << entry;
        return;
    }

    // Already committed: merge the entry into the stored extra (a theme or
    // font change re-renders the diagram and updates the entry in place).
    for (const Message &stored : m_storage->getMessages(msg.convId)) {
        if (stored.id != msg.id)
            continue;
        QList<QVariantMap> extra = stored.extra;
        bool found = false;
        for (QVariantMap &e : extra) {
            if (e.value("key").toString() == entry.value("key").toString()) {
                e = entry;
                found = true;
            }
        }
        if (!found)
            extra << entry;
        m_storage->updateMessageExtra(stored, extra);
        return;
    }
}

QVector<Message> ChatManager::filterByLeafNodeId(const QVector<Message> &messages,
                                                 qint64 leafNodeId,
                                                 bool includeRoot)
{
    return m_storage->filterByLeafNodeId(messages, leafNodeId, includeRoot);
}

void ChatManager::sendMessage(const QString &convId,
                              qint64 leafNodeId,
                              const QString &content,
                              const QList<QVariantMap> &extra,
                              std::function<void(qint64)> onChunk)
{
    if (isGenerating(convId) || content.trimmed().isEmpty() || convId.isEmpty())
        return;

    Message newMsg;
    auto now = QDateTime::currentMSecsSinceEpoch();
    newMsg.convId = convId;
    newMsg.type = "text";
    newMsg.timestamp = now;
    newMsg.role = "user";
    newMsg.content = content;
    newMsg.parent = leafNodeId;
    newMsg.children.clear();
    newMsg.extra = extra; // simple wrapper – see MessageExtra

    m_storage->appendMsg(newMsg, leafNodeId);
    onChunk(newMsg.id);

    // generate assistant reply
    generateMessage(newMsg.convId, newMsg.id, onChunk);
}

static void readSSEStream(QNetworkReply *reply,
                          std::function<void(const QJsonObject &)> onChunk,
                          std::function<void(const QString &)> onError)
{
    QObject::connect(reply, &QNetworkReply::readyRead, [reply, onChunk, onError]() {
        QByteArray buffer = reply->readAll();

        qCDebug(llamaChatNetwork).noquote() << "readSSEStream:" << buffer;

        // split on \n\n (SSE format)
        while (true) {
            int pos = buffer.indexOf("\n\n");
            if (pos < 0)
                break;
            QByteArray line = buffer.left(pos);
            buffer.remove(0, pos + 2);

            // parse the data line
            if (line.startsWith("data: "))
                line = line.mid(6);
            if (line.isEmpty() || line == "[DONE]")
                continue;

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if (err.error != QJsonParseError::NoError) {
                onError(err.errorString());
                return;
            }
            onChunk(doc.object());
        }
    });
}

void ChatManager::generateMessage(const QString &convId,
                                  qint64 leafNodeId,
                                  std::function<void(qint64)> onChunk)
{
    if (isGenerating(convId))
        return;

    // Pick up skill changes from disk for the upcoming turn. The system
    // message and the skill tool share this scan for the whole turn, so
    // they always agree on what exists.
    Skills::clearCache();

    auto currMsgs = m_storage->getMessages(convId);
    auto leafMsgs = m_storage->filterByLeafNodeId(currMsgs, leafNodeId, false);

    // prepare pending msg
    Message pending{};
    pending.id = QDateTime::currentMSecsSinceEpoch();
    pending.convId = convId;
    pending.type = "text";
    pending.timestamp = pending.id;
    pending.role = "assistant";
    pending.content = QString();
    pending.parent = leafNodeId;
    pending.children.clear();
    m_pendingMessages.insert(convId, pending);

    // The "Human Editor" endpoint: the "assistant" reply is produced by a
    // human in the Markdown editor dialog, not by the server.  The pending
    // message is committed (or discarded) by
    // commitHumanEditorMessage()/abortHumanEditorMessage().  Task
    // conversations always run against a real model.
    if (isHumanEditor() && !m_taskConversations.contains(convId)) {
        emit humanEditorReplyReady(convId);
        return;
    }

    m_abortControllers[convId] = nullptr; // will hold the reply

    sendChatRequest(
        convId,
        [leafMsgs, convId, this](QJsonObject &payload) {
            payload["messages"] = normalizeMsgsForAPI(leafMsgs);
            if (settings().toolsEnabled()) {
                const auto it = m_taskConfigs.constFind(convId);
                if (it != m_taskConfigs.constEnd()) {
                    QStringList allowed = it->allowedTools.isEmpty()
                        ? settings().enabledToolsList()
                        : it->allowedTools;
                    allowed.removeAll(QStringLiteral("task")); // no recursion
                    addToolsToPayload(payload, &allowed);
                    // First sub‑agent turn: force a tool call so smaller
                    // models start working instead of narrating a plan.
                    // Later turns stay free, so the subagent can still
                    // finish with a plain report.
                    if (leafMsgs.size() <= 1)
                        payload[QStringLiteral("tool_choice")] = QStringLiteral("required");
                } else {
                    addToolsToPayload(payload);
                }
            }

            // custom JSON from settings (if any)
            if (!settings().customJson.value().isEmpty()) {
                QJsonDocument d = QJsonDocument::fromJson(settings().customJson.value().toUtf8());
                if (d.isObject())
                    for (auto it = d.object().constBegin(); it != d.object().constEnd(); ++it)
                        payload[it.key()] = it.value();
            }
        },
        onChunk);
}

void ChatManager::followUpQuestions(const QString &convId,
                                    qint64 leafNodeId,
                                    std::function<void(const QStringList &)> onSuccess)
{
    // The "Follow up" status bar button can be used to switch the
    // suggestions off without having to change the settings page.
    if (!settings().followUpEnabled.value())
        return;

    auto allMsgs = m_storage->getMessages(convId);
    auto leafMsgs = m_storage->filterByLeafNodeId(allMsgs, leafNodeId, false);

    // Only the most recent messages are needed to suggest follow‑ups; sending
    // the whole history is expensive for long conversations or dense models.
    static constexpr int kFollowUpContextMessages = 6;
    auto recentMsgs = leafMsgs.mid(qMax(0, leafMsgs.size() - kFollowUpContextMessages));

    QJsonArray msgArray = normalizeMsgsForAPI(recentMsgs);

    QJsonArray parts;
    QJsonObject txt;
    txt["type"] = "text";
    // User-editable prompt ("Prompts" settings page); fall back to the
    // built-in default if it was cleared.
    const QString followUpPrompt = settings().followUpPrompt.value().trimmed();
    txt["text"] = followUpPrompt.isEmpty() ? defaultFollowUpPrompt() : followUpPrompt;
    parts.append(txt);
    QJsonObject prompt;
    prompt["role"] = "user";
    prompt["content"] = parts;
    msgArray.append(prompt);

    QJsonObject payload;
    payload["messages"] = msgArray;
    payload["stream"] = false;
    payload["cache_prompt"] = true;
    payload["reasoning_format"] = "deepseek";
    payload["reasoning_in_content"] = "false";
    QJsonObject responseFormat;
    responseFormat["type"] = "json_object";
    payload["response_format"] = responseFormat;
    addAuxiliaryPayloadParams(payload, /*maxTokens=*/512);

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());
    m_followUpReplies.insert(convId, reply);

    QObject::connect(reply, &QNetworkReply::finished, [reply, onSuccess, convId, this]() {
        m_followUpReplies.remove(convId);
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(llamaChatNetwork) << "Follow‑up request failed:" << reply->errorString();
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            qCWarning(llamaChatNetwork) << "Follow‑up JSON malformed";
            return;
        }

        QJsonObject obj = doc.object();
        QJsonArray choices = obj.value("choices").toArray();
        if (choices.isEmpty())
            return;

        QJsonObject choice = choices[0].toObject();
        QJsonObject message = choice.value("message").toObject();

        QString content = message.value("content").toString().trimmed();
        if (content.isEmpty())
            return;

        // The response should be a JSON object {"follow_ups": [...]}, but
        // salvage the object in case the model wraps it in extra text
        // (e.g. leftover thinking text or markdown fences).
        if (!content.startsWith('{')) {
            auto idx = content.indexOf('{');
            if (idx == -1)
                return;
            content = content.mid(idx);
        }
        if (!content.endsWith('}')) {
            auto idx = content.lastIndexOf('}');
            if (idx == -1)
                return;
            content = content.left(idx + 1);
        }

        QJsonParseError err;
        QJsonDocument parsed = QJsonDocument::fromJson(content.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            qCWarning(llamaChatNetwork) << "Could not parse follow‑up JSON:" << err.errorString();
            qCWarning(llamaChatNetwork) << "The faulty content was:" << content.toUtf8();
            return;
        }

        QJsonArray arr;
        if (parsed.isObject())
            arr = parsed.object().value("follow_ups").toArray();
        else if (parsed.isArray())
            arr = parsed.array();

        QStringList questions;
        for (const QJsonValue &v : arr)
            if (v.isString())
                questions.append(v.toString());

        if (!questions.isEmpty() && onSuccess)
            onSuccess(questions);
    });
}

void ChatManager::cancelTitleSummary(const QString &convId)
{
    if (m_titleSummaryReplies.contains(convId)) {
        auto r = m_titleSummaryReplies.take(convId);
        r->abort();
        r->deleteLater();
    }
}

void ChatManager::cancelFollowUp(const QString &convId)
{
    if (m_followUpReplies.contains(convId)) {
        auto r = m_followUpReplies.take(convId);
        r->abort();
        r->deleteLater();
    }
}

Conversation ChatManager::currentConversation()
{
    return m_storage->getOneConversation(m_activeConvId);
}

void ChatManager::setCurrentConversation(const QString &convId)
{
    m_activeConvId = convId;
}

Conversation ChatManager::createConversation(const QString &name)
{
    return m_storage->createConversation(name);
}

Conversation ChatManager::createTaskConversation(const QString &name)
{
    m_taskConvCreationPending = true;
    Conversation conv = m_storage->createConversation(name);
    m_taskConvCreationPending = false;
    m_taskConversations.insert(conv.id);
    return conv;
}

void ChatManager::configureTaskConversation(const QString &convId,
                                            const QString &systemPrompt,
                                            const QStringList &allowedTools)
{
    TaskConversationConfig config;
    config.systemPrompt = systemPrompt;
    config.allowedTools = allowedTools;
    m_taskConfigs.insert(convId, config);
}

QList<Conversation> ChatManager::allConversations()
{
    return m_storage->getAllConversations();
}

void ChatManager::summarizeConversationTitle(const QString &convId,
                                             qint64 leafNodeId,
                                             std::function<void(const QString &)> onSuccess)
{
    auto msgs = m_storage->getMessages(convId);
    auto leafMsgs = m_storage->filterByLeafNodeId(msgs, leafNodeId, false);

    QJsonArray msgArray = normalizeMsgsForAPI(leafMsgs);
    QJsonObject payload;

    // Append the prompt that asks for the title
    QJsonArray parts;
    QJsonObject txt;
    txt["type"] = "text";
    // User-editable prompt ("Prompts" settings page); fall back to the
    // built-in default if it was cleared.
    const QString titlePrompt = settings().titlePrompt.value().trimmed();
    txt["text"] = titlePrompt.isEmpty() ? defaultTitlePrompt() : titlePrompt;
    parts.append(txt);
    QJsonObject prompt;
    prompt["role"] = "user";
    prompt["content"] = parts;

    msgArray.append(prompt);
    payload["messages"] = msgArray;

    // Short housekeeping request – capped token budget, no streaming
    payload["stream"] = false;
    payload["cache_prompt"] = true;
    payload["reasoning_format"] = "deepseek";
    payload["reasoning_in_content"] = "false";
    addAuxiliaryPayloadParams(payload, /*maxTokens=*/64);

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());
    m_titleSummaryReplies.insert(convId, reply);

    QObject::connect(reply, &QNetworkReply::finished, [reply, onSuccess, this, convId]() {
        m_titleSummaryReplies.remove(convId);
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(llamaChatNetwork) << "Title summary request failed:" << reply->errorString();
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            qCWarning(llamaChatNetwork) << "Title summary JSON malformed";
            return;
        }

        QJsonObject obj = doc.object();
        QJsonArray choices = obj.value("choices").toArray();
        if (choices.isEmpty())
            return;

        QJsonObject choice = choices[0].toObject();
        QJsonObject message = choice.value("message").toObject();
        QString title = message.value("content").toString().trimmed();

        if (!title.isEmpty())
            onSuccess(title);
    });
}

void ChatManager::stopGenerating(const QString &convId)
{
    if (!m_pendingMessages.contains(convId))
        return;

    if (m_abortControllers.contains(convId)) {
        auto controller = m_abortControllers.take(convId);
        controller->abort();
        controller->deleteLater();
    }
}

void ChatManager::replaceMessageAndGenerate(const QString &convId,
                                            qint64 parentNodeId,
                                            const QString &content,
                                            const QList<QVariantMap> &extra,
                                            std::function<void(qint64)> onChunk)
{
    if (isGenerating(convId))
        return;

    if (!content.isEmpty()) {
        auto now = QDateTime::currentMSecsSinceEpoch();
        Message newMsg;
        newMsg.convId = convId;
        newMsg.type = "text";
        newMsg.timestamp = now;
        newMsg.role = "user";
        newMsg.content = content;
        newMsg.parent = parentNodeId;
        newMsg.children.clear();
        newMsg.extra = extra;

        m_storage->appendMsg(newMsg, parentNodeId);
        parentNodeId = newMsg.id;
    }

    onChunk(parentNodeId);
    generateMessage(convId, parentNodeId, onChunk);
}

bool ChatManager::isHumanEditor() const
{
    return m_selectedModel == QLatin1String(Constants::HUMAN_EDITOR_MODEL_ID);
}

void ChatManager::commitHumanEditorMessage(const QString &convId, const QString &content)
{
    auto it = m_pendingMessages.find(convId);
    if (it == m_pendingMessages.end())
        return;

    Message pm = it.value();
    pm.content = content;
    m_pendingMessages.erase(it);

    // Emits messageAppended (pendingId = the pending message's id), which
    // the chat UI uses to finalize the reply bubble.
    m_storage->appendMsg(pm, pm.parent);
}

void ChatManager::abortHumanEditorMessage(const QString &convId)
{
    if (m_pendingMessages.remove(convId) > 0)
        emit humanEditorAborted(convId);
}

void ChatManager::updateMessageContent(const QString &convId, qint64 msgId, const QString &content)
{
    for (Message m : m_storage->getMessages(convId)) {
        if (m.id != msgId)
            continue;
        m.content = content;
        // Emits messageContentUpdated, which the chat UI uses to re‑render
        // the message in place.
        m_storage->updateMessageContent(m);
        return;
    }
}

void ChatManager::registerHumanEditorSession(Core::IDocument *document,
                                             Core::IEditor *editor,
                                             const QString &convId,
                                             qint64 msgId,
                                             const QString &filePath)
{
    if (!document)
        return;
    m_humanEditorSessions.insert(document,
                                 HumanEditorSession{convId, msgId, document, editor, filePath});
}

bool ChatManager::isHumanEditorDocument(const Core::IDocument *document) const
{
    return m_humanEditorSessions.contains(document);
}

Core::IEditor *ChatManager::humanEditorEditor(const QString &convId, bool pendingOnly) const
{
    for (const HumanEditorSession &session : std::as_const(m_humanEditorSessions)) {
        if (session.convId != convId)
            continue;
        if (pendingOnly && session.msgId > 0)
            continue;
        return session.editor;
    }
    return nullptr;
}

LlamaCppServerProps ChatManager::serverProps() const
{
    return m_serverProps;
}

static Message createToolMessage(const Message &assistantCall)
{
    Message toolMsg;
    toolMsg.convId = assistantCall.convId;
    toolMsg.type = "text";
    toolMsg.timestamp = QDateTime::currentMSecsSinceEpoch();
    toolMsg.role = "tool";
    toolMsg.content = QString();
    toolMsg.parent = assistantCall.id; // link to the assistant that called it
    toolMsg.children.clear();

    toolMsg.extra << assistantCall.extra; // copy the tool calling for display
    return toolMsg;
}

namespace {

// A base64 image in the prompt is pure token cost, and the model only
// needs it while it is actively looking at the result.  Image content
// parts are therefore kept only in the last few messages; in older tool
// results they are replaced by a text note (the text part stays).
constexpr int kImageContextMessages = 3;

QJsonValue toolResultContentForApi(const QJsonValue &content, bool keepImages)
{
    if (keepImages || !content.isArray())
        return content;
    QJsonArray parts;
    bool hasImage = false;
    for (const QJsonValue &part : content.toArray()) {
        const QJsonObject obj = part.toObject();
        if (obj.value(QStringLiteral("type")).toString() == QLatin1String("image_url")) {
            hasImage = true;
            QJsonObject note;
            note[QStringLiteral("type")] = QStringLiteral("text");
            note[QStringLiteral("text")] = QStringLiteral("[image omitted from context]");
            parts.append(note);
        } else {
            parts.append(part);
        }
    }
    return hasImage ? QJsonValue(parts) : content;
}

} // namespace

QJsonArray ChatManager::normalizeMsgsForAPI(const QVector<Message> &msgs)
{
    QJsonArray res;

    // Parallel tool calls: filterByLeafNodeId() follows a single parent
    // chain, so of several sibling tool results only the one on the leaf
    // path survives the filter.  Collect the missing siblings (keyed by
    // their assistant message) so that every tool_call in the request has
    // a matching tool result – a dangling tool_call confuses the model and
    // violates the chat protocol.
    QMap<qint64, QVector<Message>> missingToolResults;
    if (!msgs.isEmpty()) {
        QSet<qint64> keptIds;
        bool hasToolCalls = false;
        QSet<QString> callIds;
        QSet<QString> resultIds;
        for (const Message &m : msgs) {
            keptIds.insert(m.id);
            for (const QVariantMap &e : m.extra) {
                if (m.role == "assistant" && e.contains("tool_calls")) {
                    hasToolCalls = true;
                    for (const QJsonValue &v : e["tool_calls"].toJsonArray())
                        callIds.insert(v.toObject().value("id").toString());
                } else if (m.role == "tool" && e.contains("tool_result")) {
                    resultIds.insert(e["tool_result"].toJsonObject()
                                        .value("tool_call_id").toString());
                }
            }
        }
        // Only fetch the conversation from storage when the leaf‑path filter
        // actually dropped a result; a full getMessages() per request is
        // wasteful in long conversations.
        const QSet<QString> unresolved = callIds - resultIds;
        if (hasToolCalls && !unresolved.isEmpty()) {
            const QVector<Message> all = m_storage->getMessages(msgs.first().convId);
            for (const Message &m : all) {
                if (m.role != "tool" || keptIds.contains(m.id))
                    continue;
                for (const QVariantMap &e : m.extra)
                    if (e.contains("tool_result")) {
                        missingToolResults[m.parent].append(m);
                        break;
                    }
            }
        }
    }

    QString sysMsgText = LlamaCpp::settings().systemMessage.value();
    const bool isTaskConversation = !msgs.isEmpty()
            && m_taskConversations.contains(msgs.first().convId);
    if (!msgs.isEmpty()) {
        // Task conversations run with their own (sub‑agent) system prompt.
        const auto it = m_taskConfigs.constFind(msgs.first().convId);
        if (it != m_taskConfigs.constEnd() && !it->systemPrompt.trimmed().isEmpty())
            sysMsgText = it->systemPrompt;
    }
    if (!isTaskConversation) {
        // Append the project instructions (AGENTS.md / CLAUDE.md) for
        // regular conversations; task conversations get their own sub‑agent
        // system prompt and no ambient project context.
        Project *project = ProjectManager::startupProject();
        if (project && projectInstructionsEnabled(project)) {
            const QString instructions = loadProjectInstructions(project->projectDirectory());
            if (!instructions.isEmpty())
                sysMsgText = sysMsgText.trimmed().isEmpty()
                        ? instructions
                        : sysMsgText + QStringLiteral("\n\n") + instructions;
        }

        // Advertise the enabled skills (name + description) so the model
        // can load the matching one with the skill tool when a task calls
        // for it.
        if (settings().toolsEnabled() && isToolEnabled(QStringLiteral("skill"))) {
            const QString skillsPrompt = Skills::formatForPrompt(Skills::enabledSkills());
            if (!skillsPrompt.isEmpty())
                sysMsgText = sysMsgText.trimmed().isEmpty()
                        ? skillsPrompt
                        : sysMsgText + QStringLiteral("\n\n") + skillsPrompt;
        }
    }
    if (!sysMsgText.trimmed().isEmpty()) {
        QJsonObject sys;
        sys["role"] = "system";
        sys["content"] = sysMsgText;
        res.append(sys);
    }

    for (int i = 0; i < msgs.size(); ++i) {
        const Message &msg = msgs.at(i);
        const bool keepImages = msgs.size() - i <= kImageContextMessages;
        if (msg.role != "user" || msg.extra.isEmpty()) {
            QJsonObject out;
            out["role"] = msg.role;
            out["content"] = msg.content;

            if (msg.role == "assistant") {
                for (const QVariantMap &e : msg.extra) {
                    if (e.contains("tool_calls"))
                        out["tool_calls"] = e["tool_calls"].toJsonArray();
                }
            } else if (msg.role == "tool") {
                for (const QVariantMap &e : msg.extra) {
                    if (e.contains("tool_result")) {
                        QJsonObject toolOut = e["tool_result"].toJsonObject();
                        toolOut["content"] =
                                toolResultContentForApi(toolOut.value("content"), keepImages);
                        res.append(toolOut);
                    }
                }
            }

            res.append(out);

            // Re‑add the tool results of parallel calls that the leaf‑path
            // filter dropped, right after their assistant message.
            if (msg.role == "assistant") {
                const auto sit = missingToolResults.constFind(msg.id);
                if (sit != missingToolResults.constEnd()) {
                    for (const Message &sib : sit.value()) {
                        for (const QVariantMap &e : sib.extra) {
                            if (e.contains("tool_result")) {
                                QJsonObject toolOut = e["tool_result"].toJsonObject();
                                toolOut["content"] = toolResultContentForApi(
                                    toolOut.value("content"), keepImages);
                                res.append(toolOut);
                            }
                        }
                        // Mirror what the loop appends for a regular tool
                        // message (the empty role/content object), so the
                        // stream shape is identical to the one the model
                        // saw while the results streamed in.
                        QJsonObject sibOut;
                        sibOut["role"] = QStringLiteral("tool");
                        sibOut["content"] = sib.content;
                        res.append(sibOut);
                    }
                }
            }

            continue;
        }

        // user msg with extra – build array of parts
        QJsonArray parts;
        for (const QVariantMap &e : msg.extra) {
            if (e.value("type").toString() == "context") {
                QJsonObject p;
                p["type"] = "text";
                p["text"] = e.value("content").toString();
                parts.append(p);
            } else if (e.value("type").toString() == "textFile") {
                QJsonObject p;
                p["type"] = "text";
                p["text"] = QString("File: %1\nContent:\n\n%2")
                                .arg(e.value("name").toString())
                                .arg(e.value("content").toString());
                parts.append(p);
            } else if (e.value("type").toString() == "imageFile") {
                QJsonObject p;
                p["type"] = "image_url";
                p["image_url"] = QJsonObject{{"url", e.value("base64Url").toString()}};
                parts.append(p);
            } else if (e.value("type").toString() == "audioFile") {
                QJsonObject p;
                p["type"] = "input_audio";
                p["input_audio"] = QJsonObject{{"data", e.value("base64Data").toString()},
                                               {"format",
                                                e.value("mimeType").toString().contains("wav")
                                                    ? "wav"
                                                    : "mp3"}};
                parts.append(p);
            }
        }

        // user text at the end
        QJsonObject txt;
        txt["type"] = "text";
        txt["text"] = msg.content;
        parts.append(txt);

        QJsonObject out;
        out["role"] = msg.role;
        out["content"] = parts;
        res.append(out); // we only need the role/content to build APIMessage
    }
    return res;
}

void ChatManager::sendChatRequest(const QString &convId,
                                  const std::function<void(QJsonObject &)> &payloadBuilder,
                                  std::function<void(qint64)> onChunk)
{
    QJsonObject payload;
    payloadBuilder(payload); // <- fills the request‑specific fields

    payload["stream"] = true;
    payload["cache_prompt"] = true;
    payload["reasoning_format"] = "deepseek";
    payload["reasoning_in_content"] = "false";
    addCommonPayloadParams(payload, m_selectedModel);

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());
    m_abortControllers[convId] = reply; // (same map used elsewhere)
    qCDebug(llamaChatNetwork).noquote() << "Request payload:" << QJsonDocument(payload).toJson();

    readSSEStream(
        reply,
        [this, convId, onChunk](const QJsonObject &chunk) {
            if (chunk.contains("error")) {
                qCWarning(llamaChatNetwork)
                    << "SSE error:" << chunk["error"].toObject()["message"].toString();
                return;
            }

            if (chunk.contains("prompt_progress")) {
                QJsonObject progressObj = chunk["prompt_progress"].toObject();
                Message &pm = m_pendingMessages[convId];

                pm.promptProgress.total = progressObj["total"].toInt();
                pm.promptProgress.cache = progressObj["cache"].toInt();
                pm.promptProgress.processed = progressObj["processed"].toInt();
                pm.promptProgress.time_ms = progressObj["time_ms"].toInteger();

                emit pendingMessageChanged(pm);
            }

            if (chunk.contains("timings")) {
                QJsonObject t = chunk["timings"].toObject();
                TimingReport tr;
                tr.cache_n = t["cache_n"].toDouble();
                tr.prompt_n = t["prompt_n"].toDouble();
                tr.prompt_ms = t["prompt_ms"].toDouble();
                tr.predicted_n = t["predicted_n"].toDouble();
                tr.predicted_ms = t["predicted_ms"].toDouble();
                m_pendingMessages[convId].timings = tr;
            }

            QJsonArray choices = chunk["choices"].toArray();
            if (!choices.isEmpty()) {
                const QJsonObject &delta = choices[0].toObject()["delta"].toObject();

                if (delta.contains("reasoning_content")) {
                    QString reasoningAdded = delta["reasoning_content"].toString();
                    if (!reasoningAdded.isEmpty()) {
                        Message &pm = m_pendingMessages[convId];

                        if (pm.content.isEmpty())
                            pm.content = ThinkingSectionParser::startToken();

                        pm.content += reasoningAdded;
                        emit pendingMessageChanged(pm);
                    }
                }

                if (delta.contains("content")) {
                    QString added = delta["content"].toString();
                    if (!added.isEmpty()) {
                        Message &pm = m_pendingMessages[convId];
                        if (!pm.haveContent
                            && pm.content.startsWith(ThinkingSectionParser::startToken())) {
                            pm.content += ThinkingSectionParser::endToken();
                            pm.haveContent = true;
                        }
                        pm.content += added;
                        emit pendingMessageChanged(pm);
                    }
                }

                for (const QJsonValue &choiceVal : choices) {
                    const QJsonObject &choice = choiceVal.toObject();
                    const QJsonObject &delta = choice["delta"].toObject();
                    if (!delta.contains("tool_calls"))
                        continue;

                    Message &pm = m_pendingMessages[convId];
                    if (!pm.haveContent
                        && pm.content.startsWith(ThinkingSectionParser::startToken())) {
                        pm.content += ThinkingSectionParser::endToken();
                        pm.haveContent = true;
                    }

                    const QJsonArray &toolCalls = delta["tool_calls"].toArray();
                    for (const QJsonValue &tcVal : toolCalls) {
                        const QJsonObject &tc = tcVal.toObject();
                        const QString toolId = tc["id"].toString();
                        const int index = tc["index"].toInt();

                        while (index >= m_toolCalls.size())
                            m_toolCalls.emplace_back();

                        if (index < 0)
                            continue;

                        ToolCall &tool = m_toolCalls[index];
                        if (!toolId.isEmpty())
                            tool.id = toolId;

                        if (tc.contains("function")) {
                            const QJsonObject &func = tc["function"].toObject();
                            if (func.contains("name"))
                                tool.name = func["name"].toString();
                            if (func.contains("arguments"))
                                tool.arguments += func["arguments"].toString();
                        }

                        if (!tool.name.isEmpty()) {
                            pm.toolCallInProgress = tool.name;
                            pm.toolCallPreview.clear();
                            if (!m_streamingTools.contains(tool.name))
                                m_streamingTools.insert(tool.name,
                                                        ToolFactory::instance().create(tool.name));
                            const auto it = m_streamingTools.find(tool.name);
                            if (it != m_streamingTools.end() && it.value())
                                pm.toolCallPreview = it.value()->streamingSummary(tool.arguments);
                        }

                        QJsonParseError err;
                        QJsonDocument parsed = QJsonDocument::fromJson(tool.arguments.toUtf8(),
                                                                       &err);
                        if (err.error == QJsonParseError::NoError) {
                            QVariantMap extra;
                            extra["tool_calls"] = QJsonArray{
                                QJsonObject{{"id", tool.id},
                                            {"type", "function"},
                                            {"function",
                                             QJsonObject{{"name", tool.name},
                                                         {"arguments", tool.arguments}}}}};
                            pm.extra << extra;

                            m_toolCalls.remove(index);
                        }

                        emit pendingMessageChanged(pm);
                    }
                }
            }
            onChunk(-1); // UI scroll‑to‑bottom
        },
        [this, convId](const QString &err) {
            qCWarning(llamaChatNetwork) << "SSE stream error:" << err;

            if (m_abortControllers.contains(convId))
                m_abortControllers[convId]->deleteLater();
        });

    QObject::connect(reply, &QNetworkReply::finished, [this, convId, reply] {
        reply->deleteLater();
        m_abortControllers.remove(convId);

        Message pm = m_pendingMessages.take(convId);
        m_storage->appendMsg(pm, pm.parent);

        const bool isTaskConversation = m_taskConversations.contains(convId);

        if (pm.role == "assistant") {
            auto msgs = m_storage->getMessages(convId);
            const bool doSummarization = msgs.size() == 3 && !isTaskConversation;
            bool haveToolExecution = false;

            for (const QVariantMap &e : pm.extra) {
                if (e.contains("tool_calls")) {
                    // Store the "tool_calls" into the database
                    m_storage->updateMessageExtra(pm, pm.extra);

                    QJsonArray array = e["tool_calls"].toJsonArray();
                    QVector<ToolCall> tools;
                    for (const QJsonValue &v : array) {
                        QJsonObject obj = v.toObject();
                        ToolCall tool;
                        tool.id = obj["id"].toString();
                        obj = obj["function"].toObject();
                        tool.name = obj["name"].toString();
                        tool.arguments = obj["arguments"].toString();
                        tools.append(tool);
                    }
                    // Parallel tool calls: the next assistant turn is
                    // requested only once *all* results are in, so the
                    // follow‑up request carries every tool result (the
                    // missing siblings are re‑added by
                    // normalizeMsgsForAPI()).  Without this, each finished
                    // tool would trigger its own request and the
                    // isGenerating() guard would drop all but the first.
                    auto batchRemaining = std::make_shared<int>(tools.size());
                    for (const ToolCall &tool : tools) {
                        executeToolAndSendResult(convId, pm, tool, [](qint64) {}, batchRemaining);
                        haveToolExecution = true;
                    }
                }
            }

            if (doSummarization) { // first assistant reply
                summarizeConversationTitle(convId, pm.id, [this, convId](const QString &title) {
                    auto [thinking, shortTitle] = ThinkingSectionParser::parseThinkingSection(title);
                    renameConversation(convId, shortTitle);
                });
            }

            if (!haveToolExecution) {
                if (isTaskConversation) {
                    // The sub‑agent reached its final answer (or the stream
                    // was aborted / produced nothing) – hand the result back
                    // to the "task" tool waiting in the parent conversation.
                    const bool ok = (reply->error() == QNetworkReply::NoError)
                                    && !pm.content.trimmed().isEmpty();
                    emit taskConversationFinished(convId, pm.content, ok);
                } else if (reply->error() == QNetworkReply::NoError
                           && !pm.content.trimmed().isEmpty()) {
                    // Only suggest follow‑ups for complete generations.
                    followUpQuestions(convId,
                                      pm.id,
                                      [this, convId, leafNodeId = pm.id](const QStringList &questions) {
                                          emit followUpQuestionsReceived(convId, leafNodeId, questions);
                                      });
                }
            }
        }
    });
}

void ChatManager::executeToolAndSendResult(const QString &convId,
                                           const Message &assistantMsg,
                                           const ToolCall &tool,
                                           std::function<void(qint64)> onChunk,
                                           std::shared_ptr<int> batchRemaining)
{
    // Append a synthetic “failed” tool‑result and continue the conversation,
    // so the model gets another turn to react (and retry) instead of the
    // conversation dying on a tool error.
    auto sendFailedToolResult = [this, convId, &assistantMsg, &tool, onChunk,
                                 batchRemaining](const QString &content) {
        Message toolMsg = createToolMessage(assistantMsg);
        QJsonObject toolJsonMsg;
        toolJsonMsg["role"] = "tool";
        toolJsonMsg["tool_call_id"] = tool.id;
        toolJsonMsg["name"] = tool.name;
        toolJsonMsg["content"] = content;
        QVariantMap toolResultExtra;
        toolResultExtra["tool_result"] = toolJsonMsg;
        toolResultExtra["tool_status"] = QStringLiteral("failed");
        toolMsg.extra << toolResultExtra;
        m_storage->appendMsg(toolMsg, assistantMsg.id);
        onChunk(toolMsg.id);
        if (batchRemaining && --*batchRemaining > 0)
            return; // sibling tool calls of the same message are still running
        generateMessage(convId, toolMsg.id, onChunk);
    };

    // Check whether the requested tool is enabled.
    if (!isToolEnabled(tool.name)) {
        qCWarning(llamaChatTools) << "Tool" << tool.name
                                  << "was called but is disabled – skipping.";
        sendFailedToolResult(QStringLiteral("Tool disabled"));
        return;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(tool.arguments.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        // Small models often emit raw control characters (real newlines in a
        // command/content string) or invalid escapes – try a string‑literal
        // repair before giving up.
        const QString repaired = repairJson(tool.arguments);
        if (repaired != tool.arguments)
            doc = QJsonDocument::fromJson(repaired.toUtf8(), &err);
    }
    if (err.error != QJsonParseError::NoError) {
        qCWarning(llamaChatTools)
            << "Tool args JSON malformed:" << err.errorString() << tool.arguments.toUtf8();
        sendFailedToolResult(
            QStringLiteral("Tool arguments were not valid JSON (%1). Call the tool "
                           "again with valid JSON arguments.")
                .arg(err.errorString()));
        return;
    }

    qCInfo(llamaChatTools).noquote() << "Calling tool:" << tool.name << "with arguments:\n"
                                     << tool.arguments;

    std::unique_ptr<Tool> realTool = ToolFactory::instance().create(tool.name);
    if (!realTool) {
        qCWarning(llamaChatTools) << "Unsupported tool:" << tool.name;
        sendFailedToolResult(QStringLiteral("Unknown tool: %1").arg(tool.name));
        return;
    }

    Message toolMsg = createToolMessage(assistantMsg);

    // The tool runs asynchronously – keep the conversation busy (spinner
    // spinning, no new messages accepted) until it has reported back.
    m_runningTools[convId]++;

    m_storage->appendMsg(toolMsg, assistantMsg.id);
    onChunk(toolMsg.id);

    // Tools that report live output (bash) show the current output tail in
    // the running tool bubble; it is persisted at most every
    // kLiveOutputIntervalMs and dropped again when the result arrives.
    static constexpr int kLiveOutputIntervalMs = 400;
    auto sharedMsg = std::make_shared<Message>(toolMsg);
    auto lastLiveUpdate = std::make_shared<qint64>(0);
    auto publishLiveOutput = [this, sharedMsg, lastLiveUpdate](const QString &tail) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - *lastLiveUpdate < kLiveOutputIntervalMs)
            return;
        *lastLiveUpdate = now;

        auto extra = sharedMsg->extra;
        for (auto &e : extra)
            e.remove(QStringLiteral("tool_live_output"));
        QVariantMap live;
        live[QStringLiteral("tool_live_output")] = tail;
        extra << live;
        sharedMsg->extra = extra;
        // Storage emits messageExtraUpdated, which the chat view follows.
        m_storage->updateMessageExtra(*sharedMsg, extra);
    };

    auto toolFinished = [this, convId, sharedMsg, tool, onChunk,
                          batchRemaining](const QString &toolOutput,
                                          bool ok) mutable {
        QJsonObject toolJsonMsg;
        toolJsonMsg["role"] = "tool";
        toolJsonMsg["tool_call_id"] = tool.id;
        toolJsonMsg["name"] = tool.name;
        // An image attachment (read_file on an image) travels as content
        // parts – text plus image_url – which llama-server tokenizes for
        // vision models.
        QString toolText = toolOutput;
        QString dataUrl;
        if (splitToolResultImage(toolOutput, toolText, dataUrl)) {
            QJsonArray parts;
            QJsonObject textPart;
            textPart["type"] = QStringLiteral("text");
            textPart["text"] = toolText;
            parts.append(textPart);
            QJsonObject imagePart;
            imagePart["type"] = QStringLiteral("image_url");
            imagePart["image_url"] = QJsonObject{{"url", dataUrl}};
            parts.append(imagePart);
            toolJsonMsg["content"] = parts;
        } else {
            toolJsonMsg["content"] = toolOutput;
        }
        QVariantMap toolResultExtra;
        toolResultExtra["tool_result"] = toolJsonMsg;
        toolResultExtra["tool_status"] = ok ? QStringLiteral("success") : "failed";

        Message &toolMsg = *sharedMsg;
        for (auto &e : toolMsg.extra)
            e.remove(QStringLiteral("tool_live_output"));
        toolMsg.extra << toolResultExtra;
        m_storage->updateMessageExtra(toolMsg, toolMsg.extra);

        if (m_runningTools.value(convId) > 0)
            --m_runningTools[convId];

        // Generate the assistant reply only once every tool call of the
        // message has reported back; the request then carries all results.
        if (batchRemaining && --*batchRemaining > 0)
            return;
        generateMessage(toolMsg.convId, toolMsg.id, onChunk);
    };

    if (realTool->supportsLiveOutput())
        realTool->runLive(doc.object(), publishLiveOutput, std::move(toolFinished));
    else
        realTool->run(doc.object(), std::move(toolFinished));
}

void ChatManager::deleteConversation(const QString &convId)
{
    // A task conversation deleted while its sub‑agent is still running must
    // fail the waiting "task" tool in the parent conversation.
    if (m_taskConversations.contains(convId)) {
        m_taskConversations.remove(convId);
        emit taskConversationFinished(convId, {}, false);
    }
    m_taskConfigs.remove(convId);

    m_storage->deleteConversation(convId);
}

void ChatManager::renameConversation(const QString &convId, const QString &name)
{
    if (name.isEmpty())
        return;

    m_storage->renameConversation(convId, name);
}

void ChatManager::deleteMessageBranch(const QString &convId, qint64 msgId)
{
    if (m_storage->deleteMessageBranch(msgId))
        emit messageDeleted(convId);
}

QPair<int, int> ChatManager::getBranchStats(const QString &convId, qint64 msgId) const
{
    QPair<int, int> stats{0, 0};
    int &userMessages = stats.first;
    int &assistantMessages = stats.second;

    QVector<Message> allMessages = m_storage->getMessages(convId);

    // Map for quick lookup
    QHash<qint64, Message> map;
    for (const auto &m : std::as_const(allMessages)) {
        map.insert(m.id, m);
    }

    if (!map.contains(msgId))
        return stats;

    // Traverse the branch using a stack (DFS)
    QVector<qint64> stack;
    stack.push_back(msgId);

    while (!stack.isEmpty()) {
        qint64 currentId = stack.takeLast();
        if (!map.contains(currentId))
            continue;

        const Message &m = map[currentId];
        if (m.role == "user") {
            ++userMessages;
        } else {
            ++assistantMessages;
        }

        // Add children to stack to continue traversal
        for (qint64 childId : m.children) {
            stack.push_back(childId);
        }
    }

    return stats;
}

} // namespace LlamaCpp
