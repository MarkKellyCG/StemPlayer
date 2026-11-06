#include "StemLoader.h"
#include "Log.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <thread>
#include <vector>

namespace stemplayer
{
    static juce::File findStemFile (const juce::File& folder, const char* id)
    {
        for (auto ext : { ".wav", ".flac", ".aif", ".aiff", ".WAV", ".FLAC" })
        {
            auto f = folder.getChildFile (juce::String (id) + ext);
            if (f.existsAsFile())
                return f;
        }
        return {};
    }

    double maxStemMemoryBytes()
    {
        const auto env = juce::SystemStats::getEnvironmentVariable ("STEMPLAYER_MAX_STEM_MB", {});
        if (env.getDoubleValue() > 0.0)
            return env.getDoubleValue() * 1048576.0;

        const double physical = (double) juce::SystemStats::getMemorySizeInMegabytes() * 1048576.0;
        return std::min (8.0e9, 0.25 * physical);
    }

    double stemSetBytes (double seconds, double rate, int numStems)
    {
        return seconds * rate * 2.0 * 4.0 * (double) numStems;
    }

    bool folderHasStems (const juce::File& folder)
    {
        if (! folder.isDirectory())
            return false;
        for (int i = 0; i < kNumStems; ++i)
            if (findStemFile (folder, kStemIds[i]).existsAsFile())
                return true;
        return false;
    }

    void resampleChannel (const float* in, juce::int64 inLen, float* out, juce::int64 outLen,
                          double srcRate, double dstRate)
    {
        if (outLen <= 0)
            return;

        if (inLen <= 0)
        {
            juce::FloatVectorOperations::clear (out, (int) outLen);
            return;
        }

        const double ratio = srcRate / dstRate;                         // input samples per output sample
        const int latency = (int) juce::WindowedSincInterpolator::getBaseLatency();   // input samples
        const int skip = (int) std::llround ((double) latency / ratio); // output samples to discard

        std::vector<float> padded ((size_t) inLen + (size_t) (3 * latency) + 16, 0.0f);
        std::copy (in, in + inLen, padded.begin());

        std::vector<float> tmp ((size_t) (outLen + skip), 0.0f);
        juce::WindowedSincInterpolator interp;
        interp.process (ratio, padded.data(), tmp.data(), (int) (outLen + skip));

        std::copy (tmp.begin() + skip, tmp.begin() + skip + outLen, out);
    }

    LoadResult loadStemFolder (const juce::File& folder, double targetRate,
                               const std::function<bool (float)>& progress)
    {
        LoadResult result;
        auto report = [&] (float p) { return ! progress || progress (juce::jlimit (0.0f, 1.0f, p)); };

        if (! folder.isDirectory())
        {
            result.error = "Folder not found: " + folder.getFullPathName();
            return result;
        }

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        struct Decoded { juce::AudioBuffer<float> buf; double rate = 0; bool ok = false; };
        std::array<Decoded, kNumStems> dec;
        std::array<juce::File, kNumStems> files;

        int found = 0;
        for (int i = 0; i < kNumStems; ++i)
        {
            files[(size_t) i] = findStemFile (folder, kStemIds[i]);
            if (files[(size_t) i].existsAsFile())
                ++found;
        }

        if (found == 0)
        {
            result.error = "No stem files (drums/bass/other/vocals/guitar/piano .wav/.flac) in " + folder.getFullPathName();
            return result;
        }

        // ---- size check before decoding anything (fail fast, no wasted work) ---------------
        {
            double finalBytes = 0.0, largestDecoded = 0.0, maxSeconds = 0.0;
            for (int i = 0; i < kNumStems; ++i)
            {
                if (! files[(size_t) i].existsAsFile()) continue;
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (files[(size_t) i]));
                if (r == nullptr || r->sampleRate <= 0.0) continue;
                const double seconds = (double) r->lengthInSamples / r->sampleRate;
                maxSeconds = std::max (maxSeconds, seconds);
                finalBytes += stemSetBytes (seconds, targetRate > 0.0 ? targetRate : r->sampleRate, 1);
                largestDecoded = std::max (largestDecoded, (double) r->lengthInSamples * 8.0);
            }

            const double limit = maxStemMemoryBytes();
            if (finalBytes + largestDecoded > limit)
            {
                const double perMinute = stemSetBytes (60.0, targetRate > 0.0 ? targetRate : 44100.0, found);
                result.error = "These stems are " + juce::String ((int) std::round (maxSeconds / 60.0))
                             + " min long and need about " + juce::String ((finalBytes + largestDecoded) / 1.0e9, 1)
                             + " GB of RAM; the limit here is " + juce::String (limit / 1.0e9, 1) + " GB (about "
                             + juce::String ((int) (limit / perMinute)) + " min). Use a shorter file.";
                stemplayer::log::write ("loader refused: " + result.error);
                return result;
            }
        }

        // ---- decode -------------------------------------------------------------------
        juce::int64 maxLen = 0;
        double firstRate = 0.0;
        int done = 0;

        for (int i = 0; i < kNumStems; ++i)
        {
            auto& d = dec[(size_t) i];
            if (! files[(size_t) i].existsAsFile())
                continue;

            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (files[(size_t) i]));
            if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
            {
                result.error = "Could not read " + files[(size_t) i].getFileName();
                return result;
            }

            if (reader->lengthInSamples > 0x7fffffffLL / 2)
            {
                result.error = files[(size_t) i].getFileName() + " is too long";
                return result;
            }

        const int len = (int) reader->lengthInSamples;
            d.buf.setSize (2, len);
            d.buf.clear();
            d.rate = reader->sampleRate;
            if (firstRate == 0.0) firstRate = d.rate;

            constexpr int blk = 1 << 16;
            for (int pos = 0; pos < len; pos += blk)
            {
                const int n = std::min (blk, len - pos);
                reader->read (&d.buf, pos, n, pos, true, true);
                if (! report (0.55f * ((float) done + (float) pos / (float) len) / (float) found))
                {
                    result.error = "Cancelled";
                    return result;
                }
            }

            if (reader->numChannels == 1)
                d.buf.copyFrom (1, 0, d.buf, 0, 0, len);

            d.ok = true;
            maxLen = std::max<juce::int64> (maxLen, (juce::int64) std::llround ((double) len * (targetRate > 0 ? targetRate : d.rate) / d.rate));
            ++done;
        }

        if (targetRate <= 0.0)
            targetRate = firstRate;

        auto set = std::make_unique<StemSet>();
        set->sampleRate = targetRate;
        set->length = maxLen;
        set->folder = folder;
        {
            auto name = folder.getFileName();
            if (name.endsWithIgnoreCase ("_stems"))
                name = name.dropLastCharacters (6);
            set->title = name;
        }

        // ---- resample (multi-threaded over stem x channel) ----------------------------
        struct Task { int stem, ch; };
        std::vector<Task> tasks;
        for (int i = 0; i < kNumStems; ++i)
        {
            auto& d = dec[(size_t) i];
            if (! d.ok) continue;

            set->present[(size_t) i] = true;
            set->audio[(size_t) i].setSize (2, (int) maxLen);
            set->audio[(size_t) i].clear();

            if (std::abs (d.rate - targetRate) < 0.5)
            {
                const int n = std::min (d.buf.getNumSamples(), (int) maxLen);
                for (int ch = 0; ch < 2; ++ch)
                    set->audio[(size_t) i].copyFrom (ch, 0, d.buf, ch, 0, n);
                d.buf.setSize (0, 0);
            }
            else
            {
                for (int ch = 0; ch < 2; ++ch) tasks.push_back ({ i, ch });
            }
        }

        if (! tasks.empty())
        {
            std::atomic<int> next { 0 }, finished { 0 };
            std::atomic<bool> cancelled { false };
            const int nThreads = (int) std::max (1u, std::min (8u, std::thread::hardware_concurrency()));
            std::vector<std::thread> pool;

            for (int t = 0; t < std::min<int> (nThreads, (int) tasks.size()); ++t)
                pool.emplace_back ([&]
                {
                    for (;;)
                    {
                        if (cancelled.load()) return;
                        const int k = next.fetch_add (1);
                        if (k >= (int) tasks.size()) return;

                        const auto task = tasks[(size_t) k];
                        auto& d = dec[(size_t) task.stem];
                        const auto outLen = (juce::int64) std::llround ((double) d.buf.getNumSamples() * targetRate / d.rate);
                        const auto n = std::min<juce::int64> (outLen, maxLen);
                        resampleChannel (d.buf.getReadPointer (task.ch), d.buf.getNumSamples(),
                                         set->audio[(size_t) task.stem].getWritePointer (task.ch), n,
                                         d.rate, targetRate);
                        finished.fetch_add (1);
                    }
                });

            // Poll progress from this thread while workers run.
            while (finished.load() < (int) tasks.size())
            {
                if (! report (0.55f + 0.45f * (float) finished.load() / (float) tasks.size()))
                    cancelled = true;
                if (cancelled.load()) break;
                juce::Thread::sleep (30);
            }
            for (auto& th : pool) th.join();

            if (cancelled.load())
            {
                result.error = "Cancelled";
                return result;
            }
        }

        report (1.0f);
        stemplayer::log::write ("loaded " + juce::String (found) + " stems from " + folder.getFullPathName() + " at "
                                + juce::String (targetRate) + " Hz, " + juce::String ((double) maxLen / targetRate, 1) + " s");
        result.set = std::move (set);
        return result;
    }
}
