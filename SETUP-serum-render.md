# SETUP — OpenDaw serum-render additions

This branch (`serum-render`) adds a headless **Serum 2 render backend** used by the
`serum-render-factory` loop-pack pipeline. It is one of three repos; the full end-to-end setup
and prerequisites are in **`serum-render-factory/SETUP.md`**.

## Build
Standard OpenDaw build — see **`CLAUDE.md`** (VS2022 `vcvarsall x64` + CMake/Ninja + Qt 6.10.2 +
vcpkg/FFmpeg). Produces `build\OpenDaw_artefacts\Debug\OpenDaw.exe`. Incremental rebuilds ~15s.

## The Serum render CLIs (added in `src/tools/SerumProbe.cpp` + `main.cpp`)
```
OpenDaw.exe --serum-bake         "<preset.SerumPreset>" "<state.serumstate>"
OpenDaw.exe --serum-render-state "<state.serumstate>" "<midi>" "<out_dir>" <bpm> [loopBars]
OpenDaw.exe --serum-render-midi  "<preset.SerumPreset>" "<midi>" "<out_dir>" <bpm> [loopBars]   # legacy
```
- **`--serum-bake`** drop-loads a preset ONCE (needs a foreground desktop) and dumps Serum's own
  `getStateInformation()` chunk. This is the only step requiring an interactive desktop.
- **`--serum-render-state`** restores that chunk with `setStateInformation` and renders the live
  instance — **no drag-drop, no desktop dependency** (the production path).
- The old `--serum-render-midi` loads via synthetic OLE drag-drop and needs a foreground desktop;
  kept only for baking/diagnostics.

## Machine-specific paths (in `src/tools/SerumProbe.cpp`)
- `kSerumBundle` = `C:\Program Files\Common Files\VST3\Serum2.vst3` — the standard VST3 install
  path; edit if Serum 2 is installed elsewhere.
- The crash-log path (`…\OpenDaw\crux\render_crash.log`) is hardcoded and only written on a render
  crash; it fails harmlessly if the folder is absent (cosmetic, not a run blocker).
