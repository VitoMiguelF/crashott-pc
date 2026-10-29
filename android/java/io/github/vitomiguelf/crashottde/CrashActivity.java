package io.github.vitomiguelf.crashottde;

import android.content.Intent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

// SDL3's activity: it loads libSDL3.so from the APK and libmain.so, the game the phone built from the
// player's dump (GameInstaller: the app's internal storage), and runs SDL_main
// (runtime/platform/android_main_android.cpp) on its own thread.
//
// The recent apps reopen it without the launcher (a task of its own), also after an update: with a
// libmain.so an older APK built (GameInstaller.installed false) it doesn't load it, opens the
// launcher, which builds the new one, and ends its process.
public class CrashActivity extends SDLActivity {
    private boolean stale;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        stale = !GameInstaller.installed(this);
        super.onCreate(savedInstanceState);
        if (stale) {
            finishAndRemoveTask();
            getApplicationContext().startActivity(new Intent(this, LauncherActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (stale)
            System.exit(0);  // SDLActivity keeps static state: the game starts again in a fresh process
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
