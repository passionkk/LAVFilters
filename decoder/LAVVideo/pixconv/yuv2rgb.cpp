/*
 *      Copyright (C) 2011 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 */

#include "stdafx.h"

#include <emmintrin.h>

#include "pixconv_internal.h"
#include "pixconv_sse2_templates.h"

// This function converts 4x2 pixels from the source into 4x2 RGB pixels in the destination
template <PixelFormat inputFormat, int shift, int out32, int right_edge> __forceinline
static int yuv2rgb_convert_pixels(const uint8_t* &srcY, const uint8_t* &srcU, const uint8_t* &srcV, uint8_t* &dst, int srcStrideY, int srcStrideUV, int dstStride, int line, RGBCoeffs *coeffs)
{
  __m128i xmm0,xmm1,xmm2,xmm3,xmm4,xmm5,xmm6,xmm7;
  xmm7 = _mm_setzero_si128 ();

  // Shift > 0 is for 9/10 bit formats
  if (shift > 0) {
    // Load 4 U/V values from line 0/1 into registers
    PIXCONV_LOAD_4PIXEL16(xmm1, srcU);
    PIXCONV_LOAD_4PIXEL16(xmm3, srcU+srcStrideUV);
    PIXCONV_LOAD_4PIXEL16(xmm0, srcV);
    PIXCONV_LOAD_4PIXEL16(xmm2, srcV+srcStrideUV);

    // Interleave U and V
    xmm0 = _mm_unpacklo_epi16(xmm1, xmm0);                       /* 0V0U0V0U */
    xmm2 = _mm_unpacklo_epi16(xmm3, xmm2);                       /* 0V0U0V0U */
  } else {
    PIXCONV_LOAD_4PIXEL8(xmm1, srcU);
    PIXCONV_LOAD_4PIXEL8(xmm3, srcU+srcStrideUV);
    PIXCONV_LOAD_4PIXEL8(xmm0, srcV);
    PIXCONV_LOAD_4PIXEL8(xmm2, srcV+srcStrideUV);

    // Interleave U and V
    xmm0 = _mm_unpacklo_epi8(xmm1, xmm0);                       /* VUVU0000 */
    xmm2 = _mm_unpacklo_epi8(xmm3, xmm2);                       /* VUVU0000 */

    // Expand to 16-bit
    xmm0 = _mm_unpacklo_epi8(xmm0, xmm7);                       /* 0V0U0V0U */
    xmm2 = _mm_unpacklo_epi8(xmm2, xmm7);                       /* 0V0U0V0U */
  }

  // xmm0/xmm2 contain 4 interleaved U/V samples from two lines each in the 16bit parts, still in their native bitdepth

  // Chroma upsampling required
  if (inputFormat == PIX_FMT_YUV420P || inputFormat == PIX_FMT_YUV422P) {
    if (shift > 0) {
      srcU += 4;
      srcV += 4;
    } else {
      srcU += 2;
      srcV += 2;
    }

    // Cut off the over-read into the stride and replace it with the last valid pixel
    if (right_edge) {
      xmm6 = _mm_set_epi32(0, 0xffffffff, 0, 0);

      // First line
      xmm1 = xmm0;
      xmm1 = _mm_slli_si128(xmm1, 4);
      xmm1 = _mm_and_si128(xmm1, xmm6);
      xmm0 = _mm_andnot_si128(xmm6, xmm0);
      xmm0 = _mm_or_si128(xmm0, xmm1);

      // Second line
      xmm3 = xmm2;
      xmm3 = _mm_slli_si128(xmm3, 4);
      xmm3 = _mm_and_si128(xmm3, xmm6);
      xmm2 = _mm_andnot_si128(xmm6, xmm2);
      xmm2 = _mm_or_si128(xmm2, xmm3);
    }

    // 4:2:0 - upsample to 4:2:2 using 75:25
    if (inputFormat == PIX_FMT_YUV420P) {
      xmm1 = xmm0;
      xmm1 = _mm_add_epi16(xmm1, xmm0);                         /* 2x line 0 */
      xmm1 = _mm_add_epi16(xmm1, xmm0);                         /* 3x line 0 */
      xmm1 = _mm_add_epi16(xmm1, xmm2);                         /* 3x line 0 + line 1 (10bit) */

      xmm3 = xmm2;
      xmm3 = _mm_add_epi16(xmm3, xmm2);                         /* 2x line 1 */
      xmm3 = _mm_add_epi16(xmm3, xmm2);                         /* 3x line 1 */
      xmm3 = _mm_add_epi16(xmm3, xmm0);                         /* 3x line 1 + line 0 (10bit) */
    } else {
      xmm1 = xmm0;
      xmm3 = xmm2;

      // Shift to 11 bit
      xmm1 = _mm_slli_epi16(xmm1, 3-shift);
      xmm3 = _mm_slli_epi16(xmm3, 3-shift);
    }
    // After this step, xmm1 and xmm3 contain 8 16-bit values, V and U interleaved. For 4:2:2, filling 11 bit. For 4:2:0, filling input+2 bits (10, 11, 12).

    // Upsample to 4:4:4 using 100:0, 50:50, 0:100 scheme (MPEG2 chroma siting)
    // TODO: MPEG1 chroma siting, use 75:25

    xmm0 = xmm1;                                               /* UV UV UV UV */
    xmm0 = _mm_unpacklo_epi32(xmm0, xmm7);                     /* UV 00 UV 00 */
    xmm1 = _mm_srli_si128(xmm1, 4);                            /* UV UV UV 00 */
    xmm1 = _mm_unpacklo_epi32(xmm7, xmm1);                     /* 00 UV 00 UV */

    xmm1 = _mm_add_epi16(xmm1, xmm0);                          /*  UV  UV  UV  UV */
    xmm1 = _mm_add_epi16(xmm1, xmm0);                          /* 2UV  UV 2UV  UV */

    xmm0 = _mm_slli_si128(xmm0, 4);                            /*  00  UV  00  UV */
    xmm1 = _mm_add_epi16(xmm1, xmm0);                          /* 2UV 2UV 2UV 2UV */

    // Same for the second row
    xmm2 = xmm3;                                               /* UV UV UV UV */
    xmm2 = _mm_unpacklo_epi32(xmm2, xmm7);                     /* UV 00 UV 00 */
    xmm3 = _mm_srli_si128(xmm3, 4);                            /* UV UV UV 00 */
    xmm3 = _mm_unpacklo_epi32(xmm7, xmm3);                     /* 00 UV 00 UV */

    xmm3 = _mm_add_epi16(xmm3, xmm2);                          /*  UV  UV  UV  UV */
    xmm3 = _mm_add_epi16(xmm3, xmm2);                          /* 2UV  UV 2UV  UV */

    xmm2 = _mm_slli_si128(xmm2, 4);                            /*  00  UV  00  UV */
    xmm3 = _mm_add_epi16(xmm3, xmm2);                          /* 2UV 2UV 2UV 2UV */

    // Shift the result to 12 bit
    // For 10-bit input, we need to shift one bit off, or we exceed the allowed processing depth
    // For 8-bit, we need to add one bit
    if (inputFormat == PIX_FMT_YUV420P && shift == 2) {
      xmm1 = _mm_srli_epi16(xmm1, 1);
      xmm3 = _mm_srli_epi16(xmm3, 1);
    } else if (inputFormat == PIX_FMT_YUV420P && shift == 0) {
      xmm1 = _mm_slli_epi16(xmm1, 1);
      xmm3 = _mm_slli_epi16(xmm3, 1);
    }

    // 12-bit result, xmm1 & xmm3 with 4 UV combinations each
  } else if (inputFormat == PIX_FMT_YUV444P) {
    if (shift > 0) {
      srcU += 8;
      srcV += 8;
    } else {
      srcU += 4;
      srcV += 4;
    }
    // Shift to 12 bit
    xmm1 = _mm_slli_epi16(xmm0, 4-shift);
    xmm3 = _mm_slli_epi16(xmm2, 4-shift);
  }

  // After this step, xmm1 & xmm3 contain 4 UV pairs, each in a 16-bit value, filling 12-bit.

  xmm2 = coeffs->CbCr_center;
  xmm1 = _mm_subs_epi16(xmm1, xmm2);
  xmm3 = _mm_subs_epi16(xmm3, xmm2);

  // Load Y
  if (shift > 0) {
    // Load 4 Y values from line 0/1 into registers
    PIXCONV_LOAD_4PIXEL16(xmm5, srcY);
    PIXCONV_LOAD_4PIXEL16(xmm0, srcY+srcStrideY);

    srcY += 8;
  } else {
    PIXCONV_LOAD_4PIXEL8(xmm5, srcY);
    PIXCONV_LOAD_4PIXEL8(xmm0, srcY+srcStrideY);
    srcY += 4;

    xmm5 = _mm_unpacklo_epi8(xmm5, xmm7);                       /* YYYY0000 (16-bit fields) */
    xmm0 = _mm_unpacklo_epi8(xmm0, xmm7);                       /* YYYY0000 (16-bit fields)*/
  }

  xmm0 = _mm_unpacklo_epi64(xmm0, xmm5);                      /* YYYYYYYY */

  // Shift to 14 bits
  xmm0 = _mm_slli_epi16(xmm0, 6-shift);
  xmm0 = _mm_subs_epu16(xmm0, coeffs->Ysub);                  /* Y-16 (in case of range expansion) */
  xmm0 = _mm_mulhi_epi16(xmm0, coeffs->cy);                   /* Y*cy (result is 28 bits, with 12 high-bits packed into the result) */
  xmm0 = _mm_add_epi16(xmm0, coeffs->rgb_add);                /* Y*cy + 16 (in case of range compression) */

  xmm6 = xmm1;
  xmm4 = xmm3;
  xmm6 = _mm_madd_epi16(xmm6, coeffs->cR_Cr);                 /* Result is 25 bits (12 from chroma, 13 from coeff) */
  xmm4 = _mm_madd_epi16(xmm4, coeffs->cR_Cr);
  xmm6 = _mm_srai_epi32(xmm6, 13);                            /* Reduce to 12 bit */
  xmm4 = _mm_srai_epi32(xmm4, 13);
  xmm6 = _mm_packs_epi32(xmm6, xmm7);                         /* Pack back into 16 bit cells */
  xmm4 = _mm_packs_epi32(xmm4, xmm7);
  xmm6 = _mm_unpacklo_epi64(xmm4, xmm6);                      /* Interleave both parts */
  xmm6 = _mm_add_epi16(xmm6, xmm0);                           /* R (12bit) */

  xmm5 = xmm1;
  xmm4 = xmm3;
  xmm5 = _mm_madd_epi16(xmm5, coeffs->cG_Cb_cG_Cr);           /* Result is 25 bits (12 from chroma, 13 from coeff) */
  xmm4 = _mm_madd_epi16(xmm4, coeffs->cG_Cb_cG_Cr);
  xmm5 = _mm_srai_epi32(xmm5, 13);                            /* Reduce to 12 bit */
  xmm4 = _mm_srai_epi32(xmm4, 13);
  xmm5 = _mm_packs_epi32(xmm5, xmm7);                         /* Pack back into 16 bit cells */
  xmm4 = _mm_packs_epi32(xmm4, xmm7);
  xmm5 = _mm_unpacklo_epi64(xmm4, xmm5);                      /* Interleave both parts */
  xmm5 = _mm_add_epi16(xmm5, xmm0);                           /* G (12bit) */

  xmm1 = _mm_madd_epi16(xmm1, coeffs->cB_Cb);                 /* Result is 25 bits (12 from chroma, 13 from coeff) */
  xmm3 = _mm_madd_epi16(xmm3, coeffs->cB_Cb);
  xmm1 = _mm_srai_epi32(xmm1, 13);                            /* Reduce to 12 bit */
  xmm3 = _mm_srai_epi32(xmm3, 13);
  xmm1 = _mm_packs_epi32(xmm1, xmm7);                         /* Pack back into 16 bit cells */
  xmm3 = _mm_packs_epi32(xmm3, xmm7);
  xmm1 = _mm_unpacklo_epi64(xmm3, xmm1);                      /* Interleave both parts */
  xmm1 = _mm_add_epi16(xmm1, xmm0);                           /* B (12bit) */

  // Dithering
  PIXCONV_LOAD_DITHER_COEFFS(xmm2, line, 4, dithers);         /* Load dithering coeffs for 12->8 dithering */
  xmm6 = _mm_adds_epu16(xmm6, xmm2);                          /* Apply coefficients to the RGB values */
  xmm5 = _mm_adds_epu16(xmm5, xmm2);
  xmm1 = _mm_adds_epu16(xmm1, xmm2);

  xmm6 = _mm_srai_epi16(xmm6, 4);                             /* Shift to 8 bit */
  xmm5 = _mm_srai_epi16(xmm5, 4);
  xmm1 = _mm_srai_epi16(xmm1, 4);

  xmm2 = _mm_cmpeq_epi8(xmm2, xmm2);                          /* 0xffffffff,0xffffffff,0xffffffff,0xffffffff */
  xmm6 = _mm_packus_epi16(xmm6, xmm7);                        /* R (lower 8bytes,8bit) * 8 */
  xmm5 = _mm_packus_epi16(xmm5, xmm7);                        /* G (lower 8bytes,8bit) * 8 */
  xmm1 = _mm_packus_epi16(xmm1, xmm7);                        /* B (lower 8bytes,8bit) * 8 */

  xmm6 = _mm_unpacklo_epi8(xmm6,xmm2); // 0xff,R
  xmm1 = _mm_unpacklo_epi8(xmm1,xmm5); // G,B
  xmm2 = xmm1;

  xmm1 = _mm_unpackhi_epi16(xmm1, xmm6); // 0xff,RGB * 4 (line 0)
  xmm2 = _mm_unpacklo_epi16(xmm2, xmm6); // 0xff,RGB * 4 (line 1)

  // TODO: RGB limiting

  if (out32) {
    _mm_stream_si128((__m128i *)(dst), xmm1);
    _mm_stream_si128((__m128i *)(dst + dstStride), xmm2);
    dst += 16;
  } else {
    // RGB 24 output is terribly inefficient due to the un-aligned size of 3 bytes per pixel
    uint32_t eax;
    DECLARE_ALIGNED(16, uint8_t, rgbbuf)[32];
    *(uint32_t *)rgbbuf = _mm_cvtsi128_si32(xmm1);
    xmm1 = _mm_srli_si128(xmm1, 4);
    *(uint32_t *)(rgbbuf+3) = _mm_cvtsi128_si32 (xmm1);
    xmm1 = _mm_srli_si128(xmm1, 4);
    *(uint32_t *)(rgbbuf+6) = _mm_cvtsi128_si32 (xmm1);
    xmm1 = _mm_srli_si128(xmm1, 4);
    *(uint32_t *)(rgbbuf+9) = _mm_cvtsi128_si32 (xmm1);

    *(uint32_t *)(rgbbuf+16) = _mm_cvtsi128_si32 (xmm2);
    xmm2 = _mm_srli_si128(xmm2, 4);
    *(uint32_t *)(rgbbuf+19) = _mm_cvtsi128_si32 (xmm2);
    xmm2 = _mm_srli_si128(xmm2, 4);
    *(uint32_t *)(rgbbuf+22) = _mm_cvtsi128_si32 (xmm2);
    xmm2 = _mm_srli_si128(xmm2, 4);
    *(uint32_t *)(rgbbuf+25) = _mm_cvtsi128_si32 (xmm2);

    xmm1 = _mm_loadl_epi64((const __m128i *)(rgbbuf));
    xmm2 = _mm_loadl_epi64((const __m128i *)(rgbbuf+16));

    _mm_storel_epi64((__m128i *)(dst), xmm1);
    eax = *(uint32_t *)(rgbbuf + 8);
    *(uint32_t *)(dst + 8) = eax;

    _mm_storel_epi64((__m128i *)(dst + dstStride), xmm2);
    eax = *(uint32_t *)(rgbbuf + 24);
    *(uint32_t *)(dst + dstStride + 8) = eax;

    dst += 12;
  }

  return 0;
}

template <PixelFormat inputFormat, int shift, int out32>
static int yuv2rgb_process_lines(const uint8_t *srcY, const uint8_t *srcU, const uint8_t *srcV, uint8_t *dst, int width, int height, int srcStrideY, int srcStrideUV, int dstStride, int sliceYStart, int sliceYEnd, RGBCoeffs *coeffs)
{
  const uint8_t *y = srcY;
  const uint8_t *u = srcU;
  const uint8_t *v = srcV;
  uint8_t *rgb = dst;

  dstStride *= (3 + out32);

  int line = sliceYStart;
  int lastLine = sliceYEnd;
  int sliceYEnd0 = sliceYEnd;

  int endx = width - 4;

  // 4:2:0 needs special handling for the first and the last line
  if (inputFormat == PIX_FMT_YUV420P) {
    if (line == 0) {
      for (int i = 0; i < endx; i += 4) {
        yuv2rgb_convert_pixels<inputFormat, shift, out32, 0>(y, u, v, rgb, 0, 0, 0, line, coeffs);
      }
      yuv2rgb_convert_pixels<inputFormat, shift, out32, 1>(y, u, v, rgb, 0, 0, 0, line, coeffs);

      line = 1;
    }
    if (lastLine == height)
      lastLine--;
  }

  for (; line < lastLine; line += 2) {
    y = srcY + line * srcStrideY;

    if (inputFormat == PIX_FMT_YUV420P) {
      u = srcU + (line >> 1) * srcStrideUV;
      v = srcV + (line >> 1) * srcStrideUV;
    } else {
      u = srcU + line * srcStrideUV;
      v = srcV + line * srcStrideUV;
    }

    rgb = dst + line * dstStride;

    for (int i = 0; i < endx; i += 4) {
      yuv2rgb_convert_pixels<inputFormat, shift, out32, 0>(y, u, v, rgb, srcStrideY, srcStrideUV, dstStride, line, coeffs);
    }
    yuv2rgb_convert_pixels<inputFormat, shift, out32, 1>(y, u, v, rgb, srcStrideY, srcStrideUV, dstStride, line, coeffs);
  }

  if (inputFormat == PIX_FMT_YUV420P) {
    if (sliceYEnd0 == height) {
      y = srcY + (height - 1) * srcStrideY;
      u = srcU + ((height >> 1) - 1)  * srcStrideUV;
      v = srcV + ((height >> 1) - 1)  * srcStrideUV;
      rgb = dst + (height - 1) * dstStride;

      for (int i = 0; i < endx; i += 4) {
        yuv2rgb_convert_pixels<inputFormat, shift, out32, 0>(y, u, v, rgb, 0, 0, 0, line, coeffs);
      }
      yuv2rgb_convert_pixels<inputFormat, shift, out32, 1>(y, u, v, rgb, 0, 0, 0, line, coeffs);
    }
  }
  return 0;
}

template <int out32>
DECLARE_CONV_FUNC_IMPL(convert_yuv_rgb)
{
  RGBCoeffs *coeffs = getRGBCoeffs(width, height);

  // Wrap the input format into template args
  switch (inputFormat) {
  case PIX_FMT_YUV420P:
  case PIX_FMT_YUVJ420P:
    return yuv2rgb_process_lines<PIX_FMT_YUV420P, 0, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  case PIX_FMT_YUV420P10LE:
    return yuv2rgb_process_lines<PIX_FMT_YUV420P, 2, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  case PIX_FMT_YUV420P9LE:
    return yuv2rgb_process_lines<PIX_FMT_YUV420P, 1, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  case PIX_FMT_YUV422P:
  case PIX_FMT_YUVJ422P:
    return yuv2rgb_process_lines<PIX_FMT_YUV422P, 0, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  case PIX_FMT_YUV422P10LE:
    return yuv2rgb_process_lines<PIX_FMT_YUV422P, 2, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  case PIX_FMT_YUV444P:
  case PIX_FMT_YUVJ444P:
    return yuv2rgb_process_lines<PIX_FMT_YUV444P, 0, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  case PIX_FMT_YUV444P10LE:
    return yuv2rgb_process_lines<PIX_FMT_YUV444P, 2, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  case PIX_FMT_YUV444P9LE:
    return yuv2rgb_process_lines<PIX_FMT_YUV444P, 1, out32>(src[0], src[1], src[2], dst, width, height, srcStride[0], srcStride[1], dstStride, 0, height, coeffs);
  default:
    ASSERT(0);
  }
  return S_OK;
}

// Force creation of these two variants
template HRESULT CLAVPixFmtConverter::convert_yuv_rgb<0>CONV_FUNC_PARAMS;
template HRESULT CLAVPixFmtConverter::convert_yuv_rgb<1>CONV_FUNC_PARAMS;

RGBCoeffs* CLAVPixFmtConverter::getRGBCoeffs(int width, int height)
{
  if (!m_rgbCoeffs || width != swsWidth || height != swsHeight) {
    swsWidth = width;
    swsHeight = height;

    if (!m_rgbCoeffs)
      m_rgbCoeffs = (RGBCoeffs *)_aligned_malloc(sizeof(RGBCoeffs), 16);

    AVColorSpace spc = swsColorSpace;
    if (spc == AVCOL_SPC_UNSPECIFIED) {
      spc = (swsHeight >= 720 || swsWidth >= 1280) ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    }

    BOOL inFullRange = (swsColorRange == AVCOL_RANGE_JPEG) || m_InputPixFmt == PIX_FMT_YUVJ420P || m_InputPixFmt == PIX_FMT_YUVJ422P || m_InputPixFmt == PIX_FMT_YUVJ444P;
    BOOL outFullRange = (swsOutputRange == 0) ? inFullRange : (swsOutputRange == 2);

    int inputWhite, inputBlack, inputChroma, outputWhite, outputBlack;
    if (inFullRange) {
      inputWhite = 255;
      inputBlack = 0;
      inputChroma = 1;
    } else {
      inputWhite = 235;
      inputBlack = 16;
      inputChroma = 16;
    }

    if (outFullRange) {
      outputWhite = 255;
      outputBlack = 0;
    } else {
      outputWhite = 235;
      outputBlack = 16;
    }

    double Kr, Kg, Kb;
    switch (spc) {
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
      Kr = 0.299;
      Kg = 0.587;
      Kb = 0.114;
      break;
    case AVCOL_SPC_SMPTE240M:
      Kr = 0.2120;
      Kg = 0.7010;
      Kb = 0.0870;
      break;
    default:
      DbgLog((LOG_TRACE, 10, L"::getRGBCoeffs(): Unknown color space: %d - defaulting to BT709", spc));
    case AVCOL_SPC_BT709:
      Kr = 0.2126;
      Kg = 0.7152;
      Kb = 0.0722;
      break;
    }

    double in_y_range = inputWhite - inputBlack;
    double chr_range = 128 - inputChroma;

    double cspOptionsRGBrange = outputWhite - outputBlack;

    double y_mul, vr_mul, ug_mul, vg_mul, ub_mul;
    y_mul  = cspOptionsRGBrange / in_y_range;
    vr_mul = (cspOptionsRGBrange / chr_range) * (1.0 - Kr);
    ug_mul = (cspOptionsRGBrange / chr_range) * (1.0 - Kb) * Kb / Kg;
    vg_mul = (cspOptionsRGBrange / chr_range) * (1.0 - Kr) * Kr / Kg;
    ub_mul = (cspOptionsRGBrange / chr_range) * (1.0 - Kb);
    short sub = min(outputBlack, inputBlack);
    short Ysub = inputBlack - sub;
    short RGB_add1 = outputBlack - sub;
    short RGB_add3 = (RGB_add1 << 8) + (RGB_add1 << 16) + RGB_add1;

    short cy  = short(y_mul * 16384 + 0.5);
    short crv = short(vr_mul * 8192 + 0.5);
    short cgu = short(-ug_mul * 8192 - 0.5);
    short cgv = short(-vg_mul * 8192 - 0.5);
    short cbu = short(ub_mul * 8192 + 0.5);

    m_rgbCoeffs->Ysub        = _mm_set1_epi16(Ysub << 6);
    m_rgbCoeffs->cy          = _mm_set1_epi16(cy);
    m_rgbCoeffs->CbCr_center = _mm_set1_epi16(128 << 4);

    m_rgbCoeffs->cR_Cr       = _mm_set1_epi32(crv << 16);         // R
    m_rgbCoeffs->cG_Cb_cG_Cr = _mm_set1_epi32((cgv << 16) + cgu); // G
    m_rgbCoeffs->cB_Cb       = _mm_set1_epi32(cbu);               // B

    m_rgbCoeffs->rgb_add     = _mm_set1_epi16(RGB_add1 << 4);
  }
  return m_rgbCoeffs;
}
