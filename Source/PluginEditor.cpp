#include "PluginEditor.h"

#include <BinaryData.h>

namespace
{
    // Flat, dark, no borders. Pale discs are the things you turn; white and pale type is what
    // is selected; the quieter grey names things. The accent is the gain being applied.
    namespace Theme
    {
        const juce::Colour background { 0xff12142a }, panel { 0xff1c1f3d }, text { 0xffffffff },
                           label { 0xffa9adcc }, accent { 0xff6f7fff }, pale { 0xffdce0ff },
                           field { 0xff363a62 };

        juce::Font make (const juce::Typeface::Ptr& typeface, float height, float tracking)
        {
            return juce::Font (juce::FontOptions (typeface).withHeight (height)).withExtraKerningFactor (tracking);
        }

        /** Labels are upper-case and tracked out; pass upper-case text. */
        juce::Font labelFont (float height = 11.0f)  { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.09f); }
        juce::Font titleFont (float height)          { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.18f); }
        juce::Font bodyFont (float height = 12.0f)   { return make (juce::SharedResourcePointer<EditorFonts>()->inter, height, 0.01f); }
        juce::Font valueFont (float height = 12.0f)  { return make (juce::SharedResourcePointer<EditorFonts>()->mono, height, 0.0f); }

        int widthOf (const juce::Font& font, const juce::String& s)
        {
            return juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (font, s)));
        }
    }

    // Spacing grammar of an option row: sibling options closest, the label further from its
    // first option.
    constexpr int kOptionGap = 10, kLabelGap = 18, kRowHeight = 18;

    const juce::Identifier helpId ("help"), helpTitleId ("helpTitle"), editorScaleId ("editorScale");

    constexpr float kScopeFloorDb = -60.0f;
    constexpr auto kDefaultHelpTitle = "DRUM GATE";
    constexpr auto kDefaultHelp =
        "Opens on hits of this drum, however soft, and turns everything in between down to the Bleed level. "
        "Hover over a control to see what it does; double-click a knob for its default, or its value to type one.";

    float toDb (float linear) noexcept { return 20.0f * std::log10 (std::max (linear, 1.0e-5f)); }

    void setHelp (juce::Component& c, const juce::String& title, const juce::String& help)
    {
        c.getProperties().set (helpTitleId, title.toUpperCase());
        c.getProperties().set (helpId, help);
    }
}

//==============================================================================
EditorFonts::EditorFonts()
    : inter (juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize)),
      mono (juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize))
{
}

//==============================================================================
void FlatLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f;
    const float angle = startAngle + position * (endAngle - startAngle);
    const float dot = juce::jmax (2.5f, radius * 0.11f);

    g.setColour (slider.isMouseOverOrDragging() ? Theme::pale.brighter (0.1f) : Theme::pale);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

    g.setColour (Theme::background);
    g.fillEllipse (juce::Rectangle<float> (dot * 2.0f, dot * 2.0f).withCentre (centre.getPointOnCircumference (radius * 0.72f, angle)));
}

void FlatLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    // Three short diagonals in the label grey, quiet until touched.
    g.setColour (Theme::label.withAlpha (isMouseOver || isMouseDragging ? 0.8f : 0.3f));
    const float size = (float) juce::jmin (w, h);
    for (float i = 0.3f; i < 1.0f; i += 0.3f)
        g.drawLine ((float) w - size * i, (float) h - 2.0f, (float) w - 2.0f, (float) h - size * i, 1.0f);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const char* paramId, const juce::String& nameToUse, const juce::String& help,
            int diameterToUse, float sweepDegrees)
    : name (nameToUse.toUpperCase()), diameter (diameterToUse), attachment (*state.getParameter (paramId), slider)
{
    const float halfSweep = juce::degreesToRadians (sweepDegrees * 0.5f), twoPi = juce::MathConstants<float>::twoPi;
    slider.setRotaryParameters (twoPi - halfSweep, twoPi + halfSweep, true);
    slider.setVelocityModeParameters (0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);   // Shift: fine
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);

    editor.setJustification (juce::Justification::centred);
    editor.setFont (Theme::valueFont (12.0f));
    editor.setIndents (0, 0);
    editor.setColour (juce::TextEditor::backgroundColourId, Theme::field);
    editor.setColour (juce::TextEditor::textColourId, Theme::pale);
    editor.setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.5f));
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.onReturnKey = [this] { finishEditing (true); };
    editor.onEscapeKey = [this] { finishEditing (false); };
    editor.onFocusLost = [this] { finishEditing (true); };
    addChildComponent (editor);

    setHelp (*this, name, help);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (kLabelHeight + kGap - 1);
    slider.setBounds (area.removeFromTop (diameter + 2).withSizeKeepingCentre (diameter + 2, diameter + 2));
    editor.setBounds (getLocalBounds().removeFromBottom (kLabelHeight).withSizeKeepingCentre (64, kLabelHeight));
}

void Knob::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (Theme::label);
    g.setFont (Theme::labelFont());
    g.drawText (name, area.removeFromTop (kLabelHeight), juce::Justification::centred, false);

    g.setFont (Theme::valueFont (12.0f));
    g.drawText (slider.getTextFromValue (slider.getValue()), area.removeFromBottom (kLabelHeight), juce::Justification::centred, false);
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < getHeight() - kLabelHeight - 2)
        return;

    editor.setText (slider.getTextFromValue (slider.getValue()), false);
    editor.setVisible (true);
    editor.selectAll();
    editor.grabKeyboardFocus();
}

void Knob::finishEditing (bool apply)
{
    if (! editor.isVisible())
        return;

    editor.setVisible (false);
    const auto text = editor.getText().trim().toLowerCase();

    // Only text that names a value: the parameters fall back to their defaults otherwise.
    if (apply && text.containsAnyOf ("0123456789"))
        slider.setValue (slider.getValueFromText (text), juce::sendNotificationSync);
    repaint();
}

//==============================================================================
OptionRow::OptionRow (juce::RangedAudioParameter& param, const juce::String& labelToUse, juce::StringArray optionsToUse,
                      const juce::String& help)
    : label (labelToUse.toUpperCase()), options (std::move (optionsToUse)),
      attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); })
{
    labelColumn = labelTextWidth() + kLabelGap;
    attachment.sendInitialUpdate();
    setHelp (*this, label, help);
}

int OptionRow::labelTextWidth() const
{
    return Theme::widthOf (Theme::labelFont(), label);
}

std::vector<juce::Range<int>> OptionRow::optionRanges() const
{
    std::vector<juce::Range<int>> ranges;
    const auto font = Theme::labelFont();
    int x = labelColumn;

    for (const auto& option : options)
    {
        const int width = Theme::widthOf (font, option.toUpperCase());
        ranges.push_back ({ x, x + width });
        x += width + kOptionGap;
    }
    return ranges;
}

int OptionRow::preferredWidth() const
{
    const auto ranges = optionRanges();
    return ranges.empty() ? labelColumn : ranges.back().getEnd();
}

int OptionRow::optionAt (int x) const
{
    const auto ranges = optionRanges();
    for (int i = 0; i < (int) ranges.size(); ++i)
        if (ranges[(size_t) i].expanded (kOptionGap / 2).contains (x))
            return i;
    return -1;
}

void OptionRow::paint (juce::Graphics& g)
{
    g.setFont (Theme::labelFont());
    g.setColour (Theme::label);
    g.drawText (label, 0, 0, labelColumn, getHeight(), juce::Justification::centredLeft, false);

    const auto ranges = optionRanges();
    for (int i = 0; i < options.size(); ++i)
    {
        const auto range = ranges[(size_t) i];
        g.setColour (i == selected ? Theme::pale : (i == hovered ? Theme::text : Theme::label.withAlpha (0.7f)));
        g.drawText (options[i].toUpperCase(), range.getStart(), 0, range.getLength() + 4, getHeight(),
                    juce::Justification::centredLeft, false);
    }
}

void OptionRow::mouseDown (const juce::MouseEvent& e)
{
    if (const int option = optionAt (e.x); option >= 0)
        attachment.setValueAsCompleteGesture ((float) option);
}

void OptionRow::mouseMove (const juce::MouseEvent& e)   { hovered = optionAt (e.x); repaint(); }
void OptionRow::mouseExit (const juce::MouseEvent&)     { hovered = -1; repaint(); }

//==============================================================================
TextAction::TextAction (const juce::String& labelToUse, const juce::String& help)
    : label (labelToUse.toUpperCase())
{
    setHelp (*this, label, help);
}

int TextAction::preferredWidth() const
{
    return Theme::widthOf (Theme::labelFont(), label);
}

void TextAction::setActive (bool shouldBeActive)
{
    if (active != shouldBeActive)
    {
        active = shouldBeActive;
        repaint();
    }
}

void TextAction::paint (juce::Graphics& g)
{
    g.setFont (Theme::labelFont());
    g.setColour (! active ? Theme::label.withAlpha (0.35f) : (isMouseOver() ? Theme::text : Theme::label));
    g.drawText (label, getLocalBounds(), juce::Justification::centredLeft, false);
}

void TextAction::mouseUp (const juce::MouseEvent& e)
{
    if (active && getLocalBounds().contains (e.getPosition()) && onClick != nullptr)
        onClick();
}

//==============================================================================
ScopeDisplay::ScopeDisplay (juce::RangedAudioParameter& bleedParam)
    : bleed (bleedParam),
      attachment (bleedParam, [this] (float v) { bleedDb = v; repaint(); })
{
    attachment.sendInitialUpdate();
    setHelp (*this, "Display", "Input level in the dim fill, what gets through in the light fill, and the gain being applied as the "
                               "blue line. Drag the dashed line to set the Bleed level.");
}

void ScopeDisplay::push (const drumgate::TelemetryFrame& frame) noexcept
{
    frames[(size_t) writeIndex] = frame;
    writeIndex = (writeIndex + 1) % kLength;
}

juce::Rectangle<float> ScopeDisplay::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop (10.0f);
}

float ScopeDisplay::yForDb (float db) const
{
    const auto area = plotArea();
    return juce::jmap (juce::jlimit (kScopeFloorDb, 0.0f, db), kScopeFloorDb, 0.0f, area.getBottom(), area.getY());
}

bool ScopeDisplay::isOverBleedLine (juce::Point<float> p) const
{
    return std::abs (p.y - yForDb (bleedDb)) < 7.0f;
}

void ScopeDisplay::paint (juce::Graphics& g)
{
    const auto area = plotArea();
    g.fillAll (Theme::panel);

    const float dx = area.getWidth() / (float) (kLength - 1);
    const auto frameAt = [this] (int i) -> const drumgate::TelemetryFrame& { return frames[(size_t) ((writeIndex + i) % kLength)]; };

    // Input behind, what gets through in front, and the gain on the same dB scale: fully open
    // is the top edge, closed sits on the bleed line.
    juce::Path input, output, gainLine;
    input.startNewSubPath (area.getX(), area.getBottom());
    output.startNewSubPath (area.getX(), area.getBottom());

    for (int i = 0; i < kLength; ++i)
    {
        const auto& f = frameAt (i);
        const float x = area.getX() + dx * (float) i;
        input.lineTo (x, yForDb (toDb (f.inputPeak)));
        output.lineTo (x, yForDb (toDb (f.inputPeak * f.gain)));

        const float y = yForDb (toDb (f.gain));
        if (i == 0) gainLine.startNewSubPath (x, y); else gainLine.lineTo (x, y);
    }

    input.lineTo (area.getRight(), area.getBottom());
    output.lineTo (area.getRight(), area.getBottom());

    g.setColour (Theme::label.withAlpha (0.22f));
    g.fillPath (input);
    g.setColour (Theme::pale.withAlpha (0.6f));
    g.fillPath (output);

    g.setFont (Theme::valueFont (10.0f));
    for (float db = -12.0f; db > kScopeFloorDb; db -= 12.0f)
    {
        const float y = yForDb (db);
        g.setColour (Theme::label.withAlpha (0.15f));
        g.fillRect (area.getX(), y, area.getWidth(), 1.0f);
        g.setColour (Theme::label.withAlpha (0.7f));
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getRight() - 40.0f, y - 12.0f, 32.0f, 11.0f), juce::Justification::right);
    }

    // The bleed level: a dashed line with an accent handle.
    const float y = yForDb (bleedDb);
    const bool hot = dragging || (isMouseOver() && isOverBleedLine (getMouseXYRelative().toFloat()));
    juce::Path line, dashed;
    line.startNewSubPath (area.getX(), y);
    line.lineTo (area.getRight(), y);
    const float dashes[] { 3.0f, 5.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
    g.setColour (hot ? Theme::text : Theme::label);
    g.fillPath (dashed);

    g.setColour (Theme::accent);
    g.strokePath (gainLine, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered));

    g.fillRect (juce::Rectangle<float> (hot ? 9.0f : 7.0f, hot ? 9.0f : 7.0f).withCentre ({ area.getX() + 10.0f, y }));
    g.setColour (hot ? Theme::text : Theme::label);
    g.setFont (Theme::labelFont (10.0f));
    g.drawText ("BLEED", juce::Rectangle<float> (area.getX() + 20.0f, y + 4.0f, 60.0f, 12.0f), juce::Justification::left);
}

void ScopeDisplay::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (isOverBleedLine (e.position) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void ScopeDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragging = isOverBleedLine (e.position);
    if (dragging)
        attachment.beginGesture();
}

void ScopeDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const auto area = plotArea();
    const float db = juce::jmap (e.position.y, area.getBottom(), area.getY(), kScopeFloorDb, 0.0f);
    attachment.setValueAsPartOfGesture (bleed.getNormalisableRange().snapToLegalValue (juce::jlimit (kScopeFloorDb, 0.0f, db)));
}

void ScopeDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
        attachment.endGesture();
    dragging = false;
}

void ScopeDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isOverBleedLine (e.position))
        attachment.setValueAsCompleteGesture (bleed.convertFrom0to1 (bleed.getDefaultValue()));
}

//==============================================================================
HitDisplay::HitDisplay()
{
    setHelp (*this, "Last hit", "The spectrum of the most recent hit's first 8 ms, from 40 Hz to 16 kHz, and how sure the gate is "
                                "that it was this drum rather than bleed.");
}

void HitDisplay::setState (const DrumGateProcessor::LastHit& newHit)
{
    if (newHit.count != hit.count)
    {
        hit = newHit;
        repaint();
    }
}

void HitDisplay::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    const bool any = hit.count > 0;
    const bool match = hit.probability >= 0.5f;

    auto header = area.removeFromTop (16.0f);
    g.setFont (Theme::labelFont());
    g.setColour (Theme::label);
    g.drawText ("LAST HIT", header, juce::Justification::centredLeft, false);

    // The classifier's confidence: a label and its value.
    const auto value = any ? juce::String (juce::roundToInt (hit.probability * 100.0f)) + "%" : juce::String ("-");
    const auto valueFontToUse = Theme::valueFont (12.0f);
    const float valueWidth = (float) Theme::widthOf (valueFontToUse, value);
    g.setFont (valueFontToUse);
    g.setColour (match ? Theme::pale : Theme::label);
    g.drawText (value, header.removeFromRight (valueWidth), juce::Justification::centredRight, false);
    header.removeFromRight (8.0f);
    g.setFont (Theme::labelFont());
    g.setColour (Theme::label);
    g.drawText ("MATCH", header, juce::Justification::centredRight, false);

    auto scale = area.removeFromBottom (16.0f);
    g.setFont (Theme::valueFont (10.0f));
    g.setColour (Theme::label.withAlpha (0.8f));
    g.drawText ("40", scale, juce::Justification::centredLeft, false);
    g.drawText ("16k Hz", scale, juce::Justification::centredRight, false);

    area.removeFromTop (8.0f);
    const float peak = std::max (1.0e-12f, *std::max_element (hit.spectrum.begin(), hit.spectrum.end()));
    const float slot = area.getWidth() / (float) drumgate::kNumBands;

    for (int b = 0; b < drumgate::kNumBands; ++b)
    {
        // Square root: bars show amplitude, which reads better than energy.
        const float height = any ? juce::jmax (2.0f, area.getHeight() * std::sqrt (hit.spectrum[(size_t) b] / peak)) : 2.0f;
        const float x = area.getX() + slot * (float) b;

        g.setColour (Theme::panel);
        g.fillRect (x, area.getY(), slot - 3.0f, area.getHeight());
        g.setColour (match ? Theme::accent : Theme::label.withAlpha (0.5f));
        g.fillRect (x, area.getBottom() - height, slot - 3.0f, height);
    }
}

//==============================================================================
EditorView::EditorView (DrumGateProcessor& p)
    : gate (p),
      scope (*p.apvts.getParameter (ParamID::bleed)),
      sensitivity (p.apvts, ParamID::sensitivity, "Sensitivity",
                   "How readily a quiet hit opens the gate. Loud hits always do; raise this until the ghost notes come through, "
                   "lower it if other drums get in.", 64),
      bleed (p.apvts, ParamID::bleed, "Bleed",
             "How much of the track stays between hits. 0 dB leaves the track untouched; lower is tighter.", 64),
      length (p.apvts, ParamID::length, "Length",
              "How long the drum rings on after each hit. The attack passes whole; the tail that is kept is the drum's body, "
              "without the cymbals above it.", 64),
      drumRow (*p.apvts.getParameter (ParamID::type), "Drum", { "Kick", "Snare", "Tom" },
               "Which drum this track is. Each has its own idea of what the drum sounds like and what bleed does."),
      latencyRow (*p.apvts.getParameter (ParamID::mode), "Latency", { "0", "10 ms" },
                  "0 ms for tracking: opens at once, and a little bleed slips in at the start of each hit. "
                  "10 ms for mixing: every hit is confirmed first and the gate opens ahead of it."),
      listenRow (*p.apvts.getParameter (ParamID::listen), "Listen", { "Output", "Bleed" },
                 "Bleed plays what is being removed. If you hear the drum itself in there, back off."),
      forgetLevel ("Forget level",
                   "Resets the loudness the gate has learned for this drum, which it keeps with the session. It works it out "
                   "again as the drum plays; use this after changing the track's gain or the drum.")
{
    for (auto* component : std::initializer_list<juce::Component*> { &scope, &sensitivity, &bleed, &length,
                                                                      &drumRow, &latencyRow, &listenRow })
        addAndMakeVisible (component);
    addAndMakeVisible (hitDisplay);
    addAndMakeVisible (forgetLevel);

    sensitivity.setComponentID (ParamID::sensitivity);
    bleed.setComponentID (ParamID::bleed);
    length.setComponentID (ParamID::length);
    drumRow.setComponentID (ParamID::type);
    latencyRow.setComponentID (ParamID::mode);
    listenRow.setComponentID (ParamID::listen);

    // The rows share one label column, so their options start together.
    const int column = juce::jmax (drumRow.labelTextWidth(), latencyRow.labelTextWidth(), listenRow.labelTextWidth()) + kLabelGap;
    for (auto* row : { &drumRow, &latencyRow, &listenRow })
        row->setLabelColumn (column);

    forgetLevel.onClick = [this] { gate.forgetDrumLevel(); };

    showHelpFor (nullptr);
    update();
    setSize (kWidth, kHeight);
}

void EditorView::update()
{
    drumgate::TelemetryFrame frame;
    bool any = false;

    while (gate.popTelemetry (frame))
    {
        scope.push (frame);
        any = true;
    }

    if (any)
        scope.repaint();

    hitDisplay.setState (gate.getLastHit());
    forgetLevel.setActive (gate.knowsDrumLevel());
}

void EditorView::showHelpFor (juce::Component* component)
{
    juce::String title = kDefaultHelpTitle, text = kDefaultHelp;

    // The help belongs to the nearest ancestor that has any (a knob's slider has none itself).
    for (auto* c = component; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (c->getProperties().contains (helpId))
        {
            title = c->getProperties()[helpTitleId].toString();
            text = c->getProperties()[helpId].toString();
            break;
        }
    }

    if (title != helpTitle || text != helpText)
    {
        helpTitle = title;
        helpText = text;
        repaint (helpArea);
    }
}

void EditorView::showHelpForParameter (const juce::String& paramId)
{
    for (auto* child : getChildren())
        if (child->getComponentID() == paramId)
            showHelpFor (child);
}

void EditorView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    g.setColour (Theme::pale);
    g.setFont (Theme::titleFont (15.0f));
    g.drawSingleLineText ("DRUM GATE", 24, 33);

    // Help, bottom left: what the control under the mouse does.
    auto help = helpArea;
    g.setColour (Theme::pale);
    g.setFont (Theme::labelFont());
    g.drawText (helpTitle, help.removeFromTop (16), juce::Justification::topLeft, false);
    g.setColour (Theme::label);
    g.setFont (Theme::bodyFont());
    g.drawFittedText (helpText, help, juce::Justification::topLeft, 3, 1.0f);
}

void EditorView::resized()
{
    constexpr int margin = 24, header = 50, columnGap = 24, sideWidth = 200, knobCell = 104, rowGap = 4;

    auto area = getLocalBounds().reduced (margin, 0);
    area.removeFromTop (header);

    // Bottom: what the control under the mouse does.
    area.removeFromBottom (10);
    helpArea = area.removeFromBottom (54);
    area.removeFromBottom (14);

    // Two columns. Left: the display, the knobs under it. Right: the choices, then the last hit.
    auto side = area.removeFromRight (sideWidth);
    area.removeFromRight (columnGap);

    for (auto* row : { &drumRow, &latencyRow, &listenRow })
    {
        row->setBounds (side.removeFromTop (kRowHeight));
        side.removeFromTop (rowGap);
    }

    side.removeFromTop (16 - rowGap);
    forgetLevel.setBounds (side.removeFromBottom (kRowHeight).withWidth (forgetLevel.preferredWidth()));
    side.removeFromBottom (8);
    hitDisplay.setBounds (side);

    auto knobs = area.removeFromBottom (sensitivity.preferredHeight());
    area.removeFromBottom (16);
    scope.setBounds (area);

    for (auto* knob : { &sensitivity, &bleed, &length })
        knob->setBounds (knobs.removeFromLeft (knobCell).withSizeKeepingCentre (knobCell, knob->preferredHeight()));
}

//==============================================================================
DrumGateEditor::DrumGateEditor (DrumGateProcessor& p)
    : AudioProcessorEditor (p), owner (p), view (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    // Scales as a whole, 0.75x to 2x, keeping the design's aspect ratio.
    constexpr int w = EditorView::kWidth, h = EditorView::kHeight;
    setResizable (true, true);
    setResizeLimits (w * 3 / 4, h * 3 / 4, w * 2, h * 2);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);

    const double scale = juce::jlimit (0.75, 2.0, (double) p.apvts.state.getProperty (editorScaleId, 1.0));
    setSize (juce::roundToInt (w * scale), juce::roundToInt (h * scale));
    startTimerHz (30);
}

DrumGateEditor::~DrumGateEditor()
{
    setLookAndFeel (nullptr);
}

void DrumGateEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void DrumGateEditor::resized()
{
    const float scale = (float) getWidth() / (float) EditorView::kWidth;
    view.setTransform (juce::AffineTransform::scale (scale));
    view.setBounds (0, 0, EditorView::kWidth, EditorView::kHeight);

    // Remember a size the user chose (a window on screen), not the ones a host passes through
    // while opening or closing it.
    if (isShowing() && getPeer() != nullptr)
        owner.apvts.state.setProperty (editorScaleId, (double) scale, nullptr);
}

void DrumGateEditor::timerCallback()
{
    view.update();

    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    view.showHelpFor (under != nullptr && view.isParentOf (under) ? under : nullptr);
}
