#pragma once
#include <QImage>
#include <QJsonObject>
#include <memory>

namespace serika {
class Document;
// Local processing hooks: model-backed implementations can replace these without a network service.
class ISubjectSelector {
  public:
    virtual ~ISubjectSelector() = default;
    virtual QImage select(const QImage &image) const = 0;
};
class ColorModelSubjectSelector final : public ISubjectSelector {
  public:
    QImage select(const QImage &image) const override;
};
using LocalSubjectSelector = ColorModelSubjectSelector;
void registerSubjectSelector(std::shared_ptr<ISubjectSelector> selector);
QImage selectSubjectLocally(const QImage &image);
// Subject/background engines. Fast is the low-resource colour model and always produces a mask;
// Ai runs the optional local ONNX model and returns a null image with an error when unavailable.
enum class SubjectEngine { Fast, Ai };
QString subjectEngineId(SubjectEngine engine);
bool parseSubjectEngine(const QString &id, SubjectEngine *engine);
SubjectEngine preferredSubjectEngine();
void setPreferredSubjectEngine(SubjectEngine engine);
QImage selectSubject(const QImage &image, SubjectEngine engine, QString *error = nullptr);
// Remove Background works on the layer's own pixels (like Photoshop), not on the document composite,
// where a placed photo's whole frame would read as the subject. layerSubjectSource() is what to
// segment; applyBackgroundMask() turns that layer-local subject mask into the layer's mask in one
// undo step, multiplied with any existing mask.
QImage layerSubjectSource(const Document *document, int layerIndex);
bool applyBackgroundMask(Document *document, int layerIndex, const QImage &subject, QString *error = nullptr);
QImage selectFocusAreaLocally(const QImage &image, const QJsonObject &parameters = {});
QImage synthesizePatches(const QImage &source, const QImage &mask);
} // namespace serika
