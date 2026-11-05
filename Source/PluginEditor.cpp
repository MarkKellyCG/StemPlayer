#include "PluginEditor.h"

using namespace stemplayer;

//==============================================================================
StemStrip::StemStrip (StemPlayerProcessor& p, int i)
    : proc (p), index (i),
      faderAtt (p.apvts, "gain_" + juce::String (kStemIds[i]), fader),
      muteAtt  (p.apvts, "mute_" + juce::String (kStemIds[i]), muteButton),
      soloAtt  (p.apvts, "solo_" + juce::String (kStemIds[i]), soloButton)
{
    nameLabel.setText (kStemNames[i], juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (nameLabel);

    fader.setTextValueSuffix (" dB");
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
    addAndMakeVisible (fader);

    for (auto* b : { &muteButton, &soloButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd04040));
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0b020));
    muteButton.setTooltip ("Mute in the Mix output (separate outputs are not affected)");
    soloButton.setTooltip ("Solo in the Mix output (separate outputs are not affected)");
}

void StemStrip::setMeter (float peak)
{
    meter = std::max (peak, meter * 0.88f);
    repaint (meterArea);
}

void StemStrip::resized()
{
    auto r = getLocalBounds().reduced (4);
    nameLabel.setBounds (r.removeFromTop (22));
    auto buttons = r.removeFromBottom (28);
    muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2));
    soloButton.setBounds (buttons.reduced (2));
    meterArea = r.removeFromRight (10).reduced (0, 6).withTrimmedBottom (20);
    fader.setBounds (r);
}

void StemStrip::paint (juce::Graphics& g)
{
    const bool present = proc.stemPresent (index);
    g.setColour (juce::Colour (present ? 0xff2a2d33 : 0xff1d1f23));
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2), 6.0f);

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRect (meterArea);

    const float db = juce::Decibels::gainToDecibels (meter, -60.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    auto bar = meterArea.toFloat();
    bar = bar.withTop (bar.getBottom() - bar.getHeight() * frac);
    g.setColour (db > -1.0f ? juce::Colours::red : db > -9.0f ? juce::Colours::orange : juce::Colour (0xff48c774));
    g.fillRect (bar);

    if (! present)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("no stem", getLocalBounds().withTrimmedTop (26).withHeight (16), juce::Justification::centred);
    }
}

//==============================================================================
StemPlayerEditor::StemPlayerEditor (StemPlayerProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      offsetAtt (p.apvts, "offset", offsetSlider),
      masterAtt (p.apvts, "master", masterSlider)
{
    titleLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    titleLabel.setText ("StemPlayer - drop an audio file or a stems folder here", juce::dontSendNotification);
    addAndMakeVisible (titleLabel);

    statusLabel.setFont (juce::FontOptions (13.0f));
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (statusLabel);

    for (auto* b : { &splitButton, &loadButton, &helperButton, &cancelButton })
        addAndMakeVisible (b);

    addAndMakeVisible (progressBar);
    cancelButton.setVisible (false);
    progressBar.setVisible (false);

    for (int i = 0; i < kNumStems; ++i)
    {
        strips[(size_t) i] = std::make_unique<StemStrip> (proc, i);
        addAndMakeVisible (*strips[(size_t) i]);
    }

    offsetLabel.setText ("Stem offset (s)", juce::dontSendNotification);
    masterLabel.setText ("Mix master (dB)", juce::dontSendNotification);
    addAndMakeVisible (offsetLabel);
    addAndMakeVisible (masterLabel);
    offsetSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
    masterSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
    offsetSlider.setDoubleClickReturnValue (true, 0.0);
    masterSlider.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (offsetSlider);
    addAndMakeVisible (masterSlider);

    splitButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose an audio file to split", juce::File(), "*.wav;*.flac;*.aif;*.aiff;*.ogg;*.mp3");
        juce::Component::SafePointer<StemPlayerEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      safe->proc.requestSplit (fc.getResult());
                              });
    };

    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a folder containing stems");
        juce::Component::SafePointer<StemPlayerEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      safe->proc.requestLoadFolder (fc.getResult());
                              });
    };

    helperButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Locate stemsplit.py (or a stemsplit executable)",
                                                       proc.getHelperPath(), "*.py;*.exe;*");
        juce::Component::SafePointer<StemPlayerEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      safe->proc.setHelperPath (fc.getResult());
                              });
    };

    cancelButton.onClick = [this] { proc.cancelJob(); };

    setSize (760, 470);          // last: triggers resized(), which needs every child to exist

    startTimerHz (30);
    timerCallback();
}

StemPlayerEditor::~StemPlayerEditor()
{
    stopTimer();
}

void StemPlayerEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff16181c));
    g.setColour (juce::Colour (0xff23262c));
    g.fillRect (getLocalBounds().removeFromTop (96));
}

void StemPlayerEditor::resized()
{
    auto r = getLocalBounds().reduced (10);

    auto top = r.removeFromTop (80);
    titleLabel.setBounds (top.removeFromTop (28));
    auto buttons = top.removeFromTop (26);
    splitButton.setBounds (buttons.removeFromLeft (150));
    buttons.removeFromLeft (6);
    loadButton.setBounds (buttons.removeFromLeft (160));
    buttons.removeFromLeft (6);
    helperButton.setBounds (buttons.removeFromLeft (80));
    cancelButton.setBounds (buttons.removeFromRight (80));
    top.removeFromTop (4);
    auto statusRow = top;
    progressBar.setBounds (statusRow.removeFromRight (200));
    statusLabel.setBounds (statusRow);

    r.removeFromTop (14);

    auto bottom = r.removeFromBottom (60);
    auto row1 = bottom.removeFromTop (28);
    offsetLabel.setBounds (row1.removeFromLeft (110));
    offsetSlider.setBounds (row1.removeFromLeft (300));
    auto row2 = bottom.removeFromTop (28);
    masterLabel.setBounds (row2.removeFromLeft (110));
    masterSlider.setBounds (row2.removeFromLeft (300));

    r.removeFromBottom (6);
    const int w = r.getWidth() / kNumStems;
    for (int i = 0; i < kNumStems; ++i)
        if (strips[(size_t) i] != nullptr)
            strips[(size_t) i]->setBounds (r.removeFromLeft (w));
}

void StemPlayerEditor::timerCallback()
{
    for (int i = 0; i < kNumStems; ++i)
    {
        strips[(size_t) i]->setMeter (proc.getMeter (i));
        const bool nowPresent = proc.stemPresent (i);
        const bool wasPresent = (bool) strips[(size_t) i]->getProperties().getWithDefault ("present", false);
        if (nowPresent != wasPresent)
        {
            strips[(size_t) i]->getProperties().set ("present", nowPresent);
            strips[(size_t) i]->repaint();
        }
    }

    const auto st = proc.getStatus();
    const auto title = proc.getTitle();

    titleLabel.setText (title.isNotEmpty() ? title : "StemPlayer - drop an audio file or a stems folder here",
                        juce::dontSendNotification);

    juce::String text = st.text;
    if (st.state == StemPlayerProcessor::JobState::Error)
        statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffff7070));
    else
        statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setText (text.isEmpty() ? juce::String ("Idle. Stems play in sync with the host transport.") : text,
                         juce::dontSendNotification);

    const bool busy = st.busy();
    cancelButton.setVisible (busy);
    progressBar.setVisible (busy);
    splitButton.setEnabled (! busy);
    loadButton.setEnabled (! busy);
    progressValue = st.progress < 0.0f ? -1.0 : (double) st.progress;
}

bool StemPlayerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1;
}

void StemPlayerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.size() == 1)
        handleDropped (juce::File (files[0]));
}

void StemPlayerEditor::handleDropped (const juce::File& f)
{
    if (proc.getStatus().busy())
        return;
    if (f.isDirectory())
        proc.requestLoadFolder (f);
    else if (f.existsAsFile())
        proc.requestSplit (f);
}
