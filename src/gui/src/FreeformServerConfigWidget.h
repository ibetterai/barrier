#pragma once

#include <QWidget>
#include <QRect>
#include <QPoint>
#include <QMap>
#include <QList>
#include <QString>
#include <QStringList>

class FreeformServerConfigWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FreeformServerConfigWidget(QWidget* parent = nullptr);

    void setServerDisplays(const QList<QRect>& rects);
    QList<QRect> serverDisplays() const { return m_serverRects; }
    // This machine's Barrier screen name; used for the per-display
    // fallback label "<name> #<index>" when the OS has no product name.
    void setServerScreenName(const QString& name);
    // Optional real monitor names (from the OS), same order as the rects
    // passed to setServerDisplays(); count-aligned so extra entries are
    // dropped and missing entries stay empty. Empty entries render as
    // "<server screen name> #<index>".
    void setServerDisplayNames(const QStringList& names);
    void setClientDisplays(const QString& clientName, const QList<QRect>& rects);
    QList<QRect> clientDisplays(const QString& clientName) const;
    QList<QRect> clientDisplays() const;
    // Optional per-display product names for a client, same order as
    // the rects passed to setClientDisplays() and count-aligned the same
    // way. The names come from the client's DDNM display metadata; empty
    // entries render as "<client screen name> #<index>".
    void setClientDisplayNames(const QString& clientName, const QStringList& names);
    void setClientDisplayNames(const QStringList& names);
    QStringList clientDisplayNames(const QString& clientName) const;
    QStringList clientDisplayNames() const;
    void setClientPosition(const QString& clientName, const QPoint& pos);
    void setClientPosition(const QPoint& pos);
    QPoint clientPosition(const QString& clientName) const;
    QPoint clientPosition() const;
    QStringList clientNames() const;
    QString clientName() const { return m_activeClientName; }

    // Label for one physical display: the OS product name when present,
    // otherwise "<screenName> #<displayIndex>" (1-based display index).
    static QString canvasDisplayLabel(const QString& screenName, int displayIndex,
                                      const QString& productName);
    // Count-align a name list to \p displayCount: extra entries are
    // dropped, missing entries are padded with empty names.
    static QStringList alignDisplayNames(const QStringList& names, int displayCount);

    // Auto-sync from local macOS display rects (CGDisplayBounds)
    void syncFromSystemDisplays();

    // Preferred size when this is the visible tab page (QStackedLayout
    // respects QSizePolicy::Ignored for sizeHint(), so this only affects
    // the CURRENT tab -- unlike minimumSize(), which QStackedLayout always
    // aggregates across every tab regardless of policy. Keep sizeHint()
    // roomy for a usable canvas and minimumSize() small so other tabs
    // ("Hotkeys", "Advanced server settings") aren't forced to match it.
    QSize sizeHint() const override;

signals:
    void clientPositionChanged(const QString& clientName, const QPoint& pos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QList<QRect> m_serverRects; // in screen-local coords, main at 0,0
    QString m_serverScreenName;
    QStringList m_serverDisplayNames;
    struct ClientLayout {
        QList<QRect> rects;
        QStringList displayNames;
        QPoint pos;
    };
    QMap<QString, ClientLayout> m_clients;
    QString m_activeClientName;
    QPoint m_dragOffset;
    bool m_dragging;
    qreal m_dragScale;   // transform frozen at drag start, so the view
    QPoint m_dragViewOffset; // doesn't rescale/shift while dragging
    QRect m_clientGlobalBounds(const ClientLayout& client) const;
    QRect m_clientGlobalBounds() const;
    // Global-to-widget transform for the current (non-dragging) layout.
    void computeTransform(qreal& scale, QPoint& offset) const;
};
