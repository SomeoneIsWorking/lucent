package io.github.someoneisworking.lucent;

/** Main-thread lifetime of one foreground import, independent of Android transport. */
final class LucentImportLifetime<T> {
    interface Host<T> {
        void start(T progress);
        void update(T progress);
        void stop();
    }

    private final Host<T> host;
    private boolean active;

    LucentImportLifetime(Host<T> host) {
        this.host = host;
    }

    void start(T progress) {
        if (active) {
            update(progress);
            return;
        }
        host.start(progress);
        active = true;
    }

    void update(T progress) {
        if (active) host.update(progress);
    }

    void stop() {
        if (!active) return;
        host.stop();
        active = false;
    }
}
