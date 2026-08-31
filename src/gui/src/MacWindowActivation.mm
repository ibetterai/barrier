#include "MacWindowActivation.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include <QWidget>
#include <QWindow>

void activateCurrentApplication()
{
    ProcessSerialNumber psn = { 0, kCurrentProcess };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // MainWindow::setVisible(false) turns Barrier into a background
    // application. Restore foreground eligibility before asking AppKit to
    // activate a status-item-owned window such as the parentless Log dialog.
    GetCurrentProcess(&psn);
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);

    // activateIgnoringOtherApps: is deprecated but still functional, and is
    // the only reliable way to bring a status-item-driven window forward.
    // The macOS 14+ cooperative -[NSApplication activate] may decline to
    // activate when another application is frontmost.
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
}

void bringWindowToFront(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }

    QWindow* qwindow = widget->windowHandle();
    if (qwindow == nullptr) {
        return;
    }
    // QWindow::winId() on macOS is the NSView* backing this window, stored
    // as an integer WId.
    NSView* view = reinterpret_cast<NSView*>(static_cast<uintptr_t>(qwindow->winId()));
    if (view == nullptr) {
        return;
    }

    NSWindow* window = [view window];
    if (window == nullptr) {
        return;
    }

    [window makeKeyAndOrderFront:nil];
}
