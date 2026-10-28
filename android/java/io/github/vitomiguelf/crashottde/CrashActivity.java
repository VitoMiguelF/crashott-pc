package io.github.vitomiguelf.crashottde;

import org.libsdl.app.SDLActivity;

// SDL3's activity: it loads libSDL3.so from the APK and libmain.so, the game the phone built from the
// player's dump (GameInstaller: the app's internal storage), and runs SDL_main
// (runtime/platform/android_main_android.cpp) on its own thread.
public class CrashActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3" };
    }

    @Override
    public void loadLibraries() {
        super.loadLibraries();
        System.load(GameInstaller.mainLibrary(this).getPath());
    }

    @Override
    protected String getMainSharedObject() {
        return GameInstaller.mainLibrary(this).getPath();
    }
}
