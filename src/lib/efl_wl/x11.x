#define WL_TEXT_STR "text/plain;charset=utf-8"
#define INCR_CHUNK_SIZE 1 << 17

static void (*xconvertselection)(Ecore_X_Display *, Ecore_X_Atom, Ecore_X_Atom, Ecore_X_Atom, Ecore_X_Window, Ecore_X_Time);
static Ecore_X_Atom string_atom;
static Ecore_X_Atom comp_dnd_atom;
static Ecore_X_Atom timestamp_atom;
static Ecore_X_Atom incr_atom;
static Ecore_X_Atom int_atom;

static Eina_Hash *pipes;

typedef struct
{
   Ecore_Fd_Handler *fdh;
   Comp_Data_Device_Source *source;
   Ecore_X_Window win;
   Ecore_X_Atom atom;
   Ecore_X_Atom selection;
   Ecore_X_Atom property;
   Eina_Binbuf *buf;
   Eina_Bool incr : 1;
} Pipe;

static void
_pipe_free(Pipe *p)
{
   close(ecore_main_fd_handler_fd_get(p->fdh));
   ecore_main_fd_handler_del(p->fdh);
   eina_binbuf_free(p->buf);
   free(p);
}

static void
_incr_update(Pipe *p, Eina_Bool success)
{
   ecore_x_selection_notify_send(p->win, p->selection, p->atom, (!!success) * p->property, 0);
}

static void
_incr_upload(Pipe *p)
{
   size_t size;

   size = eina_binbuf_length_get(p->buf);
   size = MIN(size, INCR_CHUNK_SIZE);
   ecore_x_window_prop_property_set(p->win, p->property, p->atom, 8, (void*)eina_binbuf_string_get(p->buf), size);
   eina_binbuf_free(p->buf);
   p->buf = NULL;
}

static Eina_Bool
x11_offer_write(void *data, Ecore_Fd_Handler *fdh)
{
   Comp_Data_Device_Transfer *dt = data;
   int len = -1;

   if (ecore_main_fd_handler_active_get(fdh, ECORE_FD_WRITE))
     {
        len = write(ecore_main_fd_handler_fd_get(fdh),
                    eina_binbuf_string_get(dt->source->reader_data) + dt->offset,
                    eina_binbuf_length_get(dt->source->reader_data) - dt->offset);
        if (len > 0) dt->offset += len;
     }

   if ((len <= 0) || (dt->offset == eina_binbuf_length_get(dt->source->reader_data)))
     {
        fdh_del(fdh);
        eina_stringshare_del(dt->mime_type);
        dt->source->transfers = eina_inlist_remove(dt->source->transfers, EINA_INLIST_GET(dt));

        if (dt->source->x11_owner)
          {
             Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(dt->source->seat->c->evas));
             if (dt->type == COMP_DATA_DEVICE_OFFER_TYPE_DND)
               ecore_x_client_message32_send(dt->source->x11_owner,
                 ECORE_X_ATOM_XDND_FINISHED, ECORE_X_EVENT_MASK_NONE,
                 win, (len >= 0), action_convert(dt->source->current_dnd_action), 0, 0);
             ecore_x_window_prop_property_del(win, ECORE_X_ATOM_XDND_TYPE_LIST);
             if (dt->source == dt->source->seat->drag.source)
               dt->source->seat->drag.source = NULL;
          }

        if ((!dt->source->transfers) && (dt->source->seat->selection_source != dt->source))
          comp_data_device_source_reader_clear(dt->source);
        free(dt);
     }

   return ECORE_CALLBACK_RENEW;
}

static void
x11_send_send(Comp_Data_Device_Source *source, const char* mime_type, int32_t fd, Comp_Data_Device_Offer_Type type)
{
   Ecore_X_Atom t, sel = ECORE_X_ATOM_SELECTION_CLIPBOARD;
   Comp_Data_Device_Transfer *dt;
   Ecore_Window win;
   win = ecore_evas_window_get(ecore_evas_ecore_evas_get(source->seat->c->evas));
   if (type == COMP_DATA_DEVICE_OFFER_TYPE_DND)
     sel = ECORE_X_ATOM_SELECTION_XDND;

   if (eina_streq(mime_type, WL_TEXT_STR))
     t = string_atom;
   else
     t = ecore_x_atom_get(mime_type);

   dt = calloc(1, sizeof(Comp_Data_Device_Transfer));
   dt->type = type;
   dt->fdh = ecore_main_fd_handler_add(fd, 0, x11_offer_write, dt, NULL, NULL);
   dt->source = source;
   dt->mime_type = eina_stringshare_add(ecore_x_atom_name_get(t));
   dt->source->transfers = eina_inlist_append(dt->source->transfers, EINA_INLIST_GET(dt));
   xconvertselection(ecore_x_display_get(), sel, t, comp_dnd_atom, win, ecore_x_current_time_get());
}

static Eina_Bool
x11_fixes_selection_notify(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Fixes_Selection_Notify *ev)
{
   Ecore_X_Window win;
   Eina_List *l;
   Comp *c;
   Comp_Seat *s;

   EINA_LIST_FOREACH(comps, l, c)
     {
        win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (ev->owner == win)
          return ECORE_CALLBACK_RENEW;
        c->x11_selection = 0;
     }
   if (ev->atom == ECORE_X_ATOM_SELECTION_CLIPBOARD)
     {
        EINA_LIST_FOREACH(comps, l, c)
          EINA_INLIST_FOREACH(c->seats, s)
            {
               s->x11_selection_owner = ev->owner;
               if (ev->owner) s->selection_source = NULL;
            }
        if (ev->owner)
          {
             xconvertselection(ecore_x_display_get(), ECORE_X_ATOM_SELECTION_CLIPBOARD,
               ECORE_X_ATOM_SELECTION_TARGETS, comp_dnd_atom, ev->win, 0);
          }
     }
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_selection_notify(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Selection_Notify *ev)
{
   Ecore_X_Selection_Data *sd;
   Eina_List *l, *lc = NULL;
   Comp *c;
   Comp_Seat *s;
   Comp_Data_Device_Transfer *dt;
   Eina_Binbuf *buf = NULL;

   if ((ev->selection != ECORE_X_SELECTION_XDND) && (ev->selection != ECORE_X_SELECTION_CLIPBOARD))
     {
        EINA_LIST_FOREACH(comps, l, c)
          EINA_INLIST_FOREACH(c->seats, s)
            {
               ecore_evas_free(s->drag.proxy_win);
               s->drag.proxy_win = NULL;
            }
        return ECORE_CALLBACK_RENEW;
     }
   EINA_LIST_FOREACH(comps, l, c)
     {
        Ecore_X_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (win == ev->win) lc = eina_list_append(lc, c);
     }
   if (ev->selection == ECORE_X_SELECTION_CLIPBOARD)
     {
        if (eina_streq(ev->target, "TARGETS"))
          {
             Ecore_X_Selection_Data_Targets *tgs = ev->data;
             int i, j;
             const char **types;

             types = alloca((tgs->num_targets + 1) * sizeof(void*));
             for (i = j = 0; i < tgs->num_targets; i++)
               if (tgs->targets[i])
                 types[j++] = tgs->targets[i];
             types[j] = NULL;
             EINA_LIST_FREE(lc, c)
               EINA_INLIST_FOREACH(c->seats, s)
                 s->client_selection_serial = ecore_wl2_dnd_selection_set(s->client_seat, (const char**)types);
             return ECORE_CALLBACK_RENEW;
          }
     }
   if (ev->property != comp_dnd_atom) return ECORE_CALLBACK_RENEW;
   EINA_LIST_FREE(lc, c)
     {
        EINA_INLIST_FOREACH(c->seats, s)
          {

             if (s->drag.source && s->drag.source->transfers &&
                 (ev->selection == ECORE_X_SELECTION_XDND))
               {
                  EINA_INLIST_FOREACH(s->drag.source->transfers, dt)
                    if (eina_streq(ev->target, dt->mime_type))
                      {
                         if (!s->drag.source->reader_data)
                           s->drag.source->reader_data = eina_binbuf_new();
                         buf = s->drag.source->reader_data;
                         break;
                      }
               }
             else if (s->selection_source && s->selection_source->transfers &&
                 (ev->selection == ECORE_X_SELECTION_CLIPBOARD))
               {
                  EINA_INLIST_FOREACH(s->selection_source->transfers, dt)
                    if (eina_streq(ev->target, dt->mime_type))
                      {
                         if (!s->selection_source->reader_data)
                           s->selection_source->reader_data = eina_binbuf_new();
                         buf = s->selection_source->reader_data;
                         break;
                      }
               }
             if (buf) break;
          }
        if (buf) break;
     }
   eina_list_free(lc);

   if (!buf) return ECORE_CALLBACK_RENEW;
   sd = ev->data;
   eina_binbuf_append_length(buf, sd->data, sd->length);
   ecore_main_fd_handler_active_set(dt->fdh, ECORE_FD_WRITE | ECORE_FD_ERROR);

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_pipe_read(void *data, Ecore_Fd_Handler *fdh)
{
   Pipe *p = data;
   ssize_t len;
   unsigned char *buf;

   buf = malloc(INCR_CHUNK_SIZE);
   len = read(ecore_main_fd_handler_fd_get(fdh), (void*)buf, INCR_CHUNK_SIZE);
   if (len < 0)
     {
        free(buf);
        _incr_update(p, 0);
        eina_hash_del_by_key(pipes, &p->win);
     }
   if (len == INCR_CHUNK_SIZE)
     {
        p->buf = eina_binbuf_manage_new(buf, len, 0);
        if (p->incr)
          _incr_upload(p);
        else
          {
             unsigned long size = INCR_CHUNK_SIZE;

             p->incr = 1;
             ecore_x_window_prop_property_set(p->win, p->atom, incr_atom, 32, &size, 1);
             _incr_update(p, 1);
          }
        ecore_main_fd_handler_active_set(p->fdh, 0);
        return ECORE_CALLBACK_RENEW;
     }
   if (len)
     p->buf = eina_binbuf_manage_new(buf, len, 0);
   _incr_upload(p);
   if (p->incr)
     ecore_main_fd_handler_active_set(p->fdh, 0);
   else
     {
        _incr_update(p, 1);
        eina_hash_del_by_key(pipes, &p->win);
     }
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_selection_request(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Selection_Request *ev)
{
   Comp_Data_Device_Source *source;
   Eina_List *l;
   Comp *c;
   const char *type;
   Comp_Seat *s = NULL;

   EINA_LIST_FOREACH(comps, l, c)
     {
        Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (c->x11_selection || ((ev->owner == win) && (ev->selection == ECORE_X_ATOM_SELECTION_XDND)))
          {
             s = EINA_INLIST_CONTAINER_GET(c->seats, Comp_Seat);
             break;
          }
     }
   if (!s) return ECORE_CALLBACK_RENEW;
   if (s->drag.source)
     source = s->drag.source;
   else if (s->selection_source)
     source = s->selection_source;
   else
     return ECORE_CALLBACK_RENEW;

   if (ev->target == ECORE_X_ATOM_SELECTION_TARGETS)
     {
        Ecore_X_Atom *atoms;
        int i = 0;

        atoms = alloca((2 + eina_list_count(source->mime_types)) * sizeof(void*));
        EINA_LIST_FOREACH(source->mime_types, l, type)
          atoms[i++] = ecore_x_atom_get(type);
        atoms[i++] = timestamp_atom;
        atoms[i++] = ECORE_X_ATOM_SELECTION_TARGETS;
        ecore_x_window_prop_property_set(ev->requestor, ev->property, ECORE_X_ATOM_ATOM, 32, atoms, i);
        ecore_x_selection_notify_send(ev->requestor, ev->selection, ev->target, ev->property, 0);
     }
   else if (ev->target == timestamp_atom)
     {
        Ecore_X_Time timestamp;

        timestamp = ecore_x_current_time_get();
        ecore_x_window_prop_property_set(ev->requestor, ev->property, int_atom, 32, (void*)&timestamp, 1);
        ecore_x_selection_notify_send(ev->requestor, ev->selection, ev->target, ev->property, 0);
     }
   else
     {
        const char *name;
        Pipe *p;

        name = ecore_x_atom_name_get(ev->target);
        EINA_LIST_FOREACH(source->mime_types, l, type)
          if (eina_streq(name, type))
            {
               int fds[2];

               p = calloc(1, sizeof(Pipe));
               socketpair(AF_UNIX, (SOCK_STREAM | SOCK_CLOEXEC), 0, fds);
               fcntl(fds[0], F_SETFL, O_NONBLOCK);
               p->fdh = ecore_main_fd_handler_add(fds[0], ECORE_FD_READ, x11_pipe_read, p, NULL, NULL);
               p->win = ev->requestor;
               p->source = source;
               wl_data_source_send_send(source->res, type, fds[1]);
               close(fds[1]);
               p->atom = ev->target;
               p->selection = ev->selection;
               p->property = ev->property;
               ecore_x_window_sniff(ev->requestor);
               eina_hash_add(pipes, &p->win, p);
               break;
            }
       free(name);
     }
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_property(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Window_Property *ev)
{
   Pipe *p;

   if (!ev->state) return ECORE_CALLBACK_RENEW;
   p = eina_hash_find(pipes, &ev->win);
   if (!p) return ECORE_CALLBACK_RENEW;
   /* FIXME: WHO FORGOT THE FUCKING STATE FLAG???? */
   ecore_main_fd_handler_active_set(p->fdh, ECORE_FD_READ);
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_dnd_enter(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Xdnd_Enter *ev)
{
   Comp_Seat *s;
   Comp *c;
   Eina_List *l;
   const char **types;
   int i;

   types = alloca(sizeof(void*) * (ev->num_types + 1));
   for (i = 0; i < ev->num_types; i++)
     types[i] = ev->types[i];
   types[ev->num_types] = NULL;

   EINA_LIST_FOREACH(comps, l, c)
     {
        Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (win != ev->win) continue;
        EINA_INLIST_FOREACH(c->seats, s)
          {
             if (s->drag.x11_owner == ev->source) continue;
             s->drag.x11_owner = ev->source;
             for (i = 0; i < ev->num_types; i++)
               s->drag.x11_types = eina_list_append(s->drag.x11_types, eina_stringshare_add(ev->types[i]));
             ecore_wl2_dnd_drag_types_set(s->client_seat, (const char**)types);
             ecore_wl2_dnd_set_actions(s->client_seat);
          }
     }

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_dnd_leave(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Xdnd_Leave *ev)
{
   Comp_Seat *s;
   Comp *c;
   Eina_List *l;

   EINA_LIST_FOREACH(comps, l, c)
     {
        Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (win != ev->win) continue;
        EINA_INLIST_FOREACH(c->seats, s)
          {
             if (s->drag.x11_owner == win) continue; //self drag
             if (s->drag.source)
               wl_data_source_send_cancelled(s->drag.source->res);
             if (s->client_seat)
               ecore_wl2_dnd_drag_end(s->client_seat);
             s->drag.source = NULL;
             s->drag.res = NULL;
             s->drag.enter = NULL;
             s->drag.x11_owner = 0;
          }
     }
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_dnd_position(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Xdnd_Position *ev)
{
   Comp *c;
   Eina_List *l;
   Comp_Seat *s;

   EINA_LIST_FOREACH(comps, l, c)
     {
        int x, y, cx, cy, cw, ch;
        Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (win != ev->win) continue;
        ecore_evas_geometry_get(ecore_evas_ecore_evas_get(c->evas), &x, &y, NULL, NULL);
        evas_object_geometry_get(c->obj, &cx, &cy, &cw, &ch);
        if (!COORDS_INSIDE(ev->position.x, ev->position.y, x + cx, y + cy, cw, ch)) continue;
        EINA_INLIST_FOREACH(c->seats, s)
          dnd_motion(s, ev->position.x - x - cx, ev->position.y - y - cy);
     }
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_dnd_drop(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Xdnd_Drop *ev)
{
   Comp *c;
   Eina_List *l;
   Comp_Seat *s;

   EINA_LIST_FOREACH(comps, l, c)
     {
        int x, y, cx, cy, cw, ch;
        Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (win != ev->win) continue;
        ecore_evas_geometry_get(ecore_evas_ecore_evas_get(c->evas), &x, &y, NULL, NULL);
        evas_object_geometry_get(c->obj, &cx, &cy, &cw, &ch);
        if (!COORDS_INSIDE(ev->position.x, ev->position.y, x + cx, y + cy, cw, ch)) continue;
        EINA_INLIST_FOREACH(c->seats, s)
          drag_grab_button(s, 0, s->drag.id, WL_POINTER_BUTTON_STATE_RELEASED);
     }
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_dnd_finished(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_X_Event_Xdnd_Finished *ev)
{
   Comp *c;
   Eina_List *l;
   Comp_Seat *s;

   EINA_LIST_FOREACH(comps, l, c)
     {
        Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (win != ev->win) continue;
        EINA_INLIST_FOREACH(c->seats, s)
          {
             Comp_Data_Device_Source *source = s->drag.source;
             if (s->drag.x11_owner != win) continue;
             if (s->drag.source) s->drag.source->accepted = ev->completed;
             drag_grab_button(s, 0, s->drag.id, WL_POINTER_BUTTON_STATE_RELEASED);
             data_source_notify_finish(source);
          }
     }
   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
x11_dnd_mouse_up(void *d EINA_UNUSED, int t EINA_UNUSED, Ecore_Event_Mouse_Button *ev)
{
   Comp *c;
   Eina_List *l;
   Comp_Seat *s;

   EINA_LIST_FOREACH(comps, l, c)
     {
        Ecore_Window win = ecore_evas_window_get(ecore_evas_ecore_evas_get(c->evas));
        if (win != ev->event_window) continue;
        EINA_INLIST_FOREACH(c->seats, s)
          if (s->drag.id == ev->buttons)
            {
               ecore_x_dnd_self_drop();
               ecore_x_pointer_ungrab();
               ecore_evas_free(s->drag.proxy_win);
            }
     }
   return ECORE_CALLBACK_RENEW;
}

static void
x11_dnd_move(void *data, Ecore_X_Xdnd_Position *pos)
{
   evas_object_move(data, pos->position.x, pos->position.y);
}

static void
x11_init(void)
{
   Ecore_Event_Handler *h;

   ecore_x_fixes_selection_notification_request(ecore_x_atom_get("CLIPBOARD"));
   ecore_x_fixes_selection_notification_request(ECORE_X_ATOM_SELECTION_XDND);
   h = ecore_event_handler_add(ECORE_X_EVENT_FIXES_SELECTION_NOTIFY, (Ecore_Event_Handler_Cb)x11_fixes_selection_notify, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_SELECTION_NOTIFY, (Ecore_Event_Handler_Cb)x11_selection_notify, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_SELECTION_REQUEST, (Ecore_Event_Handler_Cb)x11_selection_request, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_WINDOW_PROPERTY, (Ecore_Event_Handler_Cb)x11_property, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_XDND_ENTER, (Ecore_Event_Handler_Cb)x11_dnd_enter, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_XDND_LEAVE, (Ecore_Event_Handler_Cb)x11_dnd_leave, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_XDND_POSITION, (Ecore_Event_Handler_Cb)x11_dnd_position, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_XDND_DROP, (Ecore_Event_Handler_Cb)x11_dnd_drop, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_X_EVENT_XDND_FINISHED, (Ecore_Event_Handler_Cb)x11_dnd_finished, NULL);
   handlers = eina_list_append(handlers, h);
   h = ecore_event_handler_add(ECORE_EVENT_MOUSE_BUTTON_UP, (Ecore_Event_Handler_Cb)x11_dnd_mouse_up, NULL);
   handlers = eina_list_append(handlers, h);
   xconvertselection = dlsym(NULL, "XConvertSelection");
   string_atom = ecore_x_atom_get("UTF8_STRING");
   timestamp_atom = ecore_x_atom_get("TIMESTAMP");
   int_atom = ecore_x_atom_get("INTEGER");
   incr_atom = ecore_x_atom_get("TIMESTAMP");
   comp_dnd_atom = ecore_x_atom_get("SIRCMPWIDG_ATOM");
   pipes = eina_hash_int32_new((Eina_Free_Cb)_pipe_free);
}

static void
x11_shutdown(void)
{
   eina_hash_free(pipes);
   pipes = NULL;
}
