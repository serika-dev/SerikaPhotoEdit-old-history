#pragma once
#include "document/Document.h"
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>
namespace serika {
// Where an MCP session's documents live. The isolated host owns private in-memory documents; the
// desktop app supplies a live host whose documents are its open tabs (ui/McpLiveBridge), so an
// agent's edits appear on screen and join the window's undo history.
class McpDocumentHost {
  public:
    virtual ~McpDocumentHost() = default;
    virtual bool live() const = 0;
    virtual QStringList documentIds() = 0;
    virtual Document *document(const QString &id) = 0;
    // Takes ownership of a newly created or opened document and returns its session ID.
    virtual QString adopt(std::unique_ptr<Document> document) = 0;
    virtual bool release(const QString &id, bool discard, QString *error) = 0;
    virtual int capacity() const = 0;
    virtual QString activeDocumentId() { return {}; }
    virtual bool activate(const QString &id) {
        Q_UNUSED(id);
        return false;
    }
};

class McpServer {
  public:
    explicit McpServer(const QString &workspace, std::shared_ptr<McpDocumentHost> host = {});
    QString startupError() const { return m_startupError; }
    std::optional<QJsonObject> handle(const QJsonObject &message);
    static QJsonArray toolDefinitions(bool live = false);
    // Documented action commands with examples; mcp_tests runs every example.
    static QJsonArray actionCatalog();
    static int runStdio(const QString &workspace);
    // Connects stdio to the running desktop app (starting it when `launch` is set), so tools edit
    // the documents open in its window.
    static int runAttached(const QString &workspace, bool launch);
    // Per-user local socket the desktop app listens on for attached MCP sessions.
    static QString liveSocketName();

  private:
    QString m_root, m_startupError;
    bool m_initialized = false, m_ready = false;
    std::shared_ptr<McpDocumentHost> m_host;
    QJsonObject call(const QString &name, const QJsonObject &args);
    QString checkedPath(const QString &path, bool write, QString *error) const;
    QJsonObject describe(const QString &id, const Document *document) const;
    bool readImageInput(const QString &path, QString *error) const;
};
} // namespace serika
