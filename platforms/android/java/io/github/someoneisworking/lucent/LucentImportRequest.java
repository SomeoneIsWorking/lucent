package io.github.someoneisworking.lucent;

/** Pending picker identity and callback binding, independent of an Activity instance. */
public final class LucentImportRequest<C> {
    public static final class Snapshot {
        public final int code;
        public final boolean tree;

        public Snapshot(int code, boolean tree) {
            this.code = code;
            this.tree = tree;
        }
    }

    private Snapshot identity;
    private C callback;
    private boolean pending;

    public void begin(int code, boolean tree, C callback) {
        if (callback == null) throw new IllegalArgumentException("callback is required");
        if (this.callback != null) throw new IllegalStateException("an import request is already active");
        identity = new Snapshot(code, tree);
        this.callback = callback;
        pending = true;
    }

    public boolean pending() { return pending; }

    public Snapshot snapshot() { return pending ? identity : null; }

    /** Rebind only serializable request identity; the callback belongs to the new Activity. */
    public void restore(Snapshot snapshot, C callback) {
        if (snapshot == null) throw new IllegalArgumentException("request snapshot is required");
        begin(snapshot.code, snapshot.tree, callback);
    }

    public boolean accept(int code) {
        if (!pending || code != identity.code) return false;
        pending = false;
        return true;
    }

    public boolean tree() {
        if (identity == null) throw new IllegalStateException("no import request");
        return identity.tree;
    }

    public C complete() {
        C completed = callback;
        callback = null;
        identity = null;
        pending = false;
        return completed;
    }
}
