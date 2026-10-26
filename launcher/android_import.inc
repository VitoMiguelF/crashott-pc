// --- the dump, imported on Android --------------------------------------------------------------
// (included by launcher.cpp inside its namespace, on Android only)
//
// The phone has no game folder to point at: the dump is copied into the app's own folder (game/ in
// the external files folder, which the game reads). While there is none the launcher opens on this
// question; GAME FOLDER in SETUP asks again. LauncherActivity.java does the work on its own thread:
// Android's folder picker (no storage permission), the check of the PC installer (DUMP_FILES,
// shaders/, default.xex's SHA-256, read first) and the copy. This side asks for it, shows its
// progress and, once the files are in, starts the launcher again in a new process (the game's art
// and fonts are read at startup).

enum class ImportState { Idle, Picking, Checking, Copying, Done, Failed, Cancelled };  // LauncherActivity.IMPORT_*
enum ImportError { IMPORT_NOT_GAME = 1, IMPORT_INCOMPLETE, IMPORT_VERSION, IMPORT_SPACE, IMPORT_IO, IMPORT_PICKER };  // ERROR_*

struct ImportStatus
{
    ImportState state = ImportState::Idle;
    int error = 0;
    int64_t done = 0, total = 0;
    std::string file, detail;
};
ImportStatus g_import;
int g_importRestart = 0;  // frames left before the launcher starts again

// LauncherActivity's static methods from the launcher's thread, in a local frame of their own (the
// thread never goes back to Java, so local references would pile up).
struct LauncherJava
{
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jclass type = nullptr;
    bool framed = false;

    LauncherJava()
    {
        framed = env && env->PushLocalFrame(32) == 0;
        if (framed)
            if (jobject activity = static_cast<jobject>(SDL_GetAndroidActivity()))
                type = env->GetObjectClass(activity);
    }
    ~LauncherJava()
    {
        if (framed)
        {
            env->ExceptionClear();
            env->PopLocalFrame(nullptr);
        }
    }
    explicit operator bool() const { return type != nullptr; }
    jmethodID Method(const char* name, const char* signature)
    {
        jmethodID method = type ? env->GetStaticMethodID(type, name, signature) : nullptr;
        if (!method)
            env->ExceptionClear();
        return method;
    }
    // Whether the last call threw (and clears it).
    bool Threw()
    {
        bool threw = env->ExceptionCheck();
        if (threw)
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        return threw;
    }
    int Int(const char* name)
    {
        jmethodID method = Method(name, "()I");
        int value = method ? env->CallStaticIntMethod(type, method) : 0;
        return Threw() ? 0 : value;
    }
    int64_t Long(const char* name)
    {
        jmethodID method = Method(name, "()J");
        int64_t value = method ? env->CallStaticLongMethod(type, method) : 0;
        return Threw() ? 0 : value;
    }
    std::string String(const char* name)
    {
        jmethodID method = Method(name, "()Ljava/lang/String;");
        jstring value = method ? static_cast<jstring>(env->CallStaticObjectMethod(type, method)) : nullptr;
        if (Threw() || !value)
            return {};
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string text = chars ? chars : "";
        if (chars)
            env->ReleaseStringUTFChars(value, chars);
        return text;
    }
    bool Void(const char* name)
    {
        jmethodID method = Method(name, "()V");
        if (method)
            env->CallStaticVoidMethod(type, method);
        return method && !Threw();
    }
};

void OpenImport()
{
    if (g_import.state != ImportState::Picking && g_import.state != ImportState::Checking && g_import.state != ImportState::Copying)
        g_import = {};
    g_modal = Modal::Import;
}

void StartImport()
{
    LauncherJava java;
    jmethodID method = java ? java.Method("importGame", "([Ljava/lang/String;Ljava/lang/String;)V") : nullptr;
    if (!method)
    {
        g_import.state = ImportState::Failed, g_import.error = IMPORT_PICKER, g_import.detail = "LauncherActivity.importGame";
        return;
    }
    JNIEnv* env = java.env;
    jobjectArray required = env->NewObjectArray(jsize(std::size(DUMP_FILES) + 1), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < std::size(DUMP_FILES); i++)
        env->SetObjectArrayElement(required, jsize(i), env->NewStringUTF(Narrow(DUMP_FILES[i]).c_str()));
    env->SetObjectArrayElement(required, jsize(std::size(DUMP_FILES)), env->NewStringUTF("shaders/"));
    env->CallStaticVoidMethod(java.type, method, required, env->NewStringUTF(DUMP_XEX_SHA256));
    g_import = {};
    g_import.state = java.Threw() ? ImportState::Failed : ImportState::Picking;
    if (g_import.state == ImportState::Failed)
        g_import.error = IMPORT_PICKER, g_import.detail = "LauncherActivity.importGame";
    Log("[launcher] import: asking for the dump's folder");
}

void PollImport()
{
    if (g_import.state != ImportState::Picking && g_import.state != ImportState::Checking && g_import.state != ImportState::Copying)
        return;
    LauncherJava java;
    if (!java)
        return;
    ImportState before = g_import.state;
    g_import.state = ImportState(java.Int("importState"));
    g_import.done = java.Long("importDone");
    g_import.total = java.Long("importTotal");
    g_import.file = java.String("importFile");
    if (g_import.state == ImportState::Failed)
        g_import.error = java.Int("importError"), g_import.detail = java.String("importDetail");
    if (g_import.state != before)
        Log("[launcher] import: state %d (error %d %s)", int(g_import.state), g_import.error, g_import.detail.c_str());
    if (g_import.state == ImportState::Done)
        g_importRestart = 45;
}

void DrawImportModal()
{
    PollImport();
    float l = 220.0f, r = 1060.0f, t = 210.0f, b = 510.0f, cx = VW * 0.5f, width = r - l - 80.0f;
    DrawFrame(l, t, r, b);
    // A button of the dialog: the pad's icon (with the game's fonts) and the word, which a touch
    // or a click on it (generously) presses.
    float bx = 0.0f, by = t + 210.0f;
    auto button = [&](char32_t icon, const Text& text, bool selected) {
        float x0 = bx;
        bx = Icon(icon, bx, by, 32.0f) + 8.0f;
        bx = Draw(0, T(text), bx, by + 2.0f, 30.0f, selected ? SELECTED : NORMAL);
        bool clicked = Clicked(x0 - 24.0f, by - 18.0f, bx + 24.0f, by + 52.0f);
        bx += 70.0f;
        return clicked;
    };
    bool accept = KeyPressed({ ImGuiKey_GamepadFaceDown, ImGuiKey_Enter, ImGuiKey_KeypadEnter }, false);
    bool back = KeyPressed({ ImGuiKey_GamepadFaceRight, ImGuiKey_Escape }, false);
    std::string line[2];
    switch (g_import.state)
    {
    case ImportState::Idle:
    case ImportState::Cancelled:
    case ImportState::Failed:
    {
        if (g_import.state == ImportState::Failed)
        {
            const std::string& d = g_import.detail;
            switch (g_import.error)
            {
            case IMPORT_NOT_GAME:
                line[0] = T({ "This is not the folder of the game (no default.xex).", "Esta não é a pasta do jogo (não tem default.xex)." });
                line[1] = T({ "Choose the folder that has default.xex and the .rcf files.", "Escolha a pasta que tem o default.xex e os arquivos .rcf." });
                break;
            case IMPORT_INCOMPLETE:
                line[0] = T({ "The game files are incomplete.", "Os arquivos do jogo estão incompletos." });
                line[1] = T({ "Missing: ", "Faltam: " }) + d;
                break;
            case IMPORT_VERSION:
                line[0] = T({ "This default.xex is not the version the port supports.", "Este default.xex não é a versão que o port suporta." });
                line[1] = std::string(T({ "It needs Crash of the Titans for Xbox 360 (retail disc); SHA-256 ",
                                          "Ele precisa do Crash of the Titans de Xbox 360 (disco comercial); SHA-256 " })) +
                          d.substr(0, 16) + "...";
                break;
            case IMPORT_SPACE:
            {
                double needed = 0, available = 0;
                sscanf(d.c_str(), "%lf / %lf", &needed, &available);
                char text[160];
                snprintf(text, sizeof(text), T({ "It needs %.1f GB; %.1f GB are free.", "Ele precisa de %.1f GB; há %.1f GB livres." }), needed, available);
                line[0] = T({ "Not enough free space on the device.", "Não há espaço livre suficiente no aparelho." });
                line[1] = text;
                break;
            }
            case IMPORT_PICKER:
                line[0] = T({ "This device has no folder picker.", "Este aparelho não tem o seletor de pastas." });
                line[1] = d;
                break;
            default:
                line[0] = T({ "The game files could not be copied.", "Não foi possível copiar os arquivos do jogo." });
                line[1] = d;
                break;
            }
        }
        else
        {
            line[0] = T({ "Choose the folder of your Xbox 360 dump", "Escolha a pasta do seu dump de Xbox 360" });
            line[1] = T({ "(default.xex, the .rcf files and shaders). It is copied into the app (about 3 GB).",
                          "(default.xex, os arquivos .rcf e shaders). Ela é copiada para o app (cerca de 3 GB)." });
        }
        Draw(0, T({ "THE GAME FILES", "OS ARQUIVOS DO JOGO" }), cx, t + 40.0f, 34.0f, TITLE, 0.5f, width);
        Draw(0, line[0], cx, t + 100.0f, 26.0f, HELP, 0.5f, width);
        DrawUi(line[1], cx, t + 145.0f, 20.0f, IM_COL32(255, 245, 220, 235), 0.5f, width);
        bx = cx - 250.0f;
        bool choose = button(ma::ICON_A, g_import.state == ImportState::Failed ? Text{ "CHOOSE ANOTHER", "ESCOLHER OUTRA" } : Text{ "CHOOSE FOLDER", "ESCOLHER PASTA" }, true);
        bool later = button(ma::ICON_B, { "LATER", "DEPOIS" }, false);
        if (choose || accept)
            StartImport();
        else if (later || back)
            g_modal = Modal::None;
        break;
    }
    case ImportState::Picking:
        Draw(0, T({ "THE GAME FILES", "OS ARQUIVOS DO JOGO" }), cx, t + 40.0f, 34.0f, TITLE, 0.5f, width);
        Draw(0, T({ "Choose the dump's folder...", "Escolha a pasta do dump..." }), cx, t + 110.0f, 26.0f, HELP, 0.5f, width);
        break;
    case ImportState::Checking:
    case ImportState::Copying:
    {
        bool copying = g_import.state == ImportState::Copying;
        Draw(0, copying ? T({ "COPYING THE GAME FILES", "COPIANDO OS ARQUIVOS DO JOGO" }) : T({ "CHECKING THE GAME FILES", "VERIFICANDO OS ARQUIVOS DO JOGO" }), cx,
             t + 40.0f, 34.0f, TITLE, 0.5f, width);
        float fraction = copying && g_import.total > 0 ? float(double(g_import.done) / double(g_import.total)) : 0.0f;
        float x0 = l + 70.0f, x1 = r - 70.0f, y0 = t + 110.0f, y1 = t + 134.0f;
        g_draw->AddRectFilled(g_view.P(x0, y0), g_view.P(x1, y1), IM_COL32(60, 28, 4, 255), 6.0f * g_view.s);
        if (fraction > 0.0f)
            g_draw->AddRectFilled(g_view.P(x0, y0), g_view.P(x0 + (x1 - x0) * std::min(fraction, 1.0f), y1), IM_COL32(236, 150, 20, 255), 6.0f * g_view.s);
        if (copying)
        {
            char text[96];
            snprintf(text, sizeof(text), "%.2f / %.2f GB  (%d%%)", double(g_import.done) / 1e9, double(g_import.total) / 1e9, int(fraction * 100.0f));
            Draw(0, text, cx, t + 146.0f, 24.0f, HELP, 0.5f, width);
            DrawUi(g_import.file, cx, t + 76.0f, 18.0f, IM_COL32(255, 245, 220, 200), 0.5f, width);
        }
        bx = cx - 90.0f;
        if (button(ma::ICON_B, { "CANCEL", "CANCELAR" }, false) || back)
            LauncherJava().Void("cancelImport");
        break;
    }
    case ImportState::Done:
        Draw(0, T({ "THE GAME FILES ARE IN PLACE", "OS ARQUIVOS DO JOGO ESTÃO PRONTOS" }), cx, t + 40.0f, 34.0f, TITLE, 0.5f, width);
        Draw(0, T({ "Opening the launcher again...", "Abrindo o launcher de novo..." }), cx, t + 110.0f, 26.0f, HELP, 0.5f, width);
        if (g_importRestart > 0 && --g_importRestart == 0)
        {
            Log("[launcher] import: done, starting again");
            if (!LauncherJava().Void("restartLauncher"))
                g_quit = true;
        }
        break;
    }
}
