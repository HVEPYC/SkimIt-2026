package com.prime31;

import android.app.Activity;

import java.lang.reflect.Method;
import java.util.Timer;
import java.util.TimerTask;

/**
 * Shared plumbing for the offline plugin stand-ins.
 *
 * Like the original Prime31 plugins, Unity is reached through reflection on
 * com.unity3d.player.UnityPlayer (currentActivity and UnitySendMessage), so these
 * classes need nothing from Unity at compile time.
 */
final class Bridge {
    /** Message the original Google IAB helper produced when the billing service was unreachable. */
    static final String BILLING_UNAVAILABLE = "Billing service unavailable on device. (response: 3:Billing Unavailable)";

    /** Offline, Flurry's ad requests failed after the DNS lookup, about 0.7 s on the reference device. */
    static final long AD_FAILURE_DELAY_MS = 700;

    private static final Timer TIMER = new Timer("prime31-offline", true);

    private Bridge() {}

    static Activity activity() {
        try {
            Class<?> player = Class.forName("com.unity3d.player.UnityPlayer");
            return (Activity) player.getField("currentActivity").get(null);
        } catch (Exception e) {
            return null;
        }
    }

    static void send(String gameObject, String method, String message) {
        try {
            Class<?> player = Class.forName("com.unity3d.player.UnityPlayer");
            Method m = player.getMethod("UnitySendMessage", String.class, String.class, String.class);
            m.invoke(null, gameObject, method, message == null ? "" : message);
        } catch (Exception e) {
            // Unity is not running (e.g. a notification arriving while the game is closed).
        }
    }

    /** Sends asynchronously, as the original SDK callbacks arrived on other threads. */
    static void sendLater(long delayMs, final String gameObject, final String method, final String message) {
        TIMER.schedule(new TimerTask() {
            @Override
            public void run() {
                send(gameObject, method, message);
            }
        }, delayMs);
    }
}
