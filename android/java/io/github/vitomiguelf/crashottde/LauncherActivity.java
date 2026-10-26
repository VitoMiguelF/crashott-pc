package io.github.vitomiguelf.crashottde;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;

import org.libsdl.app.SDL;
import org.libsdl.app.SDLActivity;

// The app's first screen: the launcher (launcher/launcher.cpp as libcrashlauncher.so) on SDL3's
// glue, in a process of its own (AndroidManifest.xml, ":launcher"), so the game later starts in a
// fresh one. PLAY calls startGame from the launcher's thread.
public class LauncherActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "crashlauncher" };
    }

    // From native code (launcher.cpp StartGame): the game, CrashActivity, in the app's main process.
    public static void startGame() {
        Context context = SDL.getContext();
        Intent intent = new Intent(context, CrashActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        context.startActivity(intent);
    }
}
