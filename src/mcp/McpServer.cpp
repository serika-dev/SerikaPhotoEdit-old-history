#include "McpServer.h"
#include "actions/ActionRunner.h"
#include "document/LayerOperations.h"
#include "document/TransformOperations.h"
#include "io/FormatIO.h"
#include "tools/AiSegmenter.h"
#include "tools/LocalAlgorithms.h"
#include <QBuffer>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <cstdio>
#include <limits>
#include <serika/Version.h>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif
namespace serika {
namespace {
constexpr qint64 MaxPixels = 16'000'000;
constexpr int MaxDocuments = 8;
// 2026-07-28 is stateless: every request names its version in _meta. Earlier revisions use the
// initialize handshake, which stays supported so today's clients keep working (dual-era server).
const QString ModernVersion = "2026-07-28";
const QStringList LegacyVersions{"2025-11-25", "2025-06-18", "2025-03-26"};
const QString MetaVersion = "io.modelcontextprotocol/protocolVersion";
const QString MetaServerInfo = "io.modelcontextprotocol/serverInfo";
constexpr int UnsupportedProtocolVersion = -32022;
QJsonObject serverInfo() {
    return {{"name", "serika-photoedit"}, {"version", SERIKA_VERSION}, {"title", "Serika PhotoEdit"}};
}
QJsonArray supportedVersions() {
    QJsonArray versions{ModernVersion};
    for (const auto &version : LegacyVersions)
        versions.append(version);
    return versions;
}
// Documents owned by one stdio session and invisible to the desktop window.
class IsolatedHost final : public McpDocumentHost {
  public:
    bool live() const override { return false; }
    QStringList documentIds() override { return m_order; }
    Document *document(const QString &id) override {
        const auto found = m_documents.find(id);
        return found == m_documents.end() ? nullptr : found->second.get();
    }
    QString adopt(std::unique_ptr<Document> document) override {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        document->historyLimit = 20;
        m_documents.emplace(id, std::move(document));
        m_order.append(id);
        return id;
    }
    bool release(const QString &id, bool discard, QString *error) override {
        auto *document = this->document(id);
        if (!document) {
            *error = "Unknown session document ID.";
            return false;
        }
        if (document->isModified() && !discard) {
            *error = "Document has unsaved edits; save it or explicitly set discard=true.";
            return false;
        }
        m_documents.erase(id);
        m_order.removeAll(id);
        return true;
    }
    int capacity() const override { return MaxDocuments; }

  private:
    std::map<QString, std::unique_ptr<Document>> m_documents;
    QStringList m_order;
};
// The AI engine can take tens of seconds on a CPU. Run it on a worker thread and keep the event
// loop alive (without user input) so the desktop window keeps painting during live sessions.
QImage segmentSubject(const QImage &composite, SubjectEngine engine, QString *error) {
    if (engine == SubjectEngine::Fast || !QCoreApplication::instance())
        return selectSubject(composite, engine, error);
    QImage result;
    QString failure;
    QEventLoop loop;
    QThread *worker = QThread::create([&] { result = selectSubject(composite, engine, &failure); });
    QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    worker->wait();
    delete worker;
    if (error)
        *error = failure;
    return result;
}
QVector<quint64> layerIdList(const QJsonObject &args, const Document *document, QString *error) {
    QVector<quint64> ids;
    for (const auto &value : args["layer_ids"].toArray()) {
        bool ok = false;
        const quint64 id = value.toString().toULongLong(&ok);
        if (!ok || document->indexForId(id) < 0) {
            *error = "Unknown layer ID: " + value.toString();
            return {};
        }
        ids.append(id);
    }
    if (ids.isEmpty())
        *error = "Provide at least one layer ID.";
    return ids;
}
QJsonObject pngContent(const QImage &source, int dimension, QString *error) {
    const auto image = source.scaled(dimension, dimension, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGBA8888);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (image.isNull() || !image.save(&buffer, "PNG")) {
        *error = "Could not render the preview.";
        return {};
    }
    return {{"type", "image"}, {"mimeType", "image/png"}, {"data", QString::fromLatin1(bytes.toBase64())}};
}
QJsonObject rpcError(QJsonValue id, int code, const QString &message) {
    return {{"jsonrpc", "2.0"},
            {"id", id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
QJsonObject textResult(const QJsonObject &value, bool error = false) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"},
                                               {"text", QString::fromUtf8(QJsonDocument(value).toJson(
                                                            QJsonDocument::Compact))}}}},
            {"structuredContent", value},
            {"isError", error}};
}
QJsonObject failure(const QString &message) { return textResult({{"error", message}}, true); }
QJsonObject field(const QString &type, const QString &description = {}) {
    QJsonObject result{{"type", type}};
    if (!description.isEmpty())
        result["description"] = description;
    return result;
}
QJsonObject integer(int minimum, int maximum) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}
QJsonObject tool(const QString &name, const QString &description, QJsonObject properties = {},
                 QJsonArray required = {}, bool readOnly = false, bool destructive = false) {
    return {{"name", name},
            {"description", description},
            {"inputSchema", QJsonObject{{"type", "object"},
                                        {"properties", properties},
                                        {"required", required},
                                        {"additionalProperties", false}}},
            {"annotations", QJsonObject{{"readOnlyHint", readOnly},
                                        {"destructiveHint", destructive},
                                        {"idempotentHint", readOnly},
                                        {"openWorldHint", false}}}};
}
QString kindName(LayerKind kind) {
    switch (kind) {
    case LayerKind::Pixel:
        return "pixel";
    case LayerKind::Group:
        return "group";
    case LayerKind::Text:
        return "text";
    case LayerKind::Shape:
        return "shape";
    case LayerKind::Adjustment:
        return "adjustment";
    case LayerKind::SolidFill:
        return "solid-fill";
    case LayerKind::GradientFill:
        return "gradient-fill";
    case LayerKind::PatternFill:
        return "pattern-fill";
    case LayerKind::SmartObject:
        return "smart-object";
    case LayerKind::Artboard:
        return "artboard";
    }
    return "unknown";
}
bool validate(const QJsonObject &arguments, const QJsonObject &schema, QString *error) {
    const auto properties = schema["properties"].toObject();
    for (const auto &required : schema["required"].toArray())
        if (!arguments.contains(required.toString())) {
            *error = "Missing argument: " + required.toString();
            return false;
        }
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!properties.contains(it.key())) {
            *error = "Unknown argument: " + it.key();
            return false;
        }
        const auto rule = properties[it.key()].toObject();
        const auto type = rule["type"].toString();
        const auto value = it.value();
        const bool correct = type == "string"    ? value.isString()
                             : type == "boolean" ? value.isBool()
                             : type == "object"  ? value.isObject()
                             : type == "array"   ? value.isArray()
                                                 : value.isDouble();
        if (!correct) {
            *error = "Wrong argument type: " + it.key();
            return false;
        }
        if (type == "integer" &&
            (!std::isfinite(value.toDouble()) || std::floor(value.toDouble()) != value.toDouble())) {
            *error = "Expected an integer: " + it.key();
            return false;
        }
        if (value.isDouble() &&
            (!std::isfinite(value.toDouble()) ||
             (rule.contains("minimum") && value.toDouble() < rule["minimum"].toDouble()) ||
             (rule.contains("maximum") && value.toDouble() > rule["maximum"].toDouble()))) {
            *error = "Argument outside its supported range: " + it.key();
            return false;
        }
        if (value.isString() && value.toString().size() > 100000) {
            *error = "Text argument is too large.";
            return false;
        }
        if (rule.contains("enum") && !rule["enum"].toArray().contains(value)) {
            *error = "Unsupported value: " + it.key();
            return false;
        }
    }
    return true;
}
bool boundedRaster(QSize size, QString *error) {
    if (size.isEmpty() || size.width() > 30000 || size.height() > 30000 ||
        qint64(size.width()) * size.height() > MaxPixels) {
        *error = "MCP raster bounds are limited to 16 million pixels and 30,000 pixels per dimension.";
        return false;
    }
    return true;
}
bool transformedSize(QSize source, const QTransform &transform, QSize *output, QString *error) {
    if (!boundedRaster(source, error))
        return false;
    for (const auto &value : transformJson(transform))
        if (!std::isfinite(value.toDouble())) {
            *error = "Transform values must be finite.";
            return false;
        }
    if (!transform.isInvertible()) {
        *error = "This transform has zero area.";
        return false;
    }
    const QPointF corners[]{{0, 0},
                            {qreal(source.width()), 0},
                            {qreal(source.width()), qreal(source.height())},
                            {0, qreal(source.height())}};
    int denominatorSign = 0;
    for (const auto &corner : corners) {
        const double denominator =
            transform.m13() * corner.x() + transform.m23() * corner.y() + transform.m33();
        if (!std::isfinite(denominator) || std::abs(denominator) < 1e-9 ||
            (denominatorSign && denominatorSign != (denominator > 0 ? 1 : -1))) {
            *error = "Perspective transform crosses an unbounded projection.";
            return false;
        }
        denominatorSign = denominator > 0 ? 1 : -1;
    }
    const QRectF bounds = transform.mapRect(QRectF(QPointF(), source));
    constexpr double coordinateLimit = std::numeric_limits<int>::max() / 4.;
    for (double coordinate : {bounds.left(), bounds.top(), bounds.right(), bounds.bottom()})
        if (!std::isfinite(coordinate) || std::abs(coordinate) > coordinateLimit) {
            *error = "Transform coordinates exceed supported raster bounds.";
            return false;
        }
    const double width = std::ceil(bounds.right()) - std::floor(bounds.left()),
                 height = std::ceil(bounds.bottom()) - std::floor(bounds.top());
    if (width < 1 || height < 1 || width > 30000 || height > 30000 || width * height > MaxPixels) {
        *error = "Transform exceeds MCP raster limits before allocation.";
        return false;
    }
    *output = QSize(int(width), int(height));
    return true;
}
bool renderedSize(const Document *document, const Layer &layer, QSize *output, QString *error) {
    QSize size = layer.kind == LayerKind::Pixel || layer.kind == LayerKind::SmartObject
                     ? layer.pixels.size
                     : document->state.size;
    if (size.isEmpty()) {
        *output = {};
        return true;
    }
    if (!boundedRaster(size, error))
        return false;
    if (layer.parameters.contains("contentTransform")) {
        const auto matrix = layer.parameters["contentTransform"].toArray();
        if (matrix.size() != 9) {
            *error = "A content transform requires nine numeric matrix values.";
            return false;
        }
        for (const auto &value : matrix)
            if (!value.isDouble()) {
                *error = "A content transform requires nine numeric matrix values.";
                return false;
            }
        if (!transformedSize(size, transformFromJson(matrix), &size, error))
            return false;
    }
    *output = size;
    return true;
}
bool validDocument(const Document *document, QString *error) {
    const auto size = document->state.size;
    if (size.isEmpty() || size.width() > 30000 || size.height() > 30000 ||
        qint64(size.width()) * size.height() > MaxPixels) {
        *error = "MCP documents are limited to 16 million pixels and 30,000 pixels per dimension.";
        return false;
    }
    if (document->state.layers.size() > 256) {
        *error = "MCP documents are limited to 256 layers.";
        return false;
    }
    quint64 total = 0;
    for (const auto &layer : document->state.layers) {
        QSize renderSize;
        if (!renderedSize(document, layer, &renderSize, error) ||
            (!layer.mask.isNull() && !boundedRaster(layer.mask.size(), error)))
            return false;
        total += layer.embeddedDocument.size() + layer.mask.sizeInBytes();
        for (const auto &tile : layer.pixels.tiles)
            total += tile.sizeInBytes();
        if (total > 512ULL * 1024 * 1024) {
            *error = "MCP document storage exceeds 512 MB.";
            return false;
        }
    }
    return true;
}
bool validActionAllocation(const Document *document, const QJsonObject &step, QString *error) {
    const QString key = step.value("command").toString(step.value("type").toString()).trimmed().toLower();
    auto parameters = step["parameters"].toObject();
    if (parameters.isEmpty())
        parameters = step["params"].toObject();
    if (key == "resize" || key == "image size" || key == "image size..." || key == "resize canvas" ||
        key == "canvas size...") {
        const double width = parameters["width"].toDouble(document->state.size.width()),
                     height = parameters["height"].toDouble(document->state.size.height());
        if (!std::isfinite(width) || !std::isfinite(height) || width != std::floor(width) ||
            height != std::floor(height) || width < 1 || height < 1 || width > 30000 || height > 30000 ||
            width * height > MaxPixels) {
            *error = "Resize exceeds MCP document limits before allocation.";
            return false;
        }
    }
    if (key == "transform" || key == "flip horizontal" || key == "flip vertical" ||
        key == "rotate 90° clockwise" || key == "rotate 90° counterclockwise" || key == "rotate 180°") {
        const auto *layer = document->activeLayer();
        if (!layer)
            return true; // ActionRunner reports the missing-layer error.
        QSize source;
        if (layer->maskTarget && !layer->mask.isNull())
            source = layer->mask.size();
        else if (!renderedSize(document, *layer, &source, error))
            return false;
        QTransform transform;
        if (parameters.contains("matrix")) {
            const auto matrix = parameters["matrix"].toArray();
            if (matrix.size() != 9) {
                *error = "Transform requires nine numeric matrix values.";
                return false;
            }
            for (const auto &value : matrix)
                if (!value.isDouble()) {
                    *error = "Transform requires nine numeric matrix values.";
                    return false;
                }
            transform = transformFromJson(matrix);
        } else {
            const double scale = parameters["scale"].toDouble(100) / 100.;
            double sx = parameters["scaleX"].toDouble(scale), sy = parameters["scaleY"].toDouble(scale),
                   rotation = parameters["rotation"].toDouble(parameters["angle"].toDouble());
            if (key == "flip horizontal")
                sx = -1;
            if (key == "flip vertical")
                sy = -1;
            if (key == "rotate 90° clockwise")
                rotation = 90;
            if (key == "rotate 90° counterclockwise")
                rotation = -90;
            if (key == "rotate 180°")
                rotation = 180;
            if (!std::isfinite(rotation) || !std::isfinite(sx) || !std::isfinite(sy) || std::abs(sx) < .001 ||
                std::abs(sy) < .001 || std::abs(sx) > 100 || std::abs(sy) > 100) {
                *error = "Invalid transform scale or angle.";
                return false;
            }
            transform.rotate(rotation);
            transform.scale(sx, sy);
            transform.shear(parameters["shearX"].toDouble(), parameters["shearY"].toDouble());
            transform *= QTransform::fromTranslate(parameters["x"].toDouble(), parameters["y"].toDouble());
        }
        QSize output;
        if (!transformedSize(source, transform, &output, error))
            return false;
    }
    return true;
}
QString editLock(const Document *document, quint64 id) {
    QSet<quint64> visited;
    while (id) {
        if (visited.contains(id))
            return "The layer hierarchy contains a cycle.";
        visited.insert(id);
        const int index = document->indexForId(id);
        if (index < 0)
            return "The layer hierarchy is invalid.";
        const auto &layer = document->state.layers[index];
        if (layer.locked)
            return "The layer or a parent group is locked.";
        id = layer.parentId;
    }
    return {};
}
} // namespace
McpServer::McpServer(const QString &workspace, std::shared_ptr<McpDocumentHost> host)
    : m_host(host ? std::move(host) : std::make_shared<IsolatedHost>()) {
    const QFileInfo info(workspace);
    if (workspace.trimmed().isEmpty() || !info.isDir() || info.canonicalFilePath().isEmpty())
        m_startupError = "--mcp-root must name an existing workspace directory.";
    else
        m_root = info.canonicalFilePath();
}
QJsonArray McpServer::toolDefinitions(bool live) {
    const auto doc = field("string", "Session document ID returned by create_document or open_document.");
    const auto path = field("string", "Path inside the configured MCP workspace, relative or absolute.");
    const auto layer = field("string", "Stable layer ID from inspect_document.");
    const QJsonObject layers{{"type", "array"},
                             {"items", QJsonObject{{"type", "string"}}},
                             {"minItems", 1},
                             {"description", "Stable layer IDs from inspect_document."}};
    const QJsonObject engine{{"type", "string"},
                             {"enum", QJsonArray{"fast", "ai"}},
                             {"description", "fast: low-resource colour model. ai: local BiRefNet model "
                                             "(see ai_status). Defaults to the user's preference."}};
    const QJsonObject number{{"type", "number"}, {"minimum", -30000}, {"maximum", 30000}};
    QJsonArray tools{
        tool("server_status", "Report capabilities, limits and the authorized workspace.", {}, {}, true),
        tool("list_documents", "List documents owned by this MCP session.", {}, {}, true),
        tool("create_document", "Create a native RGB document. No file is written until save_document.",
             {{"width", integer(1, 30000)},
              {"height", integer(1, 30000)},
              {"bit_depth", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{8, 16, 32}}}},
              {"background", field("string", "Qt/CSS hex color, including #AARRGGBB; default transparent.")},
              {"title", field("string")}},
             {"width", "height"}),
        tool("open_document",
             "Open an image/native document inside the workspace into a new isolated session document.",
             {{"path", path}}, {"path"}),
        tool("inspect_document",
             "Read dimensions, layer IDs/properties, precision, selection and undo state.",
             {{"document_id", doc}}, {"document_id"}, true),
        tool("preview_document", "Render a bounded PNG preview as MCP image content without writing a file.",
             {{"document_id", doc}, {"max_dimension", integer(32, 2048)}}, {"document_id"}, true),
        tool("save_document",
             "Save SPE/SPEB/PSD/PSB or export a supported raster format inside the workspace. Existing files "
             "require overwrite=true.",
             {{"document_id", doc}, {"path", path}, {"overwrite", field("boolean")}}, {"document_id", "path"},
             false, true),
        tool("close_document", "Release a session document. Unsaved edits require discard=true.",
             {{"document_id", doc}, {"discard", field("boolean")}}, {"document_id"}, false, true),
        tool("select_layer", "Select a stable layer ID for subsequent actions.",
             {{"document_id", doc}, {"layer_id", layer}}, {"document_id", "layer_id"}),
        tool("add_layer",
             "Add an editable pixel, group, solid-fill, gradient-fill or pattern-fill layer. Parameters use "
             "native fill settings.",
             {{"document_id", doc},
              {"name", field("string")},
              {"kind", QJsonObject{{"type", "string"},
                                   {"enum", QJsonArray{"pixel", "group", "solid-fill", "gradient-fill",
                                                       "pattern-fill"}}}},
              {"color", field("string")},
              {"parameters", field("object")}},
             {"document_id", "kind"}),
        tool("create_text_layer",
             "Create editable text with native font, tracking, color and position. Rich typography "
             "parameters are retained by the native model.",
             {{"document_id", doc},
              {"text", field("string")},
              {"font_family", field("string")},
              {"font_size", integer(1, 2000)},
              {"bold", field("boolean")},
              {"italic", field("boolean")},
              {"color", field("string")},
              {"x", number},
              {"y", number},
              {"tracking", QJsonObject{{"type", "number"}, {"minimum", -100}, {"maximum", 1000}}},
              {"parameters", field("object")}},
             {"document_id", "text"}),
        tool("set_layer_properties",
             "Set a layer's name, appearance, position, text or colour in one undo step. Honors locks.",
             {{"document_id", doc},
              {"layer_id", layer},
              {"name", field("string")},
              {"opacity", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"visible", field("boolean")},
              {"blend_mode",
               QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(blendModeNames())}}},
              {"fill", QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"locked", field("boolean")},
              {"x", number},
              {"y", number},
              {"text", field("string", "New content for a text layer.")},
              {"color", field("string", "Text or fill colour.")}},
             {"document_id", "layer_id"}),
        tool("run_actions",
             "Run 1–100 native action steps atomically. Supports adjustments, filters, resize, fill, "
             "selection, transforms and other documented ActionRunner commands. File operations and scripts "
             "are not accepted.",
             {{"document_id", doc},
              {"steps", QJsonObject{{"type", "array"},
                                    {"items", QJsonObject{{"type", "object"}}},
                                    {"minItems", 1},
                                    {"maxItems", 100}}}},
             {"document_id", "steps"}),
        tool("undo", "Undo one edit in the session document.", {{"document_id", doc}}, {"document_id"}),
        tool("redo", "Redo one edit in the session document.", {{"document_id", doc}}, {"document_id"}),
        tool("history", "List the document's undo history and the current position.", {{"document_id", doc}},
             {"document_id"}, true),
        tool("list_actions",
             "Describe every run_actions command with parameters and a working example, plus the "
             "adjustment, filter and blend-mode names.",
             {}, {}, true),
        tool("ai_status", "Report the subject engines (fast, ai) and whether the local AI model is ready.",
             {}, {}, true),
        tool("preview_layer", "Render one layer alone as a bounded PNG, without writing a file.",
             {{"document_id", doc}, {"layer_id", layer}, {"max_dimension", integer(32, 2048)}},
             {"document_id", "layer_id"}, true),
        tool("delete_layers", "Delete layers (and the contents of deleted groups) in one undo step.",
             {{"document_id", doc}, {"layer_ids", layers}}, {"document_id", "layer_ids"}, false, true),
        tool("duplicate_layers", "Duplicate layers above themselves; returns the new selection.",
             {{"document_id", doc}, {"layer_ids", layers}}, {"document_id", "layer_ids"}),
        tool("group_layers", "Put layers into a new group.",
             {{"document_id", doc}, {"layer_ids", layers}, {"name", field("string")}},
             {"document_id", "layer_ids"}),
        tool("ungroup_layers", "Dissolve groups, keeping their children in place.",
             {{"document_id", doc}, {"layer_ids", layers}}, {"document_id", "layer_ids"}),
        tool("move_layers",
             "Restack layers: above/below a target layer, inside a target group (at its top), or to the "
             "top/bottom of the whole document.",
             {{"document_id", doc},
              {"layer_ids", layers},
              {"position", QJsonObject{{"type", "string"},
                                       {"enum", QJsonArray{"above", "below", "inside", "top", "bottom"}}}},
              {"target_layer_id", layer}},
             {"document_id", "layer_ids", "position"}),
        tool("select_subject", "Select the main subject of the visible image as the document selection.",
             {{"document_id", doc}, {"engine", engine}}, {"document_id"}),
        tool("remove_background",
             "Hide a layer's background with a layer mask (non-destructive; the pixels are kept).",
             {{"document_id", doc}, {"layer_id", layer}, {"engine", engine}}, {"document_id"}),
        tool("add_layer_mask", "Add a layer mask: reveal_all, hide_all, or from the current selection.",
             {{"document_id", doc},
              {"layer_id", layer},
              {"from",
               QJsonObject{{"type", "string"}, {"enum", QJsonArray{"reveal_all", "hide_all", "selection"}}}}},
             {"document_id", "layer_id"}),
        tool("delete_layer_mask", "Delete a layer mask, or apply it to the pixels first with apply=true.",
             {{"document_id", doc}, {"layer_id", layer}, {"apply", field("boolean")}},
             {"document_id", "layer_id"}, false, true),
        tool("place_image",
             "Add an image file from the workspace as a new pixel layer, optionally positioned and scaled.",
             {{"document_id", doc},
              {"path", path},
              {"name", field("string")},
              {"x", number},
              {"y", number},
              {"width", integer(1, 30000)},
              {"height", integer(1, 30000)}},
             {"document_id", "path"})};
    if (live)
        tools.append(tool("focus_document", "Bring a document's tab to the front of the Serika window.",
                          {{"document_id", doc}}, {"document_id"}));
    return tools;
}
QJsonArray McpServer::actionCatalog() {
    const auto entry = [](const QString &command, const QString &description, const QJsonObject &parameters,
                          const QJsonObject &example) {
        return QJsonObject{{"command", command},
                           {"description", description},
                           {"parameters", parameters},
                           {"example", example}};
    };
    const auto step = [](const QString &command, const QJsonObject &parameters = {},
                         const QString &name = {}) {
        QJsonObject result{{"command", command}};
        if (!name.isEmpty())
            result["name"] = name;
        if (!parameters.isEmpty())
            result["parameters"] = parameters;
        return result;
    };
    return {
        entry("adjustment", "Apply an adjustment to the active layer. name is one of the adjustments list.",
              {{"name", "adjustment name"}, {"parameters", "adjustment settings, e.g. brightness, contrast"}},
              step("adjustment", {{"brightness", 12}, {"contrast", 8}}, "Brightness/Contrast")),
        entry("filter", "Apply a filter to the active layer. name is one of the filters list.",
              {{"name", "filter name"}, {"radius", "pixels, 0–100"}, {"amount", "percent"}},
              step("filter", {{"radius", 2}}, "Gaussian Blur")),
        entry("fill", "Fill the selection (or the whole active layer) with a colour.",
              {{"color", "#RRGGBB or #AARRGGBB"}, {"opacity", "0–1"}}, step("fill", {{"color", "#2a1f4d"}})),
        entry("clear", "Clear the selected pixels of the active layer to transparency.", {}, step("clear")),
        entry("new layer", "Add an empty pixel layer above the active layer.", {{"name", "layer name"}},
              step("new layer", {{"name", "Paint"}})),
        entry("new group", "Add an empty group.", {{"name", "group name"}},
              step("new group", {{"name", "Group"}})),
        entry("duplicate layer", "Duplicate the active layer.", {}, step("duplicate layer")),
        entry("delete layer", "Delete the active layer.", {}, step("delete layer")),
        entry("add layer mask", "Add a mask to the active layer from the selection, or revealing all.", {},
              step("add layer mask")),
        entry("merge down", "Merge the active layer into the layer below.", {}, step("merge down")),
        entry("flatten image", "Flatten all layers into one.", {}, step("flatten image")),
        entry("set properties", "Change the active layer's properties.",
              {{"name", "text"},
               {"opacity", "0–1"},
               {"fill", "0–1"},
               {"blendMode", "blend mode name"},
               {"visible", "boolean"},
               {"locked", "boolean"}},
              step("set properties", {{"opacity", 0.8}, {"blendMode", "Multiply"}})),
        entry("select layer", "Make a layer active by stable id (or stack index).",
              {{"id", "layer id string"}, {"index", "0 = bottom"}}, step("select layer", {{"index", 0}})),
        entry("select all", "Select the whole canvas.", {}, step("select all")),
        entry("deselect", "Clear the selection.", {}, step("deselect")),
        entry("inverse", "Invert the selection.", {}, step("inverse")),
        entry("rectangle", "Make a rectangular selection.",
              {{"x", "px"},
               {"y", "px"},
               {"width", "px"},
               {"height", "px"},
               {"operation", "replace, add, subtract or intersect"}},
              step("rectangle", {{"x", 10}, {"y", 10}, {"width", 120}, {"height", 80}})),
        entry("ellipse", "Make an elliptical selection.",
              {{"x", "px"},
               {"y", "px"},
               {"width", "px"},
               {"height", "px"},
               {"operation", "replace, add, subtract or intersect"}},
              step("ellipse", {{"x", 20}, {"y", 20}, {"width", 100}, {"height", 100}})),
        entry("flip horizontal", "Flip the active layer horizontally.", {}, step("flip horizontal")),
        entry("flip vertical", "Flip the active layer vertically.", {}, step("flip vertical")),
        entry("rotate 180°", "Rotate the active layer by 180 degrees.", {}, step("rotate 180°")),
        entry("transform", "Transform the active layer by a 3×3 matrix in row order (QTransform m11…m33).",
              {{"matrix", "[m11, m12, m13, m21, m22, m23, m31, m32, m33]"}},
              step("transform", {{"matrix", QJsonArray{1, 0, 0, 0, 1, 0, 12, 8, 1}}})),
        entry("resize", "Resample the whole image.",
              {{"width", "px"}, {"height", "px"}, {"resample", "smooth or nearest"}},
              step("resize", {{"width", 300}, {"height", 200}})),
        entry("resize canvas", "Change the canvas size without scaling.",
              {{"width", "px"}, {"height", "px"}, {"x", "content offset"}, {"y", "content offset"}},
              step("resize canvas", {{"width", 320}, {"height", 220}})),
        entry("crop", "Crop the canvas to a rectangle.",
              {{"x", "px"}, {"y", "px"}, {"width", "px"}, {"height", "px"}},
              step("crop", {{"x", 0}, {"y", 0}, {"width", 200}, {"height", 150}}))};
}
QString McpServer::checkedPath(const QString &path, bool write, QString *error) const {
    if (path.trimmed().isEmpty() || path.contains(QChar(0))) {
        *error = "A valid workspace path is required.";
        return {};
    }
    const auto absolute = QDir::cleanPath(QDir(m_root).absoluteFilePath(path));
    const QFileInfo info(absolute);
    const auto resolved = info.exists()
                              ? info.canonicalFilePath()
                              : QFileInfo(info.absolutePath()).canonicalFilePath() + "/" + info.fileName();
    const auto relative = QDir(m_root).relativeFilePath(resolved);
    if (resolved.isEmpty() || relative == ".." || relative.startsWith("../") ||
        QDir::isAbsolutePath(relative)) {
        *error = "The path is outside the configured MCP workspace.";
        return {};
    }
    if (write) {
        if (!QFileInfo(info.absolutePath()).isDir() || info.isDir() || (info.isSymLink() && !info.exists())) {
            *error = "Output requires an existing workspace folder and a regular filename.";
            return {};
        }
    } else if (!info.isFile()) {
        *error = "The input file does not exist.";
        return {};
    }
    return info.exists() ? resolved : absolute;
}
QJsonObject McpServer::describe(const QString &id, const Document *document) const {
    QJsonArray layers;
    for (const auto &layer : document->state.layers) {
        QJsonObject item{{"id", QString::number(layer.id)},
                         {"parent_id", QString::number(layer.parentId)},
                         {"name", layer.name},
                         {"kind", kindName(layer.kind)},
                         {"visible", layer.visible},
                         {"locked", layer.locked},
                         {"opacity", layer.opacity},
                         {"blend_mode", layer.blendMode},
                         {"x", layer.offset.x()},
                         {"y", layer.offset.y()},
                         {"has_mask", !layer.mask.isNull()},
                         {"has_vector_mask", !layer.vectorMask.isEmpty()}};
        item["parameters"] = layer.parameters;
        if (layer.kind == LayerKind::Text)
            item["text"] = layer.text;
        layers.append(item);
    }
    return {{"document_id", id},
            {"title", document->title},
            {"width", document->state.size.width()},
            {"height", document->state.size.height()},
            {"bit_depth", document->state.bitDepth},
            {"color_mode", document->state.colorMode},
            {"modified", document->isModified()},
            {"can_undo", document->canUndo()},
            {"can_redo", document->canRedo()},
            {"active_layer_id",
             document->activeLayer() ? QString::number(document->activeLayer()->id) : QString()},
            {"has_selection", document->hasSelection()},
            {"selection_bounds",
             document->hasSelection()
                 ? QJsonValue(QJsonArray{document->selectionBounds().x(), document->selectionBounds().y(),
                                         document->selectionBounds().width(),
                                         document->selectionBounds().height()})
                 : QJsonValue()},
            {"layers", layers}};
}
bool McpServer::readImageInput(const QString &path, QString *error) const {
    if (QFileInfo(path).size() > 512LL * 1024 * 1024) {
        *error = "MCP input files must be at most 512 MB.";
        return false;
    }
    // Qt SVG can load external raster references while querying size. Reject it before
    // invoking that decoder, including content-sniffed files with a misleading suffix.
    const auto suffix = QFileInfo(path).suffix().toLower();
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        *error = "Could not read the input file.";
        return false;
    }
    const auto signature = probe.read(2);
    probe.close();
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const auto format = reader.format().toLower();
    if (suffix == "svg" || suffix == "svgz" || signature == QByteArray::fromHex("1f8b") || format == "svg" ||
        format == "svgz") {
        *error =
            "SVG and SVGZ input is disabled in MCP because it can reference files outside the workspace.";
        return false;
    }
    const auto rasterSize = reader.size();
    if (rasterSize.isValid() && (rasterSize.width() > 30000 || rasterSize.height() > 30000 ||
                                 qint64(rasterSize.width()) * rasterSize.height() > MaxPixels)) {
        *error = "Input image exceeds MCP document limits.";
        return false;
    }
    return true;
}
QJsonObject McpServer::call(const QString &name, const QJsonObject &args) {
    if (name == "server_status")
        return textResult(
            {{"name", "Serika PhotoEdit"},
             {"version", SERIKA_VERSION},
             {"transport", m_host->live() ? "stdio, attached to the desktop app" : "stdio"},
             {"workspace", m_root},
             {"max_documents", m_host->capacity()},
             {"max_pixels_per_document", MaxPixels},
             {"max_layers", 256},
             {"session", m_host->live() ? "Live: documents are the tabs open in the Serika window; edits "
                                          "appear immediately and can be undone there."
                                        : "Isolated in-memory documents; GUI windows are not controlled."},
             {"protocol_versions", supportedVersions()},
             {"adjustments", QJsonArray::fromStringList(adjustmentNames())}});
    if (name == "list_documents") {
        QJsonArray documents;
        const auto active = m_host->activeDocumentId();
        for (const auto &id : m_host->documentIds()) {
            auto description = describe(id, m_host->document(id));
            description["active"] = id == active;
            documents.append(description);
        }
        return textResult({{"documents", documents}});
    }
    if (name == "list_actions")
        return textResult({{"actions", actionCatalog()},
                           {"adjustments", QJsonArray::fromStringList(adjustmentNames())},
                           {"filters", QJsonArray::fromStringList(ActionRunner::filterNames())},
                           {"blend_modes", QJsonArray::fromStringList(blendModeNames())}});
    if (name == "ai_status") {
        const auto status = AiSegmenter::instance().status();
        return textResult({{"engines", QJsonArray{"fast", "ai"}},
                           {"preferred", subjectEngineId(preferredSubjectEngine())},
                           {"ai_ready", status.ready()},
                           {"ai_runtime_available", status.runtimeAvailable},
                           {"ai_model_available", status.modelAvailable},
                           {"ai_runtime_version", status.runtimeVersion},
                           {"message", status.message}});
    }
    QString error;
    if (name == "create_document" || name == "open_document") {
        if (m_host->documentIds().size() >= m_host->capacity())
            return failure(
                QString("Close a document before opening another (limit %1).").arg(m_host->capacity()));
        std::unique_ptr<Document> document;
        if (name == "create_document") {
            const auto width = args["width"].toInt(), height = args["height"].toInt();
            if (qint64(width) * height > MaxPixels)
                return failure("MCP document limit is 16 million pixels.");
            const QColor color(args["background"].toString("transparent"));
            if (!color.isValid())
                return failure("Invalid background color.");
            document.reset(Document::create({width, height}, color, args["bit_depth"].toInt(8)));
            if (!document)
                return failure("Could not allocate the document.");
            document->title = args["title"].toString("Untitled");
        } else {
            const auto path = checkedPath(args["path"].toString(), false, &error);
            if (path.isEmpty())
                return failure(error);
            if (!readImageInput(path, &error))
                return failure(error);
            document.reset(FormatIO::open(path, &error));
            if (!document)
                return failure(error);
            if (!validDocument(document.get(), &error))
                return failure(error);
        }
        const auto id = m_host->adopt(std::move(document));
        return textResult(describe(id, m_host->document(id)));
    }
    const auto id = args["document_id"].toString();
    auto *document = m_host->document(id);
    if (!document)
        return failure("Unknown session document ID.");
    if (name == "inspect_document")
        return textResult(describe(id, document));
    if (name == "close_document") {
        if (!m_host->release(id, args["discard"].toBool(), &error))
            return failure(error);
        return textResult({{"closed_document_id", id}});
    }
    if (name == "focus_document") {
        if (!m_host->activate(id))
            return failure("This session cannot focus documents.");
        return textResult(describe(id, document));
    }
    if (name == "history") {
        QJsonArray steps;
        for (const auto &step : document->historyNames())
            steps.append(step);
        return textResult({{"document_id", id},
                           {"steps", steps},
                           {"can_undo", document->canUndo()},
                           {"can_redo", document->canRedo()}});
    }
    if (name == "preview_document" || name == "preview_layer") {
        QImage source = document->composite();
        if (name == "preview_layer") {
            bool ok = false;
            const int index = document->indexForId(args["layer_id"].toString().toULongLong(&ok));
            if (!ok || index < 0)
                return failure("Unknown layer ID.");
            source = document->layerImage(document->state.layers[index]);
        }
        const auto image = pngContent(source, args["max_dimension"].toInt(1024), &error);
        if (image.isEmpty())
            return failure(error);
        return {{"content", QJsonArray{image}}, {"isError", false}};
    }
    if (name == "save_document") {
        const auto path = checkedPath(args["path"].toString(), true, &error);
        if (path.isEmpty())
            return failure(error);
        if (QFileInfo::exists(path) && !args["overwrite"].toBool())
            return failure("Output exists; use another filename or explicitly set overwrite=true.");
        if (!FormatIO::save(document, path, &error))
            return failure(error);
        const auto suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "spe" || suffix == "speb") {
            document->filePath = path;
            document->markSaved();
        }
        return textResult({{"path", path},
                           {"native_master", suffix == "spe" || suffix == "speb"},
                           {"document", describe(id, document)}});
    }
    if (name == "undo" || name == "redo") {
        if (name == "undo") {
            if (!document->canUndo())
                return failure("Nothing to undo.");
            document->undo();
        } else {
            if (!document->canRedo())
                return failure("Nothing to redo.");
            document->redo();
        }
        return textResult(describe(id, document));
    }
    if (name == "select_layer" || name == "set_layer_properties") {
        bool ok = false;
        const quint64 layerId = args["layer_id"].toString().toULongLong(&ok);
        const int index = document->indexForId(layerId);
        if (!ok || index < 0)
            return failure("Unknown layer ID.");
        if (name == "select_layer")
            document->setActiveIndex(index);
        else {
            // Unlocking is always allowed; every other change honours the layer and group locks.
            const bool onlyUnlock = args.size() == 3 && args.contains("locked") && !args["locked"].toBool();
            const auto lock = onlyUnlock ? QString() : editLock(document, layerId);
            if (!lock.isEmpty())
                return failure(lock);
            if (args.contains("text") && document->state.layers[index].kind != LayerKind::Text)
                return failure("Only text layers have text content.");
            if (args.contains("color") && !QColor(args["color"].toString()).isValid())
                return failure("Invalid colour.");
            document->mutate("MCP layer properties", [&] {
                auto &layer = document->state.layers[index];
                if (args.contains("name"))
                    layer.name = args["name"].toString();
                if (args.contains("opacity"))
                    layer.opacity = args["opacity"].toDouble();
                if (args.contains("visible"))
                    layer.visible = args["visible"].toBool();
                if (args.contains("blend_mode"))
                    layer.blendMode = args["blend_mode"].toString();
                if (args.contains("fill"))
                    layer.fill = args["fill"].toDouble();
                if (args.contains("locked"))
                    layer.locked = args["locked"].toBool();
                if (args.contains("x") || args.contains("y"))
                    layer.offset = {args["x"].toDouble(layer.offset.x()),
                                    args["y"].toDouble(layer.offset.y())};
                if (args.contains("text") && layer.kind == LayerKind::Text)
                    layer.text = args["text"].toString();
                if (args.contains("color") && QColor(args["color"].toString()).isValid())
                    layer.color = QColor(args["color"].toString());
            });
        }
        return textResult(describe(id, document));
    }
    if (name == "add_layer" || name == "create_text_layer") {
        if (document->state.layers.size() >= 256)
            return failure("MCP layer limit is 256.");
        const QColor color(args["color"].toString("#8b5cf6"));
        if (!color.isValid())
            return failure("Invalid layer color.");
        if (const auto *active = document->activeLayer()) {
            const quint64 parent = active->kind == LayerKind::Group ? active->id : active->parentId;
            const auto lock = editLock(document, parent);
            if (!lock.isEmpty())
                return failure(lock);
        }
        document->beginTransaction("MCP add layer");
        if (name == "create_text_layer") {
            document->addLayer("Text", LayerKind::Text);
            auto *layer = document->activeLayer();
            layer->text = args["text"].toString();
            layer->font = QFont(args["font_family"].toString("Onest"));
            layer->font.setPixelSize(args["font_size"].toInt(48));
            layer->font.setBold(args["bold"].toBool());
            layer->font.setItalic(args["italic"].toBool());
            layer->font.setLetterSpacing(QFont::AbsoluteSpacing, args["tracking"].toDouble());
            layer->offset = {args["x"].toDouble(), args["y"].toDouble()};
        } else {
            const auto kind = args["kind"].toString();
            document->addLayer(args["name"].toString(), kind == "group"           ? LayerKind::Group
                                                        : kind == "solid-fill"    ? LayerKind::SolidFill
                                                        : kind == "gradient-fill" ? LayerKind::GradientFill
                                                        : kind == "pattern-fill"  ? LayerKind::PatternFill
                                                                                  : LayerKind::Pixel);
        }
        document->activeLayer()->color = color;
        document->activeLayer()->parameters = args["parameters"].toObject();
        if (!validDocument(document, &error)) {
            document->cancelTransaction();
            return failure(error);
        }
        document->touch();
        document->endTransaction();
        return textResult(describe(id, document));
    }
    if (name == "delete_layers" || name == "duplicate_layers" || name == "group_layers" ||
        name == "ungroup_layers" || name == "move_layers") {
        const auto ids = layerIdList(args, document, &error);
        if (ids.isEmpty())
            return failure(error);
        LayerOperationResult result;
        if (name == "delete_layers")
            result = deleteLayers(document, ids);
        else if (name == "duplicate_layers")
            result = duplicateLayers(document, ids);
        else if (name == "group_layers")
            result = groupLayers(document, ids, args["name"].toString());
        else if (name == "ungroup_layers")
            result = ungroupLayers(document, ids);
        else {
            const auto position = args["position"].toString();
            const LayerDrop drop = position == "below"    ? LayerDrop::Below
                                   : position == "inside" ? LayerDrop::Inside
                                   : position == "top"    ? LayerDrop::Top
                                   : position == "bottom" ? LayerDrop::Bottom
                                                          : LayerDrop::Above;
            const quint64 target = args["target_layer_id"].toString().toULongLong();
            if ((drop == LayerDrop::Above || drop == LayerDrop::Below || drop == LayerDrop::Inside) &&
                !target)
                return failure("above, below and inside need target_layer_id.");
            result = moveLayers(document, ids, target, drop);
        }
        if (!result.error.isEmpty())
            return failure(result.error);
        if (document->state.layers.size() > 256) {
            document->undo();
            return failure("MCP layer limit is 256.");
        }
        auto description = describe(id, document);
        QJsonArray selection;
        for (const auto layerId : result.selection)
            selection.append(QString::number(layerId));
        description["changed"] = result.changed;
        description["result_layer_ids"] = selection;
        return textResult(description);
    }
    if (name == "select_subject" || name == "remove_background") {
        SubjectEngine chosen = preferredSubjectEngine();
        if (args.contains("engine") && !parseSubjectEngine(args["engine"].toString(), &chosen))
            return failure("engine must be fast or ai.");
        int index = document->state.activeIndex;
        if (args.contains("layer_id")) {
            bool ok = false;
            index = document->indexForId(args["layer_id"].toString().toULongLong(&ok));
            if (!ok || index < 0)
                return failure("Unknown layer ID.");
        }
        if (name == "remove_background") {
            if (index < 0)
                return failure("Select a layer first.");
            const auto lock = editLock(document, document->state.layers[index].id);
            if (!lock.isEmpty())
                return failure(lock);
        }
        QElapsedTimer timer;
        timer.start();
        const QImage mask = segmentSubject(document->composite(), chosen, &error);
        if (mask.isNull())
            return failure(error.isEmpty() ? "No subject was found." : error);
        if (name == "select_subject")
            document->setSelection(mask);
        else {
            // Build the mask from the subject through the normal Add Layer Mask path, multiply it
            // with any existing mask, and leave the user's selection and pixel targeting untouched.
            document->beginTransaction("Remove background");
            const QImage previousSelection = document->state.selection;
            const QImage previousMask = document->state.layers[index].mask;
            document->setActiveIndex(index);
            document->setSelection(mask);
            document->addMask();
            document->mutate("Remove background", [&] {
                auto &layer = document->state.layers[index];
                if (!previousMask.isNull() && previousMask.size() == layer.mask.size())
                    for (int y = 0; y < layer.mask.height(); ++y)
                        for (int x = 0; x < layer.mask.width(); ++x)
                            setMaskSample(layer.mask, x, y,
                                          maskSample(layer.mask, x, y) * maskSample(previousMask, x, y));
                layer.maskTarget = false;
                document->state.selection = previousSelection;
            });
            document->endTransaction();
        }
        auto description = describe(id, document);
        description["engine"] = subjectEngineId(chosen);
        description["milliseconds"] = qint64(timer.elapsed());
        return textResult(description);
    }
    if (name == "add_layer_mask" || name == "delete_layer_mask") {
        bool ok = false;
        const int index = document->indexForId(args["layer_id"].toString().toULongLong(&ok));
        if (!ok || index < 0)
            return failure("Unknown layer ID.");
        const auto lock = editLock(document, document->state.layers[index].id);
        if (!lock.isEmpty())
            return failure(lock);
        document->setActiveIndex(index);
        if (name == "delete_layer_mask") {
            if (document->state.layers[index].mask.isNull())
                return failure("The layer has no mask.");
            document->removeMask(args["apply"].toBool());
            return textResult(describe(id, document));
        }
        const auto from = args["from"].toString(document->hasSelection() ? "selection" : "reveal_all");
        if (from == "selection" && !document->hasSelection())
            return failure("There is no selection to build the mask from.");
        document->beginTransaction("Add Layer Mask");
        const QImage previousSelection = document->state.selection;
        if (from != "selection")
            document->state.selection = QImage();
        document->addMask();
        document->mutate("Add Layer Mask", [&] {
            auto &layer = document->state.layers[index];
            if (from == "hide_all")
                layer.mask.fill(0);
            layer.maskTarget = false;
            document->state.selection = previousSelection;
        });
        document->endTransaction();
        return textResult(describe(id, document));
    }
    if (name == "place_image") {
        if (document->state.layers.size() >= 256)
            return failure("MCP layer limit is 256.");
        const auto path = checkedPath(args["path"].toString(), false, &error);
        if (path.isEmpty() || !readImageInput(path, &error))
            return failure(error);
        QImageReader reader(path);
        reader.setDecideFormatFromContent(true);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (image.isNull())
            return failure("Could not decode the image: " + reader.errorString());
        if (args.contains("width") || args.contains("height")) {
            const QSize size(args["width"].toInt(image.width()), args["height"].toInt(image.height()));
            if (qint64(size.width()) * size.height() > MaxPixels)
                return failure("The scaled image exceeds MCP limits.");
            image = image.scaled(size,
                                 args.contains("width") && args.contains("height") ? Qt::IgnoreAspectRatio
                                                                                   : Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
        if (const auto *active = document->activeLayer()) {
            const quint64 parent = active->kind == LayerKind::Group ? active->id : active->parentId;
            const auto lock = editLock(document, parent);
            if (!lock.isEmpty())
                return failure(lock);
        }
        document->beginTransaction("Place image");
        document->addLayer(args["name"].toString(QFileInfo(path).completeBaseName()));
        document->mutate("Place image", [&] {
            auto *layer = document->activeLayer();
            layer->pixels = TileImage::fromImage(image.convertToFormat(QImage::Format_RGBA8888));
            layer->offset = {args["x"].toDouble(), args["y"].toDouble()};
        });
        if (!validDocument(document, &error)) {
            document->cancelTransaction();
            return failure(error);
        }
        document->endTransaction();
        return textResult(describe(id, document));
    }
    if (name == "run_actions") {
        const auto steps = args["steps"].toArray();
        if (steps.isEmpty() || steps.size() > 100)
            return failure("Provide 1–100 action objects.");
        for (const auto &value : steps) {
            if (!value.isObject())
                return failure("Every action must be an object.");
        }
        document->beginTransaction("MCP action set");
        for (const auto &step : steps) {
            // Resolve defaults against the state produced by the previous step, before allocating pixels.
            if (!validActionAllocation(document, step.toObject(), &error) ||
                !ActionRunner::execute(document, step.toObject(), &error) ||
                !validDocument(document, &error)) {
                document->cancelTransaction();
                return failure(error);
            }
        }
        document->endTransaction();
        return textResult(describe(id, document));
    }
    return failure("Tool is not implemented.");
}
std::optional<QJsonObject> McpServer::handle(const QJsonObject &message) {
    const auto id = message.value("id");
    const auto method = message.value("method").toString();
    if (message["jsonrpc"] != "2.0" || !message["method"].isString() || method.isEmpty() ||
        (message.contains("id") && !id.isString() && !id.isDouble()))
        return rpcError(QJsonValue::Null, -32600, "Invalid JSON-RPC request.");
    if (!message.contains("id")) {
        if (method == "notifications/initialized" && m_initialized)
            m_ready = true;
        return std::nullopt;
    }
    if (message.contains("params") && !message["params"].isObject())
        return rpcError(id, -32602, "Parameters must be an object.");
    const auto params = message["params"].toObject();
    const auto meta = params["_meta"].toObject();
    // Modern (2026-07-28) requests carry their version in _meta and need no handshake.
    const bool modern = meta.contains(MetaVersion);
    if (modern && meta[MetaVersion].toString() != ModernVersion)
        return QJsonObject{{"jsonrpc", "2.0"},
                           {"id", id},
                           {"error", QJsonObject{{"code", UnsupportedProtocolVersion},
                                                 {"message", "Unsupported protocol version"},
                                                 {"data", QJsonObject{{"supported", supportedVersions()},
                                                                      {"requested", meta[MetaVersion]}}}}}};
    const QString instructions =
        m_host->live()
            ? "Live session: documents are the tabs open in the Serika PhotoEdit window. Call "
              "list_documents, "
              "then inspect_document for stable layer IDs before editing. Every edit appears on screen and "
              "can be undone by the user. Preview before saving; save native SPE for editable layers. Paths "
              "must stay inside the configured workspace; existing files require overwrite=true."
            : "Documents belong to this isolated local session. Inspect IDs before editing, preview before "
              "saving, and save native SPE for editable layers. Paths must stay inside the configured "
              "workspace. Existing files require overwrite=true. Use list_actions for run_actions commands.";
    const QJsonObject capabilities{{"tools", QJsonObject{{"listChanged", false}}}};
    QJsonObject result;
    if (method == "server/discover") {
        result = {{"supportedVersions", supportedVersions()},
                  {"capabilities", capabilities},
                  {"instructions", instructions},
                  {"ttlMs", 3600000},
                  {"cacheScope", "private"}};
    } else if (method == "initialize" && !modern) {
        if (m_initialized)
            return rpcError(id, -32600, "This session is already initialized.");
        if (!m_startupError.isEmpty())
            return rpcError(id, -32000, m_startupError);
        if (!params["protocolVersion"].isString() || !params["capabilities"].isObject() ||
            !params["clientInfo"].isObject())
            return rpcError(id, -32602, "Initialize requires protocolVersion, capabilities and clientInfo.");
        const auto requested = params["protocolVersion"].toString();
        result = {
            {"protocolVersion", LegacyVersions.contains(requested) ? requested : LegacyVersions.first()},
            {"capabilities", capabilities},
            {"serverInfo", serverInfo()},
            {"instructions", instructions}};
        m_initialized = true;
    } else if (method == "ping" && !modern) {
        result = {};
    } else {
        if (!modern && !m_ready)
            return rpcError(
                id, -32002,
                "Send " + MetaVersion + " = " + ModernVersion +
                    " in params._meta, or initialize the session and send notifications/initialized "
                    "first (protocol " +
                    LegacyVersions.first() + " and earlier).");
        if (modern && !m_startupError.isEmpty())
            return rpcError(id, -32000, m_startupError);
        if (method == "tools/list") {
            if (params.contains("cursor"))
                return rpcError(id, -32602, "This server has no additional tool pages.");
            result = {
                {"tools", toolDefinitions(m_host->live())}, {"ttlMs", 3600000}, {"cacheScope", "private"}};
        } else if (method == "tools/call") {
            const auto name = params["name"].toString();
            QJsonObject definition;
            for (const auto &value : toolDefinitions(m_host->live()))
                if (value.toObject()["name"] == name)
                    definition = value.toObject();
            if (definition.isEmpty())
                return rpcError(id, -32602, "Unknown tool: " + name);
            if (params.contains("arguments") && !params["arguments"].isObject())
                return rpcError(id, -32602, "Tool arguments must be an object.");
            QString error;
            const auto arguments = params["arguments"].toObject();
            if (!validate(arguments, definition["inputSchema"].toObject(), &error))
                return rpcError(id, -32602, error);
            result = call(name, arguments);
        } else
            return rpcError(id, -32601, "Method not found: " + method);
    }
    // Every result is complete (no multi round-trip input is ever requested) and names the server.
    result["resultType"] = "complete";
    auto resultMeta = result["_meta"].toObject();
    resultMeta[MetaServerInfo] = serverInfo();
    result["_meta"] = resultMeta;
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
int McpServer::runStdio(const QString &workspace) {
    McpServer server(workspace);
    if (!server.startupError().isEmpty()) {
        std::fprintf(stderr, "%s\n", server.startupError().toUtf8().constData());
        return 2;
    }
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly))
        return 2;
    constexpr qint64 limit = 4 * 1024 * 1024;
    while (true) {
        auto line = input.readLine(limit + 2);
        if (line.isEmpty())
            break;
        std::optional<QJsonObject> response;
        if (line.size() > limit) {
            while (!line.endsWith('\n') && !input.atEnd())
                line = input.readLine(limit + 2);
            response = rpcError(QJsonValue::Null, -32600, "MCP message exceeds 4 MiB.");
        } else {
            QJsonParseError parse;
            const auto json = QJsonDocument::fromJson(line, &parse);
            if (parse.error != QJsonParseError::NoError)
                response = rpcError(QJsonValue::Null, -32700, "Invalid JSON.");
            else if (!json.isObject())
                response = rpcError(QJsonValue::Null, -32600,
                                    "Requests must be JSON objects; batches are unsupported.");
            else
                response = server.handle(json.object());
        }
        if (response) {
            const auto bytes = QJsonDocument(*response).toJson(QJsonDocument::Compact) + '\n';
            if (output.write(bytes) != bytes.size() || !output.flush())
                return 1;
        }
    }
    return 0;
}
QString McpServer::liveSocketName() {
    // Per user, so one person's agent never reaches another person's open documents.
    const auto user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER", "user"));
    return "serika-photoedit-mcp-" +
           QString::fromLatin1(
               QCryptographicHash::hash(user.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}
int McpServer::runAttached(const QString &workspace, bool launch) {
    QLocalSocket socket;
    const auto connectNow = [&] {
        socket.connectToServer(liveSocketName());
        return socket.waitForConnected(1000);
    };
    if (!connectNow()) {
        if (!launch) {
            std::fprintf(stderr, "Serika PhotoEdit is not running; start it or omit --no-launch.\n");
            return 2;
        }
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), {})) {
            std::fprintf(stderr, "Could not start Serika PhotoEdit.\n");
            return 2;
        }
        QElapsedTimer timer;
        timer.start();
        while (!connectNow() && timer.elapsed() < 45000)
            QThread::msleep(400);
        if (socket.state() != QLocalSocket::ConnectedState) {
            std::fprintf(stderr, "Serika PhotoEdit started but did not accept the MCP connection. Check "
                                 "Preferences > AI > Allow agents.\n");
            return 2;
        }
    }
    // Private bridge protocol: one handshake line, then exactly one reply line per request line
    // (an empty line when JSON-RPC has nothing to send back, such as after a notification).
    const auto readReply = [&](QByteArray *line) {
        while (!socket.canReadLine())
            if (!socket.waitForReadyRead(-1))
                return false;
        *line = socket.readLine();
        return true;
    };
    const QByteArray hello = QJsonDocument(QJsonObject{{"serika_attach", 1}, {"workspace", workspace}})
                                 .toJson(QJsonDocument::Compact) +
                             '\n';
    socket.write(hello);
    socket.waitForBytesWritten(-1);
    QByteArray reply;
    if (!readReply(&reply)) {
        std::fprintf(stderr, "The Serika window closed the MCP connection.\n");
        return 1;
    }
    const auto accepted = QJsonDocument::fromJson(reply).object();
    if (!accepted["ok"].toBool()) {
        std::fprintf(
            stderr, "%s\n",
            accepted["error"].toString("The Serika window refused the session.").toUtf8().constData());
        return 2;
    }
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly))
        return 2;
    while (true) {
        auto line = input.readLine(4 * 1024 * 1024 + 2);
        if (line.isEmpty())
            break;
        if (!line.endsWith('\n'))
            line += '\n';
        socket.write(line);
        if (!socket.waitForBytesWritten(-1) || !readReply(&reply))
            return 1;
        if (!reply.trimmed().isEmpty() && (output.write(reply) != reply.size() || !output.flush()))
            return 1;
    }
    return 0;
}
} // namespace serika
