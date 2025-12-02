/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright 2024 Knut Saastad <knut.saastad@pexip.com>
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
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <errno.h>

#include "gapplenetworkmonitor.h"
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
G_DEFINE_TYPE_WITH_CODE (GAppleNetworkMonitor, g_apple_network_monitor, G_TYPE_NETWORK_MONITOR_BASE,
                         G_ADD_PRIVATE (GAppleNetworkMonitor)
                         G_IMPLEMENT_INTERFACE (G_TYPE_NETWORK_MONITOR,
                                                g_apple_network_monitor_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                g_apple_network_monitor_initable_iface_init)
                         _g_io_modules_ensure_extension_points_registered ();
                         g_io_extension_point_implement (G_NETWORK_MONITOR_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         "apple",
                                                         20))

static void
g_apple_network_monitor_init (GAppleNetworkMonitor *apple)
{
  apple->priv = g_apple_network_monitor_get_instance_private (apple);
}

static void
g_apple_network_monitor_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
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
      g_value_set_enum (value, apple->priv->status == nw_path_status_satisfied ? G_NETWORK_CONNECTIVITY_FULL :
                        G_NETWORK_CONNECTIVITY_LOCAL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

typedef struct {
  char if_name[32];
  int af;
  struct sockaddr_storage addr;
  struct sockaddr_storage netmask;
  struct sockaddr_storage dstaddr;
  gint prefix_len;
} LocalRoute;

typedef struct {
  GAppleNetworkMonitor * apple;
  nw_path_status_t status;
  nw_interface_type_t interface_type;
  gboolean is_expensive;
  gboolean is_constrained;
  gboolean has_dns;
  gboolean has_ipv4;
  gboolean has_ipv6;

  GList * ipv4_gateways; /*List of struct sockaddr instances */
  GList * ipv6_gateways; /*List of struct sockaddr instances */
  GList * local_routes; /* List of LocalRoute instances */
} NetworkStatusData;

static void network_status_data_free (NetworkStatusData * ptr)
  {
    if (ptr)
      {
        g_list_free_full (ptr->ipv4_gateways, g_free);        
        g_list_free_full (ptr->ipv6_gateways, g_free);        
        g_list_free_full (ptr->local_routes, g_free);        
        g_free (ptr);
      }    
  }

static void network_status_parse_gateway (NetworkStatusData * status_data, nw_endpoint_t gateway_endpoint)
{
  struct sockaddr * sa_copy = NULL;
  GList ** list_ptr = NULL;

  const struct sockaddr *sa = nw_endpoint_get_address(gateway_endpoint);
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
        return;
    }

  sa_copy = g_new0 (struct sockaddr, 1);
  memcpy (sa_copy, sa, sizeof(struct sockaddr));

  *list_ptr = g_list_append (*list_ptr, sa_copy);
}

static gint subnet_mask_to_prefix_len(const struct sockaddr *mask)
{
  if (!mask)
      return -1;

  if (mask->sa_family == AF_INET) 
    {
      const struct sockaddr_in *sin = (const struct sockaddr_in *)mask;
      guint32 mask4 = ntohl(sin->sin_addr.s_addr);

      // Count ones from MSB
      guint prefix = 0;
      gboolean hole_found = FALSE;
      for (gint i = 31; i >= 0; --i) {
          if (mask4 & (1U << i)) {
              if (hole_found)
                  return -1; // Non-contiguous
              prefix++;
          } else {
              hole_found = TRUE;
          }
      }
      return prefix;
    } 
  else if (mask->sa_family == AF_INET6) 
    {
      const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)mask;
      guint prefix = 0;
      gboolean hole_found = FALSE;

      for (gint i = 0; i < 16; ++i) {
          guint8 octet = sin6->sin6_addr.s6_addr[i];
          for (gint b = 7; b >= 0; --b) {
              if (octet & (1U << b)) {
                  if (hole_found)
                      return -1; // Non-contiguous
                  prefix++;
              } else {
                  hole_found = TRUE;
              }
          }
      } 
      return prefix;
    }
  // Unsupported address family
  return -1;
}

static void print_address (LocalRoute * local_route)
{
  struct sockaddr * sa = (struct sockaddr *) &local_route->addr;
  if (sa->sa_family == AF_INET) 
    {
      char buf_addr[INET_ADDRSTRLEN];
      char buf_netmask[INET_ADDRSTRLEN];
      char buf_dstaddr[INET_ADDRSTRLEN] = "";
      struct sockaddr_in *sin_addr = (struct sockaddr_in *)&local_route->addr;
      struct sockaddr_in *sin_netmask = (struct sockaddr_in *)&local_route->netmask;
      struct sockaddr_in *sin_dstaddr = (struct sockaddr_in *)&local_route->dstaddr;
      inet_ntop(AF_INET, &sin_addr->sin_addr, buf_addr, sizeof(buf_addr));
      inet_ntop(AF_INET, &sin_netmask->sin_addr, buf_netmask, sizeof(buf_netmask));
      inet_ntop(AF_INET, &sin_dstaddr->sin_addr, buf_dstaddr, sizeof(buf_dstaddr));
      printf("%s IPv4: %s netmask:%s prefix:%d dstaddr:%s\n", local_route->if_name, buf_addr, buf_netmask, local_route->prefix_len, buf_dstaddr);
    }
  else if (sa->sa_family == AF_INET6)
    {
      char buf_addr[INET6_ADDRSTRLEN];
      char buf_netmask[INET6_ADDRSTRLEN];
      char buf_dstaddr[INET6_ADDRSTRLEN] = "";
      struct sockaddr_in6 *sin6_addr = (struct sockaddr_in6 *)&local_route->addr;
      struct sockaddr_in6 *sin6_netmask = (struct sockaddr_in6 *)&local_route->netmask;
      struct sockaddr_in6 *sin6_dstaddr = (struct sockaddr_in6 *)&local_route->dstaddr;
      inet_ntop(AF_INET6, &sin6_addr->sin6_addr, buf_addr, sizeof(buf_addr));
      inet_ntop(AF_INET6, &sin6_netmask->sin6_addr, buf_netmask, sizeof(buf_netmask));
      inet_ntop(AF_INET6, &sin6_dstaddr->sin6_addr, buf_dstaddr, sizeof(buf_dstaddr));
      printf("%s IPv6: %s netmask:%s prefix:%d dstaddr:%s\n", local_route->if_name, buf_addr, buf_netmask, local_route->prefix_len, buf_dstaddr);
    }
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

static void network_monitor_process_routes (GAppleNetworkMonitor * apple, NetworkStatusData * status_data)
{
  GPtrArray * networks = NULL;
  gint local_routes_len = g_list_length(status_data->local_routes);
  gint route_count = (local_routes_len + (status_data->ipv4_gateways == NULL ? 0 : 1) + (status_data->ipv6_gateways == NULL ? 0 : 1));
  GList * iter;

  if (route_count == 0)
    {
      g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (apple), NULL, 0);
      return;      
    }

  networks = g_ptr_array_new_full (route_count, g_object_unref);

  /* Add ipv4 default route if available */
  if (status_data->ipv4_gateways != NULL) 
    {
      GInetAddressMask * network = get_network_mask (G_SOCKET_FAMILY_IPV4, NULL, 0);
      if (network != NULL)
        g_ptr_array_add (networks, network);
    }

  /* Add ipv6 default route if available */
  if (status_data->ipv6_gateways != NULL)
    {
      GInetAddressMask * network = get_network_mask (G_SOCKET_FAMILY_IPV6, NULL, 0);
      if (network != NULL)
        g_ptr_array_add (networks, network);
    }

  /* Add routes extracted from the local interfaces first */
  for (iter = status_data->local_routes; iter != NULL; iter = iter->next)
    {
      LocalRoute * local_route;
      GInetAddressMask *network;
      GSocketFamily family = G_SOCKET_FAMILY_INVALID;
      const guint8 *dest = NULL;
      gsize len;

      local_route = iter->data;
      len = local_route->prefix_len;

      print_address (local_route);

      if (local_route->af == AF_INET)
        {
          family = G_SOCKET_FAMILY_IPV4;
          dest = (const guint8 *)&local_route->addr;
        }
      else if (local_route->af == AF_INET6)
        {
          family = G_SOCKET_FAMILY_IPV6;
          dest = (const guint8 *)&local_route->addr;
        }

      network = get_network_mask (family, dest, len);
      if (network == NULL)
        continue;

      g_ptr_array_add (networks, network);
    }

    g_network_monitor_base_set_networks (G_NETWORK_MONITOR_BASE (apple), networks->pdata, networks->len);
    g_ptr_array_free (networks, TRUE);      
} 

static void network_status_parse_interface_routes (NetworkStatusData * status_data)
{
  struct ifaddrs *ifaddr, *ifa;
  getifaddrs(&ifaddr);

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
      LocalRoute * route;
      gint prefix_len;

      if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_addr) || !(ifa->ifa_netmask))
        continue;

      if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
        continue;

      prefix_len = subnet_mask_to_prefix_len(ifa->ifa_netmask);
      if (prefix_len == -1)
        continue;

      route = g_new0 (LocalRoute, 1);
      strncpy (route->if_name, ifa->ifa_name, sizeof (route->if_name)-1);
      route->af = ifa->ifa_addr->sa_family;
      route->prefix_len = prefix_len;
      memcpy (&route->addr, ifa->ifa_addr, ifa->ifa_addr->sa_len);
      memcpy (&route->netmask, ifa->ifa_netmask, ifa->ifa_netmask->sa_len);
      if (ifa->ifa_dstaddr)
        memcpy (&route->dstaddr, ifa->ifa_dstaddr, ifa->ifa_dstaddr->sa_len);
      status_data->local_routes = g_list_append (status_data->local_routes, route);
    }

  freeifaddrs(ifaddr);
}

/*
static void print_gateway_addr(const struct sockaddr *sa) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (!sa) {
        printf("Null sockaddr\n");
        return;
    }
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &(sin->sin_addr), buf, sizeof(buf));
        printf("IPv4 Gateway: %s\n", buf);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &(sin6->sin6_addr), buf, sizeof(buf));
        printf("IPv6 Gateway: %s\n", buf);
    } else {
        printf("Unknown sockaddr family %d\n", sa->sa_family);
    }
}

bool (^gateway_enumerator)(nw_endpoint_t) = ^bool(nw_endpoint_t gateway_endpoint)
{
  nw_endpoint_type_t type = nw_endpoint_get_type (gateway_endpoint);
  if (type == nw_endpoint_type_address) {
      const struct sockaddr *sa = nw_endpoint_get_address(gateway_endpoint);
      print_gateway_addr(sa);
  } else if (type == nw_endpoint_type_host) {
      printf("Gateway endpoint is a host\n");
  } else {
      printf("Gateway endpoint is of unsupported type %d\n", type);
  }
  return true;
};
*/

/*
static void print_ip_addresses_for_interface(const char * if_name) {
    struct ifaddrs *ifaddr, *ifa;
    getifaddrs(&ifaddr);
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_addr))
            continue;

        if (g_strcmp0 (if_name, ifa->ifa_name) == 0) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                char buf_addr[INET_ADDRSTRLEN];
                char buf_netmask[INET_ADDRSTRLEN];
                struct sockaddr_in *sin_addr = (struct sockaddr_in *)ifa->ifa_addr;
                struct sockaddr_in *sin_netmask = (struct sockaddr_in *)ifa->ifa_netmask;
                inet_ntop(AF_INET, &sin_addr->sin_addr, buf_addr, sizeof(buf_addr));
                inet_ntop(AF_INET, &sin_netmask->sin_addr, buf_netmask, sizeof(buf_netmask));
                printf("%s IPv4: %s netmask:%s\n", ifa->ifa_name, buf_addr, buf_netmask);
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6) {
                char buf_addr[INET6_ADDRSTRLEN];
                char buf_netmask[INET6_ADDRSTRLEN];
                struct sockaddr_in6 *sin6_addr = (struct sockaddr_in6 *)ifa->ifa_addr;
                struct sockaddr_in6 *sin6_netmask = (struct sockaddr_in6 *)ifa->ifa_netmask;
                inet_ntop(AF_INET6, &sin6_addr->sin6_addr, buf_addr, sizeof(buf_addr));
                inet_ntop(AF_INET6, &sin6_netmask->sin6_addr, buf_netmask, sizeof(buf_netmask));
                printf("%s IPv6: %s netmask:%s\n", ifa->ifa_name, buf_addr, buf_netmask);
            }
        }
    }
    freeifaddrs(ifaddr);
}

bool (^interface_enumerator)(nw_interface_t interface) = ^bool(nw_interface_t interface)
{
  const char *interfaceName = nw_interface_get_name(interface);
  nw_interface_type_t interfaceType = nw_interface_get_type(interface);

  printf("Interface: %s, Type: %d\n", interfaceName, interfaceType);

  print_ip_addresses_for_interface (interfaceName);
  // Return true to continue enumeration, false to stop
  return true;
};
*/

static nw_interface_type_t network_status_get_interface_type (nw_path_t path)
{ 
  /* Only call when nw_path_status_t == nw_path_status_satisfied.
     We should never see nw_interface_type_loopback or nw_interface_type_other here, 
     but we parse all of them regardless, and default to nw_interface_type_other.
  */

  if (nw_path_uses_interface_type(path, nw_interface_type_wifi)) {
    return nw_interface_type_wifi;
  } else if (nw_path_uses_interface_type(path, nw_interface_type_cellular)) {
    return nw_interface_type_cellular;
  } else if (nw_path_uses_interface_type(path, nw_interface_type_wired)) {
    return nw_interface_type_wired;
  } else if (nw_path_uses_interface_type(path, nw_interface_type_loopback)) {
    return nw_interface_type_loopback;
  } else if (nw_path_uses_interface_type(path, nw_interface_type_other)) {
    return nw_interface_type_other;
  }
  return nw_interface_type_other;
}

static gboolean
apple_network_monitor_invoke_route_changed (gpointer user_data)
{
  GString * msg = NULL;
  NetworkStatusData * status_data = user_data;

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
  network_monitor_process_routes (status_data->apple, status_data);

  /* Debug printing */
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

  if (msg)
    {
      g_warning ("%s", msg->str);
      g_string_free (msg, TRUE);    
    }

  return G_SOURCE_REMOVE;
}

static void network_status_changed (nw_path_t path, GAppleNetworkMonitor * apple)
{
  NetworkStatusData * status_data = g_new0 (NetworkStatusData, 1);
  status_data->apple = apple;

  status_data->status = nw_path_get_status(path);
  if (status_data->status == nw_path_status_satisfied)
    {
        status_data->interface_type = network_status_get_interface_type (path);
        status_data->is_expensive = nw_path_is_expensive(path);
        status_data->is_constrained = nw_path_is_constrained(path);
        status_data->has_dns = nw_path_has_dns(path);
        status_data->has_ipv4 = nw_path_has_ipv4(path);
        status_data->has_ipv6 = nw_path_has_ipv6(path);

        nw_path_enumerate_gateways (path, ^bool(nw_endpoint_t gateway_endpoint)
          {
            nw_endpoint_type_t type = nw_endpoint_get_type (gateway_endpoint);
            if (type == nw_endpoint_type_address) {
                network_status_parse_gateway(status_data, gateway_endpoint);
            } else if (type == nw_endpoint_type_host) {
                printf("Gateway endpoint is a host\n");
            } else {
                printf("Gateway endpoint is of unsupported type %d\n", type);
            }
            return true;
          });

        network_status_parse_interface_routes (status_data);
    }

  apple->priv->status_change_source = g_idle_source_new ();
  g_source_set_priority (apple->priv->status_change_source, G_PRIORITY_DEFAULT);
  g_source_set_callback (apple->priv->status_change_source,
                         apple_network_monitor_invoke_route_changed,
                         status_data,
                         (GDestroyNotify)network_status_data_free);

  g_source_attach (apple->priv->status_change_source, apple->priv->main_context);
}

static gboolean
g_apple_network_monitor_initable_init (GInitable     *initable,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  GAppleNetworkMonitor *apple = G_APPLE_NETWORK_MONITOR (initable);
  g_assert (apple);

  if (!apple->priv->initialized)
    {
      apple->priv->main_context = g_main_context_ref_thread_default ();

      /* TODO Initialize apple specific stuff here! */

      apple->priv->monitor = nw_path_monitor_create();
      if (!apple->priv->monitor)
        {
          //TODO: FIX WIRH GERROR!
          fprintf(stderr, "Monitor creation failed.\n");
          return FALSE;
        }

      nw_path_monitor_prohibit_interface_type (apple->priv->monitor, nw_interface_type_loopback);
      nw_path_monitor_prohibit_interface_type (apple->priv->monitor, nw_interface_type_other);
      nw_path_monitor_set_update_handler(apple->priv->monitor, ^(nw_path_t path) {
        network_status_changed (path, apple);
      });

      nw_path_monitor_start(apple->priv->monitor);
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
