package io.github.someoneisworking.lucent;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
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

    private static final String STAGING_PREFIX = "lucent-import-";
    private static final String PREVIOUS_PREFIX = ".lucent-previous-";
    private static final SecureRandom RANDOM = new SecureRandom();

    private final Activity activity;
    private final Limits limits;
    private Callback callback;
    private int requestCode;
    private boolean treeRequest;
    private boolean pickerOpen;
    private boolean workerActive;
    private Thread worker;

    public LucentDocumentImport(Activity activity, Limits limits) {
        if (activity == null || limits == null) {
            throw new IllegalArgumentException("activity and limits are required");
        }
        this.activity = activity;
        this.limits = limits;
    }

    public synchronized boolean active() {
        return pickerOpen || workerActive;
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
        this.callback = callback;
        this.requestCode = requestCode;
        this.treeRequest = tree;
        this.pickerOpen = true;
        try {
            activity.startActivityForResult(intent, requestCode);
        } catch (ActivityNotFoundException error) {
            finishFailure("No Android document picker is available.");
        }
    }

    /** Returns true only if this controller owns the completed request. */
    public synchronized boolean handleActivityResult(int code, int resultCode, Intent data) {
        if (!pickerOpen || code != requestCode) {
            return false;
        }
        pickerOpen = false;
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
        boolean isTree = treeRequest;
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
        File[] candidates = activity.getFilesDir().listFiles();
        if (candidates == null) {
            return;
        }
        for (File candidate : candidates) {
            if (candidate.getName().startsWith(STAGING_PREFIX)) {
                deleteRecursively(candidate);
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
        validateLeafName(destinationName);
        File root = activity.getFilesDir().getCanonicalFile();
        File staging = validatedStaging(result, root);
        File destination = privateChild(root, destinationName);
        File previous = privateChild(root, PREVIOUS_PREFIX + destinationName);
        if (previous.exists()) {
            throw new IOException("previous selection recovery is pending");
        }
        boolean hadPrevious = destination.exists();
        if (hadPrevious && !destination.renameTo(previous)) {
            throw new IOException("cannot preserve the current validated selection");
        }
        if (!staging.renameTo(destination)) {
            if (hadPrevious && !previous.renameTo(destination)) {
                throw new IOException("cannot publish the import or restore the previous selection");
            }
            throw new IOException("cannot publish the validated import");
        }
        if (hadPrevious && !deleteRecursively(previous)) {
            throw new IOException("published import, but could not retire the previous selection");
        }
        return destination;
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
        File root = activity.getFilesDir().getCanonicalFile();
        File staging = result.stagingDirectory.getCanonicalFile();
        if (!staging.getParentFile().equals(root) || !staging.getName().startsWith(STAGING_PREFIX)
                || !staging.isDirectory()) {
            throw new IOException("import staging is not a Lucent private directory");
        }
        if (!deleteRecursively(staging)) {
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
        File root = activity.getFilesDir().getCanonicalFile();
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
            staging = createStaging();
            Budget budget = new Budget(limits);
            String documentName = "";
            if (isTree) {
                copyTree(source, DocumentsContract.getTreeDocumentId(source), staging, budget);
            } else {
                documentName = readDocumentName(source);
                validateLeafName(documentName);
                budget.addEntry(-1);
                copyFile(source, new File(staging, documentName), budget, -1);
            }
            File completedStaging = staging;
            String completedName = documentName;
            activity.runOnUiThread(() -> finishSuccess(new Result(completedStaging, completedName, isTree)));
        } catch (IOException | RuntimeException error) {
            // Android document providers are outside Lucent's control. Some
            // providers throw unchecked exceptions from openInputStream()
            // instead of returning a null stream or IOException. This is the
            // import boundary: discard partial staging and report failure to
            // the Activity rather than leaving the app process dead.
            if (staging != null) {
                deleteRecursively(staging);
            }
            String detail = error.getMessage();
            activity.runOnUiThread(() -> finishFailure(
                    "Could not import the selected game files" + (detail == null ? "." : ": " + detail)));
        }
    }

    private File createStaging() throws IOException {
        File root = activity.getFilesDir().getCanonicalFile();
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
            File root = activity.getFilesDir().getCanonicalFile();
            File destination = privateChild(root, destinationName);
            if (destination.exists()) {
                if (!deleteRecursively(previous)) {
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
                File target = new File(destination, name);
                Uri child = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (!target.mkdir()) {
                        throw new IOException("cannot create private directory " + name);
                    }
                    copyTree(tree, id, target, budget);
                } else {
                    copyFile(child, target, budget, declaredSize);
                }
            }
        }
    }

    private void copyFile(Uri source, File target, Budget budget, long declaredSize)
            throws IOException {
        /* A provider's explicit size of zero means no bytes need reading. It
         * is also the only safe escape from Waydroid's broken zero-length
         * document open path; unknown sizes remain on the normal byte stream. */
        if (declaredSize == 0) {
            if (!target.createNewFile()) {
                throw new IOException("cannot create empty private file " + target.getName());
            }
            return;
        }
        try (InputStream input = openFile(source, target.getName());
             OutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[limits.bufferBytes];
            for (int count; (count = input.read(buffer)) >= 0; ) {
                checkCancelled();
                if (count > 0) {
                    budget.addBytes(count);
                    output.write(buffer, 0, count);
                }
            }
        }
    }

    /**
     * Opens a document as an ordinary readable file descriptor.
     *
     * <p>{@link android.content.ContentResolver#openInputStream(Uri)} takes
     * Android's typed-asset path. The Waydroid DocumentsUI provider throws
     * from that path for ordinary tree children even though it can provide a
     * file descriptor. Staging only needs bytes, not MIME conversion, so use
     * the provider's plain file contract.</p>
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
            deleteRecursively(result.stagingDirectory);
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
        pickerOpen = false;
        workerActive = false;
        worker = null;
        Callback completed = callback;
        callback = null;
        return completed;
    }

    private static boolean deleteRecursively(File file) {
        if (!file.exists()) {
            return true;
        }
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                if (!deleteRecursively(child)) {
                    return false;
                }
            }
        }
        return file.delete();
    }

    private static final class Budget {
        private final Limits limits;
        private int entries;
        private long bytes;

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

        void addBytes(int count) throws IOException {
            if (count > limits.maximumBytes - bytes) {
                throw new IOException("selection exceeds the byte limit");
            }
            bytes += count;
        }
    }
}
