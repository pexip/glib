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

#ifndef __G_APPLE_NETWORK_MONITOR_H__
#define __G_APPLE_NETWORK_MONITOR_H__

#include "gnetworkmonitorbase.h"

G_BEGIN_DECLS

#define G_TYPE_APPLE_NETWORK_MONITOR         (_g_win32_network_monitor_get_type ())
#define G_APPLE_NETWORK_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_APPLE_NETWORK_MONITOR, GAppleNetworkMonitor))
#define G_APPLE_NETWORK_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_APPLE_NETWORK_MONITOR, GAppleNetworkMonitorClass))
#define G_IS_APPLE_NETWORK_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_APPLE_NETWORK_MONITOR))
#define G_IS_APPLE_NETWORK_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_APPLE_NETWORK_MONITOR))
#define G_APPLE_NETWORK_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_APPLE_NETWORK_MONITOR, GAppleNetworkMonitorClass))

typedef struct _GAppleNetworkMonitor        GAppleNetworkMonitor;
typedef struct _GAppleNetworkMonitorClass   GAppleNetworkMonitorClass;
typedef struct _GAppleNetworkMonitorPrivate GAppleNetworkMonitorPrivate;

struct _GAppleNetworkMonitor {
  GNetworkMonitorBase parent_instance;

  GAppleNetworkMonitorPrivate *priv;
};

struct _GAppleNetworkMonitorClass {
  GNetworkMonitorBaseClass parent_class;
};

GType _g_apple_network_monitor_get_type (void);

G_END_DECLS

#endif /* __G_APPLE_NETWORK_MONITOR_H__ */
