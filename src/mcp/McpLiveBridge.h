#pragma once
#include "McpServer.h"
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <map>
#include <memory>
class QLocalSocket;
namespace serika {
class MainWindow;
// Lets `SerikaPhotoEdit --mcp --attach` sessions drive the documents open in this window. Listens on
// a per-user local socket (named pipe on Windows) restricted to the current user; each connection
// gets its own McpServer whose document host is the window's tabs.
class McpLiveBridge final : public QObject {
  public:
    explicit McpLiveBridge(MainWindow *window);
    ~McpLiveBridge() override;
    bool listening() const { return m_server.isListening(); }
    QString errorString() const { return m_server.errorString(); }
    int sessionCount() const { return int(m_sessions.size()); }

  private:
    struct Session {
        std::unique_ptr<McpServer> server;
        // busy while a request runs (AI tools spin a local event loop); closed defers cleanup.
        bool attached = false, busy = false, closed = false;
    };
    QPointer<MainWindow> m_window;
    QLocalServer m_server;
    std::map<QLocalSocket *, Session> m_sessions;
    void accept();
    void drain(QLocalSocket *socket);
    void forget(QLocalSocket *socket);
    QByteArray respond(Session &session, const QByteArray &line);
};
} // namespace serika
