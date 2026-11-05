#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StemLoader.h"
#include "HelperRunner.h"

using namespace stemplayer;

namespace
{
    constexpr const char* kPropFolder = "stemsFolder";
    constexpr const char* kPropHelper = "helperPath";
    constexpr float kMinDb = -60.0f;

    float dbToGain (float db) { return db <= kMinDb + 0.05f ? 0.0f : juce::Decibels::decibelsToGain (db); }

    struct BusyGuard
    {
        BusyGuard (std::atomic<bool>& b, std::atomic<juce::uint32>& c) : busy (b), count (c) { busy.store (true); }
        ~BusyGuard() { count.fetch_add (1); busy.store (false); }
        std::atomic<bool>& busy;
        std::atomic<juce::uint32>& count;
    };

    juce::File chooseOutputDir (const juce::File& audio)
    {
        const auto name = audio.getFileNameWithoutExtension() + "_stems";
        auto beside = audio.getParentDirectory().getChildFile (name);

        if (beside.isDirectory() || beside.createDirectory().wasOk())
            if (beside.hasWriteAccess())
                return beside;

        return helper::defaultHelperDirectory().getChildFile ("stems").getChildFile (name);
    }

    bool hasCompleteStems (const juce::File& dir, const juce::File& source)
    {
        auto manifest = dir.getChildFile ("stems.json");
        if (! manifest.existsAsFile() || manifest.getLastModificationTime() < source.getLastModificationTime())
            return false;
        return folderHasStems (dir);
    }
}

struct StemPlayerProcessor::FnThread : public juce::Thread
{
    FnThread (std::function<void (juce::Thread&)> f) : juce::Thread ("StemPlayer job"), fn (std::move (f)) {}
    void run() override { fn (*this); }
    std::function<void (juce::Thread&)> fn;
};

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout StemPlayerProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto dbText = [] (float v, int) { return v <= kMinDb + 0.05f ? String ("-inf") : String (v, 1) + " dB"; };

    auto dbRange = [] { NormalisableRange<float> r (kMinDb, 12.0f, 0.1f); r.setSkewForCentre (-12.0f); return r; };

    for (int i = 0; i < kNumStems; ++i)
    {
        const String id (kStemIds[i]), name (kStemNames[i]);
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain_" + id, 1 }, name + " Gain", dbRange(), 0.0f,
                                                            AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { "mute_" + id, 1 }, name + " Mute", false));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { "solo_" + id, 1 }, name + " Solo", false));
    }

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, "Master Gain", dbRange(), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "offset", 1 }, "Stem Offset",
                                                        NormalisableRange<float> (-120.0f, 120.0f, 0.001f), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("s")));
    return layout;
}

juce::AudioProcessor::BusesProperties StemPlayerProcessor::makeBuses()
{
    auto b = BusesProperties().withOutput ("Mix", juce::AudioChannelSet::stereo(), true);
    for (int i = 0; i < kNumStems; ++i)
        b = b.withOutput (kStemNames[i], juce::AudioChannelSet::stereo(), false);
    return b;
}

StemPlayerProcessor::StemPlayerProcessor()
    : AudioProcessor (makeBuses()),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (int i = 0; i < kNumStems; ++i)
    {
        const juce::String id (kStemIds[i]);
        gainP[(size_t) i] = apvts.getRawParameterValue ("gain_" + id);
        muteP[(size_t) i] = apvts.getRawParameterValue ("mute_" + id);
        soloP[(size_t) i] = apvts.getRawParameterValue ("solo_" + id);
        meters[(size_t) i].store (0.0f);
    }
    masterP = apvts.getRawParameterValue ("master");
    offsetP = apvts.getRawParameterValue ("offset");
}

StemPlayerProcessor::~StemPlayerProcessor()
{
    cancelJob();
    {
        const juce::ScopedLock sl (jobLock);
        job.reset();
    }
    activeSet.store (nullptr);
}

//==============================================================================
bool StemPlayerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (auto& o : layouts.outputBuses)
        if (! o.isDisabled() && o != juce::AudioChannelSet::stereo())
            return false;

    return true;
}

void StemPlayerProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate.store (sampleRate);

    for (int i = 0; i < kNumStems; ++i)
    {
        auto& s = sepSm[(size_t) i];
        auto& m = mixSm[(size_t) i];
        s.reset (sampleRate, 0.02);
        m.reset (sampleRate, 0.02);

        const float g = dbToGain (gainP[(size_t) i]->load());
        s.setCurrentAndTargetValue (g);
        m.setCurrentAndTargetValue (muteP[(size_t) i]->load() > 0.5f ? 0.0f : g);
    }
    masterSm.reset (sampleRate, 0.02);
    masterSm.setCurrentAndTargetValue (dbToGain (masterP->load()));
    fallbackPos = 0;

    startLoadIfNeeded();
}

void StemPlayerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    buffer.clear();

    BusyGuard guard (audioBusy, blockCount);

    const int numSamples = buffer.getNumSamples();
    const double sr = getSampleRate();
    const StemSet* set = activeSet.load();

    // ---- transport -------------------------------------------------------------------
    bool playing = false;
    juce::int64 hostPos = 0;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();

            if (auto ts = pos->getTimeInSamples())
                hostPos = *ts;
            else if (auto t = pos->getTimeInSeconds())
                hostPos = (juce::int64) std::llround (*t * sr);
            else if (playing)
            {
                hostPos = fallbackPos;     // host gives no timeline: free-run from start of play
                fallbackPos += numSamples;
            }
        }
    }
    if (! playing)
        fallbackPos = 0;

    const juce::int64 readPos = hostPos - (juce::int64) std::llround ((double) offsetP->load() * sr);

    // ---- gains ------------------------------------------------------------------------
    bool anySolo = false;
    for (int i = 0; i < kNumStems; ++i)
        anySolo = anySolo || soloP[(size_t) i]->load() > 0.5f;

    for (int i = 0; i < kNumStems; ++i)
    {
        const float g = dbToGain (gainP[(size_t) i]->load());
        const bool audible = anySolo ? (soloP[(size_t) i]->load() > 0.5f) : (muteP[(size_t) i]->load() < 0.5f);
        sepSm[(size_t) i].setTargetValue (g);
        mixSm[(size_t) i].setTargetValue (audible ? g : 0.0f);
    }
    masterSm.setTargetValue (dbToGain (masterP->load()));

    // ---- render -----------------------------------------------------------------------
    auto mixBus = getBusBuffer (buffer, false, 0);
    float* mixL = mixBus.getNumChannels() > 0 ? mixBus.getWritePointer (0) : nullptr;
    float* mixR = mixBus.getNumChannels() > 1 ? mixBus.getWritePointer (1) : nullptr;

    for (int s = 0; s < kNumStems; ++s)
    {
        auto bus = getBusBuffer (buffer, false, 1 + s);
        float* sepL = bus.getNumChannels() > 0 ? bus.getWritePointer (0) : nullptr;
        float* sepR = bus.getNumChannels() > 1 ? bus.getWritePointer (1) : nullptr;

        const bool have = set != nullptr && set->present[(size_t) s];
        if (! have || ! playing)
        {
            sepSm[(size_t) s].skip (numSamples);
            mixSm[(size_t) s].skip (numSamples);
            meters[(size_t) s].store (0.0f);
            continue;
        }

        const auto& A = set->audio[(size_t) s];
        const float* srcL = A.getReadPointer (0);
        const float* srcR = A.getReadPointer (1);
        const juce::int64 len = A.getNumSamples();
        float peak = 0.0f;

        for (int j = 0; j < numSamples; ++j)
        {
            const float sg = sepSm[(size_t) s].getNextValue();
            const float mg = mixSm[(size_t) s].getNextValue();
            const juce::int64 idx = readPos + j;
            if (idx < 0 || idx >= len)
                continue;

            const float l = srcL[idx], r = srcR[idx];
            if (sepL) sepL[j] = l * sg;
            if (sepR) sepR[j] = r * sg;
            if (mixL) mixL[j] += l * mg;
            if (mixR) mixR[j] += r * mg;
            peak = std::max ({ peak, std::abs (l * sg), std::abs (r * sg) });
        }
        meters[(size_t) s].store (peak);
    }

    for (int j = 0; j < numSamples; ++j)
    {
        const float g = masterSm.getNextValue();
        if (mixL) mixL[j] *= g;
        if (mixR) mixR[j] *= g;
    }
}

//==============================================================================
void StemPlayerProcessor::installSet (std::shared_ptr<StemSet> newSet)
{
    std::shared_ptr<StemSet> old;
    juce::uint32 mask = 0;
    for (int i = 0; i < kNumStems; ++i)
        if (newSet->present[(size_t) i]) mask |= (1u << i);

    const auto c0 = blockCount.load();
    {
        const juce::ScopedLock sl (setLock);
        old = std::move (ownedSet);
        ownedSet = newSet;
        title = newSet->title;
        presentMask.store (mask);
        loadedRate.store (newSet->sampleRate);
        activeSet.store (newSet.get());
    }

    // Don't free the old set while a block that began before the swap may still be reading it.
    int waited = 0;
    while (blockCount.load() == c0 && audioBusy.load() && waited < 3000)
    {
        juce::Thread::sleep (5);
        waited += 5;
    }

    if (old != nullptr && blockCount.load() == c0 && audioBusy.load())
    {
        const juce::ScopedLock sl (setLock);
        retired.push_back (std::move (old));   // audio thread looks stuck: leak rather than crash
    }
}

bool StemPlayerProcessor::loadFolderBlocking (const juce::File& folder, double rate, juce::String& error)
{
    auto res = loadStemFolder (folder, rate);
    if (res.set == nullptr)
    {
        error = res.error;
        return false;
    }
    {
        const juce::ScopedLock sl (statusLock);
        desiredFolder = folder;
    }
    installSet (std::shared_ptr<StemSet> (std::move (res.set)));
    return true;
}

//==============================================================================
void StemPlayerProcessor::setJobStatus (JobState s, float progress, const juce::String& text)
{
    const juce::ScopedLock sl (statusLock);
    status.state = s;
    status.progress = progress;
    status.text = text;
}

StemPlayerProcessor::Status StemPlayerProcessor::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

juce::String StemPlayerProcessor::getTitle() const
{
    const juce::ScopedLock sl (setLock);
    return title;
}

juce::File StemPlayerProcessor::getHelperPath() const
{
    const juce::ScopedLock sl (statusLock);
    return helperPath;
}

void StemPlayerProcessor::setHelperPath (const juce::File& f)
{
    const juce::ScopedLock sl (statusLock);
    helperPath = f;
}

void StemPlayerProcessor::startJob (std::function<void (juce::Thread&)> fn)
{
    const juce::ScopedLock sl (jobLock);
    if (job != nullptr)
    {
        job->signalThreadShouldExit();
        childHandle.kill();
        job->stopThread (30000);
        job.reset();
    }
    job = std::make_unique<FnThread> (std::move (fn));
    job->startThread();
}

void StemPlayerProcessor::cancelJob()
{
    const juce::ScopedLock sl (jobLock);
    if (job != nullptr)
    {
        job->signalThreadShouldExit();
        childHandle.kill();
        job->stopThread (30000);
        job.reset();
    }
    if (getStatus().busy())
        setJobStatus (JobState::Idle, 0.0f, "Cancelled");
}

void StemPlayerProcessor::requestLoadFolder (const juce::File& folder)
{
    if (getStatus().busy())
        cancelJob();                              // an explicit user request replaces whatever is running

    {
        const juce::ScopedLock sl (statusLock);
        desiredFolder = folder;
    }
    startLoadIfNeeded();
}

void StemPlayerProcessor::startLoadIfNeeded()
{
    juce::File folder;
    {
        const juce::ScopedLock sl (statusLock);
        folder = desiredFolder;
    }
    const double rate = hostRate.load();
    if (folder == juce::File() || rate <= 0.0)
        return;                                   // will be retried from prepareToPlay

    if (getStatus().state == JobState::Splitting)
        return;                                   // runSplit loads at the then-current host rate when it finishes

    bool haveSameRate = false;
    {
        const juce::ScopedLock sl (setLock);
        haveSameRate = std::abs (loadedRate.load() - rate) < 0.5 && ownedSet != nullptr && ownedSet->folder == folder;
    }
    if (haveSameRate || std::abs (loadingRate.load() - rate) < 0.5)
        return;

    loadingRate.store (rate);
    setJobStatus (JobState::Loading, 0.0f, "Loading stems...");     // visible immediately, before the thread runs
    startJob ([this, folder, rate] (juce::Thread& t) { runLoad (t, folder, rate); });
}

void StemPlayerProcessor::requestSplit (const juce::File& audio)
{
    loadingRate.store (0.0);
    setJobStatus (JobState::Splitting, -1.0f, "Starting...");        // visible immediately, before the thread runs
    startJob ([this, audio] (juce::Thread& t) { runSplit (t, audio); });
}

void StemPlayerProcessor::runLoad (juce::Thread& t, juce::File folder, double rate)
{
    setJobStatus (JobState::Loading, 0.0f, "Loading stems...");

    auto res = loadStemFolder (folder, rate, [this, &t] (float p)
    {
        setJobStatus (JobState::Loading, p, "Loading stems...");
        return ! t.threadShouldExit();
    });

    loadingRate.store (0.0);

    if (res.set == nullptr)
    {
        if (res.error == "Cancelled") setJobStatus (JobState::Idle, 0.0f, "Cancelled");
        else                          setJobStatus (JobState::Error, 0.0f, res.error);
        return;
    }

    const auto name = res.set->title;
    installSet (std::shared_ptr<StemSet> (std::move (res.set)));
    setJobStatus (JobState::Idle, 1.0f, "Loaded: " + name);
}

void StemPlayerProcessor::runSplit (juce::Thread& t, juce::File audio)
{
    setJobStatus (JobState::Splitting, -1.0f, "Looking for helper...");

    if (! audio.existsAsFile())
    {
        setJobStatus (JobState::Error, 0.0f, "File not found: " + audio.getFullPathName());
        return;
    }

    const auto helperFile = helper::findHelper (getHelperPath());
    if (! helperFile.existsAsFile())
    {
        setJobStatus (JobState::Error, 0.0f,
                      "Splitter helper not found. Install it (see README) or click 'Helper...' and pick stemsplit.py.");
        return;
    }

    const auto outDir = chooseOutputDir (audio);

    if (! hasCompleteStems (outDir, audio))
    {
        outDir.createDirectory();
        const auto cmd = helper::buildCommand (helperFile, audio, outDir);

        auto res = helper::run (cmd,
            [this] (const juce::String& kind, const juce::String& text)
            {
                if (kind == "PROGRESS")
                {
                    const auto cur = getStatus();
                    setJobStatus (JobState::Splitting, (float) text.getDoubleValue(), cur.text);
                }
                else if (kind == "STATUS")
                {
                    const auto cur = getStatus();
                    setJobStatus (JobState::Splitting, cur.progress, text);
                }
            },
            [&t] { return t.threadShouldExit(); },
            &childHandle);

        if (res.cancelled) { setJobStatus (JobState::Idle, 0.0f, "Cancelled"); return; }
        if (! res.ok)      { setJobStatus (JobState::Error, 0.0f, res.message); return; }
    }

    {
        const juce::ScopedLock sl (statusLock);
        desiredFolder = outDir;
    }

    const double rate = hostRate.load() > 0.0 ? hostRate.load() : 44100.0;
    loadingRate.store (rate);
    runLoad (t, outDir, rate);
}

//==============================================================================
void StemPlayerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock sl (statusLock);
        state.setProperty (kPropFolder, desiredFolder.getFullPathName(), nullptr);
        state.setProperty (kPropHelper, helperPath.getFullPathName(), nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void StemPlayerProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        if (! xml->hasTagName (apvts.state.getType()))
            return;

        auto vt = juce::ValueTree::fromXml (*xml);
        apvts.replaceState (vt);

        juce::File folder, helperFile;
        if (vt.hasProperty (kPropFolder) && vt[kPropFolder].toString().isNotEmpty())
            folder = juce::File (vt[kPropFolder].toString());
        if (vt.hasProperty (kPropHelper) && vt[kPropHelper].toString().isNotEmpty())
            helperFile = juce::File (vt[kPropHelper].toString());

        setHelperPath (helperFile);
        if (folder != juce::File() && folder.isDirectory())
            requestLoadFolder (folder);
    }
}

juce::AudioProcessorEditor* StemPlayerProcessor::createEditor()
{
    return new StemPlayerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StemPlayerProcessor();
}
