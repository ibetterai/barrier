#pragma once

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

#include <map>
#include <utility>

class QSettings;

namespace barrier {

using FreeformPositions = std::map<QString, std::pair<int, int>>;
using FreeformDisplayRects = std::map<QString, QList<QRect>>;
using FreeformDisplayNames = std::map<QString, QStringList>;

void saveFreeformLayoutSettings(QSettings& settings,
                                const FreeformPositions& positions,
                                const FreeformDisplayRects& displayRects,
                                const FreeformDisplayNames& displayNames);

void loadFreeformLayoutSettings(QSettings& settings,
                                FreeformPositions& positions,
                                FreeformDisplayRects& displayRects,
                                FreeformDisplayNames& displayNames);

bool parseClientDisplayRectsLogLine(const QString& line,
                                    QString& clientName,
                                    QList<QRect>& rects);

} // namespace barrier
