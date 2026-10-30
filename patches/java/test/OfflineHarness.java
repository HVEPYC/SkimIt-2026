import com.prime31.FacebookPlugin;
import com.prime31.FlurryPlugin;
import com.prime31.GoogleIABPlugin;
import com.prime31.TwitterPlugin;
import com.unity3d.player.UnityPlayer;

/**
 * Drives the offline stand-ins the way the game's C# does and checks the events that come
 * back through UnitySendMessage (names, payloads, timing). Runs on a plain JVM.
 */
public class OfflineHarness {
    private static int failures;

    public static void main(String[] args) throws Exception {
        // PurchasingManager.Start(): GoogleIAB.init(key) -> billingNotSupported with the original message.
        expect(() -> GoogleIABPlugin.instance().init("key"),
                "GoogleIABManager", "billingNotSupported",
                "Billing service unavailable on device. (response: 3:Billing Unavailable)", 0, 200);
        check("areSubscriptionsSupported is false", !GoogleIABPlugin.instance().areSubscriptionsSupported());
        expect(() -> GoogleIABPlugin.instance().queryInventory(new String[] {"coins"}),
                "GoogleIABManager", "queryInventoryFailed", null, 0, 200);
        expect(() -> GoogleIABPlugin.instance().purchaseProduct("coins", ""),
                "GoogleIABManager", "purchaseFailed", null, 0, 200);

        // AnalyticsManager: analytics are silent; the advert request fails fast (~0.7 s, as on the reference device).
        UnityPlayer.MESSAGES.clear();
        FlurryPlugin.instance().onStartSession("key", true, false);
        FlurryPlugin.instance().logEvent("event");
        FlurryPlugin.instance().logEventWithParams("event", "{}");
        Thread.sleep(300);
        check("analytics send no events", UnityPlayer.MESSAGES.isEmpty());
        expect(() -> FlurryPlugin.instance().displayAd("splash", 6, 10000L),
                "FlurryAndroidManager", "spaceDidFailToReceiveAd", "splash", 600, 1200);
        expect(() -> FlurryPlugin.instance().fetchAdsForSpace("splash", 6),
                "FlurryAndroidManager", "spaceDidFailToReceiveAd", "splash", 600, 1200);

        // NotificationManager: Facebook login fails through the game's own handler.
        check("no Facebook session", !FacebookPlugin.instance().isSessionValid());
        check("no Facebook permissions", "[]".equals(FacebookPlugin.instance().getSessionPermissions()));
        expect(() -> FacebookPlugin.instance().loginWithReadPermissions(new String[] {"email"}),
                "FacebookManager", "loginFailed", null, 0, 200);
        expect(() -> FacebookPlugin.instance().reauthorizeWithPublishPermissions("publish_actions", "EVERYONE"),
                "FacebookManager", "reauthorizationFailed", null, 0, 200);

        check("Twitter never logged in", !TwitterPlugin.instance().isLoggedIn());
        expect(() -> TwitterPlugin.instance().showLoginDialog(), "TwitterAndroidManager", "loginDidFail", null, 0, 200);

        System.out.println(failures == 0 ? "\nALL PASSED" : "\nFAILED: " + failures);
        System.exit(failures == 0 ? 0 : 1);
    }

    /** Runs the action and waits for exactly one event, checking its payload and arrival time. */
    private static void expect(Runnable action, String object, String method, String payload, long minMs, long maxMs)
            throws InterruptedException {
        UnityPlayer.MESSAGES.clear();
        UnityPlayer.startNanos = System.nanoTime();
        action.run();
        long deadline = System.currentTimeMillis() + maxMs + 500;
        while (UnityPlayer.MESSAGES.isEmpty() && System.currentTimeMillis() < deadline) Thread.sleep(10);
        Thread.sleep(50);

        String label = object + "." + method;
        if (UnityPlayer.MESSAGES.size() != 1) {
            check(label + ": exactly one event (got " + UnityPlayer.MESSAGES.size() + ")", false);
            return;
        }
        String[] m = UnityPlayer.MESSAGES.get(0);
        long at = Long.parseLong(m[3]);
        check(label + " sent (" + at + " ms)", m[0].equals(object) && m[1].equals(method)
                && (payload == null || payload.equals(m[2])) && at >= minMs && at <= maxMs);
    }

    private static void check(String what, boolean ok) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok) failures++;
    }
}
