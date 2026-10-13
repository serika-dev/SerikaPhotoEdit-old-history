// Renders every tool glyph at sidebar size and enlarged, for visual review of icon changes.
// Usage: serika_iconsheet <sheet.png> [primary]   (primary = one icon per toolbar slot)
#include "ui/canvas/CanvasView.h"
#include "ui/shortcuts/ShortcutRegistry.h"
#include <QApplication>
#include <QPainter>
#include <cstdio>
using namespace serika;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: serika_iconsheet <sheet.png> [primary]\n");
        return 2;
    }
    QStringList names;
    for (const auto &group : ShortcutRegistry::toolGroups())
        names += group.second;
    const bool primaryOnly = argc > 2 && QString(argv[2]) == "primary";
    if (primaryOnly) {
        names.clear();
        for (const auto &group : ShortcutRegistry::toolGroups())
            names += group.second.first();
    }
    const int columns = 8, cell = 150;
    const int rows = (names.size() + columns - 1) / columns;
    QImage sheet(columns * cell, rows * cell, QImage::Format_ARGB32);
    sheet.fill(QColor("#1E1B26"));
    QPainter painter(&sheet);
    painter.setPen(QColor("#B8B0C8"));
    QFont font = painter.font();
    font.setPixelSize(11);
    painter.setFont(font);
    for (int i = 0; i < names.size(); ++i) {
        const QRect r((i % columns) * cell, (i / columns) * cell, cell, cell);
        toolIcon(names[i], QColor("#E8E4F0")).paint(&painter, QRect(r.x() + 25, r.y() + 10, 100, 100));
        toolIcon(names[i], QColor("#E8E4F0")).paint(&painter, QRect(r.x() + 8, r.y() + 112, 20, 20));
        painter.drawText(QRect(r.x() + 32, r.y() + 112, cell - 34, 30),
                         Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, names[i]);
    }
    painter.end();
    sheet.save(argv[1]);
    return 0;
}
