// Manual reload (D21, RELOAD-DESIGN.md) -- host side.
// M1: the player's toggle ([Weapon] ManualReload; the menu), "engaged" (the game's side alive, the hands tracked, the
// hook in place) and the events to the game (shared block v14, an ordered ring). The test channel (pad_cmd.txt
// "reload=eject|take|insert|rack|drop") queues events directly.
// M3, the magazine (RELOAD-DESIGN 3.2), while the manual reload drives the gun in hand ("Active", 3.1):
//   * the gun hand's release button (B right / Y left, [ManualReload] ReleaseButton) drops it (EJECT); the button is kept
//     from the pad meanwhile;
//   * the off hand's grip at the magazine grabs it; pulled out along its way out by PullOut cm it comes away in the hand
//     (EJECT) as it was held; let go before that, it slides back;
//   * with the gun's magazine out, the grip in the belt pouch ([Holsters] MagPouchSpot) takes a new one (TAKE; none
//     while the reserve is empty);
//   * brought to the gun's magazine well (within InsertRadius cm, its way out within InsertAngle degrees of the well's)
//     it goes in (INSERT); let go anywhere else it is dropped (DROP).
// M4, the action (RELOAD-DESIGN 3.3): the off hand's grip at the handle / slide takes it; the drawn action follows the
// pull back. One not held back is armed at RackArm of its travel (at least RackMin cm) and racks (RACK) when let go or
// brought forward again; one held back (locked open, or the empty cue of a handle) racks with a tug of RackTug cm and
// letting go. The game decides what a rack does (feeds a round, cocks an open bolt).
// Twin (taped) magazines (D21): a pair in the off hand is flipped with that hand's trigger (kept from the pad meanwhile);
// inserted flipped, its other half goes in (INSERT_OTHER), each half keeping its own rounds (the game's count).
// The game draws the magazine where the host says (bits 1-2 of reloadFlags, magPull, magPose) and keeps the ammo.
#pragma once
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <deque>
#include <string>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

class ManualReload {
public:
    // The shipped ini: [Weapon] ManualReload (the default; the menu's toggle is the player's), [ManualReload] tuning.
    void Init(const std::wstring& ini);
    void SetOn(bool on);
    bool On() const { return on_; }
    // Round 31: the grab rings' scale (the magazine's and the action's; the player's, the menu).
    void SetRingScale(float s) { ringScale_ = s > 0.2f ? s : 0.2f; }
    // The release button: 0 none, 1 upper (B / Y), 2 lower (A / X).
    int  ReleaseButton() const { return releaseButton_; }
    // Start of the XR frame: the game's side (geometry, ammo, state, acknowledgements) and the reconcile (3.2).
    void Poll(shared::Header* hdr, double now, bool handsOk);

    struct In {
        bool    gunOk = false, offOk = false;  // the gun hand holds the gun; the off hand is tracked
        bool    gestures = false;              // no menu open
        bool    hasView = false;               // the game draws a head-tracked view
        int     gunHand = 1;                   // 0 left, 1 right
        XrPosef gun{}, off{};                  // the final gun pose (after the foregrip turn); the off hand (its hand point)
        XrPosef offAim{};                      // the off hand's aim pose as tracked (the game's frame for magHeld)
        bool    offHeld = false;               // the off hand's grip held (hands.cpp's hysteresis)
        float   release[2]{};                  // the release button, per physical hand
        float   fitAngle = 0.0f;               // the gun fit's angle (degrees): a pouch magazine sits in the hand likewise
        float   offTrigger = 0.0f;             // the off hand's trigger: flips a held taped pair
        double  now = 0.0;
    };
    struct Ring {
        XrVector3f pos;
        float      radius;
        bool       inside, close;
    };
    struct Out {
        Ring  rings[2]{};  // the magazine's grab spot (in the gun) or the well (a magazine in the hand); the action
        int   ringCount = 0;
        float pulseAmp[2]{}, pulseMs[2]{};
        bool  mask[2]{};    // keep that hand's release button from the pad
        bool  maskTrigger[2]{};  // keep that hand's trigger from the pad (the flip of a held taped pair)
        bool  targetOk[3]{};
        XrVector3f target[3]{};  // tests: 0 the magazine's grab point, 1 the action's, 2 where the off hand's aim point
                                 // puts a held magazine's grab point at the well
    };
    // Hands::Update, first: whether the manual reload drives the gun this frame (3.1). Leaving it lets go of what the
    // off hand holds (a grabbed magazine slides back, one in the hand is dropped).
    bool Begin(const In& in);
    bool Active() const { return active_; }
    bool MagazineOut() const { return mag_ == kOut; }
    // Hands::Update, at an off-hand grip press, before the holsters and the foregrip: true = a reload spot took it (the
    // pouch while the magazine is out, the magazine while in; the nearest by distance / radius; 3.4).
    bool TakePress(const XrVector3f& hand, const XrVector3f& pouch, float pouchR);
    // Hands::Update, once the gun's pose is final.
    void Frame(const In& in, Out& out);
    // After Hands::Update: the queued events to the game (never more than the ring holds unread).
    void Send(shared::Header* hdr, double now);
    void Queue(std::uint32_t type, double now);
    // Written inside the view seqlock.
    std::uint32_t Flags() const;
    std::uint32_t KeyHash() const { return keyHash_; }
    float         MagPull() const { return mag_ == kGrabbed ? pull_ : 0.0f; }
    shared::Pose  MagPose() const;
    float         Rack() const { return boltHeld_ ? rack_ : 0.0f; }

private:
    enum Mag { kInGun = 0, kGrabbed = 1, kInHand = 2, kOut = 3 };
    enum Press { kPressNone, kPressMag, kPressPouch, kPressBolt };
    void Pulse(Out& out, int hand, float amp, float ms) const;
    void SetMag(Mag m, const char* why);

    bool          on_ = false;
    int           releaseButton_ = 1;
    float         pullOut_ = 0.04f, insertR_ = 0.05f, insertAngle_ = 40.0f;  // metres, degrees
    float         boltGrabR_ = 0.05f, rackArm_ = 0.85f, rackMin_ = 0.04f, rackTug_ = 0.01f;
    float         ringScale_ = 1.0f;
    float         hold_[3]{};                                                // metres, the left off hand's frame
    std::uint32_t keyHash_ = 0;
    std::uint32_t lastGeoSeq_ = 0, pawnSeq_ = 0;
    double        geoAt_ = -1.0;
    shared::ReloadGeo geo_{};
    bool          engaged_ = false, active_ = false;
    std::string   stateKey_;
    Mag           mag_ = kInGun;
    int           disagree_ = 0;
    Press         press_ = kPressNone;
    XrVector3f    start_{};          // where the grab began
    float         pull_ = 0.0f;
    XrPosef       heldRel_{};        // the held magazine's grab-point frame in the off hand's
    XrPosef       magPose_{};
    bool          armed_ = false;    // the held magazine has been away from the well (no insert straight after a pull)
    bool          snapHeld_ = false; // round 31: put a held magazine straight into the grip (a pouch one), else ease
    double        lastNow_ = 0.0, nearMissAt_ = 0.0;
    bool          lastGunOk_ = false;
    XrVector3f    lastGrabW_{};      // last frame's grab point (the press test comes before this frame's gun)
    XrVector3f    lastBoltW_{};      // likewise the action's
    bool          boltHeld_ = false, rackArmed_ = false, tug_ = false;  // the off hand on the action; armed; a tug
    XrVector3f    boltStart_{};
    float         rack_ = 0.0f;      // 0..1 of the travel pulled
    bool          relHeld_[2]{}, maskLatch_[2]{};
    bool          flipped_ = false;                // the held taped pair turned: its other half toward the well
    bool          trigHeld_ = false, trigLatch_ = false;
    struct Pending {
        std::uint32_t type, hash;
        double        at;
    };
    std::deque<Pending> pending_;
    std::string   loggedKey_;
    std::uint32_t loggedState_ = 0xFFFFFFFFu;
    bool          loggedEngaged_ = false;
    std::string   whyInactive_;
};

}  // namespace mohavr::host
