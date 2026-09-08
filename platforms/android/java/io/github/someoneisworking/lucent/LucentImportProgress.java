package io.github.someoneisworking.lucent;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

/** Persistent import progress presentation; titles provide identity, wording and tap destination. */
public final class LucentImportProgress {
    private final Context context;
    private final String channelId;
    private final String title;
    private final Class<? extends Activity> destination;
    private final LucentImportNotification notification;

    public LucentImportProgress(Context context, int notificationId, String channelId,
                                String channelName, String title, Class<? extends Activity> destination) {
        this.context = context.getApplicationContext();
        this.channelId = channelId;
        this.title = title;
        this.destination = destination;
        notification = new LucentImportNotification(context, notificationId);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManager manager =
                    (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
            NotificationChannel channel = new NotificationChannel(
                    channelId, channelName, NotificationManager.IMPORTANCE_LOW);
            channel.setShowBadge(false);
            manager.createNotificationChannel(channel);
        }
    }

    public void start(String progress) { notification.start(build(progress)); }
    public void update(String progress) { notification.update(build(progress)); }
    public void stop() { notification.stop(); }

    private Notification build(String progress) {
        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(context, channelId) : new Notification.Builder(context);
        Intent tap = new Intent(context, destination);
        tap.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) flags |= PendingIntent.FLAG_IMMUTABLE;
        return builder.setContentTitle(title)
                .setContentText(progress)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true)
                // SAF enumerates as it copies, so a percentage would invent an unknown total.
                .setProgress(0, 0, true)
                .setContentIntent(PendingIntent.getActivity(context, 0, tap, flags))
                .build();
    }
}
