package io.github.vitomiguelf.crashottde;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import org.json.JSONArray;
import org.json.JSONObject;

// The launcher's update check (launcher/updater.inc: UPDATE in its menu) on the phone: GitHub's
// latest release of the project against the version this APK was built from (assets/version.txt,
// the newest tag of its commit: tools/android_build.py). A newer release with an APK among its files
// is an update; the browser downloads it and Android's installer puts it over this one (the same
// signature: the saves and the dump stay, and the launcher builds the game again).
//
// The launcher checks at startup and shows UPDATE; with SKIP LAUNCHER on, the game checks while it
// opens and offers the update in a banner over it (CrashActivity), which never stops the game.
final class Updates {
    private static final String TAG = "CrashUpdates";
    private static final String LATEST = "https://api.github.com/repos/VitoMiguelF/crashott-pc/releases/latest";

    // launcher/updater.inc's UpdateState (the ones the phone reaches).
    static final int IDLE = 0, CHECKING = 1, UP_TO_DATE = 2, AVAILABLE = 3, FAILED = 4;

    private static volatile int state = IDLE;
    private static volatile String installed, latest = "", download = "", error = "";

    private Updates() {}

    static int state() { return state; }
    static String latest() { return latest; }
    static String error() { return error; }

    // This APK's version ("" for a build without a tag: no update check, as a PC install that the
    // installer didn't make).
    static String installed(Context context) {
        if (installed == null) {
            try (InputStream in = context.getAssets().open("version.txt")) {
                installed = new String(readAll(in), StandardCharsets.UTF_8).trim();
            } catch (IOException e) {
                installed = "";
            }
        }
        return installed;
    }

    // Asks GitHub on a thread of its own (once; again after a failure); `done` runs on that thread
    // when the answer is in.
    static synchronized void check(Context context, final Runnable done) {
        if (state == CHECKING || state == UP_TO_DATE || state == AVAILABLE || installed(context).isEmpty())
            return;
        error = "";
        state = CHECKING;
        new Thread(() -> {
            try {
                state = ask() ? AVAILABLE : UP_TO_DATE;
            } catch (Exception e) {
                Log.w(TAG, "update check failed", e);
                error = e.getMessage() != null ? e.getMessage() : e.toString();
                state = FAILED;
            }
            Log.i(TAG, "installed " + installed + ", latest " + (latest.isEmpty() ? "?" : latest) + ": " +
                       (state == AVAILABLE ? download : state == UP_TO_DATE ? "up to date" : error));
            if (done != null)
                done.run();
        }, "CrashUpdates").start();
    }

    // The latest release, and whether it is newer than this APK and has one of its own.
    private static boolean ask() throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(LATEST).openConnection();
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(15000);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        String body;
        try {
            int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK)
                throw new IOException("GitHub HTTP " + code);
            try (InputStream in = connection.getInputStream()) {
                body = new String(readAll(in), StandardCharsets.UTF_8);
            }
        } finally {
            connection.disconnect();
        }
        JSONObject release = new JSONObject(body);
        latest = release.optString("tag_name", "");
        if (latest.isEmpty())
            throw new IOException("GitHub's answer has no release");
        download = "";
        JSONArray assets = release.optJSONArray("assets");
        for (int i = 0; assets != null && i < assets.length(); i++) {
            String url = assets.getJSONObject(i).optString("browser_download_url", "");
            if (url.toLowerCase(java.util.Locale.ROOT).endsWith(".apk"))
                download = url;
        }
        return newer(latest, installed) && !download.isEmpty();
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        for (int n; (n = in.read(buffer)) > 0;)
            bytes.write(buffer, 0, n);
        return bytes.toByteArray();
    }

    // v1.05 < v1.06 < v1.10: the numbers of the tags, in order (updater.inc's Newer).
    static boolean newer(String latest, String installed) {
        List<Integer> a = numbers(latest), b = numbers(installed);
        if (a.isEmpty() || b.isEmpty())
            return !latest.equals(installed);
        for (int i = 0; i < Math.min(a.size(), b.size()); i++) {
            if (!a.get(i).equals(b.get(i)))
                return a.get(i) > b.get(i);
        }
        return a.size() > b.size();
    }

    private static List<Integer> numbers(String tag) {
        List<Integer> numbers = new ArrayList<>();
        for (int i = 0; i < tag.length();) {
            if (!Character.isDigit(tag.charAt(i))) {
                i++;
                continue;
            }
            int start = i;
            while (i < tag.length() && Character.isDigit(tag.charAt(i)))
                i++;
            try {
                numbers.add(Integer.parseInt(tag.substring(start, i)));
            } catch (NumberFormatException e) {
                numbers.add(Integer.MAX_VALUE);
            }
        }
        return numbers;
    }

    // UPDATE: the new APK in the browser (it downloads it; opening the file installs it).
    static void open(Context context) {
        if (download.isEmpty())
            return;
        Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(download)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            context.startActivity(intent);
        } catch (Exception e) {
            Log.w(TAG, "no browser for " + download, e);
        }
    }

    // The version whose banner the player turned down in the game (NOT NOW): not offered there again.
    static String dismissed(Context context) {
        return context.getSharedPreferences("updates", Context.MODE_PRIVATE).getString("dismissed", "");
    }

    static void dismiss(Context context) {
        context.getSharedPreferences("updates", Context.MODE_PRIVATE).edit().putString("dismissed", latest).apply();
    }
}
