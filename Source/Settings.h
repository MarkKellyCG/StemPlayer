#pragma once
#include <juce_core/juce_core.h>

namespace stemplayer::settings
{
    /** <user app data>/StemPlayer/settings.json (e.g. %APPDATA%\StemPlayer\settings.json).
        Shared by every plugin instance and DAW on this machine. */
    juce::File file();

    /** Where split stems are written (a "<song>_stems" folder is created inside it).
        File() = default behaviour: next to the song, or %APPDATA%\StemPlayer\stems if that isn't writable. */
    juce::File outputFolder();
    void setOutputFolder (const juce::File& folder);   // pass File() to reset
}
