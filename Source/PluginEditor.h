#pragma once

#include "PluginProcessor.h"

/** The embedded typefaces. Shared by all open editors and released with the last one, so
    nothing outlives the host's JUCE shutdown. */
struct EditorFonts
{
    EditorFonts();
    juce::Typeface::Ptr inter, mono;
};

/** Flat drawing for the knobs and the resize corner; everything else paints itself. */
class FlatLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

/** A flat disc with a dot, its name above and its value below. Double-click the disc for the
    default; double-click the value to type one. */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const char* paramId, const juce::String& name, const juce::String& help,
          int diameter, float sweepDegrees = 270.0f);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int kLabelHeight = 16, kGap = 8;
    /** Label, disc and value. */
    int preferredHeight() const noexcept { return 2 * (kLabelHeight + kGap) + diameter; }

private:
    void finishEditing (bool apply);

    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::String name;
    int diameter;
    juce::TextEditor editor;
    juce::SliderParameterAttachment attachment;
};

/** "LABEL  OPTION  OPTION ..." as plain text; the selected option is lit. */
class OptionRow final : public juce::Component
{
public:
    OptionRow (juce::RangedAudioParameter&, const juce::String& label, juce::StringArray options, const juce::String& help);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** The label's own width; rows stacked together share the widest plus the label gap. */
    int labelTextWidth() const;
    void setLabelColumn (int width) { labelColumn = width; repaint(); }
    int preferredWidth() const;

private:
    std::vector<juce::Range<int>> optionRanges() const;
    int optionAt (int x) const;

    juce::String label;
    juce::StringArray options;
    int labelColumn = 0, selected = 0, hovered = -1;
    juce::ParameterAttachment attachment;
};

/** A plain-text action in the option-row style: label grey, white under the mouse. */
class TextAction final : public juce::Component
{
public:
    TextAction (const juce::String& label, const juce::String& help);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    void setActive (bool);
    int preferredWidth() const;

    std::function<void()> onClick;

private:
    juce::String label;
    bool active = true;
};

/** Scrolling view of the input level, what gets through, and the gain applied. The bleed
    level is a line on the same dB scale and can be dragged. */
class ScopeDisplay final : public juce::Component
{
public:
    explicit ScopeDisplay (juce::RangedAudioParameter& bleedParam);

    void push (const drumgate::TelemetryFrame& frame) noexcept;
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plotArea() const;
    float yForDb (float db) const;
    bool isOverBleedLine (juce::Point<float>) const;

    static constexpr int kLength = 1600; // 4 s at the engine's telemetry rate
    std::array<drumgate::TelemetryFrame, kLength> frames {};
    int writeIndex = 0;

    juce::RangedAudioParameter& bleed;
    float bleedDb = -24.0f;
    bool dragging = false;
    juce::ParameterAttachment attachment;
};

/** What the detector made of the most recent hit: its spectrum and how sure the classifier was. */
class HitDisplay final : public juce::Component
{
public:
    HitDisplay();

    void setState (const DrumGateProcessor::LastHit& hit);
    void paint (juce::Graphics&) override;

private:
    DrumGateProcessor::LastHit hit;
};

/** Everything inside the window, laid out at the design size; the editor scales it. */
class EditorView final : public juce::Component
{
public:
    static constexpr int kWidth = 600, kHeight = 440;

    explicit EditorView (DrumGateProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Reads the processor's telemetry and last hit into the displays. */
    void update();
    void push (const drumgate::TelemetryFrame& frame) noexcept { scope.push (frame); }
    /** Shows the help of the control under the mouse (nullptr: the default text). */
    void showHelpFor (juce::Component*);
    /** Shows the help of the control for this parameter ID. */
    void showHelpForParameter (const juce::String& paramId);

private:
    DrumGateProcessor& gate;
    ScopeDisplay scope;
    HitDisplay hitDisplay;
    Knob sensitivity, bleed, length;
    OptionRow drumRow, latencyRow, listenRow;
    TextAction forgetLevel;

    juce::String helpTitle, helpText;
    juce::Rectangle<int> helpArea;
};

class DrumGateEditor final : public juce::AudioProcessorEditor,
                             private juce::Timer
{
public:
    explicit DrumGateEditor (DrumGateProcessor&);
    ~DrumGateEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** For the offline snapshot tool, which has no host message loop or mouse. */
    void pushTelemetryForSnapshot (const drumgate::TelemetryFrame& frame) { view.push (frame); }
    void showHelpForParameter (const juce::String& paramId) { view.showHelpForParameter (paramId); }

private:
    void timerCallback() override;

    DrumGateProcessor& owner;
    juce::SharedResourcePointer<EditorFonts> fonts;
    FlatLookAndFeel lookAndFeel;
    EditorView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumGateEditor)
};
