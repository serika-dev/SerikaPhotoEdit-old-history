#include "actions/ActionRunner.h"
#include "compositor/GpuProcessor.h"
#include "io/FormatIO.h"
#include "mcp/McpLiveBridge.h"
#include "mcp/McpServer.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QColorSpace>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>
#include <memory>
#include <serika/Version.h>
#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#endif
using namespace serika;
static int batch(const QStringList &args) {
    QTextStream output(stdout), errors(stderr);
    if (args.size() != 3) {
        errors << "Usage: SerikaPhotoEdit --batch action.json input-folder output-folder\n";
        return 2;
    }
    QJsonArray steps;
    QString actionError;
    if (!ActionRunner::load(args[0], &steps, &actionError)) {
        errors << actionError << '\n';
        return 2;
    }
    QDir input(args[1]);
    QDir().mkpath(args[2]);
    int failures = 0;
    for (const auto &entry : input.entryInfoList(QDir::Files)) {
        QString error;
        std::unique_ptr<Document> doc(FormatIO::open(entry.absoluteFilePath(), &error));
        if (!doc) {
            errors << entry.fileName() << ": " << error << '\n';
            failures++;
            continue;
        }
        if (!ActionRunner::run(doc.get(), steps, &error)) {
            errors << entry.fileName() << ": " << error << '\n';
            failures++;
            continue;
        }
        QString target = QDir(args[2]).filePath(entry.completeBaseName() + ".spe");
        if (!FormatIO::saveNative(doc.get(), target, &error)) {
            errors << error << '\n';
            failures++;
        } else
            output << target << '\n';
    }
    return failures ? 1 : 0;
}
int main(int argc, char **argv) {
    bool headless = false;
    for (int i = 1; i < argc; i++)
        if (QStringList{"--batch", "--mcp"}.contains(QString::fromLocal8Bit(argv[i])))
            headless = true;
    if (headless && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    if (headless && qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const QString systemFonts = QDir(qEnvironmentVariable("SystemRoot")).filePath("Fonts");
        if (QDir(systemFonts).exists())
            qputenv("QT_QPA_FONTDIR", QFile::encodeName(systemFonts));
    }
#endif
    QApplication app(argc, argv);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] { GpuProcessor::instance().shutdown(); });
    app.setApplicationName("SerikaPhotoEdit");
    app.setApplicationVersion(SERIKA_VERSION);
    app.setOrganizationName("Serika");
    app.setOrganizationDomain("serika.dev");
    initializeBrandFonts();
    app.setWindowIcon(QIcon(":/serika/logo.png"));
    app.setStyle("Fusion");
    QCommandLineParser parser;
    parser.setApplicationDescription("Serika PhotoEdit — native layer-based image editor");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"demo", "Open the original welcome project"});
    parser.addOption({"mcp", "Run the local MCP server on stdin/stdout (requires --mcp-root)"});
    parser.addOption({"mcp-root", "Existing workspace directory allowed for MCP file access", "directory"});
    parser.addOption(
        {"attach", "With --mcp: edit the documents open in the Serika window (starts it if needed)"});
    parser.addOption(
        {"no-launch", "With --mcp --attach: fail instead of starting Serika when it is not running"});
    parser.addOption({"gpu", "Enable supported GPU processing operations, with CPU fallback"});
    parser.addOption({"gpu-info", "Print graphics device and processing diagnostics, then exit"});
    parser.addOption({"screenshot", "Save a window capture and exit", "path"});
    parser.addOption(
        {"batch", "Apply an action to a folder; provide action, input and output positional arguments"});
    parser.addPositionalArgument("files", "Documents to open, or batch arguments");
    parser.process(app);
    if (parser.isSet("gpu"))
        GpuProcessor::instance().setEnabled(true);
    if (parser.isSet("gpu-info")) {
        QTextStream(stdout) << GpuProcessor::instance().diagnostics().summary() << '\n';
        GpuProcessor::instance().shutdown();
        return 0;
    }
    if (parser.isSet("mcp")) {
        if (parser.isSet("batch") || !parser.positionalArguments().isEmpty()) {
            QTextStream(stderr) << "--mcp cannot be combined with --batch or document arguments.\n";
            return 2;
        }
        const int result = parser.isSet("attach")
                               ? McpServer::runAttached(parser.value("mcp-root"), !parser.isSet("no-launch"))
                               : McpServer::runStdio(parser.value("mcp-root"));
        GpuProcessor::instance().shutdown();
        return result;
    }
    if (parser.isSet("batch")) {
        const int result = batch(parser.positionalArguments());
        GpuProcessor::instance().shutdown();
        return result;
    }
    MainWindow window;
    window.show();
    // Lets `--mcp --attach` agent sessions edit the documents open in this window.
    McpLiveBridge liveBridge(&window);
#ifdef Q_OS_WIN
    BOOL dark = app.palette().color(QPalette::Window).lightness() < 128;
    DwmSetWindowAttribute(reinterpret_cast<HWND>(window.winId()), 20, &dark, sizeof(dark));
#endif
    for (const auto &path : parser.positionalArguments())
        window.openFile(QFileInfo(path).absoluteFilePath());
    if (parser.isSet("demo"))
        window.openDemo();
    if (parser.isSet("screenshot")) {
        auto path = parser.value("screenshot");
        QTimer::singleShot(1500, &window, [&window, path, &app] {
            window.grab().save(path);
            app.quit();
        });
    }
    return app.exec();
}
