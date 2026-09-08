package io.github.someoneisworking.lucent;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

public final class LucentImportPromotionTest {
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }

    private static File directory(File root, String name) throws IOException {
        File path = new File(root, name);
        Files.createDirectories(path.toPath());
        return path;
    }

    private static void content(File path, String value) throws IOException {
        Files.writeString(new File(path, "content").toPath(), value);
    }

    private static String content(File path) throws IOException {
        return Files.readString(new File(path, "content").toPath());
    }

    private static void rejected(File stage, File selected, File game, File previous) throws IOException {
        boolean refused = false;
        try {
            LucentImportPromotion.publish(stage, selected, game, previous);
        } catch (IOException expected) {
            refused = true;
        }
        check(refused, "unsafe/invalid selection was accepted");
        check(content(game).equals("previous"), "rejection modified prior install");
    }

    public static void main(String[] args) throws IOException {
        if (args.length != 1) throw new IllegalArgumentException("one build-local test directory required");
        File root = new File(args[0]);
        check(LucentImportPromotion.remove(root), "stale test output could not be removed");
        check(root.mkdirs(), "could not create test output");
        try {
            File stage = directory(root, "staging");
            File game = directory(root, "game");
            File previous = new File(root, "backup");
            content(stage, "whole");
            content(game, "previous");
            LucentImportPromotion.publish(stage, stage, game, previous);
            check(content(game).equals("whole"), "whole-directory contract regressed");
            check(!stage.exists() && !previous.exists(), "whole promotion left stale directories");

            stage = directory(root, "staging");
            File nested = directory(stage, "archive/nested/title");
            content(nested, "nested");
            Files.writeString(new File(stage, "installer.exe").toPath(), "source document");
            LucentImportPromotion.publish(stage, nested, game, previous);
            check(content(game).equals("nested"), "nested directory was not published");
            check(!stage.exists() && !previous.exists(), "nested promotion retained source/wrapper");

            stage = directory(root, "staging");
            content(game, "previous");
            File outside = directory(root, "outside");
            content(outside, "outside");
            rejected(stage, outside, game, previous);
            rejected(stage, new File(stage, "missing"), game, previous);
            File file = new File(stage, "file");
            Files.writeString(file.toPath(), "not directory");
            rejected(stage, file, game, previous);
            previous.mkdir();
            rejected(stage, stage, game, previous);
            System.out.println("Lucent import publication: whole/nested and four refusals passed");
        } finally {
            check(LucentImportPromotion.remove(root), "test output cleanup failed");
        }
    }
}
