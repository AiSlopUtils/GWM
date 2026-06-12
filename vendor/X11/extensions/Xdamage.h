/* Minimal vendored Xdamage.h — used only when libxdamage-dev is not
 * installed (compile-time check / fallback). Matches libXdamage.so.1 ABI. */
#ifndef INFWM_VENDOR_XDAMAGE_H
#define INFWM_VENDOR_XDAMAGE_H

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#define XDamageNotify             0
#define XDamageReportRawRectangles 0
#define XDamageReportDeltaRectangles 1
#define XDamageReportBoundingBox  2
#define XDamageReportNonEmpty     3

typedef XID Damage;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Drawable drawable;
    Damage damage;
    int level;
    Bool more;
    Time timestamp;
    XRectangle area;
    XRectangle geometry;
} XDamageNotifyEvent;

#ifdef __cplusplus
extern "C" {
#endif

Bool   XDamageQueryExtension(Display *dpy, int *event_base, int *error_base);
Status XDamageQueryVersion(Display *dpy, int *major, int *minor);
Damage XDamageCreate(Display *dpy, Drawable drawable, int level);
void   XDamageDestroy(Display *dpy, Damage damage);
void   XDamageSubtract(Display *dpy, Damage damage,
                       XserverRegion repair, XserverRegion parts);

#ifdef __cplusplus
}
#endif

#endif
