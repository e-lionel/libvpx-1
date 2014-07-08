/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <string.h>
#include "test/acm_random.h"
#include "test/register_state_check.h"
#include "test/util.h"
#include "third_party/googletest/src/include/gtest/gtest.h"

#include "./vpx_config.h"
#include "./vp9_rtcd.h"
#include "vp9/common/vp9_filter.h"
#include "vpx_mem/vpx_mem.h"
#include "vpx_ports/mem.h"

namespace {
typedef void (*convolve_fn_t)(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h);

struct ConvolveFunctions {
  ConvolveFunctions(convolve_fn_t h8, convolve_fn_t h8_avg,
                    convolve_fn_t v8, convolve_fn_t v8_avg,
                    convolve_fn_t hv8, convolve_fn_t hv8_avg, int bps)
      : h8_(h8), v8_(v8), hv8_(hv8), h8_avg_(h8_avg), v8_avg_(v8_avg),
        hv8_avg_(hv8_avg), use_high_bps_(bps) {}

  convolve_fn_t h8_;
  convolve_fn_t v8_;
  convolve_fn_t hv8_;
  convolve_fn_t h8_avg_;
  convolve_fn_t v8_avg_;
  convolve_fn_t hv8_avg_;
  int use_high_bps_;
};

typedef std::tr1::tuple<int, int, const ConvolveFunctions*> convolve_param_t;

// Reference 8-tap subpixel filter, slightly modified to fit into this test.
#define VP9_FILTER_WEIGHT 128
#define VP9_FILTER_SHIFT 7
uint8_t clip_pixel(int x) {
  return x < 0 ? 0 :
         x > 255 ? 255 :
         x;
}

void filter_block2d_8_c(const uint8_t *src_ptr,
                        const unsigned int src_stride,
                        const int16_t *HFilter,
                        const int16_t *VFilter,
                        uint8_t *dst_ptr,
                        unsigned int dst_stride,
                        unsigned int output_width,
                        unsigned int output_height) {
  // Between passes, we use an intermediate buffer whose height is extended to
  // have enough horizontally filtered values as input for the vertical pass.
  // This buffer is allocated to be big enough for the largest block type we
  // support.
  const int kInterp_Extend = 4;
  const unsigned int intermediate_height =
      (kInterp_Extend - 1) + output_height + kInterp_Extend;

  /* Size of intermediate_buffer is max_intermediate_height * filter_max_width,
   * where max_intermediate_height = (kInterp_Extend - 1) + filter_max_height
   *                                 + kInterp_Extend
   *                               = 3 + 16 + 4
   *                               = 23
   * and filter_max_width = 16
   */
  uint8_t intermediate_buffer[71 * 64];
  const int intermediate_next_stride = 1 - intermediate_height * output_width;

  // Horizontal pass (src -> transposed intermediate).
  {
    uint8_t *output_ptr = intermediate_buffer;
    const int src_next_row_stride = src_stride - output_width;
    unsigned int i, j;
    src_ptr -= (kInterp_Extend - 1) * src_stride + (kInterp_Extend - 1);
    for (i = 0; i < intermediate_height; ++i) {
      for (j = 0; j < output_width; ++j) {
        // Apply filter...
        const int temp = (src_ptr[0] * HFilter[0]) +
                         (src_ptr[1] * HFilter[1]) +
                         (src_ptr[2] * HFilter[2]) +
                         (src_ptr[3] * HFilter[3]) +
                         (src_ptr[4] * HFilter[4]) +
                         (src_ptr[5] * HFilter[5]) +
                         (src_ptr[6] * HFilter[6]) +
                         (src_ptr[7] * HFilter[7]) +
                         (VP9_FILTER_WEIGHT >> 1);  // Rounding

        // Normalize back to 0-255...
        *output_ptr = clip_pixel(temp >> VP9_FILTER_SHIFT);
        ++src_ptr;
        output_ptr += intermediate_height;
      }
      src_ptr += src_next_row_stride;
      output_ptr += intermediate_next_stride;
    }
  }

  // Vertical pass (transposed intermediate -> dst).
  {
    uint8_t *src_ptr = intermediate_buffer;
    const int dst_next_row_stride = dst_stride - output_width;
    unsigned int i, j;
    for (i = 0; i < output_height; ++i) {
      for (j = 0; j < output_width; ++j) {
        // Apply filter...
        const int temp = (src_ptr[0] * VFilter[0]) +
                         (src_ptr[1] * VFilter[1]) +
                         (src_ptr[2] * VFilter[2]) +
                         (src_ptr[3] * VFilter[3]) +
                         (src_ptr[4] * VFilter[4]) +
                         (src_ptr[5] * VFilter[5]) +
                         (src_ptr[6] * VFilter[6]) +
                         (src_ptr[7] * VFilter[7]) +
                         (VP9_FILTER_WEIGHT >> 1);  // Rounding

        // Normalize back to 0-255...
        *dst_ptr++ = clip_pixel(temp >> VP9_FILTER_SHIFT);
        src_ptr += intermediate_height;
      }
      src_ptr += intermediate_next_stride;
      dst_ptr += dst_next_row_stride;
    }
  }
}

void block2d_average_c(uint8_t *src,
                       unsigned int src_stride,
                       uint8_t *output_ptr,
                       unsigned int output_stride,
                       unsigned int output_width,
                       unsigned int output_height) {
  unsigned int i, j;
  for (i = 0; i < output_height; ++i) {
    for (j = 0; j < output_width; ++j) {
      output_ptr[j] = (output_ptr[j] + src[i * src_stride + j] + 1) >> 1;
    }
    output_ptr += output_stride;
  }
}

void filter_average_block2d_8_c(const uint8_t *src_ptr,
                                const unsigned int src_stride,
                                const int16_t *HFilter,
                                const int16_t *VFilter,
                                uint8_t *dst_ptr,
                                unsigned int dst_stride,
                                unsigned int output_width,
                                unsigned int output_height) {
  uint8_t tmp[64 * 64];

  assert(output_width <= 64);
  assert(output_height <= 64);
  filter_block2d_8_c(src_ptr, src_stride, HFilter, VFilter, tmp, 64,
                     output_width, output_height);
  block2d_average_c(tmp, 64, dst_ptr, dst_stride,
                    output_width, output_height);
}

void high_filter_block2d_8_c(const uint16_t *src_ptr,
                        const unsigned int src_stride,
                        const int16_t *HFilter,
                        const int16_t *VFilter,
                        uint16_t *dst_ptr,
                        unsigned int dst_stride,
                        unsigned int output_width,
                        unsigned int output_height,
                        int bps) {
  // Between passes, we use an intermediate buffer whose height is extended to
  // have enough horizontally filtered values as input for the vertical pass.
  // This buffer is allocated to be big enough for the largest block type we
  // support.
  const int kInterp_Extend = 4;
  const unsigned int intermediate_height =
      (kInterp_Extend - 1) + output_height + kInterp_Extend;

  /* Size of intermediate_buffer is max_intermediate_height * filter_max_width,
   * where max_intermediate_height = (kInterp_Extend - 1) + filter_max_height
   *                                 + kInterp_Extend
   *                               = 3 + 16 + 4
   *                               = 23
   * and filter_max_width = 16
   */
  uint16_t intermediate_buffer[71 * 64];
  const int intermediate_next_stride = 1 - intermediate_height * output_width;

  // Horizontal pass (src -> transposed intermediate).
  {
    uint16_t *output_ptr = intermediate_buffer;
    const int src_next_row_stride = src_stride - output_width;
    unsigned int i, j;
    src_ptr -= (kInterp_Extend - 1) * src_stride + (kInterp_Extend - 1);
    for (i = 0; i < intermediate_height; ++i) {
      for (j = 0; j < output_width; ++j) {
        // Apply filter...
        const int temp = (src_ptr[0] * HFilter[0]) +
                         (src_ptr[1] * HFilter[1]) +
                         (src_ptr[2] * HFilter[2]) +
                         (src_ptr[3] * HFilter[3]) +
                         (src_ptr[4] * HFilter[4]) +
                         (src_ptr[5] * HFilter[5]) +
                         (src_ptr[6] * HFilter[6]) +
                         (src_ptr[7] * HFilter[7]) +
                         (VP9_FILTER_WEIGHT >> 1);  // Rounding

        // Normalize back to 0-255...
        *output_ptr = clip_pixel_bps(temp >> VP9_FILTER_SHIFT, bps);
        ++src_ptr;
        output_ptr += intermediate_height;
      }
      src_ptr += src_next_row_stride;
      output_ptr += intermediate_next_stride;
    }
  }

  // Vertical pass (transposed intermediate -> dst).
  {
    uint16_t *src_ptr = intermediate_buffer;
    const int dst_next_row_stride = dst_stride - output_width;
    unsigned int i, j;
    for (i = 0; i < output_height; ++i) {
      for (j = 0; j < output_width; ++j) {
        // Apply filter...
        const int temp = (src_ptr[0] * VFilter[0]) +
                         (src_ptr[1] * VFilter[1]) +
                         (src_ptr[2] * VFilter[2]) +
                         (src_ptr[3] * VFilter[3]) +
                         (src_ptr[4] * VFilter[4]) +
                         (src_ptr[5] * VFilter[5]) +
                         (src_ptr[6] * VFilter[6]) +
                         (src_ptr[7] * VFilter[7]) +
                         (VP9_FILTER_WEIGHT >> 1);  // Rounding

        // Normalize back to 0-255...
        *dst_ptr++ = clip_pixel_bps(temp >> VP9_FILTER_SHIFT, bps);
        src_ptr += intermediate_height;
      }
      src_ptr += intermediate_next_stride;
      dst_ptr += dst_next_row_stride;
    }
  }
}

void high_block2d_average_c(uint16_t *src,
                       unsigned int src_stride,
                       uint16_t *output_ptr,
                       unsigned int output_stride,
                       unsigned int output_width,
                       unsigned int output_height,
                       int bps) {
  unsigned int i, j;
  for (i = 0; i < output_height; ++i) {
    for (j = 0; j < output_width; ++j) {
      output_ptr[j] = (output_ptr[j] + src[i * src_stride + j] + 1) >> 1;
    }
    output_ptr += output_stride;
  }
}

void high_filter_average_block2d_8_c(const uint16_t *src_ptr,
                                const unsigned int src_stride,
                                const int16_t *HFilter,
                                const int16_t *VFilter,
                                uint16_t *dst_ptr,
                                unsigned int dst_stride,
                                unsigned int output_width,
                                unsigned int output_height,
                                int bps) {
  uint16_t tmp[64 * 64];

  assert(output_width <= 64);
  assert(output_height <= 64);
  high_filter_block2d_8_c(src_ptr, src_stride, HFilter, VFilter, tmp, 64,
                     output_width, output_height, bps);
  high_block2d_average_c(tmp, 64, dst_ptr, dst_stride,
                    output_width, output_height, bps);
}

class ConvolveTest : public ::testing::TestWithParam<convolve_param_t> {
 public:
  static void SetUpTestCase() {
    // Force input_ to be unaligned, output to be 16 byte aligned.
    input_ = reinterpret_cast<uint8_t*>(
        vpx_memalign(kDataAlignment, kInputBufferSize + 1)) + 1;
    output_ = reinterpret_cast<uint8_t*>(
        vpx_memalign(kDataAlignment, kOutputBufferSize));
    input16_ = reinterpret_cast<uint16_t*>(vpx_memalign(kDataAlignment,
      (kInputBufferSize + 1)*sizeof(uint16_t))) + 1;
    output16_ = reinterpret_cast<uint16_t*>(vpx_memalign(kDataAlignment,
      (kOutputBufferSize)*sizeof(uint16_t)));
  }

  static void TearDownTestCase() {
    vpx_free(input_ - 1);
    input_ = NULL;
    vpx_free(output_);
    output_ = NULL;
    vpx_free(input16_ - 1);
    input16_ = NULL;
    vpx_free(output16_);
    output16_ = NULL;
  }

 protected:
  static const int kDataAlignment = 16;
  static const int kOuterBlockSize = 256;
  static const int kInputStride = kOuterBlockSize;
  static const int kOutputStride = kOuterBlockSize;
  static const int kMaxDimension = 64;
  static const int kInputBufferSize = kOuterBlockSize * kOuterBlockSize;
  static const int kOutputBufferSize = kOuterBlockSize * kOuterBlockSize;

  int Width() const { return GET_PARAM(0); }
  int Height() const { return GET_PARAM(1); }
  int BorderLeft() const {
    const int center = (kOuterBlockSize - Width()) / 2;
    return (center + (kDataAlignment - 1)) & ~(kDataAlignment - 1);
  }
  int BorderTop() const { return (kOuterBlockSize - Height()) / 2; }

  bool IsIndexInBorder(int i) {
    return (i < BorderTop() * kOuterBlockSize ||
            i >= (BorderTop() + Height()) * kOuterBlockSize ||
            i % kOuterBlockSize < BorderLeft() ||
            i % kOuterBlockSize >= (BorderLeft() + Width()));
  }

  virtual void SetUp() {
    UUT_ = GET_PARAM(2);
    mask_ = (1 << UUT_->use_high_bps_)-1;
    /* Set up guard blocks for an inner block centered in the outer block */
    for (int i = 0; i < kOutputBufferSize; ++i) {
      if (IsIndexInBorder(i))
        output_[i] = 255;
      else
        output_[i] = 0;
    }

    ::libvpx_test::ACMRandom prng;
    for (int i = 0; i < kInputBufferSize; ++i) {
      if (i & 1) {
        input_[i] = 255;
        input16_[i] = mask_;
      } else {
        input_[i] = prng.Rand8Extremes();
        input16_[i] = prng.Rand16() & mask_;
      }
    }
  }

  void SetConstantInput(int value) {
    memset(input_, value, kInputBufferSize);
    vpx_memset16(input16_, value, kInputBufferSize);
  }

  void CheckGuardBlocks() {
    for (int i = 0; i < kOutputBufferSize; ++i) {
      if (IsIndexInBorder(i))
        EXPECT_EQ(255, output_[i]);
    }
  }

  uint8_t* input() const {
#if CONFIG_VP9_HIGH
    if (UUT_->use_high_bps_ == -1) {
      return input_ + BorderTop() * kOuterBlockSize + BorderLeft();
    } else {
      return CONVERT_TO_BYTEPTR(input16_ + BorderTop() * kOuterBlockSize
                                + BorderLeft());
    }
#else
      return input_ + BorderTop() * kOuterBlockSize + BorderLeft();
#endif
  }

  uint8_t* output() const {
#if CONFIG_VP9_HIGH
    if (UUT_->use_high_bps_ == -1) {
      return output_ + BorderTop() * kOuterBlockSize + BorderLeft();
    } else {
      return CONVERT_TO_BYTEPTR(output16_ + BorderTop() * kOuterBlockSize
                                + BorderLeft());
    }
#else
      return output_ + BorderTop() * kOuterBlockSize + BorderLeft();
#endif
  }

  uint16_t lookup(uint8_t* list, int index) const {
#if CONFIG_VP9_HIGH
    if (UUT_->use_high_bps_ == -1) {
      return list[index];
    } else {
      return CONVERT_TO_SHORTPTR(list)[index];
    }
#else
    return list[index];
#endif
  }
  void assign_val(uint8_t* list, int index, uint16_t val) const {
#if CONFIG_VP9_HIGH
    if (UUT_->use_high_bps_ == -1) {
      list[index] = (uint8_t) val;
    } else {
      CONVERT_TO_SHORTPTR(list)[index] = val;
    }
#else
    list[index] = (uint8_t) val;
#endif
  }

void wrapper_filter_average_block2d_8_c(const uint8_t *src_ptr,
                                const unsigned int src_stride,
                                const int16_t *HFilter,
                                const int16_t *VFilter,
                                uint8_t *dst_ptr,
                                unsigned int dst_stride,
                                unsigned int output_width,
                                unsigned int output_height) {
#if CONFIG_VP9_HIGH
    if (UUT_->use_high_bps_ == -1) {
      filter_average_block2d_8_c(src_ptr, src_stride, HFilter, VFilter,
                                 dst_ptr, dst_stride, output_width,
                                 output_height);
    } else {
      high_filter_average_block2d_8_c(CONVERT_TO_SHORTPTR(src_ptr), src_stride,
                                      HFilter, VFilter,
                                      CONVERT_TO_SHORTPTR(dst_ptr), dst_stride,
                                      output_width, output_height,
                                      UUT_->use_high_bps_);
    }
#else
    filter_average_block2d_8_c(src_ptr, src_stride, HFilter, VFilter,
                               dst_ptr, dst_stride, output_width,
                               output_height);
#endif
}

void wrapper_filter_block2d_8_c(const uint8_t *src_ptr,
                                const unsigned int src_stride,
                                const int16_t *HFilter,
                                const int16_t *VFilter,
                                uint8_t *dst_ptr,
                                unsigned int dst_stride,
                                unsigned int output_width,
                                unsigned int output_height) {
#if CONFIG_VP9_HIGH
    if (UUT_->use_high_bps_ == -1) {
      filter_block2d_8_c(src_ptr, src_stride, HFilter, VFilter,
                         dst_ptr, dst_stride, output_width, output_height);
    } else {
      high_filter_block2d_8_c(CONVERT_TO_SHORTPTR(src_ptr), src_stride,
                              HFilter, VFilter,
                              CONVERT_TO_SHORTPTR(dst_ptr), dst_stride,
                              output_width, output_height, UUT_->use_high_bps_);
    }
#else
    filter_block2d_8_c(src_ptr, src_stride, HFilter, VFilter,
                       dst_ptr, dst_stride, output_width, output_height);
#endif
}

  const ConvolveFunctions* UUT_;
  static uint8_t* input_;
  static uint8_t* output_;
  static uint16_t* input16_;
  static uint16_t* output16_;
  int mask_;
};
uint8_t* ConvolveTest::input_ = NULL;
uint8_t* ConvolveTest::output_ = NULL;
uint16_t* ConvolveTest::input16_ = NULL;
uint16_t* ConvolveTest::output16_ = NULL;

TEST_P(ConvolveTest, GuardBlocks) {
  CheckGuardBlocks();
}

TEST_P(ConvolveTest, CopyHoriz) {
  uint8_t* const in = input();
  uint8_t* const out = output();

  DECLARE_ALIGNED(256, const int16_t, filter8[8]) = {0, 0, 0, 128, 0, 0, 0, 0};
  REGISTER_STATE_CHECK(
      UUT_->h8_(in, kInputStride, out, kOutputStride, filter8, 16, filter8, 16,
                Width(), Height()));

  CheckGuardBlocks();

  for (int y = 0; y < Height(); ++y)
    for (int x = 0; x < Width(); ++x)
      ASSERT_EQ(lookup(out, y * kOutputStride + x),
                lookup(in, y * kInputStride + x))
          << "(" << x << "," << y << ")";
}

TEST_P(ConvolveTest, CopyVert) {
  uint8_t* const in = input();
  uint8_t* const out = output();
  DECLARE_ALIGNED(256, const int16_t, filter8[8]) = {0, 0, 0, 128, 0, 0, 0, 0};

  REGISTER_STATE_CHECK(
      UUT_->v8_(in, kInputStride, out, kOutputStride, filter8, 16, filter8, 16,
                Width(), Height()));

  CheckGuardBlocks();

  for (int y = 0; y < Height(); ++y)
    for (int x = 0; x < Width(); ++x)
      ASSERT_EQ(lookup(out, y * kOutputStride + x),
                lookup(in, y * kInputStride + x))
          << "(" << x << "," << y << ")";
}

TEST_P(ConvolveTest, Copy2D) {
  uint8_t* const in = input();
  uint8_t* const out = output();
  DECLARE_ALIGNED(256, const int16_t, filter8[8]) = {0, 0, 0, 128, 0, 0, 0, 0};

  REGISTER_STATE_CHECK(
      UUT_->hv8_(in, kInputStride, out, kOutputStride, filter8, 16, filter8, 16,
                 Width(), Height()));

  CheckGuardBlocks();

  for (int y = 0; y < Height(); ++y)
    for (int x = 0; x < Width(); ++x)
      ASSERT_EQ(lookup(out, y * kOutputStride + x),
                lookup(in, y * kInputStride + x))
          << "(" << x << "," << y << ")";
}

const int kNumFilterBanks = 4;
const int kNumFilters = 16;

TEST(ConvolveTest, FiltersWontSaturateWhenAddedPairwise) {
  for (int filter_bank = 0; filter_bank < kNumFilterBanks; ++filter_bank) {
    const InterpKernel *filters =
        vp9_get_interp_kernel(static_cast<INTERP_FILTER>(filter_bank));
    for (int i = 0; i < kNumFilters; i++) {
      const int p0 = filters[i][0] + filters[i][1];
      const int p1 = filters[i][2] + filters[i][3];
      const int p2 = filters[i][4] + filters[i][5];
      const int p3 = filters[i][6] + filters[i][7];
      EXPECT_LE(p0, 128);
      EXPECT_LE(p1, 128);
      EXPECT_LE(p2, 128);
      EXPECT_LE(p3, 128);
      EXPECT_LE(p0 + p3, 128);
      EXPECT_LE(p0 + p3 + p1, 128);
      EXPECT_LE(p0 + p3 + p1 + p2, 128);
      EXPECT_EQ(p0 + p1 + p2 + p3, 128);
    }
  }
}

const int16_t kInvalidFilter[8] = { 0 };

TEST_P(ConvolveTest, MatchesReferenceSubpixelFilter) {
  uint8_t* const in = input();
  uint8_t* const out = output();
#if CONFIG_VP9_HIGH
  uint8_t ref8[kOutputStride * kMaxDimension];
  uint16_t ref16[kOutputStride * kMaxDimension];
  uint8_t* ref;
  if (UUT_->use_high_bps_ == -1) {
    ref = ref8;
  } else {
    ref = CONVERT_TO_BYTEPTR(ref16);
  }
#else
  uint8_t ref[kOutputStride * kMaxDimension];
#endif


  for (int filter_bank = 0; filter_bank < kNumFilterBanks; ++filter_bank) {
    const InterpKernel *filters =
        vp9_get_interp_kernel(static_cast<INTERP_FILTER>(filter_bank));
    const InterpKernel *const eighttap_smooth =
        vp9_get_interp_kernel(EIGHTTAP_SMOOTH);

    for (int filter_x = 0; filter_x < kNumFilters; ++filter_x) {
      for (int filter_y = 0; filter_y < kNumFilters; ++filter_y) {
        wrapper_filter_block2d_8_c(in, kInputStride,
                           filters[filter_x], filters[filter_y],
                           ref, kOutputStride,
                           Width(), Height());

        if (filters == eighttap_smooth || (filter_x && filter_y))
          REGISTER_STATE_CHECK(
              UUT_->hv8_(in, kInputStride, out, kOutputStride,
                         filters[filter_x], 16, filters[filter_y], 16,
                         Width(), Height()));
        else if (filter_y)
          REGISTER_STATE_CHECK(
              UUT_->v8_(in, kInputStride, out, kOutputStride,
                        kInvalidFilter, 16, filters[filter_y], 16,
                        Width(), Height()));
        else
          REGISTER_STATE_CHECK(
              UUT_->h8_(in, kInputStride, out, kOutputStride,
                        filters[filter_x], 16, kInvalidFilter, 16,
                        Width(), Height()));

        CheckGuardBlocks();

        for (int y = 0; y < Height(); ++y)
          for (int x = 0; x < Width(); ++x)
            ASSERT_EQ(lookup(ref, y * kOutputStride + x),
                      lookup(out, y * kOutputStride + x))
                << "mismatch at (" << x << "," << y << "), "
                << "filters (" << filter_bank << ","
                << filter_x << "," << filter_y << ")";
      }
    }
  }
}

TEST_P(ConvolveTest, MatchesReferenceAveragingSubpixelFilter) {
  uint8_t* const in = input();
  uint8_t* const out = output();
#if CONFIG_VP9_HIGH
  uint8_t ref8[kOutputStride * kMaxDimension];
  uint16_t ref16[kOutputStride * kMaxDimension];
  uint8_t* ref;
  if (UUT_->use_high_bps_ == -1) {
    ref = ref8;
  } else {
    ref = CONVERT_TO_BYTEPTR(ref16);
  }
#else
  uint8_t ref[kOutputStride * kMaxDimension];
#endif

  // Populate ref and out with some random data
  ::libvpx_test::ACMRandom prng;
  for (int y = 0; y < Height(); ++y) {
    for (int x = 0; x < Width(); ++x) {
      uint16_t r;
      if (UUT_->use_high_bps_ == -1 || UUT_->use_high_bps_ == 8) {
        r = prng.Rand8Extremes();
      } else {
        r = prng.Rand16() & mask_;
      }

      assign_val(out, y * kOutputStride + x, r);
      assign_val(ref, y * kOutputStride + x, r);
    }
  }

  for (int filter_bank = 0; filter_bank < kNumFilterBanks; ++filter_bank) {
    const InterpKernel *filters =
        vp9_get_interp_kernel(static_cast<INTERP_FILTER>(filter_bank));
    const InterpKernel *const eighttap_smooth =
        vp9_get_interp_kernel(EIGHTTAP_SMOOTH);

    for (int filter_x = 0; filter_x < kNumFilters; ++filter_x) {
      for (int filter_y = 0; filter_y < kNumFilters; ++filter_y) {
        wrapper_filter_average_block2d_8_c(in, kInputStride,
                                   filters[filter_x], filters[filter_y],
                                   ref, kOutputStride,
                                   Width(), Height());

        if (filters == eighttap_smooth || (filter_x && filter_y))
          REGISTER_STATE_CHECK(
              UUT_->hv8_avg_(in, kInputStride, out, kOutputStride,
                             filters[filter_x], 16, filters[filter_y], 16,
                             Width(), Height()));
        else if (filter_y)
          REGISTER_STATE_CHECK(
              UUT_->v8_avg_(in, kInputStride, out, kOutputStride,
                            filters[filter_x], 16, filters[filter_y], 16,
                            Width(), Height()));
        else
          REGISTER_STATE_CHECK(
              UUT_->h8_avg_(in, kInputStride, out, kOutputStride,
                            filters[filter_x], 16, filters[filter_y], 16,
                            Width(), Height()));

        CheckGuardBlocks();

        for (int y = 0; y < Height(); ++y)
          for (int x = 0; x < Width(); ++x)
            ASSERT_EQ(lookup(ref, y * kOutputStride + x),
                      lookup(out, y * kOutputStride + x))
                << "mismatch at (" << x << "," << y << "), "
                << "filters (" << filter_bank << ","
                << filter_x << "," << filter_y << ")";
      }
    }
  }
}

TEST_P(ConvolveTest, FilterExtremes) {
  uint8_t* const in = input();
  uint8_t* const out = output();
#if CONFIG_VP9_HIGH
  uint8_t ref8[kOutputStride * kMaxDimension];
  uint16_t ref16[kOutputStride * kMaxDimension];
  uint8_t* ref;
  if (UUT_->use_high_bps_ == -1) {
    ref = ref8;
  } else {
    ref = CONVERT_TO_BYTEPTR(ref16);
  }
#else
  uint8_t ref[kOutputStride * kMaxDimension];
#endif

  // Populate ref and out with some random data
  ::libvpx_test::ACMRandom prng;
  for (int y = 0; y < Height(); ++y) {
    for (int x = 0; x < Width(); ++x) {
      uint16_t r;
      if (UUT_->use_high_bps_ == -1 || UUT_->use_high_bps_ == 8) {
        r = prng.Rand8Extremes();
      } else {
        r = prng.Rand16() & mask_;
      }

      assign_val(out, y * kOutputStride + x, r);
      assign_val(ref, y * kOutputStride + x, r);
    }
  }
  for (int axis = 0; axis < 2; axis++) {
    int seed_val = 0;
    while (seed_val < 256) {
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            assign_val(in, y * kOutputStride + x - SUBPEL_TAPS / 2 + 1,
                       ((seed_val >> (axis ? y : x)) & 1) * mask_);
          if (axis) seed_val++;
        }
        if (axis)
          seed_val-= 8;
        else
          seed_val++;
      }
      if (axis) seed_val += 8;

      for (int filter_bank = 0; filter_bank < kNumFilterBanks; ++filter_bank) {
        const InterpKernel *filters =
            vp9_get_interp_kernel(static_cast<INTERP_FILTER>(filter_bank));
        const InterpKernel *const eighttap_smooth =
            vp9_get_interp_kernel(EIGHTTAP_SMOOTH);
        for (int filter_x = 0; filter_x < kNumFilters; ++filter_x) {
          for (int filter_y = 0; filter_y < kNumFilters; ++filter_y) {
            wrapper_filter_block2d_8_c(in, kInputStride,
                                   filters[filter_x], filters[filter_y],
                                   ref, kOutputStride,
                                   Width(), Height());
            if (filters == eighttap_smooth || (filter_x && filter_y))
              REGISTER_STATE_CHECK(
                  UUT_->hv8_(in, kInputStride, out, kOutputStride,
                             filters[filter_x], 16, filters[filter_y], 16,
                             Width(), Height()));
            else if (filter_y)
              REGISTER_STATE_CHECK(
                  UUT_->v8_(in, kInputStride, out, kOutputStride,
                            kInvalidFilter, 16, filters[filter_y], 16,
                            Width(), Height()));
            else
              REGISTER_STATE_CHECK(
                  UUT_->h8_(in, kInputStride, out, kOutputStride,
                            filters[filter_x], 16, kInvalidFilter, 16,
                            Width(), Height()));

            for (int y = 0; y < Height(); ++y)
              for (int x = 0; x < Width(); ++x)
                ASSERT_EQ(lookup(ref, y * kOutputStride + x),
                          lookup(out, y * kOutputStride + x))
                    << "mismatch at (" << x << "," << y << "), "
                    << "filters (" << filter_bank << ","
                    << filter_x << "," << filter_y << ")";
          }
        }
      }
    }
  }
}

DECLARE_ALIGNED(256, const int16_t, kChangeFilters[16][8]) = {
    { 0,   0,   0,   0,   0,   0,   0, 128},
    { 0,   0,   0,   0,   0,   0, 128},
    { 0,   0,   0,   0,   0, 128},
    { 0,   0,   0,   0, 128},
    { 0,   0,   0, 128},
    { 0,   0, 128},
    { 0, 128},
    { 128},
    { 0,   0,   0,   0,   0,   0,   0, 128},
    { 0,   0,   0,   0,   0,   0, 128},
    { 0,   0,   0,   0,   0, 128},
    { 0,   0,   0,   0, 128},
    { 0,   0,   0, 128},
    { 0,   0, 128},
    { 0, 128},
    { 128}
};

/* This test exercises the horizontal and vertical filter functions. */
TEST_P(ConvolveTest, ChangeFilterWorks) {
  uint8_t* const in = input();
  uint8_t* const out = output();

  /* Assume that the first input sample is at the 8/16th position. */
  const int kInitialSubPelOffset = 8;

  /* Filters are 8-tap, so the first filter tap will be applied to the pixel
   * at position -3 with respect to the current filtering position. Since
   * kInitialSubPelOffset is set to 8, we first select sub-pixel filter 8,
   * which is non-zero only in the last tap. So, applying the filter at the
   * current input position will result in an output equal to the pixel at
   * offset +4 (-3 + 7) with respect to the current filtering position.
   */
  const int kPixelSelected = 4;

  /* Assume that each output pixel requires us to step on by 17/16th pixels in
   * the input.
   */
  const int kInputPixelStep = 17;

  /* The filters are setup in such a way that the expected output produces
   * sets of 8 identical output samples. As the filter position moves to the
   * next 1/16th pixel position the only active (=128) filter tap moves one
   * position to the left, resulting in the same input pixel being replicated
   * in to the output for 8 consecutive samples. After each set of 8 positions
   * the filters select a different input pixel. kFilterPeriodAdjust below
   * computes which input pixel is written to the output for a specified
   * x or y position.
   */

  /* Test the horizontal filter. */
  REGISTER_STATE_CHECK(UUT_->h8_(in, kInputStride, out, kOutputStride,
                                 kChangeFilters[kInitialSubPelOffset],
                                 kInputPixelStep, NULL, 0, Width(), Height()));

  for (int x = 0; x < Width(); ++x) {
    const int kFilterPeriodAdjust = (x >> 3) << 3;
    const int ref_x =
        kPixelSelected + ((kInitialSubPelOffset
            + kFilterPeriodAdjust * kInputPixelStep)
                          >> SUBPEL_BITS);
    ASSERT_EQ(lookup(in, ref_x), lookup(out, x))
      << "x == " << x << "width = " << Width();
  }

  /* Test the vertical filter. */
  REGISTER_STATE_CHECK(UUT_->v8_(in, kInputStride, out, kOutputStride,
                                 NULL, 0, kChangeFilters[kInitialSubPelOffset],
                                 kInputPixelStep, Width(), Height()));

  for (int y = 0; y < Height(); ++y) {
    const int kFilterPeriodAdjust = (y >> 3) << 3;
    const int ref_y =
        kPixelSelected + ((kInitialSubPelOffset
            + kFilterPeriodAdjust * kInputPixelStep)
                          >> SUBPEL_BITS);
    ASSERT_EQ(lookup(in, ref_y * kInputStride), lookup(out, y * kInputStride))
      << "y == " << y;
  }

  /* Test the horizontal and vertical filters in combination. */
  REGISTER_STATE_CHECK(UUT_->hv8_(in, kInputStride, out, kOutputStride,
                                  kChangeFilters[kInitialSubPelOffset],
                                  kInputPixelStep,
                                  kChangeFilters[kInitialSubPelOffset],
                                  kInputPixelStep,
                                  Width(), Height()));

  for (int y = 0; y < Height(); ++y) {
    const int kFilterPeriodAdjustY = (y >> 3) << 3;
    const int ref_y =
        kPixelSelected + ((kInitialSubPelOffset
            + kFilterPeriodAdjustY * kInputPixelStep)
                          >> SUBPEL_BITS);
    for (int x = 0; x < Width(); ++x) {
      const int kFilterPeriodAdjustX = (x >> 3) << 3;
      const int ref_x =
          kPixelSelected + ((kInitialSubPelOffset
              + kFilterPeriodAdjustX * kInputPixelStep)
                            >> SUBPEL_BITS);

      ASSERT_EQ(lookup(in, ref_y * kInputStride + ref_x),
                lookup(out, y * kOutputStride + x))
          << "x == " << x << ", y == " << y;
    }
  }
}

/* This test exercises that enough rows and columns are filtered with every
   possible initial fractional positions and scaling steps. */
TEST_P(ConvolveTest, CheckScalingFiltering) {
  uint8_t* const in = input();
  uint8_t* const out = output();
  const InterpKernel *const eighttap = vp9_get_interp_kernel(EIGHTTAP);

  SetConstantInput(127);

  for (int frac = 0; frac < 16; ++frac) {
    for (int step = 1; step <= 32; ++step) {
      /* Test the horizontal and vertical filters in combination. */
      REGISTER_STATE_CHECK(UUT_->hv8_(in, kInputStride, out, kOutputStride,
                                      eighttap[frac], step,
                                      eighttap[frac], step,
                                      Width(), Height()));

      CheckGuardBlocks();

      for (int y = 0; y < Height(); ++y) {
        for (int x = 0; x < Width(); ++x) {
          ASSERT_EQ(lookup(in, y * kInputStride + x),
                    lookup(out, y * kOutputStride + x))
              << "x == " << x << ", y == " << y
              << ", frac == " << frac << ", step == " << step;
        }
      }
    }
  }
}
#if CONFIG_VP9_HIGH
#if HAVE_SSE2 && ARCH_X86_64
void wrap_convolve8_horiz_sse2_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_horiz_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_avg_horiz_sse2_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_horiz_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_vert_sse2_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_vert_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_avg_vert_sse2_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_vert_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_sse2_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_avg_sse2_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_horiz_sse2_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_horiz_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_avg_horiz_sse2_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_horiz_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_vert_sse2_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_vert_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_avg_vert_sse2_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_vert_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_sse2_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_avg_sse2_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_horiz_sse2_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_horiz_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_avg_horiz_sse2_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_horiz_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_vert_sse2_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_vert_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_avg_vert_sse2_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_vert_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_sse2_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_avg_sse2_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_sse2(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
#endif
void wrap_convolve8_horiz_c_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_horiz_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_avg_horiz_c_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_horiz_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_vert_c_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_vert_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_avg_vert_c_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_vert_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_c_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_avg_c_8(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 8);
}
void wrap_convolve8_horiz_c_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_horiz_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_avg_horiz_c_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_horiz_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_vert_c_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_vert_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_avg_vert_c_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_vert_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_c_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_avg_c_10(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 10);
}
void wrap_convolve8_horiz_c_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_horiz_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_avg_horiz_c_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_horiz_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_vert_c_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_vert_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_avg_vert_c_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_vert_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_c_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
void wrap_convolve8_avg_c_12(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int filter_x_stride,
                              const int16_t *filter_y, int filter_y_stride,
                              int w, int h) {
  vp9_high_convolve8_avg_c(src, src_stride, dst, dst_stride, filter_x,
    filter_x_stride, filter_y, filter_y_stride, w, h, 12);
}
#endif
using std::tr1::make_tuple;
#if CONFIG_VP9_HIGH
const ConvolveFunctions convolve8_c(
    wrap_convolve8_horiz_c_8,   wrap_convolve8_avg_horiz_c_8,
    wrap_convolve8_vert_c_8,    wrap_convolve8_avg_vert_c_8,
    wrap_convolve8_c_8,         wrap_convolve8_avg_c_8, 8);
INSTANTIATE_TEST_CASE_P(C_8, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_c),
    make_tuple(8, 4, &convolve8_c),
    make_tuple(4, 8, &convolve8_c),
    make_tuple(8, 8, &convolve8_c),
    make_tuple(16, 8, &convolve8_c),
    make_tuple(8, 16, &convolve8_c),
    make_tuple(16, 16, &convolve8_c),
    make_tuple(32, 16, &convolve8_c),
    make_tuple(16, 32, &convolve8_c),
    make_tuple(32, 32, &convolve8_c),
    make_tuple(64, 32, &convolve8_c),
    make_tuple(32, 64, &convolve8_c),
    make_tuple(64, 64, &convolve8_c)));
const ConvolveFunctions convolve10_c(
    wrap_convolve8_horiz_c_10,  wrap_convolve8_avg_horiz_c_10,
    wrap_convolve8_vert_c_10,   wrap_convolve8_avg_vert_c_10,
    wrap_convolve8_c_10,        wrap_convolve8_avg_c_10, 10);
INSTANTIATE_TEST_CASE_P(C_10, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve10_c),
    make_tuple(8, 4, &convolve10_c),
    make_tuple(4, 8, &convolve10_c),
    make_tuple(8, 8, &convolve10_c),
    make_tuple(16, 8, &convolve10_c),
    make_tuple(8, 16, &convolve10_c),
    make_tuple(16, 16, &convolve10_c),
    make_tuple(32, 16, &convolve10_c),
    make_tuple(16, 32, &convolve10_c),
    make_tuple(32, 32, &convolve10_c),
    make_tuple(64, 32, &convolve10_c),
    make_tuple(32, 64, &convolve10_c),
    make_tuple(64, 64, &convolve10_c)));
const ConvolveFunctions convolve12_c(
    wrap_convolve8_horiz_c_12,  wrap_convolve8_avg_horiz_c_12,
    wrap_convolve8_vert_c_12,   wrap_convolve8_avg_vert_c_12,
    wrap_convolve8_c_12,        wrap_convolve8_avg_c_12, 12);
INSTANTIATE_TEST_CASE_P(C_12, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve12_c),
    make_tuple(8, 4, &convolve12_c),
    make_tuple(4, 8, &convolve12_c),
    make_tuple(8, 8, &convolve12_c),
    make_tuple(16, 8, &convolve12_c),
    make_tuple(8, 16, &convolve12_c),
    make_tuple(16, 16, &convolve12_c),
    make_tuple(32, 16, &convolve12_c),
    make_tuple(16, 32, &convolve12_c),
    make_tuple(32, 32, &convolve12_c),
    make_tuple(64, 32, &convolve12_c),
    make_tuple(32, 64, &convolve12_c),
    make_tuple(64, 64, &convolve12_c)));
#else
const ConvolveFunctions convolve8_c(
    vp9_convolve8_horiz_c, vp9_convolve8_avg_horiz_c,
    vp9_convolve8_vert_c, vp9_convolve8_avg_vert_c,
    vp9_convolve8_c, vp9_convolve8_avg_c, -1);
INSTANTIATE_TEST_CASE_P(C, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_c),
    make_tuple(8, 4, &convolve8_c),
    make_tuple(4, 8, &convolve8_c),
    make_tuple(8, 8, &convolve8_c),
    make_tuple(16, 8, &convolve8_c),
    make_tuple(8, 16, &convolve8_c),
    make_tuple(16, 16, &convolve8_c),
    make_tuple(32, 16, &convolve8_c),
    make_tuple(16, 32, &convolve8_c),
    make_tuple(32, 32, &convolve8_c),
    make_tuple(64, 32, &convolve8_c),
    make_tuple(32, 64, &convolve8_c),
    make_tuple(64, 64, &convolve8_c)));
#endif


#if HAVE_SSE2 && ARCH_X86_64
#if CONFIG_VP9_HIGH
const ConvolveFunctions convolve8_sse2(
    wrap_convolve8_horiz_sse2_8, wrap_convolve8_avg_horiz_sse2_8,
    wrap_convolve8_vert_sse2_8,  wrap_convolve8_avg_vert_sse2_8,
    wrap_convolve8_sse2_8,       wrap_convolve8_avg_sse2_8, 8);
INSTANTIATE_TEST_CASE_P(SSE2_8, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_sse2),
    make_tuple(8, 4, &convolve8_sse2),
    make_tuple(4, 8, &convolve8_sse2),
    make_tuple(8, 8, &convolve8_sse2),
    make_tuple(16, 8, &convolve8_sse2),
    make_tuple(8, 16, &convolve8_sse2),
    make_tuple(16, 16, &convolve8_sse2),
    make_tuple(32, 16, &convolve8_sse2),
    make_tuple(16, 32, &convolve8_sse2),
    make_tuple(32, 32, &convolve8_sse2),
    make_tuple(64, 32, &convolve8_sse2),
    make_tuple(32, 64, &convolve8_sse2),
    make_tuple(64, 64, &convolve8_sse2)));
const ConvolveFunctions convolve10_sse2(
    wrap_convolve8_horiz_sse2_10, wrap_convolve8_avg_horiz_sse2_10,
    wrap_convolve8_vert_sse2_10,  wrap_convolve8_avg_vert_sse2_10,
    wrap_convolve8_sse2_10,       wrap_convolve8_avg_sse2_10, 10);
INSTANTIATE_TEST_CASE_P(SSE2_10, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve10_sse2),
    make_tuple(8, 4, &convolve10_sse2),
    make_tuple(4, 8, &convolve10_sse2),
    make_tuple(8, 8, &convolve10_sse2),
    make_tuple(16, 8, &convolve10_sse2),
    make_tuple(8, 16, &convolve10_sse2),
    make_tuple(16, 16, &convolve10_sse2),
    make_tuple(32, 16, &convolve10_sse2),
    make_tuple(16, 32, &convolve10_sse2),
    make_tuple(32, 32, &convolve10_sse2),
    make_tuple(64, 32, &convolve10_sse2),
    make_tuple(32, 64, &convolve10_sse2),
    make_tuple(64, 64, &convolve10_sse2)));
const ConvolveFunctions convolve12_sse2(
    wrap_convolve8_horiz_sse2_12,  wrap_convolve8_avg_horiz_sse2_12,
    wrap_convolve8_vert_sse2_12,   wrap_convolve8_avg_vert_sse2_12,
    wrap_convolve8_sse2_12,        wrap_convolve8_avg_sse2_12, 12);
INSTANTIATE_TEST_CASE_P(SSE2_12, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve12_sse2),
    make_tuple(8, 4, &convolve12_sse2),
    make_tuple(4, 8, &convolve12_sse2),
    make_tuple(8, 8, &convolve12_sse2),
    make_tuple(16, 8, &convolve12_sse2),
    make_tuple(8, 16, &convolve12_sse2),
    make_tuple(16, 16, &convolve12_sse2),
    make_tuple(32, 16, &convolve12_sse2),
    make_tuple(16, 32, &convolve12_sse2),
    make_tuple(32, 32, &convolve12_sse2),
    make_tuple(64, 32, &convolve12_sse2),
    make_tuple(32, 64, &convolve12_sse2),
    make_tuple(64, 64, &convolve12_sse2)));
#else
const ConvolveFunctions convolve8_sse2(
    vp9_convolve8_horiz_sse2, vp9_convolve8_avg_horiz_sse2,
    vp9_convolve8_vert_sse2, vp9_convolve8_avg_vert_sse2,
    vp9_convolve8_sse2, vp9_convolve8_avg_sse2, -1);
INSTANTIATE_TEST_CASE_P(SSE2, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_sse2),
    make_tuple(8, 4, &convolve8_sse2),
    make_tuple(4, 8, &convolve8_sse2),
    make_tuple(8, 8, &convolve8_sse2),
    make_tuple(16, 8, &convolve8_sse2),
    make_tuple(8, 16, &convolve8_sse2),
    make_tuple(16, 16, &convolve8_sse2),
    make_tuple(32, 16, &convolve8_sse2),
    make_tuple(16, 32, &convolve8_sse2),
    make_tuple(32, 32, &convolve8_sse2),
    make_tuple(64, 32, &convolve8_sse2),
    make_tuple(32, 64, &convolve8_sse2),
    make_tuple(64, 64, &convolve8_sse2)));
#endif
#endif

#if HAVE_SSSE3
const ConvolveFunctions convolve8_ssse3(
    vp9_convolve8_horiz_ssse3, vp9_convolve8_avg_horiz_ssse3,
    vp9_convolve8_vert_ssse3, vp9_convolve8_avg_vert_ssse3,
    vp9_convolve8_ssse3, vp9_convolve8_avg_ssse3, -1);

INSTANTIATE_TEST_CASE_P(SSSE3, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_ssse3),
    make_tuple(8, 4, &convolve8_ssse3),
    make_tuple(4, 8, &convolve8_ssse3),
    make_tuple(8, 8, &convolve8_ssse3),
    make_tuple(16, 8, &convolve8_ssse3),
    make_tuple(8, 16, &convolve8_ssse3),
    make_tuple(16, 16, &convolve8_ssse3),
    make_tuple(32, 16, &convolve8_ssse3),
    make_tuple(16, 32, &convolve8_ssse3),
    make_tuple(32, 32, &convolve8_ssse3),
    make_tuple(64, 32, &convolve8_ssse3),
    make_tuple(32, 64, &convolve8_ssse3),
    make_tuple(64, 64, &convolve8_ssse3)));
#endif

#if HAVE_AVX2
// TODO(jzern): these prototypes can be removed after the avx2 versions are
// reenabled in vp9_rtcd_defs.pl.
extern "C" {
void vp9_convolve8_vert_avx2(const uint8_t *src, ptrdiff_t src_stride,
                             uint8_t *dst, ptrdiff_t dst_stride,
                             const int16_t *filter_x, int x_step_q4,
                             const int16_t *filter_y, int y_step_q4,
                             int w, int h);
void vp9_convolve8_horiz_avx2(const uint8_t *src, ptrdiff_t src_stride,
                              uint8_t *dst, ptrdiff_t dst_stride,
                              const int16_t *filter_x, int x_step_q4,
                              const int16_t *filter_y, int y_step_q4,
                              int w, int h);
void vp9_convolve8_avx2(const uint8_t *src, ptrdiff_t src_stride,
                        uint8_t *dst, ptrdiff_t dst_stride,
                        const int16_t *filter_x, int x_step_q4,
                        const int16_t *filter_y, int y_step_q4,
                        int w, int h);
}

const ConvolveFunctions convolve8_avx2(
    vp9_convolve8_horiz_avx2, vp9_convolve8_avg_horiz_ssse3,
    vp9_convolve8_vert_avx2, vp9_convolve8_avg_vert_ssse3,
    vp9_convolve8_avx2, vp9_convolve8_avg_ssse3, -1);

INSTANTIATE_TEST_CASE_P(AVX2, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_avx2),
    make_tuple(8, 4, &convolve8_avx2),
    make_tuple(4, 8, &convolve8_avx2),
    make_tuple(8, 8, &convolve8_avx2),
    make_tuple(8, 16, &convolve8_avx2)));

INSTANTIATE_TEST_CASE_P(DISABLED_AVX2, ConvolveTest, ::testing::Values(
    make_tuple(16, 8, &convolve8_avx2),
    make_tuple(16, 16, &convolve8_avx2),
    make_tuple(32, 16, &convolve8_avx2),
    make_tuple(16, 32, &convolve8_avx2),
    make_tuple(32, 32, &convolve8_avx2),
    make_tuple(64, 32, &convolve8_avx2),
    make_tuple(32, 64, &convolve8_avx2),
    make_tuple(64, 64, &convolve8_avx2)));
#endif
#if HAVE_NEON_ASM
const ConvolveFunctions convolve8_neon(
    vp9_convolve8_horiz_neon, vp9_convolve8_avg_horiz_neon,
    vp9_convolve8_vert_neon, vp9_convolve8_avg_vert_neon,
    vp9_convolve8_neon, vp9_convolve8_avg_neon, -1);

INSTANTIATE_TEST_CASE_P(NEON, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_neon),
    make_tuple(8, 4, &convolve8_neon),
    make_tuple(4, 8, &convolve8_neon),
    make_tuple(8, 8, &convolve8_neon),
    make_tuple(16, 8, &convolve8_neon),
    make_tuple(8, 16, &convolve8_neon),
    make_tuple(16, 16, &convolve8_neon),
    make_tuple(32, 16, &convolve8_neon),
    make_tuple(16, 32, &convolve8_neon),
    make_tuple(32, 32, &convolve8_neon),
    make_tuple(64, 32, &convolve8_neon),
    make_tuple(32, 64, &convolve8_neon),
    make_tuple(64, 64, &convolve8_neon)));
#endif

#if HAVE_DSPR2
const ConvolveFunctions convolve8_dspr2(
    vp9_convolve8_horiz_dspr2, vp9_convolve8_avg_horiz_dspr2,
    vp9_convolve8_vert_dspr2, vp9_convolve8_avg_vert_dspr2,
    vp9_convolve8_dspr2, vp9_convolve8_avg_dspr2, -1);

INSTANTIATE_TEST_CASE_P(DSPR2, ConvolveTest, ::testing::Values(
    make_tuple(4, 4, &convolve8_dspr2),
    make_tuple(8, 4, &convolve8_dspr2),
    make_tuple(4, 8, &convolve8_dspr2),
    make_tuple(8, 8, &convolve8_dspr2),
    make_tuple(16, 8, &convolve8_dspr2),
    make_tuple(8, 16, &convolve8_dspr2),
    make_tuple(16, 16, &convolve8_dspr2),
    make_tuple(32, 16, &convolve8_dspr2),
    make_tuple(16, 32, &convolve8_dspr2),
    make_tuple(32, 32, &convolve8_dspr2),
    make_tuple(64, 32, &convolve8_dspr2),
    make_tuple(32, 64, &convolve8_dspr2),
    make_tuple(64, 64, &convolve8_dspr2)));
#endif
}  // namespace
