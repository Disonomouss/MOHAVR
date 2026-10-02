#include "bridge.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>

#include <atomic>
#include <cmath>
#include <string>

#include "../common/shared_frame.hpp"
#include "log.hpp"
#include "vr_view.hpp"

namespace mohavr::bridge {
namespace {

using shared::Header;
using shared::kRing;

Header* g_hdr     = nullptr;
HANDLE  g_mapping = nullptr;
HANDLE  g_host    = nullptr;  // host process handle

// Render-thread state.
bool                        g_setupTried = false;
bool                        g_ready      = false;
IDirect3DDevice9On12*       g_d9on12     = nullptr;
ID3D12Device*               g_d12        = nullptr;
ID3D12CommandQueue*         g_queue      = nullptr;
ID3D12CommandAllocator*     g_alloc[kRing] = {};
ID3D12GraphicsCommandList*  g_list       = nullptr;
ID3D12Fence*                g_gameFence  = nullptr;
ID3D12Fence*                g_hostFence  = nullptr;
ID3D12Resource*             g_shared[kRing] = {};
IDirect3DSurface9*          g_rt         = nullptr;  // our copy of the backbuffer (9On12-backed)
long                        g_presents   = 0;
bool                        g_hostExitLogged = false;
bool                        g_paused     = false;  // backbuffer size changed by a Reset
// Reset runs on the main thread, Present on the render thread (ENGINE-NOTES 5e): g_rt is shared.
CRITICAL_SECTION            g_rtLock;
bool                        g_rtLockInit = false;

// Frame pacing (hdr->pace): the host's frame event (Local\MOHAVR_Frame_<pid>, created here, set by the host), and the
// Presents seen.
HANDLE                      g_frameEvent   = nullptr;
HANDLE                      g_presentEvent = nullptr;  // auto-reset, set after each OnPresent
std::atomic<std::uint32_t>  g_presentsSeen{0};
bool                        g_pacingHooked = false;    // the Draw hook that paces is in (SetPacingAvailable)

double QpcMs(LONGLONG q) {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(q) / static_cast<double>(f.QuadPart);
}

// Diagnostics (round 25, "slight jitter in general movement"): per published frame -- which is what the headset shows,
// one after the other -- the game time between their views (the world's step per shown frame) and how many game frames
// were committed for it; logged every 10 s.
void NotePublished(const view::PresentedFrameInfo& info) {
    static LONGLONG lastQpc = 0;
    static std::uint32_t lastSerial = 0;
    static double sum = 0.0, sum2 = 0.0, lo = 1e9, hi = 0.0, since = 0.0;
    static long n = 0, gameFrames = 0;
    if (!info.qpc) return;
    if (lastQpc && info.serial != lastSerial) {
        const double step = QpcMs(info.qpc - lastQpc);
        if (step > 0.0 && step < 250.0) {
            sum += step;
            sum2 += step * step;
            lo = step < lo ? step : lo;
            hi = step > hi ? step : hi;
            ++n;
            gameFrames += static_cast<long>(info.serial - lastSerial);
            since += step;
        }
    }
    lastQpc = info.qpc;
    lastSerial = info.serial;
    if (since >= 10000.0 && n > 0) {
        const double mean = sum / n, sd = std::sqrt(std::fmax(0.0, sum2 / n - mean * mean));
        MLOG("bridge: %ld frames shown in %.1f s -- the world stepped %.2f ms a frame (%.2f..%.2f, sd %.2f), %.2f game frames "
             "each (paced %d)", n, since / 1000.0, mean, lo, hi, sd, static_cast<double>(gameFrames) / n, info.paced ? 1 : 0);
        sum = sum2 = since = 0.0;
        lo = 1e9;
        hi = 0.0;
        n = gameFrames = 0;
    }
}

struct RtGuard {
    RtGuard() { EnterCriticalSection(&g_rtLock); }
    ~RtGuard() { LeaveCriticalSection(&g_rtLock); }
};

void SetStatus(const char* msg) {
    if (g_hdr) strncpy_s(g_hdr->gameStatus, msg, _TRUNCATE);
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool Fail(const char* what, HRESULT hr) {
    char msg[128];
    snprintf(msg, sizeof(msg), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    MLOG("bridge: %s -- bridge disabled, game continues", msg);
    SetStatus(msg);
    if (g_hdr) InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_hdr->gameState), static_cast<LONG>(shared::GameState::Failed));
    return false;
}

bool Setup(IDirect3DDevice9* dev) {
    HRESULT hr = dev->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&g_d9on12));
    if (FAILED(hr)) return Fail("QueryInterface(IDirect3DDevice9On12) -- is Bridge.D3D9On12 on?", hr);
    hr = g_d9on12->GetD3D12Device(IID_PPV_ARGS(&g_d12));
    if (FAILED(hr)) return Fail("GetD3D12Device", hr);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = g_d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) return Fail("CreateCommandQueue", hr);
    for (auto& a : g_alloc)
        if (FAILED(hr = g_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return Fail("CreateCommandAllocator", hr);
    if (FAILED(hr = g_d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list))))
        return Fail("CreateCommandList", hr);
    g_list->Close();
    if (FAILED(hr = g_d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_gameFence)))) return Fail("CreateFence(game)", hr);
    if (FAILED(hr = g_d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_hostFence)))) return Fail("CreateFence(host)", hr);

    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC bd{};
    if (FAILED(hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return Fail("GetBackBuffer", hr);
    bb->GetDesc(&bd);
    bb->Release();
    if (FAILED(hr = dev->CreateRenderTarget(bd.Width, bd.Height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, nullptr)))
        return Fail("CreateRenderTarget", hr);

    // Shared textures: B8G8R8A8 = D3D9's A8R8G8B8. Simultaneous access: used by two queues in two
    // processes, and lets COMMON promote to COPY_DEST/COPY_SOURCE without explicit barriers.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = bd.Width;
    rd.Height = bd.Height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    for (UINT i = 0; i < kRing; ++i) {
        if (FAILED(hr = g_d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&g_shared[i]))))
            return Fail("CreateCommittedResource(shared)", hr);
        HANDLE h = nullptr;
        if (FAILED(hr = g_d12->CreateSharedHandle(g_shared[i], nullptr, GENERIC_ALL, nullptr, &h))) return Fail("CreateSharedHandle(texture)", hr);
        g_hdr->textureHandles[i] = reinterpret_cast<std::uintptr_t>(h);
    }
    HANDLE hg = nullptr, hh = nullptr;
    if (FAILED(hr = g_d12->CreateSharedHandle(g_gameFence, nullptr, GENERIC_ALL, nullptr, &hg))) return Fail("CreateSharedHandle(game fence)", hr);
    if (FAILED(hr = g_d12->CreateSharedHandle(g_hostFence, nullptr, GENERIC_ALL, nullptr, &hh))) return Fail("CreateSharedHandle(host fence)", hr);

    const LUID luid = g_d12->GetAdapterLuid();
    g_hdr->width = bd.Width;
    g_hdr->height = bd.Height;
    g_hdr->dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_hdr->ring = kRing;
    g_hdr->adapterLuid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
    g_hdr->gameFenceHandle = reinterpret_cast<std::uintptr_t>(hg);
    g_hdr->hostFenceHandle = reinterpret_cast<std::uintptr_t>(hh);
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_hdr->gameState), static_cast<LONG>(shared::GameState::Ready));
    SetStatus("ready");
    MLOG("bridge: ready -- %ux%u B8G8R8A8, ring %u, adapter LUID %08lX:%08lX", bd.Width, bd.Height, kRing,
         static_cast<unsigned long>(luid.HighPart), luid.LowPart);
    return true;
}

void Publish(IDirect3DDevice9* dev) {
    const std::uint64_t published = static_cast<std::uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(&g_hdr->publishedFrame), 0, 0));
    const std::uint64_t ack = static_cast<std::uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(&g_hdr->ackFrame), 0, 0));
    // Uncapped: only once the host has taken the last frame (the rest are skipped, never blocking). Paced: every frame is
    // one the headset should show -- one the host hasn't taken yet doesn't hold this one back (it takes the newest; the
    // ring's GPU waits keep the slots safe), up to the ring's depth (shared_frame.hpp).
    const bool paced = g_pacingHooked && g_hdr->pace != 0;
    if (paced ? published - ack >= kRing : ack != published) return;

    const std::uint64_t n = published + 1;
    const UINT slot = static_cast<UINT>(n % kRing);
    // The slot's command allocator is free once our own copy of frame n - kRing has executed.
    if (n > kRing && g_gameFence->GetCompletedValue() < n - kRing) return;

    RtGuard guard;
    if (!g_rt) {  // released for a device Reset; recreate at the (unchanged) size
        if (FAILED(dev->CreateRenderTarget(g_hdr->width, g_hdr->height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, nullptr))) {
            g_rt = nullptr;
            return;
        }
        MLOG("bridge: render target recreated after device reset");
    }

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    HRESULT hr = dev->StretchRect(bb, nullptr, g_rt, nullptr, D3DTEXF_NONE);
    bb->Release();
    if (FAILED(hr)) { if (n <= 3) MLOG("bridge: StretchRect failed 0x%08lX", static_cast<unsigned long>(hr)); return; }

    ID3D12Resource* src = nullptr;
    hr = g_d9on12->UnwrapUnderlyingResource(g_rt, g_queue, IID_PPV_ARGS(&src));
    if (FAILED(hr)) { if (n <= 3) MLOG("bridge: UnwrapUnderlyingResource failed 0x%08lX", static_cast<unsigned long>(hr)); return; }

    // Don't overwrite a slot the host may still be reading (GPU-side wait; the host consumes every
    // published frame, so this always completes).
    if (n > kRing) g_queue->Wait(g_hostFence, n - kRing);
    g_alloc[slot]->Reset();
    g_list->Reset(g_alloc[slot], nullptr);
    g_list->CopyResource(g_shared[slot], src);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_gameFence, n);

    UINT64 value = n;
    ID3D12Fence* fence = g_gameFence;
    g_d9on12->ReturnUnderlyingResource(g_rt, 1, &value, &fence);
    src->Release();

    // The pose/FOV this image was rendered with (M3); the host submits it with exactly these.
    shared::SlotMeta meta{};
    shared::SlotScope scope{};
    view::PresentedFrameInfo info{};
    view::MetaForPresentedFrame(meta, &info, &scope);
    g_hdr->slotMeta[slot] = meta;
    g_hdr->slotScope[slot] = scope;
    g_hdr->slotViewQpc[slot] = info.qpc;
    MemoryBarrier();
    NotePublished(info);

    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_hdr->publishedSlot), static_cast<LONG>(slot));
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_hdr->publishedFrame), static_cast<LONG64>(n));
    if (n == 1) MLOG("bridge: first frame published (slot %u)", slot);
    else if (n % 900 == 0) MLOG("bridge: %llu frames published", static_cast<unsigned long long>(n));
}

}  // namespace

shared::Header* SharedHeader() { return g_hdr; }

void OnBeforeReset() {
    if (!g_rtLockInit) return;
    RtGuard guard;
    if (g_rt) {
        g_rt->Release();
        g_rt = nullptr;
        MLOG("bridge: released render target for device Reset");
    }
}

void OnAfterReset(unsigned width, unsigned height) {
    if (!g_hdr || !g_ready || !width || !height) return;
    if (width != g_hdr->width || height != g_hdr->height) {
        if (!g_paused) MLOG("bridge: Reset changed the backbuffer to %ux%u (was %ux%u) -- bridge paused (restart the game)",
                            width, height, g_hdr->width, g_hdr->height);
        g_paused = true;
    } else if (g_paused) {
        MLOG("bridge: backbuffer back to %ux%u -- bridge resumed", width, height);
        g_paused = false;
    }
}

void StartHost(const std::wstring& runtimeJson, float defaultUnitsPerMeter, int mirror, bool controllers) {
    if (g_hdr) return;
    InitializeCriticalSection(&g_rtLock);
    g_rtLockInit = true;
    const DWORD pid = GetCurrentProcessId();
    const std::wstring name = L"Local\\MOHAVR_" + std::to_wstring(pid);
    g_frameEvent = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\MOHAVR_Frame_" + std::to_wstring(pid)).c_str());
    g_presentEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Header), name.c_str());
    if (!g_mapping) { MLOG("bridge: CreateFileMapping failed (%lu)", GetLastError()); return; }
    g_hdr = static_cast<Header*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Header)));
    if (!g_hdr) { MLOG("bridge: MapViewOfFile failed (%lu)", GetLastError()); return; }
    ZeroMemory(g_hdr, sizeof(Header));
    g_hdr->magic = shared::kMagic;
    g_hdr->version = shared::kVersion;
    g_hdr->gamePid = pid;
    g_hdr->gameState = static_cast<std::uint32_t>(shared::GameState::Starting);
    g_hdr->defaultUnitsPerMeter = defaultUnitsPerMeter;
    g_hdr->unitsPerMeter = 0.0f;  // until the host applies the player's saved value
    g_hdr->heightOffset = 0.0f;
    g_hdr->recenterSeq = 0;
    g_hdr->gameUiMenu = 0;
    g_hdr->handValid = 0;
    g_hdr->aimDistance = 0.0f;
    g_hdr->aimSource = 0;
    g_hdr->weaponKey[0] = 0;
    g_hdr->weaponSeq = 0;
    g_hdr->fitSeq = 0;
    g_hdr->fitValid = 0;
    g_hdr->gunFlags = 0;
    g_hdr->cmdSeq = 0;
    g_hdr->cmd[0] = 0;
    g_hdr->throwSeq = 0;
    g_hdr->weaponKind = 0;
    for (int i = 0; i < 4; ++i) g_hdr->freeHand[i] = 0.0f;
    g_hdr->pace = 0;  // until the host says (its menu's Frame pacing)

    const std::wstring exe = ModuleDir() + L"\\MOHAVR-host.exe";
    std::wstring cmd = L"\"" + exe + L"\" --game-pid " + std::to_wstring(pid);
    if (!runtimeJson.empty()) cmd += L" --runtime-json \"" + runtimeJson + L"\"";
    if (mirror) cmd += L" --mirror " + std::to_wstring(mirror);
    if (controllers) cmd += L" --controllers 1";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ModuleDir().c_str(), &si, &pi)) {
        MLOG("bridge: could not start MOHAVR-host.exe (error %lu) -- no VR, game continues", GetLastError());
        SetStatus("host not started");
        return;
    }
    CloseHandle(pi.hThread);
    g_host = pi.hProcess;
    MLOG("bridge: started MOHAVR-host.exe pid %lu (shared memory %ls)", pi.dwProcessId, name.c_str());
}

namespace {
void PresentImpl(IDirect3DDevice9* device);
}

void OnPresent(IDirect3DDevice9* device) {
    PresentImpl(device);
    // Counted after the publish: the game thread's paced Draw waits for this before it commits the next frame.
    g_presentsSeen.fetch_add(1, std::memory_order_release);
    if (g_presentEvent) SetEvent(g_presentEvent);
}

std::uint32_t PresentsSeen() { return g_presentsSeen.load(std::memory_order_acquire); }

bool WaitPresents(std::uint32_t target, unsigned timeoutMs) {
    LARGE_INTEGER t0, t;
    QueryPerformanceCounter(&t0);
    while (static_cast<std::int32_t>(PresentsSeen() - target) < 0) {
        QueryPerformanceCounter(&t);
        const double spent = QpcMs(t.QuadPart - t0.QuadPart);
        if (!g_presentEvent || spent >= timeoutMs) return false;
        WaitForSingleObject(g_presentEvent, static_cast<DWORD>(timeoutMs - spent) + 1);
    }
    return true;
}

void SetPacingAvailable(bool on) { g_pacingHooked = on; }

bool HostRunning() {
    return g_frameEvent && g_hdr && !g_hostExitLogged && g_hdr->hostState == static_cast<std::uint32_t>(shared::HostState::Running);
}

int WaitHostFrame(unsigned timeoutMs) {
    if (!HostRunning()) return -1;
    return WaitForSingleObject(g_frameEvent, timeoutMs) == WAIT_OBJECT_0 ? 1 : 0;
}

namespace {
void PresentImpl(IDirect3DDevice9* device) {
    if (!g_hdr) return;
    ++g_presents;
    if (!g_setupTried) {
        g_setupTried = true;
        g_ready = Setup(device);
    }
    // The host gone (exited or killed) must not leave the game on a frozen head pose or a held stick:
    // mark it exited and withdraw the views and the pad, so everything falls back to the plain game
    // (mono, the game's own camera, the real XInput pad). Checked every 16 presents (a 0 ms wait).
    if (g_host && !g_hostExitLogged && (g_presents % 16) == 0 && WaitForSingleObject(g_host, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(g_host, &code);
        g_hdr->hostState = static_cast<std::uint32_t>(shared::HostState::Exited);
        g_hdr->padActive = 0;
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->viewSeq));  // odd: nobody reads now
        g_hdr->viewValid = 0;
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->viewSeq));
        MLOG("bridge: MOHAVR-host.exe exited (code %lu, status \"%s\") -- no VR: views and pad withdrawn, the game "
             "continues as a normal flat game", code, g_hdr->hostStatus);
        g_hostExitLogged = true;
    }
    if (g_hostExitLogged) return;
    Publish(device);
}
}  // namespace

}  // namespace mohavr::bridge
