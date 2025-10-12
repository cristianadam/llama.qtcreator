#pragma once

#include <QList>
#include <QString>

namespace LlamaCpp::Tools {
QStringList getTools();

QString runPython(const QString &code);
QString listDirectory(const QString &rawPath);
QString readFile(const QString &relPath, int firstLine, int lastLineIncl, bool readAll);
QString regexSearch(const QString &includePattern,
                    const QString &excludePattern,
                    const QString &regex);
QString createFile(const QString &relPath, const QString &content);
QString deleteFile(const QString &relPath);
QString editFile(const QString &path,
                 const QString &operation,
                 const QString &search,
                 const QString &replace,
                 const QString &newContent);
QString diffForEditFile(const QString &filePath,
                        const QString &operation,
                        const QString &search,
                        const QString &replace,
                        const QString &newFileContent);

} // namespace LlamaCpp::Tools
