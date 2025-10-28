#include "bump_mouse.h"

#include "bump_mouse_x11.h"
#include "bump_mouse_wayland.h"

#include <gdk/gdkx.h>
#include <gdk/gdkwayland.h>

bool bump_mouse(int dx, int dy)
{
  GdkDisplay *display = gdk_display_get_default();

  if (GDK_IS_X11_DISPLAY(display)) {
    return bump_mouse_x11(dx, dy);
  }
  else if (GDK_IS_WAYLAND_DISPLAY(display)) {
    return bump_mouse_wayland(dx, dy);
  }
  else {
    // Don't know how to support this.
    return false;
  }
}
