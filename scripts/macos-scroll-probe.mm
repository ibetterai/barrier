/*
 * Diagnostic-only macOS scroll event probe for Barrier issue #15.
 *
 * Build:
 *   clang++ -std=c++11 -framework ApplicationServices -framework CoreFoundation \
 *     scripts/macos-scroll-probe.mm -o build-arm64/bin/macos-scroll-probe
 *
 * Run:
 *   build-arm64/bin/macos-scroll-probe 30
 */

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace {

CFMachPortRef g_eventTap = NULL;

double
fixedPointToDouble(int64_t value)
{
    return static_cast<double>(value) / 65536.0;
}

const char*
eventTapLocationName(CGEventTapLocation location)
{
    switch (location) {
    case kCGHIDEventTap:
        return "hid";
    case kCGSessionEventTap:
        return "session";
    case kCGAnnotatedSessionEventTap:
        return "annotated-session";
    default:
        return "unknown";
    }
}

CGEventRef
handleEvent(CGEventTapProxy, CGEventType type, CGEventRef event, void* refcon)
{
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        if (g_eventTap != NULL) {
            CGEventTapEnable(g_eventTap, true);
        }
        fprintf(stderr, "event tap was disabled; re-enabled it\n");
        fflush(stderr);
        return event;
    }

    if (type != kCGEventScrollWheel) {
        return event;
    }

    const CGPoint location = CGEventGetLocation(event);
    const int64_t fixedAxis1 = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventFixedPtDeltaAxis1);
    const int64_t fixedAxis2 = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventFixedPtDeltaAxis2);
    const int64_t fixedAxis3 = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventFixedPtDeltaAxis3);
    const int64_t pointAxis1 = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventPointDeltaAxis1);
    const int64_t pointAxis2 = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventPointDeltaAxis2);
    const int64_t pointAxis3 = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventPointDeltaAxis3);
    const int64_t phase = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventScrollPhase);
    const int64_t momentum = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventMomentumPhase);
    const int64_t continuous = CGEventGetIntegerValueField(
        event, kCGScrollWheelEventIsContinuous);

    fprintf(stdout,
            "%lld type=%u location=(%.1f,%.1f) "
            "fixed=(%lld,%lld,%lld) fixedFloat=(%.4f,%.4f,%.4f) "
            "point=(%lld,%lld,%lld) phase=%lld momentum=%lld continuous=%lld\n",
            static_cast<long long>(time(NULL)),
            static_cast<unsigned int>(type),
            location.x,
            location.y,
            static_cast<long long>(fixedAxis1),
            static_cast<long long>(fixedAxis2),
            static_cast<long long>(fixedAxis3),
            fixedPointToDouble(fixedAxis1),
            fixedPointToDouble(fixedAxis2),
            fixedPointToDouble(fixedAxis3),
            static_cast<long long>(pointAxis1),
            static_cast<long long>(pointAxis2),
            static_cast<long long>(pointAxis3),
            static_cast<long long>(phase),
            static_cast<long long>(momentum),
            static_cast<long long>(continuous));
    fflush(stdout);

    return event;
}

void
stopRunLoop(CFRunLoopTimerRef, void*)
{
    CFRunLoopStop(CFRunLoopGetCurrent());
}

CFMachPortRef
createTap(CGEventTapLocation location)
{
    CGEventMask mask = CGEventMaskBit(kCGEventScrollWheel);

    return CGEventTapCreate(location,
                            kCGHeadInsertEventTap,
                            kCGEventTapOptionListenOnly,
                            mask,
                            handleEvent,
                            NULL);
}

} // namespace

int
main(int argc, char** argv)
{
    int seconds = 30;
    if (argc > 1) {
        seconds = atoi(argv[1]);
        if (seconds <= 0) {
            seconds = 30;
        }
    }

    const CGEventTapLocation locations[] = {
        kCGHIDEventTap,
        kCGSessionEventTap,
        kCGAnnotatedSessionEventTap
    };

    CFMachPortRef tap = NULL;
    CGEventTapLocation selectedLocation = kCGHIDEventTap;
    for (size_t i = 0; i < sizeof(locations) / sizeof(locations[0]); ++i) {
        tap = createTap(locations[i]);
        if (tap != NULL) {
            selectedLocation = locations[i];
            break;
        }
    }

    if (tap == NULL) {
        fprintf(stderr,
                "failed to create a listen-only scroll event tap; check "
                "Accessibility/Input Monitoring permissions for Terminal\n");
        return 1;
    }

    g_eventTap = tap;

    CFMachPortSetInvalidationCallBack(tap, NULL);
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(
        kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);

    CFRunLoopTimerContext timerContext = {0, NULL, NULL, NULL, NULL};
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + seconds,
        0,
        0,
        0,
        stopRunLoop,
        &timerContext);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);

    CGEventTapEnable(tap, true);

    fprintf(stderr,
            "listening for scroll events at %s event tap for %d seconds\n",
            eventTapLocationName(selectedLocation),
            seconds);
    fprintf(stderr,
            "try ordinary horizontal scroll, then Magic Mouse two-finger "
            "Spaces/full-screen-app swipe\n");
    fflush(stderr);

    CFRunLoopRun();

    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CFRelease(timer);
    CFRelease(source);
    CFRelease(tap);

    return 0;
}
