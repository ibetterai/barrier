#pragma once

class QWidget;

// Bring the current application to the foreground.
void activateCurrentApplication();

// Order the given widget's native NSWindow to the front and make it key.
void bringWindowToFront(QWidget* widget);
