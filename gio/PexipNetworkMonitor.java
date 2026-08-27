// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright 2026 Pexip
//
// Java half of the GLib/GIO Android network monitor.
//
// This class is the Java bridge between Android's ConnectivityManager and
// the native GAndroidNetworkMonitor in libgio. It is loaded by the native
// side via JNI; the application is responsible for shipping this file in
// its APK (e.g. by copying it into `app/src/main/java/com/pexip/glib/`).
//
// Requirements:
//   * AndroidManifest.xml must declare the permission:
//       <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE"/>
//   * The application must call, once at startup from JNI:
//       g_android_network_monitor_set_application_context(env, ctx);
//     where `ctx` is the Application context.
//   * minSdkVersion 24 or higher (for registerDefaultNetworkCallback).
//
// The class is intentionally tiny: it observes the system-default network,
// extracts the current capabilities and link properties, marshals the
// routing table into a String[], and calls back into the native side.

package com.pexip.glib;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.RouteInfo;
import android.os.Build;
import android.util.Log;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.List;

/**
 * Bridge from Android's {@link ConnectivityManager} to libgio's
 * GAndroidNetworkMonitor.
 *
 * <p>One instance is created per {@code GAndroidNetworkMonitor} on the
 * native side. The constructor immediately registers a default-network
 * callback; {@link #stop()} unregisters it. All callbacks run on the
 * binder thread allocated by {@code ConnectivityManager}; the native
 * receiver re-dispatches onto its own main context.
 */
public final class PexipNetworkMonitor {
    private static final String TAG = "PexipNetworkMonitor";

    // Must match the constants in gandroidnetworkmonitor.c.
    private static final int TRANSPORT_UNKNOWN = 0;
    private static final int TRANSPORT_WIFI = 1;
    private static final int TRANSPORT_CELLULAR = 2;
    private static final int TRANSPORT_ETHERNET = 3;
    private static final int TRANSPORT_VPN = 4;
    private static final int TRANSPORT_BLUETOOTH = 5;

    // Route descriptor flag bits, must match gandroidnetworkmonitor.c.
    private static final int FLAG_DEFAULT_ROUTE = 1;
    private static final int FLAG_LOOPBACK = 2;
    private static final int FLAG_POINT_TO_POINT = 4;

    private final long nativeHandle;
    private final ConnectivityManager connectivityManager;
    private final ConnectivityManager.NetworkCallback callback;

    // Latched values across the various NetworkCallback methods. Android
    // invokes onAvailable / onCapabilitiesChanged / onLinkPropertiesChanged
    // separately, but we want to report a single coherent snapshot to the
    // native side. We update one of these on each callback and re-emit.
    private boolean available = false;
    private boolean validated = false;
    private boolean metered = true;
    private int transport = TRANSPORT_UNKNOWN;
    private String[] routes = new String[0];

    public PexipNetworkMonitor(long nativeHandle, Context context) {
        this.nativeHandle = nativeHandle;
        this.connectivityManager =
                (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (this.connectivityManager == null) {
            throw new IllegalStateException("ConnectivityManager service unavailable");
        }

        this.callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                available = true;
                refresh(network);
            }

            @Override
            public void onLost(Network network) {
                available = false;
                validated = false;
                metered = true;
                transport = TRANSPORT_UNKNOWN;
                routes = new String[0];
                emit();
            }

            @Override
            public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                applyCapabilities(caps);
                refresh(network);
            }

            @Override
            public void onLinkPropertiesChanged(Network network, LinkProperties props) {
                routes = marshalRoutes(props);
                emit();
            }
        };

        // registerDefaultNetworkCallback requires API 24.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            connectivityManager.registerDefaultNetworkCallback(callback);
        } else {
            throw new UnsupportedOperationException(
                    "PexipNetworkMonitor requires Android API 24 or higher");
        }
    }

    /** Unregister the callback. Idempotent. */
    public void stop() {
        try {
            connectivityManager.unregisterNetworkCallback(callback);
        } catch (IllegalArgumentException e) {
            // Already unregistered; ignore.
        }
    }

    /** Read the current state for `network` and re-emit. */
    private void refresh(Network network) {
        try {
            NetworkCapabilities caps = connectivityManager.getNetworkCapabilities(network);
            if (caps != null) {
                applyCapabilities(caps);
            }
            LinkProperties props = connectivityManager.getLinkProperties(network);
            if (props != null) {
                routes = marshalRoutes(props);
            }
        } catch (SecurityException e) {
            // ACCESS_NETWORK_STATE not granted. Log loudly; the network
            // monitor will still report "unavailable" which is the
            // safest default.
            Log.w(TAG, "Missing ACCESS_NETWORK_STATE permission?", e);
        }
        emit();
    }

    private void applyCapabilities(NetworkCapabilities caps) {
        validated = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
        // "available" tracks whether the network has internet capability;
        // onLost() will reset it when the network goes away entirely.
        available = available
                && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
        metered = !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);

        if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
            transport = TRANSPORT_WIFI;
        } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
            transport = TRANSPORT_CELLULAR;
        } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
            transport = TRANSPORT_ETHERNET;
        } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) {
            transport = TRANSPORT_VPN;
        } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_BLUETOOTH)) {
            transport = TRANSPORT_BLUETOOTH;
        } else {
            transport = TRANSPORT_UNKNOWN;
        }
    }

    /**
     * Convert a LinkProperties into the descriptor format that the native
     * side parses. Each entry is
     *   "family;prefix;dest;gateway;ifname;flags"
     * where family is "4" or "6", dest is the textual address, gateway is
     * the textual gateway (or "" for on-link), ifname is the interface
     * name, and flags is a decimal bitfield.
     */
    private static String[] marshalRoutes(LinkProperties props) {
        List<String> out = new ArrayList<>();
        String ifname = props.getInterfaceName();
        if (ifname == null) {
            ifname = "";
        }

        // 1. Routes from the kernel's view, as Android reports them.
        for (RouteInfo route : props.getRoutes()) {
            String desc = describeRoute(route, ifname);
            if (desc != null) {
                out.add(desc);
            }
        }

        // 2. Also include any locally-attached link addresses that
        //    weren't already covered by the routes list (defensive; the
        //    routes list usually already contains the on-link prefix).
        for (LinkAddress la : props.getLinkAddresses()) {
            String desc = describeLinkAddress(la, ifname);
            if (desc != null) {
                out.add(desc);
            }
        }

        return out.toArray(new String[0]);
    }

    private static String describeRoute(RouteInfo route, String defaultIfname) {
        InetAddress dest = route.getDestination().getAddress();
        int prefix = route.getDestination().getPrefixLength();
        InetAddress gw = route.getGateway();
        String ifname = route.getInterface();
        if (ifname == null || ifname.isEmpty()) {
            ifname = defaultIfname;
        }

        int family = familyOf(dest);
        if (family == 0) {
            return null;
        }

        int flags = 0;
        if (route.isDefaultRoute()) {
            flags |= FLAG_DEFAULT_ROUTE;
        }
        if (dest.isLoopbackAddress()) {
            flags |= FLAG_LOOPBACK;
        }

        String gwStr = "";
        if (gw != null && !gw.isAnyLocalAddress()) {
            gwStr = gw.getHostAddress();
            if (gwStr == null) {
                gwStr = "";
            }
        }

        String destStr = dest.getHostAddress();
        if (destStr == null) {
            return null;
        }
        // Strip IPv6 zone identifiers (e.g. "fe80::1%wlan0") that
        // inet_pton on the native side does not accept.
        int pct = destStr.indexOf('%');
        if (pct >= 0) {
            destStr = destStr.substring(0, pct);
        }
        int gwPct = gwStr.indexOf('%');
        if (gwPct >= 0) {
            gwStr = gwStr.substring(0, gwPct);
        }

        return family + ";" + prefix + ";" + destStr + ";" + gwStr + ";"
                + sanitize(ifname) + ";" + flags;
    }

    private static String describeLinkAddress(LinkAddress la, String ifname) {
        InetAddress addr = la.getAddress();
        int family = familyOf(addr);
        if (family == 0) {
            return null;
        }
        String addrStr = addr.getHostAddress();
        if (addrStr == null) {
            return null;
        }
        int pct = addrStr.indexOf('%');
        if (pct >= 0) {
            addrStr = addrStr.substring(0, pct);
        }
        return family + ";" + la.getPrefixLength() + ";" + addrStr + ";;"
                + sanitize(ifname) + ";0";
    }

    private static int familyOf(InetAddress a) {
        if (a instanceof Inet4Address) {
            return 4;
        }
        if (a instanceof Inet6Address) {
            return 6;
        }
        return 0;
    }

    /** Strip ';' from a string so it cannot break our descriptor format. */
    private static String sanitize(String s) {
        if (s == null) {
            return "";
        }
        return s.replace(';', '_');
    }

    private void emit() {
        nativeOnNetworkChanged(nativeHandle, available, validated, metered, transport, routes);
    }

    // Implemented in gandroidnetworkmonitor.c.
    private static native void nativeOnNetworkChanged(
            long nativeHandle,
            boolean available,
            boolean validated,
            boolean metered,
            int transport,
            String[] routes);
}
