/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright 2026 Pexip
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * GAndroidNetworkMonitor
 *
 * This is the GLib network monitor implementation for Android. Android does
 * not expose the routing table or network state through any of the usual
 * POSIX/Linux mechanisms (netlink is blocked by SELinux for apps from
 * Android 10 onwards, /proc/net/* is locked down, getifaddrs() lacks route
 * information). The supported way to observe network state on Android is
 * through `android.net.ConnectivityManager` and `NetworkCallback`, which is
 * a Java-only API.
 *
 * This file is the native (C/JNI) half of the implementation. The Java
 * half is `gio/PexipNetworkMonitor.java`. The flow is:
 *
 *   1. The application calls
 *        g_android_network_monitor_set_application_context(env, ctx)
 *      once at startup, passing in its Application `Context`.
 *
 *   2. When the GIO subsystem creates the default `GNetworkMonitor`, the
 *      Android monitor gets selected (highest priority on Android), and
 *      its `initable_init()` instantiates the Java helper class
 *      `com.pexip.glib.PexipNetworkMonitor`, which registers a default
 *      `NetworkCallback` with `ConnectivityManager`.
 *
 *   3. Whenever the network state changes, the Java side calls back into
 *      this file via the exported JNI function
 *        Java_com_pexip_glib_PexipNetworkMonitor_nativeOnNetworkChanged()
 *      with the new state (available/metered/validated/transport) and the
 *      current routing table (as a `String[]` of
 *      "family;prefix;dest;gateway;ifname;flags" entries).
 *
 *   4. This file marshals that into the same `NetworkStatusData` /
 *      `GInetAddressMask` representation used by the other platform
 *      monitors (notably gapplenetworkmonitor.m, whose route-translation
 *      logic this file mirrors), then dispatches the update onto the
 *      monitor's main context.
 *
 * See the bundled README-android.md for application integration details
 * (manifest permissions, where to place the .java file, etc.).
 */

#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <jni.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "gandroidnetworkmonitor.h"
#include "ginetaddress.h"
#include "ginetaddressmask.h"
#include "ginitable.h"
#include "gioerror.h"
#include "giomodule-priv.h"
#include "glibintl.h"
#include "gnetworkmonitor.h"
#include "gsocket.h"

#define ANDROID_NETMON_CLASS_NAME "com/pexip/glib/PexipNetworkMonitor"

/* Mirrors the values used in the Java helper for the `transport` field. */
enum
{
  ANDROID_TRANSPORT_UNKNOWN = 0,
  ANDROID_TRANSPORT_WIFI = 1,
  ANDROID_TRANSPORT_CELLULAR = 2,
  ANDROID_TRANSPORT_ETHERNET = 3,
  ANDROID_TRANSPORT_VPN = 4,
  ANDROID_TRANSPORT_BLUETOOTH = 5,
};

static GInitableIface *initable_parent_iface;
static void g_android_network_monitor_iface_init (GNetworkMonitorInterface *iface);
static void g_android_network_monitor_initable_iface_init (GInitableIface *iface);

enum
{
  PROP_0,

  PROP_NETWORK_AVAILABLE,
  PROP_NETWORK_METERED,
  PROP_CONNECTIVITY
};

struct _GAndroidNetworkMonitorPrivate
{
  gboolean initialized;
  GError *init_error;
  GMainContext *main_context;

  /* Global ref to the Java PexipNetworkMonitor instance. */
  jobject java_monitor;
  /* Cached method id for PexipNetworkMonitor.stop()V. */
  jmethodID java_stop_method;

  /* Latched state, mirrored from the most recent Java callback. */
  gboolean available;
  gboolean validated;
  gboolean metered;
  gint transport;
  gboolean has_ipv4_gateway;
  gboolean has_ipv6_gateway;
};

#define g_android_network_monitor_get_type _g_android_network_monitor_get_type
G_DEFINE_TYPE_WITH_CODE (
    GAndroidNetworkMonitor,
    g_android_network_monitor,
    G_TYPE_NETWORK_MONITOR_BASE,
    G_ADD_PRIVATE (GAndroidNetworkMonitor)
        G_IMPLEMENT_INTERFACE (G_TYPE_NETWORK_MONITOR, g_android_network_monitor_iface_init)
            G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, g_android_network_monitor_initable_iface_init)
                _g_io_modules_ensure_extension_points_registered ();
    g_io_extension_point_implement (G_NETWORK_MONITOR_EXTENSION_POINT_NAME, g_define_type_id, "android", 20))

/* --- Process-wide JNI state ----------------------------------------------- */

/* The JavaVM is cached by JNI_OnLoad. We also expose a setter so that
 * applications which load libgio via a path other than System.loadLibrary
 * (for example, if it's a transitive dependency of another .so that
 * provides its own JNI_OnLoad) can still inject it. */
static JavaVM *g_jvm = NULL;
/* Global ref to the application Context; set via the public setter. */
static jobject g_application_context = NULL;
static GMutex g_jni_state_mutex;

/* Map of (gpointer)handle -> GAndroidNetworkMonitor*, used to look up the
 * monitor from the JNI callback and to defend against use-after-finalize
 * if a callback races with object destruction. */
static GHashTable *g_live_monitors = NULL;
static GMutex g_live_monitors_mutex;

static void
ensure_live_monitors_table (void)
{
  static gsize once = 0;
  if (g_once_init_enter (&once))
    {
      g_live_monitors = g_hash_table_new (g_direct_hash, g_direct_equal);
      g_once_init_leave (&once, 1);
    }
}

static GAndroidNetworkMonitor *
lookup_and_ref_monitor (gpointer handle)
{
  GAndroidNetworkMonitor *monitor = NULL;

  ensure_live_monitors_table ();
  g_mutex_lock (&g_live_monitors_mutex);
  if (g_hash_table_contains (g_live_monitors, handle))
    monitor = g_object_ref (G_ANDROID_NETWORK_MONITOR (handle));
  g_mutex_unlock (&g_live_monitors_mutex);

  return monitor;
}

static void
register_monitor (GAndroidNetworkMonitor *monitor)
{
  ensure_live_monitors_table ();
  g_mutex_lock (&g_live_monitors_mutex);
  g_hash_table_add (g_live_monitors, monitor);
  g_mutex_unlock (&g_live_monitors_mutex);
}

static void
unregister_monitor (GAndroidNetworkMonitor *monitor)
{
  ensure_live_monitors_table ();
  g_mutex_lock (&g_live_monitors_mutex);
  g_hash_table_remove (g_live_monitors, monitor);
  g_mutex_unlock (&g_live_monitors_mutex);
}

/* Attach the calling thread to the JVM. *out_attached is set to TRUE if the
 * caller needs to call DetachCurrentThread when done. Returns NULL if the
 * JavaVM is not available. */
static JNIEnv *
attach_current_thread (gboolean *out_attached)
{
  JNIEnv *env = NULL;
  JavaVM *vm;

  *out_attached = FALSE;

  g_mutex_lock (&g_jni_state_mutex);
  vm = g_jvm;
  g_mutex_unlock (&g_jni_state_mutex);

  if (vm == NULL)
    return NULL;

  if ((*vm)->GetEnv (vm, (void **) &env, JNI_VERSION_1_6) == JNI_OK)
    return env;

  if ((*vm)->AttachCurrentThread (vm, (void **) &env, NULL) == JNI_OK)
    {
      *out_attached = TRUE;
      return env;
    }

  return NULL;
}

static void
detach_current_thread (void)
{
  JavaVM *vm;

  g_mutex_lock (&g_jni_state_mutex);
  vm = g_jvm;
  g_mutex_unlock (&g_jni_state_mutex);

  if (vm != NULL)
    (*vm)->DetachCurrentThread (vm);
}

/* --- Route data model (mirrors gapplenetworkmonitor.m) -------------------- */

typedef enum
{
  ROUTE_TYPE_INTERFACE,
  ROUTE_TYPE_GATEWAY,
} RouteType;

typedef struct
{
  RouteType type;
  int af;
  char if_name[32];
  int if_flags;
  struct sockaddr_storage addr;     /* an address inside the network */
  struct sockaddr_storage network;  /* network address (addr & netmask) */
  struct sockaddr_storage gwaddr;   /* gateway, when type == ROUTE_TYPE_GATEWAY */
  gint prefix_len;
} Route;

typedef struct
{
  GAndroidNetworkMonitor *monitor; /* owned ref */
  gboolean available;
  gboolean validated;
  gboolean metered;
  gint transport;

  GList *ipv4_gateways; /* Route* */
  GList *ipv6_gateways; /* Route* */
  GList *local_routes;  /* Route* */
} NetworkStatusData;

static Route *
route_new (RouteType type, int af, const char *if_name)
{
  Route *route = g_new0 (Route, 1);
  route->type = type;
  route->af = af;
  if (if_name)
    {
      strncpy (route->if_name, if_name, sizeof (route->if_name) - 1);
      route->if_name[sizeof (route->if_name) - 1] = '\0';
    }
  return route;
}

static void
network_status_data_free (NetworkStatusData *ptr)
{
  if (ptr)
    {
      g_list_free_full (ptr->ipv4_gateways, g_free);
      g_list_free_full (ptr->ipv6_gateways, g_free);
      g_list_free_full (ptr->local_routes, g_free);
      g_clear_object (&ptr->monitor);
      g_free (ptr);
    }
}

static const char *
transport_to_string (gint transport)
{
  switch (transport)
    {
    case ANDROID_TRANSPORT_WIFI:
      return "Wi-Fi";
    case ANDROID_TRANSPORT_CELLULAR:
      return "Cellular";
    case ANDROID_TRANSPORT_ETHERNET:
      return "Ethernet";
    case ANDROID_TRANSPORT_VPN:
      return "VPN";
    case ANDROID_TRANSPORT_BLUETOOTH:
      return "Bluetooth";
    default:
      return "unknown transport";
    }
}

/* --- Route parsing -------------------------------------------------------- */

/* Parse one route descriptor sent by the Java side. The descriptor format
 * is:
 *   "family;prefix;dest;gateway;ifname;flags"
 * where:
 *   family   = "4" or "6"
 *   prefix   = decimal prefix length (0..32 or 0..128)
 *   dest     = textual address ("0.0.0.0", "::") - never empty
 *   gateway  = textual gateway address, or empty for an on-link route
 *   ifname   = interface name, e.g. "wlan0", may be empty
 *   flags    = decimal bitfield: 1 = default route, 2 = loopback,
 *              4 = point-to-point
 * Returns a newly allocated Route, or NULL on parse error.
 */
static Route *
parse_route_descriptor (const char *desc)
{
  gchar **parts;
  Route *route = NULL;
  int af;
  gint prefix_len;
  const char *dest, *gateway, *ifname, *flags_str;
  int flags;
  struct sockaddr_in *sin;
  struct sockaddr_in6 *sin6;

  if (desc == NULL)
    return NULL;

  parts = g_strsplit (desc, ";", -1);
  if (g_strv_length (parts) < 6)
    {
      g_strfreev (parts);
      return NULL;
    }

  if (g_strcmp0 (parts[0], "4") == 0)
    af = AF_INET;
  else if (g_strcmp0 (parts[0], "6") == 0)
    af = AF_INET6;
  else
    {
      g_strfreev (parts);
      return NULL;
    }

  prefix_len = (gint) g_ascii_strtoll (parts[1], NULL, 10);
  dest = parts[2];
  gateway = parts[3];
  ifname = parts[4];
  flags_str = parts[5];
  flags = (int) g_ascii_strtoll (flags_str, NULL, 10);

  if (prefix_len < 0 ||
      (af == AF_INET && prefix_len > 32) ||
      (af == AF_INET6 && prefix_len > 128))
    {
      g_strfreev (parts);
      return NULL;
    }

  /* Determine route type: a route with a non-empty gateway is a
   * gateway route; otherwise it's an on-link interface route. A default
   * route (prefix 0) with a gateway becomes a gateway entry; a default
   * route without a gateway is unusual and we treat it as on-link.
   */
  if (gateway != NULL && *gateway != '\0')
    route = route_new (ROUTE_TYPE_GATEWAY, af, ifname);
  else
    route = route_new (ROUTE_TYPE_INTERFACE, af, ifname);

  route->prefix_len = prefix_len;
  route->if_flags = flags;

  /* Parse dest. */
  if (af == AF_INET)
    {
      sin = (struct sockaddr_in *) &route->network;
      sin->sin_family = AF_INET;
      if (inet_pton (AF_INET, dest, &sin->sin_addr) != 1)
        {
          g_free (route);
          g_strfreev (parts);
          return NULL;
        }
      /* Use the same address as a representative "addr inside network". */
      memcpy (&route->addr, sin, sizeof (*sin));
    }
  else
    {
      sin6 = (struct sockaddr_in6 *) &route->network;
      sin6->sin6_family = AF_INET6;
      if (inet_pton (AF_INET6, dest, &sin6->sin6_addr) != 1)
        {
          g_free (route);
          g_strfreev (parts);
          return NULL;
        }
      memcpy (&route->addr, sin6, sizeof (*sin6));
    }

  /* Parse gateway, if any. */
  if (route->type == ROUTE_TYPE_GATEWAY)
    {
      if (af == AF_INET)
        {
          sin = (struct sockaddr_in *) &route->gwaddr;
          sin->sin_family = AF_INET;
          if (inet_pton (AF_INET, gateway, &sin->sin_addr) != 1)
            {
              g_free (route);
              g_strfreev (parts);
              return NULL;
            }
        }
      else
        {
          sin6 = (struct sockaddr_in6 *) &route->gwaddr;
          sin6->sin6_family = AF_INET6;
          if (inet_pton (AF_INET6, gateway, &sin6->sin6_addr) != 1)
            {
              g_free (route);
              g_strfreev (parts);
              return NULL;
            }
        }
    }

  g_strfreev (parts);
  return route;
}

static gboolean
is_default_route (const Route *route)
{
  return route->type == ROUTE_TYPE_GATEWAY && route->prefix_len == 0;
}

/* --- Logging -------------------------------------------------------------- */

static void
_print_route (Route *route, GString *msg)
{
  char buf_network[INET6_ADDRSTRLEN] = "";
  char buf_gwaddr[INET6_ADDRSTRLEN] = "";

  if (route->af == AF_INET)
    {
      struct sockaddr_in *sin_network = (struct sockaddr_in *) &route->network;
      inet_ntop (AF_INET, &sin_network->sin_addr, buf_network, sizeof (buf_network));
      if (route->type == ROUTE_TYPE_GATEWAY)
        {
          struct sockaddr_in *sin_gw = (struct sockaddr_in *) &route->gwaddr;
          inet_ntop (AF_INET, &sin_gw->sin_addr, buf_gwaddr, sizeof (buf_gwaddr));
        }
    }
  else if (route->af == AF_INET6)
    {
      struct sockaddr_in6 *sin6_network = (struct sockaddr_in6 *) &route->network;
      inet_ntop (AF_INET6, &sin6_network->sin6_addr, buf_network, sizeof (buf_network));
      if (route->type == ROUTE_TYPE_GATEWAY)
        {
          struct sockaddr_in6 *sin6_gw = (struct sockaddr_in6 *) &route->gwaddr;
          inet_ntop (AF_INET6, &sin6_gw->sin6_addr, buf_gwaddr, sizeof (buf_gwaddr));
        }
    }

  if (route->type == ROUTE_TYPE_INTERFACE)
    g_string_append_printf (msg, "%s/%d interface %s\n",
                            buf_network, route->prefix_len,
                            route->if_name[0] ? route->if_name : "?");
  else
    g_string_append_printf (msg, "%s/%d via gateway %s%s%s\n",
                            buf_network, route->prefix_len, buf_gwaddr,
                            route->if_name[0] ? " on " : "",
                            route->if_name[0] ? route->if_name : "");
}

static void
_network_update_log_update (NetworkStatusData *status_data,
                            gboolean           log_routing_table)
{
  GString *msg;

  if (status_data == NULL)
    return;

  if (!status_data->available)
    msg = g_string_new ("Network path is DOWN.");
  else
    {
      msg = g_string_new ("Network path is UP.");
      g_string_append_printf (msg, " Connected via %s.",
                              transport_to_string (status_data->transport));
      if (status_data->metered)
        msg = g_string_append (msg, " Path is metered.");
      if (status_data->validated)
        msg = g_string_append (msg, " Path is validated (internet reachable).");
      else
        msg = g_string_append (msg, " Path is not yet validated.");
    }

  if (status_data->available && log_routing_table)
    {
      GList *iter;
      GList *lists[3] = {
        status_data->ipv4_gateways,
        status_data->ipv6_gateways,
        status_data->local_routes,
      };
      int i;
      msg = g_string_append (msg, "\nRouting table:\n");
      for (i = 0; i < 3; i++)
        for (iter = lists[i]; iter != NULL; iter = iter->next)
          _print_route (iter->data, msg);
    }

  g_debug ("%s", msg->str);
  g_string_free (msg, TRUE);
}

/* --- Pushing the routing table down to GNetworkMonitorBase ---------------- */

static GInetAddressMask *
get_network_mask (GSocketFamily family, const guint8 *dest, gsize len)
{
  GInetAddressMask *network;
  GInetAddress *dest_addr;

  if (dest != NULL)
    dest_addr = g_inet_address_new_from_bytes (dest, family);
  else
    dest_addr = g_inet_address_new_any (family);

  network = g_inet_address_mask_new (dest_addr, len, NULL);
  g_object_unref (dest_addr);

  return network;
}

static void
_network_update_set_base_routes (GAndroidNetworkMonitor *self,
                                 NetworkStatusData      *status_data)
{
  GPtrArray *networks;
  gint local_routes_len = g_list_length (status_data->local_routes);
  gint route_count = local_routes_len
                     + (status_data->ipv4_gateways == NULL ? 0 : 1)
                     + (status_data->ipv6_gateways == NULL ? 0 : 1);
  GList *iter;

  if (route_count == 0)
    {
      g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (self), NULL, 0);
      return;
    }

  networks = g_ptr_array_new_full (route_count, g_object_unref);

  if (status_data->ipv4_gateways != NULL)
    {
      GInetAddressMask *network = get_network_mask (G_SOCKET_FAMILY_IPV4, NULL, 0);
      if (network != NULL)
        g_ptr_array_add (networks, network);
    }

  if (status_data->ipv6_gateways != NULL)
    {
      GInetAddressMask *network = get_network_mask (G_SOCKET_FAMILY_IPV6, NULL, 0);
      if (network != NULL)
        g_ptr_array_add (networks, network);
    }

  for (iter = status_data->local_routes; iter != NULL; iter = iter->next)
    {
      Route *local_route = iter->data;
      GInetAddressMask *network;
      GSocketFamily family = G_SOCKET_FAMILY_INVALID;
      const guint8 *dest = NULL;

      if (local_route->af == AF_INET)
        {
          family = G_SOCKET_FAMILY_IPV4;
          dest = (const guint8 *) &((struct sockaddr_in *) &local_route->network)->sin_addr;
        }
      else if (local_route->af == AF_INET6)
        {
          family = G_SOCKET_FAMILY_IPV6;
          dest = (const guint8 *) &((struct sockaddr_in6 *) &local_route->network)->sin6_addr;
        }
      else
        continue;

      network = get_network_mask (family, dest, local_route->prefix_len);
      if (network == NULL)
        continue;

      g_ptr_array_add (networks, network);
    }

  g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (self),
                                       (GInetAddressMask **) networks->pdata,
                                       networks->len);
  g_ptr_array_free (networks, TRUE);
}

/* --- Main-context dispatch ------------------------------------------------ */

static gboolean
_network_update_apply (gpointer user_data)
{
  NetworkStatusData *status_data = user_data;
  GAndroidNetworkMonitor *self = status_data->monitor;

  self->priv->available = status_data->available;
  self->priv->validated = status_data->validated;
  self->priv->metered = status_data->metered;
  self->priv->transport = status_data->transport;
  self->priv->has_ipv4_gateway = (status_data->ipv4_gateways != NULL);
  self->priv->has_ipv6_gateway = (status_data->ipv6_gateways != NULL);

  _network_update_set_base_routes (self, status_data);
  _network_update_log_update (status_data, TRUE);

  /* Notify property listeners. The "network-changed" signal is emitted by
   * GNetworkMonitorBase whenever the route set changes. */
  g_object_notify (G_OBJECT (self), "network-available");
  g_object_notify (G_OBJECT (self), "network-metered");

  return G_SOURCE_REMOVE;
}

/* --- JNI entry point: invoked by the Java helper ------------------------- */

static gchar *
jstring_to_utf8 (JNIEnv *env, jstring jstr)
{
  const char *chars;
  gchar *out;

  if (jstr == NULL)
    return NULL;

  chars = (*env)->GetStringUTFChars (env, jstr, NULL);
  if (chars == NULL)
    return NULL;

  out = g_strdup (chars);
  (*env)->ReleaseStringUTFChars (env, jstr, chars);
  return out;
}

JNIEXPORT void JNICALL
Java_com_pexip_glib_PexipNetworkMonitor_nativeOnNetworkChanged (JNIEnv *env,
                                                                jobject self,
                                                                jlong handle,
                                                                jboolean available,
                                                                jboolean validated,
                                                                jboolean metered,
                                                                jint transport,
                                                                jobjectArray routes)
{
  GAndroidNetworkMonitor *monitor;
  NetworkStatusData *status_data;
  GSource *source;
  jsize n_routes, i;

  (void) self;

  monitor = lookup_and_ref_monitor (GINT_TO_POINTER ((gintptr) handle));
  if (monitor == NULL)
    {
      /* The monitor has already been finalized; nothing to do. */
      return;
    }

  status_data = g_new0 (NetworkStatusData, 1);
  status_data->monitor = monitor; /* takes the reference */
  status_data->available = (available == JNI_TRUE);
  status_data->validated = (validated == JNI_TRUE);
  status_data->metered = (metered == JNI_TRUE);
  status_data->transport = (gint) transport;

  if (routes != NULL)
    {
      n_routes = (*env)->GetArrayLength (env, routes);
      for (i = 0; i < n_routes; i++)
        {
          jstring jdesc = (jstring) (*env)->GetObjectArrayElement (env, routes, i);
          gchar *desc;
          Route *route;

          if (jdesc == NULL)
            continue;

          desc = jstring_to_utf8 (env, jdesc);
          (*env)->DeleteLocalRef (env, jdesc);
          if (desc == NULL)
            continue;

          route = parse_route_descriptor (desc);
          g_free (desc);
          if (route == NULL)
            continue;

          if (is_default_route (route))
            {
              if (route->af == AF_INET)
                status_data->ipv4_gateways = g_list_append (status_data->ipv4_gateways, route);
              else
                status_data->ipv6_gateways = g_list_append (status_data->ipv6_gateways, route);
            }
          else
            {
              status_data->local_routes = g_list_append (status_data->local_routes, route);
            }
        }
    }

  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, _network_update_apply, status_data,
                         (GDestroyNotify) network_status_data_free);
  g_source_attach (source, monitor->priv->main_context);
  g_source_unref (source);
}

/* --- Public setters ------------------------------------------------------- */

void
g_android_network_monitor_set_application_context (JNIEnv *env, jobject context)
{
  jobject old_ref = NULL;
  jobject new_ref = NULL;

  if (env != NULL && context != NULL)
    {
      new_ref = (*env)->NewGlobalRef (env, context);

      /* Cache the JavaVM if we don't have it yet (e.g. JNI_OnLoad ran in a
       * sibling .so that doesn't share statics with us). */
      g_mutex_lock (&g_jni_state_mutex);
      if (g_jvm == NULL)
        (*env)->GetJavaVM (env, &g_jvm);
      g_mutex_unlock (&g_jni_state_mutex);
    }

  g_mutex_lock (&g_jni_state_mutex);
  old_ref = g_application_context;
  g_application_context = new_ref;
  g_mutex_unlock (&g_jni_state_mutex);

  if (old_ref != NULL && env != NULL)
    (*env)->DeleteGlobalRef (env, old_ref);
}

/* JNI_OnLoad: stash the JavaVM. Defined only when targeting Android so we
 * don't accidentally export a JNI_OnLoad symbol on other platforms. */
JNIEXPORT jint JNICALL
JNI_OnLoad (JavaVM *vm, void *reserved)
{
  (void) reserved;
  g_mutex_lock (&g_jni_state_mutex);
  if (g_jvm == NULL)
    g_jvm = vm;
  g_mutex_unlock (&g_jni_state_mutex);
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
JNI_OnUnload (JavaVM *vm, void *reserved)
{
  jobject ctx;

  (void) vm;
  (void) reserved;

  g_mutex_lock (&g_jni_state_mutex);
  ctx = g_application_context;
  g_application_context = NULL;
  g_jvm = NULL;
  g_mutex_unlock (&g_jni_state_mutex);

  if (ctx != NULL)
    {
      JNIEnv *env = NULL;
      if ((*vm)->GetEnv (vm, (void **) &env, JNI_VERSION_1_6) == JNI_OK && env != NULL)
        (*env)->DeleteGlobalRef (env, ctx);
    }
}

/* --- GObject scaffolding -------------------------------------------------- */

static void
g_android_network_monitor_init (GAndroidNetworkMonitor *self)
{
  self->priv = g_android_network_monitor_get_instance_private (self);
  self->priv->transport = ANDROID_TRANSPORT_UNKNOWN;
}

static void
g_android_network_monitor_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  GAndroidNetworkMonitor *self = G_ANDROID_NETWORK_MONITOR (object);

  switch (prop_id)
    {
    case PROP_NETWORK_AVAILABLE:
      g_value_set_boolean (value, self->priv->available);
      break;

    case PROP_NETWORK_METERED:
      g_value_set_boolean (value, self->priv->metered);
      break;

    case PROP_CONNECTIVITY:
      if (!self->priv->available)
        g_value_set_enum (value, G_NETWORK_CONNECTIVITY_LOCAL);
      else if (!self->priv->validated)
        g_value_set_enum (value, G_NETWORK_CONNECTIVITY_LIMITED);
      else
        g_value_set_enum (value, G_NETWORK_CONNECTIVITY_FULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

/* Look up the Java class, allocate the helper, store global refs and method
 * ids in `self->priv`. Sets `*error` and returns FALSE on failure. */
static gboolean
init_java_side (GAndroidNetworkMonitor *self, GError **error)
{
  JNIEnv *env;
  gboolean attached = FALSE;
  jclass cls = NULL;
  jmethodID ctor = NULL;
  jobject ctx = NULL;
  jobject local_obj = NULL;
  gboolean ok = FALSE;

  env = attach_current_thread (&attached);
  if (env == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                           "Android network monitor: JavaVM unavailable. "
                           "Ensure libgio is loaded via System.loadLibrary "
                           "from a Java process.");
      return FALSE;
    }

  g_mutex_lock (&g_jni_state_mutex);
  if (g_application_context != NULL)
    ctx = (*env)->NewLocalRef (env, g_application_context);
  g_mutex_unlock (&g_jni_state_mutex);

  if (ctx == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                           "Android network monitor: application Context not set. "
                           "Call g_android_network_monitor_set_application_context() "
                           "from your Application's onCreate().");
      goto out;
    }

  cls = (*env)->FindClass (env, ANDROID_NETMON_CLASS_NAME);
  if (cls == NULL || (*env)->ExceptionCheck (env))
    {
      (*env)->ExceptionClear (env);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "Android network monitor: helper class "
                           ANDROID_NETMON_CLASS_NAME " not found. "
                           "Ensure PexipNetworkMonitor.java is included in your APK.");
      goto out;
    }

  ctor = (*env)->GetMethodID (env, cls, "<init>", "(JLandroid/content/Context;)V");
  if (ctor == NULL || (*env)->ExceptionCheck (env))
    {
      (*env)->ExceptionClear (env);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "Android network monitor: helper constructor not found.");
      goto out;
    }

  self->priv->java_stop_method = (*env)->GetMethodID (env, cls, "stop", "()V");
  if (self->priv->java_stop_method == NULL || (*env)->ExceptionCheck (env))
    {
      (*env)->ExceptionClear (env);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "Android network monitor: stop() method not found.");
      goto out;
    }

  local_obj = (*env)->NewObject (env, cls, ctor, (jlong) (gintptr) self, ctx);
  if (local_obj == NULL || (*env)->ExceptionCheck (env))
    {
      (*env)->ExceptionClear (env);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Android network monitor: failed to instantiate helper. "
                           "Check that ACCESS_NETWORK_STATE permission is granted.");
      goto out;
    }

  self->priv->java_monitor = (*env)->NewGlobalRef (env, local_obj);
  if (self->priv->java_monitor == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Android network monitor: NewGlobalRef failed.");
      goto out;
    }

  ok = TRUE;

out:
  if (local_obj != NULL)
    (*env)->DeleteLocalRef (env, local_obj);
  if (cls != NULL)
    (*env)->DeleteLocalRef (env, cls);
  if (ctx != NULL)
    (*env)->DeleteLocalRef (env, ctx);
  if (attached)
    detach_current_thread ();
  return ok;
}

static void
teardown_java_side (GAndroidNetworkMonitor *self)
{
  JNIEnv *env;
  gboolean attached = FALSE;

  if (self->priv->java_monitor == NULL)
    return;

  env = attach_current_thread (&attached);
  if (env != NULL)
    {
      if (self->priv->java_stop_method != NULL)
        {
          (*env)->CallVoidMethod (env, self->priv->java_monitor, self->priv->java_stop_method);
          if ((*env)->ExceptionCheck (env))
            (*env)->ExceptionClear (env);
        }
      (*env)->DeleteGlobalRef (env, self->priv->java_monitor);
      if (attached)
        detach_current_thread ();
    }
  self->priv->java_monitor = NULL;
  self->priv->java_stop_method = NULL;
}

static gboolean
g_android_network_monitor_initable_init (GInitable     *initable,
                                         GCancellable  *cancellable,
                                         GError       **error)
{
  GAndroidNetworkMonitor *self = G_ANDROID_NETWORK_MONITOR (initable);

  if (!self->priv->initialized)
    {
      self->priv->main_context = g_main_context_ref_thread_default ();

      /* Register before init_java_side so the very first Java callback can
       * find us. */
      register_monitor (self);

      if (!init_java_side (self, &self->priv->init_error))
        {
          unregister_monitor (self);
          g_clear_pointer (&self->priv->main_context, g_main_context_unref);
        }

      self->priv->initialized = TRUE;
    }

  if (self->priv->init_error != NULL)
    {
      g_propagate_error (error, g_error_copy (self->priv->init_error));
      return FALSE;
    }

  return initable_parent_iface->init (initable, cancellable, error);
}

static void
g_android_network_monitor_finalize (GObject *object)
{
  GAndroidNetworkMonitor *self = G_ANDROID_NETWORK_MONITOR (object);

  /* Unregister first so any in-flight callback bails out before we tear
   * down the Java side. */
  unregister_monitor (self);

  teardown_java_side (self);

  g_clear_error (&self->priv->init_error);
  g_clear_pointer (&self->priv->main_context, g_main_context_unref);

  G_OBJECT_CLASS (g_android_network_monitor_parent_class)->finalize (object);
}

static void
g_android_network_monitor_class_init (GAndroidNetworkMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_android_network_monitor_finalize;
  gobject_class->get_property = g_android_network_monitor_get_property;

  g_object_class_override_property (gobject_class, PROP_NETWORK_AVAILABLE, "network-available");
  g_object_class_override_property (gobject_class, PROP_NETWORK_METERED, "network-metered");
  g_object_class_override_property (gobject_class, PROP_CONNECTIVITY, "connectivity");
}

static void
g_android_network_monitor_iface_init (GNetworkMonitorInterface *monitor_iface)
{
  (void) monitor_iface;
}

static void
g_android_network_monitor_initable_iface_init (GInitableIface *iface)
{
  initable_parent_iface = g_type_interface_peek_parent (iface);

  iface->init = g_android_network_monitor_initable_init;
}
