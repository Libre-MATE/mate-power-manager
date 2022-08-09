/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2005-2007 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2012-2021 MATE Developers
 *
 * Taken in part from:
 *  - lshal   (C) 2003 David Zeuthen, <david@fubar.dk>
 *  - notibat (C) 2004 Benjamin Kahn, <xkahn@zoned.net>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>
#include <errno.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gpm-common.h"
#include "gpm-icon-names.h"
#include "gpm-manager.h"
#include "gpm-session.h"
#include "org.mate.PowerManager.h"

/**
 * gpm_object_register:
 * @connection: What we want to register to
 * @object: The GObject we want to register
 *
 * Register org.mate.PowerManager on the session bus.
 * This function MUST be called before DBUS service will work.
 *
 * Return value: success
 **/
static gboolean gpm_object_register(DBusGConnection *connection,
                                    GObject *object) {
  DBusGProxy *bus_proxy = NULL;
  GError *error = NULL;
  guint request_name_result;
  gboolean ret;

  bus_proxy = dbus_g_proxy_new_for_name(connection, DBUS_SERVICE_DBUS,
                                        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS);

  ret = dbus_g_proxy_call(bus_proxy, "RequestName", &error, G_TYPE_STRING,
                          GPM_DBUS_SERVICE, G_TYPE_UINT, 0, G_TYPE_INVALID,
                          G_TYPE_UINT, &request_name_result, G_TYPE_INVALID);
  if (error) {
    g_debug("ERROR: %s", error->message);
    g_error_free(error);
  }
  if (!ret) {
    /* abort as the DBUS method failed */
    g_warning("RequestName failed!");
    return FALSE;
  }

  /* free the bus_proxy */
  g_object_unref(G_OBJECT(bus_proxy));

  /* already running */
  if (request_name_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    return FALSE;
  }

  dbus_g_object_type_install_info(GPM_TYPE_MANAGER,
                                  &dbus_glib_gpm_manager_object_info);
  dbus_g_error_domain_register(GPM_MANAGER_ERROR, NULL, GPM_MANAGER_TYPE_ERROR);
  dbus_g_connection_register_g_object(connection, GPM_DBUS_PATH, object);

  return TRUE;
}

/**
 * timed_exit_cb:
 * @loop: The main loop
 *
 * Exits the main loop, which is helpful for valgrinding g-p-m.
 *
 * Return value: FALSE, as we don't want to repeat this action.
 **/
static gboolean timed_exit_cb(GMainLoop *loop) {
  g_main_loop_quit(loop);
  return FALSE;
}

/**
 * gpm_main_stop_cb:
 **/
static void gpm_main_stop_cb(GpmSession *session, GMainLoop *loop) {
  g_main_loop_quit(loop);
}

/**
 * gpm_main_query_end_session_cb:
 **/
static void gpm_main_query_end_session_cb(GpmSession *session, guint flags,
                                          GMainLoop *loop) {
  /* just send response */
  gpm_session_end_session_response(session, TRUE, NULL);
}

/**
 * gpm_main_end_session_cb:
 **/
static void gpm_main_end_session_cb(GpmSession *session, guint flags,
                                    GMainLoop *loop) {
  /* send response */
  gpm_session_end_session_response(session, TRUE, NULL);

  /* exit loop, will unref manager */
  g_main_loop_quit(loop);
}

/**
 * main:
 **/
int main(int argc, char *argv[]) {
  GMainLoop *loop;
  DBusGConnection *system_connection;
  DBusGConnection *session_connection;
  gboolean version = FALSE;
  gboolean timed_exit = FALSE;
  gboolean immediate_exit = FALSE;
  GpmSession *session = NULL;
  GpmManager *manager = NULL;
  GError *error = NULL;
  GOptionContext *context;
  gint ret;
  guint timer_id;

  const GOptionEntry options[] = {
      {"version", '\0', 0, G_OPTION_ARG_NONE, &version,
       N_("Show version of installed program and exit"), NULL},
      {"timed-exit", '\0', 0, G_OPTION_ARG_NONE, &timed_exit,
       N_("Exit after a small delay (for debugging)"), NULL},
      {"immediate-exit", '\0', 0, G_OPTION_ARG_NONE, &immediate_exit,
       N_("Exit after the manager has loaded (for debugging)"), NULL},
      {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}};

  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, MATELOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  dbus_g_thread_init();

  context = g_option_context_new(N_("MATE Power Manager"));
  /* TRANSLATORS: program name, a simple app to view pending updates */
  g_option_context_add_main_entries(context, options, GETTEXT_PACKAGE);
  g_option_context_set_translation_domain(context, GETTEXT_PACKAGE);
  g_option_context_set_summary(context, _("MATE Power Manager"));
  g_option_context_parse(context, &argc, &argv, NULL);

  if (version) {
    g_print("Version %s\n", VERSION);
    goto unref_program;
  }

  dbus_g_thread_init();

  gtk_init(&argc, &argv);

  g_debug("MATE %s %s", GPM_NAME, VERSION);

  /* check dbus connections, exit if not valid */
  system_connection = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
  if (error) {
    g_warning("%s", error->message);
    g_error_free(error);
    g_error(
        "This program cannot start until you start "
        "the dbus system service.\n"
        "It is <b>strongly recommended</b> you reboot "
        "your computer after starting this service.");
  }

  session_connection = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
  if (error) {
    g_warning("%s", error->message);
    g_error_free(error);
    g_error(
        "This program cannot start until you start the "
        "dbus session service.\n\n"
        "This is usually started automatically in X "
        "or mate startup when you start a new session.");
  }

  /* add application specific icons to search path */
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(),
                                    GPM_ICONS_DATA);

  loop = g_main_loop_new(NULL, FALSE);

  /* optionally register with the session */
  session = gpm_session_new();
  g_signal_connect(session, "stop", G_CALLBACK(gpm_main_stop_cb), loop);
  g_signal_connect(session, "query-end-session",
                   G_CALLBACK(gpm_main_query_end_session_cb), loop);
  g_signal_connect(session, "end-session", G_CALLBACK(gpm_main_end_session_cb),
                   loop);
  gpm_session_register_client(session, "mate-power-manager",
                              getenv("DESKTOP_AUTOSTART_ID"));

  /* create a new gui object */
  manager = gpm_manager_new();

  if (!gpm_object_register(session_connection, G_OBJECT(manager))) {
    g_error("%s is already running in this session.", GPM_NAME);
    goto unref_program;
  }

  /* register to be a policy agent, just like kpackagekit does */
  ret = dbus_bus_request_name(
      dbus_g_connection_get_connection(system_connection),
      "org.freedesktop.Policy.Power", DBUS_NAME_FLAG_REPLACE_EXISTING, NULL);
  switch (ret) {
    case DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER:
      g_debug("Successfully acquired interface org.freedesktop.Policy.Power.");
      break;
    case DBUS_REQUEST_NAME_REPLY_IN_QUEUE:
      g_debug("Queued for interface org.freedesktop.Policy.Power.");
      break;
    default:
      break;
  };

  /* Only timeout and close the mainloop if we have specified it
   * on the command line */
  if (timed_exit) {
    timer_id = g_timeout_add_seconds(20, (GSourceFunc)timed_exit_cb, loop);
    g_source_set_name_by_id(timer_id, "[GpmMain] timed-exit");
  }

  if (immediate_exit == FALSE) {
    g_main_loop_run(loop);
  }

  g_main_loop_unref(loop);

  g_object_unref(session);
  g_object_unref(manager);
unref_program:
  g_option_context_free(context);
  return 0;
}
