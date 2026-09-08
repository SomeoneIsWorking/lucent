package io.github.someoneisworking.lucent;

import java.util.ArrayList;
import java.util.List;

public final class LucentImportLifetimeTest {
    private static final class Host implements LucentImportLifetime.Host<String> {
        final List<String> events = new ArrayList<>();
        boolean refuseStart;

        @Override
        public void start(String progress) {
            if (refuseStart) throw new IllegalStateException("foreground start refused");
            events.add("start:" + progress);
        }

        @Override
        public void update(String progress) {
            events.add("update:" + progress);
        }

        @Override
        public void stop() {
            events.add("stop");
        }
    }

    private static void expect(Host host, String... events) {
        if (!host.events.equals(List.of(events))) {
            throw new AssertionError("Expected " + List.of(events) + ", got " + host.events);
        }
    }

    private static void idleAndLateCallbacksCannotStartService() {
        Host host = new Host();
        LucentImportLifetime<String> lifetime = new LucentImportLifetime<>(host);
        lifetime.stop(); // Initial choices screen, no prior import.
        lifetime.update("late");
        lifetime.stop(); // Setup Activity finishes on an already installed launch.
        expect(host);
    }

    private static void handoffAndDestroyStopOnlyTheExistingImport() {
        Host host = new Host();
        LucentImportLifetime<String> lifetime = new LucentImportLifetime<>(host);
        lifetime.start("initial"); // Picker returns.
        lifetime.start("resume"); // Activity.onResume redisplays the same import.
        lifetime.update("progress");
        lifetime.stop(); // Validated install starts game.
        lifetime.stop(); // Setup Activity.onDestroy follows.
        lifetime.update("late"); // Previously posted copy callback arrives.
        expect(host, "start:initial", "update:resume", "update:progress", "stop");
    }

    private static void cancellationCanPrecedeServiceCreationAndNewImportCanStart() {
        Host host = new Host();
        LucentImportLifetime<String> lifetime = new LucentImportLifetime<>(host);
        lifetime.start("queued");
        lifetime.stop(); // No dependency on an active Android Service instance.
        lifetime.start("next");
        lifetime.stop();
        expect(host, "start:queued", "stop", "start:next", "stop");
    }

    private static void foregroundFailurePropagatesWithoutActiveLifetime() {
        Host host = new Host();
        LucentImportLifetime<String> lifetime = new LucentImportLifetime<>(host);
        host.refuseStart = true;
        boolean refused = false;
        try {
            lifetime.start("denied");
        } catch (IllegalStateException expected) {
            refused = true;
        }
        if (!refused) throw new AssertionError("foreground failure was swallowed");
        lifetime.update("late");
        lifetime.stop();
        expect(host);
        host.refuseStart = false;
        lifetime.start("accepted");
        lifetime.stop();
        expect(host, "start:accepted", "stop");
    }

    public static void main(String[] args) {
        idleAndLateCallbacksCannotStartService();
        handoffAndDestroyStopOnlyTheExistingImport();
        cancellationCanPrecedeServiceCreationAndNewImportCanStart();
        foregroundFailurePropagatesWithoutActiveLifetime();
        System.out.println("Lucent import lifetime: 4 scenarios passed (idle, handoff, cancel, refusal)");
    }
}
