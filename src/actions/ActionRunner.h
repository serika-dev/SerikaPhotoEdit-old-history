#pragma once
#include "document/Document.h"
#include <QJsonArray>

namespace serika {
// An action is a versioned JSON object with a "steps" array. Each step has
// a command, an optional name, and a parameters object. Execution is independent
// of the UI and fails explicitly when a step needs an unsupported operation.
class ActionRunner {
  public:
    static bool execute(Document *document, const QJsonObject &step, QString *error = nullptr);
    static bool run(Document *document, const QJsonArray &steps, QString *error = nullptr);
    static bool runFile(const QString &actionPath, const QString &inputPath, const QString &outputPath,
                        QString *error = nullptr);
    static bool load(const QString &path, QJsonArray *steps, QString *error = nullptr);
    // Filter names accepted by {"command": "filter", "name": ...}, matched case-insensitively.
    static QStringList filterNames();
};
} // namespace serika
