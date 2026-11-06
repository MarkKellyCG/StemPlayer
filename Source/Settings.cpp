#include "Settings.h"

namespace stemplayer::settings
{
    static juce::CriticalSection& lock()
    {
        static juce::CriticalSection cs;
        return cs;
    }

    juce::File file()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("StemPlayer").getChildFile ("settings.json");
    }

    static juce::var readAll()
    {
        auto v = juce::JSON::parse (file());
        return v.isObject() ? v : juce::var (new juce::DynamicObject());
    }

    juce::File outputFolder()
    {
        const juce::ScopedLock sl (lock());
        const auto s = readAll().getProperty ("outputFolder", {}).toString();
        return s.isNotEmpty() ? juce::File (s) : juce::File();
    }

    void setOutputFolder (const juce::File& folder)
    {
        const juce::ScopedLock sl (lock());
        auto v = readAll();
        v.getDynamicObject()->setProperty ("outputFolder", folder == juce::File() ? juce::String() : folder.getFullPathName());
        file().getParentDirectory().createDirectory();
        file().replaceWithText (juce::JSON::toString (v), false, false, "\n");
    }
}
