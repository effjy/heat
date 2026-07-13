// tray.h — minimal StatusNotifierItem (system tray) support for Heat.
//
// GTK4 removed GtkStatusIcon, so Heat speaks the freedesktop/KDE
// StatusNotifierItem D-Bus protocol directly through GDBus (part of GLib —
// no extra dependency). Supported by KDE, MATE/XFCE/GNOME (with the usual
// tray applet/extension), and any tray that implements the spec.
//
// Adapted from the sibling Pulse monitor.
#pragma once
#include <gio/gio.h>

typedef void (*TrayCb)(void *user);

// Register a tray icon. `on_show` fires when the user activates the icon or the
// "Show Heat" menu entry; `on_quit` fires for "Quit Heat". Returns true only
// if a StatusNotifierWatcher (a real tray) is present and the item registered —
// callers should fall back to ordinary window behaviour when it returns false.
bool tray_init(GApplication *app, const char *icon_name,
               TrayCb on_show, TrayCb on_quit, void *user);
