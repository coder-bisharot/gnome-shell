/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "shell-tray-icon.h"
#include "shell-gtk-embed.h"
#include "tray/na-tray-child.h"
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include "st.h"

enum {
   PROP_0,

   PROP_PID,
   PROP_TITLE,
   PROP_WM_CLASS
};

typedef struct _ShellTrayIconPrivate ShellTrayIconPrivate;

struct _ShellTrayIcon
{
    ShellGtkEmbed parent;

    ShellTrayIconPrivate *priv;
};

struct _ShellTrayIconPrivate
{
  NaTrayChild *socket;

  pid_t pid;
  char *title, *wm_class;
};

G_DEFINE_TYPE_WITH_PRIVATE (ShellTrayIcon, shell_tray_icon, SHELL_TYPE_GTK_EMBED);

static void
shell_tray_icon_finalize (GObject *object)
{
  ShellTrayIcon *icon = SHELL_TRAY_ICON (object);

  g_free (icon->priv->title);
  g_free (icon->priv->wm_class);

  G_OBJECT_CLASS (shell_tray_icon_parent_class)->finalize (object);
}

static void
shell_tray_icon_constructed (GObject *object)
{
  GdkWindow *icon_app_window;
  ShellTrayIcon *icon = SHELL_TRAY_ICON (object);
  ShellEmbeddedWindow *window;
  GdkDisplay *display;
  Window plug_xid;
  Atom _NET_WM_PID, type;
  int result, format;
  gulong nitems, bytes_after, *val = NULL;

  /* We do all this now rather than computing it on the fly later,
   * because the shell may want to see their values from a
   * tray-icon-removed signal handler, at which point the plug has
   * already been removed from the socket.
   */

  g_object_get (object, "window", &window, NULL);
  g_return_if_fail (window != NULL);
  icon->priv->socket = NA_TRAY_CHILD (gtk_bin_get_child (GTK_BIN (window)));
  g_object_unref (window);

  icon->priv->title = na_tray_child_get_title (icon->priv->socket);
  na_tray_child_get_wm_class (icon->priv->socket, NULL, &icon->priv->wm_class);

  icon_app_window = gtk_socket_get_plug_window (GTK_SOCKET (icon->priv->socket));
  plug_xid = GDK_WINDOW_XID (icon_app_window);

  display = gtk_widget_get_display (GTK_WIDGET (icon->priv->socket));
  gdk_x11_display_error_trap_push (display);
  _NET_WM_PID = gdk_x11_get_xatom_by_name_for_display (display, "_NET_WM_PID");
  result = XGetWindowProperty (GDK_DISPLAY_XDISPLAY (display), plug_xid,
                               _NET_WM_PID, 0, G_MAXLONG, False, XA_CARDINAL,
                               &type, &format, &nitems,
                               &bytes_after, (guchar **)&val);
  if (!gdk_x11_display_error_trap_pop (display) &&
      result == Success &&
      type == XA_CARDINAL &&
      nitems == 1)
    icon->priv->pid = *val;

  if (val)
    XFree (val);
}

static void
shell_tray_icon_get_property (GObject         *object,
                              guint            prop_id,
                              GValue          *value,
                              GParamSpec      *pspec)
{
  ShellTrayIcon *icon = SHELL_TRAY_ICON (object);

  switch (prop_id)
    {
    case PROP_PID:
      g_value_set_uint (value, icon->priv->pid);
      break;

    case PROP_TITLE:
      g_value_set_string (value, icon->priv->title);
      break;

    case PROP_WM_CLASS:
      g_value_set_string (value, icon->priv->wm_class);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_tray_icon_class_init (ShellTrayIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_tray_icon_get_property;
  object_class->constructed  = shell_tray_icon_constructed;
  object_class->finalize     = shell_tray_icon_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PID,
                                   g_param_spec_uint ("pid",
                                                      "PID",
                                                      "The PID of the icon's application",
                                                      0, G_MAXUINT, 0,
                                                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_TITLE,
                                   g_param_spec_string ("title",
                                                        "Title",
                                                        "The icon's window title",
                                                        NULL,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_WM_CLASS,
                                   g_param_spec_string ("wm-class",
                                                        "WM Class",
                                                        "The icon's window WM_CLASS",
                                                        NULL,
                                                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void
shell_tray_icon_init (ShellTrayIcon *icon)
{
  icon->priv = shell_tray_icon_get_instance_private (icon);
}

/*
 * Public API
 */
ClutterActor *
shell_tray_icon_new (ShellEmbeddedWindow *window)
{
  g_return_val_if_fail (SHELL_IS_EMBEDDED_WINDOW (window), NULL);

  return g_object_new (SHELL_TYPE_TRAY_ICON,
                       "window", window,
                       NULL);
}

/**
 * shell_tray_icon_click:
 * @icon: a #ShellTrayIcon
 * @event: the #ClutterEvent triggering the fake click
 *
 * Fakes a press and release on @icon. @event must be a
 * %CLUTTER_BUTTON_RELEASE, %CLUTTER_KEY_PRESS or %CLUTTER_KEY_RELEASE event.
 * Its relevant details will be passed on to the icon, but its
 * coordinates will be ignored; the click is
 * always made on the center of @icon.
 */
void
shell_tray_icon_click (ShellTrayIcon *icon,
                       ClutterEvent  *event)
{
  XKeyEvent xkevent;
  XButtonEvent xbevent;
  XCrossingEvent xcevent;
  GdkDisplay *display;
  GdkWindow *remote_window;
  GdkScreen *screen;
  int x_root, y_root;
  Display *xdisplay;
  Window xwindow, xrootwindow;
  ClutterEventType event_type = clutter_event_type (event);

  g_return_if_fail (event_type == CLUTTER_BUTTON_RELEASE ||
                    event_type == CLUTTER_KEY_PRESS ||
                    event_type == CLUTTER_KEY_RELEASE);

  remote_window = gtk_socket_get_plug_window (GTK_SOCKET (icon->priv->socket));
  if (remote_window == NULL)
    {
      g_warning ("shell tray: plug window is gone");
      return;
    }
  xdisplay = GDK_WINDOW_XDISPLAY (remote_window);

  display = gdk_x11_lookup_xdisplay (xdisplay);
  gdk_x11_display_error_trap_push (display);

  xwindow = GDK_WINDOW_XID (remote_window);
  screen = gdk_window_get_screen (remote_window);
  xrootwindow = GDK_WINDOW_XID (gdk_screen_get_root_window (screen));
  gdk_window_get_origin (remote_window, &x_root, &y_root);


  /* First make the icon believe the pointer is inside it */
  xcevent.type = EnterNotify;
  xcevent.window = xwindow;
  xcevent.root = xrootwindow;
  xcevent.subwindow = None;
  xcevent.time = clutter_event_get_time (event);
  xcevent.x = gdk_window_get_width (remote_window) / 2;
  xcevent.y = gdk_window_get_height (remote_window) / 2;
  xcevent.x_root = x_root + xcevent.x;
  xcevent.y_root = y_root + xcevent.y;
  xcevent.mode = NotifyNormal;
  xcevent.detail = NotifyNonlinear;
  xcevent.same_screen = True;
  XSendEvent (xdisplay, xwindow, False, 0, (XEvent *)&xcevent);

  /* Now do the click */
  if (event_type == CLUTTER_BUTTON_RELEASE)
    {
      xbevent.window = xwindow;
      xbevent.root = xrootwindow;
      xbevent.subwindow = None;
      xbevent.time = xcevent.time;
      xbevent.x = xcevent.x;
      xbevent.y = xcevent.y;
      xbevent.x_root = xcevent.x_root;
      xbevent.y_root = xcevent.y_root;
      xbevent.state = clutter_event_get_state (event);
      xbevent.same_screen = True;
      xbevent.type = ButtonPress;
      xbevent.button = clutter_event_get_button (event);
      XSendEvent (xdisplay, xwindow, False, 0, (XEvent *)&xbevent);

      xbevent.type = ButtonRelease;
      XSendEvent (xdisplay, xwindow, False, 0, (XEvent *)&xbevent);
    }
  else
    {
      xkevent.window = xwindow;
      xkevent.root = xrootwindow;
      xkevent.subwindow = None;
      xkevent.time = xcevent.time;
      xkevent.x = xcevent.x;
      xkevent.y = xcevent.y;
      xkevent.x_root = xcevent.x_root;
      xkevent.y_root = xcevent.y_root;
      xkevent.state = clutter_event_get_state (event);
      xkevent.same_screen = True;
      xkevent.keycode = clutter_event_get_key_code (event);

      xkevent.type = KeyPress;
      XSendEvent (xdisplay, xwindow, False, 0, (XEvent *)&xkevent);

      if (event_type == CLUTTER_KEY_RELEASE)
        {
          /* If the application takes a grab on KeyPress, we don't
           * want to send it a KeyRelease. There's no good way of
           * knowing whether a tray icon will take a grab, so just
           * assume it does, and don't send the KeyRelease. That might
           * make the tracking for key events messed up if it doesn't take
           * a grab, but the tray icon won't get key focus in normal cases,
           * so let's hope this isn't too damaging...
           */
          xkevent.type = KeyRelease;
          XSendEvent (xdisplay, xwindow, False, 0, (XEvent *)&xkevent);
        }
    }

  /* And move the pointer back out */
  xcevent.type = LeaveNotify;
  XSendEvent (xdisplay, xwindow, False, 0, (XEvent *)&xcevent);

  gdk_x11_display_error_trap_pop_ignored (display);
}
