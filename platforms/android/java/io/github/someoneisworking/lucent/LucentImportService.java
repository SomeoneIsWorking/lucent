package io.github.someoneisworking.lucent;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/** Non-exported dataSync service used only by LucentImportNotification. */
public final class LucentImportService extends Service {
    static final String NOTIFICATION_ID = "lucent.import.notificationId";
    static final String NOTIFICATION = "lucent.import.notification";

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        int notificationId = intent.getIntExtra(NOTIFICATION_ID, 0);
        Notification notification;
        if (Build.VERSION.SDK_INT >= 33) {
            notification = intent.getParcelableExtra(NOTIFICATION, Notification.class);
        } else {
            notification = intent.getParcelableExtra(NOTIFICATION);
        }
        if (notificationId <= 0 || notification == null) {
            throw new IllegalArgumentException("foreground import notification is missing");
        }
        // Promotion failures must propagate: a plain notification does not satisfy Android's
        // foreground-service contract and hiding the failure leaves a pending process kill.
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(notificationId, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(notificationId, notification);
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(true);
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
