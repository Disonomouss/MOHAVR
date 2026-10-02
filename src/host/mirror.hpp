// The desktop mirror (MOHAVR-host.exe, Bridge.Mirror). Under D3D9On12 the game's own window stays
// white (ENGINE-NOTES 5e), so the host shows what the headset gets on the monitor instead:
//   1 = over the game window: a click-through, never-activated window kept on the game's client
//       area while the game is in the foreground (hidden otherwise);
//   2 = its own movable, resizable window ("MOHAVR mirror").
// It shows the centre of the left eye cropped to the window's shape (the whole frame when the game
// renders mono). No shaders: the swapchain's buffers are the crop's size and DXGI stretches them.
// Costs the game nothing -- everything is in the host process.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

class Mirror {
public:
    // mode: 1 overlay, 2 own window. False (logged) if it can't start; the host carries on without.
    bool Init(ID3D11Device* dev, DWORD gamePid, int mode);
    // Per XR frame, after the newest game frame was copied into `frame` (meta = how it was rendered).
    void Update(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, const shared::SlotMeta& meta, bool haveFrame,
                std::uint32_t eyeWidth = 0);  // (v24: each eye's width with a scope column)
    void Shutdown();

private:
    HWND FindGameWindow() const;
    bool Place();  // mode 1: follow the game's client area; false while it should be hidden

    ID3D11Device*    dev_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    HWND             hwnd_ = nullptr;
    HWND             game_ = nullptr;
    DWORD            gamePid_ = 0;
    int              mode_ = 0;
    UINT             bufW_ = 0, bufH_ = 0;
    RECT             placed_{};
    bool             shown_ = false;
    DWORD            nextFind_ = GetTickCount();  // (not 0: past 2^31 ms of uptime the window would never be found)
    unsigned         presents_ = 0;
};

}  // namespace mohavr::host
