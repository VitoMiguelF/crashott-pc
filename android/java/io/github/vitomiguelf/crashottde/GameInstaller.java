package io.github.vitomiguelf.crashottde;

import android.content.Context;
import android.system.Os;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// The game built on the phone from the player's own dump, as the PC installer (CrashSetup) builds it:
// XenonRecomp turns game/default.xex into C++ (generated/ppc), clang compiles it and lld links it
// with the runtime into libmain.so, in the app's internal storage (an external folder can't hold
// code), which CrashActivity loads. The APK carries no game code, only what builds it:
// assets/ondevice.zip (tools/android_build.py `toolchain`: the NDK's sysroot, clang's headers,
// the runtime as static libraries, XenonRecomp's config, simde) and three programs in its native
// library folder, the one place an app may run programs from: libcrashclang.so (clang),
// libcrashlld.so (lld) and libxenonrecomp.so.
//
// The launcher starts it (install) and draws its progress from the getters, as the dump import.
final class GameInstaller {
    private static final String TAG = "CrashInstaller";

    // States (launcher/android_import.inc, Build*).
    static final int IDLE = 0, UNPACKING = 1, RECOMPILING = 2, COMPILING = 3, LINKING = 4, DONE = 5, FAILED = 6;

    private static volatile int state = IDLE;
    private static volatile int done, total;
    private static volatile String error = "";

    static int state() { return state; }
    static int done() { return done; }
    static int total() { return total; }
    static String error() { return error; }

    // libmain.so, built here; CrashActivity loads it.
    static File mainLibrary(Context context) {
        return new File(context.getFilesDir(), "libmain.so");
    }

    // Whether libmain.so was built from this dump with this APK's runtime (the stamp written after
    // the link: the on-device bundle's stamp and default.xex's size and date).
    static boolean installed(Context context) {
        File stamp = new File(context.getFilesDir(), "libmain.stamp");
        if (!mainLibrary(context).isFile() || !stamp.isFile())
            return false;
        try {
            return new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.UTF_8).equals(expectedStamp(context));
        } catch (IOException e) {
            return false;
        }
    }

    private static String bundleStamp(Context context) throws IOException {
        try (InputStream in = context.getAssets().open("ondevice.stamp")) {
            return new String(readAll(in), StandardCharsets.UTF_8).trim();
        }
    }

    private static String expectedStamp(Context context) throws IOException {
        File xex = new File(gameFolder(context), "default.xex");
        return bundleStamp(context) + " " + xex.length() + " " + xex.lastModified();
    }

    private static File gameFolder(Context context) {
        return new File(context.getExternalFilesDir(null), "game");
    }

    static synchronized void install(Context context) {
        if (state != IDLE && state != DONE && state != FAILED)
            return;
        state = UNPACKING;
        done = total = 0;
        error = "";
        final Context app = context.getApplicationContext();
        new Thread(() -> {
            try {
                build(app);
                state = DONE;
            } catch (Exception e) {
                Log.e(TAG, "install failed", e);
                error = e.getMessage() != null ? e.getMessage() : e.toString();
                state = FAILED;
            }
        }, "CrashInstall").start();
    }

    private static void build(Context context) throws Exception {
        File files = context.getFilesDir();
        File bundle = new File(files, "ondevice");
        File log = new File(files, "install.log");
        log.delete();
        long start = System.currentTimeMillis();

        // 1. The bundle, unpacked again when the APK brings another one.
        String stamp = bundleStamp(context);
        File bundleStamp = new File(bundle, "ondevice.stamp");
        if (!bundleStamp.isFile() || !new String(Files.readAllBytes(bundleStamp.toPath()), StandardCharsets.UTF_8).equals(stamp)) {
            deleteTree(bundle);
            unzip(context, bundle);
        }
        String libDir = context.getApplicationInfo().nativeLibraryDir;
        File bin = new File(bundle, "toolchain/bin");
        bin.mkdirs();
        // clang finds ld.lld next to it (-B), and lld picks its flavour from its name: links named
        // as the tools to the programs of the native library folder.
        link(bin, "clang", libDir + "/libcrashclang.so");
        link(bin, "clang++", libDir + "/libcrashclang.so");
        link(bin, "ld.lld", libDir + "/libcrashlld.so");

        // 2. XenonRecomp: default.xex -> recomp/ppc (the project's config with the dump and the output
        // folder as absolute paths). XenonRecomp glues every path in it to the config's folder as
        // text, so it runs in that folder and gets the config's bare name: the absolute paths stay
        // whole and the switch tables' relative one resolves there.
        state = RECOMPILING;
        File recomp = new File(files, "recomp");
        deleteTree(recomp);
        File ppc = new File(recomp, "ppc");
        File obj = new File(recomp, "obj");
        ppc.mkdirs();
        obj.mkdirs();
        File xenon = new File(bundle, "xenon");
        String config = new String(Files.readAllBytes(new File(xenon, "Crash.toml").toPath()), StandardCharsets.UTF_8);
        config = replaceSetting(config, "file_path", new File(gameFolder(context), "default.xex").getPath());
        config = replaceSetting(config, "out_directory_path", ppc.getPath());
        File toml = new File(xenon, "Crash.device.toml");
        Files.write(toml.toPath(), config.getBytes(StandardCharsets.UTF_8));
        exec(log, xenon, libDir + "/libxenonrecomp.so", toml.getName(), new File(xenon, "ppc_context.h").getPath());

        // 3. Each .cpp on its own core.
        state = COMPILING;
        File[] sources = ppc.listFiles((dir, name) -> name.endsWith(".cpp"));
        if (sources == null || sources.length == 0)
            throw new IOException("XenonRecomp made no code (install.log)");
        Arrays.sort(sources);
        total = sources.length;
        String clang = new File(bin, "clang++").getPath();
        String sysroot = new File(bundle, "toolchain/sysroot").getPath();
        String resource = clangResourceDir(bundle).getPath();
        String simde = new File(bundle, "include/simde").getPath();
        int workers = Math.max(1, Runtime.getRuntime().availableProcessors());
        ExecutorService pool = Executors.newFixedThreadPool(workers);
        AtomicInteger finished = new AtomicInteger();
        List<Future<?>> jobs = new ArrayList<>();
        for (File source : sources) {
            jobs.add(pool.submit(() -> {
                String name = source.getName();
                exec(log, recomp, clang, "--target=aarch64-linux-android30", "--sysroot=" + sysroot, "-resource-dir=" + resource,
                     "-O3", "-DNDEBUG", "-std=gnu++20", "-fPIC", "-fvisibility=hidden", "-fsigned-char", "-fno-strict-aliasing",
                     "-ffunction-sections", "-fdata-sections", "-Wno-unused-label", "-Wno-unused-variable",
                     "-Wno-unused-but-set-variable", "-I" + ppc.getPath(), "-I" + simde, "-c", source.getPath(), "-o",
                     new File(obj, name.substring(0, name.length() - 4) + ".o").getPath());
                done = finished.incrementAndGet();
                return null;
            }));
        }
        pool.shutdown();
        try {
            for (Future<?> job : jobs)
                job.get();
        } catch (java.util.concurrent.ExecutionException e) {
            pool.shutdownNow();
            throw e.getCause() instanceof Exception ? (Exception) e.getCause() : e;
        }

        // 4. The link, as CMake links libmain.so on a PC (CMakeLists.txt): the runtime whole, only
        // SDL_main and JNI exported, every symbol resolved now, 16 KB pages.
        state = LINKING;
        File runtime = new File(bundle, "runtime");
        File output = new File(files, "libmain.so.part");
        List<String> command = new ArrayList<>(Arrays.asList(clang, "--target=aarch64-linux-android30", "--sysroot=" + sysroot,
            "-resource-dir=" + resource, "-B" + bin.getPath(), "-fuse-ld=lld", "-shared", "-static-libstdc++",
            "-Wl,--build-id=sha1", "-Wl,--no-undefined", "-Wl,--gc-sections",
            "-Wl,--exclude-libs,libc++_static.a:libc++abi.a:libunwind.a:libatomic.a:libclang_rt.builtins-aarch64-android.a:"
                + "libXexImage.a:libadrenotools.a:liblinkernsbypass.a",
            "-Wl,-z,max-page-size=16384", "-Wl,-soname,libmain.so", "-o", output.getPath()));
        for (File source : sources) {
            String name = source.getName();
            command.add(new File(obj, name.substring(0, name.length() - 4) + ".o").getPath());
        }
        command.addAll(Arrays.asList("-Wl,--whole-archive", new File(runtime, "libcrashruntime.a").getPath(), "-Wl,--no-whole-archive",
            new File(runtime, "libXexImage.a").getPath(), libDir + "/libSDL3.so", "-llog", "-landroid", "-ldl"));
        for (String name : new String[] { "libadrenotools.a", "liblinkernsbypass.a" })
            if (new File(runtime, name).isFile())
                command.add(new File(runtime, name).getPath());
        command.addAll(Arrays.asList("-latomic", "-lm", "-pthread"));
        exec(log, recomp, command.toArray(new String[0]));
        File library = mainLibrary(context);
        library.delete();
        if (!output.renameTo(library))
            throw new IOException("can't write " + library);
        Files.write(new File(files, "libmain.stamp").toPath(), expectedStamp(context).getBytes(StandardCharsets.UTF_8));
        deleteTree(recomp);
        Log.i(TAG, "built " + library + " (" + library.length() + " bytes) in " + (System.currentTimeMillis() - start) / 1000 + " s");
    }

    // `name = "..."` in a TOML file set to `value`.
    private static String replaceSetting(String toml, String name, String value) {
        return toml.replaceAll("(?m)^" + name + "\\s*=.*$", java.util.regex.Matcher.quoteReplacement(name + " = \"" + value + "\""));
    }

    private static File clangResourceDir(File bundle) throws IOException {
        File[] versions = new File(bundle, "toolchain/lib/clang").listFiles();
        if (versions == null || versions.length == 0)
            throw new IOException("no clang resource folder in the bundle");
        return versions[0];
    }

    private static void link(File folder, String name, String target) throws Exception {
        File link = new File(folder, name);
        link.delete();
        Os.symlink(target, link.getPath());
    }

    // Runs a program; its output goes to install.log. Fails with the end of its output.
    private static void exec(File log, File directory, String... command) throws IOException, InterruptedException {
        Process process = new ProcessBuilder(command).directory(directory).redirectErrorStream(true).start();
        StringBuilder output = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (output.length() < 64 * 1024)
                    output.append(line).append('\n');
            }
        }
        int status = process.waitFor();
        synchronized (GameInstaller.class) {
            try (OutputStream out = new FileOutputStream(log, true)) {
                out.write(("$ " + String.join(" ", command) + "\n" + output + "-> " + status + "\n").getBytes(StandardCharsets.UTF_8));
            }
        }
        if (status != 0) {
            String text = output.toString().trim();
            String last = text.isEmpty() ? "" : text.substring(text.lastIndexOf('\n') + 1);
            throw new IOException(new File(command[0]).getName() + " failed (" + status + "): " + last);
        }
    }

    private static void unzip(Context context, File target) throws IOException {
        byte[] buffer = new byte[1 << 16];
        String root = target.getCanonicalPath() + File.separator;
        try (ZipInputStream zip = new ZipInputStream(context.getAssets().open("ondevice.zip"))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                File out = new File(target, entry.getName());
                if (!out.getCanonicalPath().startsWith(root))
                    throw new IOException("bad entry " + entry.getName());
                if (entry.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buffer)) > 0)
                        os.write(buffer, 0, n);
                }
            }
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int n;
        while ((n = in.read(buffer)) > 0)
            out.write(buffer, 0, n);
        return out.toByteArray();
    }

    private static void deleteTree(File file) {
        // Links are removed, not followed.
        if (file.isDirectory() && !Files.isSymbolicLink(file.toPath())) {
            File[] children = file.listFiles();
            if (children != null)
                for (File child : children)
                    deleteTree(child);
        }
        file.delete();
    }
}
