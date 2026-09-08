package io.github.someoneisworking.lucent;

import android.app.Notification;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

/**
 * Main-thread foreground protection for one import. The title supplies the notification and
 * channel, calls start while its Activity is visible, and stops after validation or cancellation.
 * Updates and repeated stop calls never start a service, including after the Activity finishes.
 * This owns foreground lifetime, not the import worker or title validation.
 */
public final class LucentImportNotification {
    private final LucentImportLifetime<Notification> lifetime;

    public LucentImportNotification(Context context, int notificationId) {
        if (notificationId <= 0) throw new IllegalArgumentException("notification ID must be positive");
        Context app = context.getApplicationContext();
        NotificationManager manager =
                (NotificationManager) app.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager == null) throw new IllegalStateException("Android notification service is missing");
        lifetime = new LucentImportLifetime<>(new LucentImportLifetime.Host<Notification>() {
            @Override
            public void start(Notification progress) {
                Intent intent = new Intent(app, LucentImportService.class);
                intent.putExtra(LucentImportService.NOTIFICATION_ID, notificationId);
                intent.putExtra(LucentImportService.NOTIFICATION, progress);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    app.startForegroundService(intent);
                } else {
                    app.startService(intent);
                }
            }

            @Override
            public void update(Notification progress) {
                manager.notify(notificationId, progress);
            }

            @Override
            public void stop() {
                app.stopService(new Intent(app, LucentImportService.class));
                manager.cancel(notificationId);
            }
        });
    }

    public void start(Notification progress) {
        lifetime.start(progress);
    }

    public void update(Notification progress) {
        lifetime.update(progress);
    }

    public void stop() {
        lifetime.stop();
    }
}
