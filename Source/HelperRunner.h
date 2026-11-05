#pragma once
#include <juce_core/juce_core.h>
#include <functional>

namespace stemplayer::helper
{
    /** <user app data>/StemPlayer  (e.g. %APPDATA%\StemPlayer on Windows). */
    juce::File defaultHelperDirectory();

    /** Returns `userChoice` if it exists, else looks for stemsplit.py / stemsplit(.exe) in the
        default helper directory and next to the plugin binary.  Returns File() if not found. */
    juce::File findHelper (const juce::File& userChoice);

    /** Builds the argv for the helper.  .py files are run with the venv python next to them
        (venv/Scripts/python.exe or venv/bin/python3), the STEMPLAYER_PYTHON env var, or
        python/python3 on PATH.  Extra args can be supplied via STEMPLAYER_HELPER_ARGS. */
    juce::StringArray buildCommand (const juce::File& helper, const juce::File& input, const juce::File& outDir);

    struct RunResult
    {
        bool ok = false;
        bool cancelled = false;
        juce::String message;   // error text when !ok
    };

    /** Lets another thread kill the running child (JUCE's blocking pipe read can't be interrupted
        otherwise). */
    struct ChildHandle
    {
        void kill()
        {
            const juce::ScopedLock sl (lock);
            if (proc != nullptr) proc->kill();
        }

        juce::CriticalSection lock;
        juce::ChildProcess* proc = nullptr;
    };

    /** Runs the command, parsing "STATUS/PROGRESS/ERROR/DONE/HB" lines.  Blocks the calling
        (background) thread.  `shouldCancel` is polled after every received byte; pass a
        ChildHandle so another thread can kill the child to cancel immediately. */
    RunResult run (const juce::StringArray& command,
                   const std::function<void (const juce::String& kind, const juce::String& text)>& onMessage,
                   const std::function<bool()>& shouldCancel,
                   ChildHandle* handle = nullptr);
}
