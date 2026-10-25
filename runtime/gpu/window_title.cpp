#include "window.h"
#include "../platform/platform.h"

#include <cstdlib>
#include <string>

namespace gpu::window
{
    // The window title says who started this instance: "<title> | <run folder or profile> |
    // session <id> | <tag> | PID n". The run folder comes from tools/run_game.py
    // (CRASH_SCREENSHOT_DIR = the run's folder, CRASH_PROFILE), the session from the Claude Code
    // session that launched it (CLAUDE_CODE_HOST_SESSION_ID, inherited), CRASH_WINDOW_TAG is free text.
    std::string InstanceTitleSuffix()
    {
        std::string extra;
        auto add = [&](const std::string& part) {
            if (!part.empty())
                extra += " | " + part;
        };
        std::string run;
        if (const char* dir = getenv("CRASH_SCREENSHOT_DIR"))
        {
            run = dir;
            while (!run.empty() && (run.back() == '\\' || run.back() == '/'))
                run.pop_back();
            run = run.substr(run.find_last_of("\\/") + 1);
        }
        const char* profile = getenv("CRASH_PROFILE");
        if (!run.empty())
            add("run " + run);
        else if (profile && *profile)
            add(std::string("profile ") + profile);
        if (const char* session = getenv("CLAUDE_CODE_HOST_SESSION_ID"); session && *session)
        {
            std::string id = session;
            if (id.rfind("local_", 0) == 0)
                id = id.substr(6);
            add("session " + id.substr(0, 8));
        }
        if (const char* tag = getenv("CRASH_WINDOW_TAG"))
            add(tag);
        add("PID " + std::to_string(plat::CurrentProcessId()));
        return extra;
    }
}
