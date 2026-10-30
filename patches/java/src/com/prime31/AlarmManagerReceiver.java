package com.prime31;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

/**
 * Posts the game's scheduled reminder, matching the original receiver: if the game is in
 * the foreground the notification is forwarded to Unity instead; otherwise a notification
 * (id 100, auto-cancel, app icon, title/subtitle/ticker from the game) opens the game with
 * the original "notificationData" extra. Uses a notification channel and immutable
 * PendingIntents, which current Android requires.
 */
public class AlarmManagerReceiver extends BroadcastReceiver {
    private static final String CHANNEL_ID = "reminders";
    private static final int NOTIFICATION_ID = 100;

    @Override
    public void onReceive(Context context, Intent intent) {
        Bundle extras = intent.getExtras();
        if (extras == null) return;
        String data = extras.getString("data");

        if (EtceteraPlugin.instance().receivedNotification(data)) return;

        Intent open = new Intent();
        open.setComponent(new ComponentName(context.getPackageName(), extras.getString("contextClassName")));
        open.putExtra("notificationData", data);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        PendingIntent contentIntent = PendingIntent.getActivity(context, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL_ID, "Reminders", NotificationManager.IMPORTANCE_DEFAULT));

        Notification notification = new Notification.Builder(context, CHANNEL_ID)
                .setSmallIcon(context.getApplicationInfo().icon)
                .setTicker(extras.getString("tickerText"))
                .setWhen(System.currentTimeMillis())
                .setContentTitle(extras.getString("title"))
                .setContentText(extras.getString("subtitle"))
                .setContentIntent(contentIntent)
                .setAutoCancel(true)
                .build();
        manager.notify(NOTIFICATION_ID, notification);
    }
}
