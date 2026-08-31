-- Surface smoke test for the installed macOS Barrier menu bar application.
-- Run with: osascript scripts/macos-tray-actions-smoke.applescript

on barrierProcess()
    tell application "System Events"
        set matches to application processes whose name is "barrier" or name is "Barrier"
        if (count of matches) is not 1 then error "Expected one Barrier GUI process"
        return item 1 of matches
    end tell
end barrierProcess

on dismissOpenMenu()
    tell application "System Events" to key code 53
    delay 0.2
end dismissOpenMenu

on hideMainWindow(p)
    tell application "System Events"
        if exists window "Barrier" of p then
            click menu item "Hide" of menu 1 of menu bar item 3 of menu bar 1 of p
            delay 0.4
        end if
        if exists window "Barrier" of p then error "Could not hide the Barrier main window"
    end tell
end hideMainWindow

on hideLogWindow(p)
    tell application "System Events"
        set logWindows to windows of p whose name contains "Log"
        repeat with logWindow in logWindows
            try
                click button "Hide" of logWindow
            on error
                perform action "AXClose" of logWindow
            end try
        end repeat
    end tell
end hideLogWindow

on statusItem(p)
    tell application "System Events"
        if (count of menu bars of p) < 2 then error "Barrier status menu is not available"
        return menu bar item 1 of menu bar 2 of p
    end tell
end statusItem

on assertFrontmostWindow(p, windowName)
    tell application "System Events"
        if not (exists window windowName of p) then error windowName & " did not appear"
        if frontmost of p is false then error windowName & " did not activate Barrier"
        if value of attribute "AXMain" of window windowName of p is false then error windowName & " is not the front window"
    end tell
end assertFrontmostWindow

set p to barrierProcess()

try
    hideLogWindow(p)
    hideMainWindow(p)

    -- A normal status-item click must only open the menu.
    tell application "System Events" to click my statusItem(p)
    delay 0.5
    tell application "System Events"
        if exists window "Barrier" of p then error "Status-item click showed the main window"
    end tell

    -- Show owns the responsibility for restoring and activating the main window.
    tell application "System Events" to click menu item "Show" of menu 1 of my statusItem(p)
    delay 0.5
    assertFrontmostWindow(p, "Barrier")
    hideMainWindow(p)

    -- Show Log owns the responsibility for displaying and activating the log window.
    tell application "System Events" to click my statusItem(p)
    delay 0.2
    tell application "System Events" to click menu item "Show Log" of menu 1 of my statusItem(p)
    delay 0.5
    tell application "System Events"
        set logWindows to windows of p whose name contains "Log"
        if (count of logWindows) is not 1 then error "Expected one Barrier log window"
        set logWindow to item 1 of logWindows
        if frontmost of p is false then error "Show Log did not activate Barrier"
        if value of attribute "AXMain" of logWindow is false then error "Log window is not the front window"
    end tell

    hideLogWindow(p)
    return "PASS: menu click stayed hidden; Show and Show Log raised the requested window"
on error messageText number messageNumber
    dismissOpenMenu()
    hideLogWindow(p)
    try
        hideMainWindow(p)
    end try
    error "FAIL: " & messageText number messageNumber
end try
