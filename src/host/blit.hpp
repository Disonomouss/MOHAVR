// A texel-exact copy through a shader (GOAL B2, D65): the game's BGRA frame into an R8G8B8A8 texture when the runtime's
// swapchain is RGBA (a raw copy between the two families is invalid; a shader read returns logical RGBA either way).
#pragma once
#include <d3d11.h>

namespace mohavr::host {

class Blit {
public:
    bool Init(ID3D11Device* dev);
    // `src` (bindable as a shader resource) -> `dst` (a render target), both w x h.
    void Run(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, unsigned w, unsigned h);

private:
    ID3D11Device*       dev_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader*  ps_ = nullptr;
    ID3D11Texture2D*    srcTex_ = nullptr;   // the views below are for these (made once, kept)
    ID3D11Texture2D*    dstTex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11RenderTargetView*   rtv_ = nullptr;
};

}  // namespace mohavr::host
