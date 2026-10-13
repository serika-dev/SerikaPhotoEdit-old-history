#include "io/FormatIO.h"
#include "mcp/McpLiveBridge.h"
#include "mcp/McpServer.h"
#include "ui/MainWindow.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace serika;
namespace {
QJsonObject request(const QString &method, QJsonObject params = {}) {
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
}
void initialize(McpServer &server) {
    server.handle(request("initialize", {{"protocolVersion", "2025-11-25"},
                                         {"capabilities", QJsonObject{}},
                                         {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}));
    server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
}
QJsonObject call(McpServer &server, const QString &name, QJsonObject args = {}) {
    return server.handle(request("tools/call", {{"name", name}, {"arguments", args}})).value();
}
QJsonObject result(const QJsonObject &response) { return response["result"].toObject(); }
QJsonObject data(const QJsonObject &response) { return result(response)["structuredContent"].toObject(); }
QString create(McpServer &server) {
    return data(call(server, "create_document",
                     {{"width", 32}, {"height", 24}, {"background", "white"}}))["document_id"]
        .toString();
}
bool failed(const QJsonObject &response) { return result(response)["isError"].toBool(); }
// 2026-07-28 requests carry their protocol version and identity in _meta instead of a handshake.
QJsonObject modern(const QString &method, QJsonObject params = {}, const QString &version = "2026-07-28") {
    params["_meta"] =
        QJsonObject{{"io.modelcontextprotocol/protocolVersion", version},
                    {"io.modelcontextprotocol/clientInfo", QJsonObject{{"name", "t"}, {"version", "1"}}},
                    {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}};
    return request(method, params);
}
QJsonObject modernCall(McpServer &server, const QString &name, QJsonObject args = {}) {
    return server.handle(modern("tools/call", {{"name", name}, {"arguments", args}})).value();
}
} // namespace
class McpTests : public QObject {
    Q_OBJECT
  private slots:
    void lifecycle() {
        QTemporaryDir root;
        McpServer server(root.path());
        QVERIFY(server.startupError().isEmpty());
        QCOMPARE(server.handle(request("tools/list"))->value("error").toObject()["code"].toInt(), -32002);
        initialize(server);
        QCOMPARE(result(server.handle(request("tools/list")).value())["tools"].toArray(),
                 McpServer::toolDefinitions());
        QVERIFY(server.handle(request("ping"))->contains("result"));
        QCOMPARE(server.handle(request("unknown"))->value("error").toObject()["code"].toInt(), -32601);
        QVERIFY(!server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}}));
        QCOMPARE(server.handle({})->value("error").toObject()["code"].toInt(), -32600);
    }
    void rejectsInvalidInputs() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        for (const auto &args :
             {QJsonObject{{"width", 0}, {"height", 32}}, QJsonObject{{"width", 1.5}, {"height", 32}},
              QJsonObject{{"width", 32}, {"height", 32}, {"surprise", true}},
              QJsonObject{{"width", "32"}, {"height", 32}}})
            QCOMPARE(call(server, "create_document", args)["error"].toObject()["code"].toInt(), -32602);
        QVERIFY(failed(call(server, "create_document", {{"width", 30000}, {"height", 30000}})));
        QVERIFY(failed(call(server, "inspect_document", {{"document_id", "missing"}})));
        McpServer absent(root.filePath("missing"));
        QVERIFY(!absent.startupError().isEmpty());
        McpServer empty("");
        QVERIFY(!empty.startupError().isEmpty());
    }
    void editsUndoAndNativeRoundTrip() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto id = create(server);
        QVERIFY(!id.isEmpty());
        auto added = call(server, "create_text_layer",
                          {{"document_id", id}, {"text", "Serika"}, {"font_size", 20}, {"x", 3}, {"y", 4}});
        QVERIFY(!failed(added));
        QCOMPARE(data(added)["layers"].toArray().size(), 2);
        const auto layer = data(added)["active_layer_id"].toString();
        QVERIFY(
            !failed(call(server, "set_layer_properties",
                         {{"document_id", id}, {"layer_id", layer}, {"opacity", 0.5}, {"name", "Title"}})));
        auto undone = call(server, "undo", {{"document_id", id}});
        QCOMPARE(data(undone)["layers"].toArray().last().toObject()["opacity"].toDouble(), 1.0);
        auto redone = call(server, "redo", {{"document_id", id}});
        QCOMPARE(data(redone)["layers"].toArray().last().toObject()["name"].toString(), QString("Title"));
        QVERIFY(failed(call(server, "close_document", {{"document_id", id}})));
        QVERIFY(!failed(call(server, "save_document", {{"document_id", id}, {"path", "master.spe"}})));
        QString error;
        std::unique_ptr<Document> reopened(FormatIO::open(root.filePath("master.spe"), &error));
        QVERIFY2(reopened, error.toUtf8());
        QCOMPARE(reopened->state.layers.last().text, QString("Serika"));
        QVERIFY(!failed(call(server, "close_document", {{"document_id", id}})));
        QVERIFY(!failed(call(server, "open_document", {{"path", "master.spe"}})));
    }
    void confinementAndOverwrite() {
        QTemporaryDir root, outside;
        McpServer server(root.path());
        initialize(server);
        const auto id = create(server);
        for (const auto &path :
             QStringList{"../escaped.spe", outside.filePath("escaped.spe"), "missing/output.spe"})
            QVERIFY(failed(call(server, "save_document", {{"document_id", id}, {"path", path}})));
        QVERIFY(!QFileInfo::exists(outside.filePath("escaped.spe")));
        QVERIFY(!failed(call(server, "save_document", {{"document_id", id}, {"path", "test.spe"}})));
        QVERIFY(failed(call(server, "save_document", {{"document_id", id}, {"path", "test.spe"}})));
        QVERIFY(!failed(
            call(server, "save_document", {{"document_id", id}, {"path", "test.spe"}, {"overwrite", true}})));
        QVERIFY(failed(call(server, "open_document", {{"path", outside.filePath("missing.png")}})));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(outside.path(), root.filePath("escape")));
        QVERIFY(failed(call(server, "save_document", {{"document_id", id}, {"path", "escape/escaped.spe"}})));
#endif
    }
    void actionsAreAtomicAndBounded() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto id = create(server);
        const QJsonObject newLayer{{"command", "New Layer"}};
        auto response =
            call(server, "run_actions",
                 {{"document_id", id}, {"steps", QJsonArray{newLayer, QJsonObject{{"command", "Unknown"}}}}});
        QVERIFY(failed(response));
        QCOMPARE(data(call(server, "inspect_document", {{"document_id", id}}))["layers"].toArray().size(), 1);
        QVERIFY(!data(call(server, "inspect_document", {{"document_id", id}}))["can_undo"].toBool());
        for (const auto &command :
             QStringList{"resize", "image size...", "resize canvas", "canvas size..."}) {
            response = call(server, "run_actions",
                            {{"document_id", id},
                             {"steps", QJsonArray{QJsonObject{
                                           {"type", command},
                                           {"params", QJsonObject{{"width", 30000}, {"height", 30000}}}}}}});
            QVERIFY(failed(response));
        }
        response =
            call(server, "run_actions", {{"document_id", id}, {"steps", QJsonArray{newLayer, newLayer}}});
        QVERIFY(!failed(response));
        QCOMPARE(data(response)["layers"].toArray().size(), 3);
        QCOMPARE(data(call(server, "undo", {{"document_id", id}}))["layers"].toArray().size(), 1);
    }
    void previewIsPngAndExportKeepsDirty() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto id = create(server);
        call(server, "add_layer", {{"document_id", id}, {"kind", "solid-fill"}, {"color", "#8b5cf6"}});
        const auto content =
            result(call(server, "preview_document", {{"document_id", id}, {"max_dimension", 64}}))["content"]
                .toArray()
                .first()
                .toObject();
        QCOMPARE(content["mimeType"].toString(), QString("image/png"));
        QImage image;
        QVERIFY(image.loadFromData(QByteArray::fromBase64(content["data"].toString().toLatin1()), "PNG"));
        QCOMPARE(image.size(), QSize(64, 48));
        QCOMPARE(image.pixelColor(10, 10), QColor("#8b5cf6"));
        QVERIFY(!failed(call(server, "save_document", {{"document_id", id}, {"path", "preview.png"}})));
        QVERIFY(data(call(server, "inspect_document", {{"document_id", id}}))["modified"].toBool());
    }
    void evolvingResizeAndTransformLimitsRollBack() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto id = create(server);
        auto resize = call(
            server, "run_actions",
            {{"document_id", id},
             {"steps", QJsonArray{QJsonObject{{"command", "resize"},
                                              {"parameters", QJsonObject{{"width", 16000}, {"height", 1}}}},
                                  QJsonObject{{"type", "canvas size..."},
                                              {"params", QJsonObject{{"height", 2000}}}}}}});
        QVERIFY(failed(resize));
        const auto after = data(call(server, "inspect_document", {{"document_id", id}}));
        QCOMPARE(after["width"].toInt(), 32);
        QCOMPARE(after["height"].toInt(), 24);
        QVERIFY(!after["can_undo"].toBool());
        const auto large =
            data(call(server, "create_document",
                      {{"width", 1000}, {"height", 1000}, {"background", "white"}}))["document_id"]
                .toString();
        for (const auto &parameters :
             {QJsonObject{{"scale", 500}}, QJsonObject{{"matrix", QJsonArray{5, 0, 0, 0, 5, 0, 0, 0, 1}}}}) {
            const auto response = call(
                server, "run_actions",
                {{"document_id", large},
                 {"steps", QJsonArray{QJsonObject{{"command", "transform"}, {"parameters", parameters}}}}});
            QVERIFY(failed(response));
            QVERIFY(data(response)["error"].toString().contains("MCP"));
            QVERIFY(!data(call(server, "inspect_document", {{"document_id", large}}))["can_undo"].toBool());
        }
    }
    void svgCannotReadExternalImages() {
        QTemporaryDir root, outside;
        McpServer server(root.path());
        initialize(server);
        const auto external = outside.filePath("external.png");
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(external));
        const auto svg =
            QString("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
                    "width=\"8\" height=\"8\"><image width=\"8\" height=\"8\" xlink:href=\"%1\"/></svg>")
                .arg(external)
                .toUtf8();
        for (const auto &name : QStringList{"external.svg", "misleading.png", "misleading.dat"}) {
            QFile file(root.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(svg), svg.size());
            file.close();
            QVERIFY(failed(call(server, "open_document", {{"path", name}})));
        }
    }
    void linkedDocumentsUseCachedPixels() {
        QTemporaryDir root, outside;
        McpServer server(root.path());
        initialize(server);
        const auto external = outside.filePath("outside.png");
        QImage source(8, 8, QImage::Format_RGB32);
        source.fill(Qt::red);
        QVERIFY(source.save(external));
        std::unique_ptr<Document> document(Document::create({8, 8}, Qt::blue));
        document->state.layers[0].kind = LayerKind::SmartObject;
        document->state.layers[0].linkedPath = external;
        QString error;
        QVERIFY2(FormatIO::saveNative(document.get(), root.filePath("linked.spe"), &error),
                 qPrintable(error));
        const auto opened = call(server, "open_document", {{"path", "linked.spe"}});
        QVERIFY(!failed(opened));
        const auto id = data(opened)["document_id"].toString();
        const auto content =
            result(call(server, "preview_document", {{"document_id", id}, {"max_dimension", 32}}))["content"]
                .toArray()
                .first()
                .toObject();
        QImage preview;
        QVERIFY(preview.loadFromData(QByteArray::fromBase64(content["data"].toString().toLatin1())));
        QCOMPARE(preview.pixelColor(4, 4), QColor(Qt::blue));
    }
    void statelessModernProtocol() {
        QTemporaryDir root;
        McpServer server(root.path());
        // No initialize: discovery and tool calls work from the first request.
        const auto discovered = result(server.handle(modern("server/discover")).value());
        QCOMPARE(discovered["resultType"].toString(), QString("complete"));
        QCOMPARE(discovered["supportedVersions"].toArray().first().toString(), QString("2026-07-28"));
        QVERIFY(discovered["supportedVersions"].toArray().contains("2025-11-25"));
        QVERIFY(discovered["capabilities"].toObject().contains("tools"));
        QCOMPARE(discovered["_meta"]
                     .toObject()["io.modelcontextprotocol/serverInfo"]
                     .toObject()["name"]
                     .toString(),
                 QString("serika-photoedit"));
        const auto listed = result(server.handle(modern("tools/list")).value());
        QCOMPARE(listed["tools"].toArray(), McpServer::toolDefinitions());
        QVERIFY(listed["ttlMs"].toInt() > 0);
        QCOMPARE(listed["cacheScope"].toString(), QString("private"));
        const auto created = modernCall(server, "create_document", {{"width", 16}, {"height", 16}});
        QCOMPARE(result(created)["resultType"].toString(), QString("complete"));
        QVERIFY(!data(created)["document_id"].toString().isEmpty());
        // Unknown versions get the 2026-07-28 error listing what is supported.
        const auto rejected =
            server.handle(modern("tools/list", {}, "1900-01-01")).value()["error"].toObject();
        QCOMPARE(rejected["code"].toInt(), -32022);
        QCOMPARE(rejected["data"].toObject()["requested"].toString(), QString("1900-01-01"));
        QVERIFY(rejected["data"].toObject()["supported"].toArray().contains("2026-07-28"));
        // ping and initialize belong to the legacy handshake era.
        QCOMPARE(server.handle(modern("ping")).value()["error"].toObject()["code"].toInt(), -32601);
    }
    void actionCatalogExamplesRun() {
        // Every documented example must execute on a fresh document, so the catalog cannot drift.
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto catalog = data(call(server, "list_actions"));
        QVERIFY(catalog["filters"].toArray().contains("Gaussian Blur"));
        QVERIFY(catalog["adjustments"].toArray().contains("Levels"));
        for (const auto &value : catalog["actions"].toArray()) {
            const auto entry = value.toObject();
            const auto id =
                data(call(server, "create_document",
                          {{"width", 320}, {"height", 240}, {"background", "#ff336699"}}))["document_id"]
                    .toString();
            QJsonArray steps;
            if (entry["command"] == "merge down" || entry["command"] == "clear")
                steps.append(QJsonObject{{"command", "new layer"}});
            steps.append(entry["example"]);
            const auto ran = call(server, "run_actions", {{"document_id", id}, {"steps", steps}});
            QVERIFY2(!failed(ran), qPrintable(entry["command"].toString() + ": " +
                                              QJsonDocument(result(ran)).toJson(QJsonDocument::Compact)));
            call(server, "close_document", {{"document_id", id}, {"discard", true}});
        }
    }
    void layerToolsAndBackgroundRemoval() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto id = data(call(server, "create_document",
                                  {{"width", 120}, {"height", 90}, {"background", "#ffffff"}}))["document_id"]
                            .toString();
        // A dark disc on white: the fast engine must find it as the subject.
        QJsonArray steps{
            QJsonObject{{"command", "new layer"}, {"parameters", QJsonObject{{"name", "Disc"}}}},
            QJsonObject{{"command", "ellipse"},
                        {"parameters", QJsonObject{{"x", 35}, {"y", 20}, {"width", 50}, {"height", 50}}}},
            QJsonObject{{"command", "fill"}, {"parameters", QJsonObject{{"color", "#202040"}}}},
            QJsonObject{{"command", "deselect"}}};
        QVERIFY(!failed(call(server, "run_actions", {{"document_id", id}, {"steps", steps}})));
        auto described = data(call(server, "inspect_document", {{"document_id", id}}));
        const auto background = described["layers"].toArray().first().toObject()["id"].toString();
        const auto disc = described["active_layer_id"].toString();
        const auto selected = data(call(server, "select_subject", {{"document_id", id}, {"engine", "fast"}}));
        QVERIFY(selected["has_selection"].toBool());
        const auto bounds = selected["selection_bounds"].toArray();
        QVERIFY(bounds[0].toInt() >= 30 && bounds[0].toInt() <= 40 && bounds[2].toInt() >= 45);
        // Remove Background segments the layer's own pixels, so the disc layer gets the mask.
        const auto removed =
            call(server, "remove_background", {{"document_id", id}, {"layer_id", disc}, {"engine", "fast"}});
        QVERIFY(!failed(removed));
        QVERIFY(data(removed)["layers"].toArray().last().toObject()["has_mask"].toBool());
        QVERIFY(!data(removed)["layers"].toArray().first().toObject()["has_mask"].toBool());
        QCOMPARE(data(removed)["engine"].toString(), QString("fast"));
        // The user's selection survives the mask operation.
        QVERIFY(data(removed)["has_selection"].toBool());
        // Duplicate, group, restack and delete through stable IDs.
        const auto duplicated =
            data(call(server, "duplicate_layers", {{"document_id", id}, {"layer_ids", QJsonArray{disc}}}));
        const auto copy = duplicated["result_layer_ids"].toArray().first().toString();
        QVERIFY(!copy.isEmpty() && copy != disc);
        const auto grouped =
            data(call(server, "group_layers",
                      {{"document_id", id}, {"layer_ids", QJsonArray{disc, copy}}, {"name", "Discs"}}));
        QCOMPARE(grouped["layers"].toArray().size(), 4);
        const auto moved =
            data(call(server, "move_layers",
                      {{"document_id", id}, {"layer_ids", QJsonArray{background}}, {"position", "top"}}));
        QCOMPARE(moved["layers"].toArray().last().toObject()["id"].toString(), background);
        QVERIFY(failed(call(server, "move_layers",
                            {{"document_id", id},
                             {"layer_ids", QJsonArray{disc}},
                             {"position", "inside"},
                             {"target_layer_id", background}})));
        const auto deleted =
            data(call(server, "delete_layers", {{"document_id", id}, {"layer_ids", QJsonArray{copy}}}));
        QCOMPARE(deleted["layers"].toArray().size(), 3);
        QVERIFY(data(call(server, "history", {{"document_id", id}}))["steps"].toArray().size() >= 5);
        // Layer previews are PNG image content.
        const auto preview = result(call(server, "preview_layer", {{"document_id", id}, {"layer_id", disc}}));
        QCOMPARE(preview["content"].toArray().first().toObject()["mimeType"].toString(),
                 QString("image/png"));
        QVERIFY(data(call(server, "ai_status")).contains("ai_ready"));
    }
    void placeImageAndTextEdits() {
        QTemporaryDir root;
        QImage logo(40, 30, QImage::Format_ARGB32);
        logo.fill(QColor("#ff8800"));
        QVERIFY(logo.save(root.filePath("logo.png")));
        McpServer server(root.path());
        initialize(server);
        const auto id = create(server);
        const auto placed =
            data(call(server, "place_image",
                      {{"document_id", id}, {"path", "logo.png"}, {"x", 5}, {"y", 6}, {"width", 20}}));
        const auto layer = placed["layers"].toArray().last().toObject();
        QCOMPARE(layer["name"].toString(), QString("logo"));
        QCOMPARE(layer["x"].toDouble(), 5.0);
        QVERIFY(failed(call(server, "place_image", {{"document_id", id}, {"path", "../outside.png"}})));
        // One dimension scales the other proportionally (40 x 30 placed 60 tall becomes 80 x 60).
        const auto tall =
            data(call(server, "place_image", {{"document_id", id}, {"path", "logo.png"}, {"height", 60}}));
        const auto tallId = tall["active_layer_id"].toString();
        const auto layerPreview = result(call(
            server, "preview_layer", {{"document_id", id}, {"layer_id", tallId}, {"max_dimension", 2048}}));
        QImage tallImage;
        QVERIFY(tallImage.loadFromData(QByteArray::fromBase64(
            layerPreview["content"].toArray().first().toObject()["data"].toString().toLatin1())));
        QVERIFY(tallImage.height() >= 59);
        const auto text = data(call(server, "create_text_layer", {{"document_id", id}, {"text", "Wii"}}));
        const auto textId = text["active_layer_id"].toString();
        const auto edited = data(call(server, "set_layer_properties",
                                      {{"document_id", id},
                                       {"layer_id", textId},
                                       {"text", "Mario Kart"},
                                       {"color", "#e60012"},
                                       {"x", 12}}));
        QCOMPARE(edited["layers"].toArray().last().toObject()["text"].toString(), QString("Mario Kart"));
        QVERIFY(failed(call(server, "set_layer_properties",
                            {{"document_id", id}, {"layer_id", layer["id"].toString()}, {"text", "nope"}})));
    }
    void upscalingToolsEnlargeLayersAndDocuments() {
        QTemporaryDir root;
        // A hard-edged checkerboard: what blurs most when enlarged.
        QImage art(40, 30, QImage::Format_ARGB32);
        for (int y = 0; y < art.height(); ++y)
            for (int x = 0; x < art.width(); ++x)
                art.setPixelColor(x, y, ((x / 5 + y / 5) % 2) ? QColor("#1d4ed8") : QColor("#fde047"));
        QVERIFY(art.save(root.filePath("art.png")));
        McpServer server(root.path());
        initialize(server);
        const auto id =
            data(call(server, "create_document",
                      {{"width", 200}, {"height", 150}, {"background", "#ffffff"}}))["document_id"]
                .toString();
        const auto boundsOf = [&](const QString &layerId) {
            for (const auto &value :
                 data(call(server, "inspect_document", {{"document_id", id}}))["layers"].toArray())
                if (value.toObject()["id"].toString() == layerId)
                    return value.toObject()["bounds"].toArray();
            return QJsonArray();
        };
        // Enlarging with plain smoothing warns and points at resample=ai.
        const auto soft =
            data(call(server, "place_image", {{"document_id", id}, {"path", "art.png"}, {"width", 80}}));
        QVERIFY(soft["warning"].toString().contains("resample=ai"));
        // replace_layer_id re-renders that layer in place: same layer, same bounds, no new layer.
        const QString softId = soft["active_layer_id"].toString();
        const auto replaced = data(call(server, "place_image",
                                        {{"document_id", id},
                                         {"path", "art.png"},
                                         {"replace_layer_id", softId},
                                         {"resample", "preserve-details"}}));
        QCOMPARE(replaced["layers"].toArray().size(), soft["layers"].toArray().size());
        QCOMPARE(replaced["resample"].toString(), QString("preserve-details"));
        QCOMPARE(boundsOf(softId), (QJsonArray{0, 0, 80, 60}));
        const auto sharp = data(call(
            server, "place_image",
            {{"document_id", id}, {"path", "art.png"}, {"width", 80}, {"resample", "preserve-details"}}));
        QCOMPARE(sharp["resample"].toString(), QString("preserve-details"));
        QVERIFY(!sharp.contains("warning"));
        const QString layerId = sharp["active_layer_id"].toString();
        QCOMPARE(boundsOf(layerId), (QJsonArray{0, 0, 80, 60}));
        // upscale_layer grows about the centre: 80 x 60 at (0, 0) becomes 160 x 120 at (-40, -30).
        const auto grown = data(
            call(server, "upscale_layer",
                 {{"document_id", id}, {"layer_id", layerId}, {"scale", 2}, {"method", "preserve-details"}}));
        QCOMPARE(grown["size"].toArray(), (QJsonArray{160, 120}));
        QCOMPARE(boundsOf(layerId), (QJsonArray{-40, -30, 160, 120}));
        QVERIFY(!failed(call(server, "undo", {{"document_id", id}})));
        QCOMPARE(boundsOf(layerId), (QJsonArray{0, 0, 80, 60}));
        // Unknown methods are rejected by the input schema.
        QCOMPARE(
            call(server, "upscale_layer",
                 {{"document_id", id}, {"layer_id", layerId}, {"scale", 2}, {"method", "magic"}})["error"]
                .toObject()["code"]
                .toInt(),
            -32602);
        // upscale_document doubles the canvas and every layer.
        const auto doubled = data(call(server, "upscale_document",
                                       {{"document_id", id}, {"scale", 2}, {"method", "preserve-details"}}));
        QCOMPARE(doubled["size"].toArray(), (QJsonArray{400, 300}));
        QCOMPARE(boundsOf(layerId), (QJsonArray{0, 0, 160, 120}));
        const auto status = data(call(server, "ai_status", {}));
        QCOMPARE(status["upscale_models"].toArray().size(), 2);
#ifdef SERIKA_TEST_ESRGAN_MODEL
        // With ONNX Runtime next to the tests and the model fetched, the real network runs.
        qputenv("SERIKA_UPSCALE_MODEL", SERIKA_TEST_ESRGAN_MODEL);
        const auto ai = call(server, "upscale_layer",
                             {{"document_id", id}, {"layer_id", layerId}, {"scale", 1.5}, {"method", "ai"}});
        qunsetenv("SERIKA_UPSCALE_MODEL");
        if (!failed(ai)) {
            QCOMPARE(data(ai)["size"].toArray(), (QJsonArray{240, 180}));
            QCOMPARE(data(ai)["method"].toString(), QString("ai"));
        } else {
            QVERIFY2(result(ai)["content"].toArray().first().toObject()["text"].toString().contains("ONNX"),
                     "AI upscaling may only fail when the runtime is missing");
        }
#endif
    }
    void layoutToolsPlaceTextExactly() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto id =
            data(call(server, "create_document",
                      {{"width", 800}, {"height", 600}, {"background", "#ffffff"}}))["document_id"]
                .toString();
        const auto layerBounds = [&](const QString &layerId) {
            for (const auto &value :
                 data(call(server, "inspect_document", {{"document_id", id}}))["layers"].toArray())
                if (value.toObject()["id"].toString() == layerId) {
                    const auto b = value.toObject()["bounds"].toArray();
                    return QRectF(b[0].toDouble(), b[1].toDouble(), b[2].toDouble(), b[3].toDouble());
                }
            return QRectF();
        };
        // Box text: the ink is centred in the box, and the layer is named after its text.
        const QRectF box(100, 80, 400, 120);
        const auto centred = data(call(server, "create_text_layer",
                                       {{"document_id", id},
                                        {"text", "Literature Club\nPlus!"},
                                        {"font_family", "Arial"},
                                        {"font_size", 40},
                                        {"box", QJsonArray{100, 80, 400, 120}},
                                        {"align", "center"},
                                        {"valign", "middle"}}));
        const auto textId = centred["active_layer_id"].toString();
        QCOMPARE(centred["layers"].toArray().last().toObject()["name"].toString(),
                 QString("Literature Club"));
        const QRectF ink = layerBounds(textId);
        QVERIFY(std::abs(ink.center().x() - box.center().x()) <= 1);
        QVERIFY(std::abs(ink.center().y() - box.center().y()) <= 1);
        // fit shrinks an oversized line until the ink is inside the box.
        const auto fitted = data(call(server, "create_text_layer",
                                      {{"document_id", id},
                                       {"text", "This line is far too long for its little box"},
                                       {"font_family", "Arial"},
                                       {"font_size", 120},
                                       {"box", QJsonArray{100, 300, 300, 60}},
                                       {"fit", true}}));
        const QRectF fitInk = layerBounds(fitted["active_layer_id"].toString());
        QVERIFY2(QRectF(99, 299, 302, 62).contains(fitInk), qPrintable(QString::number(fitInk.width())));
        // Restyling box text keeps it inside its box.
        call(server, "set_layer_properties",
             {{"document_id", id}, {"layer_id", fitted["active_layer_id"].toString()}, {"font_size", 200}});
        QVERIFY(QRectF(99, 299, 302, 62).contains(layerBounds(fitted["active_layer_id"].toString())));
        // align_layers centres anything by its ink, here on the canvas.
        call(server, "align_layers",
             {{"document_id", id},
              {"layer_ids", QJsonArray{textId}},
              {"horizontal", "center"},
              {"to", "canvas"}});
        QVERIFY(std::abs(layerBounds(textId).center().x() - 400) <= 1);
        // check_layout reports overlapping text and text crossing a panel edge, and passes clean layouts.
        QVERIFY(data(call(server, "check_layout", {{"document_id", id}}))["ok"].toBool());
        call(server, "run_actions",
             {{"document_id", id},
              {"steps",
               QJsonArray{
                   QJsonObject{{"command", "new layer"}, {"parameters", QJsonObject{{"name", "Panel"}}}},
                   QJsonObject{
                       {"command", "rectangle"},
                       {"parameters", QJsonObject{{"x", 0}, {"y", 0}, {"width", 400}, {"height", 600}}}},
                   QJsonObject{{"command", "fill"}, {"parameters", QJsonObject{{"color", "#ffc0e0"}}}},
                   QJsonObject{{"command", "deselect"}}}}});
        const auto report =
            data(call(server, "check_layout",
                      {{"document_id", id}, {"safe_areas", QJsonArray{QJsonArray{0, 0, 400, 600}}}}));
        QVERIFY(!report["ok"].toBool());
        const auto text = QJsonDocument(report).toJson();
        QVERIFY(text.contains("crosses the edge"));
        QVERIFY(text.contains("outside the safe areas"));
        // Region previews come back at full resolution.
        const auto region = result(call(server, "preview_document",
                                        {{"document_id", id}, {"region", QJsonArray{100, 80, 300, 100}}}));
        QImage crop;
        QVERIFY(crop.loadFromData(QByteArray::fromBase64(
            region["content"].toArray().first().toObject()["data"].toString().toLatin1())));
        QCOMPARE(crop.size(), QSize(300, 100));
        // measure_text grows with the font size.
        const auto small = data(call(server, "measure_text", {{"text", "Monika"}, {"font_size", 20}}));
        const auto large = data(call(server, "measure_text", {{"text", "Monika"}, {"font_size", 40}}));
        QVERIFY(large["width"].toDouble() > small["width"].toDouble() * 1.8);
    }
    void opticalCentringListsAndQuietChecks() {
        QTemporaryDir root;
        McpServer server(root.path());
        initialize(server);
        const auto id =
            data(call(server, "create_document",
                      {{"width", 900}, {"height", 600}, {"background", "#ffffff"}}))["document_id"]
                .toString();
        const auto bounds = [&](const QString &layerId) {
            for (const auto &value :
                 data(call(server, "inspect_document", {{"document_id", id}}))["layers"].toArray())
                if (value.toObject()["id"].toString() == layerId) {
                    const auto b = value.toObject()["bounds"].toArray();
                    return QRectF(b[0].toDouble(), b[1].toDouble(), b[2].toDouble(), b[3].toDouble());
                }
            return QRectF();
        };
        const auto label = [&](const QString &text, int x) {
            return data(call(server, "create_text_layer",
                             {{"document_id", id},
                              {"text", text},
                              {"font_family", "Arial"},
                              {"font_size", 40},
                              {"box", QJsonArray{x, 50, 260, 80}},
                              {"align", "center"},
                              {"valign", "middle"}}))["active_layer_id"]
                .toString();
        };
        // A descender must not lift a label: cap tops line up across a row of buttons.
        const QRectF player = bounds(label("1 Player", 20)), remote = bounds(label("Wii Remote", 320));
        QVERIFY2(std::abs(player.top() - remote.top()) <= 2,
                 qPrintable(QString("%1 vs %2").arg(player.top()).arg(remote.top())));
        // wrap=false keeps each list line whole and shrinks instead of breaking a line.
        const auto list = [&](bool wrap) {
            const auto listId = data(call(server, "create_text_layer",
                                          {{"document_id", id},
                                           {"text", "Violence\nBlood\nSuggestive Themes\nStrong Language"},
                                           {"font_family", "Arial"},
                                           {"font_size", 40},
                                           {"box", QJsonArray{20, 200, 220, 260}},
                                           {"fit", true},
                                           {"wrap", wrap}}))["active_layer_id"]
                                    .toString();
            const QRectF measured = bounds(listId);
            if (wrap) // only one of the two variants stays, so the layout check below is meaningful
                call(server, "delete_layers", {{"document_id", id}, {"layer_ids", QJsonArray{listId}}});
            return measured;
        };
        const QRectF wrapped = list(true), whole = list(false);
        QVERIFY(whole.width() <= 221);
        QVERIFY(whole.height() < wrapped.height());
        // A masked photo counts only where it shows, and a striped background is one panel.
        QImage photo(400, 400, QImage::Format_ARGB32);
        photo.fill(QColor("#336699"));
        QVERIFY(photo.save(root.filePath("photo.png")));
        const auto photoId = data(call(
            server, "place_image",
            {{"document_id", id}, {"path", "photo.png"}, {"x", 450}, {"y", 150}}))["active_layer_id"]
                                 .toString();
        call(server, "run_actions",
             {{"document_id", id},
              {"steps",
               QJsonArray{QJsonObject{
                   {"command", "rectangle"},
                   {"parameters", QJsonObject{{"x", 450}, {"y", 150}, {"width", 200}, {"height", 100}}}}}}});
        call(server, "add_layer_mask", {{"document_id", id}, {"layer_id", photoId}, {"from", "selection"}});
        call(server, "run_actions",
             {{"document_id", id}, {"steps", QJsonArray{QJsonObject{{"command", "deselect"}}}}});
        QCOMPARE(bounds(photoId), QRectF(450, 150, 200, 100));
        for (int band = 0; band < 3; ++band)
            call(server, "run_actions",
                 {{"document_id", id},
                  {"steps",
                   QJsonArray{
                       QJsonObject{{"command", "new layer"}},
                       QJsonObject{
                           {"command", "rectangle"},
                           {"parameters",
                            QJsonObject{{"x", 300}, {"y", 480 + band * 30}, {"width", 580}, {"height", 30}}}},
                       QJsonObject{{"command", "fill"}, {"parameters", QJsonObject{{"color", "#ffd0e8"}}}},
                       QJsonObject{{"command", "deselect"}}}}});
        call(server, "create_text_layer",
             {{"document_id", id},
              {"text", "Across the stripes"},
              {"font_family", "Arial"},
              {"font_size", 40},
              {"box", QJsonArray{320, 485, 540, 80}},
              {"valign", "middle"}});
        call(server, "create_text_layer",
             {{"document_id", id},
              {"text", "Beside the photo"},
              {"font_family", "Arial"},
              {"font_size", 30},
              {"box", QJsonArray{460, 300, 380, 60}}});
        const auto report = data(call(server, "check_layout", {{"document_id", id}}));
        QVERIFY2(report["ok"].toBool() && report["warnings"].toInt() == 0,
                 QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    }
    void liveBridgeEditsWindowDocuments() {
        // An attached session sees the window's tabs, and its edits land in those documents.
        QTemporaryDir root;
        MainWindow window;
        window.openDemo();
        McpLiveBridge bridge(&window);
        QVERIFY2(bridge.listening(), qPrintable(bridge.errorString()));
        QLocalSocket socket;
        socket.connectToServer(McpServer::liveSocketName());
        QVERIFY(socket.waitForConnected(3000));
        const auto exchange = [&](const QJsonObject &message) {
            socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
            socket.flush();
            QElapsedTimer timer;
            timer.start();
            while (!socket.canReadLine() && timer.elapsed() < 15000)
                QTest::qWait(10);
            return socket.canReadLine() ? QJsonDocument::fromJson(socket.readLine()).object() : QJsonObject{};
        };
        QVERIFY(exchange({{"serika_attach", 1}, {"workspace", root.path()}})["ok"].toBool());
        const auto listed =
            exchange(modern("tools/call", {{"name", "list_documents"}, {"arguments", QJsonObject{}}}));
        const auto documents = data(listed)["documents"].toArray();
        QCOMPARE(documents.size(), 1);
        QVERIFY(documents.first().toObject()["active"].toBool());
        const auto id = documents.first().toObject()["document_id"].toString();
        Document *document = window.currentDocument();
        const int before = document->state.layers.size();
        const auto added = exchange(
            modern("tools/call", {{"name", "create_text_layer"},
                                  {"arguments", QJsonObject{{"document_id", id}, {"text", "Live"}}}}));
        QVERIFY(!failed(added));
        QCOMPARE(document->state.layers.size(), before + 1);
        QCOMPARE(document->activeLayer()->text, QString("Live"));
        QVERIFY(document->canUndo());
        // Closing unsaved work requires an explicit discard, so the agent never triggers a Save prompt.
        QVERIFY(failed(exchange(modern(
            "tools/call", {{"name", "close_document"}, {"arguments", QJsonObject{{"document_id", id}}}}))));
        QCOMPARE(window.openDocuments().size(), 1);
    }
    void stdioTransport() {
        QTemporaryDir root;
        QProcess process;
        const auto executable = QString::fromUtf8(SERIKA_APP_PATH);
        process.start(executable, {"--mcp", "--mcp-root", root.path()});
        QVERIFY2(process.waitForStarted(), qPrintable(process.errorString()));
        const auto init =
            request("initialize", {{"protocolVersion", "2025-11-25"},
                                   {"capabilities", QJsonObject{}},
                                   {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}});
        process.write(QJsonDocument(init).toJson(QJsonDocument::Compact) + '\n');
        QVERIFY(process.waitForReadyRead(5000));
        const auto initial = process.readAllStandardOutput();
        QVERIFY(QJsonDocument::fromJson(initial).object().contains("result"));
        process.write("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
        process.write(QJsonDocument(request("tools/list")).toJson(QJsonDocument::Compact) + '\n');
        process.write("invalid json\n");
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        const auto output = (initial + process.readAllStandardOutput()).trimmed().split('\n');
        QCOMPARE(output.size(), 3);
        for (const auto &line : output) {
            const auto json = QJsonDocument::fromJson(line);
            QVERIFY(json.isObject());
            QCOMPARE(json.object()["jsonrpc"].toString(), QString("2.0"));
        }
        QCOMPARE(QJsonDocument::fromJson(output.last()).object()["error"].toObject()["code"].toInt(), -32700);
    }
};
QTEST_MAIN(McpTests)
#include "mcp_tests.moc"
