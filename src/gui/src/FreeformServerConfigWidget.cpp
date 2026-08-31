#include "FreeformServerConfigWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QApplication>
#include <QScreen>

#ifdef Q_OS_MAC
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#endif

#ifdef Q_OS_MAC
namespace {
// Real monitor product name (e.g. "Studio Display", "LG ULTRAFINE") for a
// CGDirectDisplayID, matched via IOKit's IODisplayConnect services by
// vendor/product id. Empty string if the OS doesn't expose one (common for
// some virtual/AirPlay displays).
QString displayProductName(CGDirectDisplayID displayID)
{
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault,
            IOServiceMatching("IODisplayConnect"), &iter) != KERN_SUCCESS) {
        return QString();
    }

    QString result;
    io_service_t service;
    while ((service = IOIteratorNext(iter)) != 0) {
        CFDictionaryRef info = IODisplayCreateInfoDictionary(
                service, kIODisplayOnlyPreferredName);

        CFIndex vendorID = 0, productID = 0;
        CFNumberRef vendorIDRef = static_cast<CFNumberRef>(
                CFDictionaryGetValue(info, CFSTR(kDisplayVendorID)));
        CFNumberRef productIDRef = static_cast<CFNumberRef>(
                CFDictionaryGetValue(info, CFSTR(kDisplayProductID)));
        if (vendorIDRef) CFNumberGetValue(vendorIDRef, kCFNumberCFIndexType, &vendorID);
        if (productIDRef) CFNumberGetValue(productIDRef, kCFNumberCFIndexType, &productID);

        if (result.isEmpty()
                && CGDisplayVendorNumber(displayID) == static_cast<UInt32>(vendorID)
                && CGDisplayModelNumber(displayID) == static_cast<UInt32>(productID)) {
            CFDictionaryRef names = static_cast<CFDictionaryRef>(
                    CFDictionaryGetValue(info, CFSTR(kDisplayProductName)));
            CFStringRef nameRef = nullptr;
            if (names && CFDictionaryGetValueIfPresent(names, CFSTR("en_US"),
                    reinterpret_cast<const void**>(&nameRef)) && nameRef) {
                result = QString::fromCFString(nameRef);
            }
        }
        CFRelease(info);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return result;
}
}
#endif

FreeformServerConfigWidget::FreeformServerConfigWidget(QWidget* parent)
    : QWidget(parent), m_dragging(false), m_dragScale(1.0)
{
    // QStackedLayout::minimumSize() (which backs QTabWidget) always takes
    // the max of every PAGE's hard minimumSize, regardless of QSizePolicy
    // -- so this floor is added to every other tab's minimum size too, not
    // just "Screens and links". Keep the FLOOR small (see sizeHint() below
    // for the roomy preferred size actually used when this tab is active).
    setMinimumSize(280, 120);
}

QSize FreeformServerConfigWidget::sizeHint() const
{
    return QSize(560, 320);
}

void FreeformServerConfigWidget::setServerDisplays(const QList<QRect>& rects)
{
    m_serverRects = rects;
    m_serverDisplayNames.clear(); // names apply to the previous rect set
    update();
}

void FreeformServerConfigWidget::setServerDisplayNames(const QStringList& names)
{
    m_serverDisplayNames = alignDisplayNames(names, m_serverRects.size());
    update();
}

void FreeformServerConfigWidget::setServerScreenName(const QString& name)
{
    m_serverScreenName = name;
    update();
}

void FreeformServerConfigWidget::setClientDisplays(const QString& clientName, const QList<QRect>& rects)
{
    if (clientName.isEmpty()) return;
    ClientLayout& client = m_clients[clientName];
    client.rects = rects;
    client.displayNames.clear(); // names apply to the previous rect set
    if (m_activeClientName.isEmpty()) {
        m_activeClientName = clientName;
    }
    update();
}

QList<QRect> FreeformServerConfigWidget::clientDisplays(const QString& clientName) const
{
    return m_clients.value(clientName).rects;
}

QList<QRect> FreeformServerConfigWidget::clientDisplays() const
{
    return clientDisplays(m_activeClientName);
}

void FreeformServerConfigWidget::setClientDisplayNames(
        const QString& clientName, const QStringList& names)
{
    if (!m_clients.contains(clientName)) return;
    ClientLayout& client = m_clients[clientName];
    client.displayNames = alignDisplayNames(names, client.rects.size());
    update();
}

void FreeformServerConfigWidget::setClientDisplayNames(const QStringList& names)
{
    setClientDisplayNames(m_activeClientName, names);
}

QStringList FreeformServerConfigWidget::clientDisplayNames(const QString& clientName) const
{
    return m_clients.value(clientName).displayNames;
}

QStringList FreeformServerConfigWidget::clientDisplayNames() const
{
    return clientDisplayNames(m_activeClientName);
}

QString FreeformServerConfigWidget::canvasDisplayLabel(
        const QString& screenName, int displayIndex, const QString& productName)
{
    if (!productName.isEmpty()) {
        return productName;
    }
    return QStringLiteral("%1 #%2").arg(screenName).arg(displayIndex);
}

QStringList FreeformServerConfigWidget::alignDisplayNames(
        const QStringList& names, int displayCount)
{
    QStringList aligned = names;
    if (displayCount < 0) {
        displayCount = 0;
    }
    // QStringList::resize() is unavailable in some Qt 5 builds; pad with
    // explicit empty strings instead.
    while (aligned.size() < displayCount) {
        aligned.append(QString());
    }
    if (aligned.size() > displayCount) {
        aligned.erase(aligned.begin() + displayCount, aligned.end());
    }
    return aligned;
}

void FreeformServerConfigWidget::setClientPosition(const QString& clientName, const QPoint& pos)
{
    if (clientName.isEmpty()) return;
    ClientLayout& client = m_clients[clientName];
    client.pos = pos;
    m_activeClientName = clientName;
    update();
}

void FreeformServerConfigWidget::setClientPosition(const QPoint& pos)
{
    setClientPosition(m_activeClientName, pos);
}

QPoint FreeformServerConfigWidget::clientPosition(const QString& clientName) const
{
    return m_clients.value(clientName).pos;
}

QPoint FreeformServerConfigWidget::clientPosition() const
{
    return clientPosition(m_activeClientName);
}

QStringList FreeformServerConfigWidget::clientNames() const
{
    return m_clients.keys();
}

void FreeformServerConfigWidget::syncFromSystemDisplays()
{
#ifdef Q_OS_MAC
    CGDisplayCount count = 0;
    CGGetActiveDisplayList(0, NULL, &count);
    if (count == 0) return;
    CGDirectDisplayID* ids = new CGDirectDisplayID[count];
    CGGetActiveDisplayList(count, ids, &count);
    QList<QRect> rects;
    QStringList names;
    for (CGDisplayCount i = 0; i < count; ++i) {
        CGRect r = CGDisplayBounds(ids[i]);
        rects.append(QRect((int)r.origin.x, (int)r.origin.y, (int)r.size.width, (int)r.size.height));
        names.append(displayProductName(ids[i]));
    }
    delete[] ids;
    // Keep the rects in global CGDisplayBounds coordinates so they match the
    // daemon's runtime geometry; the paintEvent already translates/scales for
    // display, so no normalization is needed here.
    setServerDisplays(rects);
    setServerDisplayNames(names);
#endif
}

QRect FreeformServerConfigWidget::m_clientGlobalBounds(const ClientLayout& client) const
{
    if (client.rects.isEmpty()) return QRect();
    QRect bounds = client.rects[0].translated(client.pos);
    for (int i = 1; i < client.rects.size(); ++i) {
        bounds = bounds.united(client.rects[i].translated(client.pos));
    }
    return bounds;
}

QRect FreeformServerConfigWidget::m_clientGlobalBounds() const
{
    QRect bounds;
    for (QMap<QString, ClientLayout>::const_iterator it = m_clients.constBegin();
         it != m_clients.constEnd(); ++it) {
        const QRect clientBounds = m_clientGlobalBounds(it.value());
        if (!clientBounds.isEmpty()) {
            bounds = bounds.united(clientBounds);
        }
    }
    return bounds;
}

void FreeformServerConfigWidget::computeTransform(qreal& scale, QPoint& offset) const
{
    // Compute a global-to-widget transform that fits every server display
    // plus every connected client's current bounds into the widget, with padding.
    QRect allBounds;
    for (const QRect& r : m_serverRects) allBounds = allBounds.united(r);
    const QRect clientBounds = m_clientGlobalBounds();
    if (!clientBounds.isEmpty()) allBounds = allBounds.united(clientBounds);
    if (allBounds.isEmpty()) {
        scale = 1.0;
        offset = QPoint(0, 0);
        return;
    }

    allBounds.adjust(-20, -20, 20, 20);
    qreal sx = (qreal)width() / allBounds.width();
    qreal sy = (qreal)height() / allBounds.height();
    scale = qMin(sx, sy) * 0.9;
    offset = QPoint((width() - allBounds.width() * scale) / 2 - allBounds.x() * scale,
                    (height() - allBounds.height() * scale) / 2 - allBounds.y() * scale);
}

void FreeformServerConfigWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));

    // While dragging, use the transform frozen at drag-start instead of
    // recomputing it from the client's current (moving) position -- other-
    // wise every server/client rect visibly rescales and shifts on each
    // mouse-move, making the server display look like it's moving too.
    qreal scale;
    QPoint offset;
    if (m_dragging) {
        scale = m_dragScale;
        offset = m_dragViewOffset;
    } else {
        computeTransform(scale, offset);
    }

    auto toWidget = [&](const QRect& r) -> QRect {
        return QRect(int(r.x() * scale + offset.x()),
                     int(r.y() * scale + offset.y()),
                     int(r.width() * scale),
                     int(r.height() * scale));
    };

    // Server displays (L-shaped, steel blue)
    for (int i = 0; i < m_serverRects.size(); ++i) {
        QRect wr = toWidget(m_serverRects[i]);
        p.fillRect(wr, QColor(70, 130, 180));
        p.setPen(QColor(100, 160, 210));
        p.drawRect(wr);
        p.setPen(Qt::white);
        p.drawText(wr, Qt::AlignCenter,
                   canvasDisplayLabel(m_serverScreenName, i + 1, m_serverDisplayNames.value(i)));
    }

    // Client displays (draggable, orange)
    for (QMap<QString, ClientLayout>::const_iterator it = m_clients.constBegin();
         it != m_clients.constEnd(); ++it) {
        const QString& clientName = it.key();
        const ClientLayout& client = it.value();
        for (int i = 0; i < client.rects.size(); ++i) {
            QRect gr = client.rects[i].translated(client.pos);
            QRect wr = toWidget(gr);
            p.fillRect(wr, QColor(220, 120, 40));
            p.setPen(clientName == m_activeClientName
                     ? QColor(255, 190, 100)
                     : QColor(255, 160, 80));
            p.drawRect(wr);
            p.setPen(Qt::white);
            p.drawText(wr, Qt::AlignCenter,
                       canvasDisplayLabel(clientName, i + 1, client.displayNames.value(i)));
        }
    }

    // Notch hint
    p.setPen(QColor(100, 100, 100));
    p.drawText(5, height() - 5, "Drag orange client display to align freely");
}

void FreeformServerConfigWidget::mousePressEvent(QMouseEvent* event)
{
    qreal scale;
    QPoint offset;
    computeTransform(scale, offset);

    for (QMap<QString, ClientLayout>::const_iterator it = m_clients.constBegin();
         it != m_clients.constEnd(); ++it) {
        const QRect clientBounds = m_clientGlobalBounds(it.value());
        if (clientBounds.isEmpty()) continue;

        QRect wClient(int(clientBounds.x() * scale + offset.x()),
                      int(clientBounds.y() * scale + offset.y()),
                      int(clientBounds.width() * scale),
                      int(clientBounds.height() * scale));
        if (!wClient.contains(event->pos())) continue;

        m_activeClientName = it.key();
        m_dragging = true;
        // Freeze the transform for the duration of the drag (see paintEvent).
        m_dragScale = scale;
        m_dragViewOffset = offset;
        m_dragOffset = event->pos() - wClient.topLeft();
        setCursor(Qt::ClosedHandCursor);
    }
}

void FreeformServerConfigWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging || !m_clients.contains(m_activeClientName)) return;

    QPoint widgetPos = event->pos() - m_dragOffset;
    QPoint newGlobalPos(int((widgetPos.x() - m_dragViewOffset.x()) / m_dragScale),
                        int((widgetPos.y() - m_dragViewOffset.y()) / m_dragScale));

    ClientLayout& client = m_clients[m_activeClientName];
    if (!client.rects.isEmpty()) {
        QRect localBounds = client.rects[0];
        for (int i = 1; i < client.rects.size(); ++i) {
            localBounds = localBounds.united(client.rects[i]);
        }
        const int cw = localBounds.width();
        const int ch = localBounds.height();
        const QRect tentative(newGlobalPos.x(), newGlobalPos.y(), cw, ch);

        // Snap using a visible widget-pixel threshold converted back into
        // global layout pixels. A fixed 20 layout-pixel threshold is only a
        // few screen pixels once a multi-screen layout is scaled down, so the
        // user can place displays that look adjacent but persist with a gap.
        const int snap = qMax(32, int(20 / m_dragScale));
        int bestDx = snap + 1, bestDy = snap + 1;
        int snapX = newGlobalPos.x(), snapY = newGlobalPos.y();
        for (const QRect& sr : m_serverRects) {
            const bool yOverlaps = tentative.y() < sr.y() + sr.height()
                                 && tentative.y() + tentative.height() > sr.y();
            const bool xOverlaps = tentative.x() < sr.x() + sr.width()
                                 && tentative.x() + tentative.width() > sr.x();

            if (yOverlaps) {
                int dRight = qAbs((tentative.x() + cw) - sr.x());
                if (dRight < bestDx) { bestDx = dRight; snapX = sr.x() - cw; }
                int dLeft = qAbs(tentative.x() - (sr.x() + sr.width()));
                if (dLeft < bestDx) { bestDx = dLeft; snapX = sr.x() + sr.width(); }
            }
            if (xOverlaps) {
                int dBottom = qAbs((tentative.y() + ch) - sr.y());
                if (dBottom < bestDy) { bestDy = dBottom; snapY = sr.y() - ch; }
                int dTop = qAbs(tentative.y() - (sr.y() + sr.height()));
                if (dTop < bestDy) { bestDy = dTop; snapY = sr.y() + sr.height(); }
            }
        }
        if (bestDx <= snap) newGlobalPos.setX(snapX);
        if (bestDy <= snap) newGlobalPos.setY(snapY);
    }

    client.pos = newGlobalPos;
    update();
    emit clientPositionChanged(m_activeClientName, client.pos);
}

void FreeformServerConfigWidget::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
    unsetCursor();
}
