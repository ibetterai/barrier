/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "platform/OSXClipboard.h"
#include "barrier/PlatformScreen.h"
#include "barrier/DragInformation.h"
#include "base/EventTypes.h"
#include "common/stdmap.h"
#include "common/stdvector.h"

#include <bitset>
#include <cstdint>
#include <mutex>
#include <sys/types.h>
#include <Carbon/Carbon.h>
#include <mach/mach_port.h>
#include <mach/mach_interface.h>
#include <mach/mach_init.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>

extern "C" {
    typedef int CGSConnectionID;
    CGError CGSSetConnectionProperty(CGSConnectionID cid, CGSConnectionID targetCID, CFStringRef key, CFTypeRef value);
    int _CGSDefaultConnection();
}


template <class T>
class CondVar;
class EventQueueTimer;
class Mutex;
class Thread;
class OSXKeyState;
class OSXScreenSaver;
class IEventQueue;
class Mutex;

//! Implementation of IPlatformScreen for OS X
class OSXScreen : public PlatformScreen {
public:
    OSXScreen(IEventQueue* events, bool isPrimary, bool autoShowHideCursor=true);
    virtual ~OSXScreen();

    IEventQueue*        getEvents() const { return m_events; }

    // IScreen overrides
    virtual void*        getEventTarget() const;
    virtual bool        getClipboard(ClipboardID id, IClipboard*) const;
    virtual void        getShape(SInt32& x, SInt32& y,
                            SInt32& width, SInt32& height) const;
    virtual void        getDisplays(std::vector<ScreenRect>& displays) const;
    virtual barrier::DisplayTopology getDisplayTopology() const;
    virtual void        getDisplayNames(std::vector<std::string>& names) const;
    virtual void        getCursorPos(SInt32& x, SInt32& y) const;

    // IPrimaryScreen overrides
    virtual void        reconfigure(UInt32 activeSides);
    virtual void        warpCursor(SInt32 x, SInt32 y);
    virtual UInt32        registerHotKey(KeyID key, KeyModifierMask mask);
    virtual void        unregisterHotKey(UInt32 id);
    virtual void        fakeInputBegin();
    virtual void        fakeInputEnd();
    virtual SInt32        getJumpZoneSize() const;
    virtual bool        isAnyMouseButtonDown(UInt32& buttonID) const;
    virtual void        getCursorCenter(SInt32& x, SInt32& y) const;

    // ISecondaryScreen overrides
    virtual void        fakeMouseButton(ButtonID id, bool press);
    virtual void        fakeMouseMove(SInt32 x, SInt32 y);
    virtual void        fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const;
    virtual void        fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const;

    // IPlatformScreen overrides
    virtual void        enable();
    virtual void        disable();
    virtual void        enter();
    virtual bool        leave();
    virtual bool        setClipboard(ClipboardID, const IClipboard*);
    virtual void        checkClipboards();
    virtual void        openScreensaver(bool notify);
    virtual void        closeScreensaver();
    virtual void        screensaver(bool activate);
    virtual void        resetOptions();
    virtual void        setOptions(const OptionsList& options);
    virtual void        setSequenceNumber(UInt32);
    virtual bool        isPrimary() const;
    virtual void        fakeDraggingFiles(DragFileList fileList);
    virtual std::string& getDraggingFilename();

    struct TopologyDisplayRecord {
        std::string stableId;
        ScreenRect logicalBounds;
        double rotationDegrees;
        bool primary;
    };

    enum class DisplayRefreshSource {
        None,
        Active,
        Online
    };

    enum class DisplayRefreshRole {
        PrimaryServer,
        SecondaryClient
    };

    struct DisplayRefreshDecision {
        DisplayRefreshSource source;
        bool preserveCurrentSnapshot;
        bool retryRequired;
    };

    //! Choose the CoreGraphics display set for a geometry refresh.
    /*! Primary servers publish only active displays: a successful empty
        result is a real zero-display snapshot and a query failure requires a
        bounded retry. Secondary clients may preserve an existing snapshot or
        bootstrap from online displays so a locked login session never sends
        invalid 0x0 geometry. */
    static DisplayRefreshDecision decideDisplayRefresh(
                            DisplayRefreshRole role,
                            bool hasValidSnapshot,
                            bool activeQuerySucceeded,
                            CGDisplayCount activeDisplayCount,
                            bool onlineQuerySucceeded,
                            CGDisplayCount onlineDisplayCount);

    //! Whether a display callback is late enough to publish fresh geometry.
    /*! CoreGraphics sends begin-configuration callbacks before it updates
        display state.  Those callbacks suspend routing but must not capture
        and publish the old geometry as a fresh snapshot. */
    static bool         displayReconfigurationCaptureReady(
                            CGDisplayChangeSummaryFlags flags);

    //! Whether a completed capture still belongs to the latest callback.
    static bool         displayRefreshGenerationIsCurrent(
                            std::uint64_t capturedGeneration,
                            std::uint64_t currentGeneration);

    //! Whether a disabled Quartz event tap must be re-enabled immediately.
    static bool         eventTapDisableRequiresReenable(CGEventType type);

    //! Select the run loop that owns the Quartz event-tap source.
    static CFRunLoopRef selectEventTapRunLoop(CFRunLoopRef currentRunLoop,
                            CFRunLoopRef mainRunLoop);

    static barrier::DisplayTopology topologyFromDisplayRecords(
                            const std::vector<TopologyDisplayRecord>& displays);
    static std::string normalizeDisplayIdentifier(const std::string& identifier);

    //! Index of the display containing (x, y) in an ordered display
    //! snapshot, or -1 when the coordinate is outside every display.
    /*! Rects use half-open [x, x + w) x [y, y + h) bounds so the edge
        shared by adjacent displays resolves to exactly one of them. */
    static int        displayIndexAt(const std::vector<ScreenRect>& displays,
                            SInt32 x, SInt32 y);

    //! Whether an entry must start a new wake hold.
    /*! An awake display never requests a wake (false regardless of the
        hold state).  An asleep display requests a new hold only when
        none is active (now >= holdExpiry); an entry during an active
        hold refreshes that hold instead of stacking a second child. */
    static bool        shouldRequestWake(bool displayAsleep,
                            SInt32 now, SInt32 holdExpiry);

    //! Direction sign for a host's natural-scroll preference.
    /*! Barrier's DMouseWheel transport carries wheel deltas in the
        classic wheel convention, which is what macOS reports when
        "Scroll direction: Natural" is off: positive yDelta scrolls
        content toward the top of the document and positive xDelta
        scrolls content toward the right.  Natural scrolling inverts
        both axes, so this sign maps a host's local deltas to the
        transport convention and back (the mapping is its own inverse). */
    static SInt32        naturalScrollDirectionSign(bool naturalScrolling);

    //! Convert a wheel delta pair between a host's local convention
    //! and Barrier's transport convention.
    /*! Each host applies its own preference exactly once: the source
        normalizes captured deltas before relaying them and the target
        normalizes received deltas before injecting them.  Hosts that
        share a preference pass deltas through unchanged; hosts with
        differing preferences see every axis inverted, so the perceived
        content direction matches on both sides. */
    static void        normalizeScrollDeltas(bool naturalScrolling,
                            SInt32& xDelta, SInt32& yDelta);

    //! Whether a wheel event captured by the primary screen's event
    //! tap should be forwarded to the remote target.
    /*! Only events captured while the primary screen owns a remote
        target (the cursor is off the primary and on a client screen)
        are relayed.  Wheel events captured while the cursor is on the
        primary itself are left to the local system, and a secondary
        screen never captures input. */
    static bool        shouldForwardRemoteWheel(bool isPrimary,
                            bool isOnScreen);

    //! Whether macOS scroll diagnostics should be enabled for an env value.
    /*! The diagnostic spike is intentionally opt-in so normal users never
        receive noisy logs unless BARRIER_MACOS_SCROLL_DIAGNOSTICS is set. */
    static bool        shouldEnableScrollDiagnostics(const char* value);

    //! Whether the experimental Spaces swipe shortcut fallback is enabled.
    static bool        shouldEnableSpacesSwipeFallback(const char* value);

    struct SpacesSwipeSourceState {
        double accumulatedXSignal = 0.0;
        SInt64 lastEventTimeMs = 0;
        SInt64 cooldownUntilTimeMs = 0;
    };

    //! Whether a wheel message carries an experimental Spaces swipe command
    //! rather than a real wheel delta.
    static bool        isSyntheticSpacesSwipeWheel(SInt32 xDelta,
                            SInt32 yDelta);

    //! Update accumulated Magic Mouse raw gesture signal and decide whether it
    //! should fire a synthetic Spaces swipe command.
    static bool        updateSpacesSwipeSourceState(
                            SpacesSwipeSourceState& state,
                            double xSignal,
                            SInt64 nowTimeMs,
                            double threshold,
                            SInt32 windowMs,
                            SInt32 cooldownMs,
                            SInt32& xDirection);

    const std::string& getDropTarget() const { return m_dropTarget; }
    void                waitForCarbonLoop() const;

protected:
    // IPlatformScreen overrides
    virtual void        handleSystemEvent(const Event&, void*);
    virtual void        updateButtons();
    virtual IKeyState*    getKeyState() const;

private:
    void                updateScreenShape();
    void                updateScreenShape(const CGDirectDisplayID, const CGDisplayChangeSummaryFlags);
    void                scheduleDisplayRefreshRetry();
    void                cancelDisplayRefreshRetry();
    void                destroyDisplayRefreshRetry();
    void                handleDisplayRefreshRetryRequest(const Event&, void*);
    void                handleDisplayRefreshRetry(const Event&, void*);
    void                postMouseEvent(CGPoint&) const;

    // convenience function to send events
    void                sendEvent(Event::Type type, void* = NULL) const;
    void                sendClipboardEvent(Event::Type type, ClipboardID id) const;

    // message handlers
    bool                onMouseMove(CGFloat mx, CGFloat my);
    // mouse button handler.  pressed is true if this is a mousedown
    // event, false if it is a mouseup event.  macButton is the index
    // of the button pressed using the mac button mapping.
    bool                onMouseButton(bool pressed, UInt16 macButton);
    bool                onMouseWheel(SInt32 xDelta, SInt32 yDelta) const;

    void                constructMouseButtonEventMap();

    bool                onKey(CGEventRef event);

    void                onMediaKey(CGEventRef event);

    bool                onHotKey(EventRef event) const;

    // Added here to allow the carbon cursor hack to be called.
    void                showCursor();
    void                hideCursor();

    // map barrier mouse button to mac buttons
    ButtonID            mapBarrierButtonToMac(UInt16) const;

    // map mac mouse button to barrier buttons
    ButtonID            mapMacButtonToBarrier(UInt16) const;

    // map mac scroll wheel value to a barrier scroll wheel value
    SInt32                mapScrollWheelToBarrier(float) const;

    // map barrier scroll wheel value to a mac scroll wheel value
    SInt32                mapScrollWheelFromBarrier(float) const;

    // get the current scroll wheel speed
    double                getScrollSpeed() const;

    // get the current scroll wheel speed
    double                getScrollSpeedFactor() const;

    // whether this host scrolls "naturally" (content follows the
    // fingers); missing or unreadable preferences fall back to the
    // macOS default since Lion
    bool                getNaturalScrolling() const;

    // enable/disable drag handling for buttons 3 and up
    void                enableDragTimer(bool enable);

    // drag timer handler
    void                handleDrag(const Event&, void*);

    // clipboard check timer handler
    void                handleClipboardCheck(const Event&, void*);

    // Resolution switch callback
    static void    displayReconfigurationCallback(CGDirectDisplayID,
                            CGDisplayChangeSummaryFlags, void*);

    // fast user switch callback
    static pascal OSStatus
                        userSwitchCallback(EventHandlerCallRef nextHandler,
                           EventRef theEvent, void* inUserData);

    // sleep / wakeup support
    void watchSystemPowerThread();
    static void            testCanceled(CFRunLoopTimerRef timer, void*info);
    static void            powerChangeCallback(void* refcon, io_service_t service,
                            natural_t messageType, void* messageArgument);
    void                handlePowerChangeRequest(natural_t messageType,
                             void* messageArgument);

    void                handleConfirmSleep(const Event& event, void*);

    // global hotkey operating mode
    static bool            isGlobalHotKeyOperatingModeAvailable();
    static void            setGlobalHotKeysEnabled(bool enabled);
    static bool            getGlobalHotKeysEnabled();

    // Quartz event tap support
    static CGEventRef    handleCGInputEvent(CGEventTapProxy proxy,
                                           CGEventType type,
                                           CGEventRef event,
                                           void* refcon);
    static CGEventRef    handleCGInputEventSecondary(CGEventTapProxy proxy,
                                                    CGEventType type,
                                                    CGEventRef event,
                                                    void* refcon);

    // convert CFString to char*
    static char*        CFStringRefToUTF8String(CFStringRef aString);

    void get_drop_target_thread();

private:
    struct HotKeyItem {
    public:
        HotKeyItem(UInt32, UInt32);
        HotKeyItem(EventHotKeyRef, UInt32, UInt32);

        EventHotKeyRef    getRef() const;

        bool            operator<(const HotKeyItem&) const;

    private:
        EventHotKeyRef    m_ref;
        UInt32            m_keycode;
        UInt32            m_mask;
    };

    enum EMouseButtonState {
        kMouseButtonUp = 0,
        kMouseButtonDragged,
        kMouseButtonDown,
        kMouseButtonStateMax
    };


    class MouseButtonState {
    public:
        void set(UInt32 button, EMouseButtonState state);
        bool any();
        void reset();
        void overwrite(UInt32 buttons);

        bool test(UInt32 button) const;
        SInt8 getFirstButtonDown() const;
    private:
        std::bitset<NumButtonIDs>      m_buttons;
    };

    typedef std::map<UInt32, HotKeyItem> HotKeyMap;
    typedef std::vector<UInt32> HotKeyIDList;
    typedef std::map<KeyModifierMask, UInt32> ModifierHotKeyMap;
    typedef std::map<HotKeyItem, UInt32> HotKeyToIDMap;

    // true if screen is being used as a primary screen, false otherwise
    bool                m_isPrimary;

    // true if mouse has entered the screen
    bool                m_isOnScreen;

    // Guards the published display generation across refresh callbacks and readers.
    mutable std::mutex  m_displaySnapshotMutex;

    // the display
    CGDirectDisplayID    m_displayID;

    // screen shape stuff
    SInt32                m_x, m_y;
    SInt32                m_w, m_h;
    SInt32                m_xCenter, m_yCenter;
    std::mutex            m_displayRefreshCaptureMutex;
    std::mutex            m_displayRefreshGenerationMutex;
    std::uint64_t         m_displayReconfigurationGeneration;
    bool                  m_displayConfigurationInProgress;
    std::mutex            m_displayRefreshRetryMutex;
    EventQueueTimer*      m_displayRefreshRetryTimer;
    char                  m_displayRefreshRetryEventTarget;
    bool                  m_displayRefreshRetryArmed;
    bool                  m_displayRefreshRetryRequestPending;
    bool                  m_displayRefreshRetryHandlerInstalled;

    //! One physical display captured during a geometry refresh.
    /*! id, rect, and name are resolved together so getDisplays() and
        getDisplayNames() always report the same ordered snapshot; a
        topology change rebuilds the whole vector. */
    struct DisplayEntry {
        CGDirectDisplayID    m_id;
        ScreenRect            m_rect;
        std::string            m_name;
    };

    std::vector<DisplayEntry>    m_displays;
    barrier::DisplayTopology       m_displayTopology;

    // mouse state
    mutable SInt32        m_xCursor, m_yCursor;
    mutable bool        m_cursorPosValid;

    /* FIXME: this data structure is explicitly marked mutable due
       to a need to track the state of buttons since the remote
       side only lets us know of change events, and because the
       fakeMouseButton button method is marked 'const'. This is
       Evil, and this should be moved to a place where it need not
       be mutable as soon as possible. */
    mutable MouseButtonState m_buttonState;
    typedef std::map<UInt16, CGEventType> MouseButtonEventMapType;
    std::vector<MouseButtonEventMapType> MouseButtonEventMap;

    bool                m_cursorHidden;
    SInt32                m_dragNumButtonsDown;
    Point                m_dragLastPoint;
    EventQueueTimer*    m_dragTimer;

    // keyboard stuff
    OSXKeyState*        m_keyState;

    // clipboards
    OSXClipboard       m_pasteboard;
    UInt32                m_sequenceNumber;

    // screen saver stuff
    OSXScreenSaver*    m_screensaver;
    bool                m_screensaverNotify;

    // clipboard stuff
    bool                m_ownClipboard;
    EventQueueTimer*    m_clipboardTimer;

    // window object that gets user input events when the server
    // has focus.
    WindowRef            m_hiddenWindow;
    // window object that gets user input events when the server
    // does not have focus.
    WindowRef            m_userInputWindow;

    // fast user switching
    EventHandlerRef            m_switchEventHandlerRef;

    // sleep / wakeup
    // conditionally wake the display under the entry coordinate; used
    // by the client-side enter() path
    void                wakeEnteredDisplay();
    void                startWakeHold(SInt32 now);
    void                refreshWakeHold(SInt32 now);
    void                stopWakeHold();

    Mutex*                    m_pmMutex;
    Thread*                m_pmWatchThread;
    CondVar<bool>*            m_pmThreadReady;
    CFRunLoopRef            m_pmRunloop;
    io_connect_t            m_pmRootPort;

    // hot key stuff
    HotKeyMap                m_hotKeys;
    HotKeyIDList            m_oldHotKeyIDs;
    ModifierHotKeyMap        m_modifierHotKeys;
    UInt32                    m_activeModifierHotKey;
    KeyModifierMask            m_activeModifierHotKeyMask;
    HotKeyToIDMap            m_hotKeyToIDMap;

    // global hotkey operating mode
    static bool                s_testedForGHOM;
    static bool                s_hasGHOM;

    // Quartz input event support
    CFMachPortRef            m_eventTapPort;
    CFRunLoopSourceRef        m_eventTapRLSR;
    CFRunLoopRef            m_eventTapRunLoop;

    // for double click coalescing.
    double                    m_lastClickTime;
    int                     m_clickState;
    SInt32                    m_lastSingleClickXCursor;
    SInt32                    m_lastSingleClickYCursor;

    // cursor will hide and show on enable and disable if true.
    bool                    m_autoShowHideCursor;

    IEventQueue*            m_events;

    Thread*                m_getDropTargetThread;
    std::string m_dropTarget;

    //! Active caffeinate child holding the display awake, or -1.
    pid_t                m_wakeHoldPid;
    //! Absolute time (seconds) the current wake hold expires; 0 = none.
    SInt32                m_wakeHoldExpiry;

#if defined(MAC_OS_X_VERSION_10_7)
    Mutex*                    m_carbonLoopMutex;
    CondVar<bool>*            m_carbonLoopReady;
#endif

    class OSXScreenImpl*    m_impl;
};
