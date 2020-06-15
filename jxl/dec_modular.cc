// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "jxl/dec_modular.h"

#include <stdint.h>

#include <vector>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "jxl/dec_modular.cc"
#include <hwy/foreach_target.h>

#include "jxl/alpha.h"
#include "jxl/aux_out.h"
#include "jxl/base/compiler_specific.h"
#include "jxl/base/span.h"
#include "jxl/base/status.h"
#include "jxl/modular/encoding/encoding.h"
#include "jxl/modular/image/image.h"

// SIMD code
#include <hwy/before_namespace-inl.h>
namespace jxl {
#include <hwy/begin_target-inl.h>

void MultiplySum(const size_t xsize,
                 const pixel_type* const JXL_RESTRICT row_in,
                 const pixel_type* const JXL_RESTRICT row_in_Y,
                 const float factor, float* const JXL_RESTRICT row_out) {
  const HWY_FULL(float) df;
  const HWY_CAPPED(pixel_type, MaxLanes(df)) di;  // assumes pixel_type <= float
  const auto factor_v = Set(df, factor);
  for (size_t x = 0; x < xsize; x += Lanes(di)) {
    const auto in = Load(di, row_in + x) + Load(di, row_in_Y + x);
    const auto out = ConvertTo(df, in) * factor_v;
    Store(out, df, row_out + x);
  }
}

void RgbFromSingle(const size_t xsize,
                   const pixel_type* const JXL_RESTRICT row_in,
                   const float factor, Image3F* color, size_t /*c*/, size_t y) {
  const HWY_FULL(float) df;
  const HWY_CAPPED(pixel_type, MaxLanes(df)) di;  // assumes pixel_type <= float

  float* const JXL_RESTRICT row_out_r = color->PlaneRow(0, y);
  float* const JXL_RESTRICT row_out_g = color->PlaneRow(1, y);
  float* const JXL_RESTRICT row_out_b = color->PlaneRow(2, y);

  const auto factor_v = Set(df, factor);
  for (size_t x = 0; x < xsize; x += Lanes(di)) {
    const auto in = Load(di, row_in + x);
    const auto out = ConvertTo(df, in) * factor_v;
    Store(out, df, row_out_r + x);
    Store(out, df, row_out_g + x);
    Store(out, df, row_out_b + x);
  }
}

// Same signature as RgbFromSingle so we can assign to the same pointer.
void SingleFromSingle(const size_t xsize,
                      const pixel_type* const JXL_RESTRICT row_in,
                      const float factor, Image3F* color, size_t c, size_t y) {
  const HWY_FULL(float) df;
  const HWY_CAPPED(pixel_type, MaxLanes(df)) di;  // assumes pixel_type <= float

  float* const JXL_RESTRICT row_out = color->PlaneRow(c, y);

  const auto factor_v = Set(df, factor);
  for (size_t x = 0; x < xsize; x += Lanes(di)) {
    const auto in = Load(di, row_in + x);
    const auto out = ConvertTo(df, in) * factor_v;
    Store(out, df, row_out + x);
  }
}

#include <hwy/end_target-inl.h>
}  // namespace jxl
#include <hwy/after_namespace-inl.h>

#if HWY_ONCE
namespace jxl {
HWY_EXPORT(MultiplySum)       // Local function
HWY_EXPORT(RgbFromSingle)     // Local function
HWY_EXPORT(SingleFromSingle)  // Local function

// TODO: signal these multipliers (need larger ones for encoding Patch reference
// frames in kVarDCT)
static const float kDecoderMul2[3] = {1. / 32768., 1. / 2048., 1. / 2048.};

Status ModularFrameDecoder::DecodeGlobalInfo(BitReader* reader,
                                             const FrameHeader& frame_header,
                                             ImageBundle* decoded,
                                             bool decode_color, size_t xsize,
                                             size_t ysize) {
  int nb_chans = 3, depth_chan = 3;
  if (decoded->IsGray() &&
      frame_header.color_transform == ColorTransform::kNone) {
    nb_chans = 1;
    depth_chan = 1;
  }
  do_color = decode_color;
  if (!do_color) nb_chans = depth_chan = 0;
  if (frame_header.HasAlpha()) {
    nb_chans++;
    depth_chan++;
  }
  if (decoded->HasDepth()) nb_chans++;
  if (decoded->HasExtraChannels() && frame_header.IsDisplayed()) {
    nb_chans += decoded->extra_channels().size();
  }
  // TODO(lode): must handle decoded->metadata()->floating_point_sample?
  int maxval = (1 << decoded->metadata()->bits_per_sample) - 1;
  Image gi(xsize, ysize, maxval, nb_chans);
  if (decoded->HasDepth()) {
    gi.channel[depth_chan].resize(decoded->depth().xsize(),
                                  decoded->depth().ysize());
    gi.channel[depth_chan].hshift = decoded->metadata()->m2.depth_shift;
    gi.channel[depth_chan].vshift = decoded->metadata()->m2.depth_shift;
  }
  ModularOptions options;
  options.max_chan_size = kGroupDim;
  if (!ModularGenericDecompress(reader, gi, &options, -2))
    return JXL_FAILURE("Failed to decode global modular info");

  // ensure all the channel buffers are allocated
  have_something = false;
  for (int c = 0; c < gi.channel.size(); c++) {
    Channel& gic = gi.channel[c];
    if (c >= gi.nb_meta_channels && gic.w < kGroupDim && gic.h < kGroupDim)
      have_something = true;
    gic.resize();
  }
  full_image = std::move(gi);
  return true;
}

Status ModularFrameDecoder::DecodeGroup(const DecompressParams& dparams,
                                        const Rect& rect, BitReader* reader,
                                        AuxOut* aux_out, size_t minShift,
                                        size_t maxShift) {
  const size_t xsize = rect.xsize();
  const size_t ysize = rect.ysize();
  int maxval = full_image.maxval;
  Image gi(xsize, ysize, maxval, 0);
  // start at the first bigger-than-groupsize non-metachannel
  int c = full_image.nb_meta_channels;
  for (; c < full_image.channel.size(); c++) {
    Channel& fc = full_image.channel[c];
    if (fc.w > kGroupDim || fc.h > kGroupDim) break;
  }
  int beginc = c;
  for (; c < full_image.channel.size(); c++) {
    Channel& fc = full_image.channel[c];
    int shift = std::min(fc.hshift, fc.vshift);
    if (shift > maxShift) continue;
    if (shift < minShift) continue;
    Rect r(rect.x0() >> fc.hshift, rect.y0() >> fc.vshift,
           rect.xsize() >> fc.hshift, rect.ysize() >> fc.vshift, fc.w, fc.h);
    if (r.xsize() == 0 || r.ysize() == 0) continue;
    Channel gc(r.xsize(), r.ysize());
    gc.hshift = fc.hshift;
    gc.vshift = fc.vshift;
    gi.channel.emplace_back(std::move(gc));
  }
  gi.nb_channels = gi.channel.size();
  gi.real_nb_channels = gi.nb_channels;
  ModularOptions options;
  JXL_RETURN_IF_ERROR(reader->JumpToByteBoundary());
  if (!ModularGenericDecompress(reader, gi, &options))
    return JXL_FAILURE("Failed to decode modular group");
  JXL_RETURN_IF_ERROR(reader->JumpToByteBoundary());
  int gic = 0;
  for (c = beginc; c < full_image.channel.size(); c++) {
    Channel& fc = full_image.channel[c];
    int shift = std::min(fc.hshift, fc.vshift);
    if (shift > maxShift) continue;
    if (shift < minShift) continue;
    Rect r(rect.x0() >> fc.hshift, rect.y0() >> fc.vshift,
           rect.xsize() >> fc.hshift, rect.ysize() >> fc.vshift, fc.w, fc.h);
    if (r.xsize() == 0 || r.ysize() == 0) continue;
    for (size_t y = 0; y < r.ysize(); ++y) {
      pixel_type* const JXL_RESTRICT row_out = r.MutableRow(&fc.plane, y);
      const pixel_type* const JXL_RESTRICT row_in = gi.channel[gic].Row(y);
      for (size_t x = 0; x < r.xsize(); ++x) {
        row_out[x] = row_in[x];
      }
    }
    gic++;
  }
  return true;
}

Status ModularFrameDecoder::FinalizeDecoding(Image3F* color,
                                             ImageBundle* decoded,
                                             jxl::ThreadPool* pool,
                                             const FrameHeader& frame_header) {
  Image& gi = full_image;
  size_t xsize = gi.w;
  size_t ysize = gi.h;

  // Don't use threads if total image size is smaller than a group
  if (xsize * ysize < kGroupDim * kGroupDim) pool = nullptr;

  // Undo the global transforms
  gi.undo_transforms(-1, pool);

  int c = 0;
  if (do_color) {
    for (; c < 3; c++) {
      float factor = 255.f / (float)full_image.maxval;
      int c_in = c;
      if (frame_header.color_transform == ColorTransform::kXYB) {
        factor = 1.0f;
        // XYB is encoded as YX(B-Y)
        if (c < 2) c_in = 1 - c;
        factor *= kDecoderMul2[c];
      }
      if (frame_header.color_transform == ColorTransform::kXYB && c == 2) {
        RunOnPool(
            pool, 0, ysize, jxl::ThreadPool::SkipInit(),
            [&](const int task, const int thread) {
              const size_t y = task;
              const pixel_type* const JXL_RESTRICT row_in =
                  gi.channel[c_in].Row(y);
              const pixel_type* const JXL_RESTRICT row_in_Y =
                  gi.channel[0].Row(y);
              float* const JXL_RESTRICT row_out = color->PlaneRow(c, y);
              HWY_DYNAMIC_DISPATCH(MultiplySum)(xsize, row_in, row_in_Y, factor,
                                                row_out);
            },
            "ModularIntToFloat");
      } else {
        const bool rgb_from_gray =
            decoded->IsGray() &&
            frame_header.color_transform == ColorTransform::kNone;
        RunOnPool(
            pool, 0, ysize, jxl::ThreadPool::SkipInit(),
            [&](const int task, const int thread) {
              const size_t y = task;
              const pixel_type* const JXL_RESTRICT row_in =
                  gi.channel[decoded->IsGray() ? 0 : c_in].Row(y);
              if (rgb_from_gray) {
                HWY_DYNAMIC_DISPATCH(RgbFromSingle)(xsize, row_in, factor,
                                                    color, c, y);
              } else {
                HWY_DYNAMIC_DISPATCH(SingleFromSingle)(xsize, row_in, factor,
                                                       color, c, y);
              }
            },
            "ModularIntToFloat");
      }
      if (decoded->IsGray() &&
          frame_header.color_transform == ColorTransform::kNone) {
        break;
      }
    }
    if (decoded->IsGray() &&
        frame_header.color_transform == ColorTransform::kNone) {
      c = 1;
    }
  }
  if (frame_header.HasAlpha()) {
    pixel_type max_alpha = MaxAlpha(decoded->metadata()->alpha_bits);
    for (size_t y = 0; y < ysize; ++y) {
      uint16_t* const JXL_RESTRICT row_out = decoded->alpha().MutableRow(y);
      const pixel_type* const JXL_RESTRICT row_in = gi.channel[c].Row(y);
      for (size_t x = 0; x < xsize; ++x) {
        row_out[x] = Clamp(row_in[x], 0, max_alpha);
      }
    }
    c++;
  }
  if (decoded->HasDepth()) {
    pixel_type max_depth = (1 << decoded->metadata()->m2.depth_bits) - 1;
    for (size_t y = 0; y < decoded->depth().ysize(); ++y) {
      uint16_t* const JXL_RESTRICT row_out = decoded->depth().MutableRow(y);
      const pixel_type* const JXL_RESTRICT row_in = gi.channel[c].Row(y);
      for (size_t x = 0; x < decoded->depth().xsize(); ++x) {
        row_out[x] = Clamp(row_in[x], 0, max_depth);
      }
    }
    c++;
  }
  if (decoded->HasExtraChannels() && frame_header.IsDisplayed()) {
    pixel_type max_extra =
        (1 << decoded->metadata()->m2.extra_channel_bits) - 1;
    for (size_t ec = 0; ec < decoded->extra_channels().size(); ec++, c++) {
      for (size_t y = 0; y < ysize; ++y) {
        uint16_t* const JXL_RESTRICT row_out =
            decoded->extra_channels()[ec].MutableRow(y);
        const pixel_type* const JXL_RESTRICT row_in = gi.channel[c].Row(y);
        for (size_t x = 0; x < xsize; ++x) {
          row_out[x] = Clamp(row_in[x], 0, max_extra);
        }
      }
    }
  }
  return true;
}

}  // namespace jxl
#endif  // HWY_ONCE
