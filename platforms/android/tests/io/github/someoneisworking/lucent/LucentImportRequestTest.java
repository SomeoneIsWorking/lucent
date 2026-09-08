package io.github.someoneisworking.lucent;

public final class LucentImportRequestTest {
    private static void check(boolean value) {
        if (!value) throw new AssertionError();
    }

    public static void main(String[] args) {
        Object oldActivity = new Object();
        Object newActivity = new Object();
        LucentImportRequest<Object> old = new LucentImportRequest<>();
        old.begin(42, true, oldActivity);
        LucentImportRequest.Snapshot saved = old.snapshot();
        check(saved != null && saved.code == 42 && saved.tree);
        LucentImportRequest<Object> recreated = new LucentImportRequest<>();
        recreated.restore(new LucentImportRequest.Snapshot(saved.code, saved.tree), newActivity);
        check(!recreated.accept(41) && recreated.pending());
        check(recreated.accept(42) && recreated.tree());
        check(!recreated.accept(42) && recreated.snapshot() == null);
        check(recreated.complete() == newActivity);
        check(recreated.complete() == null);
        check(old.complete() == oldActivity);
        recreated.begin(43, false, newActivity);
        check(!recreated.tree());
        check(recreated.complete() == newActivity);
        check(!recreated.pending() && recreated.snapshot() == null);
        check(!recreated.accept(43));
        recreated.restore(new LucentImportRequest.Snapshot(44, false), newActivity);
        try {
            recreated.restore(saved, oldActivity);
            throw new AssertionError("active request replaced");
        } catch (IllegalStateException expected) {
            check(recreated.accept(44));
            check(recreated.complete() == newActivity);
        }
        System.out.println("Import request: recreated picker rebinds only the new Activity; wrong/duplicate/cancelled results refused");
    }
}
