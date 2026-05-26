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

#ifndef __G_ANDROID_NETWORK_MONITOR_H__
#define __G_ANDROID_NETWORK_MONITOR_H__

#include "gnetworkmonitorbase.h"

#include <jni.h>

G_BEGIN_DECLS

#define G_TYPE_ANDROID_NETWORK_MONITOR         (_g_android_network_monitor_get_type ())
#define G_ANDROID_NETWORK_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_ANDROID_NETWORK_MONITOR, GAndroidNetworkMonitor))
#define G_ANDROID_NETWORK_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_ANDROID_NETWORK_MONITOR, GAndroidNetworkMonitorClass))
#define G_IS_ANDROID_NETWORK_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_ANDROID_NETWORK_MONITOR))
#define G_IS_ANDROID_NETWORK_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_ANDROID_NETWORK_MONITOR))
#define G_ANDROID_NETWORK_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_ANDROID_NETWORK_MONITOR, GAndroidNetworkMonitorClass))

typedef struct _GAndroidNetworkMonitor        GAndroidNetworkMonitor;
typedef struct _GAndroidNetworkMonitorClass   GAndroidNetworkMonitorClass;
typedef struct _GAndroidNetworkMonitorPrivate GAndroidNetworkMonitorPrivate;

struct _GAndroidNetworkMonitor
{
  GNetworkMonitorBase parent_instance;

  GAndroidNetworkMonitorPrivate *priv;
};

struct _GAndroidNetworkMonitorClass
{
  GNetworkMonitorBaseClass parent_class;
};

GType _g_android_network_monitor_get_type (void);

/**
 * g_android_network_monitor_set_application_context:
 * @env: a JNI environment pointer for the calling thread
 * @context: a local or global reference to an `android.content.Context`
 *           (typically the application context)
 *
 * Provides the Android `Context` that the Android network monitor will
 * use to obtain a `ConnectivityManager`. This MUST be called once at
 * application startup, before the first `GNetworkMonitor` is created,
 * otherwise the Android network monitor will fail to initialize and the
 * default `GNetworkMonitorBase` (which assumes the network is always
 * available) will be used as a fallback.
 *
 * The function takes its own global reference; the caller may release
 * @context after the call returns.
 */
GIO_AVAILABLE_IN_ALL
void g_android_network_monitor_set_application_context (JNIEnv  *env,
                                                        jobject  context);

G_END_DECLS

#endif /* __G_ANDROID_NETWORK_MONITOR_H__ */
