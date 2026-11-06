#include "HelperRunner.h"
#include "Log.h"
#include <deque>

namespace stemplayer::helper
{
    juce::File defaultHelperDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("StemPlayer");
    }

    static const char* const kNames[] = { "stemsplit.py", "stemsplit.exe", "stemsplit" };

    juce::File findHelper (const juce::File& userChoice)
    {
        if (userChoice != juce::File() && userChoice.existsAsFile())
            return userChoice;

        juce::Array<juce::File> dirs;
        dirs.add (defaultHelperDirectory());

        auto bin = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        for (auto d = bin.getParentDirectory(); d != juce::File() && dirs.size() < 6; d = d.getParentDirectory())
            dirs.add (d);

        for (auto& d : dirs)
            for (auto n : kNames)
                if (d.getChildFile (n).existsAsFile())
                    return d.getChildFile (n);

        return {};
    }

    static juce::String findPython (const juce::File& script)
    {
        auto env = juce::SystemStats::getEnvironmentVariable ("STEMPLAYER_PYTHON", {});
        if (env.isNotEmpty())
            return env;

        const auto dir = script.getParentDirectory();
       #if JUCE_WINDOWS
        auto venv = dir.getChildFile ("venv").getChildFile ("Scripts").getChildFile ("python.exe");
        return venv.existsAsFile() ? venv.getFullPathName() : juce::String ("python");
       #else
        for (auto n : { "python3", "python" })
        {
            auto venv = dir.getChildFile ("venv").getChildFile ("bin").getChildFile (n);
            if (venv.existsAsFile())
                return venv.getFullPathName();
        }
        return "python3";
       #endif
    }

    juce::StringArray buildCommand (const juce::File& helper, const juce::File& input, const juce::File& outDir)
    {
        juce::StringArray args;

        if (helper.hasFileExtension ("py"))
        {
            args.add (findPython (helper));
            args.add (helper.getFullPathName());
        }
        else
        {
            args.add (helper.getFullPathName());
        }

        args.add ("--input");   args.add (input.getFullPathName());
        args.add ("--output");  args.add (outDir.getFullPathName());
        args.add ("--progress");

        auto extra = juce::SystemStats::getEnvironmentVariable ("STEMPLAYER_HELPER_ARGS", {});
        if (extra.isNotEmpty())
            args.addTokens (extra, true);

        return args;
    }

    RunResult run (const juce::StringArray& command,
                   const std::function<void (const juce::String&, const juce::String&)>& onMessage,
                   const std::function<bool()>& shouldCancel,
                   ChildHandle* handle)
    {
        RunResult res;
        juce::ChildProcess proc;
        log::write ("helper start: " + command.joinIntoString (" "));

        struct Unpublish
        {
            ChildHandle* h;
            ~Unpublish() { if (h) { const juce::ScopedLock sl (h->lock); h->proc = nullptr; } }
        } unpublish { handle };

        if (! proc.start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            res.message = "Could not start helper: " + command.joinIntoString (" ");
            return res;
        }

        if (handle != nullptr)
        {
            const juce::ScopedLock sl (handle->lock);
            handle->proc = &proc;
        }

        juce::String lastError;
        juce::MemoryBlock lineBytes;
        std::deque<juce::String> tail;
        bool sawDone = false;

        auto handleLine = [&] (juce::String line)
        {
            line = line.trimEnd();
            if (line.isEmpty()) return;

            const auto kind = line.upToFirstOccurrenceOf (" ", false, false);
            const auto text = line.fromFirstOccurrenceOf (" ", false, false).trim();

            if (kind == "HB")                       { onMessage ("HB", {}); return; }
            if (kind == "STATUS")                   { log::write ("helper: " + text); onMessage (kind, text); return; }
            if (kind == "PROGRESS")                 { onMessage (kind, text); return; }
            if (kind == "ERROR")                    { lastError = text; log::write ("helper ERROR: " + text); onMessage (kind, text); return; }
            if (kind == "DONE")                     { sawDone = true; log::write ("helper DONE: " + text); onMessage (kind, text); return; }

            log::write ("helper output: " + line);
            tail.push_back (line);                  // anything else (e.g. a Python traceback)
            if (tail.size() > 8) tail.pop_front();
        };

        for (;;)
        {
            if (shouldCancel && shouldCancel())
            {
                log::write ("helper cancelled");
                proc.kill();
                proc.waitForProcessToFinish (3000);   // also reaps the child so it doesn't linger as a zombie
                res.cancelled = true;
                res.message = "Cancelled";
                return res;
            }

            // JUCE's readProcessOutput blocks until the *whole* requested buffer is filled (or the
            // process ends), so read a single byte: it returns as soon as any output arrives.
            char c = 0;
            const int n = proc.readProcessOutput (&c, 1);

            if (n > 0)
            {
                if (c == '\n')
                {
                    handleLine (juce::String::fromUTF8 ((const char*) lineBytes.getData(), (int) lineBytes.getSize()));
                    lineBytes.reset();
                }
                else if (c != '\r' && lineBytes.getSize() < 65536)
                {
                    lineBytes.append (&c, 1);
                }
            }
            else if (! proc.isRunning())
            {
                break;
            }
            else
            {
                juce::Thread::sleep (10);
            }
        }

        if (lineBytes.getSize() > 0)
            handleLine (juce::String::fromUTF8 ((const char*) lineBytes.getData(), (int) lineBytes.getSize()));

        // A cancel from another thread kills the child, which shows up here as EOF: report it as a
        // cancellation, not as a helper failure.
        if (shouldCancel && shouldCancel())
        {
            proc.waitForProcessToFinish (3000);
            res.cancelled = true;
            res.message = "Cancelled";
            return res;
        }

        const auto exitCode = proc.getExitCode();
        res.ok = (exitCode == 0 && sawDone);
        log::write ("helper exit code " + juce::String ((int) exitCode) + (res.ok ? " (ok)" : " (FAILED)"));

        if (! res.ok)
        {
            res.message = lastError;
            if (res.message.isEmpty())
            {
                for (auto& l : tail) res.message += l + "\n";
                res.message = res.message.trim();
            }
            if (res.message.isEmpty())
                res.message = "Helper exited with code " + juce::String ((int) exitCode);
        }
        return res;
    }
}
