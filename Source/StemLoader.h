#pragma once
#include "StemTypes.h"
#include <functional>
#include <memory>

namespace stemplayer
{
    struct LoadResult
    {
        std::unique_ptr<StemSet> set;
        juce::String error;   // non-empty when set == nullptr
    };

    /** Finds drums/bass/other/vocals/guitar/piano (.wav .flac .aif .aiff) in `folder`,
        decodes them, resamples to `targetRate` with a windowed-sinc interpolator, and
        pads to a common length.  At least one stem must exist.
        `progress` receives 0..1 and returns false to cancel.  Safe to call off the
        message thread. */
    LoadResult loadStemFolder (const juce::File& folder,
                               double targetRate,
                               const std::function<bool (float)>& progress = {});

    /** Offline windowed-sinc resample of one channel; output length = round(inLen * dst/src).
        Compensates the interpolator's fixed latency.  Exposed for tests. */
    void resampleChannel (const float* in, juce::int64 inLen, float* out, juce::int64 outLen,
                          double srcRate, double dstRate);

    /** How much RAM a loaded stem set may use: 25% of physical RAM, at most 8 GB.
        Override with the STEMPLAYER_MAX_STEM_MB environment variable (used by the tests). */
    double maxStemMemoryBytes();

    /** Bytes needed to hold `seconds` of six stereo float stems at `rate`. */
    double stemSetBytes (double seconds, double rate, int numStems = 6);

    /** True if `folder` holds at least one recognised stem file. */
    bool folderHasStems (const juce::File& folder);
}
