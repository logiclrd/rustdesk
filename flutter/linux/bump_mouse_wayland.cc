#include "bump_mouse.h"

#if !HAVE_WAYLAND

//#warning Building without Wayland support for BumpMouse.

void initialize_wayland()
{
}

bool bump_mouse_wayland(int dx, int dy)
{
	return false;
}

#else

#include <string.h>

#include <gtk/gtk.h>

#include <gdk/gdkwayland.h>

#include <wayland-util.h>
#include <wayland-client-protocol.h>

#if HAVE_WAYLAND_POINTER_WARP_V1
# include "wayland-pointer-warp-v1-client-protocol.h"
#endif
#if HAVE_WAYLAND_POINTER_CONSTRAINTS_V1
# include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#endif

namespace
{
	bool wayland_initialized = false;

	struct wl_display *wayland_display = nullptr;
	struct wl_registry *wayland_registry = nullptr;
	struct wl_seat *wayland_seat = nullptr;
	struct wl_pointer *wayland_pointer = nullptr;

#if HAVE_WAYLAND_POINTER_WARP_V1
	struct wp_pointer_warp_v1 *wp_pointer_warp_v1 = nullptr;
#endif
#if HAVE_WAYLAND_POINTER_CONSTRAINTS_V1
	struct zwp_pointer_constraints_v1 *zwp_pointer_constraints_v1 = nullptr;
#endif

	void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t /*enum wl_seat_capability*/ caps)
	{
		if (caps & WL_SEAT_CAPABILITY_POINTER)
			wayland_pointer = wl_seat_get_pointer(seat);
	}

	const struct wl_seat_listener seat_listener =
		{
			.capabilities = seat_handle_capabilities,
		};

	void handle_registry_global(
		void *data,
		struct wl_registry *registry,
		uint32_t id,
		const char *interface,
		uint32_t version)
	{
		if (strcmp(interface, wl_seat_interface.name) == 0)
		{
			wayland_seat = (struct wl_seat *)wl_registry_bind(registry, id, &wl_seat_interface, 1);
			wl_seat_add_listener(wayland_seat, &seat_listener, nullptr);
			return;
		}

#if HAVE_WAYLAND_POINTER_WARP_V1
		if (strcmp(interface, wp_pointer_warp_v1_interface.name) == 0)
		{
			wp_pointer_warp_v1 = (struct wp_pointer_warp_v1 *)wl_registry_bind(registry, id, &wp_pointer_warp_v1_interface, 1);
			return;
		}
#endif

#if HAVE_WAYLAND_POINTER_CONSTRAINTS_V1
		if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0)
		{
			zwp_pointer_constraints_v1 = (struct zwp_pointer_constraints_v1 *)wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
			return;
		}
#endif
	}

	void handle_registry_remove_global(void *data, struct wl_registry *registry, uint32_t id)
	{
		// Nothing to do here; this callback is used only to notify about outputs and seats.
	}

	const struct wl_registry_listener registry_listener =
		{
			handle_registry_global,
			handle_registry_remove_global
		};

	void get_mouse_position(gint &x, gint &y)
	{
		GdkDevice *mouse_device;

#if GTK_CHECK_VERSION(3, 20, 0)
		auto seat = gdk_display_get_default_seat(gdk_display_get_default());

		mouse_device = gdk_seat_get_pointer(seat);
#else
		auto devman = gdk_display_get_device_manager(gdk_display_get_default());

		mouse_device = gdk_device_manager_get_client_pointer(devman);
#endif

		GdkScreen *screen;

		gdk_device_get_position(mouse_device, &screen, &x, &y);
	}

	bool try_pointer_warp_v1(int x, int y);
	bool try_pointer_constraints_unstable_v1_lock_pointer(int x, int y);
} // anonymous namespace

void initialize_wayland(GtkWindow *window)
{
  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));

	wayland_display = gdk_wayland_display_get_wl_display(GDK_WAYLAND_DISPLAY(display));
	if (wayland_display == nullptr)
		return;

	wayland_registry = wl_display_get_registry(wayland_display);
	if (wayland_registry == nullptr)
		return;

	GdkNative *window_native = gtk_widget_get_native(window);
	if (window_native == nullptr)
		return;

	GdkSurface *gdk_surface = gtk_native_get_surface(window_native);
	if (gdk_surface == nullptr)
		return;

	wayland_surface = gdk_wayland_surface_get_wl_surface(gdk_surface);
	if (wayland_surface == nullptr)
		return;

	wl_registry_add_listener(wayland_registry, &registry_listener, data);

	wayland_initialized = true;
}

bool bump_mouse_wayland(int dx, int dy)
{
	if (wayland_initialized)
	{
		gint x, y;

		get_mouse_position(x, y);

		int new_x = x + dx;
		int new_y = y + dy;

		if (try_pointer_warp_v1(new_x, new_y))
			return true;

		if (try_pointer_constraints_v1_lock_pointer(new_x, new_y))
			return true;
	}

	// No way to warp the pointer.
	return false;
}

namespace
{
	///
	/// Strategy: pointer_warp_v1
	///
	/// This Wayland protocol was added in v1.45. At the time of writing this, it is not
	/// broadly supported. If available, it directly does what bump_mouse needs.
	///

	bool should_try_pointer_warp_v1 = true;

	static bool try_pointer_warp_v1(int x, y)
	{
# if HAVE_WAYLAND_POINTER_WARP_V1
		if (!should_try_pointer_warp_v1)
		{
			// Fast failure; if it fails on one call, it will probably fail on all calls.
			return false;
		}

		if (wp_pointer_warp_v1 != nullptr)
		{
			const wl_fixed_t f_x = wl_fixed_from_double(x);
			const wl_fixed_t f_y = wl_fixed_from_double(y);

			wp_pointer_warp_v1_warp_pointer(
				wp_pointer_warp_v1,
				wayland_surface,
				wayland_seat->pointer.wl_pointer,
				f_x, f_y,
				wayland_seat->pointer.enter_serial);

			return true;
		}

		should_try_pointer_warp_v1 = false;
# endif /* HAVE_WAYLAND_POINTER_WARP_V1 */

		return false;
	}

	///
	/// Strategy: pointer_constraints_unstable_v1
	///
	/// This Wayland protocol provides functionality for constraining the movement of the
	/// pointer. This can be done in two ways: it can be boxed into a rectangular region,
	/// or it can be locked in place. If it is locked in place, then when it is unlocked,
	/// it is possible to supply a "hint" coordinate to which the cursor should be warped.
	///
	/// Compositors are not technically required to respect this hint, though in practice
	/// they do.
	///

	bool should_try_pointer_constraints_unstable_v1 = true;

	static bool try_pointer_constraints_unstable_v1_lock_pointer(int x, int y)
	{
# if HAVE_WAYLAND_POINTER_CONSTRAINTS_V1
		if (should_try_pointer_constraints_unstable_v1)
		{
			zwp_locked_pointer_v1 *locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
				zwp_pointer_constraints_v1,
				wayland_surface,
				wayland_pointer,
				nullptr,
				ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);

			zwp_locked_pointer_v1_set_cursor_position_hint(locked_pointer, x, y);

			wl_surface_commit(wayland_surface);

			zwp_locked_pointer_v1_destroy(locked_pointer);

			return true;
		}

		should_try_pointer_constraints_unstable_v1 = false;

# endif /* HAVE_WAYLAND_POINTER_CONSTRAINTS_V1 */
		return false;
	}
} // anonymous namespace
#endif /* HAVE_WAYLAND */