# Drum Gate release notes

## v0.1.0

Drum Gate is a gate for close-mic kick, snare and tom tracks that keeps soft hits and ghost notes while turning bleed down. Kick and snare use a small classifier on the first 8 ms of each hit; toms use level.

- macOS Universal: AU and VST3, arm64 + x86_64.
- Windows x64: VST3, built and tested by the native Windows release workflow; requires the Microsoft Visual C++ x64 Redistributable.
- macOS bundles are ad-hoc signed, not notarized. Windows builds are unsigned.
- macOS checks run: CTest 1/1 (the editor snapshot renders), lipo slices arm64 + x86_64, ad-hoc signatures verified, package integrity verified.

Known limits: the 0 ms Tracking mode opens provisionally for up to 8 ms on every onset; the only latency modes are 0 and 10 ms; toms use level-based decisions; the classifiers were trained on a small set of rock and metal kits, and on a brush-played jazz kit the snare does worse than a conventional gate; tail handling has not been judged by ear.

Each ZIP includes installation instructions, dependency notices and build provenance, with a SHA-256 checksum companion. No DAW-host test has been run.
