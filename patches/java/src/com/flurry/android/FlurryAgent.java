package com.flurry.android;

/**
 * Offline stand-in for the static FlurryAgent calls the game makes directly from C#.
 * Analytics are dropped; nothing leaves the device.
 */
public final class FlurryAgent {
    private FlurryAgent() {}

    public static void onError(String errorId, String message, String errorClass) {}

    public static void onPageView() {}

    public static void setAge(int age) {}

    public static void setContinueSessionMillis(long millis) {}

    public static void setLogEnabled(boolean enabled) {}

    public static void setUserId(String userId) {}
}
