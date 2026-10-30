package com.prime31;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Random;

/**
 * Offline stand-in for Prime31's Etcetera plugin.
 *
 * The game only uses local notifications (the "come back and play" reminder) and the
 * rate-this-app prompt. Notifications work as in the original: an inexact alarm fires
 * AlarmManagerReceiver, which posts the reminder, or forwards notificationReceived to Unity
 * if the game is in the foreground. The rate-app prompt is inert (its Play Store page is
 * gone). Every other method of the original exists as a harmless no-op so the method set
 * is identical.
 */
public class EtceteraPlugin {
    static final String MANAGER = "EtceteraAndroidManager";
    private static EtceteraPlugin instance;

    public static synchronized EtceteraPlugin instance() {
        if (instance == null) instance = new EtceteraPlugin();
        return instance;
    }

    // ---- Local notifications (kept) ----

    public int scheduleNotification(long secondsFromNow, String title, String subtitle, String tickerText, String extraData) {
        Activity activity = Bridge.activity();
        if (activity == null) return 0;

        Intent intent = new Intent(activity, AlarmManagerReceiver.class);
        intent.putExtra("title", title);
        intent.putExtra("subtitle", subtitle);
        intent.putExtra("tickerText", tickerText);
        intent.putExtra("data", extraData);
        intent.putExtra("contextClassName", activity.getClass().getName());

        int requestCode = new Random().nextInt(Integer.MAX_VALUE);
        PendingIntent pending = PendingIntent.getBroadcast(activity, requestCode, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        AlarmManager alarms = (AlarmManager) activity.getSystemService(Context.ALARM_SERVICE);
        alarms.set(AlarmManager.RTC, System.currentTimeMillis() + secondsFromNow * 1000L, pending);
        return requestCode;
    }

    public void cancelNotification(int notificationId) {
        Activity activity = Bridge.activity();
        if (activity == null) return;

        Intent intent = new Intent(activity, AlarmManagerReceiver.class);
        PendingIntent pending = PendingIntent.getBroadcast(activity, notificationId, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        ((AlarmManager) activity.getSystemService(Context.ALARM_SERVICE)).cancel(pending);
    }

    public void checkForNotifications() {
        Activity activity = Bridge.activity();
        if (activity == null || activity.getIntent() == null) return;
        Bundle extras = activity.getIntent().getExtras();
        if (extras != null && extras.containsKey("notificationData")) {
            receivedNotification(extras.getString("notificationData"));
        }
    }

    /** Forwards the notification to Unity when the game is in front; returns whether it did. */
    public boolean receivedNotification(String extraData) {
        Activity activity = Bridge.activity();
        if (activity != null && activity.hasWindowFocus()) {
            Bridge.send(MANAGER, "notificationReceived", extraData);
            return true;
        }
        return false;
    }

    // ---- Rate this app (inert: the store listing no longer exists) ----

    public void askForReview(int launchesUntilPrompt, int hoursUntilFirstPrompt, int hoursBetweenPrompts, String title, String message) {}

    public void askForReviewNow(String title, String message) {}

    public void resetAskForReview() {}

    // ---- Everything else the original offered; unused by the game ----

    public static void copyFile(File source, File destination) throws IOException {
        try (InputStream in = new FileInputStream(source); OutputStream out = new FileOutputStream(destination)) {
            byte[] buffer = new byte[8192];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        }
    }

    public int getScreenDensity() {
        Activity activity = Bridge.activity();
        return activity == null ? 0 : activity.getResources().getDisplayMetrics().densityDpi;
    }

    public void showToast(String text, boolean useShortDuration) {}

    public void showAlert(String title, String message, String positiveButton, String negativeButton) {}

    public void showAlertPrompt(String title, String message, String promptHint, String promptText, String positiveButton, String negativeButton) {}

    public void showAlertPromptWithTwoFields(String title, String message, String promptHintOne, String promptTextOne,
                                             String promptHintTwo, String promptTextTwo, String positiveButton, String negativeButton) {}

    public void showProgressDialog(String title, String message) {}

    public void hideProgressDialog() {}

    public void showWebView(String url) {}

    public void showCustomWebView(String url, boolean disableTitle, boolean disableBackButton) {}

    public void inlineWebViewShow(String url, int x, int y, int width, int height) {}

    public void inlineWebViewClose() {}

    public void inlineWebViewSetUrl(String url) {}

    public void inlineWebViewSetFrame(int x, int y, int width, int height) {}

    public void showEmailComposer(String toAddress, String subject, String text, boolean isHTML, String attachmentFilePath) {}

    public boolean isSMSComposerAvailable() {
        return false;
    }

    public void showSMSComposer(String to, String message) {}

    public void promptToTakePhoto(int desiredWidth, int desiredHeight, String name) {}

    public void promptForPictureFromAlbum(int desiredWidth, int desiredHeight, String name) {}

    public void promptToTakeVideo(String name) {}

    public void cameraPhotoTaken(String path) {}

    public void photoAlbumItemChosen(String path) {}

    public void videoTaken(String path) {}

    public boolean saveImageToGallery(String pathToPhoto, String title) {
        return false;
    }

    public void scaleImageAtPath(String pathToImage, float scale) {}

    public void playMovie(String pathOrUrl, int bgColor, boolean showControls, int scalingMode, boolean closeOnTouch) {}

    public void setSystemUiVisibilityToLowProfile(boolean useLowProfile) {}

    public void initTTS() {}

    public void teardownTTS() {}

    public void speak(String text, int queueMode) {}

    public void stop() {}

    public void playSilence(long durationInMs, int queueMode) {}

    public void setPitch(float pitch) {}

    public void setSpeechRate(float rate) {}

    public void onInit(int status) {}

    public void onUtteranceCompleted(String utteranceId) {}
}
