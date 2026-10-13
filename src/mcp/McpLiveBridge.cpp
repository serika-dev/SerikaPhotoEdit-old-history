#include "McpLiveBridge.h"
#include "ui/MainWindow.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QSettings>
#include <QUuid>

namespace serika {
namespace {
constexpr qint64 MaxLine = 4 * 1024 * 1024;
// Documents are the window's open tabs. IDs are stable per document for the window's lifetime.
class WindowHost final : public McpDocumentHost {
  public:
    explicit WindowHost(MainWindow *window) : m_window(window) {}
    bool live() const override { return true; }
    QStringList documentIds() override {
        QStringList ids;
        if (m_window)
            for (auto *document : m_window->openDocuments())
                ids.append(idFor(document));
        return ids;
    }
    Document *document(const QString &id) override {
        if (m_window)
            for (auto *document : m_window->openDocuments())
                if (idFor(document) == id)
                    return document;
        return nullptr;
    }
    QString adopt(std::unique_ptr<Document> document) override {
        auto *raw = document.release();
        const auto id = idFor(raw);
        m_window->adoptDocument(raw);
        return id;
    }
    bool release(const QString &id, bool discard, QString *error) override {
        auto *target = document(id);
        if (!target) {
            *error = "Unknown session document ID.";
            return false;
        }
        return m_window->closeDocumentFor(target, discard, error);
    }
    int capacity() const override { return 32; }
    QString activeDocumentId() override {
        auto *current = m_window ? m_window->currentDocument() : nullptr;
        return current ? idFor(current) : QString();
    }
    bool activate(const QString &id) override {
        auto *target = document(id);
        return target && m_window->focusDocument(target);
    }

  private:
    QPointer<MainWindow> m_window;
    static QString idFor(Document *document) {
        auto id = document->property("mcpDocumentId").toString();
        if (id.isEmpty()) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            document->setProperty("mcpDocumentId", id);
        }
        return id;
    }
};
} // namespace

McpLiveBridge::McpLiveBridge(MainWindow *window) : QObject(window), m_window(window) {
    if (!QSettings().value("mcp/allowLive", true).toBool())
        return;
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(McpServer::liveSocketName());
    if (!m_server.listen(McpServer::liveSocketName()))
        return;
    connect(&m_server, &QLocalServer::newConnection, this, &McpLiveBridge::accept);
}
McpLiveBridge::~McpLiveBridge() {
    for (auto &[socket, session] : m_sessions)
        socket->disconnect(this);
}
void McpLiveBridge::accept() {
    while (auto *socket = m_server.nextPendingConnection()) {
        m_sessions[socket] = Session{};
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { drain(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            const auto found = m_sessions.find(socket);
            if (found == m_sessions.end())
                return;
            if (found->second.busy)
                found->second.closed = true; // drain() finishes the request, then forgets the socket
            else
                forget(socket);
        });
    }
}
void McpLiveBridge::forget(QLocalSocket *socket) {
    m_sessions.erase(socket);
    socket->deleteLater();
    if (m_window && m_sessions.empty())
        m_window->setAgentStatus("AI agent disconnected");
}
void McpLiveBridge::drain(QLocalSocket *socket) {
    const auto found = m_sessions.find(socket);
    if (found == m_sessions.end())
        return;
    // AI tools spin a local event loop; a nested readyRead must not interleave replies.
    if (found->second.busy)
        return;
    Session &session = found->second; // map nodes stay put; only forget() erases them
    session.busy = true;
    while (!session.closed && socket->canReadLine()) {
        const QByteArray reply = respond(session, socket->readLine(MaxLine + 2));
        if (session.closed)
            break;
        socket->write(reply + '\n');
        socket->flush();
    }
    session.busy = false;
    if (session.closed) {
        forget(socket);
        return;
    }
    if (socket->bytesAvailable() > MaxLine) {
        socket->write(
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                      {"id", QJsonValue::Null},
                                      {"error", QJsonObject{{"code", -32600},
                                                            {"message", "MCP message exceeds 4 MiB."}}}})
                .toJson(QJsonDocument::Compact) +
            '\n');
        socket->disconnectFromServer();
    }
}
QByteArray McpLiveBridge::respond(Session &session, const QByteArray &line) {
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(line, &parse);
    if (!session.attached) {
        // Handshake from the attach process: which workspace folder file tools may use.
        const auto hello = json.object();
        const QString workspace = hello["workspace"].toString();
        if (hello["serika_attach"].toInt() != 1)
            return R"({"ok":false,"error":"Not a Serika attach handshake."})";
        if (!QSettings().value("mcp/allowLive", true).toBool())
            return R"({"ok":false,"error":"Agent access is turned off in Preferences > AI."})";
        session.server = std::make_unique<McpServer>(workspace, std::make_shared<WindowHost>(m_window));
        if (!session.server->startupError().isEmpty())
            return QJsonDocument(QJsonObject{{"ok", false}, {"error", session.server->startupError()}})
                .toJson(QJsonDocument::Compact);
        session.attached = true;
        if (m_window)
            m_window->setAgentStatus("AI agent connected · workspace " + QFileInfo(workspace).fileName());
        return R"({"ok":true})";
    }
    std::optional<QJsonObject> response;
    if (parse.error != QJsonParseError::NoError)
        response = QJsonObject{{"jsonrpc", "2.0"},
                               {"id", QJsonValue::Null},
                               {"error", QJsonObject{{"code", -32700}, {"message", "Invalid JSON."}}}};
    else if (!json.isObject())
        response = QJsonObject{{"jsonrpc", "2.0"},
                               {"id", QJsonValue::Null},
                               {"error", QJsonObject{{"code", -32600},
                                                     {"message", "Requests must be JSON objects; batches are "
                                                                 "unsupported."}}}};
    else
        response = session.server->handle(json.object());
    // One reply line per request line; an empty line means JSON-RPC has nothing to return.
    return response ? QJsonDocument(*response).toJson(QJsonDocument::Compact) : QByteArray();
}
} // namespace serika
