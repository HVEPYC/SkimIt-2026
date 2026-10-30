package com.prime31;

/**
 * Offline stand-in for Prime31's Flurry plugin.
 *
 * Analytics calls are dropped. Ad requests fail the way they did on the original without a
 * network: Flurry's listener reported spaceDidFailToReceiveAd for the space shortly after the
 * request (the DNS lookup failed), which closes the game's "watch an advert" spinner.
 * The method set matches the original plugin exactly (it never had getAndroidId).
 */
public class FlurryPlugin {
    private static final String MANAGER = "FlurryAndroidManager";
    private static FlurryPlugin instance;

    public static synchronized FlurryPlugin instance() {
        if (instance == null) instance = new FlurryPlugin();
        return instance;
    }

    public void onStartSession(String apiKey, boolean initializeAds, boolean enableTestAds) {}

    public void onEndSession() {}

    public void logEvent(String eventName) {}

    public void logEventWithParams(String eventName, String paramsJson) {}

    public void logTimedEvent(String eventName) {}

    public void logTimedEventWithParams(String eventName, String paramsJson) {}

    public void endTimedEvent(String eventName) {}

    public void addUserCookie(String key, String value) {}

    public void clearUserCookies() {}

    public void fetchAdsForSpace(String adSpace, int adSize) {
        Bridge.sendLater(Bridge.AD_FAILURE_DELAY_MS, MANAGER, "spaceDidFailToReceiveAd", adSpace);
    }

    public void displayAd(String adSpace, int adSize, long timeout) {
        Bridge.sendLater(Bridge.AD_FAILURE_DELAY_MS, MANAGER, "spaceDidFailToReceiveAd", adSpace);
    }

    public void isAdAvailable(String adSpace, int adSize, long timeout) {
        Bridge.sendLater(Bridge.AD_FAILURE_DELAY_MS, MANAGER, "adNotAvailableForSpace", adSpace);
    }

    public void removeAd(String adSpace) {}

    // Listener entry points of the original plugin, kept so the method set is identical.
    public void onAdClicked(String adSpace) {}

    public void onAdClosed(String adSpace) {}

    public void onAdOpened(String adSpace) {}

    public void onApplicationExit(String adSpace) {}

    public void onRenderFailed(String adSpace) {}

    public void spaceDidFailToReceiveAd(String adSpace) {}

    public void spaceDidReceiveAd(String adSpace) {}
}
