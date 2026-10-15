#include "watermark.h"

#include <d3dcompiler.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>

namespace {
// The luma pass writes Y, the chroma pass writes UV at half size, both blended by alpha.
const char kShader[] = R"(
Texture2D tex : register(t0);
SamplerState s : register(s0);
cbuffer C : register(b0) { float now; float3 pad; float2 dot_uv; float2 dot_r; };
struct V { float4 p : SV_Position; float2 uv : TEXCOORD; };
V vs(uint id : SV_VertexID) {
  V o;
  o.uv = float2(id & 1, id >> 1);
  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 Rgba(float2 uv) {
  float4 c = tex.Sample(s, uv);
  float3 rgb = c.a > 0 ? c.rgb / c.a : 0;
  float2 d = (uv - dot_uv) / dot_r;
  if (dot(d, d) < 1.2) rgb *= 0.45 + 0.55 * (0.5 + 0.5 * cos(now * 4.2));
  float x = uv.x + (uv.y - 0.5) * 0.3, sweep = frac(now / 4) * 4 - 0.5;
  rgb = lerp(rgb, 1, 0.45 * saturate(1 - abs(x - sweep) / 0.07));
  return float4(rgb, c.a);
}
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float4 luma(V i) : SV_Target {
  float4 c = Rgba(i.uv);
  return float4(Luma(c.rgb) * 219 / 255 + 16.0 / 255, 0, 0, c.a);
}
float4 chroma(V i) : SV_Target {
  float4 c = Rgba(i.uv);
  float y = Luma(c.rgb);
  return float4((c.b - y) / 1.8556 * 224 / 255 + 128.0 / 255, (c.r - y) / 1.5748 * 224 / 255 + 128.0 / 255, 0, c.a);
})";

com_ptr<ID3DBlob> Compile(const char* entry, const char* target) {
  com_ptr<ID3DBlob> code, err;
  if (FAILED(D3DCompile(kShader, sizeof kShader - 1, nullptr, nullptr, nullptr, entry, target, 0, 0, code.put(),
                        err.put())))
    throw std::runtime_error(err ? static_cast<const char*>(err->GetBufferPointer()) : "watermark shader");
  return code;
}
}  // namespace

// Renders the badge once with Direct2D: dark, with a red edge and record dot, "Clipped with Clip" and the link.
Watermark::Watermark(ID3D11Device* dev, ID3D11DeviceContext* ctx, int frame_w, int frame_h) {
  dev_.copy_from(dev);
  ctx_.copy_from(ctx);
  const float h = std::max(36.f, std::round(frame_h * 0.075f));
  com_ptr<IDWriteFactory> dw;
  winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(dw.put())));
  auto layout = [&](const wchar_t* s, DWRITE_FONT_WEIGHT weight, float size) {
    com_ptr<IDWriteTextFormat> fmt;
    winrt::check_hresult(dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"", fmt.put()));
    com_ptr<IDWriteTextLayout> l;
    winrt::check_hresult(dw->CreateTextLayout(s, UINT32(wcslen(s)), fmt.get(), 2000, h, l.put()));
    return l;
  };
  auto title = layout(L"Clipped with Clip", DWRITE_FONT_WEIGHT_BOLD, h * 0.36f);
  auto link = layout(L"github.com/luca-bv/clip", DWRITE_FONT_WEIGHT_NORMAL, h * 0.24f);
  DWRITE_TEXT_METRICS tm, lm;
  title->GetMetrics(&tm);
  link->GetMetrics(&lm);
  const float pad = h * 0.26f, dot = h * 0.34f, text_x = pad + dot + pad * 0.8f;
  const float text_y = (h - tm.height - lm.height) / 2;
  const UINT w = (UINT(std::ceil(text_x + std::max(tm.width, lm.width) + pad * 1.2f)) + 1) & ~1u,
             ht = (UINT(h) + 1) & ~1u;

  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = ht;
  d.MipLevels = d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  com_ptr<ID3D11Texture2D> tex;
  winrt::check_hresult(dev_->CreateTexture2D(&d, nullptr, tex.put()));

  com_ptr<ID2D1Factory1> factory;
  D2D1_FACTORY_OPTIONS fo{};
  winrt::check_hresult(
      D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, factory.put_void()));
  com_ptr<ID2D1Device> d2d_dev;
  winrt::check_hresult(factory->CreateDevice(dev_.as<IDXGIDevice>().get(), d2d_dev.put()));
  com_ptr<ID2D1DeviceContext> dc;
  winrt::check_hresult(d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
  com_ptr<ID2D1Bitmap1> target;
  const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  winrt::check_hresult(dc->CreateBitmapFromDxgiSurface(tex.as<IDXGISurface>().get(), &props, target.put()));
  com_ptr<ID2D1SolidColorBrush> brush;
  winrt::check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.7f), brush.put()));
  com_ptr<ID2D1RoundedRectangleGeometry> shape;
  const float r = ht * 0.18f;
  const auto badge = D2D1::RoundedRect(D2D1::RectF(0, 0, float(w), float(ht)), r, r);
  winrt::check_hresult(factory->CreateRoundedRectangleGeometry(badge, shape.put()));
  dc->SetTarget(target.get());
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0, 0, 0, 0));
  dc->FillRoundedRectangle(badge, brush.get());
  brush->SetColor(D2D1::ColorF(0.91f, 0.19f, 0.19f, 1));
  dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.get()), nullptr);
  dc->FillRectangle(D2D1::RectF(0, 0, ht * 0.06f, float(ht)), brush.get());  // red edge, rounded with the badge
  dc->PopLayer();
  dc->FillEllipse(D2D1::Ellipse({pad + dot / 2, ht / 2.f}, dot / 2, dot / 2), brush.get());
  brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
  dc->DrawTextLayout({text_x, text_y}, title.get(), brush.get());
  brush->SetColor(D2D1::ColorF(1, 1, 1, 0.75f));
  dc->DrawTextLayout({text_x, text_y + tm.height}, link.get(), brush.get());
  winrt::check_hresult(dc->EndDraw());
  dc->SetTarget(nullptr);
  winrt::check_hresult(dev_->CreateShaderResourceView(tex.get(), nullptr, srv_.put()));

  const LONG margin = LONG(frame_h * 0.025f) & ~1;
  rect_.left = LONG(frame_w - margin - LONG(w)) & ~1;
  rect_.top = LONG(frame_h - margin - LONG(ht)) & ~1;
  rect_.right = rect_.left + LONG(w);
  rect_.bottom = rect_.top + LONG(ht);
  dot_[0] = (pad + dot / 2) / w;
  dot_[1] = 0.5f;
  dot_[2] = dot / 2 / w;
  dot_[3] = dot / 2 / ht;

  auto vs = Compile("vs", "vs_5_0"), luma = Compile("luma", "ps_5_0"), chroma = Compile("chroma", "ps_5_0");
  winrt::check_hresult(dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vs_.put()));
  winrt::check_hresult(dev_->CreatePixelShader(luma->GetBufferPointer(), luma->GetBufferSize(), nullptr, luma_.put()));
  winrt::check_hresult(
      dev_->CreatePixelShader(chroma->GetBufferPointer(), chroma->GetBufferSize(), nullptr, chroma_.put()));
  D3D11_BLEND_DESC bd{};
  auto& rt = bd.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
  rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  winrt::check_hresult(dev_->CreateBlendState(&bd, blend_.put()));
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  winrt::check_hresult(dev_->CreateSamplerState(&sd, sampler_.put()));
  D3D11_BUFFER_DESC cd{32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
  winrt::check_hresult(dev_->CreateBuffer(&cd, nullptr, cb_.put()));
}

void Watermark::Draw(ID3D11Texture2D* nv12, UINT slice, double seconds) {
  auto& [luma_rtv, chroma_rtv] = targets_[{nv12, slice}];
  if (!luma_rtv) {
    D3D11_TEXTURE2D_DESC td;
    nv12->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    if (td.ArraySize > 1) {
      rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
      rd.Texture2DArray = {0, slice, 1};
    } else {
      rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    }
    rd.Format = DXGI_FORMAT_R8_UNORM;
    winrt::check_hresult(dev_->CreateRenderTargetView(nv12, &rd, luma_rtv.put()));
    rd.Format = DXGI_FORMAT_R8G8_UNORM;
    winrt::check_hresult(dev_->CreateRenderTargetView(nv12, &rd, chroma_rtv.put()));
  }
  D3D11_MAPPED_SUBRESOURCE m;
  winrt::check_hresult(ctx_->Map(cb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
  const float c[8] = {float(seconds), 0, 0, 0, dot_[0], dot_[1], dot_[2], dot_[3]};
  memcpy(m.pData, c, sizeof c);
  ctx_->Unmap(cb_.get(), 0);
  auto* cb = cb_.get();
  auto* srv = srv_.get();
  auto* sampler = sampler_.get();
  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ctx_->VSSetShader(vs_.get(), nullptr, 0);
  ctx_->PSSetConstantBuffers(0, 1, &cb);
  ctx_->PSSetShaderResources(0, 1, &srv);
  ctx_->PSSetSamplers(0, 1, &sampler);
  ctx_->RSSetState(nullptr);
  ctx_->OMSetDepthStencilState(nullptr, 0);
  ctx_->OMSetBlendState(blend_.get(), nullptr, 0xffffffff);
  const RECT& r = rect_;
  for (int plane = 0; plane < 2; plane++) {
    const float s = plane ? 0.5f : 1;
    D3D11_VIEWPORT vp{r.left * s, r.top * s, (r.right - r.left) * s, (r.bottom - r.top) * s, 0, 1};
    auto* rtv = plane ? chroma_rtv.get() : luma_rtv.get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->PSSetShader(plane ? chroma_.get() : luma_.get(), nullptr, 0);
    ctx_->Draw(4, 0);
  }
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}
