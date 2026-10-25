/* Copyright (c) 2026 mickys
 * SPDX-License-Identifier: MIT
 * AV1 section 7.18.3: prepare the scaling LUTs and cropped grain templates
 * consumed by NVDEC. Adapted from mickys/nvenc src/av1dec.c (MIT).
 * Only the standard Gaussian sequence is shared with Mesa's AMD code;
 * synthesis of the displayed pixels is performed by NVDEC, not the CPU.
 */
#include "nouveau_nvdec_av1.h"
#include "util/u_math.h"
#include "amd/common/ac_vcn_av1_default.h"
#include <stdlib.h>
#include <string.h>

enum { LUMA_W = 82, LUMA_H = 73, CHROMA_W = 44, CHROMA_H = 38 };

struct grain_work {
   int luma[LUMA_W * LUMA_H];
   int chroma[CHROMA_W * CHROMA_H];
};

static unsigned
random_bits(uint16_t *state, unsigned bits)
{
   unsigned s = *state;
   unsigned bit = (s ^ (s >> 1) ^ (s >> 3) ^ (s >> 12)) & 1;
   *state = (s >> 1) | (bit << 15);
   return (*state >> (16 - bits)) & ((1u << bits) - 1);
}

static int
round2(int value, unsigned bits)
{
   return bits ? (value + (1 << (bits - 1))) >> bits : value;
}

static bool
scaling_lut(const uint8_t *x, const uint8_t *y, unsigned count, uint8_t lut[256])
{
   memset(lut, 0, 256);
   if (!count)
      return true;
   for (unsigned i = 1; i < count; i++)
      if (x[i] <= x[i - 1])
         return false;
   memset(lut, y[0], x[0]);
   for (unsigned i = 0; i + 1 < count; i++) {
      unsigned dx = x[i + 1] - x[i];
      int delta = ((int)y[i + 1] - y[i]) * ((65536 + (dx >> 1)) / dx);
      for (unsigned j = 0; j < dx; j++)
         lut[x[i] + j] = y[i] + (((int)j * delta + 32768) >> 16);
   }
   memset(lut + x[count - 1], y[count - 1], 256 - x[count - 1]);
   return true;
}

static void
crop(const int *src, unsigned pitch, unsigned border, unsigned size, int16_t *dst)
{
   for (unsigned y = 0; y < size; y++)
      for (unsigned x = 0; x < size; x++)
         dst[y * size + x] = src[(border + y) * pitch + border + x];
}

static void
chroma_template(const struct pipe_av1_picture_desc *picture, uint16_t seed,
                unsigned points, const int8_t *coeffs, const int *luma,
                unsigned shift, unsigned ar_shift, int min, int max, int *grain)
{
   const __typeof__(picture->picture_parameter.film_grain_info) *g =
      &picture->picture_parameter.film_grain_info;
   memset(grain, 0, CHROMA_W * CHROMA_H * sizeof(*grain));
   if (!points && !g->film_grain_info_fields.chroma_scaling_from_luma)
      return;
   for (unsigned i = 0; i < CHROMA_W * CHROMA_H; i++)
      grain[i] = round2(gaussian_sequence[random_bits(&seed, 11)], shift);
   int lag = g->film_grain_info_fields.ar_coeff_lag;
   for (int y = 3; y < CHROMA_H; y++) {
      for (int x = 3; x < CHROMA_W - 3; x++) {
         int sum = 0;
         unsigned pos = 0;
         for (int dy = -lag; dy <= 0; dy++) {
            for (int dx = -lag; dx <= lag; dx++) {
               if (!dx && !dy) {
                  if (g->num_y_points) {
                     unsigned ly = ((y - 3) * 2) + 3, lx = ((x - 3) * 2) + 3;
                     int avg = luma[ly * LUMA_W + lx] + luma[ly * LUMA_W + lx + 1] +
                               luma[(ly + 1) * LUMA_W + lx] + luma[(ly + 1) * LUMA_W + lx + 1];
                     sum += round2(avg, 2) * coeffs[pos];
                  }
                  goto update;
               }
               sum += grain[(y + dy) * CHROMA_W + x + dx] * coeffs[pos++];
            }
         }
update:
         grain[y * CHROMA_W + x] = CLAMP(grain[y * CHROMA_W + x] + round2(sum, ar_shift), min, max);
      }
   }
}

bool
nouveau_nvdec_av1_film_grain(const struct pipe_av1_picture_desc *picture,
                            av1_fgs_cfg_t *config, AV1FilmGrainMemory *memory)
{
   if (!picture || !config || !memory)
      return false;
   const __typeof__(picture->picture_parameter.film_grain_info) *g =
      &picture->picture_parameter.film_grain_info;
   memset(config, 0, sizeof(*config));
   memset(memory, 0, sizeof(*memory));
   if (!g->film_grain_info_fields.apply_grain)
      return true;
   unsigned bit_depth = picture->picture_parameter.bit_depth_idx ? 10 : 8;
   bool from_luma = g->film_grain_info_fields.chroma_scaling_from_luma;
   if (picture->picture_parameter.bit_depth_idx > 1 ||
       g->num_y_points > 14 || g->num_cb_points > 10 || g->num_cr_points > 10 ||
       (from_luma && !g->num_y_points) || g->cb_offset > 511 || g->cr_offset > 511)
      return false;
   if (!scaling_lut(g->point_y_value, g->point_y_scaling, g->num_y_points, memory->scaling_lut_y))
      return false;
   if (from_luma) {
      memcpy(memory->scaling_lut_cb, memory->scaling_lut_y, 256);
      memcpy(memory->scaling_lut_cr, memory->scaling_lut_y, 256);
   } else if (!scaling_lut(g->point_cb_value, g->point_cb_scaling, g->num_cb_points, memory->scaling_lut_cb) ||
              !scaling_lut(g->point_cr_value, g->point_cr_scaling, g->num_cr_points, memory->scaling_lut_cr)) {
      return false;
   }
   static __thread struct grain_work *work_buf;
   if (!work_buf)
      work_buf = malloc(sizeof(*work_buf));
   struct grain_work *work = work_buf;
   if (!work)
      return false;
   memset(work, 0, sizeof(*work));
   config->apply_grain = 1;
   config->overlap_flag = g->film_grain_info_fields.overlap_flag;
   config->clip_to_restricted_range = g->film_grain_info_fields.clip_to_restricted_range;
   config->chroma_scaling_from_luma = from_luma;
   config->num_y_points_b = g->num_y_points != 0;
   config->num_cb_points_b = g->num_cb_points != 0;
   config->num_cr_points_b = g->num_cr_points != 0;
   config->scaling_shift = g->film_grain_info_fields.grain_scaling_minus_8 + 8;
   config->sw_random_seed = g->grain_seed;
   if (from_luma) {
      config->cb_luma_mult = config->cr_luma_mult = 64;
   } else {
      config->cb_mult = (int)g->cb_mult - 128;
      config->cb_luma_mult = (int)g->cb_luma_mult - 128;
      config->cr_mult = (int)g->cr_mult - 128;
      config->cr_luma_mult = (int)g->cr_luma_mult - 128;
      config->cb_offset = (int)g->cb_offset - 256;
      config->cr_offset = (int)g->cr_offset - 256;
   }
   int min = -(128 << (bit_depth - 8)), max = (128 << (bit_depth - 8)) - 1;
   unsigned shift = 12 - bit_depth + g->film_grain_info_fields.grain_scale_shift;
   unsigned ar_shift = g->film_grain_info_fields.ar_coeff_shift_minus_6 + 6;
   int lag = g->film_grain_info_fields.ar_coeff_lag;
   if (g->num_y_points) {
      uint16_t seed = g->grain_seed;
      for (unsigned i = 0; i < LUMA_W * LUMA_H; i++)
         work->luma[i] = round2(gaussian_sequence[random_bits(&seed, 11)], shift);
      for (int y = 3; y < LUMA_H; y++) {
         for (int x = 3; x < LUMA_W - 3; x++) {
            int sum = 0;
            unsigned pos = 0;
            for (int dy = -lag; dy <= 0; dy++) {
               for (int dx = -lag; dx <= lag; dx++) {
                  if (!dx && !dy)
                     goto update_luma;
                  sum += work->luma[(y + dy) * LUMA_W + x + dx] * g->ar_coeffs_y[pos++];
               }
            }
update_luma:
            work->luma[y * LUMA_W + x] = CLAMP(work->luma[y * LUMA_W + x] + round2(sum, ar_shift), min, max);
         }
      }
   }
   crop(work->luma, LUMA_W, 9, 64, memory->cropped_luma_grain_block);
   chroma_template(picture, g->grain_seed ^ 0xb524, g->num_cb_points, g->ar_coeffs_cb,
                    work->luma, shift, ar_shift, min, max, work->chroma);
   crop(work->chroma, CHROMA_W, 6, 32, memory->cropped_cb_grain_block);
   chroma_template(picture, g->grain_seed ^ 0x49d8, g->num_cr_points, g->ar_coeffs_cr,
                    work->luma, shift, ar_shift, min, max, work->chroma);
   crop(work->chroma, CHROMA_W, 6, 32, memory->cropped_cr_grain_block);
   return true;
}
