# GLib/GIO Android Network Monitor

This document describes how to integrate the Android-specific
`GNetworkMonitor` implementation (`GAndroidNetworkMonitor`) into an
Android application that links against GLib/GIO.

Android does not expose the routing table or network state through any
of the usual POSIX/Linux mechanisms (netlink is blocked by SELinux for
apps from Android 10 / API 29 onwards, `/proc/net/*` is locked down,
and `getifaddrs()` lacks route information). The supported way to
observe network state on Android is through `ConnectivityManager` and
`NetworkCallback`, which is a Java-only API.

`GAndroidNetworkMonitor` is split into two halves:

* **Native (C/JNI)** — `gio/gandroidnetworkmonitor.c`, compiled into
  `libgio-2.0.so` whenever `host_system == 'android'`.
* **Java helper** — `gio/PexipNetworkMonitor.java`. This file is **not**
  built by meson. The application must include it in its APK, in the
  `com.pexip.glib` package.

## Integration checklist

### 1. Manifest permission

Add the following permission to `AndroidManifest.xml`:

```xml
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE"/>
```

Without this permission the network monitor will silently fail to
initialize and the default `GNetworkMonitorBase` (which assumes the
network is always available) will be used as a fallback.

### 2. minSdkVersion

`registerDefaultNetworkCallback` requires API 24 (Android 7.0). Set:

```gradle
android {
    defaultConfig {
        minSdkVersion 24
    }
}
```

### 3. Ship the Java helper

Copy `gio/PexipNetworkMonitor.java` into your Android module at
`src/main/java/com/pexip/glib/PexipNetworkMonitor.java`. The package
name and class name must match `com.pexip.glib.PexipNetworkMonitor`;
this is what the native side calls into via JNI.

### 4. Provide the application Context

The native side needs an Android `Context` to call
`Context.getSystemService(CONNECTIVITY_SERVICE)`. Because a native
library has no access to the application Context on its own, the
application must inject it once at startup, before any GLib/GIO calls
that might create a `GNetworkMonitor`.

If your app already has a JNI entry point at startup (most do), call
the public C function from there:

```c
#include <gio/gandroidnetworkmonitor.h>

JNIEXPORT void JNICALL
Java_com_yourcompany_yourapp_AppLoader_nativeInit (JNIEnv *env,
                                                   jclass  cls,
                                                   jobject application_context)
{
    g_android_network_monitor_set_application_context (env, application_context);
    /* ... other init ... */
}
```

The corresponding Java code:

```java
public final class AppLoader {
    static {
        System.loadLibrary("gio-2.0");
        // ... your other libraries ...
    }

    public static void init (Context context) {
        nativeInit (context.getApplicationContext ());
    }

    private static native void nativeInit (Context applicationContext);
}
```

The header `gio/gandroidnetworkmonitor.h` is installed to
`<prefix>/include/gio-android-2.0/gio/`.

### 5. Verify

Once the context is set and the application calls
`g_network_monitor_get_default ()`, you should see a `g_debug` message
similar to:

```
Network path is UP. Connected via Wi-Fi. Path is validated (internet reachable).
Routing table:
0.0.0.0/0 via gateway 192.168.1.1 on wlan0
::/0 via gateway fe80::1 on wlan0
192.168.1.0/24 interface wlan0
```

The `network-changed` signal will fire whenever the system default
network or its routes change.

## What is reported

| Property             | Source                                                                           |
| -------------------- | -------------------------------------------------------------------------------- |
| `network-available`  | `NetworkCapabilities.NET_CAPABILITY_INTERNET` (on the default network)           |
| `network-metered`    | `!NetworkCapabilities.NET_CAPABILITY_NOT_METERED`                                |
| `connectivity`       | `FULL` if validated, `LIMITED` if not yet validated, `LOCAL` if no network       |
| Routing table        | `LinkProperties.getRoutes()` + `LinkProperties.getLinkAddresses()`               |
| Transport (log only) | Wi-Fi / Cellular / Ethernet / VPN / Bluetooth                                    |

The monitor watches only the *system default* network (via
`registerDefaultNetworkCallback`), which matches what Apple's
`nw_path_monitor` does on iOS/macOS. If your app needs per-network
information (e.g. for multipath scenarios) you'll need to extend the
Java helper to use `registerNetworkCallback` with a
`NetworkRequest.Builder().clearCapabilities()`.

## Thread model

`NetworkCallback` invocations arrive on a Binder worker thread. The
native side never touches `GNetworkMonitorBase` state directly from
that thread; it parses the route descriptors and dispatches the update
onto the monitor's `GMainContext` via a `g_idle_source`. This matches
the Apple network monitor's threading model.

## Lifecycle

* Native `GAndroidNetworkMonitor` finalize → calls Java `stop()` →
  `ConnectivityManager.unregisterNetworkCallback(...)`. Global JNI
  refs are dropped.
* `JNI_OnUnload` (called when `libgio-2.0.so` is unloaded) drops the
  cached application Context global ref.
* The monitor uses a global handle table (protected by a mutex) so
  that any in-flight JNI callback that races with finalize is safely
  dropped instead of dereferencing freed memory.

## Limitations / future work

* The Java helper sends route descriptors as `String[]`. This is a
  deliberately simple wire format chosen to minimise the JNI surface;
  it could be swapped for a binary protocol if profiling shows it
  matters.
* IPv6 zone identifiers (e.g. `fe80::1%wlan0`) are stripped before
  being passed to `inet_pton`. The interface name is still preserved
  separately.
* If `ACCESS_NETWORK_STATE` is not granted, the Java helper logs a
  warning and the monitor reports "unavailable". This mirrors the
  behaviour on other platforms when underlying APIs are denied.
