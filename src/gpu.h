#pragma once
// GPU pieces shared by recording and export: the device, FFmpeg's view of it, NV12 encoder surfaces,
// the hardware H.264 encoder, and video processor views.
#include <unknwn.h>  // before C++/WinRT, so com_ptr works with classic COM

#include <d3d11_4.h>
#include <winrt/base.h>

#include <map>
#include <string>

#include "common.h"
#include "ring.h"

using winrt::com_ptr;

struct VideoDevice {
  com_ptr<ID3D11Device> dev;
  com_ptr<ID3D11DeviceContext> ctx;
};
// The high-performance GPU (where games render, with the good encoder), safe to use from several threads.
VideoDevice CreateVideoDevice(int gpu_priority);

AVBufferRef* FfmpegDevice(ID3D11Device* dev);
// NV12 render targets the video processor writes and the encoder reads.
AVBufferRef* EncoderSurfaces(AVBufferRef* hw_dev, int w, int h, int pool);
// The first hardware H.264 encoder that opens (NVENC, AMF, QSV); `name` says which.
AVCodecContext* OpenH264(AVBufferRef* frames, int w, int h, AVRational fps, int64_t bitrate, int gop, std::string& name);

// Encoder surfaces are slices of one texture array, or separate textures when the driver refuses the array.
using OutputViews = std::map<std::pair<ID3D11Texture2D*, UINT>, com_ptr<ID3D11VideoProcessorOutputView>>;
ID3D11VideoProcessorOutputView* OutputViewFor(OutputViews& cache, ID3D11VideoDevice* vdev,
                                              ID3D11VideoProcessorEnumerator* venum, ID3D11Texture2D* tex, UINT slice);
