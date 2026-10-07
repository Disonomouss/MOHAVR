#include "blit.hpp"

#include <d3dcompiler.h>

#include <cstring>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {
// A full-screen triangle; each pixel loads its own texel (no filtering, no sampler).
const char* kShader = R"(
Texture2D src : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target { return src.Load(int3(pos.xy, 0)); }
)";
}  // namespace

bool Blit::Init(ID3D11Device* dev) {
    dev_ = dev;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    const size_t len = std::strlen(kShader);
    if (FAILED(D3DCompile(kShader, len, "blit", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, len, "blit", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        MLOG("blit: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        if (vsb) vsb->Release();
        return false;
    }
    const bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) &&
                    SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_));
    vsb->Release();
    psb->Release();
    if (!ok) MLOG("blit: CreateVertexShader / CreatePixelShader failed");
    return ok;
}

void Blit::Run(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, unsigned w, unsigned h) {
    if (!vs_ || !ps_ || !src || !dst) return;
    if (src != srcTex_) {
        if (srv_) srv_->Release(), srv_ = nullptr;
        srcTex_ = src;
        if (FAILED(dev_->CreateShaderResourceView(src, nullptr, &srv_))) return;
    }
    if (dst != dstTex_) {
        if (rtv_) rtv_->Release(), rtv_ = nullptr;
        dstTex_ = dst;
        if (FAILED(dev_->CreateRenderTargetView(dst, nullptr, &rtv_))) return;
    }
    if (!srv_ || !rtv_) return;
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->PSSetShader(ps_, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv_);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

}  // namespace mohavr::host
