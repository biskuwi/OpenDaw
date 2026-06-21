#pragma once

namespace OpenDaw {

class OpenDawApplication;

// Headless render inside the full app's Qt + JuceQtBridge + Tracktion loop:
// loads Serum on track 0, drops the .SerumPreset, adds a note, renders to WAV.
//   OpenDaw.exe --serum-render "<preset.SerumPreset>" "<out_dir>"
int runSerumBatchRender(OpenDawApplication& app, int argc, char** argv);

// Headless Serum experiment / render backend entry point.
// Invoked from main() when argv contains "--serum-probe", BEFORE any Qt setup,
// so it runs as a pure-JUCE console process (its own MessageManager, no GUI).
//
// Usage:
//   OpenDaw.exe --serum-probe "<preset.SerumPreset>" "<out_dir>"
//
// It loads ONLY Serum 2 (no full plugin scan), tests whether feeding the raw
// .SerumPreset bytes through setStateInformation reproduces the preset's sound,
// renders before/after WAVs, and writes a JSON report.
int runSerumProbe(int argc, char** argv);

// Autonomous loader test: load a .SerumPreset via a synthesized OLE file-drop
// onto Serum's editor window, then render to WAV.
//   OpenDaw.exe --serum-render "<preset.SerumPreset>" "<out_dir>"
int runSerumRender(int argc, char** argv);

} // namespace OpenDaw
