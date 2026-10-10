// D79: the mission loadout's weapon list, workable with a pad ([Controls] LoadoutList).
// The game's WeaponsLoadout scene opens a UIList (WeaponList) per slot, and the list submits its index on the first input
// it gets -- any key, button, stick or mouse move -- then closes. A navigation moves the index first (so Down picked the
// next gun, and with a pad the third was never reachable), anything else submits the gun at index 0. The same in the
// unmodded game: the flat game's mouse click hides it. Measured: UIList.SetIndex(n, clamp, notify) moves the highlight and
// the stats without closing; the next A submits index n.
// So while such a list has the focus, up / down (the D-pad and the left stick) are taken out of the pad state the game
// sees and move the index here (with a repeat while held); A and B go through as they are.
#include "loadout.hpp"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "addresses.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"
#include "vr_view.hpp"

namespace mohavr::loadout {
using namespace script;
namespace {

bool g_fix = true;
std::atomic<bool> g_active{false};  // a loadout list has the focus (OnDraw -> FilterPad)
std::atomic<int>  g_steps{0};       // up / down presses taken from the pad, for OnDraw to apply (+1 down)

// The UI's active scenes: GEngine's first LocalPlayer -> ViewportClient -> UIController -> SceneClient.
std::uintptr_t SceneClient() {
    const auto engine = *reinterpret_cast<const std::uintptr_t*>(addr::kGEngine);
    const auto* arr = engine ? reinterpret_cast<const std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;
    const std::uintptr_t player = (arr && arr[1] >= 1 && arr[0]) ? *reinterpret_cast<const std::uintptr_t*>(arr[0]) : 0;
    const std::uintptr_t vc = player ? Obj(player, "ViewportClient") : 0;
    const std::uintptr_t ui = vc ? Obj(vc, "UIController") : 0;
    return ui ? Obj(ui, "SceneClient") : 0;
}

// A TArray<UObject*> property: data, count.
int Array(std::uintptr_t obj, const char* name, std::uintptr_t& data) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    data = o >= 0 ? names::ReadPointer(obj + o) : 0;
    const int n = o >= 0 ? static_cast<int>(names::ReadPointer(obj + o + 4)) : 0;
    return data && n > 0 && n < 4096 ? n : 0;
}

std::uintptr_t Focused(std::uintptr_t scene) {
    Call f(scene, "GetFocusedControl", true);
    if (!f.ok) return 0;
    const std::uint32_t yes = 1;
    f.Set("bRecurse", &yes, sizeof(yes));
    return f.Run() ? f.ReturnObject() : 0;
}

void LogTree(std::uintptr_t obj, int depth) {
    if (!obj || depth > 6) return;
    std::uintptr_t data = 0;
    const int n = Array(obj, "Children", data);
    std::string pad(static_cast<size_t>(depth) * 2, ' ');
    MLOG("ui: test -- %s%s (%s)%s, %d children", pad.c_str(), names::Name(obj).c_str(), names::ClassName(obj).c_str(),
         Bit(obj, "bHidden") ? " hidden" : "", n);
    for (int i = 0; i < n && i < 64; ++i) LogTree(names::ReadPointer(data + 4u * static_cast<std::uintptr_t>(i)), depth + 1);
}

}  // namespace

// The open scene's focused list (0 none).
std::uintptr_t FocusedList(std::uintptr_t* sceneOut = nullptr) {
    std::uintptr_t data = 0;
    const int n = Array(SceneClient(), "ActiveScenes", data);
    if (!n) return 0;
    const std::uintptr_t scene = names::ReadPointer(data + 4u * static_cast<std::uintptr_t>(n - 1));
    if (sceneOut) *sceneOut = scene;
    const std::uintptr_t f = Focused(scene);
    return f && names::IsA(f, "UIList") ? f : 0;
}

// UIList.SetIndex(NewIndex, bClampValue, bSkipNotification).
bool SetIndex(std::uintptr_t list, int index, bool notify) {
    Call c(list, "SetIndex");
    const std::uint32_t clamp = 1, skip = notify ? 0u : 1u;
    return c.ok && c.Set("NewIndex", &index, sizeof(index)) && c.Set("bClampValue", &clamp, sizeof(clamp)) &&
           c.Set("bSkipNotification", &skip, sizeof(skip)) && c.Run() && c.ReturnBool();
}

void Configure(bool fixList) { g_fix = fixList; }

// Research (the damage tint): WorldInfo's GameModified scene colours, per Draw, for a second after "mohavr pp bullet".
DWORD g_ppTraceUntil = 0;
std::uintptr_t g_ppWorld = 0;
void PpTrace() {
    if (!g_ppTraceUntil || static_cast<LONG>(GetTickCount() - g_ppTraceUntil) > 0 || !g_ppWorld) return;
    auto vec = [&](const char* n, float (&v)[3]) {
        const int o = names::PropertyOffset(g_ppWorld, n);
        v[0] = v[1] = v[2] = -1.0f;
        if (o >= 0) names::ReadVector(g_ppWorld + o, v);
    };
    float hi[3], mid[3], sh[3];
    vec("GameModified_Scene_HighLights", hi);
    vec("GameModified_Scene_MidTones", mid);
    vec("GameModified_Scene_Shadows", sh);
    MLOG("pp: trace -- highlights %.2f %.2f %.2f midtones %.2f %.2f %.2f shadows %.2f %.2f %.2f desaturation %.2f", hi[0], hi[1], hi[2],
         mid[0], mid[1], mid[2], sh[0], sh[1], sh[2], Float(g_ppWorld, "GameModified_Scene_Desaturation", -1.0f));
}

DWORD g_ppHoldUntil = 0, g_ppNext = 0;
std::uintptr_t g_ppComp = 0;

// Research (D87): the controller's rotation and the game camera, per Draw, for 2 s after "mohavr shake trace".
DWORD g_shakeUntil = 0;
void ShakeTrace() {
    if (!g_shakeUntil || static_cast<LONG>(GetTickCount() - g_shakeUntil) > 0) return;
    const auto engine = *reinterpret_cast<const std::uintptr_t*>(addr::kGEngine);
    const auto* arr = engine ? reinterpret_cast<const std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;
    const std::uintptr_t player = (arr && arr[1] >= 1 && arr[0]) ? *reinterpret_cast<const std::uintptr_t*>(arr[0]) : 0;
    const std::uintptr_t ctrl = player ? Obj(player, "Actor") : 0;
    if (!ctrl) return;
    const int* r = reinterpret_cast<const int*>(ctrl + addr::kActorRotation);
    float loc[3] = {0, 0, 0}, p = 0.0f, y = 0.0f;
    view::GameCamera(loc, p, y);
    const std::uintptr_t pawn = Obj(ctrl, "Pawn");
    const std::uintptr_t weapon = pawn ? Obj(pawn, "Weapon") : 0;
    MLOG("shake: trace -- the controller pitch %d yaw %d; the game camera %.1f %.1f %.1f (pitch %.2f yaw %.2f deg) [%s]", r[0] & 0xFFFF, r[1],
         loc[0], loc[1], loc[2], p * 57.2958f, y * 57.2958f, weapon ? names::ClassName(weapon).c_str() : "-");
}

// Research (the player: "an intermittent twitch in which the body pops in and out"): the eye's place in the world and the
// pawn's, per Draw, for 3 s after "mohavr eye trace".
DWORD g_eyeUntil = 0;
void EyeTrace() {
    if (!g_eyeUntil || static_cast<LONG>(GetTickCount() - g_eyeUntil) > 0) return;
    float head[3], yaw = 0.0f, upm = 100.0f;
    if (!view::HeadInWorld(head, yaw, upm)) return;
    const auto engine = *reinterpret_cast<const std::uintptr_t*>(addr::kGEngine);
    const auto* arr = engine ? reinterpret_cast<const std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;
    const std::uintptr_t player = (arr && arr[1] >= 1 && arr[0]) ? *reinterpret_cast<const std::uintptr_t*>(arr[0]) : 0;
    const std::uintptr_t ctrl = player ? Obj(player, "Actor") : 0;
    const std::uintptr_t pawn = ctrl ? Obj(ctrl, "Pawn") : 0;
    float pl[3] = {0, 0, 0};
    if (pawn) names::ReadVector(pawn + addr::kActorLocation, pl);
    MLOG("eye: trace -- the eye %.2f %.2f %.2f; the pawn %.2f %.2f %.2f", head[0], head[1], head[2], pl[0], pl[1], pl[2]);
}

void OnDraw(shared::Header* /*hdr*/) {
    PpTrace();
    ShakeTrace();
    EyeTrace();
    if (g_ppHoldUntil && static_cast<LONG>(GetTickCount() - g_ppHoldUntil) < 0 && g_ppComp &&
        static_cast<LONG>(GetTickCount() - g_ppNext) >= 0) {
        g_ppNext = GetTickCount() + 250;
        Call c(g_ppComp, "startBulletHit", true);
        if (c.ok) c.Run();
    }
    if (!g_fix) return;
    std::uintptr_t scene = 0;
    const std::uintptr_t list = FocusedList(&scene);
    const bool active = list && names::Name(scene).find("Loadout") != std::string::npos;
    if (active != g_active.load()) {
        MLOG("loadout: %s", active ? ("the weapon list open (" + names::Name(list) + " in " + names::Name(scene) +
                                      "): up / down move it here, A picks").c_str()
                                   : "the weapon list closed");
        g_steps.store(0);
    }
    g_active.store(active);
    if (!active) return;
    const int steps = g_steps.exchange(0);
    if (!steps) return;
    std::uintptr_t items = 0;
    const int count = Array(list, "Items", items);
    const int from = Int(list, "Index", 0);
    const int to = count > 0 ? std::max(0, std::min(count - 1, from + steps)) : from;
    if (to != from) SetIndex(list, to, true);
    static int logged = 0;
    if (logged < 40) {
        ++logged;
        MLOG("loadout: %s -- index %d -> %d (of %d)", steps > 0 ? "down" : "up", from, Int(list, "Index", -1), count);
    }
}

void FilterPad(shared::PadState& pad) {
    if (!g_active.load()) return;
    // Up / down from the D-pad or the left stick (past half), with a repeat while held: 0.45 s, then every 0.18 s.
    constexpr std::uint16_t kUp = 0x0001, kDown = 0x0002;
    const int dir = (pad.buttons & kUp) || pad.thumbLY > 16000 ? -1 : (pad.buttons & kDown) || pad.thumbLY < -16000 ? 1 : 0;
    static int held = 0;
    static DWORD next = 0;
    const DWORD now = GetTickCount();
    if (dir != held) {
        held = dir;
        if (dir) {
            g_steps.fetch_add(dir);
            next = now + 450;
        }
    } else if (dir && static_cast<LONG>(now - next) >= 0) {
        g_steps.fetch_add(dir);
        next = now + 180;
    }
    pad.buttons = static_cast<std::uint16_t>(pad.buttons & ~(kUp | kDown | 0x0004 | 0x0008));
    pad.thumbLX = pad.thumbLY = 0;
}

bool TestCommand(const wchar_t* line) {
    // Research (the damage tint): "mohavr pp bullet|melee|state" -- the player controller's MOHAPostProcessComponent's own
    // effects started, and WorldInfo's colour-curve state logged.
    if (!std::wcscmp(line, L"mohavr eye trace")) {
        g_eyeUntil = GetTickCount() + 3000;
        return true;
    }
    if (!std::wcscmp(line, L"mohavr shake trace")) {
        g_shakeUntil = GetTickCount() + 2000;
        return true;
    }
    if (!std::wcsncmp(line, L"mohavr pp", 9)) {
        const auto engine = *reinterpret_cast<const std::uintptr_t*>(addr::kGEngine);
        const auto* arr = engine ? reinterpret_cast<const std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;
        const std::uintptr_t player = (arr && arr[1] >= 1 && arr[0]) ? *reinterpret_cast<const std::uintptr_t*>(arr[0]) : 0;
        const std::uintptr_t ctrl = player ? Obj(player, "Actor") : 0;
        const std::uintptr_t ppc = ctrl ? Obj(ctrl, "PostProcessComponent") : 0;
        const std::uintptr_t wi = ctrl ? Obj(ctrl, "WorldInfo") : 0;
        const char* fn = !std::wcscmp(line, L"mohavr pp bullet") ? "startBulletHit" : !std::wcscmp(line, L"mohavr pp melee") ? "startMeleeHit" : nullptr;
        bool ran = false;
        g_ppWorld = wi;
        if (fn) g_ppTraceUntil = GetTickCount() + 1200;
        if (!std::wcscmp(line, L"mohavr pp hold")) {  // the bullet hit re-started every 0.25 s for 6 s (captures)
            g_ppComp = ppc;
            g_ppHoldUntil = GetTickCount() + 6000;
            g_ppNext = 0;
        }
        if (fn && ppc) {
            Call c(ppc, fn);
            ran = c.ok && c.Run();
        }
        MLOG("pp: test -- %s%s; the component %s: ColorCorrectionRunning %d bBulletHit %d bMeleeHit %d bLowHealth %d bDoingCurve %d; "
             "WorldInfo bCalcCurves %d, curve main %s secondary %s blend %.2f", fn ? fn : "state", fn ? (ran ? " ran" : " FAILED") : "",
             names::Name(ppc).c_str(), Bit(ppc, "ColorCorrectionRunning") ? 1 : 0, Bit(ppc, "bBulletHit") ? 1 : 0, Bit(ppc, "bMeleeHit") ? 1 : 0,
             Bit(ppc, "bLowHealth") ? 1 : 0, Bit(ppc, "bDoingCurve") ? 1 : 0, Bit(wi, "bCalcCurves") ? 1 : 0,
             names::Name(Obj(wi, "CurveTextureMain")).c_str(), names::Name(Obj(wi, "CurveTextureSecondary")).c_str(),
             Float(wi, "CurveBlendValue", -1.0f));
        return true;
    }
    if (std::wcsncmp(line, L"mohavr ui", 9) != 0) return false;
    int idx = 0;
    wchar_t mode[16] = L"";
    if (swscanf_s(line, L"mohavr ui index %d %15s", &idx, mode, 16) >= 1) {
        const std::uintptr_t list = FocusedList();
        const bool notify = !std::wcscmp(mode, L"notify");
        const int before = list ? Int(list, "Index", -9) : -9;
        const bool ok = list && SetIndex(list, idx, notify);
        MLOG("ui: test -- %s.SetIndex(%d, clamp, %s) %s: Index %d -> %d", list ? names::Name(list).c_str() : "(no list)", idx,
             notify ? "notify" : "silent", ok ? "true" : "false", before, list ? Int(list, "Index", -9) : -9);
        return true;
    }
    const std::uintptr_t sc = SceneClient();
    std::uintptr_t data = 0;
    const int n = Array(sc, "ActiveScenes", data);
    MLOG("ui: test -- the scene client %s, %d active scene(s)", names::Name(sc).c_str(), n);
    for (int i = 0; i < n; ++i) {
        const std::uintptr_t scene = names::ReadPointer(data + 4u * static_cast<std::uintptr_t>(i));
        const std::uintptr_t f = Focused(scene);
        MLOG("ui: test -- scene #%d %s (%s); focused %s (%s)", i, names::Name(scene).c_str(), names::ClassName(scene).c_str(),
             names::Name(f).c_str(), names::ClassName(f).c_str());
        if (f && names::IsA(f, "UIList")) {
            std::uintptr_t items = 0;
            const int count = Array(f, "Items", items);
            std::string list;
            for (int k = 0; k < count && k < 16; ++k) list += " " + std::to_string(static_cast<int>(names::ReadPointer(items + 4u * k)));
            MLOG("ui: test --   the list: Index %d, TopIndex %d, %d items:%s", Int(f, "Index", -9), Int(f, "TopIndex", -9), count, list.c_str());
        }
        if (!std::wcscmp(line, L"mohavr ui tree")) LogTree(scene, 0);
    }
    return true;
}

}  // namespace mohavr::loadout
