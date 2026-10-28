package io.github.vitomiguelf.crashottde;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

// The app's first screen: the launcher (launcher/launcher.cpp as libcrashlauncher.so) on SDL3's
// glue, in a process of its own (AndroidManifest.xml, ":launcher"), so the game later starts in a
// fresh one. PLAY calls startGame from the launcher's thread.
//
// The dump comes in here too: the launcher asks for it (importGame) while the app's game folder
// (game/ in the external files folder) has none. The player picks the dump's folder in Android's
// folder picker (the storage access framework: no storage permission); it is checked as the PC
// installer checks it (the files the launcher names, default.xex's SHA-256) and copied into game/.
// The launcher draws the progress from the import* getters.
//
// The language packs (languages/ of the project, in the APK's assets) are copied into the files
// folder at each start, as the PC's make_dist.py copies them next to the exe.
public class LauncherActivity extends SDLActivity {
    private static final String TAG = "CrashLauncher";
    private static final int PICK_GAME = 0x4352;

    // Import states and errors (launcher.cpp, Import*): the launcher words the errors, with
    // importDetail (the missing files, the digest, the sizes, the exception).
    public static final int IMPORT_IDLE = 0, IMPORT_PICKING = 1, IMPORT_CHECKING = 2, IMPORT_COPYING = 3, IMPORT_DONE = 4,
                            IMPORT_FAILED = 5, IMPORT_CANCELLED = 6;
    public static final int ERROR_NONE = 0, ERROR_NOT_GAME = 1, ERROR_INCOMPLETE = 2, ERROR_VERSION = 3, ERROR_SPACE = 4, ERROR_IO = 5,
                            ERROR_PICKER = 6;

    private static volatile int importState = IMPORT_IDLE, importError = ERROR_NONE;
    private static volatile long importDone, importTotal;
    private static volatile String importFile = "", importDetail = "";
    private static volatile boolean importCancel;
    private static volatile String[] importRequired = new String[0];
    private static volatile String importSha256 = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The app's icon opens this activity's task, not the game's (CrashActivity, a task of its
        // own): with the game still running in the background, back to it instead of a launcher.
        if (bringGameToFront()) {
            super.onCreate(savedInstanceState);
            finishAndRemoveTask();
            return;
        }
        try {
            copyAssets(getAssets(), "languages", new File(getExternalFilesDir(null), "languages"));
        } catch (IOException e) {
            Log.w(TAG, "language packs not copied", e);
        }
        super.onCreate(savedInstanceState);
    }

    private boolean bringGameToFront() {
        ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        for (ActivityManager.AppTask task : manager.getAppTasks()) {
            ActivityManager.RecentTaskInfo info = task.getTaskInfo();
            ComponentName top = info.topActivity;
            if (info.isRunning && top != null && top.getClassName().equals(CrashActivity.class.getName())) {
                Log.i(TAG, "the game is running: back to it");
                task.moveToFront();
                return true;
            }
        }
        return false;
    }

    // A folder of the assets into `target`; a file of the same size is kept.
    private static void copyAssets(AssetManager assets, String path, File target) throws IOException {
        String[] names = assets.list(path);
        if (names == null)
            return;
        for (String name : names) {
            String child = path + "/" + name;
            File out = new File(target, name);
            String[] inside = assets.list(child);
            if (inside != null && inside.length > 0) {
                copyAssets(assets, child, out);
                continue;
            }
            try (InputStream in = assets.open(child)) {
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                byte[] buffer = new byte[16384];
                int n;
                while ((n = in.read(buffer)) > 0)
                    bytes.write(buffer, 0, n);
                byte[] data = bytes.toByteArray();
                if (out.isFile() && out.length() == data.length)
                    continue;
                if (!target.isDirectory() && !target.mkdirs())
                    throw new IOException("can't make " + target);
                try (OutputStream os = new FileOutputStream(out)) {
                    os.write(data);
                }
            }
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "crashlauncher" };
    }

    // From native code (launcher.cpp StartGame): the game, CrashActivity, in the app's main process.
    // The launcher's task leaves the recent apps: the game's is the one to come back to.
    public static void startGame() {
        Context context = SDL.getContext();
        Intent intent = new Intent(context, CrashActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        context.startActivity(intent);
        if (context instanceof Activity) {
            Activity activity = (Activity) context;
            activity.runOnUiThread(activity::finishAndRemoveTask);
        }
    }

    // From native code: the launcher again in a new process (it reads the game's art at startup).
    // The activity is started while this one is in front, then this process ends; Android brings
    // the started activity up in a fresh :launcher process.
    public static void restartLauncher() {
        Context context = SDL.getContext();
        Intent intent = new Intent(context, LauncherActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        context.startActivity(intent);
        Process.killProcess(Process.myPid());
    }

    // From native code: asks for the dump's folder and imports it. `required` = what a dump has at
    // its root (a trailing '/' = a folder), `sha256` = default.xex's digest.
    public static void importGame(String[] required, String sha256) {
        final Activity activity = (Activity) SDL.getContext();
        importRequired = required;
        importSha256 = sha256;
        importCancel = false;
        importError = ERROR_NONE;
        importDetail = "";
        importFile = "";
        importDone = importTotal = 0;
        importState = IMPORT_PICKING;
        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            try {
                activity.startActivityForResult(intent, PICK_GAME);
            } catch (Exception e) {
                Log.e(TAG, "no folder picker", e);
                fail(ERROR_PICKER, e.toString());
            }
        });
    }

    // From native code: the game built from the imported dump (GameInstaller), and its progress.
    public static boolean gameInstalled() { return GameInstaller.installed(SDL.getContext()); }
    public static void installGame() { GameInstaller.install(SDL.getContext()); }
    public static int buildState() { return GameInstaller.state(); }
    public static int buildDone() { return GameInstaller.done(); }
    public static int buildTotal() { return GameInstaller.total(); }
    public static String buildError() { return GameInstaller.error(); }

    // From native code: the subtitles (SUBTITLES on the dump's screen, EXTRA DATA in SETUP).
    public static boolean subtitlesInstalled() { return Subtitles.installed(SDL.getContext()); }
    public static void downloadSubtitles() { Subtitles.download(SDL.getContext()); }
    public static int subtitlesState() { return Subtitles.state(); }
    public static long subtitlesDone() { return Subtitles.done(); }
    public static long subtitlesTotal() { return Subtitles.total(); }
    public static String subtitlesError() { return Subtitles.error(); }

    public static void cancelImport() { importCancel = true; }
    public static int importState() { return importState; }
    public static int importError() { return importError; }
    public static long importDone() { return importDone; }
    public static long importTotal() { return importTotal; }
    public static String importFile() { return importFile; }
    public static String importDetail() { return importDetail; }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_GAME) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            importState = IMPORT_CANCELLED;
            return;
        }
        final Uri tree = data.getData();
        final ContentResolver resolver = getContentResolver();
        final File target = new File(getExternalFilesDir(null), "game");
        importState = IMPORT_CHECKING;
        new Thread(() -> {
            try {
                copyDump(resolver, tree, target);
            } catch (Exception e) {
                Log.e(TAG, "import failed", e);
                fail(ERROR_IO, e.getMessage() != null ? e.getMessage() : e.toString());
            }
        }, "CrashImport").start();
    }

    private static void fail(int error, String detail) {
        Log.w(TAG, "import: error " + error + " " + detail);
        importError = error;
        importDetail = detail;
        importState = IMPORT_FAILED;
    }

    // A file or folder of the picked tree; path is relative to the dump's root, '/' separated.
    private static final class Entry {
        final String id, name, path;
        final boolean folder;
        final long size;

        Entry(String id, String name, String path, boolean folder, long size) {
            this.id = id;
            this.name = name;
            this.path = path;
            this.folder = folder;
            this.size = size;
        }
    }

    private static List<Entry> children(ContentResolver resolver, Uri tree, Entry parent) {
        List<Entry> list = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent.id);
        String[] columns = { Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE };
        try (Cursor c = resolver.query(uri, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String name = c.getString(1);
                list.add(new Entry(c.getString(0), name, parent.path.isEmpty() ? name : parent.path + "/" + name,
                                   Document.MIME_TYPE_DIR.equals(c.getString(2)), c.isNull(3) ? 0 : c.getLong(3)));
            }
        }
        return list;
    }

    private static boolean has(List<Entry> entries, String name, boolean folder) {
        for (Entry e : entries)
            if (e.name.equals(name) && e.folder == folder)
                return true;
        return false;
    }

    private static void listFiles(ContentResolver resolver, Uri tree, Entry folder, List<Entry> files) {
        for (Entry e : children(resolver, tree, folder)) {
            if (importCancel)
                return;
            if (e.folder)
                listFiles(resolver, tree, e, files);
            else
                files.add(e);
        }
    }

    private static void copyDump(ContentResolver resolver, Uri tree, File target) throws Exception {
        // The picked folder, or the folder inside it that has default.xex (FindDumpIn on a PC).
        Entry root = new Entry(DocumentsContract.getTreeDocumentId(tree), "", "", true, 0);
        List<Entry> top = children(resolver, tree, root);
        if (!has(top, "default.xex", false))
            for (Entry e : top)
                if (e.folder) {
                    Entry inner = new Entry(e.id, "", "", true, 0);
                    List<Entry> inside = children(resolver, tree, inner);
                    if (has(inside, "default.xex", false)) {
                        root = inner;
                        top = inside;
                        break;
                    }
                }
        if (!has(top, "default.xex", false)) {
            fail(ERROR_NOT_GAME, "");
            return;
        }
        StringBuilder missing = new StringBuilder();
        for (String name : importRequired) {
            boolean folder = name.endsWith("/");
            if (!has(top, folder ? name.substring(0, name.length() - 1) : name, folder))
                missing.append(missing.length() == 0 ? "" : ", ").append(name);
        }
        if (missing.length() > 0) {
            fail(ERROR_INCOMPLETE, missing.toString());
            return;
        }

        List<Entry> files = new ArrayList<>();
        listFiles(resolver, tree, root, files);
        if (importCancel) {
            importState = IMPORT_CANCELLED;
            return;
        }
        // default.xex first: its digest says whether the rest is worth copying.
        for (int i = 0; i < files.size(); i++)
            if (files.get(i).path.equals("default.xex"))
                files.add(0, files.remove(i));

        long total = 0, needed = 0;
        for (Entry e : files) {
            total += e.size;
            File out = new File(target, e.path);
            if (!(out.isFile() && out.length() == e.size))
                needed += e.size;
        }
        if (!target.isDirectory() && !target.mkdirs())
            throw new IOException("can't make " + target);
        long free = new StatFs(target.getPath()).getAvailableBytes();
        if (needed + (64L << 20) > free) {
            fail(ERROR_SPACE, String.format(java.util.Locale.ROOT, "%.1f / %.1f", needed / 1e9, free / 1e9));
            return;
        }
        Log.i(TAG, "import: " + files.size() + " files, " + total + " bytes (" + needed + " to copy) into " + target);

        importTotal = total;
        importState = IMPORT_COPYING;
        byte[] buffer = new byte[1 << 20];
        for (Entry e : files) {
            importFile = e.path;
            File out = new File(target, e.path);
            boolean xex = e.path.equals("default.xex");
            // Already there (an import stopped half way) is kept; default.xex is always read for its digest.
            if (!xex && out.isFile() && out.length() == e.size) {
                importDone += e.size;
                continue;
            }
            File parent = out.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                throw new IOException("can't make " + parent);
            File part = new File(out.getPath() + ".part");
            MessageDigest sha = xex ? MessageDigest.getInstance("SHA-256") : null;
            Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, e.id);
            try (InputStream in = resolver.openInputStream(uri); OutputStream os = new FileOutputStream(part)) {
                if (in == null)
                    throw new IOException("can't read " + e.path);
                int n;
                while (!importCancel && (n = in.read(buffer)) > 0) {
                    os.write(buffer, 0, n);
                    if (sha != null)
                        sha.update(buffer, 0, n);
                    importDone += n;
                }
            }
            if (importCancel) {
                part.delete();
                importState = IMPORT_CANCELLED;
                return;
            }
            if (sha != null) {
                StringBuilder hex = new StringBuilder();
                for (byte b : sha.digest())
                    hex.append(String.format("%02x", b));
                if (!hex.toString().equals(importSha256)) {
                    part.delete();
                    fail(ERROR_VERSION, hex.toString());
                    return;
                }
            }
            out.delete();
            if (!part.renameTo(out))
                throw new IOException("can't write " + out);
        }
        importFile = "";
        Log.i(TAG, "import: done");
        importState = IMPORT_DONE;
    }
}
