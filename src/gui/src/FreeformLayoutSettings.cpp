#include "FreeformLayoutSettings.h"

#include <QSettings>

namespace barrier {

void saveFreeformLayoutSettings(QSettings& settings,
                                const FreeformPositions& positions,
                                const FreeformDisplayRects& displayRects,
                                const FreeformDisplayNames& displayNames)
{
    settings.beginWriteArray("freeformPositions");
    int index = 0;
    for (FreeformPositions::const_iterator it = positions.begin();
         it != positions.end(); ++it, ++index) {
        settings.setArrayIndex(index);
        settings.setValue("name", it->first);
        settings.setValue("x", it->second.first);
        settings.setValue("y", it->second.second);
    }
    settings.endArray();

    settings.beginWriteArray("freeformDisplayRects");
    index = 0;
    for (FreeformDisplayRects::const_iterator it = displayRects.begin();
         it != displayRects.end(); ++it, ++index) {
        settings.setArrayIndex(index);
        settings.setValue("name", it->first);
        settings.beginWriteArray("rects");
        for (int rectIndex = 0; rectIndex < it->second.size(); ++rectIndex) {
            const QRect& rect = it->second[rectIndex];
            settings.setArrayIndex(rectIndex);
            settings.setValue("x", rect.x());
            settings.setValue("y", rect.y());
            settings.setValue("w", rect.width());
            settings.setValue("h", rect.height());
        }
        settings.endArray();
    }
    settings.endArray();

    settings.beginWriteArray("freeformDisplayNames");
    index = 0;
    for (FreeformDisplayNames::const_iterator it = displayNames.begin();
         it != displayNames.end(); ++it, ++index) {
        settings.setArrayIndex(index);
        settings.setValue("name", it->first);
        settings.setValue("names", it->second);
    }
    settings.endArray();
}

void loadFreeformLayoutSettings(QSettings& settings,
                                FreeformPositions& positions,
                                FreeformDisplayRects& displayRects,
                                FreeformDisplayNames& displayNames)
{
    positions.clear();
    int count = settings.beginReadArray("freeformPositions");
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        positions[settings.value("name").toString()] = std::make_pair(
                settings.value("x").toInt(), settings.value("y").toInt());
    }
    settings.endArray();

    displayRects.clear();
    count = settings.beginReadArray("freeformDisplayRects");
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        const QString name = settings.value("name").toString();
        QList<QRect> rects;
        const int rectCount = settings.beginReadArray("rects");
        for (int rectIndex = 0; rectIndex < rectCount; ++rectIndex) {
            settings.setArrayIndex(rectIndex);
            rects.append(QRect(settings.value("x").toInt(),
                               settings.value("y").toInt(),
                               settings.value("w").toInt(),
                               settings.value("h").toInt()));
        }
        settings.endArray();
        displayRects[name] = rects;
    }
    settings.endArray();

    displayNames.clear();
    count = settings.beginReadArray("freeformDisplayNames");
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        displayNames[settings.value("name").toString()] =
                settings.value("names").toStringList();
    }
    settings.endArray();
}

} // namespace barrier
