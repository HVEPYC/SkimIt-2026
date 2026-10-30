package com.prime31;

import android.os.Bundle;

/**
 * Offline stand-in for Prime31's Facebook plugin.
 *
 * There is never an active session: logins and reauthorisations fail through the events
 * the game already handles, requests and dialogs report failure, and getSessionPermissions
 * returns "[]" exactly as the original did without a session.
 */
public class FacebookPlugin {
    private static final String MANAGER = "FacebookManager";
    private static final String OFFLINE = "Facebook is not available in this offline edition";
    private static FacebookPlugin instance;

    public static synchronized FacebookPlugin instance() {
        if (instance == null) instance = new FacebookPlugin();
        return instance;
    }

    public void init() {}

    public boolean isSessionValid() {
        return false;
    }

    public String getAccessToken() {
        return "";
    }

    public String getSessionPermissions() {
        return "[]";
    }

    public void loginWithReadPermissions(String[] permissions) {
        Bridge.sendLater(0, MANAGER, "loginFailed", OFFLINE);
    }

    public void loginWithPublishPermissions(String[] permissions) {
        Bridge.sendLater(0, MANAGER, "loginFailed", OFFLINE);
    }

    public void reauthorizeWithReadPermissions(String permissions) {
        Bridge.sendLater(0, MANAGER, "reauthorizationFailed", OFFLINE);
    }

    public void reauthorizeWithPublishPermissions(String permissions, String defaultAudience) {
        Bridge.sendLater(0, MANAGER, "reauthorizationFailed", OFFLINE);
    }

    public void logout() {}

    public void graphRequest(String graphPath, String httpMethod, Bundle parameters) {
        Bridge.sendLater(0, MANAGER, "graphRequestFailed", OFFLINE);
    }

    public void restRequest(String restMethod, String httpMethod, Bundle parameters) {
        Bridge.sendLater(0, MANAGER, "restRequestFailed", OFFLINE);
    }

    public void showDialog(String dialogType, Bundle parameters) {
        Bridge.sendLater(0, MANAGER, "dialogFailedWithError", OFFLINE);
    }
}
