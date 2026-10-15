#pragma once
#include "gpu.h"

#include <d2d1_1.h>

// The "Clipped with Clip" badge, blended into NV12 frames on the GPU in the bottom-right corner. Used for
// exports and shares; recordings stay clean. Animated by clip time: the record dot pulses and a sheen sweeps.
class Watermark {
 public:
  Watermark(ID3D11Device* dev, ID3D11DeviceContext* ctx, int frame_w, int frame_h);
  void Draw(ID3D11Texture2D* nv12, UINT slice, double seconds);  // throws on device trouble

 private:
  com_ptr<ID3D11Device> dev_;
  com_ptr<ID3D11DeviceContext> ctx_;
  RECT rect_{};  // in the frame, even coordinates for the half-size chroma plane
  float dot_[4] = {};
  com_ptr<ID3D11ShaderResourceView> srv_;
  com_ptr<ID3D11VertexShader> vs_;
  com_ptr<ID3D11PixelShader> luma_, chroma_;
  com_ptr<ID3D11BlendState> blend_;
  com_ptr<ID3D11SamplerState> sampler_;
  com_ptr<ID3D11Buffer> cb_;
  std::map<std::pair<ID3D11Texture2D*, UINT>, std::pair<com_ptr<ID3D11RenderTargetView>, com_ptr<ID3D11RenderTargetView>>>
      targets_;
};
