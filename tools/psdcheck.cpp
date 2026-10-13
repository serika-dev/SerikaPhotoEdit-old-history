// Loads a file through Serika's importers, prints the layer tree and import report, and can save
// the composite. Compare it with scripts/psd_inspect.py, which reads the raw records and Photoshop's
// own embedded composite. Usage: serika_psdcheck <file.psd> [composite.png]
#include "io/FormatIO.h"
#include <QApplication>
#include <QHash>
#include <QTextStream>
#include <functional>
using namespace serika;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTextStream out(stdout);
    if (argc < 2) {
        out << "usage: serika_psdcheck <file.psd> [composite.png]\n";
        return 2;
    }
    QString error;
    Document *doc = FormatIO::open(QString::fromLocal8Bit(argv[1]), &error);
    if (!doc) {
        out << "OPEN FAILED: " << error << "\n";
        return 1;
    }
    out << "size " << doc->state.size.width() << "x" << doc->state.size.height() << " layers "
        << doc->state.layers.size() << "\n";
    QHash<quint64, QVector<int>> children;
    for (int i = doc->state.layers.size() - 1; i >= 0; --i)
        children[doc->state.layers[i].parentId].append(i);
    std::function<void(quint64, int)> dump = [&](quint64 parent, int depth) {
        for (int i : children.value(parent)) {
            const auto &l = doc->state.layers[i];
            out << QString(depth * 2, ' ') << "[" << i << "] " << l.name << "  kind=" << int(l.kind)
                << " vis=" << l.visible << " op=" << l.opacity << " fill=" << l.fill
                << " blend=" << l.blendMode << " clip=" << l.clipped << " mask=" << !l.mask.isNull()
                << " off=" << l.offset.x() << "," << l.offset.y() << " px=" << l.pixels.size.width() << "x"
                << l.pixels.size.height() << (l.adjustment.isEmpty() ? QString() : " adj=" + l.adjustment)
                << "\n";
            dump(l.id, depth + 1);
        }
    };
    dump(0, 0);
    out << "--- report (" << doc->importReport.size() << " lines)\n";
    for (const auto &line : doc->importReport)
        out << line << "\n";
    if (argc > 2)
        doc->composite().save(QString::fromLocal8Bit(argv[2]));
    return 0;
}
