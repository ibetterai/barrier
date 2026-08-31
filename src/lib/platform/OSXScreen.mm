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

#include "platform/OSXScreen.h"

#include "base/EventQueue.h"
#include "client/Client.h"
#include "platform/OSXClipboard.h"
#include "base/SimpleEventQueueBuffer.h"
#include "platform/OSXKeyState.h"
#include "platform/OSXScreenSaver.h"
#include "platform/OSXDragSimulator.h"
#include "platform/OSXMediaKeySupport.h"
#include "platform/OSXPasteboardPeeker.h"
#include "barrier/Clipboard.h"
#include "barrier/KeyMap.h"
#include "barrier/ClientApp.h"
#include "mt/CondVar.h"
#include "mt/Lock.h"
#include "mt/Mutex.h"
#include "mt/Thread.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"

#include <math.h>
#include <stdexcept>
#include <mach-o/dyld.h>
#include <AvailabilityMacros.h>
#include <IOKit/hidsystem/event_status_driver.h>
#include <AppKit/NSEvent.h>
#include <AppKit/NSTouch.h>
#include <IOKit/graphics/IOGraphicsLib.h>

#include <stdarg.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// This isn't in any Apple SDK that I know of as of yet.
enum {
	kBarrierEventMouseScroll = 11,
	kBarrierMouseScrollAxisX = 'saxx',
	kBarrierMouseScrollAxisY = 'saxy'
};

enum {
	kCarbonLoopWaitTimeout = 10
};

namespace {

//! Path to the tool that wakes and briefly holds an asleep display.
const char kWakeHoldPath[] = "/usr/bin/caffeinate";
//! How long a wake hold lasts before caffeinate exits on its own.
const SInt32 kWakeHoldSeconds = 15;
//! Enables scroll-event diagnostics for the Magic Mouse Spaces swipe spike.
const char kScrollDiagnosticsEnv[] = "BARRIER_MACOS_SCROLL_DIAGNOSTICS";
//! Overrides the Magic Mouse Spaces swipe fallback; enabled by default on macOS.
const char kSpacesSwipeFallbackEnv[] = "BARRIER_MACOS_SPACES_SWIPE_FALLBACK";
//! Optional direction inversion for the fallback.
const char kSpacesSwipeInvertEnv[] = "BARRIER_MACOS_SPACES_SWIPE_INVERT";
//! Optional raw gesture threshold for source-side Magic Mouse swipe detection.
const char kSpacesSwipeSourceThresholdEnv[] =
	"BARRIER_MACOS_SPACES_SWIPE_SOURCE_THRESHOLD";
//! Optional maximum gap between source raw gesture events in one swipe.
const char kSpacesSwipeSourceWindowMsEnv[] =
	"BARRIER_MACOS_SPACES_SWIPE_SOURCE_WINDOW_MS";
//! Optional source-side cooldown after emitting a synthetic swipe.
const char kSpacesSwipeSourceCooldownMsEnv[] =
	"BARRIER_MACOS_SPACES_SWIPE_SOURCE_COOLDOWN_MS";
//! Observed Magic Mouse type=30 raw field 124 sums to about 0.6-5 per swipe.
const double kSpacesSwipeDefaultSourceThreshold = 0.55;
//! Observed type=30 raw bursts are tight; keep this short to separate repeats.
const SInt32 kSpacesSwipeDefaultSourceWindowMs = 180;
//! Source raw signal is cleaner than target wheel momentum; repeats can be fast.
const SInt32 kSpacesSwipeDefaultSourceCooldownMs = 180;
//! Private CGEvent field carrying horizontal gesture direction on the test Macs.
const CGEventField kMagicMouseSpacesSwipeRawXField =
	static_cast<CGEventField>(124);
//! Synthetic wheel magnitude large enough for target-side fallback recognition.
const SInt32 kSpacesSwipeSyntheticWheelDelta = 30000;
//! Marks a mouseWheel packet as a Spaces swipe command.
const SInt32 kSpacesSwipeSyntheticWheelSentinel = -31415;
//! Fixed, bounded retry delay after a primary CoreGraphics query failure.
const double kDisplayRefreshRetrySeconds = 0.25;
//! Extra test-build log sink that does not depend on the GUI log window.
const char kSwipeTestDiagnosticsLogPath[] = "/tmp/barrier-macos-scroll-diagnostics.log";
OSXScreen::SpacesSwipeSourceState g_spacesSwipeSourceState;

typedef CGError (*DisplayListQuery)(CGDisplayCount,
	CGDirectDisplayID*, CGDisplayCount*);

struct DisplayQueryResult {
	bool succeeded;
	std::vector<CGDirectDisplayID> displays;
};

DisplayQueryResult
queryDisplayList(DisplayListQuery query)
{
	DisplayQueryResult result = {false, {}};
	CGDisplayCount count = 0;
	if (query(0, NULL, &count) != CGDisplayNoErr) {
		return result;
	}

	result.succeeded = true;
	if (count == 0) {
		return result;
	}

	result.displays.resize(count);
	CGDisplayCount actualCount = 0;
	if (query(count, result.displays.data(), &actualCount) != CGDisplayNoErr) {
		result.succeeded = false;
		result.displays.clear();
		return result;
	}

	// The display set can shrink between the count and data queries.  A
	// concurrent increase is bounded by the capacity passed to CoreGraphics.
	if (actualCount < count) {
		result.displays.resize(actualCount);
	}
	return result;
}

std::vector<CGDirectDisplayID>
drawableDisplayList(const std::vector<CGDirectDisplayID>& queriedDisplays,
	bool filterMirrorFollowers)
{
	std::vector<CGDirectDisplayID> displays;
	displays.reserve(queriedDisplays.size());
	for (CGDirectDisplayID display : queriedDisplays) {
		const CGRect bounds = CGDisplayBounds(display);
		// Online-display queries may include hardware-mirror followers, which
		// are not independently drawable.  Active-display queries already
		// return a drawable representative for a mirror set, but any display
		// ID can become stale between the list query and the geometry read.
		if ((filterMirrorFollowers &&
			 CGDisplayMirrorsDisplay(display) != kCGNullDirectDisplay) ||
			bounds.size.width <= 0 || bounds.size.height <= 0) {
			continue;
		}
		displays.push_back(display);
	}
	return displays;
}

bool
isScrollDiagnosticsEnabled()
{
	return OSXScreen::shouldEnableScrollDiagnostics(getenv(kScrollDiagnosticsEnv));
}

bool
isSpacesSwipeFallbackEnabled()
{
	const char* value = getenv(kSpacesSwipeFallbackEnv);
	if (value == NULL || value[0] == '\0') {
		return true;
	}
	return OSXScreen::shouldEnableSpacesSwipeFallback(value);
}

SInt32
getPositiveEnvOrDefault(const char* envName, SInt32 defaultValue)
{
	const char* value = getenv(envName);
	if (value == NULL || value[0] == '\0') {
		return defaultValue;
	}

	const long parsed = strtol(value, NULL, 10);
	if (parsed <= 0 || parsed > INT32_MAX) {
		return defaultValue;
	}

	return static_cast<SInt32>(parsed);
}

double
getPositiveDoubleEnvOrDefault(const char* envName, double defaultValue)
{
	const char* value = getenv(envName);
	if (value == NULL || value[0] == '\0') {
		return defaultValue;
	}

	char* end = NULL;
	const double parsed = strtod(value, &end);
	if (end == value || parsed <= 0.0 || !isfinite(parsed)) {
		return defaultValue;
	}

	return parsed;
}

bool
isSpacesSwipeDirectionInverted()
{
	return OSXScreen::shouldEnableSpacesSwipeFallback(
		getenv(kSpacesSwipeInvertEnv));
}

double
getSpacesSwipeSourceThreshold()
{
	return getPositiveDoubleEnvOrDefault(kSpacesSwipeSourceThresholdEnv,
										 kSpacesSwipeDefaultSourceThreshold);
}

SInt32
getSpacesSwipeSourceWindowMs()
{
	return getPositiveEnvOrDefault(kSpacesSwipeSourceWindowMsEnv,
								   kSpacesSwipeDefaultSourceWindowMs);
}

SInt32
getSpacesSwipeSourceCooldownMs()
{
	return getPositiveEnvOrDefault(kSpacesSwipeSourceCooldownMsEnv,
								   kSpacesSwipeDefaultSourceCooldownMs);
}

SInt64
getMonotonicMilliseconds()
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}

	return static_cast<SInt64>(now.tv_sec) * 1000 +
		   static_cast<SInt64>(now.tv_nsec / 1000000);
}

SInt64
getCGEventIntegerField(CGEventRef event, CGEventField field)
{
	return CGEventGetIntegerValueField(event, field);
}

double
fixedPointToDouble(SInt64 value)
{
	return static_cast<double>(value) / 65536.0;
}

const char*
nsEventTypeName(NSInteger type)
{
	switch (type) {
	case NSEventTypeScrollWheel:
		return "scroll";
	case NSEventTypeGesture:
		return "gesture";
	case NSEventTypeMagnify:
		return "magnify";
	case NSEventTypeSwipe:
		return "swipe";
	case NSEventTypeRotate:
		return "rotate";
	case NSEventTypeBeginGesture:
		return "begin-gesture";
	case NSEventTypeEndGesture:
		return "end-gesture";
	case NSEventTypeSystemDefined:
		return "system-defined";
	default:
		return "other";
	}
}

bool
nsEventSupportsDelta(NSEventType type)
{
	switch (type) {
	case NSEventTypeScrollWheel:
	case NSEventTypeMouseMoved:
	case NSEventTypeLeftMouseDragged:
	case NSEventTypeRightMouseDragged:
	case NSEventTypeOtherMouseDragged:
	case NSEventTypeSwipe:
		return true;
	default:
		return false;
	}
}

void
appendDiagnostic(char* buffer, size_t bufferSize, size_t& offset,
				 const char* format, ...)
{
	if (offset >= bufferSize) {
		return;
	}

	va_list args;
	va_start(args, format);
	const int written = vsnprintf(buffer + offset, bufferSize - offset,
								  format, args);
	va_end(args);

	if (written < 0) {
		return;
	}

	if (static_cast<size_t>(written) >= bufferSize - offset) {
		offset = bufferSize - 1;
		buffer[offset] = '\0';
		return;
	}

	offset += static_cast<size_t>(written);
}

void
describeRawCGEventFields(CGEventRef event, char* buffer, size_t bufferSize)
{
	if (bufferSize == 0) {
		return;
	}

	buffer[0] = '\0';
	size_t offset = 0;
	appendDiagnostic(buffer, bufferSize, offset, "rawFields=");

	bool sawField = false;
	for (int field = 0; field <= 255 && offset < bufferSize - 1; ++field) {
		const CGEventField eventField = static_cast<CGEventField>(field);
		const SInt64 integerValue = CGEventGetIntegerValueField(event,
																eventField);
		const double doubleValue = CGEventGetDoubleValueField(event,
															  eventField);
		if (integerValue == 0 && fabs(doubleValue) < 0.000001) {
			continue;
		}

		sawField = true;
		appendDiagnostic(buffer, bufferSize, offset, "f%d=%lld/%.6f;",
						 field,
						 static_cast<long long>(integerValue),
						 doubleValue);
	}

	if (!sawField) {
		appendDiagnostic(buffer, bufferSize, offset, "none");
	}
}

void
describeNSEventFromCGEvent(CGEventRef event, char* buffer, size_t bufferSize)
{
	if (bufferSize == 0) {
		return;
	}

	buffer[0] = '\0';

	@autoreleasepool {
		NSEvent* nsEvent = [NSEvent eventWithCGEvent:event];
		if (nsEvent == nil) {
			snprintf(buffer, bufferSize, "nsEvent=nil");
			return;
		}

		const NSEventType nsType = [nsEvent type];
		CGFloat deltaX = 0.0;
		CGFloat deltaY = 0.0;
		CGFloat deltaZ = 0.0;
		CGFloat scrollingDeltaX = 0.0;
		CGFloat scrollingDeltaY = 0.0;
		BOOL hasPreciseScrollingDeltas = NO;
		BOOL directionInvertedFromDevice = NO;
		NSEventPhase phase = NSEventPhaseNone;
		NSEventPhase momentumPhase = NSEventPhaseNone;
		CGFloat magnification = 0.0;
		CGFloat rotation = 0.0;
		NSUInteger touchCount = 0;
		char touchDetails[512];
		touchDetails[0] = '\0';

		if (nsEventSupportsDelta(nsType)) {
			deltaX = [nsEvent deltaX];
			deltaY = [nsEvent deltaY];
			deltaZ = [nsEvent deltaZ];
		}

		if (nsType == NSEventTypeScrollWheel) {
			hasPreciseScrollingDeltas = [nsEvent hasPreciseScrollingDeltas];
			scrollingDeltaX = [nsEvent scrollingDeltaX];
			scrollingDeltaY = [nsEvent scrollingDeltaY];
			directionInvertedFromDevice = [nsEvent isDirectionInvertedFromDevice];
			phase = [nsEvent phase];
			momentumPhase = [nsEvent momentumPhase];
		}

		if (nsType == NSEventTypeMagnify) {
			magnification = [nsEvent magnification];
		}
		else if (nsType == NSEventTypeRotate) {
			rotation = [nsEvent rotation];
		}
		else if (nsType == NSEventTypeGesture) {
			NSSet<NSTouch*>* touches = [nsEvent allTouches];
			touchCount = [touches count];
			size_t touchOffset = 0;
			NSUInteger touchIndex = 0;
			appendDiagnostic(touchDetails, sizeof(touchDetails), touchOffset,
							 " nsTouches=%lu",
							 static_cast<unsigned long>(touchCount));
			for (NSTouch* touch in touches) {
				if (touchIndex >= 4) {
					appendDiagnostic(touchDetails, sizeof(touchDetails),
									 touchOffset, " ...");
					break;
				}

				const NSPoint normalizedPosition = [touch normalizedPosition];
				const NSSize deviceSize = [touch deviceSize];
				appendDiagnostic(touchDetails, sizeof(touchDetails),
								 touchOffset,
								 " t%lu{phase=%lu pos=(%.4f,%.4f) "
								 "device=(%.4f,%.4f) resting=%d}",
								 static_cast<unsigned long>(touchIndex),
								 static_cast<unsigned long>([touch phase]),
								 static_cast<double>(normalizedPosition.x),
								 static_cast<double>(normalizedPosition.y),
								 static_cast<double>(deviceSize.width),
								 static_cast<double>(deviceSize.height),
								 [touch isResting] ? 1 : 0);
				++touchIndex;
			}
		}

		snprintf(buffer, bufferSize,
			"nsType=%ld(%s) nsDelta=(%.4f,%.4f,%.4f) "
			"nsScrolling=(%.4f,%.4f) nsPrecise=%d nsInverted=%d "
			"nsPhase=%lu nsMomentum=%lu nsMagnification=%.6f "
			"nsRotation=%.6f%s",
			static_cast<long>(nsType), nsEventTypeName(nsType),
			static_cast<double>(deltaX),
			static_cast<double>(deltaY),
			static_cast<double>(deltaZ),
			static_cast<double>(scrollingDeltaX),
			static_cast<double>(scrollingDeltaY),
			hasPreciseScrollingDeltas ? 1 : 0,
			directionInvertedFromDevice ? 1 : 0,
			static_cast<unsigned long>(phase),
			static_cast<unsigned long>(momentumPhase),
			static_cast<double>(magnification),
			static_cast<double>(rotation),
			touchDetails);
	}
}

void
appendSwipeTestDiagnosticLine(const char* line)
{
	if (strstr(BARRIER_VERSION, "swipe-test") == NULL) {
		return;
	}

	FILE* file = fopen(kSwipeTestDiagnosticsLogPath, "a");
	if (file == NULL) {
		return;
	}

	fprintf(file, "%s\n", line);
	fclose(file);
}

void
logScrollDiagnosticsEvent(const char* side, CGEventRef event,
							bool isPrimary, bool isOnScreen,
							bool willForward, bool naturalScrolling,
							SInt32 barrierXDelta, SInt32 barrierYDelta,
							SInt32 transportXDelta, SInt32 transportYDelta)
{
	if (!isScrollDiagnosticsEnabled()) {
		return;
	}

	const SInt64 fixedAxis1 = getCGEventIntegerField(
		event, kCGScrollWheelEventFixedPtDeltaAxis1);
	const SInt64 fixedAxis2 = getCGEventIntegerField(
		event, kCGScrollWheelEventFixedPtDeltaAxis2);
	const SInt64 fixedAxis3 = getCGEventIntegerField(
		event, kCGScrollWheelEventFixedPtDeltaAxis3);
	const SInt64 pointAxis1 = getCGEventIntegerField(
		event, kCGScrollWheelEventPointDeltaAxis1);
	const SInt64 pointAxis2 = getCGEventIntegerField(
		event, kCGScrollWheelEventPointDeltaAxis2);
	const SInt64 pointAxis3 = getCGEventIntegerField(
		event, kCGScrollWheelEventPointDeltaAxis3);
	const SInt64 scrollPhase = getCGEventIntegerField(
		event, kCGScrollWheelEventScrollPhase);
	const SInt64 momentumPhase = getCGEventIntegerField(
		event, kCGScrollWheelEventMomentumPhase);
	const SInt64 continuous = getCGEventIntegerField(
		event, kCGScrollWheelEventIsContinuous);
	const CGPoint location = CGEventGetLocation(event);
	char nsDetails[512];
	describeNSEventFromCGEvent(event, nsDetails, sizeof(nsDetails));

	char message[1600];
	snprintf(message, sizeof(message),
		"pid=%d unixTime=%lld macOS scroll diagnostics [%s]: type=%u location=(%.1f,%.1f) "
		"primary=%d onScreen=%d willForward=%d natural=%d "
		"fixed=(%lld,%lld,%lld) fixedFloat=(%.4f,%.4f,%.4f) "
		"point=(%lld,%lld,%lld) phase=%lld momentum=%lld continuous=%lld "
		"barrierLocal=(%d,%d) barrierTransport=(%d,%d) %s",
		static_cast<int>(getpid()),
		static_cast<long long>(time(NULL)),
		side,
		static_cast<unsigned int>(CGEventGetType(event)),
		location.x, location.y, isPrimary ? 1 : 0, isOnScreen ? 1 : 0,
		willForward ? 1 : 0, naturalScrolling ? 1 : 0,
		static_cast<long long>(fixedAxis1),
		static_cast<long long>(fixedAxis2),
		static_cast<long long>(fixedAxis3),
		fixedPointToDouble(fixedAxis1),
		fixedPointToDouble(fixedAxis2),
		fixedPointToDouble(fixedAxis3),
		static_cast<long long>(pointAxis1),
		static_cast<long long>(pointAxis2),
		static_cast<long long>(pointAxis3),
		static_cast<long long>(scrollPhase),
		static_cast<long long>(momentumPhase),
		static_cast<long long>(continuous),
		barrierXDelta, barrierYDelta, transportXDelta, transportYDelta,
		nsDetails);
	LOG((CLOG_NOTE "%s", message));
	appendSwipeTestDiagnosticLine(message);
}

void
logUnhandledInputDiagnosticsEvent(const char* side, CGEventType type,
								  CGEventRef event, bool isPrimary,
								  bool isOnScreen)
{
	if (!isScrollDiagnosticsEnabled()) {
		return;
	}

	const CGPoint location = CGEventGetLocation(event);
	char nsDetails[1024];
	describeNSEventFromCGEvent(event, nsDetails, sizeof(nsDetails));
	char rawDetails[1400];
	describeRawCGEventFields(event, rawDetails, sizeof(rawDetails));

	char message[2800];
	snprintf(message, sizeof(message),
		"pid=%d unixTime=%lld macOS input diagnostics [%s]: type=%u "
		"location=(%.1f,%.1f) primary=%d onScreen=%d %s %s",
		static_cast<int>(getpid()),
		static_cast<long long>(time(NULL)),
		side,
		static_cast<unsigned int>(type),
		location.x, location.y,
		isPrimary ? 1 : 0,
		isOnScreen ? 1 : 0,
		nsDetails,
		rawDetails);
	LOG((CLOG_NOTE "%s", message));
	appendSwipeTestDiagnosticLine(message);
}

static const KeyButton kSpacesSwipeSyntheticControlButton = 0x01fe;
static const KeyButton kSpacesSwipeSyntheticArrowButton = 0x01ff;

void
postControlArrowShortcut(const OSXScreen* screen, KeyID arrowKey)
{
	OSXScreen* mutableScreen = const_cast<OSXScreen*>(screen);

	mutableScreen->fakeKeyDown(kKeyControl_L, KeyModifierControl,
							   kSpacesSwipeSyntheticControlButton);
	mutableScreen->fakeKeyDown(arrowKey, KeyModifierControl,
							   kSpacesSwipeSyntheticArrowButton);
	mutableScreen->fakeKeyUp(kSpacesSwipeSyntheticArrowButton);
	mutableScreen->fakeKeyUp(kSpacesSwipeSyntheticControlButton);
}

void
postSpacesSwipeShortcut(const OSXScreen* screen, bool moveRight,
						const char* source, SInt32 xDelta, SInt32 yDelta,
						double rawSignal, double accumulatedSignal)
{
	if (isSpacesSwipeDirectionInverted()) {
		moveRight = !moveRight;
	}

	const KeyID arrowKey = moveRight ? kKeyRight : kKeyLeft;
	char message[384];
	snprintf(message, sizeof(message),
		"pid=%d macOS Spaces swipe fallback [%s]: xDelta=%d yDelta=%d "
		"rawSignal=%.6f accumulatedRaw=%.6f action=ctrl+%s "
		"postPath=native-keypath",
		static_cast<int>(getpid()), source, xDelta, yDelta,
		rawSignal, accumulatedSignal,
		moveRight ? "right" : "left");
	LOG((CLOG_NOTE "%s", message));
	appendSwipeTestDiagnosticLine(message);
	postControlArrowShortcut(screen, arrowKey);
}

bool
handleSyntheticSpacesSwipeFallback(const OSXScreen* screen, SInt32 xDelta,
								   SInt32 yDelta)
{
	if (!isSpacesSwipeFallbackEnabled() ||
		!OSXScreen::isSyntheticSpacesSwipeWheel(xDelta, yDelta)) {
		return false;
	}

	postSpacesSwipeShortcut(screen, xDelta > 0, "target-synthetic", xDelta,
							yDelta, 0.0, 0.0);
	return true;
}

bool
detectRemoteSpacesSwipeGesture(CGEventType type, CGEventRef event,
							   bool isPrimary, bool isOnScreen,
							   SInt32& syntheticXDelta)
{
	syntheticXDelta = 0;

	if (type != static_cast<CGEventType>(NSEventTypeMagnify) ||
		!isPrimary || isOnScreen ||
		!isSpacesSwipeFallbackEnabled()) {
		return false;
	}

	const double rawXSignal = CGEventGetDoubleValueField(
		event, kMagicMouseSpacesSwipeRawXField);
	SInt32 xDirection = 0;
	if (!OSXScreen::updateSpacesSwipeSourceState(
			g_spacesSwipeSourceState, rawXSignal,
			getMonotonicMilliseconds(),
			getSpacesSwipeSourceThreshold(),
			getSpacesSwipeSourceWindowMs(),
			getSpacesSwipeSourceCooldownMs(), xDirection)) {
		return false;
	}

	syntheticXDelta = xDirection > 0 ? kSpacesSwipeSyntheticWheelDelta :
									   -kSpacesSwipeSyntheticWheelDelta;
	char message[384];
	snprintf(message, sizeof(message),
		"pid=%d macOS Spaces swipe source: rawField124=%.6f "
		"syntheticWheel=(%d,%d)",
		static_cast<int>(getpid()), rawXSignal,
		syntheticXDelta, kSpacesSwipeSyntheticWheelSentinel);
	LOG((CLOG_NOTE "%s", message));
	appendSwipeTestDiagnosticLine(message);
	return true;
}

} // namespace

// TODO: upgrade deprecated function usage in these functions.
void setZeroSuppressionInterval();
void avoidSupression();
void logCursorVisibility();
void avoidHesitatingCursor();

//
// OSXScreen
//

bool					OSXScreen::s_testedForGHOM = false;
bool					OSXScreen::s_hasGHOM	    = false;

OSXScreen::OSXScreen(IEventQueue* events, bool isPrimary, bool autoShowHideCursor) :
	PlatformScreen(events),
	m_isPrimary(isPrimary),
	m_isOnScreen(m_isPrimary),
	m_displayID(kCGNullDirectDisplay),
	m_x(0),
	m_y(0),
	m_w(0),
	m_h(0),
	m_xCenter(0),
	m_yCenter(0),
	m_displayReconfigurationGeneration(0),
	m_displayConfigurationInProgress(false),
	m_displayRefreshRetryTimer(NULL),
	m_displayRefreshRetryEventTarget(0),
	m_displayRefreshRetryArmed(false),
	m_displayRefreshRetryRequestPending(false),
	m_displayRefreshRetryHandlerInstalled(false),
	m_cursorPosValid(false),
	MouseButtonEventMap(NumButtonIDs),
	m_cursorHidden(false),
	m_dragNumButtonsDown(0),
	m_dragTimer(NULL),
	m_keyState(NULL),
	m_sequenceNumber(0),
	m_screensaver(NULL),
	m_screensaverNotify(false),
	m_ownClipboard(false),
	m_clipboardTimer(NULL),
	m_hiddenWindow(NULL),
	m_userInputWindow(NULL),
	m_switchEventHandlerRef(0),
	m_pmMutex(new Mutex),
	m_pmWatchThread(NULL),
	m_pmThreadReady(new CondVar<bool>(m_pmMutex, false)),
	m_pmRootPort(0),
	m_activeModifierHotKey(0),
	m_activeModifierHotKeyMask(0),
	m_eventTapPort(nullptr),
	m_eventTapRLSR(nullptr),
	m_eventTapRunLoop(nullptr),
	m_lastClickTime(0),
	m_clickState(1),
	m_lastSingleClickXCursor(0),
	m_lastSingleClickYCursor(0),
	m_autoShowHideCursor(autoShowHideCursor),
	m_events(events),
	m_getDropTargetThread(NULL),
	m_wakeHoldPid(-1),
	m_wakeHoldExpiry(0),
	m_impl(NULL)
{
	try {
		// Install the thread-safe platform buffer before any capture or
		// CoreGraphics callback can enqueue an event. Replacing it afterward
		// can discard a live-reset retry request while leaving its coalescing
		// bit set, permanently preventing another retry from being queued.
		m_events->adoptBuffer(new SimpleEventQueueBuffer());

		m_events->adoptHandler(
			m_events->forOSXScreen().displayRefreshRetryRequested(),
			&m_displayRefreshRetryEventTarget,
			new TMethodEventJob<OSXScreen>(
				this, &OSXScreen::handleDisplayRefreshRetryRequest));
		m_displayRefreshRetryHandlerInstalled = true;
		m_events->adoptHandler(
			Event::kTimer, &m_displayRefreshRetryEventTarget,
			new TMethodEventJob<OSXScreen>(
				this, &OSXScreen::handleDisplayRefreshRetry));
		m_displayID   = CGMainDisplayID();
		updateScreenShape(m_displayID, 0);
		m_screensaver = new OSXScreenSaver(m_events, getEventTarget());
		m_keyState	  = new OSXKeyState(m_events);

		// only needed when running as a server.
		if (m_isPrimary) {

#if defined(MAC_OS_X_VERSION_10_9)
			// we can't pass options to show the dialog, this must be done by the gui.
			if (!AXIsProcessTrusted()) {
				throw XArch("assistive devices does not trust this process, allow it in system settings.");
			}
#else
			// now deprecated in mavericks.
			if (!AXAPIEnabled()) {
				throw XArch("assistive devices is not enabled, enable it in system settings.");
			}
#endif
		}

		// install display manager notification handler
		CGDisplayRegisterReconfigurationCallback(displayReconfigurationCallback, this);

		// install fast user switching event handler
		EventTypeSpec switchEventTypes[2];
		switchEventTypes[0].eventClass = kEventClassSystem;
		switchEventTypes[0].eventKind  = kEventSystemUserSessionDeactivated;
		switchEventTypes[1].eventClass = kEventClassSystem;
		switchEventTypes[1].eventKind  = kEventSystemUserSessionActivated;
		EventHandlerUPP switchEventHandler =
			NewEventHandlerUPP(userSwitchCallback);
		InstallApplicationEventHandler(switchEventHandler, 2, switchEventTypes,
									   this, &m_switchEventHandlerRef);
		DisposeEventHandlerUPP(switchEventHandler);

		constructMouseButtonEventMap();

		// watch for requests to sleep
		m_events->adoptHandler(m_events->forOSXScreen().confirmSleep(),
								getEventTarget(),
								new TMethodEventJob<OSXScreen>(this,
									&OSXScreen::handleConfirmSleep));

		// create thread for monitoring system power state.
		*m_pmThreadReady = false;
#if defined(MAC_OS_X_VERSION_10_7)
		m_carbonLoopMutex = new Mutex();
		m_carbonLoopReady = new CondVar<bool>(m_carbonLoopMutex, false);
#endif
		LOG((CLOG_DEBUG "starting watchSystemPowerThread"));
        m_pmWatchThread = new Thread([this](){ watchSystemPowerThread(); });
	}
	catch (...) {
		destroyDisplayRefreshRetry();
		m_events->removeHandler(m_events->forOSXScreen().confirmSleep(),
								getEventTarget());
		if (m_switchEventHandlerRef != 0) {
			RemoveEventHandler(m_switchEventHandlerRef);
		}

		CGDisplayRemoveReconfigurationCallback(displayReconfigurationCallback, this);

		delete m_keyState;
		delete m_screensaver;
		throw;
	}

	// install event handlers
	m_events->adoptHandler(Event::kSystem, m_events->getSystemTarget(),
							new TMethodEventJob<OSXScreen>(this,
								&OSXScreen::handleSystemEvent));

}

OSXScreen::~OSXScreen()
{
	// Stop CoreGraphics from re-arming a refresh timer while teardown is in
	// progress.  The callback was previously removed only after most members
	// had already been dismantled.
	CGDisplayRemoveReconfigurationCallback(displayReconfigurationCallback, this);
	destroyDisplayRefreshRetry();
	disable();
	m_events->adoptBuffer(NULL);
	m_events->removeHandler(Event::kSystem, m_events->getSystemTarget());

	if (m_pmWatchThread) {
		// make sure the thread has setup the runloop.
		{
			Lock lock(m_pmMutex);
			while (!(bool)*m_pmThreadReady) {
				m_pmThreadReady->wait();
			}
		}

		// now exit the thread's runloop and wait for it to exit
		LOG((CLOG_DEBUG "stopping watchSystemPowerThread"));
		CFRunLoopStop(m_pmRunloop);
		m_pmWatchThread->wait();
		delete m_pmWatchThread;
		m_pmWatchThread = NULL;
	}
	delete m_pmThreadReady;
	delete m_pmMutex;

	m_events->removeHandler(m_events->forOSXScreen().confirmSleep(),
								getEventTarget());

	RemoveEventHandler(m_switchEventHandlerRef);

	delete m_keyState;
	delete m_screensaver;

#if defined(MAC_OS_X_VERSION_10_7)
	delete m_carbonLoopMutex;
	delete m_carbonLoopReady;
#endif
}

void*
OSXScreen::getEventTarget() const
{
	return const_cast<OSXScreen*>(this);
}

bool
OSXScreen::getClipboard(ClipboardID, IClipboard* dst) const
{
	Clipboard::copy(dst, &m_pasteboard);
	return true;
}

void
OSXScreen::getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
{
	std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
	x = m_x;
	y = m_y;
	w = m_w;
	h = m_h;
}

namespace {

// Resolve the preferred product name for \p displayID from the IOKit
// IODisplayConnect registry, matched by the display's vendor and model
// identifiers (the same identifiers CoreGraphics reports).  Returns an
// empty string only when IOKit cannot provide a product name for the
// display; the empty name is then carried as a valid length-zero DDNM
// entry.
std::string displayNameForID(CGDirectDisplayID displayID)
{
	const UInt32 vendorID = CGDisplayVendorNumber(displayID);
	const UInt32 modelID  = CGDisplayModelNumber(displayID);

	CFDictionaryRef matching = IOServiceMatching("IODisplayConnect");
	if (matching == NULL) {
		return std::string();
	}

	io_iterator_t iterator = 0;
	if (IOServiceGetMatchingServices(kIOMasterPortDefault, matching,
								 &iterator) != KERN_SUCCESS) {
		return std::string();
	}

	std::string name;
	io_service_t service;
	while ((service = IOIteratorNext(iterator)) != 0) {
		CFDictionaryRef info = IODisplayCreateInfoDictionary(service, 0);
		if (info != NULL) {
			UInt32 vendor = 0;
			UInt32 model = 0;
			CFNumberRef vendorRef = (CFNumberRef)CFDictionaryGetValue(info, kDisplayVendorID);
			CFNumberRef modelRef  = (CFNumberRef)CFDictionaryGetValue(info, kDisplayProductID);
			if (vendorRef != NULL && CFGetTypeID(vendorRef) == CFNumberGetTypeID()) {
				CFNumberGetValue(vendorRef, kCFNumberSInt32Type, &vendor);
			}
			if (modelRef != NULL && CFGetTypeID(modelRef) == CFNumberGetTypeID()) {
				CFNumberGetValue(modelRef, kCFNumberSInt32Type, &model);
			}

			if (vendor == vendorID && model == modelID) {
				CFDictionaryRef productNames =
					(CFDictionaryRef)CFDictionaryGetValue(info, kDisplayProductName);
				if (productNames != NULL &&
					CFGetTypeID(productNames) == CFDictionaryGetTypeID()) {
					CFStringRef localized =
						(CFStringRef)CFDictionaryGetValue(productNames, CFSTR("en_US"));
					if (localized == NULL) {
						localized = (CFStringRef)CFDictionaryGetValue(productNames, CFSTR("en"));
					}
					if (localized == NULL && CFDictionaryGetCount(productNames) > 0) {
						// any available localization
						CFStringRef firstKey[1];
						CFDictionaryGetKeysAndValues(productNames,
												 (const void**)firstKey, NULL);
						localized = (CFStringRef)CFDictionaryGetValue(productNames, firstKey[0]);
					}
					if (localized != NULL &&
						CFGetTypeID(localized) == CFStringGetTypeID()) {
						name = [(__bridge NSString*)localized UTF8String];
					}
				}
			}
			CFRelease(info);
		}
		IOObjectRelease(service);
		if (!name.empty()) {
			break;
		}
	}
	IOObjectRelease(iterator);
	return name;
}

std::string stableDisplayIdForID(CGDirectDisplayID displayID)
{
	CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(displayID);
	if (uuid == NULL) {
		return std::string();
	}
	CFStringRef uuidString = CFUUIDCreateString(kCFAllocatorDefault, uuid);
	CFRelease(uuid);
	if (uuidString == NULL) {
		return std::string();
	}

	char buffer[64] = {};
	const bool converted = CFStringGetCString(
		uuidString, buffer, sizeof(buffer), kCFStringEncodingUTF8);
	CFRelease(uuidString);
	if (!converted) {
		return std::string();
	}

	return OSXScreen::normalizeDisplayIdentifier(buffer);
}

} // namespace
OSXScreen::DisplayRefreshDecision
OSXScreen::decideDisplayRefresh(DisplayRefreshRole role,
	bool hasValidSnapshot,
	bool activeQuerySucceeded, CGDisplayCount activeDisplayCount,
	bool onlineQuerySucceeded, CGDisplayCount onlineDisplayCount)
{
	if (activeQuerySucceeded && activeDisplayCount > 0) {
		return {DisplayRefreshSource::Active, false, false};
	}
	if (role == DisplayRefreshRole::PrimaryServer) {
		if (!activeQuerySucceeded) {
			return {DisplayRefreshSource::None, hasValidSnapshot, true};
		}
		return {DisplayRefreshSource::None, false, false};
	}
	if (hasValidSnapshot) {
		return {DisplayRefreshSource::None, true, false};
	}
	if (onlineQuerySucceeded && onlineDisplayCount > 0) {
		return {DisplayRefreshSource::Online, false, false};
	}
	return {DisplayRefreshSource::None, false, false};
}

bool
OSXScreen::displayReconfigurationCaptureReady(
	CGDisplayChangeSummaryFlags flags)
{
	return (flags & kCGDisplayBeginConfigurationFlag) == 0;
}

bool
OSXScreen::displayRefreshGenerationIsCurrent(
	std::uint64_t capturedGeneration, std::uint64_t currentGeneration)
{
	return capturedGeneration == currentGeneration;
}

bool
OSXScreen::eventTapDisableRequiresReenable(CGEventType type)
{
	return type == kCGEventTapDisabledByTimeout ||
		type == kCGEventTapDisabledByUserInput;
}

CFRunLoopRef
OSXScreen::selectEventTapRunLoop(CFRunLoopRef currentRunLoop,
	CFRunLoopRef mainRunLoop)
{
	return mainRunLoop != nullptr ? mainRunLoop : currentRunLoop;
}

std::string
OSXScreen::normalizeDisplayIdentifier(const std::string& identifier)
{
	std::string normalized(identifier);
	for (char& character : normalized) {
		if (character >= 'A' && character <= 'Z') {
			character = static_cast<char>(character - 'A' + 'a');
		}
	}
	return normalized;
}


barrier::DisplayTopology
OSXScreen::topologyFromDisplayRecords(
	const std::vector<TopologyDisplayRecord>& displays)
{
	barrier::DisplayTopology topology;
	topology.displays.reserve(displays.size());
	for (const TopologyDisplayRecord& display : displays) {
		const double quarterTurns = round(display.rotationDegrees / 90.0);
		const double normalizedRotation = quarterTurns * 90.0;
		if (fabs(display.rotationDegrees - normalizedRotation) > 0.01) {
			throw std::invalid_argument("display rotation is not a quarter turn");
		}
		int rotation = static_cast<int>(normalizedRotation) % 360;
		if (rotation < 0) {
			rotation += 360;
		}
		topology.displays.push_back({
			normalizeDisplayIdentifier(display.stableId),
			display.logicalBounds,
			rotation,
			display.primary
		});
	}
	topology.validate();
	return topology.normalized();
}

void
OSXScreen::getDisplays(std::vector<ScreenRect>& displays) const
{
	std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
	// read the snapshot captured during the last geometry refresh; the
	// rectangles are ordered exactly like the names below
	displays.clear();
	displays.reserve(m_displays.size());
	for (std::vector<DisplayEntry>::const_iterator it = m_displays.begin();
		 it != m_displays.end(); ++it) {
		displays.push_back(it->m_rect);
	}
}
barrier::DisplayTopology
OSXScreen::getDisplayTopology() const
{
	std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
	return m_displayTopology;
}


void
OSXScreen::getDisplayNames(std::vector<std::string>& names) const
{
	std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
	// read the same snapshot as getDisplays(), so names always line up
	// with the DDIS rectangles -- no independent display query, no
	// count matching
	names.clear();
	names.reserve(m_displays.size());
	for (std::vector<DisplayEntry>::const_iterator it = m_displays.begin();
		 it != m_displays.end(); ++it) {
		names.push_back(it->m_name);
	}
}

void
OSXScreen::getCursorPos(SInt32& x, SInt32& y) const
{
	CGEventRef event = CGEventCreate(NULL);
	CGPoint mouse = CGEventGetLocation(event);
	x                = mouse.x;
	y                = mouse.y;
	m_cursorPosValid = true;
	m_xCursor        = x;
	m_yCursor        = y;
	CFRelease(event);
}

void
OSXScreen::reconfigure(UInt32)
{
	// do nothing
}

void
OSXScreen::warpCursor(SInt32 x, SInt32 y)
{
	// move cursor without generating events
	CGPoint pos;
	pos.x = x;
	pos.y = y;
	CGWarpMouseCursorPosition(pos);

	// save new cursor position
	m_xCursor        = x;
	m_yCursor        = y;
	m_cursorPosValid = true;
}

void
OSXScreen::fakeInputBegin()
{
	// FIXME -- not implemented
}

void
OSXScreen::fakeInputEnd()
{
	// FIXME -- not implemented
}

SInt32
OSXScreen::getJumpZoneSize() const
{
	return 1;
}

bool
OSXScreen::isAnyMouseButtonDown(UInt32& buttonID) const
{
	if (m_buttonState.test(0)) {
		buttonID = kButtonLeft;
		return true;
	}

	return (GetCurrentButtonState() != 0);
}

void
OSXScreen::getCursorCenter(SInt32& x, SInt32& y) const
{
	std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
	x = m_xCenter;
	y = m_yCenter;
}

UInt32
OSXScreen::registerHotKey(KeyID key, KeyModifierMask mask)
{
	// get mac virtual key and modifier mask matching barrier key and mask
	UInt32 macKey, macMask;
	if (!m_keyState->mapBarrierHotKeyToMac(key, mask, macKey, macMask)) {
		LOG((CLOG_DEBUG "could not map hotkey id=%04x mask=%04x", key, mask));
		return 0;
	}

	// choose hotkey id
	UInt32 id;
	if (!m_oldHotKeyIDs.empty()) {
		id = m_oldHotKeyIDs.back();
		m_oldHotKeyIDs.pop_back();
	}
	else {
		id = m_hotKeys.size() + 1;
	}

	// if this hot key has modifiers only then we'll handle it specially
	EventHotKeyRef ref = NULL;
	bool okay;
	if (key == kKeyNone) {
		if (m_modifierHotKeys.count(mask) > 0) {
			// already registered
			okay = false;
		}
		else {
			m_modifierHotKeys[mask] = id;
			okay = true;
		}
	}
	else {
		EventHotKeyID hkid = { 'SNRG', (UInt32)id };
		OSStatus status = RegisterEventHotKey(macKey, macMask, hkid,
								GetApplicationEventTarget(), 0,
								&ref);
		okay = (status == noErr);
		m_hotKeyToIDMap[HotKeyItem(macKey, macMask)] = id;
	}

	if (!okay) {
		m_oldHotKeyIDs.push_back(id);
		m_hotKeyToIDMap.erase(HotKeyItem(macKey, macMask));
		LOG((CLOG_WARN "failed to register hotkey %s (id=%04x mask=%04x)", barrier::KeyMap::formatKey(key, mask).c_str(), key, mask));
		return 0;
	}

	m_hotKeys.insert(std::make_pair(id, HotKeyItem(ref, macKey, macMask)));

	LOG((CLOG_DEBUG "registered hotkey %s (id=%04x mask=%04x) as id=%d", barrier::KeyMap::formatKey(key, mask).c_str(), key, mask, id));
	return id;
}

void
OSXScreen::unregisterHotKey(UInt32 id)
{
	// look up hotkey
	HotKeyMap::iterator i = m_hotKeys.find(id);
	if (i == m_hotKeys.end()) {
		return;
	}

	// unregister with OS
	bool okay;
	if (i->second.getRef() != NULL) {
		okay = (UnregisterEventHotKey(i->second.getRef()) == noErr);
	}
	else {
		okay = false;
		// XXX -- this is inefficient
		for (ModifierHotKeyMap::iterator j = m_modifierHotKeys.begin();
								j != m_modifierHotKeys.end(); ++j) {
			if (j->second == id) {
				m_modifierHotKeys.erase(j);
				okay = true;
				break;
			}
		}
	}
	if (!okay) {
		LOG((CLOG_WARN "failed to unregister hotkey id=%d", id));
	}
	else {
		LOG((CLOG_DEBUG "unregistered hotkey id=%d", id));
	}

	// discard hot key from map and record old id for reuse
	m_hotKeyToIDMap.erase(i->second);
	m_hotKeys.erase(i);
	m_oldHotKeyIDs.push_back(id);
	if (m_activeModifierHotKey == id) {
		m_activeModifierHotKey     = 0;
		m_activeModifierHotKeyMask = 0;
	}
}

void
OSXScreen::constructMouseButtonEventMap()
{
	const CGEventType source[NumButtonIDs][3] = {
		{kCGEventLeftMouseUp, kCGEventLeftMouseDragged, kCGEventLeftMouseDown},
		{kCGEventRightMouseUp, kCGEventRightMouseDragged, kCGEventRightMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown}
	};

	for (UInt16 button = 0; button < NumButtonIDs; button++) {
		MouseButtonEventMapType new_map;
		for (UInt16 state = (UInt32) kMouseButtonUp; state < kMouseButtonStateMax; state++) {
			CGEventType curEvent = source[button][state];
			new_map[state] = curEvent;
		}
		MouseButtonEventMap[button] = new_map;
	}
}

void
OSXScreen::postMouseEvent(CGPoint& pos) const
{
	// check if cursor position is valid on the client display configuration
	// stkamp@users.sourceforge.net
	CGDisplayCount displayCount = 0;
	CGGetDisplaysWithPoint(pos, 0, NULL, &displayCount);
	if (displayCount == 0) {
		// cursor position invalid - clamp to bounds of last valid display.
		// find the last valid display using the last cursor position.
		displayCount = 0;
		CGDirectDisplayID displayID;
		CGGetDisplaysWithPoint(CGPointMake(m_xCursor, m_yCursor), 1,
								&displayID, &displayCount);
		if (displayCount != 0) {
			CGRect displayRect = CGDisplayBounds(displayID);
			if (pos.x < displayRect.origin.x) {
				pos.x = displayRect.origin.x;
			}
			else if (pos.x > displayRect.origin.x +
								displayRect.size.width - 1) {
				pos.x = displayRect.origin.x + displayRect.size.width - 1;
			}
			if (pos.y < displayRect.origin.y) {
				pos.y = displayRect.origin.y;
			}
			else if (pos.y > displayRect.origin.y +
								displayRect.size.height - 1) {
				pos.y = displayRect.origin.y + displayRect.size.height - 1;
			}
		}
	}

	CGEventType type = kCGEventMouseMoved;

	SInt8 button = m_buttonState.getFirstButtonDown();
	if (button != -1) {
		MouseButtonEventMapType thisButtonType = MouseButtonEventMap[button];
		type = thisButtonType[kMouseButtonDragged];
	}

	CGEventRef event = CGEventCreateMouseEvent(NULL, type, pos, static_cast<CGMouseButton>(button));

    // Dragging events also need the click state
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, m_clickState);

    // Fix for sticky keys
    CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
    CGEventSetFlags(event, modifiers);

    // Set movement deltas to fix issues with certain 3D programs
    SInt64 deltaX = pos.x;
    deltaX -= m_xCursor;

    SInt64 deltaY = pos.y;
    deltaY -= m_yCursor;

    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, deltaX);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, deltaY);

    double deltaFX = deltaX;
    double deltaFY = deltaY;

    CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, deltaFX);
    CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, deltaFY);

	CGEventPost(kCGHIDEventTap, event);

	CFRelease(event);
}

void
OSXScreen::fakeMouseButton(ButtonID id, bool press)
{
	// Buttons are indexed from one, but the button down array is indexed from zero
	UInt32 index = mapBarrierButtonToMac(id) - kButtonLeft;
	if (index >= NumButtonIDs) {
		return;
	}

	CGPoint pos;
	if (!m_cursorPosValid) {
		SInt32 x, y;
		getCursorPos(x, y);
	}
	pos.x = m_xCursor;
	pos.y = m_yCursor;

	// variable used to detect mouse coordinate differences between
	// old & new mouse clicks. Used in double click detection.
	SInt32 xDiff = m_xCursor - m_lastSingleClickXCursor;
	SInt32 yDiff = m_yCursor - m_lastSingleClickYCursor;
	double diff = sqrt(xDiff * xDiff + yDiff * yDiff);
	// max sqrt(x^2 + y^2) difference allowed to double click
	// since we don't have double click distance in NX APIs
	// we define our own defaults.
	const double maxDiff = sqrt(2) + 0.0001;

    double clickTime = [NSEvent doubleClickInterval];

    // As long as the click is within the time window and distance window
    // increase clickState (double click, triple click, etc)
    // This will allow for higher than triple click but the quartz documentation
    // does not specify that this should be limited to triple click
    if (press) {
        if ((ARCH->time() - m_lastClickTime) <= clickTime && diff <= maxDiff) {
            m_clickState++;
        }
        else {
            m_clickState = 1;
        }

        m_lastClickTime = ARCH->time();
    }

    if (m_clickState == 1) {
        m_lastSingleClickXCursor = m_xCursor;
        m_lastSingleClickYCursor = m_yCursor;
    }

    EMouseButtonState state = press ? kMouseButtonDown : kMouseButtonUp;

    LOG((CLOG_DEBUG1 "faking mouse button id: %d press: %s", index, press ? "pressed" : "released"));

    MouseButtonEventMapType thisButtonMap = MouseButtonEventMap[index];
    CGEventType type = thisButtonMap[state];

    CGEventRef event = CGEventCreateMouseEvent(NULL, type, pos, static_cast<CGMouseButton>(index));

    CGEventSetIntegerValueField(event, kCGMouseEventClickState, m_clickState);

    // Fix for sticky keys
    CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
    CGEventSetFlags(event, modifiers);

    m_buttonState.set(index, state);
    CGEventPost(kCGHIDEventTap, event);

    CFRelease(event);

	if (!press && (id == kButtonLeft)) {
		if (m_fakeDraggingStarted) {
            m_getDropTargetThread = new Thread([this](){ get_drop_target_thread(); });
		}

		m_draggingStarted = false;
	}
}

void OSXScreen::get_drop_target_thread()
{
#if defined(MAC_OS_X_VERSION_10_7)
	char* cstr = NULL;

	// wait for 5 secs for the drop destinaiton string to be filled.
	UInt32 timeout = ARCH->time() + 5;

	while (ARCH->time() < timeout) {
		CFStringRef cfstr = getCocoaDropTarget();
		cstr = CFStringRefToUTF8String(cfstr);
		CFRelease(cfstr);

		if (cstr != NULL) {
			break;
		}
		ARCH->sleep(.1f);
	}

	if (cstr != NULL) {
		LOG((CLOG_DEBUG "drop target: %s", cstr));
		m_dropTarget = cstr;
	}
	else {
		LOG((CLOG_ERR "failed to get drop target"));
		m_dropTarget.clear();
	}
#else
	LOG((CLOG_WARN "drag drop not supported"));
#endif
	m_fakeDraggingStarted = false;
}

void
OSXScreen::fakeMouseMove(SInt32 x, SInt32 y)
{
	if (m_fakeDraggingStarted) {
		m_buttonState.set(0, kMouseButtonDown);
	}

	// index 0 means left mouse button
	if (m_buttonState.test(0)) {
		m_draggingStarted = true;
	}

	// synthesize event
	CGPoint pos;
	pos.x = x;
	pos.y = y;
	postMouseEvent(pos);

	// save new cursor position
	m_xCursor        = static_cast<SInt32>(pos.x);
	m_yCursor        = static_cast<SInt32>(pos.y);
	m_cursorPosValid = true;
}

void
OSXScreen::fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const
{
	// OS X does not appear to have a fake relative mouse move function.
	// simulate it by getting the current mouse position and adding to
	// that.  this can yield the wrong answer but there's not much else
	// we can do.

	// get current position
	CGEventRef event = CGEventCreate(NULL);
	CGPoint oldPos = CGEventGetLocation(event);
	CFRelease(event);

	// synthesize event
	CGPoint pos;
	m_xCursor = static_cast<SInt32>(oldPos.x);
	m_yCursor = static_cast<SInt32>(oldPos.y);
	pos.x     = oldPos.x + dx;
	pos.y     = oldPos.y + dy;
	postMouseEvent(pos);

	// we now assume we don't know the current cursor position
	m_cursorPosValid = false;
}

void
OSXScreen::fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const
{
	if (xDelta != 0 || yDelta != 0) {
		const SInt32 transportXDelta = xDelta;
		const SInt32 transportYDelta = yDelta;

		if (handleSyntheticSpacesSwipeFallback(this, transportXDelta,
											   transportYDelta)) {
			return;
		}

		// convert the transport convention into this host's local
		// convention before injecting, so the perceived content
		// direction matches the source host
		normalizeScrollDeltas(getNaturalScrolling(), xDelta, yDelta);

		// create a scroll event, post it and release it.  not sure if kCGScrollEventUnitLine
		// is the right choice here over kCGScrollEventUnitPixel
		CGEventRef scrollEvent = CGEventCreateScrollWheelEvent(
			NULL, kCGScrollEventUnitLine, 2,
			mapScrollWheelFromBarrier(yDelta),
			-mapScrollWheelFromBarrier(xDelta));

        // Fix for sticky keys
        CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
        CGEventSetFlags(scrollEvent, modifiers);

		logScrollDiagnosticsEvent("target-post", scrollEvent,
								  m_isPrimary, m_isOnScreen, false,
								  getNaturalScrolling(),
								  xDelta, yDelta,
								  transportXDelta, transportYDelta);

		CGEventPost(kCGHIDEventTap, scrollEvent);
		CFRelease(scrollEvent);
	}
}

void
OSXScreen::showCursor()
{
	LOG((CLOG_DEBUG "showing cursor"));

	CFStringRef propertyString = CFStringCreateWithCString(
		NULL, "SetsCursorInBackground", kCFStringEncodingMacRoman);

	CGSSetConnectionProperty(
		_CGSDefaultConnection(), _CGSDefaultConnection(),
		propertyString, kCFBooleanTrue);

	CFRelease(propertyString);

	CGDirectDisplayID displayID;
	{
		std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
		displayID = m_displayID;
	}
	CGError error = CGDisplayShowCursor(displayID);
	if (error != kCGErrorSuccess) {
		LOG((CLOG_ERR "failed to show cursor, error=%d", error));
	}

	// appears to fix "mouse randomly not showing" bug
	CGAssociateMouseAndMouseCursorPosition(true);

	logCursorVisibility();

	m_cursorHidden = false;
}

void
OSXScreen::hideCursor()
{
	LOG((CLOG_DEBUG "hiding cursor"));

	CFStringRef propertyString = CFStringCreateWithCString(
		NULL, "SetsCursorInBackground", kCFStringEncodingMacRoman);

	CGSSetConnectionProperty(
		_CGSDefaultConnection(), _CGSDefaultConnection(),
		propertyString, kCFBooleanTrue);

	CFRelease(propertyString);

	CGDirectDisplayID displayID;
	{
		std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
		displayID = m_displayID;
	}
	CGError error = CGDisplayHideCursor(displayID);
	if (error != kCGErrorSuccess) {
		LOG((CLOG_ERR "failed to hide cursor, error=%d", error));
	}

	// appears to fix "mouse randomly not hiding" bug
	CGAssociateMouseAndMouseCursorPosition(true);

	logCursorVisibility();

	m_cursorHidden = true;
}

void
OSXScreen::enable()
{
	// watch the clipboard
	m_clipboardTimer = m_events->newTimer(1.0, NULL);
	m_events->adoptHandler(Event::kTimer, m_clipboardTimer,
							new TMethodEventJob<OSXScreen>(this,
								&OSXScreen::handleClipboardCheck));

	if (m_isPrimary) {
		// FIXME -- start watching jump zones

		// kCGEventTapOptionDefault = 0x00000000 (Missing in 10.4, so specified literally)
		m_eventTapPort = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
										kCGEventMaskForAllEvents,
										handleCGInputEvent,
										this);
	}
	else {
		// FIXME -- prevent system from entering power save mode

		if (m_autoShowHideCursor) {
			hideCursor();
		}

		// warp the mouse to the cursor center
		SInt32 centerX;
		SInt32 centerY;
		getCursorCenter(centerX, centerY);
		fakeMouseMove(centerX, centerY);

                // there may be a better way to do this, but we register an event handler even if we're
                // not on the primary display (acting as a client). This way, if a local event comes in
                // (either keyboard or mouse), we can make sure to show the cursor if we've hidden it.
		m_eventTapPort = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
										kCGEventMaskForAllEvents,
										handleCGInputEventSecondary,
										this);
	}

	if (!m_eventTapPort) {
		LOG((CLOG_ERR "failed to create quartz event tap"));
		return;
	}

	m_eventTapRLSR = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_eventTapPort, 0);
	if (!m_eventTapRLSR) {
		LOG((CLOG_ERR "failed to create a CFRunLoopSourceRef for the quartz event tap"));
		return;
	}

	// Power-resume handlers run on Barrier's event-queue worker. That worker
	// does not pump a Core Foundation run loop, so a tap source attached to
	// CFRunLoopGetCurrent() there never receives mouse or keyboard events.
	// The Cocoa loop always runs on the process main thread.
	m_eventTapRunLoop = selectEventTapRunLoop(
		CFRunLoopGetCurrent(), CFRunLoopGetMain());
	CFRetain(m_eventTapRunLoop);
	CFRunLoopAddSource(m_eventTapRunLoop, m_eventTapRLSR,
		kCFRunLoopDefaultMode);
	CFRunLoopWakeUp(m_eventTapRunLoop);
}

void
OSXScreen::disable()
{
	if (m_autoShowHideCursor) {
		showCursor();
	}

	// FIXME -- stop watching jump zones, stop capturing input

	if (m_eventTapRLSR) {
		if (m_eventTapRunLoop) {
			CFRunLoopRemoveSource(m_eventTapRunLoop, m_eventTapRLSR,
				kCFRunLoopDefaultMode);
			CFRunLoopWakeUp(m_eventTapRunLoop);
		}
		CFRelease(m_eventTapRLSR);
		m_eventTapRLSR = nullptr;
	}
	if (m_eventTapRunLoop) {
		CFRelease(m_eventTapRunLoop);
		m_eventTapRunLoop = nullptr;
	}

	if (m_eventTapPort) {
		CGEventTapEnable(m_eventTapPort, false);
		CFRelease(m_eventTapPort);
		m_eventTapPort = nullptr;
	}
	// FIXME -- allow system to enter power saving mode

	// stop any wake hold so the display is free to sleep again
	stopWakeHold();

	// disable drag handling
	m_dragNumButtonsDown = 0;
	enableDragTimer(false);

	// uninstall clipboard timer
	if (m_clipboardTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_clipboardTimer);
		m_events->deleteTimer(m_clipboardTimer);
		m_clipboardTimer = NULL;
	}

	m_isOnScreen = m_isPrimary;
}

void
OSXScreen::enter()
{
	showCursor();

	if (m_isPrimary) {
		setZeroSuppressionInterval();
	}
	else {
		// reset buttons
		m_buttonState.reset();

		// wake the client display only when it is actually asleep; an
		// awake target gets zero wake actions
		wakeEnteredDisplay();

		avoidSupression();
	}

	// now on screen
	m_isOnScreen = true;
}

bool
OSXScreen::leave()
{
    hideCursor();

	if (isDraggingStarted()) {
		String& fileList = getDraggingFilename();

		if (!m_isPrimary) {
			if (fileList.empty() == false) {
				ClientApp& app = ClientApp::instance();
				Client* client = app.getClientPtr();

				DragInformation di;
				di.setFilename(fileList);
				DragFileList dragFileList;
				dragFileList.push_back(di);
				String info;
				UInt32 fileCount = DragInformation::setupDragInfo(
					dragFileList, info);
				client->sendDragInfo(fileCount, info, info.size());
				LOG((CLOG_DEBUG "send dragging file to server"));

				// TODO: what to do with multiple file or even
				// a folder
				client->sendFileToServer(fileList.c_str());
			}
		}
		m_draggingStarted = false;
	}

	if (m_isPrimary) {
		avoidHesitatingCursor();

	}

	// now off screen
	m_isOnScreen = false;

	return true;
}

int
OSXScreen::displayIndexAt(const std::vector<ScreenRect>& displays,
                          SInt32 x, SInt32 y)
{
	for (std::size_t i = 0; i < displays.size(); ++i) {
		const ScreenRect& rect = displays[i];
		if (x >= rect.x && x < rect.x + rect.w &&
			y >= rect.y && y < rect.y + rect.h) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool
OSXScreen::shouldRequestWake(bool displayAsleep, SInt32 now, SInt32 holdExpiry)
{
	if (!displayAsleep) {
		return false;
	}
	// an active hold (now < holdExpiry) absorbs the entry as a refresh;
	// a new child is only needed when none is running
	return now >= holdExpiry;
}

SInt32
OSXScreen::naturalScrollDirectionSign(bool naturalScrolling)
{
	return naturalScrolling ? -1 : 1;
}

void
OSXScreen::normalizeScrollDeltas(bool naturalScrolling,
								SInt32& xDelta, SInt32& yDelta)
{
	const SInt32 sign = naturalScrollDirectionSign(naturalScrolling);
	xDelta *= sign;
	yDelta *= sign;
}

bool
OSXScreen::shouldForwardRemoteWheel(bool isPrimary, bool isOnScreen)
{
	return isPrimary && !isOnScreen;
}

bool
OSXScreen::shouldEnableScrollDiagnostics(const char* value)
{
	if (value == NULL || value[0] == '\0') {
		return false;
	}

	return strcmp(value, "0") != 0 &&
		   strcmp(value, "false") != 0 &&
		   strcmp(value, "FALSE") != 0 &&
		   strcmp(value, "off") != 0 &&
		   strcmp(value, "OFF") != 0 &&
		   strcmp(value, "no") != 0 &&
		   strcmp(value, "NO") != 0;
}

bool
OSXScreen::shouldEnableSpacesSwipeFallback(const char* value)
{
	return shouldEnableScrollDiagnostics(value);
}

bool
OSXScreen::isSyntheticSpacesSwipeWheel(SInt32 xDelta, SInt32 yDelta)
{
	return yDelta == kSpacesSwipeSyntheticWheelSentinel &&
		   (xDelta == kSpacesSwipeSyntheticWheelDelta ||
			xDelta == -kSpacesSwipeSyntheticWheelDelta);
}

bool
OSXScreen::updateSpacesSwipeSourceState(SpacesSwipeSourceState& state,
										double xSignal,
										SInt64 nowTimeMs,
										double threshold,
										SInt32 windowMs,
										SInt32 cooldownMs,
										SInt32& xDirection)
{
	xDirection = 0;

	if (threshold <= 0.0 || windowMs <= 0 || cooldownMs < 0) {
		state = SpacesSwipeSourceState();
		return false;
	}

	if (nowTimeMs < state.cooldownUntilTimeMs) {
		return false;
	}

	if (fabs(xSignal) < 0.000001) {
		return false;
	}

	const bool expired = state.lastEventTimeMs == 0 ||
		nowTimeMs - state.lastEventTimeMs > windowMs;
	const bool changedDirection = state.accumulatedXSignal != 0.0 &&
		((state.accumulatedXSignal > 0.0) != (xSignal > 0.0));
	if (expired || changedDirection) {
		state.accumulatedXSignal = 0.0;
	}

	state.accumulatedXSignal += xSignal;
	state.lastEventTimeMs = nowTimeMs;

	if (fabs(state.accumulatedXSignal) < threshold) {
		return false;
	}

	xDirection = state.accumulatedXSignal > 0.0 ? 1 : -1;
	state.accumulatedXSignal = 0.0;
	state.lastEventTimeMs = 0;
	state.cooldownUntilTimeMs = nowTimeMs + cooldownMs;
	return true;
}

void
OSXScreen::wakeEnteredDisplay()
{
	// Client::enter() applies the received entry coordinate via
	// fakeMouseMove() immediately before calling enter(), so the saved
	// cursor position is the entry point; fall back to the live cursor
	// if it was never recorded.
	SInt32 x = m_xCursor;
	SInt32 y = m_yCursor;
	if (!m_cursorPosValid) {
		getCursorPos(x, y);
	}

	// Resolve the entry coordinate and physical display identifier from
	// one display generation.
	CGDirectDisplayID target = kCGNullDirectDisplay;
	{
		std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
		std::vector<ScreenRect> displays;
		displays.reserve(m_displays.size());
		for (const DisplayEntry& display : m_displays) {
			displays.push_back(display.m_rect);
		}
		const int index = displayIndexAt(displays, x, y);
		if (index >= 0) {
			target = m_displays[index].m_id;
		}
	}
	if (target == kCGNullDirectDisplay) {
		LOG((CLOG_WARN "wake: entry %d,%d not on any display; "
		     "skipping wake hold", x, y));
		return;
	}
	if (!CGDisplayIsAsleep(target)) {
		LOG((CLOG_DEBUG1 "wake: target display %u is awake; no wake hold",
		     target));
		return;
	}

	// target is asleep: start one debounced short hold, or refresh the
	// active hold instead of stacking a second caffeinate child
	const SInt32 now = static_cast<SInt32>(time(NULL));
	if (shouldRequestWake(true, now, m_wakeHoldExpiry)) {
		startWakeHold(now);
	} else {
		refreshWakeHold(now);
	}
}

void
OSXScreen::startWakeHold(SInt32 now)
{
	if (m_wakeHoldPid > 0) {
		// a hold is already active; refresh it, never stack a second
		refreshWakeHold(now);
		return;
	}

	char timeout[16];
	snprintf(timeout, sizeof(timeout), "%d", kWakeHoldSeconds);
	pid_t pid = 0;
	char* const argv[] = {
		const_cast<char*>(kWakeHoldPath),
		const_cast<char*>("-u"),
		const_cast<char*>("-d"),
		const_cast<char*>("-t"),
		timeout,
		NULL
	};
	extern char** environ;
	const int rc = posix_spawn(&pid, kWakeHoldPath, NULL, NULL, argv, environ);
	if (rc != 0) {
		LOG((CLOG_WARN "wake: failed to start %s: %s; pointer entry continues",
		     kWakeHoldPath, strerror(rc)));
		return;
	}

	m_wakeHoldPid = pid;
	m_wakeHoldExpiry = now + kWakeHoldSeconds;
	LOG((CLOG_DEBUG "wake: holding display awake via %s (pid %d, %ds)",
	     kWakeHoldPath, static_cast<int>(pid), kWakeHoldSeconds));
}

void
OSXScreen::refreshWakeHold(SInt32 now)
{
	// refresh instead of stacking: replace the active child so the 15s
	// window restarts with exactly one caffeinate hold running
	stopWakeHold();
	startWakeHold(now);
}

void
OSXScreen::stopWakeHold()
{
	if (m_wakeHoldPid <= 0) {
		m_wakeHoldPid = -1;
		m_wakeHoldExpiry = 0;
		return;
	}

	// reap a child that already exited; the zombie holds the pid so it
	// cannot be recycled into another process before we are done with it
	int status = 0;
	pid_t result = waitpid(m_wakeHoldPid, &status, WNOHANG);
	if (result == 0) {
		// still running: terminate it and reap promptly
		kill(m_wakeHoldPid, SIGTERM);
		for (int i = 0; i < 20; ++i) {
			if (waitpid(m_wakeHoldPid, &status, WNOHANG) == m_wakeHoldPid) {
				break;
			}
			usleep(10 * 1000); // 10ms, bounded to ~200ms
		}
	}

	m_wakeHoldPid = -1;
	m_wakeHoldExpiry = 0;
}

bool
OSXScreen::setClipboard(ClipboardID, const IClipboard* src)
{
	if (src != NULL) {
		LOG((CLOG_DEBUG "setting clipboard"));
		Clipboard::copy(&m_pasteboard, src);
	}
	return true;
}

void
OSXScreen::checkClipboards()
{
	LOG((CLOG_DEBUG2 "checking clipboard"));
	if (m_pasteboard.synchronize()) {
		LOG((CLOG_DEBUG "clipboard changed"));
		sendClipboardEvent(m_events->forClipboard().clipboardGrabbed(), kClipboardClipboard);
		sendClipboardEvent(m_events->forClipboard().clipboardGrabbed(), kClipboardSelection);
	}
}

void
OSXScreen::openScreensaver(bool notify)
{
	m_screensaverNotify = notify;
	if (!m_screensaverNotify) {
		m_screensaver->disable();
	}
}

void
OSXScreen::closeScreensaver()
{
	if (!m_screensaverNotify) {
		m_screensaver->enable();
	}
}

void
OSXScreen::screensaver(bool activate)
{
	if (activate) {
		m_screensaver->activate();
	}
	else {
		m_screensaver->deactivate();
	}
}

void
OSXScreen::resetOptions()
{
	// no options
}

void
OSXScreen::setOptions(const OptionsList&)
{
	// no options
}

void
OSXScreen::setSequenceNumber(UInt32 seqNum)
{
	m_sequenceNumber = seqNum;
}

bool
OSXScreen::isPrimary() const
{
	return m_isPrimary;
}

void
OSXScreen::sendEvent(Event::Type type, void* data) const
{
	m_events->addEvent(Event(type, getEventTarget(), data));
}

void
OSXScreen::sendClipboardEvent(Event::Type type, ClipboardID id) const
{
	ClipboardInfo* info   = (ClipboardInfo*)malloc(sizeof(ClipboardInfo));
	info->m_id             = id;
	info->m_sequenceNumber = m_sequenceNumber;
	sendEvent(type, info);
}

void
OSXScreen::handleSystemEvent(const Event& event, void*)
{
	EventRef* carbonEvent = static_cast<EventRef*>(event.getData());
	assert(carbonEvent != NULL);

	UInt32 eventClass = GetEventClass(*carbonEvent);

	switch (eventClass) {
	case kEventClassMouse:
		switch (GetEventKind(*carbonEvent)) {
		case kBarrierEventMouseScroll:
		{
			OSStatus r;
			long xScroll;
			long yScroll;

			// get scroll amount
			r = GetEventParameter(*carbonEvent,
					kBarrierMouseScrollAxisX,
					typeSInt32,
					NULL,
					sizeof(xScroll),
					NULL,
					&xScroll);
			if (r != noErr) {
				xScroll = 0;
			}
			r = GetEventParameter(*carbonEvent,
					kBarrierMouseScrollAxisY,
					typeSInt32,
					NULL,
					sizeof(yScroll),
					NULL,
					&yScroll);
			if (r != noErr) {
				yScroll = 0;
			}

			if (xScroll != 0 || yScroll != 0) {
				onMouseWheel(-mapScrollWheelToBarrier(xScroll),
								mapScrollWheelToBarrier(yScroll));
			}
		}
		}
		break;

	case kEventClassKeyboard:
			switch (GetEventKind(*carbonEvent)) {
				case kEventHotKeyPressed:
				case kEventHotKeyReleased:
					onHotKey(*carbonEvent);
					break;
			}

			break;

	case kEventClassWindow:
		// 2nd param was formerly GetWindowEventTarget(m_userInputWindow) which is 32-bit only,
		// however as m_userInputWindow is never initialized to anything we can take advantage of
		// the fact that GetWindowEventTarget(NULL) == NULL
		SendEventToEventTarget(*carbonEvent, NULL);
		switch (GetEventKind(*carbonEvent)) {
		case kEventWindowActivated:
			LOG((CLOG_DEBUG1 "window activated"));
			break;

		case kEventWindowDeactivated:
			LOG((CLOG_DEBUG1 "window deactivated"));
			break;

		case kEventWindowFocusAcquired:
			LOG((CLOG_DEBUG1 "focus acquired"));
			break;

		case kEventWindowFocusRelinquish:
			LOG((CLOG_DEBUG1 "focus released"));
			break;
		}
		break;

	default:
		SendEventToEventTarget(*carbonEvent, GetEventDispatcherTarget());
		break;
	}
}

bool
OSXScreen::onMouseMove(CGFloat mx, CGFloat my)
{
	LOG((CLOG_DEBUG2 "mouse move %+f,%+f", mx, my));
	SInt32 screenX;
	SInt32 screenY;
	SInt32 screenWidth;
	SInt32 screenHeight;
	SInt32 centerX;
	SInt32 centerY;
	{
		std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
		screenX = m_x;
		screenY = m_y;
		screenWidth = m_w;
		screenHeight = m_h;
		centerX = m_xCenter;
		centerY = m_yCenter;
	}

	CGFloat x = mx - m_xCursor;
	CGFloat y = my - m_yCursor;

	if ((x == 0 && y == 0) || (mx == centerX && mx == centerY)) {
		return true;
	}

	// save position to compute delta of next motion
	m_xCursor = (SInt32)mx;
	m_yCursor = (SInt32)my;

	if (m_isOnScreen) {
		// motion on primary screen
		sendEvent(m_events->forIPrimaryScreen().motionOnPrimary(),
							MotionInfo::alloc(m_xCursor, m_yCursor));
		if (m_buttonState.test(0)) {
			m_draggingStarted = true;
		}
	}
	else {
		// motion on secondary screen.  warp mouse back to
		// center.
		warpCursor(centerX, centerY);

		// examine the motion.  if it's about the distance
		// from the center of the screen to an edge then
		// it's probably a bogus motion that we want to
		// ignore (see warpCursorNoFlush() for a further
		// description).
		static SInt32 bogusZoneSize = 10;
		if (-x + bogusZoneSize > centerX - screenX ||
			 x + bogusZoneSize > screenX + screenWidth - centerX ||
			-y + bogusZoneSize > centerY - screenY ||
			 y + bogusZoneSize > screenY + screenHeight - centerY) {
			LOG((CLOG_DEBUG "dropped bogus motion %+d,%+d", x, y));
		}
		else {
			// send motion
			// Accumulate together the move into the running total
			static CGFloat m_xFractionalMove = 0;
			static CGFloat m_yFractionalMove = 0;

			m_xFractionalMove += x;
			m_yFractionalMove += y;

			// Return the integer part
			SInt32 intX = (SInt32)m_xFractionalMove;
			SInt32 intY = (SInt32)m_yFractionalMove;

			// And keep only the fractional part
			m_xFractionalMove -= intX;
			m_yFractionalMove -= intY;
			sendEvent(m_events->forIPrimaryScreen().motionOnSecondary(), MotionInfo::alloc(intX, intY));
		}
	}

	return true;
}

bool
OSXScreen::onMouseButton(bool pressed, UInt16 macButton)
{
	// Buttons 2 and 3 are inverted on the mac
	ButtonID button = mapMacButtonToBarrier(macButton);

	if (pressed) {
		LOG((CLOG_DEBUG1 "event: button press button=%d", button));
		if (button != kButtonNone) {
			KeyModifierMask mask = m_keyState->getActiveModifiers();
			sendEvent(m_events->forIPrimaryScreen().buttonDown(), ButtonInfo::alloc(button, mask));
		}
	}
	else {
		LOG((CLOG_DEBUG1 "event: button release button=%d", button));
		if (button != kButtonNone) {
			KeyModifierMask mask = m_keyState->getActiveModifiers();
			sendEvent(m_events->forIPrimaryScreen().buttonUp(), ButtonInfo::alloc(button, mask));
		}
	}

	// handle drags with any button other than button 1 or 2
	if (macButton > 2) {
		if (pressed) {
			// one more button
			if (m_dragNumButtonsDown++ == 0) {
				enableDragTimer(true);
			}
		}
		else {
			// one less button
			if (--m_dragNumButtonsDown == 0) {
				enableDragTimer(false);
			}
		}
	}

	if (macButton == kButtonLeft) {
		EMouseButtonState state = pressed ? kMouseButtonDown : kMouseButtonUp;
		m_buttonState.set(kButtonLeft - 1, state);
		if (pressed) {
			m_draggingFilename.clear();
			LOG((CLOG_DEBUG2 "dragging file directory is cleared"));
		}
		else {
			if (m_fakeDraggingStarted) {
                m_getDropTargetThread = new Thread([this](){ get_drop_target_thread(); });
			}

			m_draggingStarted = false;
		}
	}

	return true;
}

bool
OSXScreen::onMouseWheel(SInt32 xDelta, SInt32 yDelta) const
{
	LOG((CLOG_DEBUG1 "event: button wheel delta=%+d,%+d", xDelta, yDelta));
	sendEvent(m_events->forIPrimaryScreen().wheel(), WheelInfo::alloc(xDelta, yDelta));
	return true;
}

void
OSXScreen::handleClipboardCheck(const Event&, void*)
{
	checkClipboards();
}

void
OSXScreen::displayReconfigurationCallback(CGDirectDisplayID displayID, CGDisplayChangeSummaryFlags flags, void* inUserData)
{
	OSXScreen* screen = (OSXScreen*)inUserData;

	// Closing or opening the lid when an external monitor is
    // connected causes an kCGDisplayBeginConfigurationFlag event
	CGDisplayChangeSummaryFlags mask = kCGDisplayBeginConfigurationFlag | kCGDisplayMovedFlag |
		kCGDisplaySetModeFlag | kCGDisplayAddFlag | kCGDisplayRemoveFlag |
		kCGDisplayEnabledFlag | kCGDisplayDisabledFlag |
		kCGDisplayMirrorFlag | kCGDisplayUnMirrorFlag |
		kCGDisplayDesktopShapeChangedFlag;

	LOG((CLOG_DEBUG1 "event: display was reconfigured: %x %x %x", flags, mask, flags & mask));

	if (flags & mask) { /* Something actually did change */

		const bool captureReady = displayReconfigurationCaptureReady(flags);
		{
			// Advance the generation before a retry capture can commit. The
			// final commit checks this under the same mutex, so begin/shape
			// event ordering cannot be inverted across threads.
			std::lock_guard<std::mutex> lock(
				screen->m_displayRefreshGenerationMutex);
			++screen->m_displayReconfigurationGeneration;
			screen->m_displayConfigurationInProgress = !captureReady;
			if (screen->m_isPrimary) {
				screen->sendEvent(
					screen->m_events->forIScreen().displayReconfigurationStarted());
			}
		}
		if (!captureReady) {
			screen->cancelDisplayRefreshRetry();
			LOG((CLOG_DEBUG1 "event: display configuration began; waiting for post-change callback"));
			return;
		}
		LOG((CLOG_DEBUG1 "event: screen changed shape; refreshing dimensions"));
		screen->updateScreenShape(displayID, flags);
	}
}

bool
OSXScreen::onKey(CGEventRef event)
{
	CGEventType eventKind = CGEventGetType(event);

	// get the key and active modifiers
	UInt32 virtualKey = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
	CGEventFlags macMask = CGEventGetFlags(event);
	LOG((CLOG_DEBUG1 "event: Key event kind: %d, keycode=%d", eventKind, virtualKey));

	// Special handling to track state of modifiers
	if (eventKind == kCGEventFlagsChanged) {
		// get old and new modifier state
		KeyModifierMask oldMask = getActiveModifiers();
		KeyModifierMask newMask = m_keyState->mapModifiersFromOSX(macMask);
		m_keyState->handleModifierKeys(getEventTarget(), oldMask, newMask);

		// if the current set of modifiers exactly matches a modifiers-only
		// hot key then generate a hot key down event.
		if (m_activeModifierHotKey == 0) {
			if (m_modifierHotKeys.count(newMask) > 0) {
				m_activeModifierHotKey     = m_modifierHotKeys[newMask];
				m_activeModifierHotKeyMask = newMask;
				m_events->addEvent(Event(m_events->forIPrimaryScreen().hotKeyDown(),
								getEventTarget(),
								HotKeyInfo::alloc(m_activeModifierHotKey)));
			}
		}

		// if a modifiers-only hot key is active and should no longer be
		// then generate a hot key up event.
		else if (m_activeModifierHotKey != 0) {
			KeyModifierMask mask = (newMask & m_activeModifierHotKeyMask);
			if (mask != m_activeModifierHotKeyMask) {
				m_events->addEvent(Event(m_events->forIPrimaryScreen().hotKeyUp(),
								getEventTarget(),
								HotKeyInfo::alloc(m_activeModifierHotKey)));
				m_activeModifierHotKey     = 0;
				m_activeModifierHotKeyMask = 0;
			}
		}

		return true;
	}

	HotKeyToIDMap::const_iterator i = m_hotKeyToIDMap.find(HotKeyItem(virtualKey, m_keyState->mapModifiersToCarbon(macMask) & 0xff00u));
	if (i != m_hotKeyToIDMap.end()) {
		UInt32 id = i->second;
		// determine event type
		Event::Type type;
		//UInt32 eventKind = GetEventKind(event);
		if (eventKind == kCGEventKeyDown) {
			type = m_events->forIPrimaryScreen().hotKeyDown();
		}
		else if (eventKind == kCGEventKeyUp) {
			type = m_events->forIPrimaryScreen().hotKeyUp();
		}
		else {
			return false;
		}
		m_events->addEvent(Event(type, getEventTarget(), HotKeyInfo::alloc(id)));
		return true;
	}

	// decode event type
	bool down	  = (eventKind == kCGEventKeyDown);
	bool up		  = (eventKind == kCGEventKeyUp);
	bool isRepeat = (CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) == 1);

	// map event to keys
	KeyModifierMask mask;
	OSXKeyState::KeyIDs keys;
	KeyButton button = m_keyState->mapKeyFromEvent(keys, &mask, event);
	if (button == 0) {
		return false;
	}

	// check for AltGr in mask.  if set we send neither the AltGr nor
	// the super modifiers to clients then remove AltGr before passing
	// the modifiers to onKey.
	KeyModifierMask sendMask = (mask & ~KeyModifierAltGr);
	if ((mask & KeyModifierAltGr) != 0) {
		sendMask &= ~KeyModifierSuper;
	}
	mask &= ~KeyModifierAltGr;

	// update button state
	if (down) {
		m_keyState->onKey(button, true, mask);
	}
	else if (up) {
		if (!m_keyState->isKeyDown(button)) {
			// up event for a dead key.  throw it away.
			return false;
		}
		m_keyState->onKey(button, false, mask);
	}

	// send key events
	for (OSXKeyState::KeyIDs::const_iterator i = keys.begin();
							i != keys.end(); ++i) {
		m_keyState->sendKeyEvent(getEventTarget(), down, isRepeat,
							*i, sendMask, 1, button);
	}

	return true;
}

void
OSXScreen::onMediaKey(CGEventRef event)
{
	KeyID keyID;
	bool down;
	bool isRepeat;

	if (!getMediaKeyEventInfo (event, &keyID, &down, &isRepeat)) {
		LOG ((CLOG_ERR "Failed to decode media key event"));
		return;
	}

	LOG ((CLOG_DEBUG2 "Media key event: keyID=0x%02x, %s, repeat=%s",
						keyID, (down ? "down": "up"),
						(isRepeat ? "yes" : "no")));

	KeyButton button = 0;
	KeyModifierMask mask = m_keyState->getActiveModifiers();
	m_keyState->sendKeyEvent(getEventTarget(), down, isRepeat, keyID, mask, 1, button);
}

bool
OSXScreen::onHotKey(EventRef event) const
{
	// get the hotkey id
	EventHotKeyID hkid;
	GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
							NULL, sizeof(EventHotKeyID), NULL, &hkid);
	UInt32 id = hkid.id;

	// determine event type
	Event::Type type;
	UInt32 eventKind = GetEventKind(event);
	if (eventKind == kEventHotKeyPressed) {
		type = m_events->forIPrimaryScreen().hotKeyDown();
	}
	else if (eventKind == kEventHotKeyReleased) {
		type = m_events->forIPrimaryScreen().hotKeyUp();
	}
	else {
		return false;
	}

	m_events->addEvent(Event(type, getEventTarget(),
								HotKeyInfo::alloc(id)));

	return true;
}

ButtonID
OSXScreen::mapBarrierButtonToMac(UInt16 button) const
{
    switch (button) {
    case 1:
        return kButtonLeft;
    case 2:
        return kMacButtonMiddle;
    case 3:
        return kMacButtonRight;
    }

    return static_cast<ButtonID>(button);
}

ButtonID
OSXScreen::mapMacButtonToBarrier(UInt16 macButton) const
{
	switch (macButton) {
	case 1:
		return kButtonLeft;

	case 2:
		return kButtonRight;

	case 3:
		return kButtonMiddle;
	}

	return static_cast<ButtonID>(macButton);
}

SInt32
OSXScreen::mapScrollWheelToBarrier(float x) const
{
	// return accelerated scrolling but not exponentially scaled as it is
	// on the mac.
	double d = (1.0 + getScrollSpeed()) * x / getScrollSpeedFactor();
	return static_cast<SInt32>(120.0 * d);
}

SInt32
OSXScreen::mapScrollWheelFromBarrier(float x) const
{
	// use server's acceleration with a little boost since other platforms
	// take one wheel step as a larger step than the mac does.
	return static_cast<SInt32>(3.0 * x / 120.0);
}

double
OSXScreen::getScrollSpeed() const
{
	double scaling = 0.0;

	CFPropertyListRef pref = ::CFPreferencesCopyValue(
							CFSTR("com.apple.scrollwheel.scaling") ,
							kCFPreferencesAnyApplication,
							kCFPreferencesCurrentUser,
							kCFPreferencesAnyHost);
	if (pref != NULL) {
		CFTypeID id = CFGetTypeID(pref);
		if (id == CFNumberGetTypeID()) {
			CFNumberRef value = static_cast<CFNumberRef>(pref);
			if (CFNumberGetValue(value, kCFNumberDoubleType, &scaling)) {
				if (scaling < 0.0) {
					scaling = 0.0;
				}
			}
		}
		CFRelease(pref);
	}

	return scaling;
}

bool
OSXScreen::getNaturalScrolling() const
{
	// "Scroll direction: Natural" in Trackpad/Mouse settings; the value
	// lives in the global preferences domain.  Missing, malformed, or
	// unreadable values fall back to natural scrolling, which is the
	// macOS default since Lion.
	CFPropertyListRef pref = ::CFPreferencesCopyValue(
							CFSTR("com.apple.swipescrolldirection"),
							kCFPreferencesAnyApplication,
							kCFPreferencesCurrentUser,
							kCFPreferencesAnyHost);
	bool natural = true;
	if (pref != NULL) {
		CFTypeID id = CFGetTypeID(pref);
		if (id == CFBooleanGetTypeID()) {
			natural = (CFBooleanGetValue(
							static_cast<CFBooleanRef>(pref)) != 0);
		}
		else if (id == CFNumberGetTypeID()) {
			// tolerate 0/1 stored as a number by third-party toggles
			SInt32 value = 0;
			if (CFNumberGetValue(static_cast<CFNumberRef>(pref),
								 kCFNumberSInt32Type, &value)) {
				natural = (value != 0);
			}
		}
		CFRelease(pref);
	}

	return natural;
}

double
OSXScreen::getScrollSpeedFactor() const
{
	return pow(10.0, getScrollSpeed());
}

void
OSXScreen::enableDragTimer(bool enable)
{
	if (enable && m_dragTimer == NULL) {
		m_dragTimer = m_events->newTimer(0.01, NULL);
		m_events->adoptHandler(Event::kTimer, m_dragTimer,
							new TMethodEventJob<OSXScreen>(this,
								&OSXScreen::handleDrag));
		CGEventRef event = CGEventCreate(NULL);
		CGPoint mouse = CGEventGetLocation(event);
		m_dragLastPoint.h = (short)mouse.x;
		m_dragLastPoint.v = (short)mouse.y;
		CFRelease(event);
	}
	else if (!enable && m_dragTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_dragTimer);
		m_events->deleteTimer(m_dragTimer);
		m_dragTimer = NULL;
	}
}

void
OSXScreen::handleDrag(const Event&, void*)
{
	CGEventRef event = CGEventCreate(NULL);
	CGPoint p = CGEventGetLocation(event);
	CFRelease(event);

	if ((short)p.x != m_dragLastPoint.h || (short)p.y != m_dragLastPoint.v) {
		m_dragLastPoint.h = (short)p.x;
		m_dragLastPoint.v = (short)p.y;
		onMouseMove((SInt32)p.x, (SInt32)p.y);
	}
}

void
OSXScreen::updateButtons()
{
	UInt32 buttons = GetCurrentButtonState();

	m_buttonState.overwrite(buttons);
}

IKeyState*
OSXScreen::getKeyState() const
{
	return m_keyState;
}

void
OSXScreen::updateScreenShape(const CGDirectDisplayID, const CGDisplayChangeSummaryFlags flags)
{
	updateScreenShape();
}

void
OSXScreen::scheduleDisplayRefreshRetry()
{
	bool enqueueRequest = false;
	{
		std::lock_guard<std::mutex> lock(m_displayRefreshRetryMutex);
		if (!m_isPrimary || !m_displayRefreshRetryHandlerInstalled) {
			return;
		}
		m_displayRefreshRetryArmed = true;
		if (m_displayRefreshRetryTimer == NULL &&
			!m_displayRefreshRetryRequestPending) {
			m_displayRefreshRetryRequestPending = true;
			enqueueRequest = true;
		}
	}
	if (enqueueRequest) {
		// CoreGraphics invokes display callbacks outside Barrier's event loop.
		// A queued event both wakes that loop and makes timer creation single-
		// threaded with its otherwise-unlocked timer-queue reads.
		m_events->addEvent(Event(
			m_events->forOSXScreen().displayRefreshRetryRequested(),
			&m_displayRefreshRetryEventTarget));
	}
}

void
OSXScreen::cancelDisplayRefreshRetry()
{
	// This can be called by CoreGraphics on a non-event-loop thread. Do not
	// remove a handler or destroy its timer here: EventQueue dispatches raw
	// handler pointers outside its lock. The stable handler below consumes
	// the one-shot and discards it when this retry is no longer armed.
	std::lock_guard<std::mutex> lock(m_displayRefreshRetryMutex);
	m_displayRefreshRetryArmed = false;
}

void
OSXScreen::destroyDisplayRefreshRetry()
{
	EventQueueTimer* timer = NULL;
	bool removeHandler = false;
	{
		std::lock_guard<std::mutex> lock(m_displayRefreshRetryMutex);
		m_displayRefreshRetryArmed = false;
		m_displayRefreshRetryRequestPending = false;
		timer = m_displayRefreshRetryTimer;
		m_displayRefreshRetryTimer = NULL;
		removeHandler = m_displayRefreshRetryHandlerInstalled;
		m_displayRefreshRetryHandlerInstalled = false;
	}
	if (timer != NULL) {
		m_events->deleteTimer(timer);
	}
	if (removeHandler) {
		m_events->removeHandler(
			m_events->forOSXScreen().displayRefreshRetryRequested(),
			&m_displayRefreshRetryEventTarget);
		m_events->removeHandler(
			Event::kTimer, &m_displayRefreshRetryEventTarget);
	}
}

void
OSXScreen::handleDisplayRefreshRetryRequest(const Event&, void*)
{
	// TimerQueue is only safe when its mutations and reads stay on the event
	// loop. Cross-thread callers merely enqueue this request and update the
	// armed bit protected by m_displayRefreshRetryMutex.
	std::lock_guard<std::mutex> lock(m_displayRefreshRetryMutex);
	m_displayRefreshRetryRequestPending = false;
	if (!m_displayRefreshRetryArmed ||
		!m_displayRefreshRetryHandlerInstalled ||
		m_displayRefreshRetryTimer != NULL) {
		return;
	}
	m_displayRefreshRetryTimer = m_events->newOneShotTimer(
		kDisplayRefreshRetrySeconds,
		&m_displayRefreshRetryEventTarget);
}

void
OSXScreen::handleDisplayRefreshRetry(const Event& event, void*)
{
	const IEventQueue::TimerEvent* timerEvent =
		static_cast<const IEventQueue::TimerEvent*>(event.getData());
	EventQueueTimer* timer = timerEvent == NULL ? NULL : timerEvent->m_timer;
	bool refresh = false;
	{
		std::lock_guard<std::mutex> lock(m_displayRefreshRetryMutex);
		if (timer == NULL || timer != m_displayRefreshRetryTimer) {
			return;
		}
		m_displayRefreshRetryTimer = NULL;
		refresh = m_displayRefreshRetryArmed;
		m_displayRefreshRetryArmed = false;
	}

	// Timer lifetime is owned by the event-loop handler. A callback can only
	// change the armed bit, so it cannot invalidate this dispatch's job.
	m_events->deleteTimer(timer);
	if (refresh) {
		updateScreenShape();
	}
}

void
OSXScreen::updateScreenShape()
{
	// CoreGraphics callbacks and event-loop retries can arrive on different
	// threads. Only one capture may run at a time; a generation check below
	// drops a retry whose geometry became stale while it was being queried.
	std::lock_guard<std::mutex> captureLock(m_displayRefreshCaptureMutex);
	std::uint64_t captureGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(m_displayRefreshGenerationMutex);
		if (m_displayConfigurationInProgress) {
			LOG((CLOG_DEBUG1 "display refresh deferred until configuration completes"));
			return;
		}
		captureGeneration = m_displayReconfigurationGeneration;
	}

	const auto generationIsCurrent = [this, captureGeneration]() {
		std::lock_guard<std::mutex> lock(m_displayRefreshGenerationMutex);
		return !m_displayConfigurationInProgress &&
			displayRefreshGenerationIsCurrent(
				captureGeneration, m_displayReconfigurationGeneration);
	};
	const auto clearSnapshot = [this, captureGeneration]() {
		{
			std::lock_guard<std::mutex> generationLock(
				m_displayRefreshGenerationMutex);
			if (m_displayConfigurationInProgress ||
				!displayRefreshGenerationIsCurrent(
					captureGeneration, m_displayReconfigurationGeneration)) {
				return false;
			}
			{
				std::lock_guard<std::mutex> snapshotLock(m_displaySnapshotMutex);
				m_displayID = kCGNullDirectDisplay;
				m_displays.clear();
				m_displayTopology.displays.clear();
				m_x = m_y = m_w = m_h = 0;
				m_xCenter = m_yCenter = 0;
			}
			// A repeated empty capture is still a fresh snapshot. Server needs
			// this event to clear a pending Begin while preserving the original
			// non-extending NoDisplayGrace deadline.
			sendEvent(m_events->forIScreen().shapeChanged());
		}
		return true;
	};

	bool hasValidSnapshot = false;
	{
		std::lock_guard<std::mutex> lock(m_displaySnapshotMutex);
		hasValidSnapshot = m_w > 0 && m_h > 0 && !m_displays.empty();
	}

	const DisplayQueryResult activeQuery =
		queryDisplayList(CGGetActiveDisplayList);
	const std::vector<CGDirectDisplayID> activeDisplays =
		activeQuery.succeeded ? drawableDisplayList(activeQuery.displays, false) :
		std::vector<CGDirectDisplayID>();
	DisplayQueryResult onlineQuery = {false, {}};
	std::vector<CGDirectDisplayID> onlineDisplays;
	if (!m_isPrimary &&
		(!activeQuery.succeeded || activeDisplays.empty()) &&
		!hasValidSnapshot) {
		onlineQuery = queryDisplayList(CGGetOnlineDisplayList);
		if (onlineQuery.succeeded) {
			onlineDisplays = drawableDisplayList(onlineQuery.displays, true);
		}
	}

	const DisplayRefreshDecision decision = decideDisplayRefresh(
		m_isPrimary ? DisplayRefreshRole::PrimaryServer :
			DisplayRefreshRole::SecondaryClient,
		hasValidSnapshot,
		activeQuery.succeeded,
		static_cast<CGDisplayCount>(activeDisplays.size()),
		onlineQuery.succeeded,
		static_cast<CGDisplayCount>(onlineDisplays.size()));
	if (decision.source == DisplayRefreshSource::None) {
		if (decision.retryRequired) {
			if (generationIsCurrent()) {
				LOG((CLOG_ERR "failed to query active displays; retrying geometry capture"));
				scheduleDisplayRefreshRetry();
			}
			return;
		}
		if (decision.preserveCurrentSnapshot) {
			LOG((CLOG_DEBUG "screen shape refresh unavailable; preserving last valid snapshot"));
		}
		else if (m_isPrimary) {
			cancelDisplayRefreshRetry();
			if (clearSnapshot()) {
				LOG((CLOG_DEBUG "screen shape: no active displays"));
			}
			else {
				LOG((CLOG_DEBUG1 "discarding stale empty display capture"));
			}
		}
		else if (!activeQuery.succeeded || !onlineQuery.succeeded) {
			LOG((CLOG_ERR "failed to query active and online displays"));
		}
		else {
			LOG((CLOG_DEBUG "screen shape: no active or online displays"));
		}
		return;
	}
	cancelDisplayRefreshRetry();

	const std::vector<CGDirectDisplayID>& displays =
		decision.source == DisplayRefreshSource::Active ?
		activeDisplays : onlineDisplays;

	const CGDisplayCount displayCount =
		static_cast<CGDisplayCount>(displays.size());
	if (decision.source == DisplayRefreshSource::Online) {
		LOG((CLOG_DEBUG "screen shape: active displays unavailable; using %u online display%s",
			displayCount, displayCount == 1 ? "" : "s"));
	}

	CGRect totalBounds = CGRectZero;
	std::vector<DisplayEntry> nextDisplays;
	nextDisplays.reserve(displayCount);
	std::vector<TopologyDisplayRecord> topologyRecords;
	topologyRecords.reserve(displayCount);
	CGDirectDisplayID main = CGMainDisplayID();
	bool selectedMain = false;
	for (CGDirectDisplayID display : displays) {
		if (display == main) {
			selectedMain = true;
			break;
		}
	}
	if (!selectedMain) {
		// CGGetOnlineDisplayList does not guarantee the main display is first
		// (or even present), but every published topology needs one primary.
		main = displays.front();
	}
	for (CGDisplayCount i = 0; i < displayCount; ++i) {
		const CGRect bounds = CGDisplayBounds(displays[i]);
		totalBounds = (i == 0) ? bounds : CGRectUnion(totalBounds, bounds);

		DisplayEntry entry;
		entry.m_id = displays[i];
		entry.m_rect.x = (SInt32)bounds.origin.x;
		entry.m_rect.y = (SInt32)bounds.origin.y;
		entry.m_rect.w = (SInt32)bounds.size.width;
		entry.m_rect.h = (SInt32)bounds.size.height;
		entry.m_name = displayNameForID(displays[i]);
		nextDisplays.push_back(entry);

		topologyRecords.push_back({
			stableDisplayIdForID(displays[i]),
			entry.m_rect,
			CGDisplayRotation(displays[i]),
			displays[i] == main
		});
	}

	barrier::DisplayTopology nextTopology;
	try {
		nextTopology = topologyFromDisplayRecords(topologyRecords);
	}
	catch (const std::invalid_argument& error) {
		LOG((CLOG_ERR "failed to identify display topology: %s", error.what()));
		if (m_isPrimary && generationIsCurrent()) {
			scheduleDisplayRefreshRetry();
		}
		return;
	}

	const CGRect mainBounds = CGDisplayBounds(main);
	const SInt32 nextX = (SInt32)totalBounds.origin.x;
	const SInt32 nextY = (SInt32)totalBounds.origin.y;
	const SInt32 nextWidth = (SInt32)totalBounds.size.width;
	const SInt32 nextHeight = (SInt32)totalBounds.size.height;
	const SInt32 nextCenterX =
		(mainBounds.origin.x + mainBounds.size.width) / 2;
	const SInt32 nextCenterY =
		(mainBounds.origin.y + mainBounds.size.height) / 2;
	if (nextWidth <= 0 || nextHeight <= 0) {
		LOG((CLOG_ERR "refusing invalid screen shape %dx%d",
			nextWidth, nextHeight));
		if (m_isPrimary && generationIsCurrent()) {
			scheduleDisplayRefreshRetry();
		}
		return;
	}
	bool committed = false;
	{
		std::lock_guard<std::mutex> generationLock(
			m_displayRefreshGenerationMutex);
		if (!m_displayConfigurationInProgress &&
			displayRefreshGenerationIsCurrent(
				captureGeneration, m_displayReconfigurationGeneration)) {
			std::lock_guard<std::mutex> snapshotLock(m_displaySnapshotMutex);
			m_displayID = main;
			m_displays = std::move(nextDisplays);
			m_displayTopology = std::move(nextTopology);
			m_x = nextX;
			m_y = nextY;
			m_w = nextWidth;
			m_h = nextHeight;
			m_xCenter = nextCenterX;
			m_yCenter = nextCenterY;
			committed = true;
		}
		if (committed) {
			// Queue shapeChanged while the generation is pinned so a newer Begin
			// event cannot be ordered before this completed snapshot.
			sendEvent(m_events->forIScreen().shapeChanged());
		}
	}
	if (!committed) {
		LOG((CLOG_DEBUG1 "discarding stale display geometry capture"));
		return;
	}
	LOG((CLOG_DEBUG "screen shape: center=%d,%d size=%dx%d on %u %s",
		 nextX, nextY, nextWidth, nextHeight, displayCount,
		 (displayCount == 1) ? "display" : "displays"));
}

#pragma mark -

//
// FAST USER SWITCH NOTIFICATION SUPPORT
//
// OSXScreen::userSwitchCallback(void*)
//
// gets called if a fast user switch occurs
//

pascal OSStatus
OSXScreen::userSwitchCallback(EventHandlerCallRef nextHandler,
								EventRef theEvent,
								void* inUserData)
{
	OSXScreen* screen = (OSXScreen*)inUserData;
	UInt32 kind        = GetEventKind(theEvent);
	IEventQueue* events = screen->getEvents();

	if (kind == kEventSystemUserSessionDeactivated) {
		LOG((CLOG_DEBUG "user session deactivated"));
		events->addEvent(Event(events->forIScreen().suspend(),
									screen->getEventTarget()));
	}
	else if (kind == kEventSystemUserSessionActivated) {
		LOG((CLOG_DEBUG "user session activated"));
		events->addEvent(Event(events->forIScreen().resume(),
									screen->getEventTarget()));
	}
	return (CallNextEventHandler(nextHandler, theEvent));
}

#pragma mark -

//
// SLEEP/WAKEUP NOTIFICATION SUPPORT
//
// OSXScreen::watchSystemPowerThread(void*)
//
// main of thread monitoring system power (sleep/wakeup) using a CFRunLoop
//

void OSXScreen::watchSystemPowerThread()
{
	io_object_t				notifier;
	IONotificationPortRef	notificationPortRef;
	CFRunLoopSourceRef		runloopSourceRef = 0;

	m_pmRunloop = CFRunLoopGetCurrent();
	// install system power change callback
	m_pmRootPort = IORegisterForSystemPower(this, &notificationPortRef,
											powerChangeCallback, &notifier);
	if (m_pmRootPort == 0) {
		LOG((CLOG_WARN "IORegisterForSystemPower failed"));
	}
	else {
		runloopSourceRef =
			IONotificationPortGetRunLoopSource(notificationPortRef);
		CFRunLoopAddSource(m_pmRunloop, runloopSourceRef,
								kCFRunLoopCommonModes);
	}

	// thread is ready
	{
		Lock lock(m_pmMutex);
		*m_pmThreadReady = true;
		m_pmThreadReady->signal();
	}

	// if we were unable to initialize then exit.  we must do this after
	// setting m_pmThreadReady to true otherwise the parent thread will
	// block waiting for it.
	if (m_pmRootPort == 0) {
		LOG((CLOG_WARN "failed to init watchSystemPowerThread"));
		return;
	}

	LOG((CLOG_DEBUG "started watchSystemPowerThread"));

	LOG((CLOG_DEBUG "waiting for event loop"));
	m_events->waitForReady();

#if defined(MAC_OS_X_VERSION_10_7)
	{
		Lock lockCarbon(m_carbonLoopMutex);
		if (*m_carbonLoopReady == false) {

			// we signalling carbon loop ready before starting
			// unless we know how to do it within the loop
			LOG((CLOG_DEBUG "signalling carbon loop ready"));

			*m_carbonLoopReady = true;
			m_carbonLoopReady->signal();
		}
	}
#endif

	// start the run loop
	LOG((CLOG_DEBUG "starting carbon loop"));
	CFRunLoopRun();
	LOG((CLOG_DEBUG "carbon loop has stopped"));

	// cleanup
	if (notificationPortRef) {
		CFRunLoopRemoveSource(m_pmRunloop,
								runloopSourceRef, kCFRunLoopDefaultMode);
		CFRunLoopSourceInvalidate(runloopSourceRef);
		CFRelease(runloopSourceRef);
	}

	Lock lock(m_pmMutex);
	IODeregisterForSystemPower(&notifier);
	m_pmRootPort = 0;
	LOG((CLOG_DEBUG "stopped watchSystemPowerThread"));
}

void
OSXScreen::powerChangeCallback(void* refcon, io_service_t service,
						  natural_t messageType, void* messageArg)
{
	((OSXScreen*)refcon)->handlePowerChangeRequest(messageType, messageArg);
}

void
OSXScreen::handlePowerChangeRequest(natural_t messageType, void* messageArg)
{
	// we've received a power change notification
	switch (messageType) {
	case kIOMessageSystemWillSleep:
		// OSXScreen has to handle this in the main thread so we have to
		// queue a confirm sleep event here.  we actually don't allow the
		// system to sleep until the event is handled.
		m_events->addEvent(Event(m_events->forOSXScreen().confirmSleep(),
								getEventTarget(), messageArg,
								Event::kDontFreeData));
		return;

	case kIOMessageSystemHasPoweredOn:
		LOG((CLOG_DEBUG "system wakeup"));
		m_events->addEvent(Event(m_events->forIScreen().resume(),
								getEventTarget()));
		break;

	default:
		break;
	}

	Lock lock(m_pmMutex);
	if (m_pmRootPort != 0) {
		IOAllowPowerChange(m_pmRootPort, (long)messageArg);
	}
}

void
OSXScreen::handleConfirmSleep(const Event& event, void*)
{
	long messageArg = (long)event.getData();
	if (messageArg != 0) {
		Lock lock(m_pmMutex);
		if (m_pmRootPort != 0) {
			// deliver suspend event immediately.
			m_events->addEvent(Event(m_events->forIScreen().suspend(),
									getEventTarget(), NULL,
									Event::kDeliverImmediately));

			LOG((CLOG_DEBUG "system will sleep"));
			IOAllowPowerChange(m_pmRootPort, messageArg);
		}
	}
}

#pragma mark -

//
// GLOBAL HOTKEY OPERATING MODE SUPPORT (10.3)
//
// CoreGraphics private API (OSX 10.3)
// Source: http://ichiro.nnip.org/osx/Cocoa/GlobalHotkey.html
//
// We load the functions dynamically because they're not available in
// older SDKs.  We don't use weak linking because we want users of
// older SDKs to build an app that works on newer systems and older
// SDKs will not provide the symbols.
//
// FIXME: This is hosed as of OS 10.5; patches to repair this are
// a good thing.
//
#if 0

#ifdef	__cplusplus
extern "C" {
#endif

typedef int CGSConnection;
typedef enum {
	CGSGlobalHotKeyEnable = 0,
	CGSGlobalHotKeyDisable = 1,
} CGSGlobalHotKeyOperatingMode;

extern CGSConnection _CGSDefaultConnection(void) WEAK_IMPORT_ATTRIBUTE;
extern CGError CGSGetGlobalHotKeyOperatingMode(CGSConnection connection, CGSGlobalHotKeyOperatingMode *mode) WEAK_IMPORT_ATTRIBUTE;
extern CGError CGSSetGlobalHotKeyOperatingMode(CGSConnection connection, CGSGlobalHotKeyOperatingMode mode) WEAK_IMPORT_ATTRIBUTE;

typedef CGSConnection (*_CGSDefaultConnection_t)(void);
typedef CGError (*CGSGetGlobalHotKeyOperatingMode_t)(CGSConnection connection, CGSGlobalHotKeyOperatingMode *mode);
typedef CGError (*CGSSetGlobalHotKeyOperatingMode_t)(CGSConnection connection, CGSGlobalHotKeyOperatingMode mode);

static _CGSDefaultConnection_t				s__CGSDefaultConnection;
static CGSGetGlobalHotKeyOperatingMode_t	s_CGSGetGlobalHotKeyOperatingMode;
static CGSSetGlobalHotKeyOperatingMode_t	s_CGSSetGlobalHotKeyOperatingMode;

#ifdef	__cplusplus
}
#endif

#define LOOKUP(name_)													\
	s_ ## name_ = NULL;													\
	if (NSIsSymbolNameDefinedWithHint("_" #name_, "CoreGraphics")) {	\
		s_ ## name_ = (name_ ## _t)NSAddressOfSymbol(					\
							NSLookupAndBindSymbolWithHint(				\
								"_" #name_, "CoreGraphics"));			\
	}

bool
OSXScreen::isGlobalHotKeyOperatingModeAvailable()
{
	if (!s_testedForGHOM) {
		s_testedForGHOM = true;
		LOOKUP(_CGSDefaultConnection);
		LOOKUP(CGSGetGlobalHotKeyOperatingMode);
		LOOKUP(CGSSetGlobalHotKeyOperatingMode);
		s_hasGHOM = (s__CGSDefaultConnection != NULL &&
					s_CGSGetGlobalHotKeyOperatingMode != NULL &&
					s_CGSSetGlobalHotKeyOperatingMode != NULL);
	}
	return s_hasGHOM;
}

void
OSXScreen::setGlobalHotKeysEnabled(bool enabled)
{
	if (isGlobalHotKeyOperatingModeAvailable()) {
		CGSConnection conn = s__CGSDefaultConnection();

		CGSGlobalHotKeyOperatingMode mode;
		s_CGSGetGlobalHotKeyOperatingMode(conn, &mode);

		if (enabled && mode == CGSGlobalHotKeyDisable) {
			s_CGSSetGlobalHotKeyOperatingMode(conn, CGSGlobalHotKeyEnable);
		}
		else if (!enabled && mode == CGSGlobalHotKeyEnable) {
			s_CGSSetGlobalHotKeyOperatingMode(conn, CGSGlobalHotKeyDisable);
		}
	}
}

bool
OSXScreen::getGlobalHotKeysEnabled()
{
	CGSGlobalHotKeyOperatingMode mode;
	if (isGlobalHotKeyOperatingModeAvailable()) {
		CGSConnection conn = s__CGSDefaultConnection();
		s_CGSGetGlobalHotKeyOperatingMode(conn, &mode);
	}
	else {
		mode = CGSGlobalHotKeyEnable;
	}
	return (mode == CGSGlobalHotKeyEnable);
}

#endif

//
// OSXScreen::HotKeyItem
//

OSXScreen::HotKeyItem::HotKeyItem(UInt32 keycode, UInt32 mask) :
	m_ref(NULL),
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

OSXScreen::HotKeyItem::HotKeyItem(EventHotKeyRef ref,
				UInt32 keycode, UInt32 mask) :
	m_ref(ref),
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

EventHotKeyRef
OSXScreen::HotKeyItem::getRef() const
{
	return m_ref;
}

bool
OSXScreen::HotKeyItem::operator<(const HotKeyItem& x) const
{
	return (m_keycode < x.m_keycode ||
			(m_keycode == x.m_keycode && m_mask < x.m_mask));
}

// Quartz event tap support for the secondary display. This makes sure that we
// will show the cursor if a local event comes in while barrier has the cursor
// off the screen.
CGEventRef
OSXScreen::handleCGInputEventSecondary(
	CGEventTapProxy proxy,
	CGEventType type,
	CGEventRef event,
	void* refcon)
{
	// this fix is really screwing with the correct show/hide behavior. it
	// should be tested better before reintroducing.
	return event;
}

// Quartz event tap support
CGEventRef
OSXScreen::handleCGInputEvent(CGEventTapProxy proxy,
							   CGEventType type,
							   CGEventRef event,
							   void* refcon)
{
	OSXScreen* screen = (OSXScreen*)refcon;
	CGPoint pos;

	switch(type) {
		case kCGEventLeftMouseDown:
		case kCGEventRightMouseDown:
		case kCGEventOtherMouseDown:
			screen->onMouseButton(true, CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) + 1);
			break;
		case kCGEventLeftMouseUp:
		case kCGEventRightMouseUp:
		case kCGEventOtherMouseUp:
			screen->onMouseButton(false, CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) + 1);
			break;
		case kCGEventLeftMouseDragged:
		case kCGEventRightMouseDragged:
		case kCGEventOtherMouseDragged:
		case kCGEventMouseMoved:
			pos = CGEventGetLocation(event);
			screen->onMouseMove(pos.x, pos.y);

			// The system ignores our cursor-centering calls if
			// we don't return the event. This should be harmless,
			// but might register as slight movement to other apps
			// on the system. It hasn't been a problem before, though.
			return event;
			break;
		case kCGEventScrollWheel:
			// relay only while the primary owns a remote target; wheel
			// events captured while the cursor is on the primary itself
			// pass through for the local system
			{
				const bool willForward = shouldForwardRemoteWheel(
					screen->m_isPrimary, screen->m_isOnScreen);
				const SInt32 localXDelta = screen->mapScrollWheelToBarrier(
					CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2) / 65536.0f);
				const SInt32 localYDelta = screen->mapScrollWheelToBarrier(
					CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1) / 65536.0f);
				SInt32 transportXDelta = localXDelta;
				SInt32 transportYDelta = localYDelta;

				// normalize the source host's local convention into the
				// transport convention before relaying
				screen->normalizeScrollDeltas(screen->getNaturalScrolling(),
											  transportXDelta, transportYDelta);

				logScrollDiagnosticsEvent("source-capture", event,
										  screen->m_isPrimary,
										  screen->m_isOnScreen,
										  willForward,
										  screen->getNaturalScrolling(),
										  localXDelta, localYDelta,
										  transportXDelta, transportYDelta);

				if (willForward) {
					screen->onMouseWheel(transportXDelta, transportYDelta);
				}
			}
			break;
		case kCGEventKeyDown:
		case kCGEventKeyUp:
		case kCGEventFlagsChanged:
			screen->onKey(event);
			break;
		case kCGEventTapDisabledByTimeout:
		case kCGEventTapDisabledByUserInput:
			if (eventTapDisableRequiresReenable(type)) {
				CGEventTapEnable(screen->m_eventTapPort, true);
				LOG((CLOG_INFO "quartz event tap was disabled by %s, re-enabling",
					type == kCGEventTapDisabledByTimeout ? "timeout" :
					"user input"));
			}
			break;
		case NX_NULLEVENT:
			break;
		default: {
			SInt32 syntheticXDelta = 0;
			if (detectRemoteSpacesSwipeGesture(type, event,
											   screen->m_isPrimary,
											   screen->m_isOnScreen,
											   syntheticXDelta)) {
				screen->onMouseWheel(syntheticXDelta,
									 kSpacesSwipeSyntheticWheelSentinel);
				break;
			}

			if (type == NX_SYSDEFINED) {
				if (isMediaKeyEvent(event)) {
					LOG((CLOG_DEBUG2 "detected media key event"));
					screen->onMediaKey(event);
				}
				else {
					logUnhandledInputDiagnosticsEvent("source-system-defined",
													 type, event,
													 screen->m_isPrimary,
													 screen->m_isOnScreen);
					LOG((CLOG_DEBUG2 "ignoring unknown system defined event"));
					return event;
				}
				break;
			}

			logUnhandledInputDiagnosticsEvent("source-unhandled", type,
											 event, screen->m_isPrimary,
											 screen->m_isOnScreen);
			LOG((CLOG_DEBUG3 "unknown quartz event type: 0x%02x", type));
			break;
		}
	}

	if (screen->m_isOnScreen) {
		return event;
	} else {
		return NULL;
	}
}

void
OSXScreen::MouseButtonState::set(UInt32 button, EMouseButtonState state)
{
	bool newState = (state == kMouseButtonDown);
	m_buttons.set(button, newState);
}

bool
OSXScreen::MouseButtonState::any()
{
	return m_buttons.any();
}

void
OSXScreen::MouseButtonState::reset()
{
	m_buttons.reset();
}

void
OSXScreen::MouseButtonState::overwrite(UInt32 buttons)
{
	m_buttons = std::bitset<NumButtonIDs>(buttons);
}

bool
OSXScreen::MouseButtonState::test(UInt32 button) const
{
	return m_buttons.test(button);
}

SInt8
OSXScreen::MouseButtonState::getFirstButtonDown() const
{
	if (m_buttons.any()) {
		for (unsigned short button = 0; button < m_buttons.size(); button++) {
			if (m_buttons.test(button)) {
				return button;
			}
		}
	}
	return -1;
}

char*
OSXScreen::CFStringRefToUTF8String(CFStringRef aString)
{
	if (aString == NULL) {
		return NULL;
	}

	CFIndex length = CFStringGetLength(aString);
	CFIndex maxSize = CFStringGetMaximumSizeForEncoding(
		length,
		kCFStringEncodingUTF8);
	char* buffer = (char*)malloc(maxSize);
	if (CFStringGetCString(aString, buffer, maxSize, kCFStringEncodingUTF8)) {
		return buffer;
	}
	return NULL;
}

void
OSXScreen::fakeDraggingFiles(DragFileList fileList)
{
	m_fakeDraggingStarted = true;
	String fileExt;
	if (fileList.size() == 1) {
		fileExt = DragInformation::getDragFileExtension(
			fileList.at(0).getFilename());
	}

#if defined(MAC_OS_X_VERSION_10_7)
	fakeDragging(fileExt.c_str(), m_xCursor, m_yCursor);
#else
	LOG((CLOG_WARN "drag drop not supported"));
#endif
}

String&
OSXScreen::getDraggingFilename()
{
	if (m_draggingStarted) {
		CFStringRef dragInfo = getDraggedFileURL();
		char* info = NULL;
		info = CFStringRefToUTF8String(dragInfo);
		if (info == NULL) {
			m_draggingFilename.clear();
		}
		else {
			LOG((CLOG_DEBUG "drag info: %s", info));
			CFRelease(dragInfo);
			String fileList(info);
			m_draggingFilename = fileList;
		}

		// fake a escape key down and up then left mouse button up
		fakeKeyDown(kKeyEscape, 8192, 1);
		fakeKeyUp(1);
		fakeMouseButton(kButtonLeft, false);
	}
	return m_draggingFilename;
}

void
OSXScreen::waitForCarbonLoop() const
{
#if defined(MAC_OS_X_VERSION_10_7)
	if (*m_carbonLoopReady) {
		LOG((CLOG_DEBUG "carbon loop already ready"));
		return;
	}

	Lock lock(m_carbonLoopMutex);

	LOG((CLOG_DEBUG "waiting for carbon loop"));

	double timeout = ARCH->time() + kCarbonLoopWaitTimeout;
	while (!m_carbonLoopReady->wait()) {
		if (ARCH->time() > timeout) {
			LOG((CLOG_DEBUG "carbon loop not ready, waiting again"));
			timeout = ARCH->time() + kCarbonLoopWaitTimeout;
		}
	}

	LOG((CLOG_DEBUG "carbon loop ready"));
#endif

}

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

void
setZeroSuppressionInterval()
{
	CGSetLocalEventsSuppressionInterval(0.0);
}

void
avoidSupression()
{
	// avoid suppression of local hardware events
	// stkamp@users.sourceforge.net
	CGSetLocalEventsFilterDuringSupressionState(
							kCGEventFilterMaskPermitAllEvents,
							kCGEventSupressionStateSupressionInterval);
	CGSetLocalEventsFilterDuringSupressionState(
							(kCGEventFilterMaskPermitLocalKeyboardEvents |
							kCGEventFilterMaskPermitSystemDefinedEvents),
							kCGEventSupressionStateRemoteMouseDrag);
}

void
logCursorVisibility()
{
	// CGCursorIsVisible is probably deprecated because its unreliable.
	if (!CGCursorIsVisible()) {
		LOG((CLOG_WARN "cursor may not be visible"));
	}
}

void
avoidHesitatingCursor()
{
	// This used to be necessary to get smooth mouse motion on other screens,
	// but now is just to avoid a hesitating cursor when transitioning to
	// the primary (this) screen.
	CGSetLocalEventsSuppressionInterval(0.0001);
}

#pragma GCC diagnostic error "-Wdeprecated-declarations"
