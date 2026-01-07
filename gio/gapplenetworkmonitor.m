/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright 2026 Knut Saastad <knut.saastad@pexip.com>
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

#include "config.h"

#include <Network/Network.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "gapplenetworkmonitor.h"
#include "ginetaddress.h"
#include "ginetaddressmask.h"
#include "ginitable.h"
#include "gioerror.h"
#include "giomodule-priv.h"
#include "glib/gstdio.h"
#include "glibintl.h"
#include "gnetworkingprivate.h"
#include "gnetworkmonitor.h"
#include "gsocket.h"

static GInitableIface *initable_parent_iface;
static void g_apple_network_monitor_iface_init (GNetworkMonitorInterface *iface);
static void g_apple_network_monitor_initable_iface_init (GInitableIface *iface);

enum
{
  PROP_0,

  PROP_NETWORK_AVAILABLE,
  PROP_NETWORK_METERED,
  PROP_CONNECTIVITY
};

struct _GAppleNetworkMonitorPrivate
{
  gboolean initialized;
  GError *init_error;
  GMainContext *main_context;
  GSource *status_change_source;

  nw_path_monitor_t monitor;
  dispatch_queue_t queue;

  nw_path_status_t status;
  nw_interface_type_t interface_type;
  gboolean is_expensive;
  gboolean is_constrained;
  gboolean has_dns;
  gboolean has_ipv4;
  gboolean has_ipv4_gateway;
  gboolean has_ipv6;
  gboolean has_ipv6_gateway;
};

#define g_apple_network_monitor_get_type _g_apple_network_monitor_get_type
G_DEFINE_TYPE_WITH_CODE (
    GAppleNetworkMonitor,
    g_apple_network_monitor,
    G_TYPE_NETWORK_MONITOR_BASE,
    G_ADD_PRIVATE (GAppleNetworkMonitor)
        G_IMPLEMENT_INTERFACE (G_TYPE_NETWORK_MONITOR, g_apple_network_monitor_iface_init)
            G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, g_apple_network_monitor_initable_iface_init)
                _g_io_modules_ensure_extension_points_registered ();
    g_io_extension_point_implement (G_NETWORK_MONITOR_EXTENSION_POINT_NAME, g_define_type_id, "apple", 20))

static void
g_apple_network_monitor_init (GAppleNetworkMonitor *apple)
{
  apple->priv = g_apple_network_monitor_get_instance_private (apple);
}

static void
g_apple_network_monitor_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GAppleNetworkMonitor *apple = G_APPLE_NETWORK_MONITOR (object);

  switch (prop_id)
    {
    case PROP_NETWORK_AVAILABLE:
      g_value_set_boolean (value, (apple->priv->status == nw_path_status_satisfied));
      break;

    case PROP_NETWORK_METERED:
      g_value_set_boolean (value, apple->priv->is_expensive);
      break;

    case PROP_CONNECTIVITY:
      g_value_set_enum (value, apple->priv->status == nw_path_status_satisfied ? G_NETWORK_CONNECTIVITY_FULL
                                                                               : G_NETWORK_CONNECTIVITY_LOCAL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

typedef enum
{
  ROUTE_TYPE_INTERFACE,
  ROUTE_TYPE_GATEWAY,
} RouteType;

typedef struct
{
  RouteType type;
  int af;
  char if_name[32]; /* Only used for ROUTE_TYPE_INTERFACE */
  int if_flags;     /* Only used for ROUTE_TYPE_INTERFACE */
  struct sockaddr_storage addr;
  struct sockaddr_storage netmask;
  struct sockaddr_storage network;
  union
  {
    struct sockaddr_storage dstaddr;
    struct sockaddr_storage gwaddr;
  };
  gint prefix_len; /* netmask cidr notation */
} Route;

typedef struct
{
  GAppleNetworkMonitor *apple;
  nw_path_status_t status;
  nw_interface_type_t interface_type;
  gboolean is_expensive;
  gboolean is_constrained;
  gboolean has_dns;
  gboolean has_ipv4;
  gboolean has_ipv6;

  GList *ipv4_gateways; /*List of Route instances */
  GList *ipv6_gateways; /*List of Route instances */
  GList *local_routes;  /* List of Route instances */
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
      g_free (ptr);
    }
}

static void
network_status_parse_gateway (NetworkStatusData *status_data, nw_endpoint_t gateway_endpoint)
{
  struct sockaddr *sa_copy = NULL;
  GList **list_ptr = NULL;

  const struct sockaddr *sa = nw_endpoint_get_address (gateway_endpoint);
  if (sa == NULL)
    return;

  switch (sa->sa_family)
    {
    case AF_INET:
      list_ptr = &status_data->ipv4_gateways;
      break;
    case AF_INET6:
      list_ptr = &status_data->ipv6_gateways;
      break;
    default:
      break;
    }

  if (list_ptr == NULL)
    return;

  Route *route = route_new (ROUTE_TYPE_GATEWAY, sa->sa_family, NULL);
  memcpy (&route->gwaddr, sa, sa->sa_len);
  route->prefix_len = 0;

  *list_ptr = g_list_append (*list_ptr, route);
}

static gint
subnet_mask_to_prefix_len (const struct sockaddr *mask)
{
  if (!mask)
    return -1;

  if (mask->sa_family == AF_INET)
    {
      const struct sockaddr_in *sin = (const struct sockaddr_in *) mask;
      guint32 mask4 = ntohl (sin->sin_addr.s_addr);

      // Count ones from MSB
      guint prefix = 0;
      gboolean hole_found = FALSE;
      for (gint i = 31; i >= 0; --i)
        {
          if (mask4 & (1U << i))
            {
              if (hole_found)
                return -1; // Non-contiguous
              prefix++;
            }
          else
            {
              hole_found = TRUE;
            }
        }
      return prefix;
    }
  else if (mask->sa_family == AF_INET6)
    {
      const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) mask;
      guint prefix = 0;
      gboolean hole_found = FALSE;

      for (gint i = 0; i < 16; ++i)
        {
          guint8 octet = sin6->sin6_addr.s6_addr[i];
          for (gint b = 7; b >= 0; --b)
            {
              if (octet & (1U << b))
                {
                  if (hole_found)
                    return -1; // Non-contiguous
                  prefix++;
                }
              else
                {
                  hole_found = TRUE;
                }
            }
        }
      return prefix;
    }
  // Unsupported address family
  return -1;
}

static void
_print_route (Route *route, GString *msg)
{
  char buf_network[INET6_ADDRSTRLEN];
  char buf_gwaddr[INET6_ADDRSTRLEN] = "";
  if (route->af == AF_INET)
    {
      struct sockaddr_in *sin_network = (struct sockaddr_in *) &route->network;
      struct sockaddr_in *sin_gwaddr = (struct sockaddr_in *) &route->gwaddr;
      inet_ntop (AF_INET, &sin_network->sin_addr, buf_network, sizeof (buf_network));
      inet_ntop (AF_INET, &sin_gwaddr->sin_addr, buf_gwaddr, sizeof (buf_gwaddr));
    }
  else if (route->af == AF_INET6)
    {
      struct sockaddr_in6 *sin6_network = (struct sockaddr_in6 *) &route->network;
      struct sockaddr_in6 *sin6_gwaddr = (struct sockaddr_in6 *) &route->gwaddr;
      inet_ntop (AF_INET6, &sin6_network->sin6_addr, buf_network, sizeof (buf_network));
      inet_ntop (AF_INET6, &sin6_gwaddr->sin6_addr, buf_gwaddr, sizeof (buf_gwaddr));
    }

  if (route->type == ROUTE_TYPE_INTERFACE)
    if (route->if_flags & IFF_LOOPBACK)
      g_string_append_printf (msg, "%s/%d loopback interface %s\n", buf_network, route->prefix_len, route->if_name);
    else if (route->if_flags & IFF_POINTOPOINT)
      g_string_append_printf (msg, "%s/%d point-to-point interface %s\n", buf_network, route->prefix_len,
                              route->if_name);
    else
      g_string_append_printf (msg, "%s/%d interface %s\n", buf_network, route->prefix_len, route->if_name);
  else
    g_string_append_printf (msg, "%s/%d via gateway %s\n", buf_network, route->prefix_len, buf_gwaddr);
}

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
_network_update_set_base_routes (GAppleNetworkMonitor *apple, NetworkStatusData *status_data)
{
  GPtrArray *networks = NULL;
  gint local_routes_len = g_list_length (status_data->local_routes);
  gint route_count =
      (local_routes_len + (status_data->ipv4_gateways == NULL ? 0 : 1) + (status_data->ipv6_gateways == NULL ? 0 : 1));
  GList *iter;

  if (route_count == 0)
    {
      g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (apple), NULL, 0);
      return;
    }

  networks = g_ptr_array_new_full (route_count, g_object_unref);

  /* Add ipv4 default route if available */
  if (status_data->ipv4_gateways != NULL)
    {
      GInetAddressMask *network = get_network_mask (G_SOCKET_FAMILY_IPV4, NULL, 0);
      if (network != NULL)
        g_ptr_array_add (networks, network);
    }

  /* Add ipv6 default route if available */
  if (status_data->ipv6_gateways != NULL)
    {
      GInetAddressMask *network = get_network_mask (G_SOCKET_FAMILY_IPV6, NULL, 0);
      if (network != NULL)
        g_ptr_array_add (networks, network);
    }

  /* Add routes extracted from the local interfaces first */
  for (iter = status_data->local_routes; iter != NULL; iter = iter->next)
    {
      Route *local_route;
      GInetAddressMask *network;
      GSocketFamily family = G_SOCKET_FAMILY_INVALID;
      const guint8 *dest = NULL;
      gsize len;

      local_route = iter->data;
      len = local_route->prefix_len;

      if (local_route->af == AF_INET)
        {
          family = G_SOCKET_FAMILY_IPV4;
          dest = (const guint8 *) &local_route->addr;
        }
      else if (local_route->af == AF_INET6)
        {
          family = G_SOCKET_FAMILY_IPV6;
          dest = (const guint8 *) &local_route->addr;
        }

      network = get_network_mask (family, dest, len);
      if (network == NULL)
        continue;

      g_ptr_array_add (networks, network);
    }

  g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (apple), networks->pdata, networks->len);
  g_ptr_array_free (networks, TRUE);
}

static void
network_status_parse_interface_routes (NetworkStatusData *status_data)
{
  struct ifaddrs *ifaddr, *ifa;
  getifaddrs (&ifaddr);

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
      Route *route;
      gint prefix_len;

      if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_addr) || !(ifa->ifa_netmask))
        continue;

      if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
        continue;

      prefix_len = subnet_mask_to_prefix_len (ifa->ifa_netmask);
      if (prefix_len == -1)
        continue;

      route = route_new (ROUTE_TYPE_INTERFACE, ifa->ifa_addr->sa_family, ifa->ifa_name);
      route->if_flags = ifa->ifa_flags;
      route->prefix_len = prefix_len;
      memcpy (&route->addr, ifa->ifa_addr, ifa->ifa_addr->sa_len);
      memcpy (&route->netmask, ifa->ifa_netmask, ifa->ifa_netmask->sa_len);

      memcpy (&route->network, ifa->ifa_addr,
              ifa->ifa_addr->sa_len); /*Yes, this _is_ correct. The real value will be calculated below. */
      if (route->af == AF_INET)
        {
          struct sockaddr_in *addr_in = (struct sockaddr_in *) &route->addr;
          struct sockaddr_in *mask_in = (struct sockaddr_in *) &route->netmask;
          struct sockaddr_in *net_in = (struct sockaddr_in *) &route->network;
          net_in->sin_addr.s_addr = addr_in->sin_addr.s_addr & mask_in->sin_addr.s_addr;
        }
      else
        {
          struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *) &route->addr;
          struct sockaddr_in6 *mask_in6 = (struct sockaddr_in6 *) &route->netmask;
          struct sockaddr_in6 *net_in6 = (struct sockaddr_in6 *) &route->network;
          for (int i = 0; i < 16; ++i)
            {
              net_in6->sin6_addr.s6_addr[i] = addr_in6->sin6_addr.s6_addr[i] & mask_in6->sin6_addr.s6_addr[i];
            }
        }

      if (ifa->ifa_dstaddr)
        {
          memcpy (&route->dstaddr, ifa->ifa_dstaddr, ifa->ifa_dstaddr->sa_len);
        }
      status_data->local_routes = g_list_append (status_data->local_routes, route);
    }

  freeifaddrs (ifaddr);
}

static nw_interface_type_t
_nw_path_get_interface_type (nw_path_t path)
{
  /* Only call when nw_path_status_t == nw_path_status_satisfied.
     We should never see nw_interface_type_loopback or nw_interface_type_other here,
     but we parse all of them regardless, and default to nw_interface_type_other.
  */

  if (nw_path_uses_interface_type (path, nw_interface_type_wifi))
    {
      return nw_interface_type_wifi;
    }
  else if (nw_path_uses_interface_type (path, nw_interface_type_cellular))
    {
      return nw_interface_type_cellular;
    }
  else if (nw_path_uses_interface_type (path, nw_interface_type_wired))
    {
      return nw_interface_type_wired;
    }
  else if (nw_path_uses_interface_type (path, nw_interface_type_loopback))
    {
      return nw_interface_type_loopback;
    }
  else if (nw_path_uses_interface_type (path, nw_interface_type_other))
    {
      return nw_interface_type_other;
    }
  return nw_interface_type_other;
}

static void
_network_update_log_update (NetworkStatusData *status_data, gboolean log_routing_table)
{
  if (status_data == NULL)
    return;

  GString *msg = NULL;
  switch (status_data->apple->priv->status)
    {
    case nw_path_status_invalid:
      msg = g_string_new ("Network path is DOWN (invalid).");
      break;
    case nw_path_status_unsatisfied:
      msg = g_string_new ("Network path is DOWN (unsatisfied).");
      break;
    case nw_path_status_satisfied:
      msg = g_string_new ("Network path is UP.");
      if (status_data->apple->priv->interface_type == nw_interface_type_wifi)
        msg = g_string_append (msg, " Connected via Wi-Fi.");
      if (status_data->apple->priv->interface_type == nw_interface_type_cellular)
        msg = g_string_append (msg, " Connected via Cellular.");
      if (status_data->apple->priv->interface_type == nw_interface_type_wired)
        msg = g_string_append (msg, " Connected via Ethernet.");

      if (status_data->apple->priv->is_expensive)
        msg = g_string_append (msg, " Path is expensive (likely cellular).");
      if (status_data->apple->priv->is_constrained)
        msg = g_string_append (msg, " Path is constrained (low data mode).");
      if (status_data->apple->priv->has_dns)
        msg = g_string_append (msg, " Path has DNS.");
      if (status_data->apple->priv->has_ipv4)
        {
          if (status_data->apple->priv->has_ipv4_gateway)
            msg = g_string_append (msg, " Path has IPv4 internet connection.");
          else
            msg = g_string_append (msg, " Path has limited IPv4 capabilities.");
        }
      if (status_data->apple->priv->has_ipv6)
        {
          if (status_data->apple->priv->has_ipv6_gateway)
            msg = g_string_append (msg, " Path has IPv6 internet connection.");
          else
            msg = g_string_append (msg, " Path has limited IPv6 capabilities.");
        }
      break;
    case nw_path_status_satisfiable:
      msg = g_string_new ("Network path is DOWN (satisfiable).");
      break;
    }

  /* Print routing table */
  if (status_data->apple->priv->status == nw_path_status_satisfied && log_routing_table)
    {
      int i;
      GList *lists[3] = { status_data->ipv4_gateways, status_data->ipv6_gateways, status_data->local_routes };
      msg = g_string_append (msg, "\nRouting table:\n");
      for (i = 0; i < 3; i++)
        {
          GList *iter;
          for (iter = lists[i]; iter != NULL; iter = iter->next)
            {
              _print_route (iter->data, msg);
            }
        }
    }

  if (msg)
    {
      g_debug ("%s", msg->str);
      g_string_free (msg, TRUE);
    }
}

static gboolean
_network_update_invoke_route_changed (gpointer user_data)
{
  GString *msg = NULL;
  NetworkStatusData *status_data = user_data;

  status_data->apple->priv->status = status_data->status;
  status_data->apple->priv->interface_type = status_data->interface_type;
  status_data->apple->priv->is_expensive = status_data->is_expensive;
  status_data->apple->priv->is_constrained = status_data->is_constrained;
  status_data->apple->priv->has_dns = status_data->has_dns;
  status_data->apple->priv->has_ipv4 = status_data->has_ipv4;
  status_data->apple->priv->has_ipv6 = status_data->has_ipv6;
  status_data->apple->priv->has_ipv4_gateway = (status_data->ipv4_gateways != NULL);
  status_data->apple->priv->has_ipv6_gateway = (status_data->ipv6_gateways != NULL);

  /* Populate base for route resolving */
  _network_update_set_base_routes (status_data->apple, status_data);

  /* Log the change */
  _network_update_log_update (status_data, TRUE);

  return G_SOURCE_REMOVE;
}

static void
_network_update_handler (nw_path_t path, GAppleNetworkMonitor *apple)
{
  NetworkStatusData *status_data = g_new0 (NetworkStatusData, 1);
  status_data->apple = apple;

  status_data->status = nw_path_get_status (path);
  if (status_data->status == nw_path_status_satisfied)
    {
      status_data->interface_type = _nw_path_get_interface_type (path);
      status_data->is_expensive = nw_path_is_expensive (path);
      status_data->is_constrained = nw_path_is_constrained (path);
      status_data->has_dns = nw_path_has_dns (path);
      status_data->has_ipv4 = nw_path_has_ipv4 (path);
      status_data->has_ipv6 = nw_path_has_ipv6 (path);

      nw_path_enumerate_gateways (path, ^bool (nw_endpoint_t gateway_endpoint) {
        nw_endpoint_type_t type = nw_endpoint_get_type (gateway_endpoint);
        if (type == nw_endpoint_type_address)
          {
            network_status_parse_gateway (status_data, gateway_endpoint);
          }
        return true;
      });

      network_status_parse_interface_routes (status_data);
    }

  apple->priv->status_change_source = g_idle_source_new ();
  g_source_set_priority (apple->priv->status_change_source, G_PRIORITY_DEFAULT);
  g_source_set_callback (apple->priv->status_change_source, _network_update_invoke_route_changed, status_data,
                         (GDestroyNotify) network_status_data_free);

  g_source_attach (apple->priv->status_change_source, apple->priv->main_context);
}

static gboolean
g_apple_network_monitor_initable_init (GInitable *initable, GCancellable *cancellable, GError **error)
{
  GAppleNetworkMonitor *apple = G_APPLE_NETWORK_MONITOR (initable);
  g_assert (apple);

  if (!apple->priv->initialized)
    {
      apple->priv->main_context = g_main_context_ref_thread_default ();
      apple->priv->queue = dispatch_queue_create ("com.pexip.networkmonitor", DISPATCH_QUEUE_SERIAL);
      apple->priv->monitor = nw_path_monitor_create ();
      if (!apple->priv->monitor)
        {
          g_warning ("Monitor creation failed.");
          return FALSE;
        }

      nw_path_monitor_prohibit_interface_type (apple->priv->monitor, nw_interface_type_loopback);
      nw_path_monitor_prohibit_interface_type (apple->priv->monitor, nw_interface_type_other);
      nw_path_monitor_set_queue (apple->priv->monitor, apple->priv->queue);
      nw_path_monitor_set_update_handler (apple->priv->monitor, ^(nw_path_t path) {
        _network_update_handler (path, apple);
      });

      nw_path_monitor_start (apple->priv->monitor);
      apple->priv->initialized = TRUE;
    }

  /* Forward the results. */
  if (apple->priv->init_error != NULL)
    {
      g_propagate_error (error, g_error_copy (apple->priv->init_error));
      return FALSE;
    }

  return initable_parent_iface->init (initable, cancellable, error);
}

static void
g_apple_network_monitor_finalize (GObject *object)
{
  GAppleNetworkMonitor *apple = G_APPLE_NETWORK_MONITOR (object);

  g_clear_error (&apple->priv->init_error);

  if (apple->priv->monitor)
    {
      nw_path_monitor_cancel (apple->priv->monitor);
      nw_release (apple->priv->monitor);
      apple->priv->monitor = NULL;
      dispatch_release (apple->priv->queue);
      apple->priv->queue = NULL;
    }

  if (apple->priv->status_change_source != NULL)
    {
      g_source_destroy (apple->priv->status_change_source);
      g_source_unref (apple->priv->status_change_source);
    }

  g_main_context_unref (apple->priv->main_context);

  G_OBJECT_CLASS (g_apple_network_monitor_parent_class)->finalize (object);
}

static void
g_apple_network_monitor_class_init (GAppleNetworkMonitorClass *apple_class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (apple_class);

  gobject_class->finalize = g_apple_network_monitor_finalize;
  gobject_class->get_property = g_apple_network_monitor_get_property;

  g_object_class_override_property (gobject_class, PROP_NETWORK_AVAILABLE, "network-available");
  g_object_class_override_property (gobject_class, PROP_NETWORK_METERED, "network-metered");
  //  g_object_class_override_property (gobject_class, PROP_CONNECTIVITY, "connectivity");
}

static void
g_apple_network_monitor_iface_init (GNetworkMonitorInterface *monitor_iface)
{
}

static void
g_apple_network_monitor_initable_iface_init (GInitableIface *iface)
{
  initable_parent_iface = g_type_interface_peek_parent (iface);

  iface->init = g_apple_network_monitor_initable_init;
}
