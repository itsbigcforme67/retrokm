/* Minimal stand-in for <X11/extensions/XTest.h>, for build hosts that have
 * libXtst.so but not its development package.  Real systems use their own. */
#ifndef RKM_STUB_XTEST_H
#define RKM_STUB_XTEST_H
#include <X11/Xlib.h>
Bool XTestQueryExtension(Display *, int *, int *, int *, int *);
int XTestFakeKeyEvent(Display *, unsigned int, Bool, unsigned long);
int XTestFakeButtonEvent(Display *, unsigned int, Bool, unsigned long);
int XTestFakeMotionEvent(Display *, int, int, int, unsigned long);
int XTestFakeRelativeMotionEvent(Display *, int, int, unsigned long);
#endif
