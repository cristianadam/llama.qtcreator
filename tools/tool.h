#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>

namespace LlamaCpp {

class Tool
{
public:
    virtual ~Tool() = default;

    /*! Returns the name that appears in the JSON schema, e.g. "shell",
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
        The default implementation can be overridden when the tool wants a
        custom view (diff, code block, table, …). */
    virtual QString detailsMarkdown(const QJsonObject &arguments, const QString &result) const;

    /*! Executes the tool.  The concrete class may delegate to the old free
        function (runPython, editFile, …) or implement a new algorithm.
        done is a callback that **must** be called exactly once when the
        tool finishes.  The first argument is the textual output,
        the second argument tells whether the run was successful. */
    virtual void run(const QJsonObject &arguments,
                     std::function<void(const QString &output, bool ok)> done) const
        = 0;
};

} // namespace LlamaCpp
