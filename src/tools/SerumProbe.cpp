#include "tools/SerumProbe.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include "app/OpenDawApplication.h"
#include "engine/EditManager.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace te = tracktion::engine;

#ifdef _WIN32
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <oleidl.h>
#include <crtdbg.h>
#include <csignal>
#include <exception>
#include <cstdlib>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#ifndef MK_LEFTBUTTON
#define MK_LEFTBUTTON 0x0001
#endif
#ifndef DROPEFFECT_COPY
#define DROPEFFECT_COPY 1
#endif
#endif

namespace OpenDaw {

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int    kBlockSize  = 512;
constexpr double kDuration   = 3.0;   // seconds
constexpr double kGate       = 1.0;   // note-off time
constexpr int    kMidiNote   = 36;    // C2
constexpr int    kVelocity   = 100;

// JUCE's VST3 format wants the bundle root (it resolves Contents/x86_64-win itself).
const char* kSerumBundle = R"(C:\Program Files\Common Files\VST3\Serum2.vst3)";
const char* kSerumInner  = R"(C:\Program Files\Common Files\VST3\Serum2.vst3\Contents\x86_64-win\Serum2.vst3)";

juce::File g_logFile;
void flog(const juce::String& s)
{
    std::cout << s << std::endl;
    if (g_logFile.getFullPathName().isNotEmpty())
        g_logFile.appendText(s + "\n");
}

juce::String markerSummary(const juce::MemoryBlock& mb)
{
    auto has = [&](const char* needle) {
        auto n = (int) std::strlen(needle);
        auto* d = static_cast<const char*>(mb.getData());
        int sz = (int) mb.getSize();
        for (int i = 0; i + n <= sz; ++i)
            if (std::memcmp(d + i, needle, (size_t) n) == 0) return true;
        return false;
    };
    juce::StringArray found;
    if (has("XfsX")) found.add("XfsX");
    if (has("Serum")) found.add("Serum");
    if (has("Xfer")) found.add("Xfer");
    if (has("XferJson")) found.add("XferJson");
    // zstd magic 0x28 0xB5 0x2F 0xFD
    {
        auto* d = static_cast<const unsigned char*>(mb.getData());
        int sz = (int) mb.getSize();
        for (int i = 0; i + 4 <= sz; ++i)
            if (d[i]==0x28 && d[i+1]==0xB5 && d[i+2]==0x2F && d[i+3]==0xFD) { found.add("zstd"); break; }
    }
    return found.isEmpty() ? juce::String("none") : found.joinIntoString(",");
}

// Render `inst` to a stereo buffer: noteOn at t=0, noteOff at kGate, capture kDuration.
juce::AudioBuffer<float> renderNote(juce::AudioPluginInstance& inst, int midiNote = kMidiNote)
{
    inst.prepareToPlay(kSampleRate, kBlockSize);
    const int total = (int) (kSampleRate * kDuration);
    const int offAt = (int) (kSampleRate * kGate);
    juce::AudioBuffer<float> out(2, total);
    out.clear();

    juce::AudioBuffer<float> block(2, kBlockSize);
    int pos = 0;
    while (pos < total)
    {
        const int n = juce::jmin(kBlockSize, total - pos);
        block.setSize(2, n, false, false, true);
        block.clear();

        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, midiNote, (juce::uint8) kVelocity), 0);
        if (offAt >= pos && offAt < pos + n)
            midi.addEvent(juce::MidiMessage::noteOff(1, midiNote), offAt - pos);

        inst.processBlock(block, midi);

        for (int ch = 0; ch < 2; ++ch)
        {
            const int src = juce::jmin(ch, block.getNumChannels() - 1);
            out.copyFrom(ch, pos, block, src, 0, n);
        }
        pos += n;
    }
    return out;
}

float rms(const juce::AudioBuffer<float>& b)
{
    if (b.getNumSamples() == 0) return 0.0f;
    return b.getRMSLevel(0, 0, b.getNumSamples());
}

void writeWav(const juce::File& f, const juce::AudioBuffer<float>& buf)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    if (auto* os = f.createOutputStream().release())
    {
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(os, kSampleRate, 2, 24, {}, 0));
        if (writer) writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        else delete os;
    }
}

void log(const juce::String& s) { flog(s); qDebug().noquote() << s.toRawUTF8(); }

// ---- MIDI-clip printing ----------------------------------------------------
// Parse a .mid to a time-sorted event list in SECONDS at the given tempo, then
// render the whole clip through the LIVE Serum instance (+ release tail). The MT
// clips carry no reliable tempo meta, so we force `bpm` from the pack manifest.
struct TimedMidi { double timeSec; juce::MidiMessage msg; };

std::vector<TimedMidi> loadMidiClip(const juce::File& midiFile, double bpm, double& clipEndSec)
{
    std::vector<TimedMidi> events;
    clipEndSec = 0.0;
    juce::FileInputStream in(midiFile);
    log("[midi] open ok=" + juce::String(in.openedOk() ? 1 : 0) + " " + midiFile.getFullPathName());
    if (!in.openedOk()) { log("[midi] cannot open " + midiFile.getFullPathName()); return events; }
    juce::MidiFile mf;
    const bool rd = mf.readFrom(in);
    log("[midi] readFrom=" + juce::String(rd ? 1 : 0)
        + " tracks=" + juce::String(mf.getNumTracks())
        + " timeFormat=" + juce::String(mf.getTimeFormat()));
    if (!rd) { log("[midi] readFrom failed"); return events; }

    const short tf = mf.getTimeFormat();
    const double tpq = tf > 0 ? (double) tf : 96.0;      // ticks per quarter note
    const double secPerTick = 60.0 / (bpm * tpq);

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* seq = mf.getTrack(t);
        for (int e = 0; e < seq->getNumEvents(); ++e)
        {
            const auto& m = seq->getEventPointer(e)->message;
            const bool keep = m.isNoteOnOrOff() || m.isController()
                            || m.isPitchWheel()  || m.isAftertouch()
                            || m.isChannelPressure();
            if (!keep) continue;
            const double ts = m.getTimeStamp() * secPerTick;
            juce::MidiMessage mm = m;
            mm.setTimeStamp(ts);
            events.push_back({ ts, mm });
            if (m.isNoteOnOrOff()) clipEndSec = juce::jmax(clipEndSec, ts);
        }
    }
    std::sort(events.begin(), events.end(),
              [](const TimedMidi& a, const TimedMidi& b) { return a.timeSec < b.timeSec; });
    return events;
}

juce::AudioBuffer<float> renderMidiClip(juce::AudioPluginInstance& inst,
                                        const std::vector<TimedMidi>& events,
                                        double clipEndSec, double tailSec)
{
    inst.prepareToPlay(kSampleRate, kBlockSize);
    const int total = (int) (kSampleRate * (clipEndSec + tailSec));
    juce::AudioBuffer<float> out(2, total);
    out.clear();

    juce::AudioBuffer<float> block(2, kBlockSize);
    size_t idx = 0;
    int pos = 0;
    while (pos < total)
    {
        const int n = juce::jmin(kBlockSize, total - pos);
        block.setSize(2, n, false, false, true);
        block.clear();

        juce::MidiBuffer midi;
        while (idx < events.size())
        {
            const int samp = (int) (events[idx].timeSec * kSampleRate);
            if (samp >= pos + n) break;
            midi.addEvent(events[idx].msg, juce::jmax(0, samp - pos));
            ++idx;
        }

        inst.processBlock(block, midi);
        for (int ch = 0; ch < 2; ++ch)
        {
            const int src = juce::jmin(ch, block.getNumChannels() - 1);
            out.copyFrom(ch, pos, block, src, 0, n);
        }
        pos += n;
    }
    return out;
}

// Render `events` continuously into a fresh 2ch buffer of exactly `nSamples`,
// feeding note/CC events at their sample positions. Shared engine for clip/loop.
static void renderEventsInto(juce::AudioPluginInstance& inst,
                             const std::vector<TimedMidi>& events,
                             juce::AudioBuffer<float>& out)
{
    const int total = out.getNumSamples();
    juce::AudioBuffer<float> block(2, kBlockSize);
    size_t idx = 0;
    int pos = 0;
    while (pos < total)
    {
        const int n = juce::jmin(kBlockSize, total - pos);
        block.setSize(2, n, false, false, true);
        block.clear();

        juce::MidiBuffer midi;
        while (idx < events.size())
        {
            const int samp = (int) (events[idx].timeSec * kSampleRate);
            if (samp >= pos + n) break;
            midi.addEvent(events[idx].msg, juce::jmax(0, samp - pos));
            ++idx;
        }

        inst.processBlock(block, midi);
        for (int ch = 0; ch < 2; ++ch)
        {
            const int src = juce::jmin(ch, block.getNumChannels() - 1);
            out.copyFrom(ch, pos, block, src, 0, n);
        }
        pos += n;
    }
}

// Seamless loop render. Play the pattern TWICE back-to-back and render
// 2*loopLen + a wrap tail. Keep the 2nd period [L,2L] (by then reverb has built
// up AND the 1st period's tail bleeds into its start), then overlap-add the
// spillover ringing past 2L back onto the loop's start so the loop's own end
// tail continues seamlessly into its beginning. Output is EXACTLY loopLen long
// with NO trailing tail — a clean bar-locked loop.
juce::AudioBuffer<float> renderMidiLoop(juce::AudioPluginInstance& inst,
                                        const std::vector<TimedMidi>& events,
                                        double loopLenSec)
{
    constexpr double kWrapSec = 4.0;   // tail captured past the loop end for wrap-around
    inst.prepareToPlay(kSampleRate, kBlockSize);

    // Two passes: original + a copy shifted by exactly one loop length.
    std::vector<TimedMidi> two;
    two.reserve(events.size() * 2);
    for (const auto& e : events) two.push_back(e);
    for (const auto& e : events)
    {
        TimedMidi c = e;
        c.timeSec = e.timeSec + loopLenSec;
        c.msg.setTimeStamp(c.timeSec);
        two.push_back(c);
    }
    std::sort(two.begin(), two.end(),
              [](const TimedMidi& a, const TimedMidi& b) { return a.timeSec < b.timeSec; });

    const int loopSamples = (int) (loopLenSec * kSampleRate + 0.5);
    const int total = 2 * loopSamples + (int) (kWrapSec * kSampleRate);

    juce::AudioBuffer<float> full(2, total);
    full.clear();
    renderEventsInto(inst, two, full);

    // Extract the 2nd period [loopSamples, 2*loopSamples).
    juce::AudioBuffer<float> loop(2, loopSamples);
    loop.clear();
    for (int ch = 0; ch < 2; ++ch)
        loop.copyFrom(ch, 0, full, ch, loopSamples, loopSamples);

    // Tail-wrap: the spillover ringing past 2*loopSamples is pure decaying tail
    // (no note starts after 2L), so adding it onto the start injects the loop's
    // own end tail into its beginning — seamless even for reverb-heavy presets.
    const int wrapSamples = juce::jmin((int) (kWrapSec * kSampleRate), loopSamples);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* spill = full.getReadPointer(ch, 2 * loopSamples);
        float* dst = loop.getWritePointer(ch, 0);
        for (int i = 0; i < wrapSamples; ++i)
            dst[i] += spill[i];
    }
    return loop;
}

} // namespace

int runSerumProbe(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---- parse args -------------------------------------------------------
    juce::String presetPath, outDir = R"(C:\Users\yalci\mt-dev\OpenDaw\crux)";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--serum-probe") == 0)
        {
            if (i + 1 < argc) presetPath = juce::String::fromUTF8(argv[i + 1]);
            if (i + 2 < argc) outDir     = juce::String::fromUTF8(argv[i + 2]);
            break;
        }
    }
    juce::File outFolder(outDir);
    outFolder.createDirectory();
    g_logFile = outFolder.getChildFile("probe_console.log");
    g_logFile.replaceWithText("");   // truncate
    log("[probe] preset = " + presetPath);
    log("[probe] outDir = " + outDir);

    // ---- load ONLY Serum (no full scan) -----------------------------------
    juce::AudioPluginFormatManager fm;
    fm.addFormat(new juce::VST3PluginFormat());  // headless JUCE: addDefaultFormats() is deleted

    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> descs;
    for (const char* path : { kSerumBundle, kSerumInner })
    {
        log("[probe] scanning path: " + juce::String(path));
        try {
            vst3.findAllTypesForFile(descs, juce::String(path));
        } catch (const std::exception& e) {
            log("[probe] scan threw std::exception: " + juce::String(e.what()));
        } catch (...) {
            log("[probe] scan threw unknown exception");
        }
        log("[probe]   -> descriptions: " + juce::String(descs.size()));
        if (descs.size() > 0) break;
    }

    juce::PluginDescription* chosen = nullptr;
    for (auto* d : descs)
    {
        log("    - " + d->name + " (instrument=" + (d->isInstrument ? "yes" : "no") + ")");
        if (d->isInstrument && chosen == nullptr) chosen = d;
    }
    if (chosen == nullptr && descs.size() > 0) chosen = descs[0];
    if (chosen == nullptr) { log("[probe] FATAL: no Serum plugin description"); return 2; }

    log("[probe] creating instance of: " + chosen->name);
    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst(
        fm.createPluginInstance(*chosen, kSampleRate, kBlockSize, err));
    if (!inst) { log("[probe] FATAL: createPluginInstance failed: " + err); return 3; }
    log("[probe] instantiated: " + chosen->name + "  params=" + juce::String(inst->getParameters().size()));

    juce::var report(new juce::DynamicObject());
    auto* obj = report.getDynamicObject();
    obj->setProperty("plugin", chosen->name);
    obj->setProperty("num_params", inst->getParameters().size());

    // ---- default-state size + markers -------------------------------------
    {
        juce::MemoryBlock st;
        inst->getStateInformation(st);
        obj->setProperty("default_state_bytes", (int) st.getSize());
        obj->setProperty("default_state_markers", markerSummary(st));
        log("[probe] default getState: " + juce::String((int) st.getSize())
            + " bytes, markers=" + markerSummary(st));
    }

    // ---- baseline render (default patch) ----------------------------------
    auto baseline = renderNote(*inst);
    const float baseRms = rms(baseline);
    writeWav(outFolder.getChildFile("probe_baseline.wav"), baseline);
    obj->setProperty("baseline_rms", baseRms);
    log("[probe] baseline render rms=" + juce::String(baseRms));

    // ---- THE TEST: inject raw .SerumPreset bytes via setStateInformation --
    juce::File presetFile(presetPath);
    if (presetFile.existsAsFile())
    {
        juce::MemoryBlock presetBytes;
        presetFile.loadFileAsData(presetBytes);
        obj->setProperty("preset_file_bytes", (int) presetBytes.getSize());
        log("[probe] preset file = " + juce::String((int) presetBytes.getSize()) + " bytes");

        inst->setStateInformation(presetBytes.getData(), (int) presetBytes.getSize());
        // let Serum's message-thread load complete
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);

        juce::MemoryBlock after;
        inst->getStateInformation(after);
        obj->setProperty("state_after_inject_bytes", (int) after.getSize());
        obj->setProperty("state_after_inject_markers", markerSummary(after));

        auto injected = renderNote(*inst);
        const float injRms = rms(injected);
        writeWav(outFolder.getChildFile("probe_injected_raw.wav"), injected);
        obj->setProperty("injected_raw_rms", injRms);
        obj->setProperty("raw_injection_changed_sound", std::abs(injRms - baseRms) > 1.0e-4);
        log("[probe] AFTER raw inject: getState=" + juce::String((int) after.getSize())
            + " bytes, render rms=" + juce::String(injRms)
            + "  changed=" + (std::abs(injRms - baseRms) > 1.0e-4 ? "YES" : "no"));
    }
    else
    {
        log("[probe] preset file not found, skipping injection test");
        obj->setProperty("preset_found", false);
    }

    auto reportFile = outFolder.getChildFile("serum_probe_report.json");
    reportFile.replaceWithText(juce::JSON::toString(report, true));
    log("[probe] report -> " + reportFile.getFullPathName());
    log("[probe] DONE");
    return 0;
}

// ============================================================================
//  --serum-render : load a .SerumPreset by synthesizing an OLE file-drop onto
//  Serum's editor window (the only autonomous way, since Serum has no load API
//  and its VST3 state is encrypted), then render. Proves the autonomous loop.
// ============================================================================

#ifdef _WIN32
namespace {

void writeCrashMarker(const char* msg)
{
    HANDLE h = CreateFileW(L"C:\\Users\\yalci\\mt-dev\\OpenDaw\\crux\\render_crash.log",
                           GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(h, msg, (DWORD) std::strlen(msg), &w, nullptr);
        CloseHandle(h);
    }
}

int crtReportHook(int reportType, char* message, int* returnValue)
{
    juce::ignoreUnused(reportType);
    if (returnValue) *returnValue = 0;          // continue, do NOT break/abort
    if (message) writeCrashMarker(message);
    return 1;                                    // handled
}

LONG WINAPI serumCrashHandler(EXCEPTION_POINTERS* ep)
{
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t modName[MAX_PATH] = L"?";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &mod);
    if (mod) GetModuleFileNameW(mod, modName, MAX_PATH);

    char buf[1024];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "CRASH code=0x%08lX addr=%p module=%ls\n",
        ep->ExceptionRecord->ExceptionCode, addr, modName);

    HANDLE h = CreateFileW(L"C:\\Users\\yalci\\mt-dev\\OpenDaw\\crux\\render_crash.log",
                           GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD) n, &written, nullptr);
        CloseHandle(h);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

IDropTarget* getDropTarget(HWND h)
{
    return reinterpret_cast<IDropTarget*>(GetPropW(h, L"OleDropTargetInterface"));
}

BOOL CALLBACK childEnumProc(HWND child, LPARAM lp)
{
    auto** found = reinterpret_cast<IDropTarget**>(lp);
    if (auto* dt = getDropTarget(child)) { *found = dt; return FALSE; }
    return TRUE;
}

IDropTarget* findDropTargetRecursive(HWND top)
{
    if (top == nullptr) return nullptr;
    if (auto* dt = getDropTarget(top)) return dt;
    IDropTarget* found = nullptr;
    EnumChildWindows(top, childEnumProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

// Minimal IDataObject exposing exactly ONE format: CF_HDROP (a file list).
// This is what drop targets actually read; the shell's data object got rejected.
class FileDropData : public IDataObject
{
public:
    explicit FileDropData(const juce::String& path) : ref_(1)
    {
        std::wstring w(path.toWideCharPointer());
        size_t bytes = sizeof(DROPFILES) + (w.size() + 2) * sizeof(wchar_t);
        hdrop_ = GlobalAlloc(GHND, bytes);                 // GHND zero-inits (=> double null)
        auto* df = static_cast<DROPFILES*>(GlobalLock(hdrop_));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
        std::memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hdrop_);
    }
    virtual ~FileDropData() { if (hdrop_) GlobalFree(hdrop_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDataObject)
        { *ppv = static_cast<IDataObject*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return (ULONG) InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override
    { LONG c = InterlockedDecrement(&ref_); if (c == 0) delete this; return (ULONG) c; }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* med) override
    {
        if (fe->cfFormat != CF_HDROP || !(fe->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
        size_t sz = GlobalSize(hdrop_);
        HGLOBAL dup = GlobalAlloc(GHND, sz);
        void* s = GlobalLock(hdrop_); void* d = GlobalLock(dup);
        std::memcpy(d, s, sz);
        GlobalUnlock(hdrop_); GlobalUnlock(dup);
        med->tymed = TYMED_HGLOBAL; med->hGlobal = dup; med->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override
    { return (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) ? S_OK : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override
    {
        if (dir != DATADIR_GET) { *out = nullptr; return E_NOTIMPL; }
        FORMATETC fe { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return SHCreateStdEnumFmtEtc(1, &fe, out);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* o) override
    { if (o) o->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    LONG ref_;
    HGLOBAL hdrop_ = nullptr;
};

IDataObject* makeFileDataObject(const juce::String& path)
{
    return new FileDropData(path);
}

// Returns the IDataObject still alive (caller releases AFTER the load completes,
// because Serum may read the dropped file asynchronously on a later message tick).
IDataObject* dropFileOnTarget(IDropTarget* dt, const juce::String& path, POINTL pt, bool& ok)
{
    ok = false;
    IDataObject* pdo = makeFileDataObject(path);
    if (pdo == nullptr) { log("[drop] makeFileDataObject NULL"); return nullptr; }

    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    HRESULT h1 = dt->DragEnter(pdo, MK_LEFTBUTTON, pt, &eff);
    log("[drop] DragEnter hr=" + juce::String((int) h1) + " eff=" + juce::String((int) eff));
    DWORD eff2 = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    HRESULT h2 = dt->DragOver(MK_LEFTBUTTON, pt, &eff2);
    log("[drop] DragOver  hr=" + juce::String((int) h2) + " eff=" + juce::String((int) eff2));
    if (eff2 == 0)   // target rejects -> a real drop wouldn't call Drop()
        log("[drop] target REJECTED (eff=0) at this point");
    DWORD eff3 = DROPEFFECT_COPY;
    HRESULT hr = dt->Drop(pdo, MK_LEFTBUTTON, pt, &eff3);
    log("[drop] Drop      hr=" + juce::String((int) hr) + " eff=" + juce::String((int) eff3));
    ok = SUCCEEDED(hr);
    return pdo;   // intentionally NOT released here
}

// Try CF_HDROP on EVERY descendant window that has a drop target, at that
// window's own centre, and use whichever ACCEPTS (eff != 0). Returns the data
// object alive (release after the load pump).
IDataObject* dropFileBestTarget(HWND top, const juce::String& path, bool& ok)
{
    ok = false;
    IDataObject* pdo = makeFileDataObject(path);
    if (pdo == nullptr) { log("[drop] data object NULL"); return nullptr; }

    std::vector<HWND> wins;
    wins.push_back(top);
    EnumChildWindows(top, [](HWND h, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&wins));

    int targets = 0;
    for (HWND h : wins)
    {
        IDropTarget* dt = getDropTarget(h);
        if (dt == nullptr) continue;
        ++targets;
        RECT r {};
        GetWindowRect(h, &r);
        const int w = r.right - r.left, ht = r.bottom - r.top;
        if (w <= 0 || ht <= 0) continue;
        // Serum's DragEnter hit-tests the drop point: it accepts a preset over the
        // synth/oscillator area but REJECTS over some controls, and which region
        // the window centre lands on varies run-to-run. So try several points and
        // use the first that accepts (eff != 0) instead of the centre alone.
        const POINTL pts[] = {
            { r.left + w / 2, r.top + ht / 2 },        // centre
            { r.left + w / 2, r.top + ht / 4 },        // upper middle
            { r.left + w / 2, r.top + 3 * ht / 4 },    // lower middle
            { r.left + w / 4, r.top + ht / 2 },        // left middle
            { r.left + 3 * w / 4, r.top + ht / 2 },    // right middle
            { r.left + w / 2, r.top + ht / 8 },        // top (browser/header)
        };
        for (const POINTL& pt : pts)
        {
            DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            HRESULT he = dt->DragEnter(pdo, MK_LEFTBUTTON, pt, &eff);
            if (SUCCEEDED(he) && eff != 0)
            {
                DWORD e2 = DROPEFFECT_COPY; dt->DragOver(MK_LEFTBUTTON, pt, &e2);
                DWORD e3 = DROPEFFECT_COPY; HRESULT hd = dt->Drop(pdo, MK_LEFTBUTTON, pt, &e3);
                log("[drop] ACCEPTED target#" + juce::String(targets) + " at ("
                    + juce::String((int) pt.x) + "," + juce::String((int) pt.y)
                    + ") drop eff=" + juce::String((int) e3));
                ok = SUCCEEDED(hd);
                return pdo;
            }
            dt->DragLeave();
        }
        log("[drop] target#" + juce::String(targets) + " rejected all points rect="
            + juce::String(w) + "x" + juce::String(ht));
    }
    log("[drop] NO OLE target accepted (targets=" + juce::String(targets) + "); trying WM_DROPFILES");

    // Fallback: legacy WM_DROPFILES to each window (for DragAcceptFiles-style handlers).
    std::wstring w(path.toWideCharPointer());
    for (HWND h : wins)
    {
        size_t bytes = sizeof(DROPFILES) + (w.size() + 2) * sizeof(wchar_t);
        HGLOBAL hd = GlobalAlloc(GHND, bytes);
        auto* df = static_cast<DROPFILES*>(GlobalLock(hd));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        RECT r {};
        GetWindowRect(h, &r);
        df->pt.x = (r.right - r.left) / 2;
        df->pt.y = (r.bottom - r.top) / 2;
        auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
        std::memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hd);
        PostMessageW(h, WM_DROPFILES, reinterpret_cast<WPARAM>(hd), 0);
    }
    log("[drop] WM_DROPFILES posted to " + juce::String((int) wins.size()) + " windows");
    return pdo;
}

// IDropSource for a synthetic-but-real OLE drag via DoDragDrop.
class DragSource : public IDropSource
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource)
        { *ppv = static_cast<IDropSource*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return (ULONG) InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override
    { LONG c = InterlockedDecrement(&ref_); if (c == 0) delete this; return (ULONG) c; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keyState) override
    {
        log("[drop] QCD call=" + juce::String(calls_) + " esc=" + juce::String((int) esc)
            + " keys=0x" + juce::String::toHexString((int) keyState));
        if (esc) return DRAGDROP_S_CANCEL;
        // Complete the drop once the held button is released (natural drop) or after
        // a few settle iterations, whichever comes first.
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        if (++calls_ >= 4) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
private:
    LONG ref_ = 1;
    int  calls_ = 0;
};

// Real OLE drag: move the cursor over Serum, press the button, DoDragDrop so OLE
// delivers a genuine drag to whatever window is under the cursor (= Serum).
bool dropFileViaDragLoop(POINTL center, const juce::String& path)
{
    log("[drop] begin");
    IDataObject* pdo = makeFileDataObject(path);
    if (pdo == nullptr) { log("[drop] makeFileDataObject FAILED"); return false; }
    auto* src = new DragSource();

    // Force Serum's window to the FOREGROUND + active input window. DoDragDrop only
    // captures/pumps input for the foreground thread's window, so unattended (when
    // OpenDaw is launched in the background and never gets focus) the drag loop
    // never sees input and hangs. SetForegroundWindow alone is denied by Windows'
    // foreground lock, so attach to the current foreground thread's input queue
    // first (the canonical bypass), then promote our window.
    if (HWND under = WindowFromPoint(POINT{ center.x, center.y }))
    {
        HWND root = GetAncestor(under, GA_ROOT);
        DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        DWORD myThread = GetCurrentThreadId();
        AttachThreadInput(myThread, fgThread, TRUE);
        BringWindowToTop(root);
        SetForegroundWindow(root);
        SetActiveWindow(root);
        SetFocus(root);
        AttachThreadInput(myThread, fgThread, FALSE);
        log("[drop] foreground forced, fg=" + juce::String((int) (GetForegroundWindow() == root)));
    }

    SetCursorPos(center.x, center.y);
    INPUT dn {}; dn.type = INPUT_MOUSE; dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &dn, sizeof(INPUT));

    // DoDragDrop runs a modal loop that only calls IDropSource::QueryContinueDrag
    // when it observes a change in mouse/button state. With no ambient cursor
    // movement (unattended batch) the loop blocks forever, so a watchdog thread
    // drives it: jiggle the cursor over Serum (SetCursorPos => WM_MOUSEMOVE), then
    // RELEASE the held button (LBUTTONUP) — a real drop gesture that forces the
    // loop to call QueryContinueDrag with the button up and complete the drop.
    std::atomic<bool> dragDone{ false };
    std::thread nudger([&dragDone, center]
    {
        for (int i = 0; i < 16 && !dragDone.load(); ++i)
        {
            SetCursorPos(center.x + (i % 2 ? 4 : -4), center.y + (i % 3 ? 3 : -3));
            Sleep(20);
        }
        // Drop: release the button over Serum.
        INPUT up {}; up.type = INPUT_MOUSE; up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &up, sizeof(INPUT));
    });

    log("[drop] calling DoDragDrop");
    DWORD effect = 0;
    HRESULT hr = DoDragDrop(pdo, src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    dragDone.store(true);
    nudger.join();

    INPUT up {}; up.type = INPUT_MOUSE; up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &up, sizeof(INPUT));

    log("[drop] DoDragDrop hr=" + juce::String((int) hr) + " effect=" + juce::String((int) effect));
    src->Release();
    pdo->Release();
    return effect != 0;
}

} // namespace
#endif

// Pump the running Qt event loop (so JuceQtBridge keeps pumping JUCE) for `ms`.
static void pumpFor(int ms)
{
    QElapsedTimer t;
    t.start();
    while ((int) t.elapsed() < ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
        QThread::msleep(2);
    }
}

#ifdef _WIN32
// The one-time bake is explicitly interactive: Serum rejects DragOver unless its
// editor is the active foreground window on the input desktop. Windows normally
// prevents a background process from stealing focus, so temporarily join the
// foreground thread's input queue and activate the Serum peer immediately before
// the drop. This is never called by --serum-render-state.
static bool activateBakeWindow(HWND window)
{
    const HWND foreground = GetForegroundWindow();
    const DWORD ours = GetCurrentThreadId();
    const DWORD theirs = foreground != nullptr
        ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = theirs != 0 && theirs != ours
        && AttachThreadInput(ours, theirs, TRUE) != FALSE;

    ShowWindow(window, SW_SHOW);
    BringWindowToTop(window);
    SetActiveWindow(window);
    SetFocus(window);
    SetForegroundWindow(window);

    if (attached) AttachThreadInput(ours, theirs, FALSE);
    return GetForegroundWindow() == window;
}
#endif

// Headless render driven from inside the full app's Qt + JuceQtBridge + Tracktion
// loop (the environment where Serum loads cleanly). One preset -> one WAV.
int runSerumBatchRender(OpenDawApplication& app, int argc, char** argv)
{
    enum class BatchMode { DropRender, BakeState, RenderState };
    BatchMode mode = BatchMode::DropRender;
    juce::String presetPath, outDir = R"(C:\Users\yalci\mt-dev\OpenDaw\crux)";
    juce::String statePath;
    juce::String midiPath;   // if set -> render this whole MIDI clip instead of C1..C5
    double bpm = 122.0;      // musical tempo for the clip (from the pack manifest)
    int loopBars = 0;        // >0 -> seamless bar-locked LOOP; 0 -> one-shot + tail
    int octaveOffset = 0;   // preset's sounding-vs-MIDI octave offset (pre-compensation)
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--serum-render") == 0)
        {
            if (i + 1 < argc) presetPath = juce::String::fromUTF8(argv[i + 1]);
            if (i + 2 < argc) outDir     = juce::String::fromUTF8(argv[i + 2]);
            if (i + 3 < argc) octaveOffset = std::atoi(argv[i + 3]);
            break;
        }
        // --serum-render-midi <preset> <midi> <outdir> <bpm> [loopBars] : print a
        // MIDI clip through the preset. Reuses the exact same load path as
        // --serum-render; only the final render step differs. loopBars>0 renders a
        // seamless bar-locked loop (no tail); 0/absent renders a one-shot + tail.
        if (std::strcmp(argv[i], "--serum-render-midi") == 0)
        {
            if (i + 1 < argc) presetPath = juce::String::fromUTF8(argv[i + 1]);
            if (i + 2 < argc) midiPath   = juce::String::fromUTF8(argv[i + 2]);
            if (i + 3 < argc) outDir     = juce::String::fromUTF8(argv[i + 3]);
            if (i + 4 < argc) bpm        = std::atof(argv[i + 4]);
            if (i + 5 < argc) loopBars   = std::atoi(argv[i + 5]);
            break;
        }
        // One foreground operation per preset: load via Serum's editor drop target,
        // then persist Serum's OWN VST3 state chunk (not the .SerumPreset bytes).
        if (std::strcmp(argv[i], "--serum-bake") == 0)
        {
            mode = BatchMode::BakeState;
            if (i + 1 < argc) presetPath = juce::String::fromUTF8(argv[i + 1]);
            if (i + 2 < argc) statePath  = juce::String::fromUTF8(argv[i + 2]);
            if (statePath.isNotEmpty()) outDir = juce::File(statePath).getParentDirectory().getFullPathName();
            break;
        }
        // Fully unattended path: restore a previously baked VST3 state chunk and
        // render the live Serum instance. No editor window or OLE drop is needed.
        if (std::strcmp(argv[i], "--serum-render-state") == 0)
        {
            mode = BatchMode::RenderState;
            if (i + 1 < argc) statePath = juce::String::fromUTF8(argv[i + 1]);
            if (i + 2 < argc) midiPath  = juce::String::fromUTF8(argv[i + 2]);
            if (i + 3 < argc) outDir    = juce::String::fromUTF8(argv[i + 3]);
            if (i + 4 < argc) bpm       = std::atof(argv[i + 4]);
            if (i + 5 < argc) loopBars  = std::atoi(argv[i + 5]);
            break;
        }
    }

    juce::File outFolder(outDir);
    outFolder.createDirectory();
    g_logFile = outFolder.getChildFile("batch_console.log");
    g_logFile.replaceWithText("");
    log("[batch] preset = " + presetPath + "  octaveOffset = " + juce::String(octaveOffset));
    if (statePath.isNotEmpty())
        log("[batch] statePath = " + statePath
            + (mode == BatchMode::RenderState ? " (restore)" : " (bake)"));
    if (midiPath.isNotEmpty())
        log("[batch] midiPath = " + midiPath + "  bpm = " + juce::String(bpm)
            + "  loopBars = " + juce::String(loopBars));

#ifdef _WIN32
    SetUnhandledExceptionFilter(serumCrashHandler);   // crash -> crux\render_crash.log
    OleInitialize(nullptr);
#endif

    auto& em = app.editManager();
    auto* track = em.getAudioTrack(0);
    if (track == nullptr) { log("[batch] FATAL: no audio track"); return 2; }

    // Load Serum on the track (scan-free, by path).
    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> descs;
    vst3.findAllTypesForFile(descs, juce::String(kSerumBundle));
    juce::PluginDescription* chosen = nullptr;
    for (auto* d : descs) if (d->isInstrument) { chosen = d; break; }
    if (chosen == nullptr) { log("[batch] FATAL: no Serum description"); return 3; }
    em.setTrackInstrument(*track, *chosen);
    pumpFor(1500);
    log("[batch] Serum instrument set");

    auto* plugin = em.getTrackInstrument(track);
    auto* ext = dynamic_cast<te::ExternalPlugin*>(plugin);
    juce::AudioPluginInstance* inst = ext ? ext->getAudioPluginInstance() : nullptr;
    if (inst == nullptr) { log("[batch] FATAL: no Serum instance"); return 4; }
    log("[batch] Serum instance ready, params=" + juce::String(inst->getParameters().size()));

    juce::MemoryBlock st0;
    inst->getStateInformation(st0);
    log("[batch] state BEFORE load = " + juce::String((int) st0.getSize()));

    bool loaded = false;
    if (mode == BatchMode::RenderState)
    {
        juce::MemoryBlock saved;
        const juce::File stateFile(statePath);
        if (!stateFile.existsAsFile() || !stateFile.loadFileAsData(saved) || saved.isEmpty())
        {
            log("[batch] FATAL: cannot read baked state " + statePath);
        }
        else
        {
            log("[batch] restoring baked state bytes=" + juce::String((int) saved.getSize()));
            inst->setStateInformation(saved.getData(), (int) saved.getSize());
            // Some plugins finish state application on the message thread. Serum's
            // editor object is constructed (but never attached to a desktop) so any
            // editor-owned state bindings exist, then the real Qt/JUCE loop is pumped.
            if (inst->hasEditor()) inst->createEditorIfNeeded();
            int settleMs = 1800;
            const auto settleEnv = juce::SystemStats::getEnvironmentVariable(
                "OPENDAW_SERUM_STATE_SETTLE_MS", {});
            if (settleEnv.isNotEmpty())
                settleMs = juce::jlimit(100, 30000, settleEnv.getIntValue());
            log("[batch] state settle ms=" + juce::String(settleMs));
            pumpFor(settleMs);
            juce::MemoryBlock restored;
            inst->getStateInformation(restored);
            const bool differsFromDefault = restored.getSize() != st0.getSize()
                || (restored.getSize() > 0
                    && std::memcmp(restored.getData(), st0.getData(), restored.getSize()) != 0);
            loaded = true;
            log("[batch] state AFTER restore = " + juce::String((int) restored.getSize())
                + " differsFromDefault=" + juce::String(differsFromDefault ? 1 : 0));
        }
    }
#ifdef _WIN32
    if (mode != BatchMode::RenderState)
    {
        log("[batch] hasEditor=" + juce::String(inst->hasEditor() ? 1 : 0) + " -> creating editor");
        juce::AudioProcessorEditor* ed = inst->hasEditor() ? inst->createEditorIfNeeded() : nullptr;
        log("[batch] createEditorIfNeeded returned ed=" + juce::String(ed != nullptr ? 1 : 0));
        if (ed != nullptr)
        {
            ed->setOpaque(true);
            ed->addToDesktop(juce::ComponentPeer::windowIgnoresKeyPresses);
            ed->setTopLeftPosition(0, 0);   // on-screen window; Serum's GUI must be
            ed->setVisible(true);            // realized for its drop target to work
            log("[batch] editor on desktop, pumping");
            pumpFor(2500);   // let Serum's UI fully realize so its drop target is ready

            HWND top = reinterpret_cast<HWND>(ed->getWindowHandle());
            RECT wr {};
            GetWindowRect(top, &wr);
            POINTL center { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
            log("[batch] window rect L=" + juce::String((int) wr.left) + " T=" + juce::String((int) wr.top)
                + " R=" + juce::String((int) wr.right) + " B=" + juce::String((int) wr.bottom)
                + " center=(" + juce::String((int) center.x) + "," + juce::String((int) center.y) + ")");

        // Load the preset by calling Serum's registered OLE drop target DIRECTLY
        // (DragEnter/DragOver/Drop on the main/STA thread). This needs no cursor,
        // foreground, or input injection, so it works fully unattended — unlike
        // DoDragDrop, whose modal loop hangs when launched in the background with
        // no ambient mouse movement. Verify the state grew (preset loaded); retry.
            const auto baseSize = (int) st0.getSize();
            IDataObject* keepAlive = nullptr;
            const int directAttempts = mode == BatchMode::BakeState ? 1 : 5;
            for (int attempt = 0; attempt < directAttempts && !loaded; ++attempt)
            {
                if (attempt > 0) pumpFor(700 * attempt);   // give Serum more UI-ready time each retry
                const bool foreground = activateBakeWindow(top);
                pumpFor(120);
                log("[batch] bake window foreground=" + juce::String(foreground ? 1 : 0));
                bool dropOk = false;
                IDataObject* pdo = dropFileBestTarget(top, presetPath, dropOk);
                pumpFor(1300);                 // Serum may read the file on a later tick
                juce::MemoryBlock s;
                inst->getStateInformation(s);
                loaded = ((int) s.getSize() > baseSize + 500);
                log("[batch] direct-drop attempt " + juce::String(attempt) + " ok=" + juce::String(dropOk ? 1 : 0)
                    + " state=" + juce::String((int) s.getSize()) + (loaded ? "  <-- LOADED" : ""));
                if (keepAlive) keepAlive->Release();
                keepAlive = pdo;               // hold the latest data object alive across the pump
            }
            // Serum 2 may reject direct IDropTarget calls even while its peer is
            // foreground because it also expects OLE's real modal drag state. The
            // bake phase is the only interactive phase, so use the bounded real OLE
            // loop as its final fallback. The watchdog in dropFileViaDragLoop always
            // releases the mouse and terminates the loop; render-state never enters it.
            if (!loaded && mode != BatchMode::RenderState)
            {
                activateBakeWindow(top);
                const bool dragOk = dropFileViaDragLoop(center, presetPath);
                pumpFor(1800);
                juce::MemoryBlock s;
                inst->getStateInformation(s);
                loaded = ((int) s.getSize() > baseSize + 500);
                log("[batch] real-drag fallback ok=" + juce::String(dragOk ? 1 : 0)
                    + " state=" + juce::String((int) s.getSize())
                    + (loaded ? "  <-- LOADED" : ""));
            }
            // Keep the data object alive until Serum has consumed any deferred load.
            if (keepAlive) { pumpFor(300); keepAlive->Release(); }
        }
        else { log("[batch] Serum reports no editor"); }
    }
#endif
    if (!loaded) log("[batch] WARNING: preset NOT loaded after retries");

    bool stateWritten = false;
    if (loaded && mode == BatchMode::BakeState)
    {
        juce::MemoryBlock baked;
        inst->getStateInformation(baked);
        juce::File stateFile(statePath);
        stateFile.getParentDirectory().createDirectory();
        stateWritten = !baked.isEmpty()
            && stateFile.replaceWithData(baked.getData(), baked.getSize());
        log("[batch] baked state bytes=" + juce::String((int) baked.getSize())
            + " written=" + juce::String(stateWritten ? 1 : 0));
    }

    // Render the LIVE instance directly: it holds the just-loaded preset. The
    // offline te::Renderer would re-instantiate Serum and CANNOT restore its
    // (editor-dependent) encrypted state, so it renders the default sound.
    // Headless opened only the default output; its audio thread would also call
    // processBlock, so suspend the engine (which CLOSES that device) before we render
    // manually. We deliberately do NOT resumeEngine() afterward — that re-opens the
    // user's saved interface (restoreSavedAudioSettings) — and we hard-exit anyway.
    em.suspendEngine();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(150);

    int written = stateWritten ? 1 : 0, target = mode == BatchMode::BakeState ? 1 : 0;
    if (mode != BatchMode::BakeState && midiPath.isNotEmpty())
    {
        // MIDI print: play the clip through the live preset AS WRITTEN (no octave
        // pre-comp — this is what a producer hears dropping the preset on the clip
        // in a DAW). loopBars>0 -> seamless bar-locked loop (no tail); else one-shot
        // with a release tail so FX ring out.
        double clipEndSec = 0.0;
        log("[batch] entering midi branch -> loadMidiClip");
        auto events = loadMidiClip(juce::File(midiPath), bpm, clipEndSec);
        target = 1;
        if (!events.empty())
        {
            juce::AudioBuffer<float> audio;
            if (loopBars > 0)
            {
                const double loopLenSec = loopBars * 4.0 * 60.0 / bpm;   // 4/4 bars
                log("[batch] LOOP bars=" + juce::String(loopBars)
                    + " loopLenSec=" + juce::String(loopLenSec)
                    + " events=" + juce::String((int) events.size())
                    + " bpm=" + juce::String(bpm));
                audio = renderMidiLoop(*inst, events, loopLenSec);
            }
            else
            {
                constexpr double kTailSec = 3.0;
                log("[batch] ONESHOT endSec=" + juce::String(clipEndSec)
                    + " tail=" + juce::String(kTailSec)
                    + " events=" + juce::String((int) events.size()));
                audio = renderMidiClip(*inst, events, clipEndSec, kTailSec);
            }
            juce::File wav = outFolder.getChildFile("clip.wav");
            writeWav(wav, audio);
            if (wav.existsAsFile()) ++written;
            log("[batch] midi render rms=" + juce::String(rms(audio))
                + " frames=" + juce::String(audio.getNumSamples())
                + " durSec=" + juce::String(audio.getNumSamples() / kSampleRate));
        }
    }
    else if (mode != BatchMode::BakeState)
    {
        // Multisample: load the preset once, render so each sample SOUNDS at the
        // target octave C1..C5. The preset transposes by octaveOffset (sounding =
        // midi + offset), so play midi = targetBase - 12*offset. Skip if the
        // pre-compensated MIDI falls outside 0..127 (octave unreachable by keytrack).
        struct Note { const char* name; int base; };
        const Note notes[] = { {"C1", 24}, {"C2", 36}, {"C3", 48}, {"C4", 60}, {"C5", 72} };
        for (const auto& nt : notes)
        {
            const int midi = nt.base - 12 * octaveOffset;
            if (midi < 0 || midi > 127)
            {
                log("[batch] skip " + juce::String(nt.name) + " (MIDI " + juce::String(midi)
                    + " out of range, octave unreachable)");
                continue;
            }
            ++target;
            auto audio = renderNote(*inst, midi);
            juce::File wav = outFolder.getChildFile(juce::String(nt.name) + ".wav");
            writeWav(wav, audio);
            if (wav.existsAsFile()) ++written;
            log("[batch] render " + juce::String(nt.name) + " (midi=" + juce::String(midi)
                + ") rms=" + juce::String(rms(audio)));
        }
    }
    // No resumeEngine(): headless never opened a device, and resuming would re-open
    // the user's audio interface. We hard-exit right below anyway.
    const bool ok = (written == target) && target > 0 && loaded;
    log("[batch] DONE written=" + juce::String(written) + "/" + juce::String(target)
        + " loaded=" + juce::String(loaded ? 1 : 0));

#ifdef _WIN32
    std::cout.flush();
    std::_Exit(ok ? 0 : 5);   // hard-exit: WAV is flushed; avoids editor/teardown hang
#endif
    return ok ? 0 : 5;
}

int runSerumRender(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::String presetPath, outDir = R"(C:\Users\yalci\mt-dev\OpenDaw\crux)";
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--serum-render") == 0)
        {
            if (i + 1 < argc) presetPath = juce::String::fromUTF8(argv[i + 1]);
            if (i + 2 < argc) outDir     = juce::String::fromUTF8(argv[i + 2]);
            break;
        }

    juce::File outFolder(outDir);
    outFolder.createDirectory();
    g_logFile = outFolder.getChildFile("render_console.log");
    g_logFile.replaceWithText("");
    log("[render] preset = " + presetPath);

#ifdef _WIN32
    SetUnhandledExceptionFilter(serumCrashHandler);
    // Debug build: route CRT/JUCE asserts to a log + CONTINUE instead of breaking/aborting.
    _CrtSetReportHook(crtReportHook);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::set_terminate([] { writeCrashMarker("std::terminate\n"); std::_Exit(7); });
    std::signal(SIGABRT, [](int) { writeCrashMarker("SIGABRT\n"); std::_Exit(7); });
    OleInitialize(nullptr);
#endif

    // --- Host Serum via Tracktion (full VST3 host => Serum loads without aborting) ---
    te::Engine engine { "OpenDawSerumRender" };
    log("[render] engine created");
    engine.getDeviceManager().initialise(0, 2);   // output device => context can allocate
    juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
    log("[render] device initialised");
    auto edit = te::createEmptyEdit(engine, juce::File());
    edit->ensureNumberOfAudioTracks(1);
    auto tracks = te::getAudioTracks(*edit);
    if (tracks.isEmpty()) { log("[render] FATAL: no audio track"); return 2; }
    auto* track = tracks.getFirst();
    log("[render] edit + track ready");

    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> descs;
    vst3.findAllTypesForFile(descs, juce::String(kSerumBundle));
    juce::PluginDescription* chosen = nullptr;
    for (auto* d : descs) if (d->isInstrument) { chosen = d; break; }
    if (chosen == nullptr) { log("[render] FATAL: no Serum description"); return 3; }

    auto pluginState = te::ExternalPlugin::create(engine, *chosen);
    auto plugin = edit->getPluginCache().createNewPlugin(pluginState);
    if (plugin == nullptr) { log("[render] FATAL: createNewPlugin failed"); return 4; }
    track->pluginList.insertPlugin(plugin, 0, nullptr);
    log("[render] plugin inserted");
    edit->getTransport().ensureContextAllocated();   // instantiates the plugin instances
    log("[render] context allocated");
    edit->initialiseAllPlugins();
    log("[render] plugins initialised");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1500);
    log("[render] post-init pump done");

    auto* ext = dynamic_cast<te::ExternalPlugin*>(plugin.get());
    juce::AudioPluginInstance* inst = ext ? ext->getAudioPluginInstance() : nullptr;
    if (inst == nullptr) { log("[render] FATAL: Serum not instantiated (no AudioPluginInstance)"); return 5; }
    log("[render] Serum hosted via Tracktion, params=" + juce::String(inst->getParameters().size()));

    juce::MemoryBlock st0;
    inst->getStateInformation(st0);
    log("[render] state BEFORE drop = " + juce::String((int) st0.getSize()) + " bytes");

#ifdef _WIN32
    juce::AudioProcessorEditor* ed = inst->hasEditor() ? inst->createEditorIfNeeded() : nullptr;
    if (ed != nullptr)
    {
        ed->setOpaque(true);
        ed->addToDesktop(juce::ComponentPeer::windowIgnoresKeyPresses);
        ed->setTopLeftPosition(0, 0);
        ed->setVisible(true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1500);

        HWND top = reinterpret_cast<HWND>(ed->getWindowHandle());
        IDropTarget* dt = findDropTargetRecursive(top);
        RECT wr {};
        GetWindowRect(top, &wr);
        POINTL pt { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
        log(juce::String("[render] dropTarget=") + (dt ? "FOUND" : "NONE"));

        if (dt != nullptr)
        {
            bool ok = false;
            IDataObject* pdo = dropFileOnTarget(dt, presetPath, pt, ok);
            log("[render] OLE drop hr_ok=" + juce::String(ok ? 1 : 0));
            for (int k = 0; k < 10; ++k)
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                log("[render]   pump " + juce::String(k));
            }
            if (pdo != nullptr) pdo->Release();
        }
    }
    else { log("[render] Serum reports no editor"); }
#endif

    juce::MemoryBlock st1;
    inst->getStateInformation(st1);
    log("[render] state AFTER drop  = " + juce::String((int) st1.getSize()) + " bytes"
        + (st1.getSize() != st0.getSize() ? "  <-- CHANGED / LOADED" : "  (unchanged)"));

    // MIDI (120 BPM default => 1s = 2 beats): note on@0, off@1s; clip spans 3s.
    auto clipRef = track->insertMIDIClip("note",
        tracktion::TimeRange(tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(kDuration)),
        nullptr);
    if (auto* clip = clipRef.get())
        clip->getSequence().addNote(kMidiNote, tracktion::BeatPosition::fromBeats(0.0),
            tracktion::BeatDuration::fromBeats(2.0), kVelocity, 0, nullptr);
    log("[render] midi note added");

    // Render the edit to WAV via Tracktion's offline renderer.
    juce::File wav = outFolder.getChildFile("rendered_te.wav");
    te::Renderer::Parameters params(*edit);
    params.destFile = wav;
    params.sampleRateForAudio = kSampleRate;
    params.bitDepth = 24;
    params.time = { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(kDuration) };
    params.usePlugins = true;
    params.useMasterPlugins = true;
    params.canRenderInMono = false;
    juce::BigInteger tracksToDo;
    for (auto* t : te::getAudioTracks(*edit))
    {
        int idx = t->getIndexInEditTrackList();
        if (idx >= 0) tracksToDo.setBit(idx);
    }
    params.tracksToDo = tracksToDo;
    juce::WavAudioFormat wavFormat;
    params.audioFormat = &wavFormat;

    std::atomic<float> prog { 0.0f };
    auto task = std::make_unique<te::Renderer::RenderTask>("serum-render", params, &prog, nullptr);
    while (task->runJob() != juce::ThreadPoolJob::jobHasFinished) {}
    if (!task->errorMessage.isEmpty())
        log("[render] RENDER ERROR: " + task->errorMessage);
    log("[render] rendered -> rendered_te.wav exists=" + juce::String(wav.existsAsFile() ? 1 : 0)
        + " size=" + juce::String((int) wav.getSize()));
    log("[render] DONE");

#ifdef _WIN32
    OleUninitialize();
#endif
    return 0;
}

} // namespace OpenDaw
