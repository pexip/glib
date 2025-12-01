/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright 2014-2018 Jan-Michael Brummer <jan.brummer@tabos.org>
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

/* The logic here used to be based on NotifyRouteChange2 callbacks, which really didn't do too good a job. Problem with that approach
   is that the routing table returned by GetIpForwardTable2 does not reflect interface or connection state - if you unplug the network
   cable or disconnect from a wifi, the old default routes does not get removed, it only changes when we for some reason receive a new
   default route (eg jumping between WIFI networks). Because of this (somewhat strange behavior in the windows API), NotifyRouteChange2
   does not fire when network connectivity is lost.

   The new approach here, is using a combination of NotifyRouteChange2() and NotifyNetworkConnectivityHintChange(), and base our updates of the
   g_network_monitor_base_set_networks route mapping on the connection state hints:

   * State: NetworkConnectivityLevelHintUnknown:
     Desc: Specifies a hint for an unknown level of connectivity. There is a short window of time during Windows (or application container) boot when this value might be returned.
     HintCbAction: Clear the gnetworkmonitorbase routetable.
     RouteCbAction: Ignore.
   * State: NetworkConnectivityLevelHintNone:
     Desc: Specifies a hint for no connectivity.
     HintCbAction: Clear the gnetworkmonitorbase routetable.
     RouteCbAction: Ignore.
   * State: NetworkConnectivityLevelHintLocalAccess:
     Desc: Specifies a hint for local network access only.
     HintCbAction: Populate route table, but filter out any default routes.
     RouteCbAction: Update local routes only, ignore default routes.
   * State: NetworkConnectivityLevelHintInternetAccess:
     Desc: Specifies a hint for local and internet access.
     HintCbAction: Populate full route table.
     RouteCbAction: Update all routes.
   * State: NetworkConnectivityLevelHintConstrainedInternetAccess:
     Desc: Specifies a hint for limited internet access. This value indicates captive portal connectivity, where local access to a web portal is provided, but access to the internet requires that specific credentials are provided via the portal. This level of connectivity is generally encountered when using connections hosted in public locations (for example, coffee shops and book stores). This doesn't guarantee detection of a captive portal. You should be aware that when Windows reports the connectivity level hint as NetworkConnectivityLevelHintLocalAccess, your application's network requests might be redirected, and thus receive a different response than expected. Other protocols might also be impacted; for example, HTTPS might be redirected, and fail authentication.
     HintCbAction: Populate full route table.
     RouteCbAction: Update all routes.
   * State: NetworkConnectivityLevelHintHidden:
     Desc: Specifies a hint for a network interface that's hidden from normal connectivity (and is not, by default, accessible to applications). This could be because no packets are allowed at all over that network (for example, the adapter flags itself NCF_HIDDEN), or (by default) routes are ignored on that interface (for example, a cellular network is hidden when WiFi is connected).
     HintCbAction: Clear the gnetworkmonitorbase routetable.
     RouteCbAction: Ignore.
*/

#include "config.h"

#include <errno.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>

#include "gwin32networkmonitor.h"
#include "ginetaddress.h"
#include "ginetaddressmask.h"
#include "ginitable.h"
#include "giomodule-priv.h"
#include "glibintl.h"
#include "glib/gstdio.h"
#include "gnetworkingprivate.h"
#include "gsocket.h"
#include "gnetworkmonitor.h"
#include "gioerror.h"

static GInitableIface *initable_parent_iface;
static void g_win32_network_monitor_iface_init (GNetworkMonitorInterface *iface);
static void g_win32_network_monitor_initable_iface_init (GInitableIface *iface);

enum
{
  PROP_0,

  PROP_NETWORK_AVAILABLE,
  PROP_NETWORK_METERED,
  PROP_CONNECTIVITY
};

struct _GWin32NetworkMonitorPrivate
{
  gboolean initialized;
  GError *init_error;
  GMainContext *main_context;
  GSource *route_change_source;
  GSource *connectivity_hint_source;

  NL_NETWORK_CONNECTIVITY_HINT hint;

  HANDLE route_change_handle;
  HANDLE conn_hint_handle;
};

#define g_win32_network_monitor_get_type _g_win32_network_monitor_get_type
G_DEFINE_TYPE_WITH_CODE (GWin32NetworkMonitor, g_win32_network_monitor, G_TYPE_NETWORK_MONITOR_BASE,
                         G_ADD_PRIVATE (GWin32NetworkMonitor)
                         G_IMPLEMENT_INTERFACE (G_TYPE_NETWORK_MONITOR,
                                                g_win32_network_monitor_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                g_win32_network_monitor_initable_iface_init)
                         _g_io_modules_ensure_extension_points_registered ();
                         g_io_extension_point_implement (G_NETWORK_MONITOR_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         "win32",
                                                         20))

static void
g_win32_network_monitor_init (GWin32NetworkMonitor *win)
{
  win->priv = g_win32_network_monitor_get_instance_private (win);
}

static gboolean
gwin32_network_monitor_is_available (NL_NETWORK_CONNECTIVITY_HINT hint)
{
  /**
   * Checks if the network is available. "Available" here means that the
   * system has a default route available for at least one of IPv4 or
   * IPv6. It does not necessarily imply that the public Internet is
   * reachable. 
   * See #GNetworkMonitor:network-available for more details.
   */

  switch (hint.ConnectivityLevel)
    {
      case NetworkConnectivityLevelHintInternetAccess:
      case NetworkConnectivityLevelHintConstrainedInternetAccess:
        return TRUE;
      default:
        break;
    } 
  return FALSE;
}

static gboolean
gwin32_network_monitor_is_metered (NL_NETWORK_CONNECTIVITY_HINT hint)
{
  /**
   * Checks if the network is metered.
   * See #GNetworkMonitor:network-metered for more details.
   */

  switch (hint.ConnectivityCost)
    {
      case NetworkConnectivityCostHintFixed:
      case NetworkConnectivityCostHintVariable:
        return TRUE;
      default:
        break;
    } 
  return FALSE;
}

static GNetworkConnectivity
gwin32_network_monitor_get_connectivity (NL_NETWORK_CONNECTIVITY_HINT hint)
{
  /**
   * Checks if the network is available. "Available" here means that the
   * system has a default route available for at least one of IPv4 or
   * IPv6. It does not necessarily imply that the public Internet is
   * reachable. 
   * See #GNetworkMonitor:network-available for more details.
   */

  switch (hint.ConnectivityLevel)
    {
      case NetworkConnectivityLevelHintInternetAccess:
        return G_NETWORK_CONNECTIVITY_FULL;
      case NetworkConnectivityLevelHintConstrainedInternetAccess:
        return G_NETWORK_CONNECTIVITY_PORTAL;
      default:
        break;
    } 
  return G_NETWORK_CONNECTIVITY_LOCAL;
}

static void
g_win32_network_monitor_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  GWin32NetworkMonitor *win = G_WIN32_NETWORK_MONITOR (object);

  switch (prop_id)
    {
    case PROP_NETWORK_AVAILABLE:
      g_value_set_boolean (value, gwin32_network_monitor_is_available (win->priv->hint));
      break;

    case PROP_NETWORK_METERED:
      g_value_set_boolean (value, gwin32_network_monitor_is_metered (win->priv->hint));
      break;

    case PROP_CONNECTIVITY:
      g_value_set_enum (value, gwin32_network_monitor_get_connectivity (win->priv->hint));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static const gchar * g_win32_network_monitor_connectivity_level_to_string (NL_NETWORK_CONNECTIVITY_LEVEL_HINT level)
{
  switch (level)
    {
      case NetworkConnectivityLevelHintUnknown:
        return "Unknown";
      case NetworkConnectivityLevelHintNone:
        return "None";
      case NetworkConnectivityLevelHintLocalAccess:
        return "LocalAccess";
      case NetworkConnectivityLevelHintInternetAccess:
        return "InternetAccess";
      case NetworkConnectivityLevelHintConstrainedInternetAccess:
        return "ConstrainedInternetAccess";
      case NetworkConnectivityLevelHintHidden:
        return "Hidden";
      default:
        break;
    }
  return "Unknown";
}

static gboolean
win_network_monitor_get_ip_info (IP_ADDRESS_PREFIX  prefix,
                                 GSocketFamily     *family,
                                 const guint8     **dest,
                                 gsize             *len,
                                 gboolean          *is_default_route)
{
  switch (prefix.Prefix.si_family)
    {
      case AF_UNSPEC:
        /* Fall-through: AF_UNSPEC deliveres both IPV4 and IPV6 infos, let`s stick with IPV4 here */
      case AF_INET:
        *family = G_SOCKET_FAMILY_IPV4;
        *dest = (guint8 *) &prefix.Prefix.Ipv4.sin_addr;
        *len = prefix.PrefixLength;
        break;
      case AF_INET6:
        *family = G_SOCKET_FAMILY_IPV6;
        *dest = (guint8 *) &prefix.Prefix.Ipv6.sin6_addr;
        *len = prefix.PrefixLength;
        break;
      default:
        return FALSE;
    }

  g_assert (*dest);
  g_assert (*family);
  GInetAddress *dest_addr = g_inet_address_new_from_bytes (*dest, *family);
  *is_default_route = prefix.PrefixLength == 0 && g_inet_address_get_is_any(dest_addr);
  g_object_unref (dest_addr);

  return TRUE;
}

static GInetAddressMask *
get_network_mask (GSocketFamily  family,
                  const guint8  *dest,
                  gsize          len)
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

static gboolean
win_network_monitor_process_table (GWin32NetworkMonitor  *win,
                                   gboolean local_routes_only,
                                   GError                 **error)
{
  DWORD ret = 0;
  GPtrArray *networks;
  gsize i;
  MIB_IPFORWARD_TABLE2 *routes = NULL;
  MIB_IPFORWARD_ROW2 *route;

  ret = GetIpForwardTable2 (AF_UNSPEC, &routes);
  if (ret != ERROR_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "GetIpForwardTable2 () failed: %ld", ret);

      return FALSE;
    }

  networks = g_ptr_array_new_full (routes->NumEntries, g_object_unref);
  for (i = 0; i < routes->NumEntries; i++)
    {
      GInetAddressMask *network;
      const guint8 *dest;
      gsize len;
      GSocketFamily family;
      gboolean is_default_route;

      route = routes->Table + i;

      if (!win_network_monitor_get_ip_info (route->DestinationPrefix, &family, &dest, &len, &is_default_route))
        continue;

      if (local_routes_only && is_default_route)
        continue;

      network = get_network_mask (family, dest, len);
      if (network == NULL)
        continue;

      g_ptr_array_add (networks, network);
    }

  g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (win),
                                       (GInetAddressMask **) networks->pdata,
                                       networks->len);

  return TRUE;
}

static void win_network_monitor_init_clear_networks_table (GWin32NetworkMonitor  *win)
{
  g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (win),
                                       (GInetAddressMask **) NULL,
                                       0);
}

static gboolean win_network_monitor_check_forward_table_accessible (GWin32NetworkMonitor  *win, GError **error)
{
  DWORD ret = 0;
  MIB_IPFORWARD_TABLE2 *routes = NULL;

  ret = GetIpForwardTable2 (AF_UNSPEC, &routes);
  if (ret != ERROR_SUCCESS)
  {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "GetIpForwardTable2 () failed: %ld", ret);

    return FALSE;
  }

  if (routes != NULL)
    FreeMibTable(routes);

  return TRUE;
}

static void
add_network (GWin32NetworkMonitor *win,
             GSocketFamily         family,
             const guint8         *dest,
             gsize                 dest_len)
{
  GInetAddressMask *network;

  network = get_network_mask (family, dest, dest_len);
  if (network != NULL)
    {
      g_network_monitor_base_add_network (G_NETWORK_MONITOR_BASE (win), network);
      g_object_unref (network);
    }
}

static void
remove_network (GWin32NetworkMonitor *win,
                GSocketFamily         family,
                const guint8         *dest,
                gsize                 dest_len)
{
  GInetAddressMask *network;

  network = get_network_mask (family, dest, dest_len);
  if (network != NULL)
    {
      g_network_monitor_base_remove_network (G_NETWORK_MONITOR_BASE (win), network);
      g_object_unref (network);
    }
}

typedef struct {
  PMIB_IPFORWARD_ROW2 route;
  MIB_NOTIFICATION_TYPE type;
  GWin32NetworkMonitor *win;
} RouteData;

typedef struct {
  NL_NETWORK_CONNECTIVITY_HINT hint;
  GWin32NetworkMonitor *win;
} ConnectivityHintData;

static gboolean
win_network_monitor_invoke_route_changed (gpointer user_data)
{
  GSocketFamily family;
  RouteData *route_data = user_data;
  const guint8 *dest;
  gsize len;
  gboolean is_default_route;

  gboolean process_route = TRUE;
  gboolean local_routes_only = FALSE;

  switch (route_data->win->priv->hint.ConnectivityLevel)
    {
      case NetworkConnectivityLevelHintUnknown:
        process_route = FALSE;
        break;
      case NetworkConnectivityLevelHintNone:
        process_route = FALSE;
        break;
      case NetworkConnectivityLevelHintLocalAccess:
        local_routes_only = TRUE;
        break;
      case NetworkConnectivityLevelHintInternetAccess:
        break;
      case NetworkConnectivityLevelHintConstrainedInternetAccess:
        break;
      case NetworkConnectivityLevelHintHidden:
        process_route = FALSE;
        break;
    }

  if (process_route)
    {
      switch (route_data->type)
        {
          case MibDeleteInstance:
            if (!win_network_monitor_get_ip_info (route_data->route->DestinationPrefix, &family, &dest, &len, &is_default_route))
              break;
            if (local_routes_only && is_default_route){
              /* Skip processing default route changes when local_routes_only. The route table will we recreated completely during ConnectivityLevel change */
              break;
            }
            remove_network (route_data->win, family, dest, len);
            break;
          case MibAddInstance:
            if (!win_network_monitor_get_ip_info (route_data->route->DestinationPrefix, &family, &dest, &len, &is_default_route))
                break;
            if (local_routes_only && is_default_route){
              /* Skip processing default route changes when local_routes_only. The route table will we recreated completely during ConnectivityLevel change */
              break;
            }
            add_network (route_data->win, family, dest, len);
            break;
          case MibInitialNotification:
          default:
            break;
        }
    }
 
  return G_SOURCE_REMOVE;
}

static VOID WINAPI
win_network_monitor_route_changed_cb (PVOID                 context,
                                      PMIB_IPFORWARD_ROW2   route,
                                      MIB_NOTIFICATION_TYPE type)
{
  GWin32NetworkMonitor *win = context;
  RouteData *route_data;

  route_data = g_new0 (RouteData, 1);
  route_data->route = route;
  route_data->type = type;
  route_data->win = win;

  win->priv->route_change_source = g_idle_source_new ();
  g_source_set_priority (win->priv->route_change_source, G_PRIORITY_DEFAULT);
  g_source_set_callback (win->priv->route_change_source,
                         win_network_monitor_invoke_route_changed,
                         route_data,
                         g_free);

  g_source_attach (win->priv->route_change_source, win->priv->main_context);
}

static gboolean
win_network_monitor_invoke_connectivity_hint (gpointer user_data)
{
  GSocketFamily family;
  ConnectivityHintData * connectivity_hint_data = user_data;

  gboolean send_connectivity_notification = FALSE;
  gboolean send_network_metered_notification = (connectivity_hint_data->win->priv->hint.ConnectivityCost != connectivity_hint_data->hint.ConnectivityCost);

  if (connectivity_hint_data->win->priv->hint.ConnectivityLevel != connectivity_hint_data->hint.ConnectivityLevel)
    {
      g_debug ("Connectivity state change %s -> %s", 
        g_win32_network_monitor_connectivity_level_to_string(connectivity_hint_data->win->priv->hint.ConnectivityLevel), 
        g_win32_network_monitor_connectivity_level_to_string(connectivity_hint_data->hint.ConnectivityLevel));
      send_connectivity_notification = TRUE;
      GError * error = NULL;
      switch (connectivity_hint_data->hint.ConnectivityLevel)
        {
          case NetworkConnectivityLevelHintUnknown:
            win_network_monitor_init_clear_networks_table (connectivity_hint_data->win);
            break;
          case NetworkConnectivityLevelHintNone:
            win_network_monitor_init_clear_networks_table (connectivity_hint_data->win);
            break;
          case NetworkConnectivityLevelHintLocalAccess:
            win_network_monitor_process_table (connectivity_hint_data->win, TRUE, &error);
            break;
          case NetworkConnectivityLevelHintInternetAccess:
            win_network_monitor_process_table (connectivity_hint_data->win, FALSE, &error);
            break;
          case NetworkConnectivityLevelHintConstrainedInternetAccess:
            win_network_monitor_process_table (connectivity_hint_data->win, FALSE, &error);
            break;
          case NetworkConnectivityLevelHintHidden:
            win_network_monitor_init_clear_networks_table (connectivity_hint_data->win);
            break;
        }
      if (error != NULL) {
        g_warning ("Failed to update routing table! %s", error->message);
        g_clear_error (&error);
      }
    }

  connectivity_hint_data->win->priv->hint = connectivity_hint_data->hint;

  /* network_available notification is handled in the gnetworkmonitorbase */
  if (send_network_metered_notification)
    g_object_notify (G_OBJECT (connectivity_hint_data->win), "network-metered");
  if (send_connectivity_notification)
    g_object_notify (G_OBJECT (connectivity_hint_data->win), "connectivity");

  return G_SOURCE_REMOVE;
}

static VOID WINAPI
win_network_monitor_connectivity_hint_cb (PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint)
{
  GWin32NetworkMonitor *win = context;
  ConnectivityHintData *connectivity_hint_data;

  connectivity_hint_data = g_new0 (ConnectivityHintData, 1);
  connectivity_hint_data->hint = hint;
  connectivity_hint_data->win = win;

  win->priv->connectivity_hint_source = g_idle_source_new ();
  g_source_set_priority (win->priv->connectivity_hint_source, G_PRIORITY_DEFAULT);
  g_source_set_callback (win->priv->connectivity_hint_source,
                         win_network_monitor_invoke_connectivity_hint,
                         connectivity_hint_data,
                         g_free);

  g_source_attach (win->priv->connectivity_hint_source, win->priv->main_context);
}

static gboolean
g_win32_network_monitor_initable_init (GInitable     *initable,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  GWin32NetworkMonitor *win = G_WIN32_NETWORK_MONITOR (initable);
  NTSTATUS status;
  gboolean read;

  if (!win->priv->initialized)
    {
      win->priv->main_context = g_main_context_ref_thread_default ();

      win_network_monitor_init_clear_networks_table (win);
      if (win_network_monitor_check_forward_table_accessible (win, &win->priv->init_error))
        {
          /* Register for IPv4 and IPv6 route updates. */
          status = NotifyRouteChange2 (AF_UNSPEC, (PIPFORWARD_CHANGE_CALLBACK) win_network_monitor_route_changed_cb, win, FALSE, &win->priv->route_change_handle);
          if (status != NO_ERROR){
            g_set_error (&win->priv->init_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "NotifyRouteChange2() error: %ld", status);
          }

          /* Register for IPv4 and IPv6 connectivity hints. */
          status = NotifyNetworkConnectivityHintChange ((PNETWORK_CONNECTIVITY_HINT_CHANGE_CALLBACK) win_network_monitor_connectivity_hint_cb, win, TRUE, &win->priv->conn_hint_handle);
          if (status != NO_ERROR){
            g_set_error (&win->priv->init_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "NotifyNetworkConnectivityHintChange() error: %ld", status);
          }
        }
      win->priv->initialized = TRUE;
    }

  /* Forward the results. */
  if (win->priv->init_error != NULL)
    {
      g_propagate_error (error, g_error_copy (win->priv->init_error));
      return FALSE;
    }

  return initable_parent_iface->init (initable, cancellable, error);
}

static void
g_win32_network_monitor_finalize (GObject *object)
{
  GWin32NetworkMonitor *win = G_WIN32_NETWORK_MONITOR (object);

  /* Cancel notification event */
  if (win->priv->route_change_handle)
    CancelMibChangeNotify2 (win->priv->route_change_handle);
  if (win->priv->conn_hint_handle)
    CancelMibChangeNotify2 (win->priv->conn_hint_handle);

  g_clear_error (&win->priv->init_error);

  if (win->priv->route_change_source != NULL)
    {
      g_source_destroy (win->priv->route_change_source);
      g_source_unref (win->priv->route_change_source);
    }
  if (win->priv->connectivity_hint_source != NULL)
    {
      g_source_destroy (win->priv->connectivity_hint_source);
      g_source_unref (win->priv->connectivity_hint_source);
    }

  g_main_context_unref (win->priv->main_context);

  G_OBJECT_CLASS (g_win32_network_monitor_parent_class)->finalize (object);
}

static void
g_win32_network_monitor_class_init (GWin32NetworkMonitorClass *win_class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (win_class);

  gobject_class->finalize = g_win32_network_monitor_finalize;
  gobject_class->get_property = g_win32_network_monitor_get_property;

  g_object_class_override_property (gobject_class, PROP_NETWORK_AVAILABLE, "network-available");
  g_object_class_override_property (gobject_class, PROP_NETWORK_METERED, "network-metered");
  g_object_class_override_property (gobject_class, PROP_CONNECTIVITY, "connectivity");
}

static void
g_win32_network_monitor_iface_init (GNetworkMonitorInterface *monitor_iface)
{
}

static void
g_win32_network_monitor_initable_iface_init (GInitableIface *iface)
{
  initable_parent_iface = g_type_interface_peek_parent (iface);

  iface->init = g_win32_network_monitor_initable_init;
}
