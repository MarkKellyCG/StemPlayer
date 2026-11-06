#pragma once
#include <juce_core/juce_core.h>

namespace stemplayer::log
{
    /** <user app data>/StemPlayer/StemPlayer.log  (e.g. %APPDATA%\StemPlayer\StemPlayer.log).
        Rotates to StemPlayer.log.old above 2 MB. */
    juce::File file();

    /** Thread-safe, never throws.  NOT for the audio thread (it touches the disk). */
    void write (const juce::String& message);
}
