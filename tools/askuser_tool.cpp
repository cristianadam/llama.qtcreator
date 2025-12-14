#include "askuser_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>

namespace LlamaCpp {

namespace {
// Register the tool with the global factory
const bool registered = [] {
    ToolFactory::instance().registerCreator(AskUserTool{}.name(),
                                            []() { return std::make_unique<AskUserTool>(); });
    return true;
}();
} // namespace

QString AskUserTool::name() const
{
    return QStringLiteral("ask_user");
}

QString AskUserTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "ask_user",
            "description": "Use this tool to ask the user for clarifications if something is unclear.",
            "parameters": {
                "type": "object",
                "properties": {
                    "question": { "type": "string", "description": "The question to the user." }
                },
                "required": [ "question" ],
                "strict": true
            }
        }
    })raw";
}

QString AskUserTool::oneLineSummary(const QJsonObject &args) const
{
    Q_UNUSED(args);
    return QStringLiteral("ask user");
}

namespace {
void showAskUserDialog(const QString &question, std::function<void(const QString &, bool)> done)
{
    QDialog dialog;
    dialog.setWindowTitle(Tr::tr("Ask the user"));
    dialog.setModal(true);
    dialog.resize(400, 200);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // The question label (read‑only)
    auto *questionLabel = new QLabel(question, &dialog);
    questionLabel->setWordWrap(true);
    questionLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    layout->addWidget(questionLabel);

    // The answer edit – QTextEdit gives us multi‑line input out of the box
    auto *answerEdit = new QTextEdit(&dialog);
    answerEdit->setPlaceholderText(Tr::tr("Type your answer here ..."));
    layout->addWidget(answerEdit, 1); // stretch factor = 1 (takes remaining space)

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                           Qt::Horizontal,
                                           &dialog);
    layout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        const QString answer = answerEdit->toPlainText();
        done(answer, true);
    } else {
        done(QString(), false);
    }
}
} // anonymous namespace

void AskUserTool::run(const QJsonObject &arguments,
                      std::function<void(const QString &, bool)> done) const
{
    const QString question = arguments.value(QStringLiteral("question")).toString();

    if (QApplication::instance()->thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(
            QApplication::instance(),
            [question, done = std::move(done)]() mutable {
                showAskUserDialog(question, std::move(done));
            },
            Qt::QueuedConnection);
        return;
    }

    showAskUserDialog(question, std::move(done));
}

} // namespace LlamaCpp
