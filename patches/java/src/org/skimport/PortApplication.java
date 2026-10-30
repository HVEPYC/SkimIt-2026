package org.skimport;

import android.app.Activity;
import android.app.Application;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * The port's Application class (added by scripts/modernize_manifest.py; no original code changed).
 * Keeps every game window in immersive full screen: status and navigation/gesture bars hidden,
 * shown briefly by a swipe from the edge, as modern full-screen games do. The original only hid
 * the status bar (hide_status_bar); 2013 devices had no gesture bar over the game.
 */
public final class PortApplication extends Application implements Application.ActivityLifecycleCallbacks {
    @Override
    public void onCreate() {
        super.onCreate();
        registerActivityLifecycleCallbacks(this);
    }

    static void immersive(Window w) {
        if (Build.VERSION.SDK_INT >= 30) {
            // Draw into the camera cutout too (otherwise a black band stays at the top).
            WindowManager.LayoutParams lp = w.getAttributes();
            if (lp.layoutInDisplayCutoutMode != WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS) {
                lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
                w.setAttributes(lp);
            }
            WindowInsetsController c = w.getInsetsController();
            if (c == null) return;
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    public void onActivityCreated(Activity a, Bundle saved) {
        final Window w = a.getWindow();
        // Dialogs and returning from Home give focus back: re-apply.
        w.getDecorView().getViewTreeObserver().addOnWindowFocusChangeListener(new ViewTreeObserver.OnWindowFocusChangeListener() {
            @Override
            public void onWindowFocusChanged(boolean hasFocus) {
                if (hasFocus) immersive(w);
            }
        });
    }

    @Override
    public void onActivityResumed(Activity a) {
        immersive(a.getWindow());
        askForNotifications(a);
    }

    /**
     * The game schedules a local reminder (EtceteraPlugin.scheduleNotification). Android 13+ only
     * shows it after a runtime permission grant, which a 2013 app never asked for, so the port asks
     * once, on the game's own activity (the proxy activity finishes immediately).
     */
    private void askForNotifications(Activity a) {
        if (Build.VERSION.SDK_INT < 33 || !"com.unity3d.player.UnityPlayerActivity".equals(a.getClass().getName())) return;
        if (a.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) == android.content.pm.PackageManager.PERMISSION_GRANTED) return;
        android.content.SharedPreferences prefs = getSharedPreferences("skimport", MODE_PRIVATE);
        if (prefs.getBoolean("asked_notifications", false)) return;
        prefs.edit().putBoolean("asked_notifications", true).apply();
        a.requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 31);
    }
    @Override public void onActivityStarted(Activity a) {}
    @Override public void onActivityPaused(Activity a) {}
    @Override public void onActivityStopped(Activity a) {}
    @Override public void onActivitySaveInstanceState(Activity a, Bundle out) {}
    @Override public void onActivityDestroyed(Activity a) {}
}
