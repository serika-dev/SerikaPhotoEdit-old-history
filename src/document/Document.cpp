#include "Document.h"
#include "SmartObjectOperations.h"
#include <QBuffer>
#include <QColorSpace>
#include <QDataStream>
#include <QJsonArray>
#include <QSet>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace serika {
QImage renderLayerImage(const Layer &layer, const QSize &canvas, int bitDepth);

namespace {
QImage::Format storageFormat(const QImage &image) {
    if (image.format() == QImage::Format_RGBA32FPx4 ||
        image.format() == QImage::Format_RGBA32FPx4_Premultiplied ||
        image.format() == QImage::Format_RGBX32FPx4)
        return QImage::Format_RGBA32FPx4;
    return image.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
}
bool transparent(const QImage &image) {
    if (image.format() == QImage::Format_RGBA64) {
        for (int y = 0; y < image.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if (line[x].alpha())
                    return false;
        }
    } else if (image.format() == QImage::Format_RGBA32FPx4) {
        for (int y = 0; y < image.height(); ++y) {
            const auto *line = reinterpret_cast<const float *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if (line[x * 4 + 3] != 0)
                    return false;
        }
    } else {
        for (int y = 0; y < image.height(); ++y) {
            const auto *line = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                if (line[x * 4 + 3])
                    return false;
        }
    }
    return true;
}
QImage maskAtSize(const QImage &image, QSize size, int depth = 0) {
    const int bits = depth ? depth
                           : (image.format() == QImage::Format_RGBA32FPx4    ? 32
                              : image.format() == QImage::Format_Grayscale16 ? 16
                                                                             : 8);
    QImage result = makeMask(size, bits);
    if (!image.isNull()) {
        const QImage input = normalizeMask(image, bits);
        const QSize extent = input.size().boundedTo(size);
        for (int y = 0; y < extent.height(); ++y)
            std::memcpy(result.scanLine(y), input.constScanLine(y), extent.width() * input.depth() / 8);
    }
    return result;
}
} // namespace

qreal maskSample(const QImage &mask, int x, int y) {
    if (!mask.rect().contains(x, y))
        return 0;
    if (mask.format() == QImage::Format_Grayscale16)
        return reinterpret_cast<const quint16 *>(mask.constScanLine(y))[x] / 65535.;
    if (mask.format() == QImage::Format_Grayscale8)
        return mask.constScanLine(y)[x] / 255.;
    if (mask.format() == QImage::Format_RGBA32FPx4) {
        const qreal value = reinterpret_cast<const float *>(mask.constScanLine(y))[x * 4];
        return std::isfinite(value) ? std::clamp(value, 0., 1.) : 0;
    }
    return qGray(mask.pixel(x, y)) / 255.;
}
void setMaskSample(QImage &mask, int x, int y, qreal value) {
    if (!mask.rect().contains(x, y))
        return;
    value = std::isfinite(value) ? std::clamp(value, 0., 1.) : 0;
    if (mask.format() == QImage::Format_Grayscale16)
        reinterpret_cast<quint16 *>(mask.scanLine(y))[x] = quint16(qRound(value * 65535));
    else if (mask.format() == QImage::Format_Grayscale8)
        mask.scanLine(y)[x] = uchar(qRound(value * 255));
    else if (mask.format() == QImage::Format_RGBA32FPx4) {
        float *pixel = reinterpret_cast<float *>(mask.scanLine(y)) + x * 4;
        pixel[0] = pixel[1] = pixel[2] = float(value);
        pixel[3] = 1;
    } else
        mask.setPixelColor(x, y, QColor::fromRgbF(value, value, value));
}
QImage makeMask(QSize size, int bitDepth, qreal value) {
    QImage mask(size, bitDepth == 32 ? QImage::Format_RGBA32FPx4
                      : bitDepth > 8 ? QImage::Format_Grayscale16
                                     : QImage::Format_Grayscale8);
    if (mask.isNull())
        return mask;
    value = std::isfinite(value) ? std::clamp(value, 0., 1.) : 0;
    if (mask.format() == QImage::Format_Grayscale16) {
        const quint16 sample = quint16(qRound(value * 65535));
        for (int y = 0; y < size.height(); ++y)
            std::fill_n(reinterpret_cast<quint16 *>(mask.scanLine(y)), size.width(), sample);
    } else if (mask.format() == QImage::Format_RGBA32FPx4) {
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                float *pixel = reinterpret_cast<float *>(mask.scanLine(y)) + x * 4;
                pixel[0] = pixel[1] = pixel[2] = float(value);
                pixel[3] = 1;
            }
    } else
        mask.fill(uchar(qRound(value * 255)));
    return mask;
}
QImage normalizeMask(const QImage &mask, int bitDepth) {
    if (mask.isNull())
        return {};
    if (!bitDepth)
        bitDepth = mask.format() == QImage::Format_RGBA32FPx4                         ? 32
                   : mask.format() == QImage::Format_Grayscale16 || mask.depth() > 32 ? 16
                                                                                      : 8;
    const QImage::Format format = bitDepth == 32 ? QImage::Format_RGBA32FPx4
                                  : bitDepth > 8 ? QImage::Format_Grayscale16
                                                 : QImage::Format_Grayscale8;
    return mask.format() == format ? mask : mask.convertToFormat(format);
}
namespace {
QImage blurCoverage(const QImage &input, qreal radius) {
    if (input.isNull() || radius <= 0)
        return input;
    const int width = input.width(), height = input.height();
    // Three separable boxes approximate a Gaussian with sigma equal to the requested feather.
    // Double accumulators retain sub-8-bit coverage in native 16-bit masks.
    const int r = std::clamp(qRound(radius), 1, 4096);
    std::vector<double> values(size_t(width) * height), temporary(values.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            values[size_t(y) * width + x] = maskSample(input, x, y);
    for (int iteration = 0; iteration < 3; ++iteration) {
        for (int y = 0; y < height; ++y) {
            double sum = 0;
            for (int x = -r; x <= r; ++x)
                sum += values[size_t(y) * width + std::clamp(x, 0, width - 1)];
            for (int x = 0; x < width; ++x) {
                temporary[size_t(y) * width + x] = sum / (2 * r + 1);
                sum -= values[size_t(y) * width + std::clamp(x - r, 0, width - 1)];
                sum += values[size_t(y) * width + std::clamp(x + r + 1, 0, width - 1)];
            }
        }
        for (int x = 0; x < width; ++x) {
            double sum = 0;
            for (int y = -r; y <= r; ++y)
                sum += temporary[size_t(std::clamp(y, 0, height - 1)) * width + x];
            for (int y = 0; y < height; ++y) {
                values[size_t(y) * width + x] = sum / (2 * r + 1);
                sum -= temporary[size_t(std::clamp(y - r, 0, height - 1)) * width + x];
                sum += temporary[size_t(std::clamp(y + r + 1, 0, height - 1)) * width + x];
            }
        }
    }
    QImage result = makeMask(input.size(), input.format() == QImage::Format_RGBA32FPx4    ? 32
                                           : input.format() == QImage::Format_Grayscale16 ? 16
                                                                                          : 8);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            setMaskSample(result, x, y, values[size_t(y) * width + x]);
    return result;
}
QImage morphCoverage(const QImage &input, int radius, bool grow) {
    if (input.isNull() || !radius)
        return input;
    radius = std::clamp(radius, 1, 4096);
    QImage horizontal = input, result = input;
    auto scan = [&](int extent, const auto &read, const auto &write) {
        std::deque<std::pair<int, qreal>> window;
        for (int i = -radius; i < extent + radius; ++i) {
            const qreal value = read(std::clamp(i, 0, extent - 1));
            while (!window.empty() && (grow ? window.back().second <= value : window.back().second >= value))
                window.pop_back();
            window.emplace_back(i, value);
            const int position = i - radius;
            if (position >= 0) {
                while (!window.empty() && window.front().first < position - radius)
                    window.pop_front();
                write(position, window.front().second);
            }
        }
    };
    for (int y = 0; y < input.height(); ++y)
        scan(
            input.width(), [&](int x) { return maskSample(input, x, y); },
            [&](int x, qreal value) { setMaskSample(horizontal, x, y, value); });
    for (int x = 0; x < input.width(); ++x)
        scan(
            input.height(), [&](int y) { return maskSample(horizontal, x, y); },
            [&](int y, qreal value) { setMaskSample(result, x, y, value); });
    return result;
}
} // namespace
QImage refineMask(const QImage &input, const QJsonObject &parameters, const QImage &source) {
    QImage mask = normalizeMask(input);
    if (mask.isNull())
        return mask;
    const int edgeRadius = std::clamp(qRound(parameters.value("radius").toDouble()), 0, 100);
    if (edgeRadius > 0) {
        if (!source.isNull() && source.size() == mask.size()) {
            const QImage guide = source.convertToFormat(QImage::Format_RGBA8888);
            QImage refined = mask;
            const int step = std::max(1, edgeRadius / 6);
            const bool adaptive = parameters.value("smartRadius").toBool();
            for (int y = 0; y < mask.height(); ++y)
                for (int x = 0; x < mask.width(); ++x) {
                    const qreal center = maskSample(mask, x, y);
                    const qreal localMin =
                        std::min({maskSample(mask, std::max(0, x - edgeRadius), y),
                                  maskSample(mask, std::min(mask.width() - 1, x + edgeRadius), y),
                                  maskSample(mask, x, std::max(0, y - edgeRadius)),
                                  maskSample(mask, x, std::min(mask.height() - 1, y + edgeRadius)), center});
                    const qreal localMax =
                        std::max({maskSample(mask, std::max(0, x - edgeRadius), y),
                                  maskSample(mask, std::min(mask.width() - 1, x + edgeRadius), y),
                                  maskSample(mask, x, std::max(0, y - edgeRadius)),
                                  maskSample(mask, x, std::min(mask.height() - 1, y + edgeRadius)), center});
                    if (localMax - localMin < .005)
                        continue;
                    const uchar *pixel = guide.constScanLine(y) + x * 4;
                    double total = 0, weight = 0;
                    const double sigma = adaptive ? 30. : 48.;
                    for (int dy = -edgeRadius; dy <= edgeRadius; dy += step)
                        for (int dx = -edgeRadius; dx <= edgeRadius; dx += step) {
                            const int sx = std::clamp(x + dx, 0, mask.width() - 1),
                                      sy = std::clamp(y + dy, 0, mask.height() - 1);
                            const uchar *sample = guide.constScanLine(sy) + sx * 4;
                            double difference = 0;
                            for (int c = 0; c < 3; ++c)
                                difference += std::pow(double(pixel[c]) - sample[c], 2);
                            const double w =
                                std::exp(-difference / (2 * sigma * sigma) -
                                         double(dx * dx + dy * dy) / (2 * edgeRadius * edgeRadius));
                            total += maskSample(mask, sx, sy) * w;
                            weight += w;
                        }
                    if (weight > 0)
                        setMaskSample(refined, x, y, total / weight);
                }
            mask = refined;
        } else
            mask = blurCoverage(mask, edgeRadius / 3.);
    }
    const qreal smooth = std::max(0., parameters.value("smooth").toDouble());
    if (smooth > 0) {
        const int radius = std::clamp(qRound(smooth), 1, 100);
        // Opening and closing suppress small spurs and fill small holes without shrinking the outline.
        mask = morphCoverage(morphCoverage(mask, radius, false), radius, true);
        mask = morphCoverage(morphCoverage(mask, radius, true), radius, false);
    }
    const qreal edge = parameters.value("shiftEdge").toDouble();
    if (std::abs(edge) >= .5)
        mask = morphCoverage(mask, qRound(std::abs(edge)), edge > 0);
    mask = blurCoverage(mask, parameters.value("feather").toDouble());
    const qreal contrast = std::clamp(parameters.value("contrast").toDouble(), 0., 100.);
    if (contrast > 0)
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x) {
                const qreal value = maskSample(mask, x, y);
                setMaskSample(mask, x, y,
                              contrast >= 100 ? (value >= .5 ? 1 : 0)
                                              : (value - .5) / (1 - contrast / 100) + .5);
            }
    return mask;
}
QImage renderedLayerMask(const Layer &layer, QSize canvas, int bitDepth, bool includeRaster,
                         bool includeVector) {
    const bool raster = includeRaster && layer.maskEnabled && !layer.mask.isNull();
    const bool vector = includeVector && layer.vectorMaskEnabled && !layer.vectorMask.isEmpty();
    if (!raster && !vector)
        return {};
    const QImage pixels =
        raster ? refineMask(normalizeMask(layer.mask, bitDepth), {{"feather", layer.maskFeather}}) : QImage();
    QImage vectorCoverage;
    if (vector) {
        // Paint vector coverage directly at canvas resolution with a 16-bit alpha target.
        QImage target(canvas, bitDepth == 32 ? QImage::Format_RGBA32FPx4
                              : bitDepth > 8 ? QImage::Format_RGBA64
                                             : QImage::Format_RGBA8888);
        target.fill(Qt::transparent);
        QPainter painter(&target);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(layer.offset + (layer.vectorMaskLinked ? QPointF() : layer.vectorMaskOffset));
        painter.fillPath(layer.vectorMask, Qt::white);
        painter.end();
        vectorCoverage = makeMask(canvas, bitDepth);
        for (int y = 0; y < canvas.height(); ++y)
            for (int x = 0; x < canvas.width(); ++x)
                setMaskSample(vectorCoverage, x, y,
                              bitDepth == 32
                                  ? reinterpret_cast<const float *>(target.constScanLine(y))[x * 4 + 3]
                                  : target.pixelColor(x, y).alphaF());
        vectorCoverage = refineMask(vectorCoverage, {{"feather", layer.vectorMaskFeather}});
    }
    QImage result = makeMask(canvas, bitDepth, 1);
    const QPointF origin = layer.offset + (layer.maskLinked ? QPointF() : layer.maskOffset);
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x) {
            qreal value = 1;
            if (raster) {
                const qreal sx = x - origin.x(), sy = y - origin.y();
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const qreal dx = sx - ix, dy = sy - iy;
                const qreal coverage = maskSample(pixels, ix, iy) * (1 - dx) * (1 - dy) +
                                       maskSample(pixels, ix + 1, iy) * dx * (1 - dy) +
                                       maskSample(pixels, ix, iy + 1) * (1 - dx) * dy +
                                       maskSample(pixels, ix + 1, iy + 1) * dx * dy;
                value *= 1 - std::clamp(layer.maskDensity, 0., 1.) * (1 - coverage);
            }
            if (vector)
                value *=
                    1 - std::clamp(layer.vectorMaskDensity, 0., 1.) * (1 - maskSample(vectorCoverage, x, y));
            setMaskSample(result, x, y, value);
        }
    return result;
}

TileImage TileImage::fromImage(const QImage &image) {
    TileImage result;
    result.setImage(image);
    return result;
}
void TileImage::setImage(const QImage &image) {
    size = image.size();
    tiles.clear();
    if (image.isNull())
        return;
    format = storageFormat(image);
    const QImage input = image.convertToFormat(format);
    for (int y = 0; y < size.height(); y += TileSize)
        for (int x = 0; x < size.width(); x += TileSize) {
            const QImage tile = input.copy(QRect(x, y, TileSize, TileSize).intersected(input.rect()));
            if (!transparent(tile))
                tiles.insert(key(x / TileSize, y / TileSize), tile);
        }
}
QImage TileImage::image() const { return region(QRect(QPoint(), size)); }
QImage TileImage::region(const QRect &rect) const {
    if (rect.isEmpty())
        return {};
    QImage result(rect.size(), format);
    result.fill(Qt::transparent);
    const QRect clipped = rect.intersected(QRect(QPoint(), size));
    if (clipped.isEmpty())
        return result;
    for (int ty = clipped.top() / TileSize; ty <= clipped.bottom() / TileSize; ++ty)
        for (int tx = clipped.left() / TileSize; tx <= clipped.right() / TileSize; ++tx) {
            const auto it = tiles.constFind(key(tx, ty));
            if (it != tiles.cend()) {
                const QPoint origin(tx * TileSize, ty * TileSize);
                const QRect overlap = QRect(origin, it.value().size()).intersected(rect);
                const int bytesPerPixel = result.depth() / 8;
                // Copy straight-alpha bytes directly: a painter would round-trip through
                // premultiplied alpha and can lose a 16-bit value at partial transparency.
                for (int y = overlap.top(); y <= overlap.bottom(); ++y)
                    std::memcpy(result.scanLine(y - rect.top()) +
                                    (overlap.left() - rect.left()) * bytesPerPixel,
                                it.value().constScanLine(y - origin.y()) +
                                    (overlap.left() - origin.x()) * bytesPerPixel,
                                overlap.width() * bytesPerPixel);
            }
        }
    return result;
}
void TileImage::paint(const QRect &bounds, const std::function<void(QPainter &)> &draw) {
    const QRect clipped = bounds.intersected(QRect(QPoint(), size));
    if (clipped.isEmpty())
        return;
    for (int ty = clipped.top() / TileSize; ty <= clipped.bottom() / TileSize; ++ty)
        for (int tx = clipped.left() / TileSize; tx <= clipped.right() / TileSize; ++tx) {
            const quint64 tileKey = key(tx, ty);
            const QPoint origin(tx * TileSize, ty * TileSize);
            QImage tile = tiles.value(tileKey);
            const QSize tileSize = QSize(TileSize, TileSize).boundedTo(size - QSize(origin.x(), origin.y()));
            if (tile.isNull()) {
                tile = QImage(tileSize, format);
                tile.fill(Qt::transparent);
            } else if (tile.size() != tileSize) {
                QImage expanded(tileSize, format);
                expanded.fill(Qt::transparent);
                const QSize overlap = tile.size().boundedTo(tileSize);
                const int pixelBytes = tile.depth() / 8;
                for (int y = 0; y < overlap.height(); ++y)
                    std::memcpy(expanded.scanLine(y), tile.constScanLine(y), overlap.width() * pixelBytes);
                tile = expanded;
            }
            {
                // QPainter detaches only the modified tile. Undo snapshots retain other tiles.
                QPainter painter(&tile);
                painter.translate(-origin);
                painter.setClipRect(clipped);
                draw(painter);
            }
            if (transparent(tile))
                tiles.remove(tileKey);
            else
                tiles.insert(tileKey, tile);
        }
}

Document::Document(QObject *parent) : QObject(parent) {}
Document *Document::create(QSize size, QColor background, int bitDepth, QObject *parent) {
    auto *document = new Document(parent);
    document->state.size = size.expandedTo(QSize(1, 1));
    document->state.bitDepth = bitDepth == 16 ? 16 : bitDepth == 32 ? 32 : 8;
    Layer layer;
    layer.id = document->m_nextId++;
    layer.name = "Background";
    layer.pixels.size = document->state.size;
    layer.pixels.format = bitDepth == 16   ? QImage::Format_RGBA64
                          : bitDepth == 32 ? QImage::Format_RGBA32FPx4
                                           : QImage::Format_RGBA8888;
    if (background.alpha() != 0)
        layer.pixels.paint(QRect(QPoint(), document->state.size), [&](QPainter &p) {
            p.fillRect(QRect(QPoint(), document->state.size), background);
        });
    document->state.layers.append(layer);
    return document;
}
Layer *Document::activeLayer() {
    return state.activeIndex >= 0 && state.activeIndex < state.layers.size()
               ? &state.layers[state.activeIndex]
               : nullptr;
}
const Layer *Document::activeLayer() const {
    return state.activeIndex >= 0 && state.activeIndex < state.layers.size()
               ? &state.layers[state.activeIndex]
               : nullptr;
}
int Document::indexForId(quint64 id) const {
    for (int i = 0; i < state.layers.size(); ++i)
        if (state.layers[i].id == id)
            return i;
    return -1;
}
QPointF Document::effectiveLayerOffset(const Layer &layer) const {
    QPointF offset = layer.offset;
    QSet<quint64> visited{layer.id};
    quint64 parent = layer.parentId;
    while (parent && !visited.contains(parent)) {
        visited.insert(parent);
        const int index = indexForId(parent);
        if (index < 0)
            break;
        offset += state.layers[index].offset;
        parent = state.layers[index].parentId;
    }
    return offset;
}
void Document::setActiveIndex(int index) {
    if (index < 0 || index >= state.layers.size() || index == state.activeIndex)
        return;
    state.activeIndex = index;
    emit activeLayerChanged();
    emit changed();
}
quint64 Document::addLayer(QString name, LayerKind kind) {
    quint64 id = 0;
    mutate("New Layer", [&] {
        for (const auto &layer : state.layers)
            m_nextId = std::max(m_nextId, layer.id + 1);
        Layer layer;
        layer.id = id = m_nextId++;
        layer.kind = kind;
        layer.name = name.isEmpty() ? QString("Layer %1").arg(state.layers.size() + 1) : name;
        layer.pixels.size = state.size;
        layer.pixels.format = state.bitDepth == 16   ? QImage::Format_RGBA64
                              : state.bitDepth == 32 ? QImage::Format_RGBA32FPx4
                                                     : QImage::Format_RGBA8888;
        if (kind == LayerKind::Group || kind == LayerKind::Artboard)
            layer.blendMode = "Pass Through";
        const Layer *active = activeLayer();
        if (active)
            layer.parentId = active->kind == LayerKind::Group ? active->id : active->parentId;
        int index = std::clamp(state.activeIndex + 1, 0, int(state.layers.size()));
        state.layers.insert(index, layer);
        state.activeIndex = index;
    });
    emit activeLayerChanged();
    return id;
}
void Document::removeActiveLayer() {
    if (!activeLayer() || activeLayer()->locked)
        return;
    mutate("Delete Layer", [&] {
        QSet<quint64> removed{activeLayer()->id};
        bool added = true;
        while (added) {
            added = false;
            for (const auto &l : state.layers)
                if (removed.contains(l.parentId) && !removed.contains(l.id)) {
                    removed.insert(l.id);
                    added = true;
                }
        }
        state.layers.erase(std::remove_if(state.layers.begin(), state.layers.end(),
                                          [&](const Layer &l) { return removed.contains(l.id); }),
                           state.layers.end());
        state.activeIndex = std::min(state.activeIndex, int(state.layers.size()) - 1);
    });
    emit activeLayerChanged();
}
void Document::duplicateActiveLayer() {
    if (!activeLayer())
        return;
    mutate("Duplicate Layer", [&] {
        const Layer original = *activeLayer();
        QSet<quint64> ids{original.id};
        bool added = true;
        while (added) {
            added = false;
            for (const auto &l : state.layers)
                if (ids.contains(l.parentId) && !ids.contains(l.id)) {
                    ids.insert(l.id);
                    added = true;
                }
        }
        QHash<quint64, quint64> mapping;
        for (const auto &l : state.layers)
            m_nextId = std::max(m_nextId, l.id + 1);
        QVector<Layer> copies;
        for (const auto &l : state.layers)
            if (ids.contains(l.id)) {
                copies.append(l);
                mapping.insert(l.id, m_nextId++);
            }
        int position = state.activeIndex + 1;
        for (auto &l : copies) {
            l.id = mapping.value(l.id);
            if (mapping.contains(l.parentId))
                l.parentId = mapping.value(l.parentId);
            if (l.id == mapping.value(original.id))
                l.name += " copy";
            state.layers.insert(position++, l);
        }
        state.activeIndex = indexForId(mapping.value(original.id));
    });
    emit activeLayerChanged();
}
void Document::moveLayer(int from, int to) {
    if (from < 0 || to < 0 || from >= state.layers.size() || to >= state.layers.size() || from == to ||
        state.layers[from].locked)
        return;
    mutate("Reorder Layers", [&] {
        quint64 active = activeLayer() ? activeLayer()->id : 0;
        state.layers.move(from, to);
        state.activeIndex = indexForId(active);
    });
    emit activeLayerChanged();
}
void Document::mergeDown() {
    if (!activeLayer() || state.activeIndex < 1 || activeLayer()->locked)
        return;
    const int upper = state.activeIndex, lower = upper - 1;
    if (state.layers[lower].parentId != state.layers[upper].parentId ||
        state.layers[lower].kind == LayerKind::Group || state.layers[upper].kind == LayerKind::Group)
        return;
    mutate("Merge Down", [&] {
        DocumentState pair = state;
        pair.layers = {state.layers[lower], state.layers[upper]};
        for (auto &l : pair.layers)
            l.parentId = 0;
        Layer merged;
        merged.id = state.layers[lower].id;
        merged.name = state.layers[lower].name;
        merged.parentId = state.layers[lower].parentId;
        merged.pixels = TileImage::fromImage(compositeDocument(pair, blendLinear));
        state.layers[lower] = merged;
        state.layers.removeAt(upper);
        state.activeIndex = lower;
    });
    emit activeLayerChanged();
}
void Document::flatten() {
    if (state.layers.isEmpty())
        return;
    const QImage flattened = composite();
    mutate("Flatten Image", [&] {
        for (const auto &layer : state.layers)
            m_nextId = std::max(m_nextId, layer.id + 1);
        Layer l;
        l.id = m_nextId++;
        l.name = "Background";
        l.pixels = TileImage::fromImage(flattened);
        state.layers = {l};
        state.activeIndex = 0;
    });
    emit activeLayerChanged();
}
void Document::addMask() {
    if (!activeLayer() || activeLayer()->locked)
        return;
    mutate("Add Layer Mask", [&] {
        Layer &l = *activeLayer();
        QSize size = layerImage(l).size();
        if (size.isEmpty())
            size = state.size;
        l.mask = makeMask(size, state.bitDepth, 1);
        if (hasSelection()) {
            const QPoint origin = effectiveLayerOffset(l).toPoint();
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    const QPoint p = QPoint(x, y) + origin;
                    setMaskSample(l.mask, x, y, maskSample(state.selection, p.x(), p.y()));
                }
        }
        l.maskEnabled = true;
        l.maskTarget = true;
        l.maskDensity = 1;
        l.maskFeather = 0;
        l.maskLinked = true;
        l.maskOffset = {};
    });
}
void Document::removeMask(bool apply) {
    Layer *layer = activeLayer();
    if (!layer || layer->locked || layer->mask.isNull())
        return;
    mutate(apply ? "Apply Layer Mask" : "Delete Layer Mask", [&] {
        layer = activeLayer();
        if (apply) {
            QImage image = layerImage(*layer);
            Layer local = *layer;
            local.offset = {};
            const QImage coverage = renderedLayerMask(local, image.size(), state.bitDepth, true, false);
            if (!coverage.isNull())
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const qreal alpha = maskSample(coverage, x, y);
                        if (image.format() == QImage::Format_RGBA32FPx4) {
                            float *pixel = reinterpret_cast<float *>(image.scanLine(y)) + x * 4;
                            pixel[3] *= float(alpha);
                        } else if (image.format() == QImage::Format_RGBA64) {
                            auto *pixel = reinterpret_cast<QRgba64 *>(image.scanLine(y)) + x;
                            pixel->setAlpha(quint16(qRound(pixel->alpha() * alpha)));
                        } else
                            image.scanLine(y)[x * 4 + 3] =
                                uchar(qRound(image.constScanLine(y)[x * 4 + 3] * alpha));
                    }
            layer->pixels.setImage(image);
            if (layer->kind != LayerKind::Pixel)
                layer->kind = LayerKind::Pixel;
            layer->smartFilters = {};
            layer->parameters.remove("contentTransform");
        }
        layer->mask = {};
        layer->maskTarget = false;
        layer->maskEnabled = true;
        layer->maskDensity = 1;
        layer->maskFeather = 0;
    });
}
void Document::invertMask() {
    if (!activeLayer() || activeLayer()->locked || activeLayer()->mask.isNull())
        return;
    mutate("Invert Layer Mask", [&] {
        QImage &mask = activeLayer()->mask;
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                setMaskSample(mask, x, y, 1 - maskSample(mask, x, y));
    });
}
void Document::addVectorMask(const QPainterPath &path) {
    if (!activeLayer() || activeLayer()->locked)
        return;
    mutate("Add Vector Mask", [&] {
        activeLayer()->vectorMask = path;
        activeLayer()->vectorMaskEnabled = true;
        activeLayer()->vectorMaskDensity = 1;
        activeLayer()->vectorMaskFeather = 0;
    });
}
void Document::loadMaskSelection(bool vector, const QString &operation) {
    if (!activeLayer())
        return;
    Layer layer = *activeLayer();
    QSet<quint64> visited{layer.id};
    quint64 parent = layer.parentId;
    while (parent && !visited.contains(parent)) {
        visited.insert(parent);
        const int index = indexForId(parent);
        if (index < 0)
            break;
        layer.offset += state.layers[index].offset;
        parent = state.layers[index].parentId;
    }
    const QImage coverage = renderedLayerMask(layer, state.size, state.bitDepth, !vector, vector);
    if (!coverage.isNull())
        setSelection(coverage, operation);
}
void Document::convertToSmartObject() {
    if (!activeLayer() || activeLayer()->kind == LayerKind::SmartObject)
        return;
    convertLayerToEmbeddedSmartObject(this, activeLayer()->id);
}
void Document::addSmartFilter(const QString &name, const QJsonObject &parameters) {
    if (!activeLayer() || activeLayer()->locked)
        return;
    mutate("Smart Filter: " + name, [&] {
        convertToSmartObject();
        if (activeLayer()->kind == LayerKind::SmartObject)
            activeLayer()->smartFilters.append(QJsonObject{{"name", name},
                                                           {"parameters", parameters},
                                                           {"enabled", true},
                                                           {"opacity", 1.},
                                                           {"blendMode", "Normal"}});
    });
}
bool Document::updateSmartFilter(int index, const QJsonObject &entry) {
    if (!activeLayer() || activeLayer()->locked || index < 0 || index >= activeLayer()->smartFilters.size())
        return false;
    mutate("Edit Smart Filter", [&] { activeLayer()->smartFilters[index] = entry; });
    return true;
}
bool Document::removeSmartFilter(int index) {
    if (!activeLayer() || activeLayer()->locked || index < 0 || index >= activeLayer()->smartFilters.size())
        return false;
    mutate("Remove Smart Filter", [&] { activeLayer()->smartFilters.removeAt(index); });
    return true;
}
void Document::addAdjustment(const QString &name, const QJsonObject &params) {
    mutate(name, [&] {
        addLayer(name, LayerKind::Adjustment);
        activeLayer()->adjustment = name;
        activeLayer()->parameters = params;
    });
}
void Document::beginTransaction(const QString &name) {
    if (m_inTransaction)
        return;
    m_before = state;
    m_beforeRevision = m_revision;
    m_transactionName = name;
    m_transactionChanged = false;
    m_inTransaction = true;
}
void Document::endTransaction() {
    if (!m_inTransaction)
        return;
    m_inTransaction = false;
    if (!m_transactionChanged) {
        m_before = DocumentState();
        return;
    }
    m_history.resize(m_historyCursor);
    const quint64 next = m_nextRevision++;
    m_history.append({m_transactionName, m_before, state, m_beforeRevision, next});
    m_revision = next;
    if (m_history.size() > std::max(1, historyLimit))
        m_history.remove(0, m_history.size() - std::max(1, historyLimit));
    m_historyCursor = m_history.size();
    m_before = DocumentState();
    emit historyChanged();
    emit changed();
}
void Document::cancelTransaction() {
    if (!m_inTransaction)
        return;
    state = m_before;
    m_revision = m_beforeRevision;
    m_inTransaction = false;
    m_transactionChanged = false;
    m_before = DocumentState();
    m_dirty = true;
    emit changed();
    emit activeLayerChanged();
    emit historyChanged();
}
void Document::mutate(const QString &name, const std::function<void()> &operation) {
    const bool outer = m_inTransaction;
    if (!outer)
        beginTransaction(name);
    operation();
    touch();
    if (!outer)
        endTransaction();
}
void Document::touch() {
    m_dirty = true;
    if (m_inTransaction)
        m_transactionChanged = true;
    else
        m_revision = m_nextRevision++;
    emit changed();
}
void Document::undo() {
    if (m_inTransaction)
        cancelTransaction();
    if (!canUndo())
        return;
    const auto &entry = m_history[--m_historyCursor];
    state = entry.before;
    m_revision = entry.beforeRevision;
    m_dirty = true;
    emit changed();
    emit activeLayerChanged();
    emit historyChanged();
}
void Document::redo() {
    if (m_inTransaction)
        cancelTransaction();
    if (!canRedo())
        return;
    const auto &entry = m_history[m_historyCursor++];
    state = entry.after;
    m_revision = entry.afterRevision;
    m_dirty = true;
    emit changed();
    emit activeLayerChanged();
    emit historyChanged();
}
bool Document::canUndo() const { return m_historyCursor > 0; }
bool Document::canRedo() const { return m_historyCursor < m_history.size(); }
QStringList Document::historyNames() const {
    QStringList result;
    for (int i = 0; i < m_historyCursor; ++i)
        result.append(m_history[i].name);
    return result;
}
void Document::clearHistory() {
    if (m_inTransaction)
        endTransaction();
    m_history.clear();
    m_historyCursor = 0;
    emit historyChanged();
    emit changed();
}
void Document::markSaved() {
    m_savedRevision = m_revision;
    emit changed();
}
bool Document::isModified() const {
    return m_revision != m_savedRevision || (m_inTransaction && m_transactionChanged);
}
QImage Document::composite() const {
    if (!m_dirty && m_cachedLinear == blendLinear)
        return m_composite;
    bool regional = m_inTransaction && !m_composite.isNull() && m_cachedLinear == blendLinear &&
                    m_composite.size() == state.size && m_before.size == state.size &&
                    m_before.bitDepth == state.bitDepth && m_before.iccProfile == state.iccProfile &&
                    m_before.layers.size() == state.layers.size();
    QRect dirty;
    if (regional)
        for (int i = 0; i < state.layers.size(); ++i) {
            const Layer &before = m_before.layers[i], &after = state.layers[i];
            const bool raster = after.kind == LayerKind::Pixel || after.kind == LayerKind::SmartObject;
            if (!raster || before.kind != after.kind || before.id != after.id ||
                before.visible != after.visible || before.offset != after.offset ||
                before.opacity != after.opacity || before.fill != after.fill ||
                before.blendMode != after.blendMode || after.blendMode == "Dissolve" ||
                before.clipped != after.clipped || before.parentId != after.parentId || after.parentId ||
                !before.effects.isEmpty() || !after.effects.isEmpty() ||
                before.mask.cacheKey() != after.mask.cacheKey() || before.maskEnabled != after.maskEnabled ||
                before.maskDensity != after.maskDensity || before.maskFeather != after.maskFeather ||
                before.maskLinked != after.maskLinked || before.maskOffset != after.maskOffset ||
                after.maskFeather > 0 || !after.maskLinked || !before.vectorMask.isEmpty() ||
                !after.vectorMask.isEmpty() || !before.smartFilters.isEmpty() ||
                !after.smartFilters.isEmpty() || before.parameters != after.parameters ||
                after.parameters.contains("contentTransform") || before.pixels.size != after.pixels.size ||
                before.pixels.format != after.pixels.format ||
                after.offset != QPointF(after.offset.toPoint())) {
                regional = false;
                break;
            }
            QSet<quint64> keys;
            for (auto it = before.pixels.tiles.cbegin(); it != before.pixels.tiles.cend(); ++it)
                keys.insert(it.key());
            for (auto it = after.pixels.tiles.cbegin(); it != after.pixels.tiles.cend(); ++it)
                keys.insert(it.key());
            for (quint64 key : keys) {
                const auto old = before.pixels.tiles.constFind(key), now = after.pixels.tiles.constFind(key);
                if (old != before.pixels.tiles.cend() && now != after.pixels.tiles.cend() &&
                    old.value().cacheKey() == now.value().cacheKey())
                    continue;
                const int tx = int(quint32(key >> 32)), ty = int(quint32(key));
                dirty = dirty.united(
                    QRect(QPoint(tx * TileImage::TileSize, ty * TileImage::TileSize) + after.offset.toPoint(),
                          QSize(TileImage::TileSize, TileImage::TileSize)));
            }
        }
    if (regional) {
        dirty = dirty.intersected(QRect(QPoint(), state.size));
        if (!dirty.isEmpty()) {
            DocumentState region = state;
            region.size = dirty.size();
            for (Layer &layer : region.layers) {
                const QRect local = dirty.translated(-layer.offset.toPoint());
                layer.pixels = TileImage::fromImage(layer.pixels.region(local));
                if (!layer.mask.isNull())
                    layer.mask = layer.mask.copy(local);
                layer.offset = {};
            }
            const QImage patch = compositeDocument(region, blendLinear);
            const int pixelBytes = m_composite.depth() / 8;
            for (int y = 0; y < dirty.height(); ++y)
                std::memcpy(m_composite.scanLine(dirty.y() + y) + dirty.x() * pixelBytes,
                            patch.constScanLine(y), dirty.width() * pixelBytes);
        }
    } else
        m_composite = compositeDocument(state, blendLinear);
    m_dirty = false;
    m_cachedLinear = blendLinear;
    return m_composite;
}
QImage Document::layerImage(const Layer &layer) const {
    return renderLayerImage(layer, state.size, state.bitDepth);
}
QRect Document::selectionBounds() const {
    const qint64 key = state.selection.cacheKey();
    if (key == m_selectionCacheKey)
        return m_cachedSelectionBounds;
    m_selectionCacheKey = key;
    if (state.selection.isNull()) {
        m_cachedSelectionBounds = {};
        return {};
    }
    const QImage &mask = state.selection;
    int left = mask.width(), top = mask.height(), right = -1, bottom = -1;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x)
            if (maskSample(mask, x, y) > 0) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
    m_cachedSelectionBounds = right < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
    return m_cachedSelectionBounds;
}
bool Document::hasSelection() const { return !selectionBounds().isEmpty(); }
void Document::setSelection(const QImage &mask, const QString &operation) {
    mutate("Selection", [&] {
        const int depth = state.bitDepth == 32 || mask.format() == QImage::Format_RGBA32FPx4 ||
                                  state.selection.format() == QImage::Format_RGBA32FPx4
                              ? 32
                          : state.bitDepth > 8 || mask.format() == QImage::Format_Grayscale16 ||
                                  state.selection.format() == QImage::Format_Grayscale16
                              ? 16
                              : 8;
        QImage incoming = maskAtSize(mask, state.size, depth);
        if (operation == "replace" || (state.selection.isNull() && operation == "add")) {
            state.selection = incoming;
            return;
        }
        QImage old = maskAtSize(state.selection, state.size, depth);
        // Integer masks combine row by row; max/min/difference give the same values as the float
        // path below, which per-pixel helpers made take about a second per operation on large images.
        const auto combine = [&](auto sample) {
            using T = decltype(sample);
            for (int y = 0; y < state.size.height(); ++y) {
                T *row = reinterpret_cast<T *>(old.scanLine(y));
                const T *in = reinterpret_cast<const T *>(incoming.constScanLine(y));
                for (int x = 0; x < state.size.width(); ++x) {
                    const int a = row[x], b = in[x];
                    row[x] = T(operation == "add"         ? std::max(a, b)
                               : operation == "subtract"  ? std::max(0, a - b)
                               : operation == "intersect" ? std::min(a, b)
                               : operation == "xor"       ? std::abs(a - b)
                                                          : b);
                }
            }
        };
        if (old.format() == incoming.format() && old.format() == QImage::Format_Grayscale8) {
            combine(uchar());
            state.selection = old;
            return;
        }
        if (old.format() == incoming.format() && old.format() == QImage::Format_Grayscale16) {
            combine(quint16());
            state.selection = old;
            return;
        }
        for (int y = 0; y < state.size.height(); ++y)
            for (int x = 0; x < state.size.width(); ++x) {
                const qreal a = maskSample(old, x, y), b = maskSample(incoming, x, y);
                qreal c = b;
                if (operation == "add")
                    c = std::max(a, b);
                else if (operation == "subtract")
                    c = std::max(0., a - b);
                else if (operation == "intersect")
                    c = std::min(a, b);
                else if (operation == "xor")
                    c = std::abs(a - b);
                setMaskSample(old, x, y, c);
            }
        state.selection = old;
    });
}
void Document::selectAll() {
    QImage mask = makeMask(state.size, state.bitDepth, 1);
    setSelection(mask);
}
void Document::deselect() {
    if (state.selection.isNull())
        return;
    mutate("Deselect", [&] {
        const auto channels = state.metadata.value("selectionChannels");
        const QString key = "__last_selection__";
        saveSelection(key);
        state.metadata["lastSelectionPrecision"] = state.metadata["selectionChannels"].toObject()[key];
        if (channels.isUndefined())
            state.metadata.remove("selectionChannels");
        else
            state.metadata["selectionChannels"] = channels;
        state.selection = {};
    });
}
bool Document::reselect() {
    const auto value = state.metadata.value("lastSelectionPrecision");
    if (value.isUndefined())
        return false;
    bool loaded = false;
    mutate("Reselect", [&] {
        const auto original = state.metadata.value("selectionChannels");
        auto channels = original.toObject();
        channels["__last_selection__"] = value;
        state.metadata["selectionChannels"] = channels;
        loaded = loadSelection("__last_selection__");
        if (original.isUndefined())
            state.metadata.remove("selectionChannels");
        else
            state.metadata["selectionChannels"] = original;
    });
    return loaded;
}
void Document::invertSelection() {
    mutate("Inverse Selection", [&] {
        QImage mask = maskAtSize(state.selection, state.size, state.bitDepth);
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                setMaskSample(mask, x, y, 1 - maskSample(mask, x, y));
        state.selection = mask;
    });
}
void Document::refineSelection(const QJsonObject &parameters) {
    if (state.selection.isNull())
        return;
    mutate("Refine Selection", [&] { state.selection = refineMask(state.selection, parameters); });
}
void Document::saveSelection(const QString &name) {
    if (name.trimmed().isEmpty() || state.selection.isNull())
        return;
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_8);
    const QImage mask = normalizeMask(state.selection);
    stream << quint32(0x534b5343) << qint32(mask.format()) << qint32(mask.width()) << qint32(mask.height())
           << qint32(mask.bytesPerLine())
           << QByteArray(reinterpret_cast<const char *>(mask.constBits()), mask.sizeInBytes());
    mutate("Save Selection", [&] {
        QJsonObject channels = state.metadata.value("selectionChannels").toObject();
        channels[name] = QString::fromLatin1(qCompress(bytes).toBase64());
        state.metadata["selectionChannels"] = channels;
    });
}
bool Document::loadSelection(const QString &name, const QString &operation) {
    const QJsonObject channels = state.metadata.value("selectionChannels").toObject();
    if (!channels.contains(name))
        return false;
    const QByteArray encoded = QByteArray::fromBase64(channels.value(name).toString().toLatin1());
    QByteArray bytes = qUncompress(encoded);
    QDataStream stream(&bytes, QIODevice::ReadOnly);
    stream.setVersion(QDataStream::Qt_6_8);
    QImage image;
    quint32 magic = 0;
    qint32 format = 0, width = 0, height = 0, stride = 0;
    QByteArray pixels;
    stream >> magic >> format >> width >> height >> stride >> pixels;
    if (magic != 0x534b5343 || width < 1 || height < 1 || qint64(width) * height > 300000000 ||
        (format != QImage::Format_Grayscale8 && format != QImage::Format_Grayscale16 &&
         format != QImage::Format_RGBA32FPx4) ||
        stream.status() != QDataStream::Ok)
        return false;
    image = QImage(width, height, QImage::Format(format));
    if (image.isNull() || image.bytesPerLine() != stride || image.sizeInBytes() != pixels.size())
        return false;
    std::memcpy(image.bits(), pixels.constData(), size_t(pixels.size()));
    setSelection(image, operation);
    return true;
}
QStringList Document::savedSelectionNames() const {
    return state.metadata.value("selectionChannels").toObject().keys();
}
QJsonArray Document::layerComps() const { return state.metadata.value("layerComps").toArray(); }
int Document::captureLayerComp(const QString &name, bool visibility, bool position, bool appearance) {
    if (name.trimmed().isEmpty())
        return -1;
    int index = -1;
    mutate("Capture Layer Comp", [&] {
        QJsonArray layers;
        for (const Layer &layer : state.layers) {
            QJsonObject entry{{"id", QString::number(layer.id)}};
            if (visibility)
                entry["visible"] = layer.visible;
            if (position) {
                entry["offset"] = QJsonArray{layer.offset.x(), layer.offset.y()};
                entry["maskOffset"] = QJsonArray{layer.maskOffset.x(), layer.maskOffset.y()};
                entry["vectorMaskOffset"] =
                    QJsonArray{layer.vectorMaskOffset.x(), layer.vectorMaskOffset.y()};
            }
            if (appearance) {
                entry["opacity"] = layer.opacity;
                entry["fill"] = layer.fill;
                entry["blendMode"] = layer.blendMode;
                entry["effects"] = layer.effects;
                entry["parameters"] = layer.parameters;
                entry["smartFilters"] = layer.smartFilters;
                entry["maskEnabled"] = layer.maskEnabled;
                entry["maskDensity"] = layer.maskDensity;
                entry["maskFeather"] = layer.maskFeather;
                entry["vectorMaskEnabled"] = layer.vectorMaskEnabled;
                entry["vectorMaskDensity"] = layer.vectorMaskDensity;
                entry["vectorMaskFeather"] = layer.vectorMaskFeather;
            }
            layers.append(entry);
        }
        QJsonArray comps = layerComps();
        index = comps.size();
        comps.append(QJsonObject{{"name", name},
                                 {"visibility", visibility},
                                 {"position", position},
                                 {"appearance", appearance},
                                 {"layers", layers}});
        state.metadata["layerComps"] = comps;
    });
    return index;
}
bool Document::applyLayerComp(int index) {
    const QJsonArray comps = layerComps();
    if (index < 0 || index >= comps.size())
        return false;
    const QJsonObject comp = comps[index].toObject();
    mutate("Apply Layer Comp: " + comp.value("name").toString(), [&] {
        for (const auto &value : comp.value("layers").toArray()) {
            const QJsonObject entry = value.toObject();
            const int layerIndex = indexForId(entry.value("id").toString().toULongLong());
            if (layerIndex < 0)
                continue;
            Layer &layer = state.layers[layerIndex];
            if (comp.value("visibility").toBool())
                layer.visible = entry.value("visible").toBool(true);
            if (comp.value("position").toBool()) {
                auto point = [&](const QString &key) {
                    const QJsonArray a = entry.value(key).toArray();
                    return a.size() == 2 ? QPointF(a[0].toDouble(), a[1].toDouble()) : QPointF();
                };
                layer.offset = point("offset");
                layer.maskOffset = point("maskOffset");
                layer.vectorMaskOffset = point("vectorMaskOffset");
            }
            if (comp.value("appearance").toBool()) {
                layer.opacity = entry.value("opacity").toDouble(1);
                layer.fill = entry.value("fill").toDouble(1);
                layer.blendMode = entry.value("blendMode").toString("Normal");
                layer.effects = entry.value("effects").toObject();
                layer.parameters = entry.value("parameters").toObject();
                layer.smartFilters = entry.value("smartFilters").toArray();
                layer.maskEnabled = entry.value("maskEnabled").toBool(true);
                layer.maskDensity = entry.value("maskDensity").toDouble(1);
                layer.maskFeather = entry.value("maskFeather").toDouble();
                layer.vectorMaskEnabled = entry.value("vectorMaskEnabled").toBool(true);
                layer.vectorMaskDensity = entry.value("vectorMaskDensity").toDouble(1);
                layer.vectorMaskFeather = entry.value("vectorMaskFeather").toDouble();
            }
        }
    });
    return true;
}
bool Document::deleteLayerComp(int index) {
    QJsonArray comps = layerComps();
    if (index < 0 || index >= comps.size())
        return false;
    mutate("Delete Layer Comp", [&] {
        comps.removeAt(index);
        state.metadata["layerComps"] = comps;
    });
    return true;
}
QImage Document::historyLayerImage(quint64 layerId, int historyIndex) const {
    const DocumentState *snapshot = nullptr;
    if (historyIndex == -1)
        snapshot = m_history.isEmpty() ? &state : &m_history.first().before;
    else if (historyIndex >= 0 && historyIndex < m_history.size())
        snapshot = &m_history[historyIndex].after;
    if (!snapshot)
        return {};
    for (const Layer &layer : snapshot->layers)
        if (layer.id == layerId)
            return renderLayerImage(layer, snapshot->size, snapshot->bitDepth);
    return {};
}
bool Document::restoreHistoryPixels(quint64 layerId, const QImage &coverage, int historyIndex) {
    const int index = indexForId(layerId);
    if (index < 0 || state.layers[index].locked || state.layers[index].kind != LayerKind::Pixel)
        return false;
    const QImage source = historyLayerImage(layerId, historyIndex);
    if (source.isNull())
        return false;
    mutate("History Brush", [&] {
        Layer &layer = state.layers[index];
        QImage target = layer.pixels.image();
        const QImage native = source.convertToFormat(target.format());
        const QPoint origin = effectiveLayerOffset(layer).toPoint();
        for (int y = 0; y < target.height(); ++y)
            for (int x = 0; x < target.width(); ++x) {
                if (!native.rect().contains(x, y))
                    continue;
                const QPoint point = QPoint(x, y) + origin;
                const qreal weight = maskSample(coverage, point.x(), point.y());
                if (!weight)
                    continue;
                if (target.format() == QImage::Format_RGBA32FPx4) {
                    float *a = reinterpret_cast<float *>(target.scanLine(y)) + x * 4;
                    const float *b = reinterpret_cast<const float *>(native.constScanLine(y)) + x * 4;
                    for (int c = 0; c < 4; ++c)
                        a[c] = float(a[c] * (1 - weight) + b[c] * weight);
                } else if (target.format() == QImage::Format_RGBA64) {
                    auto *a = reinterpret_cast<QRgba64 *>(target.scanLine(y)) + x;
                    const auto b = reinterpret_cast<const QRgba64 *>(native.constScanLine(y))[x];
                    *a = QRgba64::fromRgba64(qRound(a->red() * (1 - weight) + b.red() * weight),
                                             qRound(a->green() * (1 - weight) + b.green() * weight),
                                             qRound(a->blue() * (1 - weight) + b.blue() * weight),
                                             qRound(a->alpha() * (1 - weight) + b.alpha() * weight));
                } else {
                    uchar *a = target.scanLine(y) + x * 4;
                    const uchar *b = native.constScanLine(y) + x * 4;
                    for (int c = 0; c < 4; ++c)
                        a[c] = uchar(qRound(a[c] * (1 - weight) + b[c] * weight));
                }
            }
        layer.pixels.setImage(target);
    });
    return true;
}
void Document::resizeImage(QSize newSize, Qt::TransformationMode mode) {
    if (newSize.isEmpty() || newSize == state.size)
        return;
    mutate("Image Size", [&] {
        const qreal sx = qreal(newSize.width()) / state.size.width(),
                    sy = qreal(newSize.height()) / state.size.height();
        for (auto &l : state.layers) {
            if (!l.pixels.size.isEmpty()) {
                const QSize pixelSize(std::max(1, qRound(l.pixels.size.width() * sx)),
                                      std::max(1, qRound(l.pixels.size.height() * sy)));
                if (l.kind == LayerKind::SmartObject) {
                    const QJsonArray matrix = l.parameters.value("contentTransform").toArray();
                    QTransform existing;
                    if (matrix.size() == 9)
                        existing =
                            QTransform(matrix[0].toDouble(), matrix[1].toDouble(), matrix[2].toDouble(),
                                       matrix[3].toDouble(), matrix[4].toDouble(), matrix[5].toDouble(),
                                       matrix[6].toDouble(), matrix[7].toDouble(), matrix[8].toDouble());
                    QTransform scale;
                    scale.scale(sx, sy);
                    const QTransform transform =
                        QImage::trueMatrix(existing, l.pixels.size.width(), l.pixels.size.height()) * scale;
                    l.parameters["contentTransform"] = QJsonArray{
                        transform.m11(), transform.m12(), transform.m13(), transform.m21(), transform.m22(),
                        transform.m23(), transform.m31(), transform.m32(), transform.m33()};
                } else if (l.pixels.empty())
                    l.pixels.size = pixelSize;
                else
                    l.pixels.setImage(l.pixels.image().scaled(pixelSize, Qt::IgnoreAspectRatio, mode));
            }
            if (!l.mask.isNull())
                l.mask = l.mask.scaled(QSize(std::max(1, qRound(l.mask.width() * sx)),
                                             std::max(1, qRound(l.mask.height() * sy))),
                                       Qt::IgnoreAspectRatio, mode);
            l.offset = QPointF(l.offset.x() * sx, l.offset.y() * sy);
            QTransform transform;
            transform.scale(sx, sy);
            l.shape = transform.map(l.shape);
            l.vectorMask = transform.map(l.vectorMask);
            l.maskOffset = QPointF(l.maskOffset.x() * sx, l.maskOffset.y() * sy);
            l.vectorMaskOffset = QPointF(l.vectorMaskOffset.x() * sx, l.vectorMaskOffset.y() * sy);
            l.maskFeather *= std::sqrt(sx * sy);
            l.vectorMaskFeather *= std::sqrt(sx * sy);
            l.strokeWidth *= std::sqrt(sx * sy);
            if (l.font.pixelSize() > 0)
                l.font.setPixelSize(std::max(1, qRound(l.font.pixelSize() * sy)));
            else if (l.font.pointSizeF() > 0)
                l.font.setPointSizeF(l.font.pointSizeF() * sy);
        }
        if (!state.selection.isNull())
            state.selection = state.selection.scaled(newSize, Qt::IgnoreAspectRatio, mode);
        for (auto &g : state.guides)
            g.position *= g.vertical ? sx : sy;
        state.size = newSize;
    });
}
void Document::resizeCanvas(QSize newSize, QPoint anchorOffset) {
    if (newSize.isEmpty() || (newSize == state.size && anchorOffset.isNull()))
        return;
    mutate("Canvas Size", [&] {
        for (auto &l : state.layers)
            if (!l.parentId)
                l.offset += anchorOffset;
        if (!state.selection.isNull()) {
            QImage selection =
                makeMask(newSize, state.selection.format() == QImage::Format_RGBA32FPx4    ? 32
                                  : state.selection.format() == QImage::Format_Grayscale16 ? 16
                                                                                           : 8);
            const QImage &old = state.selection;
            for (int y = 0; y < old.height(); ++y)
                for (int x = 0; x < old.width(); ++x) {
                    QPoint p = QPoint(x, y) + anchorOffset;
                    if (selection.rect().contains(p))
                        setMaskSample(selection, p.x(), p.y(), maskSample(old, x, y));
                }
            state.selection = selection;
        }
        for (auto &g : state.guides)
            g.position += g.vertical ? anchorOffset.x() : anchorOffset.y();
        state.size = newSize;
    });
}
void Document::crop(const QRect &rect, bool deletePixels) {
    if (rect.isEmpty())
        return;
    mutate("Crop", [&] {
        resizeCanvas(rect.size(), -rect.topLeft());
        if (deletePixels)
            for (Layer &layer : state.layers) {
                if (layer.kind != LayerKind::Pixel && layer.kind != LayerKind::SmartObject)
                    continue;
                QPointF effective = layer.offset;
                quint64 parent = layer.parentId;
                QSet<quint64> visited{layer.id};
                while (parent && !visited.contains(parent)) {
                    visited.insert(parent);
                    const int index = indexForId(parent);
                    if (index < 0)
                        break;
                    effective += state.layers[index].offset;
                    parent = state.layers[index].parentId;
                }
                const QRect bounds = QRect(QPoint(), state.size).translated(-effective.toPoint());
                // Retain the local extent and clear discarded samples so vector/raster-mask alignment
                // and future transforms continue to use the same coordinate system.
                QImage image = layer.pixels.image();
                QTransform content;
                const QJsonArray matrix = layer.parameters.value("contentTransform").toArray();
                if (matrix.size() == 9) {
                    content = QTransform(matrix[0].toDouble(), matrix[1].toDouble(), matrix[2].toDouble(),
                                         matrix[3].toDouble(), matrix[4].toDouble(), matrix[5].toDouble(),
                                         matrix[6].toDouble(), matrix[7].toDouble(), matrix[8].toDouble());
                    content = QImage::trueMatrix(content, image.width(), image.height());
                }
                const int pixelBytes = image.depth() / 8;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x)
                        if (matrix.size() == 9
                                ? !QRectF(QPointF(), state.size)
                                       .contains(content.map(QPointF(x + .5, y + .5)) + effective)
                                : !bounds.contains(x, y))
                            std::memset(image.scanLine(y) + x * pixelBytes, 0, pixelBytes);
                layer.pixels.setImage(image);
            }
        deselect();
    });
}
} // namespace serika
