#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class StemStrip : public juce::Component
{
public:
    StemStrip (StemPlayerProcessor&, int stemIndex);
    void paint (juce::Graphics&) override;
    void resized() override;
    void setMeter (float peak);

private:
    StemPlayerProcessor& proc;
    int index;
    juce::Label nameLabel;
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    juce::AudioProcessorValueTreeState::SliderAttachment faderAtt;
    juce::AudioProcessorValueTreeState::ButtonAttachment muteAtt, soloAtt;
    juce::Rectangle<int> meterArea;
    float meter = 0.0f;
};

class StemPlayerEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    explicit StemPlayerEditor (StemPlayerProcessor&);
    ~StemPlayerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void timerCallback() override;
    void handleDropped (const juce::File&);

    StemPlayerProcessor& proc;
    std::array<std::unique_ptr<StemStrip>, stemplayer::kNumStems> strips;

    juce::Label titleLabel, statusLabel, offsetLabel, masterLabel;
    juce::TextButton splitButton { "Split audio file..." }, loadButton { "Load stems folder..." },
                     helperButton { "Helper..." }, cancelButton { "Cancel" };
    juce::Slider offsetSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight },
                 masterSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::AudioProcessorValueTreeState::SliderAttachment offsetAtt, masterAtt;

    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };

    std::unique_ptr<juce::FileChooser> chooser;
    StemPlayerProcessor::Status lastStatus;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPlayerEditor)
};
