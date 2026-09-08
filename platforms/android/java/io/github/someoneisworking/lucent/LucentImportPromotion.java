package io.github.someoneisworking.lucent;

import java.io.File;
import java.io.IOException;

/** Filesystem publication of a validated private import, including a selected nested directory. */
final class LucentImportPromotion {
    private LucentImportPromotion() {}

    static File publish(File staging, File selected, File destination, File previous) throws IOException {
        File stage = staging.getCanonicalFile();
        File candidate = selected.getCanonicalFile();
        File ancestor = candidate;
        while (ancestor != null && !ancestor.equals(stage)) ancestor = ancestor.getParentFile();
        if (ancestor == null || !candidate.isDirectory()) {
            throw new IOException("validated selection is not a directory inside its private import");
        }
        if (previous.exists()) throw new IOException("previous selection recovery is pending");
        boolean hadPrevious = destination.exists();
        if (hadPrevious && !destination.renameTo(previous)) {
            throw new IOException("cannot preserve the current validated selection");
        }
        if (!candidate.renameTo(destination)) {
            if (hadPrevious && !previous.renameTo(destination)) {
                throw new IOException("cannot publish the import or restore the previous selection");
            }
            throw new IOException("cannot publish the validated import");
        }
        if (!candidate.equals(stage) && !remove(stage)) {
            throw new IOException("published import, but could not retire its staging wrapper");
        }
        if (hadPrevious && !remove(previous)) {
            throw new IOException("published import, but could not retire the previous selection");
        }
        return destination;
    }

    static boolean remove(File file) {
        if (!file.exists()) return true;
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                if (!remove(child)) return false;
            }
        }
        return file.delete();
    }
}
