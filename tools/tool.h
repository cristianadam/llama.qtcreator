#pragma once

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
        default derives it from detailsMarkdown(). */
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
};

//! Truncates \a text for inline display: at most \a maxLines lines and 300
//! characters.  A code fence left open by the cut is closed so the markdown
//! stays well‑formed.  No ellipsis is appended: the details header already
//! gets the expand icon, which signals more content.  Returns an empty
//! string for empty input.
QString truncatedPreview(const QString &text, int maxLines);

} // namespace LlamaCpp
