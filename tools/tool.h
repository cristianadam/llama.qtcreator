#pragma once

#include <QJsonObject>
#include <QString>

namespace LlamaCpp {

class Tool
{
public:
    virtual ~Tool() = default;

    /*! Returns the name that appears in the JSON schema, e.g. "python",
        "edit_file", … */
    virtual QString name() const = 0;

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
        The return value is the *result* that will later be shown in the
        details block. */
    virtual QString run(const QJsonObject &arguments) const = 0;
};

} // namespace LlamaCpp
