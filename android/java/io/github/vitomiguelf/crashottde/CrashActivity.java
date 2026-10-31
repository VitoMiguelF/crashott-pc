package io.github.vitomiguelf.crashottde;

import android.content.Intent;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

import org.libsdl.app.SDLActivity;

// SDL3's activity: it loads libSDL3.so from the APK and libmain.so, the game the phone built from the
// player's dump (GameInstaller: the app's internal storage), and runs SDL_main
// (runtime/platform/android_main_android.cpp) on its own thread.
//
// The recent apps reopen it without the launcher (a task of its own), also after an update: with a
// libmain.so an older APK built (GameInstaller.installed false) it doesn't load it, opens the
// launcher, which builds the new one, and ends its process.
//
// Opened by the icon itself (SKIP LAUNCHER, LauncherActivity), the game shows a LAUNCHER button for
// its first seconds (back to the launcher: this game ends) and checks for updates on the way, as
// the launcher would have (Updates.java): a newer version is offered in a banner over the game,
// which goes on all the while; NOT NOW hides it for that version.
public class CrashActivity extends SDLActivity {
    private static final String TAG = "CrashActivity";
    static final String SKIPPED_LAUNCHER = "skippedLauncher";  // the intent's extra: opened by the icon
    private static final long LAUNCHER_BUTTON_MS = 8000, UPDATE_BANNER_MS = 20000;
    private boolean stale, leaving;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        stale = !GameInstaller.installed(this);
        super.onCreate(savedInstanceState);
        if (stale) {
            finishAndRemoveTask();
            getApplicationContext().startActivity(new Intent(this, LauncherActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } else if (savedInstanceState == null && getIntent() != null && getIntent().getBooleanExtra(SKIPPED_LAUNCHER, false)) {
            boolean portuguese = Locale.getDefault().getLanguage().equals("pt");
            LinearLayout bar = banner(portuguese ? "Launcher pulado" : "Launcher skipped");
            button(bar, "LAUNCHER", v -> openLauncher());
            show(bar, 8, LAUNCHER_BUTTON_MS);
            Updates.check(this, () -> runOnUiThread(() -> offerUpdate(portuguese)));
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (stale || leaving)
            System.exit(0);  // SDLActivity keeps static state: the game starts again in a fresh process
    }

    // LAUNCHER: the launcher (SHOW_LAUNCHER: it doesn't skip itself), and this game ends.
    private void openLauncher() {
        Log.i(TAG, "LAUNCHER: the game ends, the launcher opens");
        leaving = true;
        startActivity(new Intent(this, LauncherActivity.class).setAction(LauncherActivity.SHOW_LAUNCHER).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        finishAndRemoveTask();
    }

    private void offerUpdate(boolean portuguese) {
        if (isFinishing() || Updates.state() != Updates.AVAILABLE || Updates.latest().equals(Updates.dismissed(this)))
            return;
        LinearLayout bar = banner((portuguese ? "Nova vers\u00e3o: " : "New version: ") + Updates.latest());
        button(bar, portuguese ? "ATUALIZAR" : "UPDATE", v -> {
            hide(bar);
            Updates.open(this);
        });
        button(bar, portuguese ? "AGORA N\u00c3O" : "NOT NOW", v -> {
            hide(bar);
            Updates.dismiss(this);
        });
        show(bar, 60, UPDATE_BANNER_MS);
    }

    // A small bar over the top of the game, in the launcher's colours. Its buttons never take the
    // focus (the controller's and the keyboard's input stays with the game); touches outside it
    // reach the game.
    private LinearLayout banner(String text) {
        float dp = getResources().getDisplayMetrics().density;
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(Math.round(14 * dp), Math.round(4 * dp), Math.round(4 * dp), Math.round(4 * dp));
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xE61E0E02);
        background.setCornerRadius(12 * dp);
        background.setStroke(Math.round(2 * dp), 0xFFD6780E);
        bar.setBackground(background);
        TextView label = new TextView(this);
        label.setText(text);
        label.setTextColor(0xFFEEE2C4);
        label.setTextSize(15);
        bar.addView(label);
        return bar;
    }

    private void button(LinearLayout bar, String text, View.OnClickListener click) {
        float dp = getResources().getDisplayMetrics().density;
        TextView button = new TextView(this);
        button.setText(text);
        button.setTextColor(0xFFFFEC78);
        button.setTextSize(15);
        button.setTypeface(Typeface.DEFAULT_BOLD);
        button.setPadding(Math.round(14 * dp), Math.round(8 * dp), Math.round(10 * dp), Math.round(8 * dp));
        button.setFocusable(false);
        button.setOnClickListener(click);
        bar.addView(button);
    }

    private void show(View bar, int topDp, long forMs) {
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                       Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        params.topMargin = Math.round(topDp * getResources().getDisplayMetrics().density);
        addContentView(bar, params);
        new Handler(Looper.getMainLooper()).postDelayed(() -> hide(bar), forMs);
    }

    private static void hide(View bar) {
        if (bar.getParent() instanceof ViewGroup)
            ((ViewGroup) bar.getParent()).removeView(bar);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3" };
    }

    @Override
    public void loadLibraries() {
        super.loadLibraries();
        if (!stale)
            System.load(GameInstaller.mainLibrary(this).getPath());
    }

    @Override
    protected String getMainSharedObject() {
        return GameInstaller.mainLibrary(this).getPath();
    }
}
