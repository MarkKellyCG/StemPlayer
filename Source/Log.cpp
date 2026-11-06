#include "Log.h"

namespace stemplayer::log
{
    static juce::CriticalSection& lock()
    {
        static juce::CriticalSection cs;
        return cs;
    }

    juce::File file()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("StemPlayer").getChildFile ("StemPlayer.log");
    }

    void write (const juce::String& message)
    {
        try
        {
            const juce::ScopedLock sl (lock());
            auto f = file();
            f.getParentDirectory().createDirectory();

            if (f.getSize() > 2 * 1024 * 1024)
            {
                auto old = f.withFileExtension ("log.old");
                old.deleteFile();
                f.moveFileTo (old);
            }

            const auto now = juce::Time::getCurrentTime();
            const auto* t = juce::Thread::getCurrentThread();
            const juce::String who = t != nullptr ? t->getThreadName() : juce::String ("ui/host");

            f.appendText (now.formatted ("%Y-%m-%d %H:%M:%S") + juce::String::formatted (".%03d", now.getMilliseconds())
                              + " [" + who + "] " + message + "\n",
                          false, false, "\n");
        }
        catch (...) {}
    }
}
