// Renders the plug-in editor to a PNG without a host or a screen, after running some of the
// synthetic snare scene through it so the display has content.
//
//   drumgate_snapshot out.png

#include "../Source/PluginEditor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "editor.png");

    std::unique_ptr<juce::AudioProcessor> processor (createPluginFilter());
    processor->setPlayConfigDetails (1, 1, 48000.0, 512);
    processor->prepareToPlay (48000.0, 512);

    // Full hits at 0 dB, ghosts at -20 dB, and hi-hat-like noise bursts in between.
    juce::Random random (1);
    juce::AudioBuffer<float> audio (1, 48000 * 4);
    audio.clear();

    const auto burst = [&] (double time, float amp, bool isHat)
    {
        const int start = (int) (time * 48000.0);
        for (int n = 0; n < 6000 && start + n < audio.getNumSamples(); ++n)
        {
            const float t = (float) n / 48000.0f;
            const float noise = random.nextFloat() * 2.0f - 1.0f;
            const float v = isHat ? noise * std::exp (-t / 0.02f) * (n % 2 == 0 ? 1.0f : -1.0f)
                                  : std::sin (6.2832f * 200.0f * t) * std::exp (-t / 0.05f) + 0.5f * noise * std::exp (-t / 0.05f);
            audio.addSample (0, start + n, amp * v);
        }
    };

    for (int i = 0; i < 16; ++i)
    {
        const double t = 0.1 + i * 0.24;
        if (i % 4 == 2)             burst (t, 0.8f, false);
        else if (i % 4 == 3)        burst (t, 0.08f, false);
        if (i % 2 == 0)             burst (t + 0.12, 0.12f, true);
    }

    juce::MidiBuffer midi;
    auto& gate = dynamic_cast<DrumGateProcessor&> (*processor);
    DrumGateEditor editor (gate);

    for (int pos = 0; pos + 512 <= audio.getNumSamples(); pos += 512)
    {
        juce::AudioBuffer<float> block (audio.getArrayOfWritePointers(), 1, pos, 512);
        processor->processBlock (block, midi);

        drumgate::TelemetryFrame frame;
        while (gate.popTelemetry (frame))
            editor.pushTelemetryForSnapshot (frame);
    }

    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);   // let the editor's timer catch up
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
    output.deleteFile();
    juce::FileOutputStream out (output);
    return juce::PNGImageFormat().writeImageToStream (image, out) ? 0 : 1;
}
