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

#include <atomic>
#include <cstring>
#include <iostream>
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
juce::AudioBuffer<float> renderNote(juce::AudioPluginInstance& inst)
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
            midi.addEvent(juce::MidiMessage::noteOn(1, kMidiNote, (juce::uint8) kVelocity), 0);
        if (offAt >= pos && offAt < pos + n)
            midi.addEvent(juce::MidiMessage::noteOff(1, kMidiNote), offAt - pos);

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
        if (r.right <= r.left || r.bottom <= r.top) continue;
        POINTL pt { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        HRESULT he = dt->DragEnter(pdo, MK_LEFTBUTTON, pt, &eff);
        log("[drop] target#" + juce::String(targets) + " enter eff=" + juce::String((int) eff)
            + " rect=" + juce::String((int)(r.right - r.left)) + "x" + juce::String((int)(r.bottom - r.top)));
        if (SUCCEEDED(he) && eff != 0)
        {
            DWORD e2 = DROPEFFECT_COPY; dt->DragOver(MK_LEFTBUTTON, pt, &e2);
            DWORD e3 = DROPEFFECT_COPY; HRESULT hd = dt->Drop(pdo, MK_LEFTBUTTON, pt, &e3);
            log("[drop] ACCEPTED target#" + juce::String(targets) + " drop eff=" + juce::String((int) e3));
            ok = SUCCEEDED(hd);
            return pdo;
        }
        dt->DragLeave();
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
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD) override
    {
        if (esc) return DRAGDROP_S_CANCEL;
        if (++calls_ >= 2) return DRAGDROP_S_DROP;   // force the drop after settling
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
    IDataObject* pdo = makeFileDataObject(path);
    if (pdo == nullptr) return false;
    auto* src = new DragSource();

    SetCursorPos(center.x, center.y);
    INPUT dn {}; dn.type = INPUT_MOUSE; dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &dn, sizeof(INPUT));

    DWORD effect = 0;
    HRESULT hr = DoDragDrop(pdo, src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);

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

// Headless render driven from inside the full app's Qt + JuceQtBridge + Tracktion
// loop (the environment where Serum loads cleanly). One preset -> one WAV.
int runSerumBatchRender(OpenDawApplication& app, int argc, char** argv)
{
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
    g_logFile = outFolder.getChildFile("batch_console.log");
    g_logFile.replaceWithText("");
    log("[batch] preset = " + presetPath);

#ifdef _WIN32
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
    log("[batch] state BEFORE drop = " + juce::String((int) st0.getSize()));

    bool loaded = false;
#ifdef _WIN32
    juce::AudioProcessorEditor* ed = inst->hasEditor() ? inst->createEditorIfNeeded() : nullptr;
    if (ed != nullptr)
    {
        ed->setOpaque(true);
        ed->addToDesktop(juce::ComponentPeer::windowIgnoresKeyPresses);
        ed->setTopLeftPosition(0, 0);   // real on-screen window (needs an interactive desktop session)
        ed->setVisible(true);
        pumpFor(1200);

        HWND top = reinterpret_cast<HWND>(ed->getWindowHandle());
        RECT wr {};
        GetWindowRect(top, &wr);
        POINTL center { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };

        // The real-OLE drag is timing sensitive, so verify the state grew (preset
        // loaded) and retry until it does.
        const auto baseSize = (int) st0.getSize();
        for (int attempt = 0; attempt < 5 && !loaded; ++attempt)
        {
            dropFileViaDragLoop(center, presetPath);
            pumpFor(900);
            juce::MemoryBlock s;
            inst->getStateInformation(s);
            loaded = ((int) s.getSize() > baseSize + 500);
            log("[batch] drop attempt " + juce::String(attempt)
                + " state=" + juce::String((int) s.getSize()) + (loaded ? "  <-- LOADED" : ""));
        }
    }
    else { log("[batch] Serum reports no editor"); }
#endif
    if (!loaded) log("[batch] WARNING: preset NOT loaded after retries");

    // Render the LIVE instance directly: it holds the just-loaded preset. The
    // offline te::Renderer would re-instantiate Serum and CANNOT restore its
    // (editor-dependent) encrypted state, so it renders the default sound.
    // Suspend Tracktion's audio thread first so it isn't also calling processBlock.
    em.suspendEngine();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
    auto audio = renderNote(*inst);
    juce::File wav = outFolder.getChildFile("rendered_batch.wav");
    writeWav(wav, audio);
    em.resumeEngine();
    const bool ok = wav.existsAsFile();
    log("[batch] DIRECT render rms=" + juce::String(rms(audio))
        + " exists=" + juce::String(ok ? 1 : 0));
    log("[batch] DONE");

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
