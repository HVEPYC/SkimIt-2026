package com.prime31;

/**
 * Offline stand-in for Prime31's Twitter plugin (shipped with the game, never called by it).
 * Nobody is ever logged in; login and requests report failure.
 */
public class TwitterPlugin {
    private static final String MANAGER = "TwitterAndroidManager";
    private static final String OFFLINE = "Twitter is not available in this offline edition";
    private static TwitterPlugin instance;

    public static synchronized TwitterPlugin instance() {
        if (instance == null) instance = new TwitterPlugin();
        return instance;
    }

    public void init(String consumerKey, String consumerSecret) {}

    public boolean isLoggedIn() {
        return false;
    }

    public void logout() {}

    public void showLoginDialog() {
        Bridge.sendLater(0, MANAGER, "loginDidFail", OFFLINE);
    }

    public void postUpdateWithImage(String update, byte[] image) {
        Bridge.sendLater(0, MANAGER, "requestFailed", OFFLINE);
    }

    public void performRequest(String methodType, String path, String parameters) {
        Bridge.sendLater(0, MANAGER, "requestFailed", OFFLINE);
    }
}
