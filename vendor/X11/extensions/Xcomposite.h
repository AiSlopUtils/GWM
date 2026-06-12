/* Minimal vendored Xcomposite.h — used only when libxcomposite-dev is not
 * installed. Matches the ABI of libXcomposite.so.1. */
#ifndef INFWM_VENDOR_XCOMPOSITE_H
#define INFWM_VENDOR_XCOMPOSITE_H

#include <X11/Xlib.h>

#define CompositeRedirectAutomatic 0
#define CompositeRedirectManual    1

#ifdef __cplusplus
extern "C" {
#endif

Bool   XCompositeQueryExtension(Display *dpy, int *event_base, int *error_base);
Status XCompositeQueryVersion(Display *dpy, int *major, int *minor);
void   XCompositeRedirectWindow(Display *dpy, Window window, int update);
void   XCompositeRedirectSubwindows(Display *dpy, Window window, int update);
void   XCompositeUnredirectWindow(Display *dpy, Window window, int update);
void   XCompositeUnredirectSubwindows(Display *dpy, Window window, int update);
Pixmap XCompositeNameWindowPixmap(Display *dpy, Window window);

#ifdef __cplusplus
}
#endif

#endif
