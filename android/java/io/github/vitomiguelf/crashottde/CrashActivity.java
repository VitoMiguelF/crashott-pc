package io.github.vitomiguelf.crashottde;

import org.libsdl.app.SDLActivity;

// SDL3's activity as it is: it loads libSDL3.so and libmain.so and runs SDL_main
// (runtime/platform/android_main_android.cpp) on its own thread.
public class CrashActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
