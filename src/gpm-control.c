/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006-2007 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2012-2021 MATE Developers
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include <gio/gio.h>
#include <glib/gi18n.h>

#ifdef WITH_LIBSECRET
#include <libsecret/secret.h>
#endif /* WITH_LIBSECRET */
#ifdef WITH_KEYRING
#include <gnome-keyring.h>
#endif /* WITH_KEYRING */

#include "gpm-common.h"
#include "gpm-control.h"
#include "gpm-networkmanager.h"

struct GpmControlPrivate {
  GSettings *settings;
};

enum { RESUME, SLEEP, LAST_SIGNAL };

static guint signals[LAST_SIGNAL] = {0};
static gpointer gpm_control_object = NULL;

G_DEFINE_TYPE_WITH_PRIVATE(GpmControl, gpm_control, G_TYPE_OBJECT)

/**
 * gpm_control_error_quark:
 * Return value: Our personal error quark.
 **/
GQuark gpm_control_error_quark(void) {
  static GQuark quark = 0;
  if (!quark) quark = g_quark_from_static_string("gpm_control_error");
  return quark;
}

/**
 * gpm_manager_systemd_shutdown:
 *
 * Shutdown the system using systemd-logind.
 *
 * Return value: fd, -1 on error
 **/
static gboolean gpm_control_systemd_shutdown(void) {
  GError *error = NULL;
  GDBusProxy *proxy;
  GVariant *res = NULL;

  g_debug("Requesting systemd to shutdown");
  proxy = g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL,
      "org.freedesktop.login1", "/org/freedesktop/login1",
      "org.freedesktop.login1.Manager", NULL, &error);
  // append all our arguments
  if (proxy == NULL) {
    g_warning("Error connecting to dbus - %s", error->message);
    g_error_free(error);
    return FALSE;
  }

  res = g_dbus_proxy_call_sync(proxy, "PowerOff", g_variant_new("(b)", FALSE),
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  if (error != NULL) {
    g_warning("Error in dbus - %s", error->message);
    g_error_free(error);
    return FALSE;
  }

  g_variant_unref(res);
  return TRUE;
}

/**
 * gpm_control_shutdown:
 * @control: This class instance
 *
 * Shuts down the computer
 **/
gboolean gpm_control_shutdown(GpmControl *control, GError **error) {
  gboolean ret = FALSE;

  if (LOGIND_RUNNING()) {
    ret = gpm_control_systemd_shutdown();
  }
  return ret;
}

/**
 * gpm_control_suspend:
 **/
gboolean gpm_control_suspend(GpmControl *control, GError **error) {
  gboolean ret = FALSE;
  gboolean nm_sleep;
#ifdef WITH_LIBSECRET
  gboolean lock_libsecret;
  GCancellable *libsecret_cancellable = NULL;
  SecretService *secretservice_proxy = NULL;
  gint num_secrets_locked;
  GList *libsecret_collections = NULL;
#endif /* WITH_LIBSECRET */
#ifdef WITH_KEYRING
  gboolean lock_gnome_keyring;
  GnomeKeyringResult keyres;
#endif /* WITH_KEYRING */

  GError *dbus_error = NULL;
  GDBusProxy *proxy;
  GVariant *res = NULL;

  if (!LOGIND_RUNNING()) {
    goto out;
  }

#ifdef WITH_LIBSECRET
  /* we should perhaps lock keyrings when sleeping #375681 */
  lock_libsecret = g_settings_get_boolean(control->priv->settings,
                                          GPM_SETTINGS_LOCK_KEYRING_SUSPEND);
  if (lock_libsecret) {
    libsecret_cancellable = g_cancellable_new();
    secretservice_proxy = secret_service_get_sync(
        SECRET_SERVICE_LOAD_COLLECTIONS, libsecret_cancellable, error);
    if (secretservice_proxy == NULL) {
      g_warning("failed to connect to secret service");
    } else {
      libsecret_collections =
          secret_service_get_collections(secretservice_proxy);
      if (libsecret_collections == NULL) {
        g_warning("failed to get secret collections");
      } else {
        num_secrets_locked =
            secret_service_lock_sync(secretservice_proxy, libsecret_collections,
                                     libsecret_cancellable, NULL, error);
        if (num_secrets_locked <= 0) g_warning("could not lock keyring");
        g_list_free(libsecret_collections);
      }
      g_object_unref(secretservice_proxy);
    }
    g_object_unref(libsecret_cancellable);
  }
#endif /* WITH_LIBSECRET */
#ifdef WITH_KEYRING
  /* we should perhaps lock keyrings when sleeping #375681 */
  lock_gnome_keyring = g_settings_get_boolean(
      control->priv->settings, GPM_SETTINGS_LOCK_KEYRING_SUSPEND);
  if (lock_gnome_keyring) {
    keyres = gnome_keyring_lock_all_sync();
    if (keyres != GNOME_KEYRING_RESULT_OK) g_warning("could not lock keyring");
  }
#endif /* WITH_KEYRING */

  nm_sleep = g_settings_get_boolean(control->priv->settings,
                                    GPM_SETTINGS_NETWORKMANAGER_SLEEP);
  if (nm_sleep) gpm_networkmanager_sleep();

  /* Do the suspend */
  g_debug("emitting sleep");
  g_signal_emit(control, signals[SLEEP], 0, GPM_CONTROL_ACTION_SUSPEND);

  if (LOGIND_RUNNING()) {
    /* sleep via logind */
    proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL,
        "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", NULL, &dbus_error);
    if (proxy == NULL) {
      g_warning("Error connecting to dbus - %s", dbus_error->message);
      g_error_free(dbus_error);
      ret = FALSE;
      goto out;
    }
    res = g_dbus_proxy_call_sync(proxy, "Suspend", g_variant_new("(b)", FALSE),
                                 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &dbus_error);
    if (dbus_error != NULL) {
      g_warning("Error in dbus - %s", dbus_error->message);
      g_error_free(dbus_error);
      ret = TRUE;
    } else {
      g_variant_unref(res);
      ret = TRUE;
    }
    g_object_unref(proxy);
  }

  g_debug("emitting resume");
  g_signal_emit(control, signals[RESUME], 0, GPM_CONTROL_ACTION_SUSPEND);

  nm_sleep = g_settings_get_boolean(control->priv->settings,
                                    GPM_SETTINGS_NETWORKMANAGER_SLEEP);
  if (nm_sleep) gpm_networkmanager_wake();

out:
  return ret;
}

/**
 * gpm_control_hibernate:
 **/
gboolean gpm_control_hibernate(GpmControl *control, GError **error) {
  gboolean ret = FALSE;
  gboolean nm_sleep;
#ifdef WITH_LIBSECRET
  gboolean lock_libsecret;
  GCancellable *libsecret_cancellable = NULL;
  SecretService *secretservice_proxy = NULL;
  gint num_secrets_locked;
  GList *libsecret_collections = NULL;
#endif /* WITH_LIBSECRET */
#ifdef WITH_KEYRING
  gboolean lock_gnome_keyring;
  GnomeKeyringResult keyres;
#endif /* WITH_KEYRING */

  GError *dbus_error = NULL;
  GDBusProxy *proxy;
  GVariant *res = NULL;

  if (!LOGIND_RUNNING()) {
    goto out;
  }

#ifdef WITH_LIBSECRET
  /* we should perhaps lock keyrings when sleeping #375681 */
  lock_libsecret = g_settings_get_boolean(control->priv->settings,
                                          GPM_SETTINGS_LOCK_KEYRING_SUSPEND);
  if (lock_libsecret) {
    libsecret_cancellable = g_cancellable_new();
    secretservice_proxy = secret_service_get_sync(
        SECRET_SERVICE_LOAD_COLLECTIONS, libsecret_cancellable, error);
    if (secretservice_proxy == NULL) {
      g_warning("failed to connect to secret service");
    } else {
      libsecret_collections =
          secret_service_get_collections(secretservice_proxy);
      if (libsecret_collections == NULL) {
        g_warning("failed to get secret collections");
      } else {
        num_secrets_locked =
            secret_service_lock_sync(secretservice_proxy, libsecret_collections,
                                     libsecret_cancellable, NULL, error);
        if (num_secrets_locked <= 0) g_warning("could not lock keyring");
        g_list_free(libsecret_collections);
      }
      g_object_unref(secretservice_proxy);
    }
    g_object_unref(libsecret_cancellable);
  }
#endif /* WITH_LIBSECRET */
#ifdef WITH_KEYRING
  /* we should perhaps lock keyrings when sleeping #375681 */
  lock_gnome_keyring = g_settings_get_boolean(
      control->priv->settings, GPM_SETTINGS_LOCK_KEYRING_HIBERNATE);
  if (lock_gnome_keyring) {
    keyres = gnome_keyring_lock_all_sync();
    if (keyres != GNOME_KEYRING_RESULT_OK) {
      g_warning("could not lock keyring");
    }
  }
#endif /* WITH_KEYRING */

  nm_sleep = g_settings_get_boolean(control->priv->settings,
                                    GPM_SETTINGS_NETWORKMANAGER_SLEEP);
  if (nm_sleep) gpm_networkmanager_sleep();

  g_debug("emitting sleep");
  g_signal_emit(control, signals[SLEEP], 0, GPM_CONTROL_ACTION_HIBERNATE);

  if (LOGIND_RUNNING()) {
    /* sleep via logind */
    proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL,
        "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", NULL, &dbus_error);
    if (proxy == NULL) {
      g_warning("Error connecting to dbus - %s", dbus_error->message);
      g_error_free(dbus_error);
      ret = FALSE;
      goto out;
    }
    res =
        g_dbus_proxy_call_sync(proxy, "Hibernate", g_variant_new("(b)", FALSE),
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, &dbus_error);
    if (dbus_error != NULL) {
      g_warning("Error in dbus - %s", dbus_error->message);
      g_error_free(dbus_error);
      ret = TRUE;
    } else {
      g_variant_unref(res);
      ret = TRUE;
    }
  }

  g_debug("emitting resume");
  g_signal_emit(control, signals[RESUME], 0, GPM_CONTROL_ACTION_HIBERNATE);

  nm_sleep = g_settings_get_boolean(control->priv->settings,
                                    GPM_SETTINGS_NETWORKMANAGER_SLEEP);
  if (nm_sleep) gpm_networkmanager_wake();

out:
  return ret;
}

/**
 * gpm_control_finalize:
 **/
static void gpm_control_finalize(GObject *object) {
  GpmControl *control;

  g_return_if_fail(object != NULL);
  g_return_if_fail(GPM_IS_CONTROL(object));
  control = GPM_CONTROL(object);

  g_object_unref(control->priv->settings);

  g_return_if_fail(control->priv != NULL);
  G_OBJECT_CLASS(gpm_control_parent_class)->finalize(object);
}

/**
 * gpm_control_class_init:
 **/
static void gpm_control_class_init(GpmControlClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gpm_control_finalize;

  signals[RESUME] =
      g_signal_new("resume", G_TYPE_FROM_CLASS(object_class), G_SIGNAL_RUN_LAST,
                   G_STRUCT_OFFSET(GpmControlClass, resume), NULL, NULL,
                   g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SLEEP] =
      g_signal_new("sleep", G_TYPE_FROM_CLASS(object_class), G_SIGNAL_RUN_LAST,
                   G_STRUCT_OFFSET(GpmControlClass, sleep), NULL, NULL,
                   g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
}

/**
 * gpm_control_init:
 * @control: This control class instance
 **/
static void gpm_control_init(GpmControl *control) {
  control->priv = gpm_control_get_instance_private(control);

  control->priv->settings = g_settings_new(GPM_SETTINGS_SCHEMA);
}

/**
 * gpm_control_new:
 * Return value: A new control class instance.
 **/
GpmControl *gpm_control_new(void) {
  if (gpm_control_object != NULL) {
    g_object_ref(gpm_control_object);
  } else {
    gpm_control_object = g_object_new(GPM_TYPE_CONTROL, NULL);
    g_object_add_weak_pointer(gpm_control_object, &gpm_control_object);
  }
  return GPM_CONTROL(gpm_control_object);
}
