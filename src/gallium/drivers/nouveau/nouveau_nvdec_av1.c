/* SPDX-License-Identifier: MIT
 * Gallium AV1 descriptors to the documented NVDEC firmware interface.
 * NVDEC layout reference: mickys/nvenc, src/av1dec.c (MIT, Copyright 2026 mickys).
 * AV1 algorithms follow sections 7.8, 7.11, 7.16 and 7.18 of the AV1 spec.
 */
#include "nouveau_nvdec_av1.h"
#include "util/u_math.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int
relative_dist(unsigned a, unsigned b, unsigned bits)
{
   unsigned diff = (a - b) & ((1u << bits) - 1);
   return (diff & ((1u << (bits - 1)) - 1)) - (int)(diff & (1u << (bits - 1)));
}

static int64_t
round_signed(int64_t v, unsigned bits)
{
   if (!bits)
      return v;
   return v < 0 ? -((-v + (1ll << (bits - 1))) >> bits) :
                   (v + (1ll << (bits - 1))) >> bits;
}

static bool
shear(const int32_t m[6], int16_t out[4])
{
   if (m[2] <= 0)
      return false;
   for (unsigned i = 0; i < 6; i++)
      if (m[i] < -(1 << 24) || m[i] > (1 << 24))
         return false;
   unsigned n = util_logbase2(m[2]);
   int64_t e = m[2] - (1ll << n);
   unsigned f = n > 8 ? round_signed(e, n - 8) : e << (8 - n);
   int64_t div = ((1ll << 22) + (256 + f) / 2) / (256 + f);
   int64_t values[] = {
      m[2] - (1 << 16), m[3],
      round_signed((int64_t)m[4] * 65536 * div, n + 14),
      m[5] - round_signed((int64_t)m[3] * m[4] * div, n + 14) - (1 << 16),
   };
   for (unsigned i = 0; i < 4; i++)
      values[i] = round_signed(CLAMP(values[i], INT16_MIN, INT16_MAX), 6) * 64;
   if (4 * llabs(values[0]) + 7 * llabs(values[1]) >= 65536 ||
       4 * llabs(values[2]) + 4 * llabs(values[3]) >= 65536)
      return false;
   for (unsigned i = 0; i < 4; i++)
      out[i] = values[i];
   return true;
}

static void
superres(unsigned coded, unsigned upscaled, unsigned sub,
         unsigned short *step, unsigned short *init)
{
   int dw = (coded + sub) >> sub, uw = (upscaled + sub) >> sub;
   int s = ((dw << 14) + uw / 2) / uw;
   int err = uw * s - (dw << 14);
   int x = (-((uw - dw) << 13) + uw / 2) / uw + 128 - err / 2;
   *step = s;
   *init = x & 16383;
}

static unsigned
filter_layout(unsigned width, unsigned height, nvdec_av1_pic_s *p)
{
   unsigned offset = 0;
   if (p)
      p->Av1FltTopOffset = offset >> 8;
   offset += AV1_DBLK_TOP_BUF_SIZE(width);
   if (p)
      p->Av1FltVertOffset = offset >> 8;
   offset += AV1_DBLK_LEFT_BUF_SIZE(height);
   if (p)
      p->Av1CdefVertOffset = offset >> 8;
   offset += AV1_CDEF_LEFT_BUF_SIZE(height);
   if (p)
      p->Av1LrVertOffset = offset >> 8;
   offset += AV1_LR_LEFT_BUF_SIZE(height);
   if (p)
      p->Av1HusVertOffset = offset >> 8;
   offset += AV1_HUS_LEFT_BUF_SIZE(height);
   if (p)
      p->Av1FgsVertOffset = offset >> 8;
   return offset + AV1_FGS_LEFT_BUF_SIZE(height);
}

unsigned
nouveau_nvdec_av1_filter_size(unsigned width, unsigned height)
{
   return filter_layout(width, height, NULL);
}

bool
nouveau_nvdec_av1_picture(const struct pipe_av1_picture_desc *picture,
                          size_t stream_size, unsigned pitch_y, unsigned pitch_uv,
                          unsigned storage_width, unsigned storage_height,
                          const struct nouveau_nvdec_av1_ref *const refs[8],
                          struct nouveau_nvdec_av1_frame *out)
{
   if (!picture || !out || !refs || !stream_size || stream_size > UINT32_MAX)
      return false;
   const __typeof__(picture->picture_parameter) *a = &picture->picture_parameter;
   bool intra = a->pic_info_fields.frame_type == 0 || a->pic_info_fields.frame_type == 2;
   unsigned bpp = a->bit_depth_idx ? 2 : 1;
   unsigned tiles = a->tile_cols * a->tile_rows;
   if (picture->base.protected_playback || a->profile || a->bit_depth_idx > 1 ||
       a->seq_info_fields.mono_chrome || !a->seq_info_fields.subsampling_x ||
       !a->seq_info_fields.subsampling_y || a->frame_width < 128 || a->frame_height < 128 ||
       a->frame_width > storage_width || a->frame_height > storage_height ||
       storage_width > 4096 || storage_height > 4096 ||
       pitch_y < a->frame_width * bpp || pitch_uv < a->frame_width * bpp ||
       pitch_y % 64 || pitch_uv % 64 || pitch_y / 16 > UINT16_MAX || pitch_uv / 16 > UINT16_MAX ||
       a->order_hint_bits_minus_1 > 7 || a->primary_ref_frame > 7 ||
       !a->tile_cols || a->tile_cols > 64 || !a->tile_rows || a->tile_rows > 64 ||
       tiles > 256 || picture->slice_parameter.slice_count != tiles ||
       a->context_update_tile_id >= tiles || a->pic_info_fields.large_scale_tile ||
       a->mode_control_fields.tx_mode > 2 || a->interp_filter > 4 || a->cdef_bits > 3)
      return false;
   memset(out, 0, sizeof(*out));
   out->primary = -1;
   memcpy(out->reference_map, a->ref_frame_idx, sizeof(out->reference_map));
   for (unsigned i = 0; i < 3; i++)
      out->projections[i] = -1;
   nvdec_av1_pic_s *p = &out->pic;
   if (!nouveau_nvdec_av1_film_grain(picture, &p->fgs_cfg, &out->grain))
      return false;
   p->enable_fgs = p->fgs_cfg.apply_grain;
   unsigned coded_width = a->frame_width;
   if (a->pic_info_fields.use_superres) {
      if (a->superres_scale_denominator < 9 || a->superres_scale_denominator > 16)
         return false;
      coded_width = (a->frame_width * 8 + a->superres_scale_denominator / 2) / a->superres_scale_denominator;
   }
   p->gptimer_timeout_value = 81000000;
   p->stream_len = stream_size;
   p->use_128x128_superblock = a->seq_info_fields.use_128x128_superblock;
   p->chroma_format = 1;
   p->bit_depth = bpp == 2 ? 10 : 8;
   p->enable_filter_intra = a->seq_info_fields.enable_filter_intra;
   p->enable_intra_edge_filter = a->seq_info_fields.enable_intra_edge_filter;
   p->enable_interintra_compound = a->seq_info_fields.enable_interintra_compound;
   p->enable_masked_compound = a->seq_info_fields.enable_masked_compound;
   p->enable_dual_filter = a->seq_info_fields.enable_dual_filter;
   p->reserved10 = a->seq_info_fields.enable_order_hint;
   p->enable_jnt_comp = a->seq_info_fields.enable_jnt_comp;
   p->frame_type = a->pic_info_fields.frame_type;
   p->show_frame = 1;
   p->disable_cdf_update = a->pic_info_fields.disable_cdf_update;
   p->allow_screen_content_tools = a->pic_info_fields.allow_screen_content_tools;
   p->cur_frame_force_integer_mv = a->pic_info_fields.force_integer_mv;
   p->scale_denom_minus9 = a->pic_info_fields.use_superres ? a->superres_scale_denominator - 9 : 0;
   p->allow_intrabc = a->pic_info_fields.allow_intrabc;
   p->allow_high_precision_mv = a->pic_info_fields.allow_high_precision_mv;
   p->interp_filter = a->interp_filter;
   p->switchable_motion_mode = a->pic_info_fields.is_motion_mode_switchable;
   p->use_ref_frame_mvs = a->pic_info_fields.use_ref_frame_mvs;
   p->refresh_frame_context = !a->pic_info_fields.disable_frame_end_update_cdf;
   p->delta_q_present_flag = a->mode_control_fields.delta_q_present_flag;
   p->delta_q_res = a->mode_control_fields.log2_delta_q_res;
   p->delta_lf_present_flag = a->mode_control_fields.delta_lf_present_flag;
   p->delta_lf_res = a->mode_control_fields.log2_delta_lf_res;
   p->delta_lf_multi = a->mode_control_fields.delta_lf_multi;
   p->coded_lossless = !a->y_dc_delta_q && !a->u_dc_delta_q && !a->u_ac_delta_q &&
                       !a->v_dc_delta_q && !a->v_ac_delta_q;
   for (unsigned i = 0; i < 8; i++) {
      int delta = a->seg_info.segment_info_fields.enabled && (a->seg_info.feature_mask[i] & 1) ?
                  a->seg_info.feature_data[i][0] : 0;
      if (CLAMP((int)a->base_qindex + delta, 0, 255))
         p->coded_lossless = 0;
   }
   p->tile_enabled = tiles > 1;
   p->tile_cols = a->tile_cols;
   p->tile_rows = a->tile_rows;
   p->context_update_tile_id = a->context_update_tile_id;
   p->superres_is_scaled = a->pic_info_fields.use_superres;
   bool cdef = a->seq_info_fields.enable_cdef && !p->coded_lossless && !p->allow_intrabc &&
               (a->cdef_bits || a->cdef_y_strengths[0] || a->cdef_uv_strengths[0]);
   p->enable_cdef = cdef;
   if (cdef) {
      p->cdef_damping_minus_3 = a->cdef_damping_minus_3;
      p->cdef_bits = a->cdef_bits;
      for (unsigned i = 0; i < 1u << a->cdef_bits; i++) {
         p->cdef_y_pri_strength |= (a->cdef_y_strengths[i] >> 2) << (4 * i);
         p->cdef_uv_pri_strength |= (a->cdef_uv_strengths[i] >> 2) << (4 * i);
         p->cdef_y_sec_strength |= (a->cdef_y_strengths[i] & 3) << (2 * i);
         p->cdef_uv_sec_strength |= (a->cdef_uv_strengths[i] & 3) << (2 * i);
      }
   }
   static const unsigned tx_modes[] = {0, 3, 4};
   p->frame_tx_mode = tx_modes[a->mode_control_fields.tx_mode];
   p->frame_reference_mode = a->mode_control_fields.reference_select ? 2 : 0;
   p->skip_mode_flag = a->mode_control_fields.skip_mode_present;
   p->allow_warp = a->pic_info_fields.allow_warped_motion;
   p->reduced_tx_set_used = a->mode_control_fields.reduced_tx_set_used;
   p->superres_upscaled_width = a->frame_width;
   p->superres_luma_step = p->superres_chroma_step = 14;
   if (p->superres_is_scaled) {
      superres(coded_width, a->frame_width, 0, &p->superres_luma_step, &p->superres_init_luma_subpel_x);
      superres(coded_width, a->frame_width, 1, &p->superres_chroma_step, &p->superres_init_chroma_subpel_x);
   }
   p->base_qindex = a->base_qindex;
   p->y_dc_delta_q = a->y_dc_delta_q;
   p->u_dc_delta_q = a->u_dc_delta_q;
   p->u_ac_delta_q = a->u_ac_delta_q;
   p->v_dc_delta_q = a->v_dc_delta_q;
   p->v_ac_delta_q = a->v_ac_delta_q;
   p->qm_y = a->qmatrix_fields.using_qmatrix ? a->qmatrix_fields.qm_y : 15;
   p->qm_u = a->qmatrix_fields.using_qmatrix ? a->qmatrix_fields.qm_u : 15;
   p->qm_v = a->qmatrix_fields.using_qmatrix ? a->qmatrix_fields.qm_v : 15;
   p->segment_enabled = a->seg_info.segment_info_fields.enabled;
   p->segment_update_map = a->seg_info.segment_info_fields.update_map;
   p->segment_temporal_update = a->seg_info.segment_info_fields.temporal_update;
   if (p->segment_enabled) {
      for (unsigned i = 0; i < 8; i++) {
         for (unsigned j = 0; j < 8; j++) {
            if (!(a->seg_info.feature_mask[i] & (1 << j)))
               continue;
            int v = a->seg_info.feature_data[i][j];
            if (j == 0) {
               if (v < -255 || v > 255)
                  return false;
               p->segment_quant_sign |= (v < 0) << i;
               v = abs(v);
            } else if (j == 5) {
               if (v < 0 || v > 7)
                  return false;
               v = p->frame_type == 0 ? 0 : v + 1;
            } else if (j >= 6) {
               v = 1;
            }
            p->segment_feature_data[i][j] = v;
            p->last_active_segid = i;
            p->segid_preskip |= j >= 5;
         }
      }
   }
   memcpy(p->filter_level, a->filter_level, sizeof(p->filter_level));
   p->filter_level_u = a->filter_level_u;
   p->filter_level_v = a->filter_level_v;
   p->lf_sharpness_level = a->loop_filter_info_fields.sharpness_level;
   if (a->loop_filter_info_fields.mode_ref_delta_enabled) {
      memcpy(p->lf_ref_deltas, a->ref_deltas, sizeof(p->lf_ref_deltas));
      memcpy(p->lf_mode_deltas, a->mode_deltas, sizeof(p->lf_mode_deltas));
   }
   unsigned lr_types[] = {a->loop_restoration_fields.yframe_restoration_type,
                           a->loop_restoration_fields.cbframe_restoration_type,
                           a->loop_restoration_fields.crframe_restoration_type};
   p->lr_unit_size = 0x3f;
   if (!p->coded_lossless && !p->allow_intrabc) {
      p->lr_unit_size = 0;
      for (unsigned i = 0; i < 3; i++) {
         unsigned size = a->lr_unit_size[i];
         if (size != 32 && size != 64 && size != 128 && size != 256)
            return false;
         p->lr_type |= lr_types[i] << (2 * i);
         p->lr_unit_size |= (util_logbase2(size) - 5) << (2 * i);
      }
   }
   p->current_frame.width = coded_width;
   p->current_frame.height = a->frame_height;
   p->current_frame.stride[0] = pitch_y / 16;
   p->current_frame.stride[1] = pitch_uv / 16;
   p->current_frame.frame_buffer_idx = 8;
   p->tileformat = 1;
   p->gob_height = 0;
   filter_layout(storage_width, storage_height, p);

   unsigned sb = p->use_128x128_superblock ? 128 : 64;
   unsigned sb_cols = DIV_ROUND_UP(coded_width, sb), sb_rows = DIV_ROUND_UP(a->frame_height, sb);
   if (a->tile_col_start_sb[0] || a->tile_row_start_sb[0] ||
       a->tile_col_start_sb[a->tile_cols] != sb_cols ||
       a->tile_row_start_sb[a->tile_rows] != sb_rows)
      return false;
   bool seen[256] = {false};
   for (unsigned i = 0; i < tiles; i++) {
      unsigned row = picture->slice_parameter.slice_data_row[i];
      unsigned col = picture->slice_parameter.slice_data_col[i];
      unsigned start = picture->slice_parameter.slice_data_offset[i];
      unsigned size = picture->slice_parameter.slice_data_size[i];
      if (row >= a->tile_rows || col >= a->tile_cols || !size ||
          start > stream_size || size > stream_size - start)
         return false;
      unsigned index = row * a->tile_cols + col;
      if (seen[index])
         return false;
      seen[index] = true;
      unsigned w = a->tile_col_start_sb[col + 1] - a->tile_col_start_sb[col];
      unsigned h = a->tile_row_start_sb[row + 1] - a->tile_row_start_sb[row];
      if (!w || w > 255 || !h || h > 255 || w > sb_cols || h > sb_rows)
         return false;
      AV1TileInfo_OLD *t = &out->tiles[index];
      t->width_in_sb = w;
      t->height_in_sb = h;
      uint32_t end = start + size;
      memcpy(&t->tile_start_b0, &start, sizeof(start));
      memcpy(&t->tile_end_b0, &end, sizeof(end));
   }

   unsigned bits = a->order_hint_bits_minus_1 + 1;
   const struct nouveau_nvdec_av1_ref *r[7] = {NULL};
   int before = -1, after = -1;
   for (unsigned i = 0; i < 7; i++) {
      unsigned index = a->ref_frame_idx[i];
      if (index >= 8)
         return false;
      if (!intra) {
         r[i] = refs[index];
         if (!r[i])
            return false;
      }
      if (r[i] || p->allow_intrabc) {
         ref_frame_struct_s *dst = &p->ref_frame[i];
         unsigned w = r[i] ? r[i]->width : a->frame_width;
         unsigned h = r[i] ? r[i]->height : a->frame_height;
         dst->info.width = w;
         dst->info.height = h;
         dst->info.stride[0] = r[i] ? r[i]->pitch[0] / 16 : pitch_y / 16;
         dst->info.stride[1] = r[i] ? r[i]->pitch[1] / 16 : pitch_uv / 16;
         dst->info.frame_buffer_idx = r[i] ? i : 8;
         int sx = ((w << 14) + coded_width / 2) / coded_width;
         int sy = ((h << 14) + a->frame_height / 2) / a->frame_height;
         if (sx > INT16_MAX || sy > INT16_MAX)
            return false;
         dst->sf.x_scale_fp = sx;
         dst->sf.y_scale_fp = sy;
         if (r[i] && a->seq_info_fields.enable_order_hint) {
            dst->frame_off = relative_dist(a->order_hint, r[i]->order_hint, bits);
            dst->roffset = relative_dist(r[i]->order_hint, a->order_hint, bits);
            dst->sign_bias = dst->roffset > 0;
            if (dst->roffset < 0 && (before < 0 || dst->roffset > p->ref_frame[before].roffset))
               before = i;
            if (dst->roffset > 0 && (after < 0 || dst->roffset < p->ref_frame[after].roffset))
               after = i;
         }
         dst->wmtype = intra ? 0 : a->wm[i].wmtype;
      }
      int32_t matrix[6] = {0, 0, 65536, 0, 0, 65536};
      int16_t sh[4] = {0};
      if (!intra) {
         memcpy(matrix, a->wm[i].wmmat, sizeof(matrix));
         if (a->wm[i].invalid || !shear(matrix, sh) || p->ref_frame[i].sf.x_scale_fp != 16384 ||
             p->ref_frame[i].sf.y_scale_fp != 16384)
            for (unsigned j = 0; j < 4; j++)
               sh[j] = INT16_MIN;
      }
      memcpy(out->global_motion + i * 32, matrix, sizeof(matrix));
      memcpy(out->global_motion + i * 32 + 24, sh, sizeof(sh));
   }
   if (p->skip_mode_flag) {
      if (before < 0)
         return false;
      if (after < 0) {
         for (unsigned i = 0; i < 7; i++) {
            if (r[i] && p->ref_frame[i].roffset < p->ref_frame[before].roffset &&
                (after < 0 || p->ref_frame[i].roffset > p->ref_frame[after].roffset))
               after = i;
         }
      }
      if (after < 0)
         return false;
      p->skip_ref0 = MIN2(before, after) + 1;
      p->skip_ref1 = MAX2(before, after) + 1;
   }
   if (!intra && a->primary_ref_frame < 7) {
      out->primary = a->ref_frame_idx[a->primary_ref_frame];
      const struct nouveau_nvdec_av1_ref *src = r[a->primary_ref_frame];
      p->prevsegid_flag = p->segment_enabled &&
                          DIV_ROUND_UP(src->coded_width, 8) == DIV_ROUND_UP(coded_width, 8) &&
                          DIV_ROUND_UP(src->height, 8) == DIV_ROUND_UP(a->frame_height, 8);
   }
   /* AV1 motion-field projection order: LAST, BWDREF, ALTREF2, ALTREF, LAST2. */
   const unsigned order[] = {0, 4, 5, 6, 1};
   unsigned projections = 0;
   int stamp = 2;
   for (unsigned k = 0; !intra && k < ARRAY_SIZE(order); k++) {
      unsigned i = order[k];
      if (!r[i])
         continue;
      bool use = !r[i]->intra && DIV_ROUND_UP(r[i]->coded_width, 8) == DIV_ROUND_UP(coded_width, 8) &&
                  DIV_ROUND_UP(r[i]->height, 8) == DIV_ROUND_UP(a->frame_height, 8);
      if (i == 0) {
         use &= r[i]->saved_order_hints[6] != (r[3] ? r[3]->order_hint : 0);
         stamp--;
      } else if (i == 4 || i == 5) {
         use &= p->ref_frame[i].roffset > 0;
         if (use)
            stamp--;
      } else if (i == 6) {
         use &= stamp >= 0 && p->ref_frame[i].roffset > 0;
         if (use)
            stamp--;
      } else {
         use &= stamp >= 0;
      }
      if (!use || projections == 3)
         continue;
      unsigned n = projections++;
      out->projections[n] = a->ref_frame_idx[i];
      unsigned type = i + 1;
      if (n == 0) p->mf1_type = type;
      if (n == 1) p->mf2_type = type;
      if (n == 2) p->mf3_type = type;
      int distance = p->ref_frame[i].frame_off;
      bool enable = p->use_ref_frame_mvs && distance >= -31 && distance <= 31;
      if (n == 0) p->use_temporal0_mvs = enable;
      if (n == 1) p->use_temporal1_mvs = enable;
      if (n == 2) p->use_temporal2_mvs = enable;
      if (enable) {
         for (unsigned j = 0; j < 7; j++) {
            p->mfmv_offset[n][j] = relative_dist(r[i]->order_hint, r[i]->saved_order_hints[j], bits);
            p->mfmv_side[n][j] = relative_dist(r[i]->saved_order_hints[j], r[i]->order_hint, bits) > 0;
         }
      }
   }
   return true;
}
