// Headless tests for StemPlayer.  Exit code 0 = all passed.
//
// Optional environment:
//   STEMPLAYER_TEST_HELPER=/path/to/Helper/stemsplit.py   (+ STEMPLAYER_PYTHON, STEMPLAYER_HELPER_ARGS="--model fake")
//       -> runs the real split job end to end
//   STEMPLAYER_PERF=1   -> times a 4 minute / 6 stem resample
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "../Source/PluginProcessor.h"
#include "../Source/StemLoader.h"
#include "../Source/Log.h"
#include "../Source/Settings.h"
#include <cstdlib>
#include <iostream>

using namespace stemplayer;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg)                                                                         \
    do { ++g_checks; if (! (cond)) { ++g_fail; std::cout << "  FAIL: " << msg << "  [" #cond "]\n"; } } while (0)

static double goertzelAmp (const float* x, int n, double freq, double sr)
{
    double re = 0, im = 0;
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / sr;
    for (int i = 0; i < n; ++i) { re += x[i] * std::cos (w * i); im += x[i] * std::sin (w * i); }
    return 2.0 * std::sqrt (re * re + im * im) / n;
}

static void writeWav (const juce::File& f, const std::vector<float>& L, const std::vector<float>* R, double rate)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    const unsigned int chans = R ? 2 : 1;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, chans, 24, {}, 0));
    if (w == nullptr) { std::cout << "cannot write " << f.getFullPathName() << "\n"; std::exit (2); }
    os.release();

    juce::AudioBuffer<float> b ((int) chans, (int) L.size());
    b.copyFrom (0, 0, L.data(), (int) L.size());
    if (R) b.copyFrom (1, 0, R->data(), (int) R->size());
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

static std::vector<float> sine (double freq, double amp, double rate, double seconds)
{
    std::vector<float> v ((size_t) (rate * seconds));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) i / rate));
    return v;
}

struct FakeHead : public juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setTimeInSamples (pos);
        return p;
    }
    bool playing = true;
    juce::int64 pos = 0;
};

using Channels = std::vector<std::vector<float>>;

static Channels render (StemPlayerProcessor& p, FakeHead& h, juce::int64 start, int numSamples, int block = 512)
{
    const int nch = p.getTotalNumOutputChannels();
    Channels out ((size_t) nch);
    h.pos = start;
    juce::AudioBuffer<float> buf (nch, block);
    juce::MidiBuffer mb;
    for (int done = 0; done < numSamples; done += block)
    {
        const int n = std::min (block, numSamples - done);
        buf.setSize (nch, n, false, false, true);
        buf.clear();
        p.processBlock (buf, mb);
        for (int c = 0; c < nch; ++c)
            out[(size_t) c].insert (out[(size_t) c].end(), buf.getReadPointer (c), buf.getReadPointer (c) + n);
        h.pos += n;
    }
    return out;
}

static void setParam (StemPlayerProcessor& p, const juce::String& id, float plainValue)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plainValue));
}

static float maxAbs (const std::vector<float>& v, size_t from = 0, size_t to = (size_t) -1)
{
    float m = 0;
    for (size_t i = from; i < std::min (to, v.size()); ++i) m = std::max (m, std::abs (v[i]));
    return m;
}

static bool waitFor (const std::function<bool()>& cond, int ms)
{
    for (int t = 0; t < ms; t += 20)
    {
        if (cond()) return true;
        juce::Thread::sleep (20);
    }
    return cond();
}

static int auxL (int stem) { return 2 + 2 * stem; }
static int auxR (int stem) { return 3 + 2 * stem; }

//==============================================================================
static void testResampler()
{
    std::cout << "[resampler]\n";
    struct Case { double src, dst; };
    for (auto c : { Case { 44100, 48000 }, Case { 96000, 44100 }, Case { 48000, 44100 }, Case { 44100, 88200 } })
    {
        const int inLen = (int) c.src;                   // 1 second
        const int impulseAt = inLen / 2;
        std::vector<float> in ((size_t) inLen, 0.0f);
        in[(size_t) impulseAt] = 1.0f;
        const auto outLen = (juce::int64) std::llround (inLen * c.dst / c.src);
        std::vector<float> out ((size_t) outLen);
        resampleChannel (in.data(), inLen, out.data(), outLen, c.src, c.dst);

        size_t peak = 0;
        for (size_t i = 0; i < out.size(); ++i) if (std::abs (out[i]) > std::abs (out[peak])) peak = i;
        const double expected = impulseAt * c.dst / c.src;
        CHECK (std::abs ((double) peak - expected) <= 1.0,
               c.src << "->" << c.dst << " impulse at " << peak << ", expected " << expected);

        auto tone = sine (1000.0, 0.5, c.src, 1.0);
        std::vector<float> to ((size_t) outLen);
        resampleChannel (tone.data(), (juce::int64) tone.size(), to.data(), outLen, c.src, c.dst);
        const int n = (int) (c.dst * 0.5);
        const double amp = goertzelAmp (to.data() + (int) (c.dst * 0.25), n, 1000.0, c.dst);
        CHECK (std::abs (amp - 0.5) < 0.01, c.src << "->" << c.dst << " tone amplitude " << amp);
    }
}

static void testLoaderEdgeCases (const juce::File& root)
{
    std::cout << "[loader edge cases]\n";

    // empty folder
    auto empty = root.getChildFile ("empty"); empty.createDirectory();
    auto r = loadStemFolder (empty, 48000.0);
    CHECK (r.set == nullptr && r.error.isNotEmpty(), "empty folder must fail with a message");

    // missing folder
    r = loadStemFolder (root.getChildFile ("nope"), 48000.0);
    CHECK (r.set == nullptr, "missing folder must fail");

    // only vocals, mono source at 44.1k
    auto one = root.getChildFile ("one_Song_stems"); one.createDirectory();
    auto tone = sine (440.0, 0.4, 44100.0, 1.0);
    writeWav (one.getChildFile ("vocals.wav"), tone, nullptr, 44100.0);
    r = loadStemFolder (one, 48000.0);
    CHECK (r.set != nullptr, "single-stem folder should load: " << r.error);
    if (r.set)
    {
        CHECK (r.set->present[3] && ! r.set->present[0] && ! r.set->present[1], "only vocals present");
        CHECK (r.set->title == "one_Song", "title strips _stems: " << r.set->title);
        CHECK (std::abs ((double) r.set->length - 48000.0) <= 1.0, "length " << r.set->length);
        const auto& b = r.set->audio[3];
        float d = 0;
        for (int i = 0; i < b.getNumSamples(); ++i) d = std::max (d, std::abs (b.getSample (0, i) - b.getSample (1, i)));
        CHECK (d == 0.0f, "mono must be duplicated to both channels");
    }

    // cancel
    r = loadStemFolder (one, 48000.0, [] (float) { return false; });
    CHECK (r.set == nullptr && r.error == "Cancelled", "cancel via progress callback");

    // no resample needed -> bit exact length
    r = loadStemFolder (one, 44100.0);
    CHECK (r.set && r.set->length == 44100, "same-rate load keeps length");
}

//==============================================================================
static void testPlayback (const juce::File& root)
{
    std::cout << "[playback / routing]\n";

    auto dir = root.getChildFile ("Song_stems"); dir.createDirectory();
    for (int i = 0; i < kNumStems; ++i)
    {
        auto L = sine (200.0 * (i + 1), 0.5, 44100.0, 3.0);
        auto R = sine (200.0 * (i + 1), 0.25, 44100.0, 3.0);
        writeWav (dir.getChildFile (juce::String (kStemIds[i]) + ".wav"), L, &R, 44100.0);
    }

    const double sr = 48000.0;
    StemPlayerProcessor p;
    for (int i = 1; i <= kNumStems; ++i)
        CHECK (p.getBus (false, i)->enable (true), "can enable aux bus " << i);
    CHECK (p.getTotalNumOutputChannels() == 14, "14 output channels, got " << p.getTotalNumOutputChannels());

    p.setRateAndBufferSizeDetails (sr, 512);
    FakeHead head;
    p.setPlayHead (&head);
    p.prepareToPlay (sr, 512);

    juce::String err;
    CHECK (p.loadFolderBlocking (dir, sr, err), "load: " << err);
    for (int i = 0; i < kNumStems; ++i) CHECK (p.stemPresent (i), "stem present " << i);
    CHECK (p.getTitle() == "Song", "title");

    const int N = 48000;
    auto ref = render (p, head, 0, 80000);      // long enough for the locate test

    // 1. every stem output carries its own tone, correct level per channel, no leakage
    for (int s = 0; s < kNumStems; ++s)
    {
        const double f = 200.0 * (s + 1);
        const float* L = ref[(size_t) auxL (s)].data() + 4800;
        const float* R = ref[(size_t) auxR (s)].data() + 4800;
        const int n = 38400;
        CHECK (std::abs (goertzelAmp (L, n, f, sr) - 0.5) < 0.015, kStemNames[s] << " L level " << goertzelAmp (L, n, f, sr));
        CHECK (std::abs (goertzelAmp (R, n, f, sr) - 0.25) < 0.01, kStemNames[s] << " R level " << goertzelAmp (R, n, f, sr));
        for (int o = 0; o < kNumStems; ++o)
            if (o != s) CHECK (goertzelAmp (L, n, 200.0 * (o + 1), sr) < 0.004, kStemNames[s] << " leaks " << kStemNames[o]);
    }

    // 2. Mix == sum of stems
    {
        float worst = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int j = 0; j < N; ++j)
            {
                float sum = 0;
                for (int s = 0; s < kNumStems; ++s) sum += ref[(size_t) (2 + 2 * s + ch)][(size_t) j];
                worst = std::max (worst, std::abs (sum - ref[(size_t) ch][(size_t) j]));
            }
        CHECK (worst < 1e-5f, "mix equals sum of stems, worst error " << worst);
    }

    // 3. mute drums: gone from Mix, still on its own output
    setParam (p, "mute_drums", 1.0f);
    {
        auto o = render (p, head, 0, N);
        CHECK (goertzelAmp (o[0].data() + 4800, 38400, 200.0, sr) < 0.002, "muted drums absent from mix");
        CHECK (goertzelAmp (o[1].data() + 4800, 38400, 400.0, sr) > 0.2, "bass still in mix");
        CHECK (goertzelAmp (o[(size_t) auxL (0)].data() + 4800, 38400, 200.0, sr) > 0.45, "muted drums still on own output");
    }
    setParam (p, "mute_drums", 0.0f);

    // 4. solo vocals
    setParam (p, "solo_vocals", 1.0f);
    {
        auto o = render (p, head, 0, N);
        CHECK (goertzelAmp (o[0].data() + 4800, 38400, 800.0, sr) > 0.45, "solo vocals present in mix");
        for (int s = 0; s < kNumStems; ++s)
            if (s != 3) CHECK (goertzelAmp (o[0].data() + 4800, 38400, 200.0 * (s + 1), sr) < 0.002, "solo hides " << kStemNames[s]);
    }
    setParam (p, "solo_vocals", 0.0f);

    // 5. per-stem gain applies to its own output and to the mix
    setParam (p, "gain_bass", -6.0f);
    {
        auto o = render (p, head, 0, N);
        const double a = goertzelAmp (o[(size_t) auxL (1)].data() + 4800, 38400, 400.0, sr);
        CHECK (std::abs (a - 0.5 * 0.5012) < 0.012, "bass -6 dB on own output: " << a);
        const double m = goertzelAmp (o[0].data() + 4800, 38400, 400.0, sr);
        CHECK (std::abs (m - 0.5 * 0.5012) < 0.012, "bass -6 dB in mix: " << m);
    }
    setParam (p, "gain_bass", 0.0f);

    // 6. master gain affects the Mix only
    setParam (p, "master", -6.0f);
    {
        auto o = render (p, head, 0, N);
        CHECK (std::abs (goertzelAmp (o[0].data() + 4800, 38400, 600.0, sr) - 0.5 * 0.5012) < 0.012, "master -6 dB on mix");
        CHECK (std::abs (goertzelAmp (o[(size_t) auxL (2)].data() + 4800, 38400, 600.0, sr) - 0.5) < 0.015, "master does not touch stem outputs");
    }
    setParam (p, "master", 0.0f);

    // 7. stopped transport is silent
    head.playing = false;
    {
        auto o = render (p, head, 0, 4096);
        float m = 0; for (auto& c : o) m = std::max (m, maxAbs (c));
        CHECK (m == 0.0f, "silent when host stopped");
    }
    head.playing = true;

    // 8. random access (loop / locate) is sample accurate
    {
        auto o = render (p, head, 70000, 4096);
        float worst = 0;
        for (size_t c = 0; c < o.size(); ++c)
            for (size_t j = 0; j < 4096; ++j)
                worst = std::max (worst, std::abs (o[c][j] - ref[c][70000 + j]));
        CHECK (worst < 1e-6f, "locate to 70000 matches continuous playback, worst " << worst);
    }

    // 9. offset
    setParam (p, "offset", 0.5f);                       // stems start 0.5 s into the timeline
    {
        auto o = render (p, head, 0, N);
        float pre = 0; for (int s = 0; s < kNumStems; ++s) pre = std::max (pre, maxAbs (o[(size_t) auxL (s)], 0, 24000));
        CHECK (pre == 0.0f, "silence before offset, got " << pre);
        float worst = 0;
        for (size_t j = 24000; j < (size_t) N; ++j)
            worst = std::max (worst, std::abs (o[(size_t) auxL (1)][j] - ref[(size_t) auxL (1)][j - 24000]));
        CHECK (worst < 1e-5f, "content shifted by exactly 24000 samples, worst " << worst);
    }
    setParam (p, "offset", -0.5f);
    {
        auto o = render (p, head, 0, 4096);
        float worst = 0;
        for (size_t j = 0; j < 4096; ++j)
            worst = std::max (worst, std::abs (o[(size_t) auxL (1)][j] - ref[(size_t) auxL (1)][j + 24000]));
        CHECK (worst < 1e-5f, "negative offset shifts the other way, worst " << worst);
    }
    setParam (p, "offset", 0.0f);

    // 10. past the end
    {
        const juce::int64 len = 144000;
        auto o = render (p, head, len + 1000, 2048);
        float m = 0; for (auto& c : o) m = std::max (m, maxAbs (c));
        CHECK (m == 0.0f, "silent after the end of the stems");

        o = render (p, head, len - 256, 512);
        float tail = 0; for (int s = 0; s < kNumStems; ++s) tail = std::max (tail, maxAbs (o[(size_t) auxL (s)], 256, 512));
        CHECK (tail == 0.0f, "block straddling the end is zero past the end");
        float head_ = 0; for (int s = 0; s < kNumStems; ++s) head_ = std::max (head_, maxAbs (o[(size_t) auxL (s)], 0, 256));
        CHECK (head_ > 0.1f, "block straddling the end plays up to the end");
    }

    // 11. odd block sizes
    {
        auto o = render (p, head, 1000, 3000, 37);
        float worst = 0;
        for (size_t c = 0; c < o.size(); ++c)
            for (size_t j = 0; j < 3000; ++j)
                worst = std::max (worst, std::abs (o[c][j] - ref[c][1000 + j]));
        CHECK (worst < 1e-6f, "block size 37 identical to 512, worst " << worst);
    }

    // 12. host that enables only the Mix bus
    {
        StemPlayerProcessor q;
        q.setRateAndBufferSizeDetails (sr, 512);
        FakeHead h2; q.setPlayHead (&h2);
        q.prepareToPlay (sr, 512);
        juce::String e2;
        CHECK (q.loadFolderBlocking (dir, sr, e2), "second instance load");
        CHECK (q.getTotalNumOutputChannels() == 2, "stereo only by default");
        auto o = render (q, h2, 0, N);
        float worst = 0;
        for (int j = 0; j < N; ++j) worst = std::max (worst, std::abs (o[0][(size_t) j] - ref[0][(size_t) j]));
        CHECK (worst < 1e-6f, "mix-only host gets identical Mix, worst " << worst);
    }

    // 13. state round trip including async reload of the folder
    setParam (p, "gain_piano", -12.0f);
    juce::MemoryBlock state;
    p.getStateInformation (state);
    {
        StemPlayerProcessor q;
        q.setRateAndBufferSizeDetails (44100.0, 256);
        q.prepareToPlay (44100.0, 256);              // host rate known first
        q.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (std::abs (q.apvts.getRawParameterValue ("gain_piano")->load() + 12.0f) < 0.11f, "parameter restored");
        CHECK (waitFor ([&] { return q.stemPresent (0) && ! q.getStatus().busy(); }, 20000), "stems reloaded from saved folder");
        CHECK (q.getTitle() == "Song", "title after reload");
    }
    {
        StemPlayerProcessor q;                        // state first, prepare later
        q.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (! q.stemPresent (0), "nothing loads until the host rate is known");
        q.setRateAndBufferSizeDetails (96000.0, 256);
        q.prepareToPlay (96000.0, 256);
        CHECK (waitFor ([&] { return q.stemPresent (0) && ! q.getStatus().busy(); }, 20000), "deferred load after prepareToPlay");
    }
    setParam (p, "gain_piano", 0.0f);

    // 14. sample-rate change reloads at the new rate
    p.setRateAndBufferSizeDetails (44100.0, 512);
    p.prepareToPlay (44100.0, 512);
    CHECK (waitFor ([&] { return ! p.getStatus().busy(); }, 20000), "reload finished");
    {
        juce::String e3;
        head.pos = 0;
        auto o = render (p, head, 0, 44100);
        CHECK (goertzelAmp (o[(size_t) auxL (0)].data() + 4410, 35280, 200.0, 44100.0) > 0.45, "plays after rate change");
    }
    p.setPlayHead (nullptr);
}

//==============================================================================
static void testJobs (const juce::File& root)
{
    std::cout << "[helper jobs]\n";
    const double sr = 44100.0;

    // helper missing -> clear error, no crash
    {
        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        p.setHelperPath (root.getChildFile ("does_not_exist.py"));
        auto wav = root.getChildFile ("in.wav");
        writeWav (wav, sine (440, 0.3, 44100, 1.0), nullptr, 44100);
        p.requestSplit (wav);
        CHECK (waitFor ([&] { return p.getStatus().state == StemPlayerProcessor::JobState::Error; }, 5000), "missing helper -> Error state");
        CHECK (p.getStatus().text.containsIgnoreCase ("helper"), "error mentions helper: " << p.getStatus().text);
    }

    // cancel a long running helper; child must die
    {
        auto script = root.getChildFile ("slow_helper_marker.sh");
        script.replaceWithText ("#!/bin/sh\necho $$ > \"$(dirname \"$0\")/slow.pid\"\necho 'STATUS busy'\nwhile true; do echo HB; sleep 0.2; done\n", false, false, "\n");
        script.setExecutePermission (true);

        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        p.setHelperPath (script);
        auto wav = root.getChildFile ("in2.wav");
        writeWav (wav, sine (440, 0.3, 44100, 1.0), nullptr, 44100);
        p.requestSplit (wav);
        CHECK (waitFor ([&] { return p.getStatus().state == StemPlayerProcessor::JobState::Splitting && p.getStatus().text == "busy"; }, 5000),
               "status text relayed from helper: " << p.getStatus().text);
        const int pid = script.getSiblingFile ("slow.pid").loadFileAsString().trim().getIntValue();
        CHECK (pid > 1 && juce::File ("/proc/" + juce::String (pid)).isDirectory(), "helper process is alive before cancel, pid " << pid);
        const auto t0 = juce::Time::getMillisecondCounter();
        p.cancelJob();
        CHECK (juce::Time::getMillisecondCounter() - t0 < 4000, "cancel returns promptly");
        CHECK (p.getStatus().state == StemPlayerProcessor::JobState::Idle && p.getStatus().text == "Cancelled", "cancelled state");
        juce::Thread::sleep (300);
        CHECK (! juce::File ("/proc/" + juce::String (pid)).exists(), "helper process killed and reaped (no zombie) after cancel");
    }

    // helper that fails -> message surfaces
    {
        auto script = root.getChildFile ("fail_helper.sh");
        script.replaceWithText ("#!/bin/sh\necho 'ERROR model exploded'\nexit 3\n", false, false, "\n");
        script.setExecutePermission (true);
        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        p.setHelperPath (script);
        p.requestSplit (root.getChildFile ("in.wav"));
        CHECK (waitFor ([&] { return p.getStatus().state == StemPlayerProcessor::JobState::Error; }, 5000), "failing helper -> Error");
        CHECK (p.getStatus().text == "model exploded", "ERROR line surfaced: " << p.getStatus().text);
    }

    // real helper end to end (optional)
    if (auto helperPath = juce::SystemStats::getEnvironmentVariable ("STEMPLAYER_TEST_HELPER", {}); helperPath.isNotEmpty())
    {
        std::cout << "[real helper end-to-end]\n";
        auto dir = root.getChildFile ("e2e"); dir.createDirectory();
        const double r = 48000.0;
        auto mix = sine (100.0, 0.3, r, 3.0), t2 = sine (1000.0, 0.2, r, 3.0), t3 = sine (5000.0, 0.1, r, 3.0);
        for (size_t i = 0; i < mix.size(); ++i) mix[i] += t2[i] + t3[i];
        auto audio = dir.getChildFile ("Track.wav");
        writeWav (audio, mix, nullptr, r);

        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (r, 512); p.prepareToPlay (r, 512);
        for (int i = 1; i <= kNumStems; ++i) p.getBus (false, i)->enable (true);
        p.setHelperPath (juce::File (helperPath));

        std::vector<float> progressSeen;
        p.requestSplit (audio);
        bool ok = waitFor ([&]
        {
            auto s = p.getStatus();
            if (s.state == StemPlayerProcessor::JobState::Splitting) progressSeen.push_back (s.progress);
            return s.state == StemPlayerProcessor::JobState::Idle && s.text.startsWith ("Loaded");
        }, 120000);
        CHECK (ok, "split + load completed: " << p.getStatus().text);

        if (ok)
        {
            CHECK (dir.getChildFile ("Track_stems").isDirectory(), "stems folder created next to the source");
            CHECK (p.getTitle() == "Track", "title " << p.getTitle());
            for (int i = 0; i < kNumStems; ++i) CHECK (p.stemPresent (i), "stem present after split " << i);

            FakeHead h; p.setPlayHead (&h);
            auto o = render (p, h, 0, 48000);
            CHECK (goertzelAmp (o[(size_t) auxL (1)].data() + 4800, 38400, 100.0, r) > 0.2, "bass output holds the 100 Hz tone");
            CHECK (goertzelAmp (o[(size_t) auxL (2)].data() + 4800, 38400, 1000.0, r) > 0.1, "other output holds the 1 kHz tone");
            CHECK (goertzelAmp (o[(size_t) auxL (4)].data() + 4800, 38400, 5000.0, r) > 0.05, "guitar output holds the 5 kHz tone");
            p.setPlayHead (nullptr);

            // second request reuses the existing stems instead of re-running the helper
            const auto before = dir.getChildFile ("Track_stems").getChildFile ("stems.json").getLastModificationTime();
            juce::Thread::sleep (1100);
            p.requestSplit (audio);
            CHECK (waitFor ([&] { auto s = p.getStatus(); return s.state == StemPlayerProcessor::JobState::Idle && s.text.startsWith ("Loaded"); }, 60000), "reload existing");
            CHECK (dir.getChildFile ("Track_stems").getChildFile ("stems.json").getLastModificationTime() == before, "existing stems reused");
        }
        CHECK (! progressSeen.empty(), "saw progress while splitting");
    }
}

static void testEditor (const juce::File& root)
{
    std::cout << "[editor]\n";
    StemPlayerProcessor p;
    p.setRateAndBufferSizeDetails (48000.0, 512);
    p.prepareToPlay (48000.0, 512);

    // Regression: the constructor used to call resized() before the stem strips existed (null deref).
    for (int round = 0; round < 3; ++round)
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        CHECK (ed != nullptr, "editor is created");
        if (ed == nullptr) return;

        CHECK (ed->getWidth() > 300 && ed->getHeight() > 200, "editor has a sensible size");
        ed->setSize (900, 520);                           // host-driven resize
        ed->setSize (400, 300);                           // very small
        ed->setSize (760, 470);

        juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        juce::Graphics g (img);
        ed->paintEntireComponent (g, true);               // paint() of every child, headless

        if (round == 1)                                    // with stems loaded, so strips/meters have content
        {
            auto dir = root.getChildFile ("ed_stems"); dir.createDirectory();
            writeWav (dir.getChildFile ("drums.wav"), sine (200, 0.4, 44100, 1.0), nullptr, 44100);
            juce::String err;
            p.loadFolderBlocking (dir, 48000.0, err);
            ed->repaint();
        }

        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);   // lets the 30 Hz timer run
        ed.reset();                                                        // destroyed while a job may be idle
    }
    CHECK (true, "editor lifecycle survived");
}

static void testSafety (const juce::File& root)
{
    std::cout << "[safety: memory guard, state restore, manifest reuse, log]\n";
    const double sr = 44100.0;

    // ---- memory guard (loader + split pre-check) ----
    {
        auto dir = root.getChildFile ("Big_stems"); dir.createDirectory();
        for (int i = 0; i < kNumStems; ++i)
            writeWav (dir.getChildFile (juce::String (kStemIds[i]) + ".wav"), sine (100.0 * (i + 1), 0.3, 44100.0, 2.0), nullptr, 44100.0);

        ::setenv ("STEMPLAYER_MAX_STEM_MB", "1", 1);        // 6 x 2 s stereo float needs ~4 MB
        auto r = loadStemFolder (dir, sr);
        CHECK (r.set == nullptr && r.error.contains ("RAM"), "loader refuses stems that don't fit: " << r.error);

        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        auto wav = root.getChildFile ("toolong.wav");
        writeWav (wav, sine (440, 0.3, 44100, 3.0), nullptr, 44100);
        auto script = root.getChildFile ("never_run.sh");
        script.replaceWithText ("#!/bin/sh\necho ERROR helper-should-not-run\nexit 1\n", false, false, "\n");
        script.setExecutePermission (true);
        p.setHelperPath (script);
        p.requestSplit (wav);
        CHECK (waitFor ([&] { return p.getStatus().state == StemPlayerProcessor::JobState::Error; }, 5000), "split of a too-long file is refused");
        CHECK (p.getStatus().text.contains ("shorter file"), "message tells the user what to do: " << p.getStatus().text);
        ::unsetenv ("STEMPLAYER_MAX_STEM_MB");
    }

    // ---- state restore must not cancel a job or revert stems ----
    {
        auto A = root.getChildFile ("A_stems"); A.createDirectory();
        auto B = root.getChildFile ("B_stems"); B.createDirectory();
        for (int i = 0; i < kNumStems; ++i)
        {
            writeWav (A.getChildFile (juce::String (kStemIds[i]) + ".wav"), sine (200.0, 0.3, 44100.0, 1.0), nullptr, 44100.0);
            writeWav (B.getChildFile (juce::String (kStemIds[i]) + ".wav"), sine (300.0, 0.3, 44100.0, 1.0), nullptr, 44100.0);
        }

        StemPlayerProcessor src;                           // makes a state that points at A
        src.setRateAndBufferSizeDetails (sr, 512); src.prepareToPlay (sr, 512);
        juce::String e;
        CHECK (src.loadFolderBlocking (A, sr, e), "load A");
        juce::MemoryBlock stateA; src.getStateInformation (stateA);

        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        CHECK (p.loadFolderBlocking (B, sr, e), "load B");
        p.setStateInformation (stateA.getData(), (int) stateA.getSize());   // host replays an OLDER state (undo)
        juce::Thread::sleep (300);
        CHECK (p.getTitle() == "B", "restoring older state must not swap the loaded stems back, title is " << p.getTitle());
        CHECK (! p.getStatus().busy(), "no job started by ignored state");

        // a running job is not cancelled by a state restore
        auto slow = root.getChildFile ("slow2.sh");
        slow.replaceWithText ("#!/bin/sh\necho 'STATUS busy2'\nwhile true; do echo HB; sleep 0.2; done\n", false, false, "\n");
        slow.setExecutePermission (true);
        p.setHelperPath (slow);
        auto wav = root.getChildFile ("in3.wav");
        writeWav (wav, sine (440, 0.3, 44100, 1.0), nullptr, 44100);
        p.requestSplit (wav);
        CHECK (waitFor ([&] { return p.getStatus().text == "busy2"; }, 5000), "slow helper running");
        p.setStateInformation (stateA.getData(), (int) stateA.getSize());
        juce::Thread::sleep (300);
        CHECK (p.getStatus().state == StemPlayerProcessor::JobState::Splitting && p.getStatus().text == "busy2", "state restore did not cancel the running split");
        p.prepareToPlay (sr, 512);                         // hosts do this constantly
        juce::Thread::sleep (200);
        CHECK (p.getStatus().state == StemPlayerProcessor::JobState::Splitting, "prepareToPlay did not cancel the running split");
        p.cancelJob();
    }

    // ---- stems from an older helper are re-split; current ones are reused ----
    {
        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        auto fail = root.getChildFile ("fail2.sh");
        fail.replaceWithText ("#!/bin/sh\necho 'ERROR helper was invoked'\nexit 3\n", false, false, "\n");
        fail.setExecutePermission (true);
        p.setHelperPath (fail);

        for (int version : { 0, 2 })
        {
            auto d = root.getChildFile ("reuse" + juce::String (version)); d.createDirectory();
            auto audio = d.getChildFile ("Song.wav");
            writeWav (audio, sine (440, 0.3, 44100, 1.0), nullptr, 44100);
            auto out = d.getChildFile ("Song_stems"); out.createDirectory();
            for (int i = 0; i < kNumStems; ++i)
                writeWav (out.getChildFile (juce::String (kStemIds[i]) + ".wav"), sine (150.0, 0.3, 44100.0, 1.0), nullptr, 44100.0);
            juce::Thread::sleep (1100);                    // manifest must be newer than the audio
            out.getChildFile ("stems.json").replaceWithText (version ? "{\"format_version\": 2, \"preserve_mix\": true}" : "{\"model\": \"htdemucs_6s\"}");

            p.requestSplit (audio);
            if (version == 2)
            {
                CHECK (waitFor ([&] { return p.getStatus().state == StemPlayerProcessor::JobState::Idle && p.getStatus().text.startsWith ("Loaded"); }, 20000),
                       "current-format stems are reused without running the helper: " << p.getStatus().text);
            }
            else
            {
                CHECK (waitFor ([&] { return p.getStatus().state == StemPlayerProcessor::JobState::Error; }, 20000), "old-format stems trigger a re-split");
                CHECK (p.getStatus().text == "helper was invoked", "helper was actually run: " << p.getStatus().text);
            }
        }
    }

    // ---- the log file is written and describes what happened ----
    {
        const auto text = stemplayer::log::file().loadFileAsString();
        CHECK (text.contains ("prepareToPlay"), "log has prepareToPlay");
        CHECK (text.contains ("requestSplit (user)"), "log has requestSplit");
        CHECK (text.contains ("setStateInformation"), "log has setStateInformation decisions");
        CHECK (text.contains ("ignored: 'B' is already loaded") || text.contains ("already loaded"), "log explains an ignored restore");
        CHECK (text.contains ("loader refused"), "log has the memory refusal");
        CHECK (text.contains ("helper start:"), "log has the helper command line");
    }
}

static void testOutputFolder (const juce::File& root)
{
    std::cout << "[output folder]\n";
    const double sr = 44100.0;

    // ---- settings file: round trip, reset, other keys preserved ----
    {
        auto f = stemplayer::settings::file();
        f.getParentDirectory().createDirectory();
        f.replaceWithText ("{ \"somethingElse\": 42 }");
        auto dir = root.getChildFile ("chosen out");
        stemplayer::settings::setOutputFolder (dir);
        CHECK (stemplayer::settings::outputFolder() == dir, "output folder persists: " << stemplayer::settings::outputFolder().getFullPathName());
        CHECK (f.loadFileAsString().contains ("somethingElse"), "unrelated settings are preserved");
        stemplayer::settings::setOutputFolder ({});
        CHECK (stemplayer::settings::outputFolder() == juce::File(), "reset returns to the default");
        f.replaceWithText ("this is not json {{{");                       // corrupt file must not crash or wedge
        CHECK (stemplayer::settings::outputFolder() == juce::File(), "corrupt settings file is treated as default");
        stemplayer::settings::setOutputFolder (dir);
        CHECK (stemplayer::settings::outputFolder() == dir, "a corrupt file is repaired by the next save");
        stemplayer::settings::setOutputFolder ({});
    }

    // ---- an unusable folder is reported, not silently ignored ----
    {
        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        auto notAFolder = root.getChildFile ("iamafile.txt");
        notAFolder.replaceWithText ("x");
        p.setOutputFolder (notAFolder);
        CHECK (p.getOutputFolder() == notAFolder, "processor exposes the setting");
        auto script = root.getChildFile ("unused_helper.sh");
        script.replaceWithText ("#!/bin/sh\necho ERROR must-not-run\nexit 1\n", false, false, "\n");
        script.setExecutePermission (true);
        p.setHelperPath (script);
        auto wav = root.getChildFile ("of_in.wav");
        writeWav (wav, sine (440, 0.3, 44100, 1.0), nullptr, 44100);
        p.requestSplit (wav);
        CHECK (waitFor ([&] { return p.getStatus().state == StemPlayerProcessor::JobState::Error; }, 5000), "unwritable output folder -> Error");
        CHECK (p.getStatus().text.contains ("Can't write to the stems folder"), "message names the problem: " << p.getStatus().text);
        p.setOutputFolder ({});
    }

    // ---- end to end with the real helper (optional) ----
    if (auto helperPath = juce::SystemStats::getEnvironmentVariable ("STEMPLAYER_TEST_HELPER", {}); helperPath.isNotEmpty())
    {
        const double r = 48000.0;
        auto custom = root.getChildFile ("my stems folder");
        auto dirA = root.getChildFile ("albumA"); dirA.createDirectory();
        auto dirB = root.getChildFile ("albumB"); dirB.createDirectory();
        auto a = dirA.getChildFile ("Track.wav"), b = dirB.getChildFile ("Track.wav");   // same file name, different songs
        writeWav (a, sine (300.0, 0.3, r, 2.0), nullptr, r);
        writeWav (b, sine (700.0, 0.3, r, 2.0), nullptr, r);

        StemPlayerProcessor p;
        p.setRateAndBufferSizeDetails (r, 512); p.prepareToPlay (r, 512);
        p.setHelperPath (juce::File (helperPath));
        p.setOutputFolder (custom);

        auto splitAndWait = [&] (const juce::File& f)
        {
            p.requestSplit (f);
            return waitFor ([&] { auto s = p.getStatus(); return (s.state == StemPlayerProcessor::JobState::Idle && s.text.startsWith ("Loaded"))
                                                                  || s.state == StemPlayerProcessor::JobState::Error; }, 120000)
                   && p.getStatus().state == StemPlayerProcessor::JobState::Idle;
        };

        CHECK (splitAndWait (a), "split A into the chosen folder: " << p.getStatus().text);
        auto outA = custom.getChildFile ("Track_stems");
        CHECK (outA.getChildFile ("stems.json").existsAsFile(), "A's stems are in the chosen folder");
        CHECK (! dirA.getChildFile ("Track_stems").exists(), "nothing was written next to the song");
        CHECK (p.getTitle() == "Track", "title " << p.getTitle());

        CHECK (splitAndWait (b), "split B (same file name) into the same folder: " << p.getStatus().text);
        int folders = 0; juce::File outB;
        for (auto& d : custom.findChildFiles (juce::File::findDirectories, false))
            if (d.getChildFile ("stems.json").existsAsFile()) { ++folders; if (d != outA) outB = d; }
        CHECK (folders == 2, "two songs with the same name get two folders, found " << folders);
        CHECK (outB.getFileName().startsWith ("Track [") && outB.getFileName().endsWith ("]_stems"), "second folder is de-duplicated: " << outB.getFileName());
        CHECK (juce::JSON::parse (outA.getChildFile ("stems.json")).getProperty ("source_path", {}).toString() == a.getFullPathName(), "A's manifest records its source");
        CHECK (juce::JSON::parse (outB.getChildFile ("stems.json")).getProperty ("source_path", {}).toString() == b.getFullPathName(), "B's manifest records its source");
        CHECK (p.getTitle() == "Track", "de-duplicated folder still shows the song name: " << p.getTitle());

        const auto before = outA.getChildFile ("stems.json").getLastModificationTime();
        juce::Thread::sleep (1100);
        CHECK (splitAndWait (a), "re-split of A");
        CHECK (outA.getChildFile ("stems.json").getLastModificationTime() == before, "A's existing stems are reused, not re-run");
        CHECK (custom.findChildFiles (juce::File::findDirectories, false).size() == 2, "no extra folder was created");
        p.setOutputFolder ({});
    }
}

static void testPerf (const juce::File& root)
{
    std::cout << "[perf] building 6 x 4 min @44.1k stems...\n";
    auto dir = root.getChildFile ("perf_stems"); dir.createDirectory();
    for (int i = 0; i < kNumStems; ++i)
    {
        auto L = sine (200.0 * (i + 1), 0.3, 44100, 240.0);
        writeWav (dir.getChildFile (juce::String (kStemIds[i]) + ".wav"), L, &L, 44100);
    }
    const auto t0 = juce::Time::getMillisecondCounter();
    auto r = loadStemFolder (dir, 48000.0);
    const auto t1 = juce::Time::getMillisecondCounter();
    std::cout << "  load + resample 44.1k->48k: " << (t1 - t0) / 1000.0 << " s, ok=" << (r.set != nullptr) << ", threads="
              << std::thread::hardware_concurrency() << "\n";
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto root = juce::File::getCurrentWorkingDirectory().getChildFile ("test_tmp");
    root.deleteRecursively();
    root.createDirectory();

    stemplayer::settings::setOutputFolder ({});                 // tests share ~/.config with real use: start from the default
    testResampler();
    testLoaderEdgeCases (root);
    testPlayback (root);
    testJobs (root);
    testEditor (root);
    testSafety (root);
    testOutputFolder (root);
    if (juce::SystemStats::getEnvironmentVariable ("STEMPLAYER_PERF", {}) == "1")
        testPerf (root);

    root.deleteRecursively();
    std::cout << "\n" << (g_checks - g_fail) << "/" << g_checks << " checks passed\n";
    return g_fail == 0 ? 0 : 1;
}
