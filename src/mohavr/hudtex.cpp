#include "hudtex.hpp"

#include <windows.h>
#include <d3d9.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cstring>

#include "addresses.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "script_call.hpp"

namespace mohavr::hudtex {
namespace {

int  g_w = 1280, g_h = 720;
bool g_installed = false;
int  g_placeDefault = 0;               // [HUD] Place (0 screen, 1 wrist) until the host says
std::atomic<bool> g_shared{false};     // the bridge's HUD texture ring exists
std::atomic<bool> g_alphaOk{false};    // the SetRenderState filter is in place (else the texture's alpha would stay 0)
bool g_failed = false;                 // a pass's canvas matrix wasn't ours: the screen panel from now on

SafetyHookMid    g_flushHook;    // FCanvas::Flush entry (game thread)
SafetyHookMid    g_closeHook;    // after the HUD loop's closing flush (game thread)
SafetyHookInline g_execHook;     // FlushCommand::Execute (render thread; or inside Flush without threaded rendering)

// --- Game thread ----------------------------------------------------------------------------------------------------
bool           g_thisDraw = false;  // this Draw's HUD goes to the texture
bool           g_pass = false;      // between the matrix push and the closing flush of that pass
std::uint32_t  g_serial = 0;        // HUD passes so far
std::uintptr_t g_hud = 0;           // the pass's MOHAHUD (its elements are read after the pass)
std::uintptr_t g_ctrl = 0;          // its PlayerController (the pawn's health: dead or alive)

// The batches flushed during a pass, in order: written by the game thread, read by the render thread (SPSC).
struct Entry {
    std::uintptr_t batch;
    std::uint32_t  serial;
};
constexpr std::uint32_t kRing = 256;
Entry                      g_ring[kRing] = {};
std::atomic<std::uint32_t> g_head{0}, g_tail{0};

// Each pass's element rectangles (written by the game thread after the pass; read by the render thread at the Present of
// the frame whose batches it drew -- that Present is queued after the Draw returns, so after the write).
struct PassRects {
    std::atomic<std::uint32_t> serial{0};
    float         rs = 1.0f;
    std::uint32_t shown = 0;
    bool          valid = false;
    bool          dead = false;   // no pawn, or its Health <= 0 (the host hides the wrist panels)
    float         rect[shared::kHudRects][4] = {};
};
constexpr std::uint32_t kPassSlots = 8;
PassRects g_rects[kPassSlots];

// --- Render thread ----------------------------------------------------------------------------------------------------
std::atomic<IDirect3DDevice9*> g_dev{nullptr};
std::atomic<bool>              g_rtReady{false};  // read by the game thread (PlanDraw)
IDirect3DSurface9*             g_rt = nullptr;    // the HUD texture (D3DPOOL_DEFAULT, A8R8G8B8)
CRITICAL_SECTION               g_rtLock;           // g_rt: Reset runs on the main thread
bool                           g_forceAlpha = false;  // inside a redirected batch
int                            g_logAfterReset = 0;   // batches to log after the texture is recreated
long                           g_frameBatches = 0;    // batches redirected since the last EndFrame
std::uint32_t                  g_frameSerial = 0;     // the pass they belonged to (the last one)

// Statistics (logged every 10 s from Present).
std::atomic<long long> g_overheadTicks{0}, g_drawTicks{0};  // QPC ticks: the redirect's own work / the batches' draws
std::atomic<long> g_passes{0}, g_pushed{0}, g_overflow{0}, g_redirected{0}, g_dropped{0}, g_skipped{0}, g_clears{0};

// --- Premultiplied alpha forced while a HUD batch draws (IDirect3DDevice9::SetRenderState, slot 57) --------------------
// The game's canvas blend is SRCALPHA / INVSRCALPHA for colour with a separate alpha blend of ZERO / ONE: the texture's alpha
// would stay 0 (W0, measured). Forced to ONE / INVSRCALPHA, colour and alpha give premultiplied RGBA. A device Reset restores
// the vtable slot (W0: the same vtable, the slot reset), so the hook is checked every Present.
using PFN_SetRenderState = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
PFN_SetRenderState g_realSetRenderState = nullptr;
constexpr int kSlotSetRenderState = 57;

DWORD ForcedValue(D3DRENDERSTATETYPE s, DWORD v) {
    switch (s) {
        case D3DRS_SEPARATEALPHABLENDENABLE: return TRUE;
        case D3DRS_SRCBLENDALPHA: return D3DBLEND_ONE;
        case D3DRS_DESTBLENDALPHA: return D3DBLEND_INVSRCALPHA;
        case D3DRS_BLENDOPALPHA: return D3DBLENDOP_ADD;
        case D3DRS_COLORWRITEENABLE: return v | D3DCOLORWRITEENABLE_ALPHA;
        default: return v;
    }
}

HRESULT STDMETHODCALLTYPE Hook_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v) {
    if (g_forceAlpha) v = ForcedValue(s, v);
    return g_realSetRenderState(dev, s, v);
}

void HookSetRenderState(IDirect3DDevice9* dev) {
    auto** vtbl = *reinterpret_cast<void***>(dev);
    void* cur = vtbl[kSlotSetRenderState];
    if (cur == reinterpret_cast<void*>(&Hook_SetRenderState)) {
        g_alphaOk.store(g_realSetRenderState != nullptr, std::memory_order_release);
        return;
    }
    static void** lastVtbl = nullptr;
    static int rehooks = 0;
    if (lastVtbl && rehooks++ < 20)
        MLOG("hudtex: SetRenderState not hooked any more (vtable %p, was %p; slot %p) -- hooking again", static_cast<void*>(vtbl),
             static_cast<void*>(lastVtbl), cur);
    lastVtbl = vtbl;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(cur), &owner)) {
        static int warned = 0;
        if (warned++ < 3) MLOG("hudtex: SetRenderState slot %p is not inside a module -- alpha not forced (the HUD stays on the screen)", cur);
        g_alphaOk.store(false, std::memory_order_release);
        return;
    }
    g_realSetRenderState = reinterpret_cast<PFN_SetRenderState>(cur);
    if (!patch::SwapPointer(reinterpret_cast<std::uintptr_t>(&vtbl[kSlotSetRenderState]), cur,
                            reinterpret_cast<void*>(&Hook_SetRenderState))) {
        g_realSetRenderState = nullptr;
        g_alphaOk.store(false, std::memory_order_release);
        MLOG("hudtex: SetRenderState vtable swap failed -- alpha not forced (the HUD stays on the screen)");
        return;
    }
    g_alphaOk.store(true, std::memory_order_release);
    static bool logged = false;
    if (!logged) MLOG("hudtex: SetRenderState hooked (premultiplied alpha forced inside HUD batches)");
    logged = true;
}

// --- The HUD elements' rectangles (design 3.3; the W0 measurement: PosX / PosY are the top-left corner) -----------------
int IntProp(std::uintptr_t o, const char* name, int def) {
    const int off = names::PropertyOffset(o, name);
    return off >= 0 ? *reinterpret_cast<const int*>(o + off) : def;
}
std::uintptr_t ObjProp(std::uintptr_t o, const char* name) {
    const int off = o ? names::PropertyOffset(o, name) : -1;
    return off >= 0 ? names::ReadPointer(o + off) : 0;
}
bool Rendered(std::uintptr_t o) {
    int off = 0;
    std::uint32_t mask = 0;
    return o && names::BoolProperty(o, "bRender", off, mask) && (*reinterpret_cast<const std::uint32_t*>(o + off) & mask) != 0;
}
void Box(float (&r)[4], float x0, float y0, float x1, float y1) {
    r[0] = x0;
    r[1] = y0;
    r[2] = x1;
    r[3] = y1;
}
// An element's own box (Pos, Size); false if it isn't there.
bool ElemBox(std::uintptr_t e, float (&r)[4]) {
    if (!e) return false;
    const int px = IntProp(e, "PosX", 0), py = IntProp(e, "PosY", 0), sx = IntProp(e, "SizeX", 0), sy = IntProp(e, "SizeY", 0);
    Box(r, static_cast<float>(px), static_cast<float>(py), static_cast<float>(px + sx), static_cast<float>(py + sy));
    return true;
}

void ReadRects(std::uintptr_t hud, std::uint32_t serial) {
    PassRects& out = g_rects[serial % kPassSlots];
    out.serial.store(0, std::memory_order_relaxed);  // (being written)
    out.valid = false;
    out.shown = 0;
    if (!hud) return;
    const int ro = names::PropertyOffset(hud, "resolutionScale");
    float rs = ro >= 0 ? *reinterpret_cast<const float*>(hud + ro) : 1.0f;
    if (!(rs > 0.2f && rs < 4.0f)) rs = 1.0f;
    out.rs = rs;
    const float W = static_cast<float>(g_w), H = static_cast<float>(g_h);
    auto& R = out.rect;
    auto shown = [&](int i, bool on) { if (on) out.shown |= 1u << i; };
    int got = 0;
    // The left panel: health, the compass (padded by a rim bead's radius), the stance icon.
    const std::uintptr_t health = ObjProp(hud, "hud_health"), compass = ObjProp(hud, "hud_compass"),
                         stance = ObjProp(hud, "hud_stanceIcon");
    if (ElemBox(health, R[shared::kHudHealth])) ++got, shown(shared::kHudHealth, Rendered(health));
    if (ElemBox(compass, R[shared::kHudCompass])) {
        ++got;
        const int bo = names::PropertyOffset(hud, "NPCCompassBeadSize");
        const float bead = bo >= 0 ? *reinterpret_cast<const float*>(hud + bo) : 20.0f;
        const float pad = 0.5f * (bead > 0.0f && bead < 100.0f ? bead : 20.0f) * rs;
        float* c = R[shared::kHudCompass];
        Box(R[shared::kHudCompass], c[0] - pad, c[1] - pad, c[2] + pad, c[3] + pad);
        shown(shared::kHudCompass, Rendered(compass));
    }
    if (ElemBox(stance, R[shared::kHudStance])) ++got, shown(shared::kHudStance, Rendered(stance));
    // The right panel: the ammo bar, its two counts (left-justified at their positions: the glyphs measured +1..+93 px
    // across and +12..+32 down of the ammo count's, +1..+25 and +4..+19 of the grenade count's -- wristhud1), the icons
    // bounded by the largest tiles (weapons 96 x 256, grenades 64 x 120: the design's check), right / bottom at the exp
    // bars' corners and drawn ExperienceIconOffset (27) to the left of them (measured: the exp bar's Pos / Size are the
    // current icon's tile, e.g. the StG44's 96 x 256 at 1039,369; it is drawn 1012..1108), the level badges, the medals.
    const std::uintptr_t bar = ObjProp(hud, "hud_ammoCountBg"), ammo = ObjProp(hud, "hud_ammoCount"),
                         nade = ObjProp(hud, "hud_grenadeAmmoCount"), wexp = ObjProp(hud, "hud_weaponExperience"),
                         gexp = ObjProp(hud, "hud_grenadeExperience");
    if (ElemBox(bar, R[shared::kHudAmmoBar])) ++got, shown(shared::kHudAmmoBar, Rendered(bar));
    auto text = [&](std::uintptr_t e, int i) {
        if (!e) return;
        const float x = static_cast<float>(IntProp(e, "PosX", 0)), y = static_cast<float>(IntProp(e, "PosY", 0));
        Box(R[i], x - 4.0f * rs, y - 2.0f * rs, x + 124.0f * rs, y + 36.0f * rs);
        shown(i, Rendered(e));
        ++got;
    };
    text(ammo, shared::kHudAmmoText);
    text(nade, shared::kHudNadeText);
    auto icon = [&](std::uintptr_t e, int i, float w, float h) {
        if (!e) return;
        const float ax = static_cast<float>(IntProp(e, "PosX", 0) + IntProp(e, "SizeX", 0)),
                    ay = static_cast<float>(IntProp(e, "PosY", 0) + IntProp(e, "SizeY", 0));
        Box(R[i], ax - (w + 27.0f + 4.0f) * rs, ay - (h + 4.0f) * rs, ax + 4.0f * rs, ay + 4.0f * rs);
        shown(i, Rendered(e));
        ++got;
    };
    icon(wexp, shared::kHudWeaponIcon, 96.0f, 256.0f);
    icon(gexp, shared::kHudNadeIcon, 64.0f, 120.0f);
    const std::uintptr_t wbadge = ObjProp(wexp, "ExperienceLevelBg"), gbadge = ObjProp(gexp, "ExperienceLevelBg");
    if (ElemBox(wbadge, R[shared::kHudWeaponBadge])) ++got, shown(shared::kHudWeaponBadge, Rendered(wbadge));
    if (ElemBox(gbadge, R[shared::kHudNadeBadge])) ++got, shown(shared::kHudNadeBadge, Rendered(gbadge));
    // The kill medals: MOHAHUDMessageQueue's GunKillNotifyIconLoc (0.93, 0.88) / GrenadeKillNotifyIconLoc (0.795, 0.88),
    // MedalMomentIconSize 60, centred (the safe zone: 0.05 + 0.9 n).
    auto medal = [&](int i, float nx, float ny) {
        const float x = (0.05f + 0.9f * nx) * W, y = (0.05f + 0.9f * ny) * H, s = 30.0f * rs + 4.0f;
        Box(R[i], x - s, y - s, x + s, y + s);
    };
    medal(shared::kHudWeaponMedal, 0.93f, 0.88f);
    medal(shared::kHudNadeMedal, 0.795f, 0.88f);
    out.valid = got >= 4;
    {
        const std::uintptr_t pawn = ObjProp(g_ctrl, "Pawn");
        out.dead = !pawn || IntProp(pawn, "Health", 1) <= 0;
        static int seenDead = -1, loggedDead = 0;
        if ((out.dead ? 1 : 0) != seenDead && loggedDead < 20) {
            ++loggedDead;
            seenDead = out.dead ? 1 : 0;
            MLOG("hudtex: the player %s", out.dead ? "is dead or has no pawn (the wrist panels hide)" : "is alive");
        }
    }
    out.serial.store(serial, std::memory_order_release);
    // Logged as they change (a few times): the first pass, a new HUD, a new scale, the badges.
    static float lastRs = -1.0f;
    static std::uintptr_t lastHud = 0;
    static std::uint32_t lastBadges = 0xFFFFFFFFu;
    static int logged = 0;
    const std::uint32_t badges = out.shown & ((1u << shared::kHudWeaponBadge) | (1u << shared::kHudNadeBadge));
    if ((hud != lastHud || rs != lastRs || badges != lastBadges) && logged < 30) {
        ++logged;
        lastHud = hud;
        lastRs = rs;
        lastBadges = badges;
        const int co = names::PropertyOffset(hud, "CurrentScreenResolution");
        MLOG("hudtex: pass %u -- %s resolutionScale %.3f (bucket %d), canvas %dx%d, %d elements, shown 0x%03X", serial,
             names::Name(hud).c_str(), rs, co >= 0 ? *reinterpret_cast<const std::uint8_t*>(hud + co) : -1, g_w, g_h, got, out.shown);
        static const char* kNames[shared::kHudRects] = {"health", "compass", "stance", "ammo bar", "ammo count", "grenade count",
                                                        "weapon icon", "grenade icon", "weapon badge", "grenade badge",
                                                        "weapon medal", "grenade medal"};
        for (int i = 0; i < shared::kHudRects; ++i)
            MLOG("hudtex:   %-13s x %6.1f..%6.1f  y %6.1f..%6.1f%s", kNames[i], R[i][0], R[i][2], R[i][1], R[i][3],
                 (out.shown >> i) & 1u ? "  (shown)" : "");
    }
}

// --- Game thread hooks --------------------------------------------------------------------------------------------------
void OnCanvasFlush(SafetyHookContext& ctx) {
    if (!g_pass) return;
    const std::uintptr_t canvas = *reinterpret_cast<const std::uintptr_t*>(ctx.esp + 4);
    const std::uintptr_t batch = canvas ? *reinterpret_cast<const std::uintptr_t*>(canvas + addr::kCanvasBatch) : 0;
    if (!batch) return;  // Flush returns at once
    const std::uint32_t h = g_head.load(std::memory_order_relaxed);
    if (h - g_tail.load(std::memory_order_acquire) >= kRing) {  // the render thread is behind: this batch draws in the eye
        ++g_overflow;
        return;
    }
    g_ring[h % kRing] = {batch, g_serial};
    g_head.store(h + 1, std::memory_order_release);
    ++g_pushed;
}

void OnClosingFlushDone(SafetyHookContext&) {
    if (!g_pass) return;
    g_pass = false;
    ReadRects(g_hud, g_serial);
}

// --- Render thread: FlushCommand::Execute -------------------------------------------------------------------------------
// Finds `batch` in the ring; entries before it (never executed -- should not happen) are dropped.
bool Pop(std::uintptr_t batch, std::uint32_t& serial) {
    const std::uint32_t t = g_tail.load(std::memory_order_relaxed);
    const std::uint32_t h = g_head.load(std::memory_order_acquire);
    for (std::uint32_t i = t; i != h; ++i) {
        if (g_ring[i % kRing].batch == batch) {
            serial = g_ring[i % kRing].serial;
            if (i != t) g_skipped += static_cast<long>(i - t);
            g_tail.store(i + 1, std::memory_order_release);
            return true;
        }
    }
    return false;
}

int __fastcall Hook_Execute(std::uint8_t* cmd, void* /*edx*/) {
    const std::uintptr_t batch = *reinterpret_cast<const std::uintptr_t*>(cmd + addr::kFlushCmdBatch);
    std::uint32_t serial = 0;
    IDirect3DDevice9* dev = g_dev.load(std::memory_order_acquire);
    if (!batch || !dev || !Pop(batch, serial)) return g_execHook.thiscall<int>(cmd);

    LARGE_INTEGER q0, q1, q2;
    QueryPerformanceCounter(&q0);
    q1 = q2 = q0;
    EnterCriticalSection(&g_rtLock);
    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DSurface9* oldDs = nullptr;
    D3DVIEWPORT9 vp{};
    RECT sc{};
    DWORD scOn = 0;
    DWORD alphaSaved[5] = {};
    const D3DRENDERSTATETYPE alphaStates[5] = {D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA,
                                               D3DRS_BLENDOPALPHA, D3DRS_COLORWRITEENABLE};
    const bool haveRt = SUCCEEDED(dev->GetRenderTarget(0, &oldRt));
    dev->GetDepthStencilSurface(&oldDs);  // D3DERR_NOTFOUND (null) when none is bound
    dev->GetViewport(&vp);
    dev->GetScissorRect(&sc);
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scOn);
    static int logged = 0;
    const bool log = logged < 2 || g_logAfterReset > 0;
    if (g_logAfterReset > 0) --g_logAfterReset;
    if (log) {
        ++logged;
        D3DSURFACE_DESC od{};
        if (oldRt) oldRt->GetDesc(&od);
        MLOG("hudtex: batch %08X (pass %u) -- the command's size %d x %d; bound target %ux%u, depth %s, viewport (%lu,%lu) %lux%lu, "
             "scissor %s", static_cast<unsigned>(batch), serial, *reinterpret_cast<const int*>(cmd + addr::kFlushCmdSizeX),
             *reinterpret_cast<const int*>(cmd + addr::kFlushCmdSizeY), od.Width, od.Height, oldDs ? "bound" : "none", vp.X, vp.Y,
             vp.Width, vp.Height, scOn ? "on" : "off");
    }
    int r = 0;
    if (g_rt && haveRt) {
        dev->SetRenderTarget(0, g_rt);  // (resets the viewport and the scissor rect to the whole texture)
        dev->SetDepthStencilSurface(nullptr);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        if (g_realSetRenderState) {
            for (int i = 0; i < 5; ++i) {
                dev->GetRenderState(alphaStates[i], &alphaSaved[i]);
                g_realSetRenderState(dev, alphaStates[i], ForcedValue(alphaStates[i], alphaSaved[i]));
            }
            g_forceAlpha = true;
        }
        QueryPerformanceCounter(&q1);
        r = g_execHook.thiscall<int>(cmd);
        QueryPerformanceCounter(&q2);
        g_forceAlpha = false;
        if (g_realSetRenderState)
            for (int i = 0; i < 5; ++i) g_realSetRenderState(dev, alphaStates[i], alphaSaved[i]);
        dev->SetRenderTarget(0, oldRt);
        dev->SetDepthStencilSurface(oldDs);
        ++g_redirected;
        ++g_frameBatches;
        g_frameSerial = serial;
    } else {
        // No texture (a device reset in between): the batch is dropped -- an empty scissor, never the eye.
        const RECT none{0, 0, 0, 0};
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        dev->SetScissorRect(&none);
        r = g_execHook.thiscall<int>(cmd);
        ++g_dropped;
    }
    dev->SetViewport(&vp);
    dev->SetScissorRect(&sc);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scOn);
    if (oldRt) oldRt->Release();
    if (oldDs) oldDs->Release();
    LeaveCriticalSection(&g_rtLock);
    LARGE_INTEGER q3;
    QueryPerformanceCounter(&q3);
    g_drawTicks += q2.QuadPart - q1.QuadPart;
    g_overheadTicks += (q3.QuadPart - q0.QuadPart) - (q2.QuadPart - q1.QuadPart);
    return r;
}

bool MidHook(SafetyHookMid& slot, std::uintptr_t va, safetyhook::MidHookFn fn, const char* what) {
    auto res = safetyhook::MidHook::create(reinterpret_cast<void*>(va), fn);
    if (!res) {
        MLOG("hudtex: MidHook %s at 0x%08X failed (error %d) -- standing down", what, static_cast<unsigned>(va),
             static_cast<int>(res.error().type));
        return false;
    }
    slot = std::move(*res);
    MLOG("hudtex: MidHook %s at 0x%08X installed", what, static_cast<unsigned>(va));
    return true;
}

bool WriteBmp32(const std::wstring& path, const D3DLOCKED_RECT& lr, UINT w, UINT h) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = -static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;  // the 4th byte is the alpha as rendered (tools read it raw)
    const DWORD rowBytes = w * 4;
    ih.biSizeImage = rowBytes * h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(f, &fh, sizeof(fh), &wr, nullptr) && WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
    const auto* src = static_cast<const BYTE*>(lr.pBits);
    for (UINT y = 0; ok && y < h; ++y) ok = WriteFile(f, src + static_cast<size_t>(y) * lr.Pitch, rowBytes, &wr, nullptr) != 0;
    CloseHandle(f);
    return ok;
}

}  // namespace

bool Install(const Config& cfg) {
    InitializeCriticalSection(&g_rtLock);
    g_placeDefault = cfg.hudPlace;
    if (cfg.hudMode != 1 || !cfg.hudRedirect) {
        MLOG("hudtex: the wrist HUD's redirect not installed (%s) -- the HUD stays on screen",
             cfg.hudMode != 1 ? "HUD.Mode is not 1" : "HUD.Redirect=0");
        return false;
    }
    g_w = cfg.hudCanvas & ~1;
    g_h = (g_w * 9 / 16) & ~1;
    // Standing rule 4: the build check verified these signatures; check them again next to the code that hooks them.
    const auto vt = reinterpret_cast<const std::uintptr_t*>(addr::kFlushCommandVtable);
    if (!patch::BytesMatch(addr::kCanvasFlush, addr::kCanvasFlushBytes, sizeof(addr::kCanvasFlushBytes)) ||
        !patch::BytesMatch(addr::kFlushCommandExecute, addr::kFlushCommandExecuteBytes, sizeof(addr::kFlushCommandExecuteBytes)) ||
        !patch::BytesMatch(addr::kHudClosingFlushDone, addr::kHudClosingFlushDoneBytes, sizeof(addr::kHudClosingFlushDoneBytes)) ||
        vt[1] != addr::kFlushCommandExecute) {
        MLOG("hudtex: a prologue or the FlushCommand vtable doesn't match -- standing down (the HUD stays on screen)");
        return false;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kFlushCommandExecute), reinterpret_cast<void*>(&Hook_Execute));
    if (!res) {
        MLOG("hudtex: inline hook on FlushCommand::Execute failed (error %d) -- standing down", static_cast<int>(res.error().type));
        return false;
    }
    g_execHook = std::move(*res);
    if (!MidHook(g_closeHook, addr::kHudClosingFlushDone, OnClosingFlushDone, "HUD closing flush done") ||
        !MidHook(g_flushHook, addr::kCanvasFlush, OnCanvasFlush, "FCanvas::Flush")) {
        g_closeHook = {};
        g_flushHook = {};
        g_execHook = {};
        return false;
    }
    g_installed = true;
    MLOG("hudtex: installed -- the wrist HUD's pass draws into a %dx%d texture of the mod's own (Place %s until the host says)",
         g_w, g_h, g_placeDefault ? "wrist" : "screen");
    return true;
}

bool Installed() { return g_installed; }

void CanvasSize(int& w, int& h) {
    w = g_w;
    h = g_h;
}

void SetShared(bool ok) { g_shared.store(ok, std::memory_order_release); }

bool PlanDraw(const shared::Header* hdr) {
    g_pass = false;
    const std::uint32_t hp = hdr ? hdr->hudPlace : 0u;
    const bool wrist = hp == 2u || (hp == 0u && g_placeDefault == 1);
    g_thisDraw = g_installed && wrist && !g_failed && g_rtReady.load(std::memory_order_acquire) &&
                 g_shared.load(std::memory_order_acquire) && g_alphaOk.load(std::memory_order_acquire);
    static int seen = -1, logged = 0;
    const int now = g_thisDraw ? 1 : 0;
    if (now != seen && logged < 40) {
        ++logged;
        MLOG("hudtex: the HUD -> %s (host place %u, default %s%s%s%s%s)", now ? "the wrist (the HUD texture)" : "the screen panel", hp,
             g_placeDefault ? "wrist" : "screen", g_failed ? "; a pass failed" : "",
             g_rtReady.load() ? "" : "; no render target yet", g_shared.load() ? "" : "; no shared ring",
             g_alphaOk.load() ? "" : "; alpha not forced");
        seen = now;
    }
    return g_thisDraw;
}

bool ThisDraw() { return g_thisDraw; }

void BeginPass(std::uintptr_t localPlayer) {
    if (!g_thisDraw) return;
    ++g_serial;
    g_pass = true;
    ++g_passes;
    const std::uintptr_t ctrl = localPlayer ? names::ReadPointer(localPlayer + addr::kLocalPlayerActor) : 0;
    g_hud = ObjProp(ctrl, "myHUD");
    g_ctrl = ctrl;
}

void FailPass() {
    if (!g_failed) MLOG("hudtex: the HUD canvas matrix is not identity-plus-translation -- the wrist HUD falls back to the screen "
                        "panel for this session");
    g_failed = true;
    g_thisDraw = false;
    g_pass = false;
}

void EndDraw() {
    g_thisDraw = false;
    g_pass = false;
}

void OnPresent(IDirect3DDevice9* dev) {
    if (!g_installed || !dev) return;
    if (g_dev.load(std::memory_order_relaxed) != dev) g_dev.store(dev, std::memory_order_release);
    HookSetRenderState(dev);  // (a device Reset restores the slot: checked every Present)
    EnterCriticalSection(&g_rtLock);
    if (!g_rt) {
        const HRESULT hr = dev->CreateRenderTarget(static_cast<UINT>(g_w), static_cast<UINT>(g_h), D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE,
                                                   0, FALSE, &g_rt, nullptr);
        static int tries = 0;
        if (FAILED(hr)) {
            g_rt = nullptr;
            if (++tries <= 3) MLOG("hudtex: CreateRenderTarget %dx%d failed 0x%08lX", g_w, g_h, static_cast<unsigned long>(hr));
        } else {
            MLOG("hudtex: render target %dx%d A8R8G8B8 %s", g_w, g_h, tries++ ? "recreated after a device reset" : "created");
            dev->ColorFill(g_rt, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
            if (tries > 1) g_logAfterReset = 2;
        }
    }
    g_rtReady.store(g_rt != nullptr, std::memory_order_release);
    LeaveCriticalSection(&g_rtLock);

    static DWORD last = GetTickCount();
    const DWORD now = GetTickCount();
    if (now - last >= 10000) {
        last = now;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const long passes = g_passes.exchange(0);
        const double ovMs = 1000.0 * static_cast<double>(g_overheadTicks.exchange(0)) / static_cast<double>(f.QuadPart);
        const double drMs = 1000.0 * static_cast<double>(g_drawTicks.exchange(0)) / static_cast<double>(f.QuadPart);
        const long pushed = g_pushed.exchange(0);
        if (passes || pushed)
            MLOG("hudtex: 10 s -- %ld passes, %ld batches recorded, %ld redirected, %ld dropped (no texture), %ld skipped, %ld "
                 "overflowed, %ld clears; render thread: the redirect's own work %.1f us a pass, the HUD batches' draws %.1f us a "
                 "pass", passes, pushed, g_redirected.exchange(0), g_dropped.exchange(0), g_skipped.exchange(0),
                 g_overflow.exchange(0), g_clears.exchange(0), passes ? 1000.0 * ovMs / passes : 0.0,
                 passes ? 1000.0 * drMs / passes : 0.0);
    }
}

IDirect3DSurface9* ForPublish(shared::SlotHud& slot) {
    slot = shared::SlotHud{};
    if (!g_installed) return nullptr;
    EnterCriticalSection(&g_rtLock);
    IDirect3DSurface9* s = g_rt;
    if (s) s->AddRef();
    const long batches = g_frameBatches;
    const std::uint32_t serial = g_frameSerial;
    LeaveCriticalSection(&g_rtLock);
    if (!s) return nullptr;
    slot.canvasW = static_cast<std::uint32_t>(g_w);
    slot.canvasH = static_cast<std::uint32_t>(g_h);
    slot.rs = 1.0f;
    if (batches > 0) {
        slot.flags |= 1u | 2u;
        const PassRects& pr = g_rects[serial % kPassSlots];
        if (pr.serial.load(std::memory_order_acquire) == serial && pr.valid) {
            slot.flags |= 4u | (pr.dead ? 32u : 0u);
            slot.rs = pr.rs;
            slot.shown = pr.shown;
            std::memcpy(slot.rect, pr.rect, sizeof(slot.rect));
        } else {
            // (the pass's rectangles not there: the last valid ones, any serial -- the layout rarely changes)
            for (std::uint32_t k = 1; k <= kPassSlots; ++k) {
                const PassRects& o = g_rects[(serial - k) % kPassSlots];
                if (o.valid && o.serial.load(std::memory_order_acquire) != 0) {
                    slot.flags |= 4u | (o.dead ? 32u : 0u);
                    slot.rs = o.rs;
                    slot.shown = o.shown;
                    std::memcpy(slot.rect, o.rect, sizeof(slot.rect));
                    break;
                }
            }
        }
    }
    return s;
}

void EndFrame(IDirect3DDevice9* dev) {
    if (!g_installed || !dev) return;
    EnterCriticalSection(&g_rtLock);
    if (g_rt && g_frameBatches > 0) {
        dev->ColorFill(g_rt, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
        ++g_clears;
    }
    g_frameBatches = 0;
    LeaveCriticalSection(&g_rtLock);
}

void OnCapture(IDirect3DDevice9* dev, const std::wstring& dir) {
    if (!g_installed || !dev || !g_rtReady.load()) return;
    EnterCriticalSection(&g_rtLock);
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = g_rt ? dev->CreateOffscreenPlainSurface(static_cast<UINT>(g_w), static_cast<UINT>(g_h), D3DFMT_A8R8G8B8,
                                                         D3DPOOL_SYSTEMMEM, &sys, nullptr)
                      : E_POINTER;
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(g_rt, sys);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        const std::wstring tmp = dir + L"\\capture-hud.tmp", out = dir + L"\\capture-hud.bmp";
        const bool ok = WriteBmp32(tmp, lr, static_cast<UINT>(g_w), static_cast<UINT>(g_h));
        sys->UnlockRect();
        if (!(ok && MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING))) MLOG("hudtex: writing capture-hud.bmp failed");
        static int n = 0;
        if (ok && ++n <= 3) MLOG("hudtex: texture %dx%d -> %%TEMP%%\\MOHAVR\\capture-hud.bmp", g_w, g_h);
    } else {
        MLOG("hudtex: texture capture failed 0x%08lX", static_cast<unsigned long>(hr));
    }
    if (sys) sys->Release();
    LeaveCriticalSection(&g_rtLock);
}

bool TestCommand(std::uintptr_t player, const wchar_t* line) {
    if (wcsncmp(line, L"mohavr hud ", 11) != 0) return false;
    const wchar_t* arg = line + 11;
    const std::uintptr_t ctrl = player ? names::ReadPointer(player + addr::kLocalPlayerActor) : 0;
    const std::uintptr_t hud = ObjProp(ctrl, "myHUD");
    int yaw = 0;
    if (swscanf_s(arg, L"hit %d", &yaw) == 1) {
        // MOHAHUD.NotifyHitIndicator(Rotator rHitDirection): a hit indicator from that direction (2.5 s).
        script::Call c(hud, "NotifyHitIndicator");
        const int rot[3] = {0, yaw, 0};  // Pitch, Yaw, Roll
        const bool ok = c.Set("rHitDirection", rot, sizeof(rot)) && c.Run();
        MLOG("test: hud hit from yaw %d -> %s", yaw, ok ? "shown" : "failed");
        return true;
    }
    if (!wcscmp(arg, L"objective")) {
        // MOHAHUD.SetObjectiveText(NewText, newTitle, fade in, hold, fade out): an objective message (the strings are
        // copied by the script; the parameters are never destroyed by ProcessEvent).
        struct FStr {
            const wchar_t* data;
            int            num, max;
        };
        static const wchar_t kText[] = L"Wrist HUD test: this objective stays in view", kTitle[] = L"OBJECTIVE";
        const FStr text{kText, static_cast<int>(wcslen(kText) + 1), static_cast<int>(wcslen(kText) + 1)};
        const FStr title{kTitle, static_cast<int>(wcslen(kTitle) + 1), static_cast<int>(wcslen(kTitle) + 1)};
        const float fin = 0.3f, hold = 6.0f, fout = 1.0f;
        script::Call c(hud, "SetObjectiveText");
        const bool ok = c.Set("NewText", &text, sizeof(text)) && c.Set("newTitle", &title, sizeof(title)) &&
                        c.Set("fFadeInTime", &fin, 4) && c.Set("fMaxAlphaTime", &hold, 4) && c.Set("fFadeOutTime", &fout, 4) && c.Run();
        MLOG("test: hud objective -> %s", ok ? "shown" : "failed");
        return true;
    }
    if (!wcscmp(arg, L"badges")) {
        // MOHAHUD.ToggleWeaponInfo(true, 6 s, Both, FOLDER_INFO): the level badges shown (as on a weapon switch).
        script::Call c(hud, "ToggleWeaponInfo");
        const std::uint32_t on = 1;
        const float dur = 6.0f;
        const std::uint8_t both = 2, folder = 1;
        const bool ok = c.Set("Enabled", &on, 4) && c.Set("Duration", &dur, 4) && c.Set("infoType", &both, 1) &&
                        c.Set("complexity", &folder, 1) && c.Run();
        MLOG("test: hud badges -> %s", ok ? "shown" : "failed");
        return true;
    }
    if (!wcscmp(arg, L"status")) {
        MLOG("test: hud -- redirect %s, %s, render target %s, shared ring %s, passes %u; HUD %s", g_installed ? "installed" : "off",
             g_thisDraw ? "this Draw on the wrist" : "this Draw on the screen", g_rtReady.load() ? "ready" : "none",
             g_shared.load() ? "ready" : "none", g_serial, hud ? names::Name(hud).c_str() : "(none)");
        return true;
    }
    MLOG("test: unknown hud command '%ls'", arg);
    return true;
}

void OnBeforeReset() {
    if (!g_installed) return;
    EnterCriticalSection(&g_rtLock);
    g_rtReady.store(false, std::memory_order_release);
    if (g_rt) {
        g_rt->Release();
        g_rt = nullptr;
        MLOG("hudtex: render target released for a device Reset");
    }
    g_frameBatches = 0;
    LeaveCriticalSection(&g_rtLock);
}

}  // namespace mohavr::hudtex
