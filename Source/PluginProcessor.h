#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StemTypes.h"
#include "HelperRunner.h"
#include <atomic>
#include <functional>
#include <memory>

class StemPlayerProcessor : public juce::AudioProcessor
{
public:
    enum class JobState { Idle = 0, Splitting, Loading, Error };

    struct Status
    {
        JobState state = JobState::Idle;
        float progress = 0.0f;      // 0..1, or < 0 for indeterminate
        juce::String text;
        bool busy() const { return state == JobState::Splitting || state == JobState::Loading; }
    };

    StemPlayerProcessor();
    ~StemPlayerProcessor() override;

    // --- AudioProcessor ---------------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "StemPlayer"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- Stem API (message thread) ----------------------------------------------------
    /** Load an existing folder of stems (async). */
    void requestLoadFolder (const juce::File& folder);
    /** Run the helper on an audio file, then load the result (async). */
    void requestSplit (const juce::File& audioFile);
    void cancelJob();

    Status getStatus() const;
    juce::String getTitle() const;
    bool stemPresent (int stem) const { return (presentMask.load() >> stem) & 1u; }

    juce::File getHelperPath() const;
    void setHelperPath (const juce::File&);

    float getMeter (int stem) const { return meters[(size_t) stem].load(); }

    // --- test / tooling hooks ---------------------------------------------------------
    /** Synchronously load a folder at `rate` and publish it. */
    bool loadFolderBlocking (const juce::File& folder, double rate, juce::String& error);

    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    static BusesProperties makeBuses();

    struct FnThread;
    void startJob (std::function<void (juce::Thread&)> fn);
    void runLoad (juce::Thread&, juce::File folder, double rate);
    void runSplit (juce::Thread&, juce::File audio);
    void installSet (std::shared_ptr<stemplayer::StemSet> newSet);
    void setJobStatus (JobState, float progress, const juce::String& text);
    void startLoadIfNeeded();

    // parameters
    std::array<std::atomic<float>*, stemplayer::kNumStems> gainP {}, muteP {}, soloP {};
    std::atomic<float>* masterP = nullptr;
    std::atomic<float>* offsetP = nullptr;

    std::array<juce::SmoothedValue<float>, stemplayer::kNumStems> sepSm, mixSm;
    juce::SmoothedValue<float> masterSm;

    // audio-thread view of the loaded stems
    std::atomic<const stemplayer::StemSet*> activeSet { nullptr };
    std::atomic<bool> audioBusy { false };
    std::atomic<juce::uint32> blockCount { 0 };
    std::atomic<juce::uint32> presentMask { 0 };
    std::atomic<double> loadedRate { 0.0 };
    std::array<std::atomic<float>, stemplayer::kNumStems> meters {};
    juce::int64 fallbackPos = 0;

    // ownership of loaded stems (never touched from the audio thread)
    juce::CriticalSection setLock;
    std::shared_ptr<stemplayer::StemSet> ownedSet;
    std::vector<std::shared_ptr<stemplayer::StemSet>> retired;
    juce::String title;

    // settings / job state
    mutable juce::CriticalSection statusLock;
    Status status;
    juce::File desiredFolder, helperPath;
    std::atomic<double> hostRate { 0.0 };
    std::atomic<double> loadingRate { 0.0 };

    juce::CriticalSection jobLock;
    std::unique_ptr<FnThread> job;
    stemplayer::helper::ChildHandle childHandle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPlayerProcessor)
};
