package io.github.someoneisworking.lucent;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.SecureRandom;
import java.util.HashSet;
import java.util.Set;

/**
 * Shared, fail-closed Storage Access Framework import transaction.
 *
 * <p>This class owns persisted URI grants, exactly one picker/import at a time, bounded background
 * copying to app-private staging, and cancellation. A title decides which picker to show, validates
 * its own files, and publishes a validated result; Lucent never guesses a document's filesystem path
 * or knows a game's media format.</p>
 */
public final class LucentDocumentImport {
    public static final class Limits {
        public final int maximumEntries;
        public final long maximumBytes;
        public final int bufferBytes;

        public Limits(int maximumEntries, long maximumBytes, int bufferBytes) {
            if (maximumEntries <= 0 || maximumBytes <= 0 || bufferBytes < 4096) {
                throw new IllegalArgumentException("invalid document import limits");
            }
            this.maximumEntries = maximumEntries;
            this.maximumBytes = maximumBytes;
            this.bufferBytes = bufferBytes;
        }
    }

    public static final class Result {
        public final File stagingDirectory;
        public final String documentName;
        public final boolean isTree;

        private Result(File stagingDirectory, String documentName, boolean isTree) {
            this.stagingDirectory = stagingDirectory;
            this.documentName = documentName;
            this.isTree = isTree;
        }
    }

    public interface Callback {
        void onImported(Result result);
        void onCancelled();
        void onFailed(String message);
    }

    /**
     * Optional running account of a copy that is already under way.
     *
     * <p>A whole game installation over SAF is gigabytes and minutes, and the
     * copy owns the screen for all of it. Without this the consumer has
     * nothing true to say and shows a still screen, which reads as a hung
     * app. Lucent reports what it has copied; the words stay with the
     * consumer.</p>
     *
     * <p>Delivered on the Activity's main thread, at most every 500 ms. For a
     * document, {@code totalBytes} comes from its descriptor and permits a real
     * determinate bar. Tree providers may report zero when no total is available.
     * {@code bytes} and {@code entries} are what has been copied so far.</p>
     *
     * <p>A last update may arrive just after the import finished, because the
     * post that carries it was already in flight. Consumers that care should
     * ignore progress once {@link #active()} is false.</p>
     */
    public interface ProgressListener {
        void onProgress(long entries, long bytes, long totalBytes, String currentName);
    }

    /* Android notification managers commonly allow about five updates per second.  A 500 ms
       cadence leaves headroom for provider and lifecycle traffic while keeping the import visibly
       alive. */
    private static final long PROGRESS_INTERVAL_MILLIS = 500;
    private static final String STAGING_PREFIX = "lucent-import-";
    private static final String PREVIOUS_PREFIX = ".lucent-previous-";
    private static final String SOURCE_MARKER = ".lucent-source";
    private static final SecureRandom RANDOM = new SecureRandom();

    private final Activity activity;
    private final File storageRoot;
    private final Limits limits;
    private final LucentImportRequest<Callback> request = new LucentImportRequest<>();
    private boolean workerActive;
    private Thread worker;
    private ProgressListener progressListener;
    /* Worker-thread only: the copy is the sole writer. */
    private long lastProgressMillis;

    public LucentDocumentImport(Activity activity, Limits limits) {
        this(activity, activity == null ? null : activity.getFilesDir(), limits);
    }

    /** Uses a caller-owned persistent root, such as Android's package OBB directory. */
    public LucentDocumentImport(Activity activity, File storageRoot, Limits limits) {
        if (activity == null || storageRoot == null || limits == null) {
            throw new IllegalArgumentException("activity, storage root and limits are required");
        }
        this.activity = activity;
        this.storageRoot = storageRoot;
        this.limits = limits;
    }

    /** Set before starting an import; null removes a previous listener. */
    public synchronized void setProgressListener(ProgressListener listener) {
        this.progressListener = listener;
    }

    public synchronized boolean active() {
        return request.pending() || workerActive;
    }

    /** Save while the external picker is open, including Activity process recreation. */
    public synchronized Bundle savePickerState() {
        LucentImportRequest.Snapshot pending = request.snapshot();
        if (pending == null) return null;
        Bundle state = new Bundle();
        state.putInt("requestCode", pending.code);
        state.putBoolean("tree", pending.tree);
        return state;
    }

    /** Call from onCreate before Android delivers onActivityResult to the new Activity. */
    public synchronized boolean restorePickerState(Bundle state, Callback callback) {
        if (state == null) return false;
        if (active()) throw new IllegalStateException("an import is already active");
        if (!state.containsKey("requestCode") || !state.containsKey("tree")) {
            throw new IllegalArgumentException("incomplete picker state");
        }
        request.restore(new LucentImportRequest.Snapshot(
                state.getInt("requestCode"), state.getBoolean("tree")), callback);
        return true;
    }

    public synchronized void pickDocument(int requestCode, Callback callback) {
        begin(requestCode, callback, false);
    }

    public synchronized void pickTree(int requestCode, Callback callback) {
        begin(requestCode, callback, true);
    }

    private void begin(int requestCode, Callback callback, boolean tree) {
        if (callback == null) {
            throw new IllegalArgumentException("callback is required");
        }
        if (active()) {
            callback.onFailed("A game-file import is already active.");
            return;
        }
        Intent intent = new Intent(tree ? Intent.ACTION_OPEN_DOCUMENT_TREE : Intent.ACTION_OPEN_DOCUMENT);
        if (!tree) {
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        if (tree) {
            intent.addFlags(Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        }
        request.begin(requestCode, tree, callback);
        try {
            activity.startActivityForResult(intent, requestCode);
        } catch (ActivityNotFoundException error) {
            finishFailure("No Android document picker is available.");
        }
    }

    /** Returns true only if this controller owns the completed request. */
    public synchronized boolean handleActivityResult(int code, int resultCode, Intent data) {
        if (!request.accept(code)) {
            return false;
        }
        Uri source = resultCode == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (source == null) {
            finishCancelled();
            return true;
        }
        try {
            persistReadPermission(source, data.getFlags());
        } catch (SecurityException | IllegalArgumentException error) {
            finishFailure("Android could not retain access to the selected files.");
            return true;
        }
        workerActive = true;
        boolean isTree = request.tree();
        worker = new Thread(() -> importSelection(source, isTree), "lucent-document-import");
        worker.start();
        return true;
    }

    /** Cancels a picker or import when the owning Activity is finishing. */
    public synchronized void cancel() {
        if (!active()) {
            return;
        }
        if (worker != null) {
            worker.interrupt();
        }
        finishCancelled();
    }

    /** Removes abandoned staging only while no picker or import owns such a directory. */
    public synchronized void cleanStaleImports() {
        if (active()) {
            return;
        }
        File[] candidates = storageRoot.listFiles();
        if (candidates == null) {
            return;
        }
        for (File candidate : candidates) {
            if (candidate.getName().startsWith(STAGING_PREFIX)) {
                if (!new File(candidate, SOURCE_MARKER).isFile()) {
                    LucentImportPromotion.remove(candidate);
                }
            } else if (candidate.getName().startsWith(PREVIOUS_PREFIX)) {
                recoverPreviousSelection(candidate);
            }
        }
    }

    /**
     * Publishes a title-validated import under one app-private leaf name.
     *
     * <p>The title must validate {@code result} completely before calling this method. The old
     * selection remains intact until the staged directory is ready to replace it; an interrupted
     * replacement is recovered by {@link #cleanStaleImports()} on the next startup.</p>
     */
    public synchronized File promoteValidated(Result result, String destinationName) throws IOException {
        if (result == null) throw new IllegalArgumentException("import result is required");
        return promoteValidated(result, result.stagingDirectory, destinationName);
    }

    /** Publishes only a validated directory contained in this import (for nested archives). */
    public synchronized File promoteValidated(Result result, File selectedDirectory,
                                              String destinationName) throws IOException {
        validateLeafName(destinationName);
        File root = storageRoot.getCanonicalFile();
        File staging = validatedStaging(result, root);
        File marker = new File(staging, SOURCE_MARKER);
        if (marker.isFile() && !marker.delete()) {
            throw new IOException("cannot retire the resumable import marker");
        }
        File destination = privateChild(root, destinationName);
        File previous = privateChild(root, PREVIOUS_PREFIX + destinationName);
        return LucentImportPromotion.publish(staging, selectedDirectory, destination, previous);
    }

    /**
     * Discards a completed import that the title declined to validate.
     *
     * <p>A title calls this after its own identity or complete-install check fails. It accepts only
     * a still-private Lucent staging directory, so a rejected document can never delete the current
     * validated installation or an arbitrary app-private path.</p>
     */
    public synchronized void discard(Result result) throws IOException {
        if (result == null) {
            throw new IllegalArgumentException("import result is required");
        }
        if (active()) {
            throw new IOException("cannot discard an import while another import is active");
        }
        File root = storageRoot.getCanonicalFile();
        File staging = result.stagingDirectory.getCanonicalFile();
        if (!staging.getParentFile().equals(root) || !staging.getName().startsWith(STAGING_PREFIX)
                || !staging.isDirectory()) {
            throw new IOException("import staging is not a Lucent private directory");
        }
        if (!LucentImportPromotion.remove(staging)) {
            throw new IOException("cannot discard rejected import staging");
        }
    }

    /**
     * Discards the original selected document from a validated staging directory.
     *
     * <p>This is for archive importers that have already extracted and validated their retained
     * content under the same staging directory. It frees the archive before promotion without
     * weakening Lucent's all-or-nothing directory publication. Trees have no one source document
     * and cannot use this operation.</p>
     */
    public synchronized void discardValidatedDocument(Result result) throws IOException {
        if (result == null || result.isTree) {
            throw new IllegalArgumentException("only a staged document can be discarded");
        }
        validateLeafName(result.documentName);
        File root = storageRoot.getCanonicalFile();
        File staging = validatedStaging(result, root);
        File document = privateChild(staging, result.documentName);
        if (!document.isFile()) {
            throw new IOException("staged document is missing");
        }
        if (!document.delete()) {
            throw new IOException("cannot discard the validated source document");
        }
    }

    private void persistReadPermission(Uri source, int grantedFlags) {
        int flags = grantedFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION;
        if (flags == 0) {
            throw new IllegalArgumentException("picker did not grant read permission");
        }
        activity.getContentResolver().takePersistableUriPermission(source, flags);
    }

    private void importSelection(Uri source, boolean isTree) {
        File staging = null;
        try {
            String documentName = isTree ? "" : readDocumentName(source);
            if (!isTree) validateLeafName(documentName);
            staging = isTree ? createStaging() : findOrCreateResumableStaging(documentName, source);
            Budget budget = new Budget(limits);
            if (!isTree) budget.setTotalBytes(sourceSize(source));
            if (isTree) {
                copyTree(source, DocumentsContract.getTreeDocumentId(source), staging, budget);
            } else {
                budget.addEntry(-1);
                File target = new File(staging, documentName);
                long existing = target.isFile() ? target.length() : 0;
                budget.addBytes(existing);
                copyFile(source, target, budget, -1, existing);
            }
            File completedStaging = staging;
            String completedName = documentName;
            activity.runOnUiThread(() -> finishSuccess(new Result(completedStaging, completedName, isTree)));
        } catch (IOException | RuntimeException error) {
            // Android document providers are outside Lucent's control. Keep a
            // staged archive and its source marker so the next selection can
            // resume from its current length; the title discards rejected input.
            String detail = error.getMessage();
            activity.runOnUiThread(() -> finishFailure(
                    "Could not import the selected game files" + (detail == null ? "." : ": " + detail)));
        }
    }

    /**
     * Post one progress update, no more often than the interval allows.
     *
     * <p>Throttled here rather than in the consumer: an unthrottled report is
     * one main-thread post per buffer, which is tens of thousands of posts for
     * one install and makes the copy slower than the disk.</p>
     */
    private void noteProgress(String currentName, Budget budget) {
        final ProgressListener listener;
        synchronized (this) {
            listener = progressListener;
        }
        if (listener == null) {
            return;
        }
        long now = SystemClock.uptimeMillis();
        if (now - lastProgressMillis < PROGRESS_INTERVAL_MILLIS) {
            return;
        }
        lastProgressMillis = now;
        final long entries = budget.entries();
        final long bytes = budget.bytes();
        final long totalBytes = budget.totalBytes();
        activity.runOnUiThread(() -> listener.onProgress(entries, bytes, totalBytes, currentName));
    }

    private File findOrCreateResumableStaging(String documentName, Uri source) throws IOException {
        File root = storageRoot.getCanonicalFile();
        File[] candidates = root.listFiles();
        if (candidates != null) {
            for (File candidate : candidates) {
                if (!candidate.getName().startsWith(STAGING_PREFIX)) continue;
                File marker = new File(candidate, SOURCE_MARKER);
                if (!marker.isFile()) continue;
                String saved;
                try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(marker))) {
                    saved = reader.readLine();
                }
                if (saved != null && saved.equals(source.toString())
                        && new File(candidate, documentName).isFile()) {
                    return candidate;
                }
            }
        }
        File staging = createStaging();
        try {
            try (java.io.FileWriter writer = new java.io.FileWriter(new File(staging, SOURCE_MARKER))) {
                writer.write(source.toString());
            }
        } catch (IOException error) {
            LucentImportPromotion.remove(staging);
            throw error;
        }
        return staging;
    }

    private File createStaging() throws IOException {
        File root = storageRoot.getCanonicalFile();
        for (int attempt = 0; attempt < 16; ++attempt) {
            File staging = new File(root, STAGING_PREFIX + Long.toUnsignedString(RANDOM.nextLong(), 36));
            if (staging.mkdir()) {
                return staging;
            }
        }
        throw new IOException("cannot create private import staging");
    }

    private static File privateChild(File root, String name) throws IOException {
        File child = new File(root, name).getCanonicalFile();
        if (!child.getParentFile().equals(root)) {
            throw new IOException("private destination escapes the app data root");
        }
        return child;
    }

    private static File validatedStaging(Result result, File root) throws IOException {
        if (result == null) {
            throw new IllegalArgumentException("import result is required");
        }
        File staging = result.stagingDirectory.getCanonicalFile();
        if (!staging.getParentFile().equals(root) || !staging.getName().startsWith(STAGING_PREFIX)
                || !staging.isDirectory()) {
            throw new IOException("import staging is not a Lucent private directory");
        }
        return staging;
    }

    private void recoverPreviousSelection(File previous) {
        String destinationName = previous.getName().substring(PREVIOUS_PREFIX.length());
        try {
            validateLeafName(destinationName);
            File root = storageRoot.getCanonicalFile();
            File destination = privateChild(root, destinationName);
            if (destination.exists()) {
                if (!LucentImportPromotion.remove(previous)) {
                    throw new IOException("cannot retire an interrupted previous selection");
                }
            } else if (!previous.renameTo(destination)) {
                throw new IOException("cannot restore an interrupted previous selection");
            }
        } catch (IOException error) {
            throw new IllegalStateException("cannot recover interrupted Lucent import promotion", error);
        }
    }

    private void copyTree(Uri tree, String parentId, File destination, Budget budget) throws IOException {
        checkCancelled();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] columns = {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE,
        };
        Set<String> names = new HashSet<>();
        try (Cursor cursor = activity.getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) {
                throw new IOException("selected provider returned no folder contents");
            }
            while (cursor.moveToNext()) {
                checkCancelled();
                String id = cursor.getString(0);
                String name = cursor.getString(1);
                String mime = cursor.getString(2);
                long declaredSize = cursor.isNull(3) ? -1 : cursor.getLong(3);
                validateLeafName(name);
                if (!names.add(name)) {
                    throw new IOException("selected folder contains duplicate name: " + name);
                }
                budget.addEntry(declaredSize);
                noteProgress(name, budget);
                File target = new File(destination, name);
                Uri child = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (!target.mkdir()) {
                        throw new IOException("cannot create private directory " + name);
                    }
                    copyTree(tree, id, target, budget);
                } else {
                    copyFile(child, target, budget, declaredSize, 0);
                }
            }
        }
    }

    private void copyFile(Uri source, File target, Budget budget, long declaredSize, long resumeBytes)
            throws IOException {
        /* A provider's explicit size of zero means no bytes need reading.
         * Unknown sizes remain on the normal byte stream. */
        if (declaredSize == 0) {
            if (!target.createNewFile()) {
                throw new IOException("cannot create empty private file " + target.getName());
            }
            return;
        }
        try (InputStream input = openFile(source, target.getName());
             OutputStream output = new FileOutputStream(target, resumeBytes > 0)) {
            long skipped = 0;
            while (skipped < resumeBytes) {
                long step = input.skip(resumeBytes - skipped);
                if (step <= 0) {
                    if (input.read() < 0) throw new IOException("resumable source changed");
                    step = 1;
                }
                skipped += step;
            }
            byte[] buffer = new byte[limits.bufferBytes];
            for (int count; (count = input.read(buffer)) >= 0; ) {
                checkCancelled();
                if (count > 0) {
                    budget.addBytes(count);
                    output.write(buffer, 0, count);
                    noteProgress(target.getName(), budget);
                }
            }
        }
    }

    /**
     * Opens a document as an ordinary readable file descriptor.
     *
     * <p>The platform resolver owns provider acquisition, calling attribution,
     * and descriptor lifetime for the selected SAF URI.</p>
     */
    private InputStream openFile(Uri source, String displayName) throws IOException {
        try {
            ParcelFileDescriptor descriptor = activity.getContentResolver()
                    .openFileDescriptor(source, "r");
            if (descriptor == null) {
                throw new IOException("selected provider could not open " + displayName);
            }
            return new ParcelFileDescriptor.AutoCloseInputStream(descriptor);
        } catch (RuntimeException error) {
            throw new IOException("selected provider could not open " + displayName, error);
        }
    }

    private long sourceSize(Uri source) throws IOException {
        try (ParcelFileDescriptor descriptor = activity.getContentResolver().openFileDescriptor(source, "r")) {
            if (descriptor == null || descriptor.getStatSize() < 0) return 0;
            return descriptor.getStatSize();
        } catch (RuntimeException error) {
            throw new IOException("selected provider could not measure the document", error);
        }
    }

    private String readDocumentName(Uri source) throws IOException {
        try (Cursor cursor = activity.getContentResolver().query(
                source, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getString(0);
            }
        }
        String fallback = source.getLastPathSegment();
        if (fallback == null) {
            throw new IOException("selected provider did not provide a document name");
        }
        return fallback;
    }

    private static void validateLeafName(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) {
            throw new IOException("selected provider returned an unsafe file name");
        }
    }

    private static void checkCancelled() throws IOException {
        if (Thread.currentThread().isInterrupted()) {
            throw new IOException("import cancelled");
        }
    }

    private synchronized void finishSuccess(Result result) {
        if (!workerActive) {
            LucentImportPromotion.remove(result.stagingDirectory);
            return;
        }
        Callback completed = clearCallback();
        completed.onImported(result);
    }

    private synchronized void finishCancelled() {
        Callback completed = clearCallback();
        if (completed != null) {
            completed.onCancelled();
        }
    }

    private synchronized void finishFailure(String message) {
        Callback completed = clearCallback();
        if (completed != null) {
            completed.onFailed(message);
        }
    }

    private Callback clearCallback() {
        workerActive = false;
        worker = null;
        return request.complete();
    }


    private static final class Budget {
        private final Limits limits;
        private int entries;
        private long bytes;
        private long totalBytes;

        Budget(Limits limits) {
            this.limits = limits;
        }

        void addEntry(long declaredBytes) throws IOException {
            if (++entries > limits.maximumEntries) {
                throw new IOException("selection exceeds the entry limit");
            }
            if (declaredBytes > 0 && declaredBytes > limits.maximumBytes - bytes) {
                throw new IOException("selection exceeds the byte limit");
            }
        }

        int entries() {
            return entries;
        }

        long bytes() {
            return bytes;
        }

        long totalBytes() {
            return totalBytes;
        }

        void setTotalBytes(long total) {
            totalBytes = total > 0 ? total : 0;
        }

        void addBytes(long count) throws IOException {
            if (count > limits.maximumBytes - bytes) {
                throw new IOException("selection exceeds the byte limit");
            }
            bytes += count;
        }
    }
}
