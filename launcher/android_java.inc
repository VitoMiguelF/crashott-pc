// --- LauncherActivity's methods from native code --------------------------------------------------
// (included by launcher.cpp inside its namespace, on Android only: android_import.inc and updater.inc)

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
