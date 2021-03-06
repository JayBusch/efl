#ifndef _ECORE_WL2_INTERNAL_H
# define _ECORE_WL2_INTERNAL_H

# ifdef EAPI
#  undef EAPI
# endif

# ifdef _WIN32
#  ifdef EFL_BUILD
#   ifdef DLL_EXPORT
#    define EAPI __declspec(dllexport)
#   else
#    define EAPI
#   endif
#  else
#   define EAPI __declspec(dllimport)
#  endif
# else
#  ifdef __GNUC__
#   if __GNUC__ >= 4
#    define EAPI __attribute__ ((visibility("default")))
#   else
#    define EAPI
#   endif
#  else
#   define EAPI
#  endif
# endif

EAPI extern int ECORE_WL2_EVENT_WINDOW_ROTATION_CHANGE_PREPARE; /** @since 1.20 */
EAPI extern int ECORE_WL2_EVENT_WINDOW_ROTATION_CHANGE_PREPARE_DONE; /** @since 1.20 */
EAPI extern int ECORE_WL2_EVENT_WINDOW_ROTATION_CHANGE_REQUEST; /** @since 1.20 */
EAPI extern int ECORE_WL2_EVENT_WINDOW_ROTATION_CHANGE_DONE; /** @since 1.20 */

EAPI void ecore_wl2_window_rotation_change_prepare_send(Ecore_Wl2_Window *window, int rot, int w, int h, Eina_Bool resize);
EAPI void ecore_wl2_window_rotation_change_prepare_done_send(Ecore_Wl2_Window *window, int rot);
EAPI void ecore_wl2_window_rotation_change_request_send(Ecore_Wl2_Window *window, int rot);
EAPI void ecore_wl2_window_rotation_change_done_send(Ecore_Wl2_Window *window, int rot, int w, int h);

EAPI void ecore_wl2_window_false_commit(Ecore_Wl2_Window *window);
EAPI Eina_Bool ecore_wl2_buffer_fit(Ecore_Wl2_Buffer *b, int w, int h);

EAPI void ecore_wl2_window_damage(Ecore_Wl2_Window *window, Eina_Rectangle *rects, unsigned int count);
EAPI Eina_Bool ecore_wl2_buffer_init(Ecore_Wl2_Display *ewd, Ecore_Wl2_Buffer_Type types);
EAPI Ecore_Wl2_Buffer *ecore_wl2_buffer_create(Ecore_Wl2_Display *ewd, int w, int h, Eina_Bool alpha);
EAPI void ecore_wl2_buffer_destroy(Ecore_Wl2_Buffer *b);
EAPI struct wl_buffer *ecore_wl2_buffer_wl_buffer_get(Ecore_Wl2_Buffer *buf);
EAPI void *ecore_wl2_buffer_map(Ecore_Wl2_Buffer *buf, int *w, int *h, int *stride);
EAPI void ecore_wl2_buffer_unmap(Ecore_Wl2_Buffer *buf);
EAPI void ecore_wl2_buffer_discard(Ecore_Wl2_Buffer *buf);
EAPI void ecore_wl2_buffer_lock(Ecore_Wl2_Buffer *b);
EAPI void ecore_wl2_buffer_unlock(Ecore_Wl2_Buffer *b);
EAPI void ecore_wl2_buffer_destroy(Ecore_Wl2_Buffer *b);
EAPI Eina_Bool ecore_wl2_buffer_busy_get(Ecore_Wl2_Buffer *buffer);
EAPI void ecore_wl2_buffer_busy_set(Ecore_Wl2_Buffer *buffer);
EAPI int ecore_wl2_buffer_age_get(Ecore_Wl2_Buffer *buffer);
EAPI void ecore_wl2_buffer_age_set(Ecore_Wl2_Buffer *buffer, int age);
EAPI void ecore_wl2_buffer_age_inc(Ecore_Wl2_Buffer *buffer);

EAPI Ecore_Wl2_Surface *ecore_wl2_surface_create(Ecore_Wl2_Window *win, Eina_Bool alpha);
EAPI void ecore_wl2_surface_destroy(Ecore_Wl2_Surface *surface);
EAPI void ecore_wl2_surface_reconfigure(Ecore_Wl2_Surface *surface, int w, int h, uint32_t flags, Eina_Bool alpha);
EAPI void *ecore_wl2_surface_data_get(Ecore_Wl2_Surface *surface, int *w, int *h);
EAPI int  ecore_wl2_surface_assign(Ecore_Wl2_Surface *surface);
EAPI void ecore_wl2_surface_post(Ecore_Wl2_Surface *surface, Eina_Rectangle *rects, unsigned int count);
EAPI void ecore_wl2_surface_flush(Ecore_Wl2_Surface *surface, Eina_Bool purge);
EAPI void ecore_wl2_window_surface_flush(Ecore_Wl2_Window *window, Eina_Bool purge);
EAPI Ecore_Wl2_Buffer *ecore_wl2_surface_buffer_create(Ecore_Wl2_Surface *surface);
EAPI int ecore_wl2_surface_manager_add(Ecore_Wl2_Surface_Interface *intf);
EAPI void ecore_wl2_surface_manager_del(Ecore_Wl2_Surface_Interface *intf);
EAPI Ecore_Wl2_Window *ecore_wl2_surface_window_get(Ecore_Wl2_Surface *surface);
EAPI Eina_Bool ecore_wl2_surface_alpha_get(Ecore_Wl2_Surface *surface);

/**
 * Attach a buffer to a window
 *
 * Note that the GL stack my attach buffers to a surface - we should call this
 * function at that time (with a NULL buffer) to track whether a surface
 * has a valid buffer.  That is, call with implicit true and buffer NULL at
 * the time of glSwapBuffers.
 *
 * @param window The target window
 * @param buffer The buffer to attach
 * @param x X offset from corner
 * @param y Y offset from corner
 * @param implicit True if an external library is doing the actual attaching
 *
 * @since 1.21
 */
EAPI void ecore_wl2_window_buffer_attach(Ecore_Wl2_Window *win, void *buffer, int x, int y, Eina_Bool implicit);

/**
 * Set a buffer transform on a given window
 *
 * @param window The window on which to set the buffer transform
 * @param transform The buffer transform being requested
 *
 * @ingroup Ecore_Wl2_Window_Group
 * @since 1.20
 */
EAPI void ecore_wl2_window_buffer_transform_set(Ecore_Wl2_Window *window, int transform);

# undef EAPI
# define EAPI

#endif
