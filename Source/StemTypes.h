#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <array>

namespace stemplayer
{
    constexpr int kNumStems = 6;

    // Order matches Demucs htdemucs_6s output.
    inline const char* const kStemIds[kNumStems]   = { "drums", "bass", "other", "vocals", "guitar", "piano" };
    inline const char* const kStemNames[kNumStems] = { "Drums", "Bass", "Other", "Vocals", "Guitar", "Piano" };

    /** Immutable once published to the audio thread. All audio is stereo at `sampleRate`
        (the host rate it was prepared for). */
    struct StemSet
    {
        std::array<juce::AudioBuffer<float>, kNumStems> audio;
        std::array<bool, kNumStems> present {};
        double sampleRate = 0.0;
        juce::int64 length = 0;
        juce::String title;
        juce::File folder;
    };
}
