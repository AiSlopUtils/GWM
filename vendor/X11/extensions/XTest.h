/* Minimal vendored XTest.h — used only when libxtst-dev is not installed.
 * Matches the ABI of libXtst.so.6. Only used by the test tool, not the WM. */
#ifndef GWM_VENDOR_XTEST_H
#define GWM_VENDOR_XTEST_H

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

Bool XTestQueryExtension(Display *dpy, int *event_base, int *error_base,
                         int *major, int *minor);
int  XTestFakeKeyEvent(Display *dpy, unsigned int keycode, Bool is_press,
                       unsigned long delay);
int  XTestFakeButtonEvent(Display *dpy, unsigned int button, Bool is_press,
                          unsigned long delay);
int  XTestFakeMotionEvent(Display *dpy, int screen, int x, int y,
                          unsigned long delay);

#ifdef __cplusplus
}
#endif

#endif
