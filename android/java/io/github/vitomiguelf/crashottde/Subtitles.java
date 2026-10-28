package io.github.vitomiguelf.crashottde;

import android.content.Context;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// The subtitles, the PC installer's SUBTITLES option (tools/setup.py install_extras): the release
// asset CrashRecomp-<tag>-legendas.zip (the English transcript of the game's voices, made from the
// game, and the PT-BR translation), checked against the release's SHA256SUMS.txt and unpacked into
// the app's files folder, which stands for the PC's project / install folder (paths.cpp): the zip's
// private/ goes to data/, languages/ to languages/. The launcher starts it (download) and draws its
// progress from the getters, as the dump import.
final class Subtitles {
    private static final String TAG = "CrashSubtitles";

    // tools/setup.py SUBTITLES_TAG / SUBTITLES_ASSET / release_url: bump together.
    private static final String RELEASE = "v1.07";
    private static final String ASSET = "CrashRecomp-" + RELEASE + "-legendas.zip";
    private static final String URL_BASE = "https://github.com/VitoMiguelF/crashott-pc/releases/download/" + RELEASE + "/";
    private static final String SUMS = "SHA256SUMS.txt";

    // States (launcher/android_import.inc, Subtitles*).
    static final int IDLE = 0, DOWNLOADING = 1, DONE = 2, FAILED = 3;

    private static volatile int state = IDLE;
    private static volatile long done, total;
    private static volatile String error = "";

    static int state() { return state; }
    static long done() { return done; }
    static long total() { return total; }
    static String error() { return error; }

    private static File files(Context context) {
        return context.getExternalFilesDir(null);
    }

    // The release the installed subtitles came from (written last): an older one is replaced.
    private static File stamp(Context context) {
        return new File(files(context), "data/subtitles/release.txt");
    }

    static boolean installed(Context context) {
        try {
            return new File(files(context), "data/subtitles/english.txt").isFile() &&
                   new String(Files.readAllBytes(stamp(context).toPath()), StandardCharsets.UTF_8).trim().equals(RELEASE);
        } catch (IOException e) {
            return false;
        }
    }

    static synchronized void download(Context context) {
        if (state == DOWNLOADING)
            return;
        final Context app = context.getApplicationContext();
        done = total = 0;
        error = "";
        state = DOWNLOADING;
        new Thread(() -> {
            try {
                run(app);
                state = DONE;
            } catch (Exception e) {
                Log.e(TAG, "download failed", e);
                error = e.getMessage() != null ? e.getMessage() : e.toString();
                state = FAILED;
            }
        }, "CrashSubtitles").start();
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(30000);
        connection.setInstanceFollowRedirects(true);  // GitHub sends the asset from another host (https too)
        int code = connection.getResponseCode();
        if (code != HttpURLConnection.HTTP_OK)
            throw new IOException("HTTP " + code + " " + url.substring(url.lastIndexOf('/') + 1));
        return connection;
    }

    private static void run(Context context) throws Exception {
        // The asset's digest from the release's SHA256SUMS.txt ("<sha256>  <name>" lines).
        String digest = null;
        HttpURLConnection sums = open(URL_BASE + SUMS);
        try (InputStream in = sums.getInputStream()) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            int n;
            while ((n = in.read(buffer)) > 0)
                bytes.write(buffer, 0, n);
            for (String line : new String(bytes.toByteArray(), StandardCharsets.UTF_8).split("\n")) {
                String[] parts = line.trim().split("\\s+", 2);
                if (parts.length == 2 && parts[1].replaceFirst("^\\*", "").trim().equals(ASSET))
                    digest = parts[0].toLowerCase(java.util.Locale.ROOT);
            }
        } finally {
            sums.disconnect();
        }
        if (digest == null)
            throw new IOException(ASSET + " not in " + SUMS);

        File zip = new File(context.getCacheDir(), ASSET);
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        HttpURLConnection connection = open(URL_BASE + ASSET);
        try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(zip)) {
            total = Math.max(connection.getContentLengthLong(), 0);
            byte[] buffer = new byte[65536];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
                sha.update(buffer, 0, n);
                done += n;
            }
        } finally {
            connection.disconnect();
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : sha.digest())
            hex.append(String.format("%02x", b));
        if (!hex.toString().equals(digest)) {
            zip.delete();
            throw new IOException(ASSET + ": SHA-256 does not match " + SUMS);
        }

        // private/subtitles/english.txt -> data/subtitles/, languages/pt-br/subtitles.txt -> languages/pt-br/.
        File root = files(context);
        try (ZipInputStream in = new ZipInputStream(new FileInputStream(zip))) {
            byte[] buffer = new byte[65536];
            ZipEntry entry;
            while ((entry = in.getNextEntry()) != null) {
                String name = entry.getName();
                if (entry.isDirectory())
                    continue;
                if (name.contains("..") || name.startsWith("/"))
                    throw new IOException(ASSET + ": bad path " + name);
                String path = name.startsWith("private/") ? "data/" + name.substring(8) : name.startsWith("languages/") ? name : null;
                if (path == null) {
                    Log.w(TAG, "skipped " + name);
                    continue;
                }
                File out = new File(root, path);
                File parent = out.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                    throw new IOException("can't make " + parent);
                File part = new File(out.getPath() + ".part");
                try (OutputStream os = new FileOutputStream(part)) {
                    int n;
                    while ((n = in.read(buffer)) > 0)
                        os.write(buffer, 0, n);
                }
                out.delete();
                if (!part.renameTo(out))
                    throw new IOException("can't write " + out);
                Log.i(TAG, "installed " + path);
            }
        }
        zip.delete();
        File stamp = stamp(context);
        File parent = stamp.getParentFile();
        if (parent != null)
            parent.mkdirs();
        Files.write(stamp.toPath(), RELEASE.getBytes(StandardCharsets.UTF_8));
    }
}
