#pragma once

#include <QJsonValue>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace LlamaCpp {

class Tool
{
public:
    virtual ~Tool() = default;

    /*! Returns the name that appears in the JSON schema, e.g. "bash",
        "apply_patch", … */
    virtual QString name() const = 0;

    /*! Returns a short human‑readable summary of the tool call derived from
        the (possibly incomplete) raw argument JSON that is being streamed,
        e.g. "Add src/main.cpp".  Return an empty string when no meaningful
        summary can be derived yet; the UI then falls back to the tool name. */
    virtual QString streamingSummary(const QString &partialArguments) const { return {}; }

    /*! Returns the JSON tool defintion */
    virtual QString toolDefinition() const = 0;

    /*! Human‑readable one‑liner that will be shown as the <summary>
        of the <details> block, e.g.   "> running python"
        or "> create file src/main.cpp". */
    virtual QString oneLineSummary(const QJsonObject &arguments) const = 0;

    /*! Full markdown that will be placed **inside** the <details> block.
        \a ok tells whether the tool run succeeded; pass it on to tools
        whose presentation depends on the outcome.  The default
        implementation can be overridden when the tool wants a custom view
        (diff, code block, table, …). */
    virtual QString detailsMarkdown(const QJsonObject &arguments,
                                    const QString &result,
                                    bool ok) const;

    /*! Returns a short markdown preview of the outcome that is shown in the
        tool call's summary (below the one‑line description), so the result
        is visible without expanding the details – like pi and opencode show
        a few output lines on a collapsed tool call.  The preview must stay
        small (a few lines); return an empty string to show nothing.  The
        default derives it from detailsMarkdown() and is empty on failure:
        the ✗ icon in the summary marks the call as failed, and the error
        text is only shown in the expanded details. */
    virtual QString summaryPreview(const QJsonObject &arguments,
                                   const QString &result,
                                   bool ok) const;

    /*! Executes the tool.  The concrete class may delegate to the old free
        function (runPython, editFile, …) or implement a new algorithm.
        done is a callback that **must** be called exactly once when the
        tool finishes.  The first argument is the textual output,
        the second argument tells whether the run was successful. */
    virtual void run(const QJsonObject &arguments,
                     std::function<void(const QString &output, bool ok)> done) const
        = 0;

    //! @p onOutput receives the tail of the output produced so far while
    //! the tool runs (throttling is the caller's job).
    using OutputHandler = std::function<void(const QString &partialOutput)>;

    /*! Whether runLive() reports live output while the tool runs. */
    virtual bool supportsLiveOutput() const { return false; }

    /*! Like run(), but additionally reports the output tail while the
        tool runs.  The default implementation ignores the live channel; */
    virtual void runLive(const QJsonObject &arguments,
                         const OutputHandler &onOutput,
                         std::function<void(const QString &output, bool ok)> done) const
    {
        Q_UNUSED(onOutput);
        run(arguments, done);
    }

    /*! Aborts an in‑flight execution because the user asked to stop
        (pressed Escape).  Tools that run a process or a network request
        should terminate it, so their done callback is then invoked with
        ok=false.  The default does nothing: the run simply finishes on its
        own, and ChatManager makes sure the conversation does not continue
        after it.  Must be safe to call before run() started its work, after
        it finished, and more than once.  The done callback must eventually
        be invoked in all cases – ChatManager only unblocks the conversation
        (and clears its stop flag) when every aborted tool has reported
        back. */
    virtual void abort() {}
};

//! Wraps \a content in a markdown code fence (optionally with an \a info
//! string, e.g. "diff").  The fence is made of at least one backtick more
//! than the longest backtick run in \a content, so content that itself
//! contains triple backticks (markdown source files, code snippets in
//! command output) cannot close the fence early and be rendered as live
//! markdown.
QString codeFence(const QString &content, const QString &info = {});

//! Wraps \a text in a markdown inline code span.  A path that itself
//! contains a backtick is wrapped in double backticks (space‑padded, so
//! CommonMark strips the padding back off), which cannot be closed by the
//! inner backtick.
QString codeSpan(const QString &text);

//! Truncates \a text for inline display: at most \a maxLines lines and 300
//! characters.  A code fence left open by the cut is closed so the markdown
//! stays well‑formed.  No ellipsis is appended: the details header already
//! gets the expand icon, which signals more content.  Returns an empty
//! string for empty input.
QString truncatedPreview(const QString &text, int maxLines);

//! Collapsed‑view preview for tools whose result is a plain list of lines
//! (find, ls, search): the first few lines inside a well‑formed code fence.
//! The default preview would truncate detailsMarkdown() after three lines,
//! which for these tools lands right after the opening fence of the result
//! block and renders an empty code block.  On failure (or for an empty
//! result) returns an empty string: the ✗ icon marks the failed call, and
//! the error text is only shown in the expanded details.
QString listPreview(const QString &result, bool ok);

//! Wraps \a text and an image \a dataUrl (e.g. "data:image/png;base64,\u2026")
//! into the single result string a Tool::run() done‑callback can carry.
//! ChatManager recognises the wrapper and stores/sends the tool result as
//! content parts (text + image_url) so a vision model can see the image;
//! the markers never reach storage or the UI.
QString toolResultWithImage(const QString &text, const QString &dataUrl);

//! Splits a string created by toolResultWithImage() back into \a text and
//! \a dataUrl.  Returns false, leaving both untouched, when \a output
//! carries no image.
bool splitToolResultImage(const QString &output, QString &text, QString &dataUrl);

//! Display text of a tool‑result "content" value that is either a plain
//! string or an array of content parts (text + image_url); image parts are
//! skipped.
QString toolResultText(const QJsonValue &content);

} // namespace LlamaCpp
