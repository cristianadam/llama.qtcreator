#pragma once
#include "tool.h"

#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QVector>

namespace LlamaCpp::Tools {

class EditFileTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString streamingSummary(const QString &partialArguments) const override;
    QString detailsMarkdown(const QJsonObject &args,
                                    const QString &result,
                                    bool ok) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

//! Applies a list of exact text replacements to \a content.
//!
//! Every replacement's old text is matched against the *original* content
//! (not incrementally); matched regions must not overlap.  Matching is
//! exact first (including whitespace); when that fails and the old text is
//! line-aligned, a fuzzy line-based match is used that tolerates trailing
//! whitespace and unicode punctuation differences (smart quotes, dashes,
//! non-breaking spaces).  When \a replaceAll is false the old text must
//! occur exactly once, otherwise all occurrences are replaced.
//!
//! The BOM and the dominant line ending (LF / CRLF) of \a content are
//! preserved; unchanged parts of the file keep their original bytes.
//!
//! On success the new content is written to \a newContentOut, the number of
//! replacements to \a replacementsOut (when non-null), and an empty string
//! is returned.  On failure a human‑readable error message is returned.
QString applyTextEdits(const QString &content,
                       const QVector<QPair<QString, QString>> &edits,
                       bool replaceAll,
                       QString &newContentOut,
                       int *replacementsOut = nullptr);

} // namespace LlamaCpp::Tools
