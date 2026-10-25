/* Copyright (c) 2026 mickys
 * SPDX-License-Identifier: MIT
 * Native NVENC HEVC configuration and H.265 parameter sets. Adapted from
 * mickys/nvenc src/enc.c and src/hevchdr.c (MIT). The register submission
 * stays in nouveau_nvenc_backend.c; no CUDA/NVENC/Vulkan API is loaded.
 */
#include "nouveau_nvenc_hevc.h"
#include "nvenc_drv.h"
#include "util/u_math.h"
#include <string.h>

enum { OFF_ME = 0x400, OFF_MD = 0x500, OFF_Q = 0x600, OFF_SLICE = 0x700 };
_Static_assert(sizeof(nvenc_h265_drv_pic_setup_s) == OFF_ME, "HEVC picture ABI");
_Static_assert(sizeof(nvenc_h264_me_control_s) <= OFF_MD - OFF_ME, "HEVC ME area");
_Static_assert(sizeof(nvenc_h265_md_control_s) <= OFF_Q - OFF_MD, "HEVC MD area");
_Static_assert(sizeof(nvenc_h265_quant_control_s) <= OFF_SLICE - OFF_Q, "HEVC Q area");

bool
nouveau_nvenc_hevc_config(void *output, const struct nvenc_hevc_params *a)
{
   if (!output || !a || a->width < 160 || a->width > 3840 || a->height < 96 || a->height > 2160 ||
       (a->width & 1) || (a->height & 1) || (a->bit_depth != 8 && a->bit_depth != 10) ||
       a->qp > 51 || a->ref_slot > 1 || a->pitch_bytes % 64 ||
       a->pitch_bytes < align(a->width, 32) * (a->bit_depth > 8 ? 2 : 1) ||
       !a->history_bytes || !a->bitstream_bytes)
      return false;
   memset(output, 0, NOUVEAU_NVENC_HEVC_CONFIG_SIZE);
   unsigned bpp = a->bit_depth > 8 ? 2 : 1;
   unsigned cw = align(a->width, 32), ch = align(a->height, 32);
   nvenc_h265_drv_pic_setup_s *p = output;
   p->magic = 0xc7b70006;
   p->input_cfg = (nvenc_h265_surface_cfg_s) {
      .frame_width_minus1 = a->width - 1, .frame_height_minus1 = a->height - 1,
      .sfc_pitch = a->pitch_bytes / bpp, .sfc_pitch_chroma = a->pitch_bytes / bpp,
      .block_height = 2,
   };
   p->refpic_cfg = p->outputpic_cfg = p->input_cfg;
   p->refpic_cfg.frame_width_minus1 = p->outputpic_cfg.frame_width_minus1 = cw - 1;
   p->refpic_cfg.frame_height_minus1 = p->outputpic_cfg.frame_height_minus1 = ch - 1;
   p->refpic_cfg.luma_bot_offset = p->outputpic_cfg.luma_bot_offset = ch / 2 * a->pitch_bytes / 256;
   p->refpic_cfg.chroma_bot_offset = p->outputpic_cfg.chroma_bot_offset =
      align(ch / 2, 32) / 2 * a->pitch_bytes / 256;
   p->sps_data.chroma_format_idc = 1;
   p->sps_data.log2_max_pic_order_cnt_lsb_minus4 = 4;
   p->sps_data.min_cu_size = 1;
   p->sps_data.max_cu_size = 2;
   p->sps_data.max_tu_size = 3;
   p->sps_data.max_transform_depth_inter = p->sps_data.max_transform_depth_intra = 3;
   p->sps_data.amp_enabled_flag = 1;
   p->sps_data.num_short_term_ref_pic_sets = 1;
   p->sps_data.sample_adaptive_offset_enabled_flag = 1;
   p->sps_data.bit_depth_minus_8 = a->bit_depth - 8;
   p->pps_data.transform_skip_enabled_flag = a->bit_depth == 8;
   p->pps_data.pps_loop_filter_across_slices_enabled_flag = 1;
   p->pps_data.cu_qp_delta_enabled_flag = 1;
   p->pps_data.cabac_init_present_flag = 1;
   /* Sign data hiding stays OFF: the baseline engine controls do not apply
    * it. Signalling it in the PPS would corrupt decoded residuals.
    */
   if (a->bit_depth == 10) {
      /* Main10 firmware layout fields used by the native reference. */
      ((uint8_t *)&p->pps_data)[7] |= 0x08;
      ((uint8_t *)p)[0x314] = 0x22;
      ((uint8_t *)p)[0x315] = 0x02;
   }
   for (unsigned i = 0; i < 3; i++) {
      p->rate_control.QP[i] = a->qp;
      p->rate_control.minQP[i] = a->qp;
      p->rate_control.maxQP[i] = a->qp;
      p->rate_control.rhopbi[i] = 256;
   }
   p->rate_control.framerate = a->rate_den ? MAX2(1, a->rate_num / a->rate_den) : 30;
   p->rate_control.gop_length = UINT32_MAX;
   nvenc_h265_pic_control_s *pc = &p->pic_control;
   memset(pc->l0, -2, sizeof(pc->l0));
   memset(pc->l1, -2, sizeof(pc->l1));
   memset(pc->poc_entry_l0, -1, sizeof(pc->poc_entry_l0));
   memset(pc->poc_entry_l1, -1, sizeof(pc->poc_entry_l1));
   pc->pic_output_flag = 1;
   pc->slice_control_offset = OFF_SLICE;
   pc->me_control_offset = OFF_ME;
   pc->md_control_offset = OFF_MD;
   pc->q_control_offset = OFF_Q;
   pc->hist_buf_size = a->history_bytes;
   pc->bitstream_buf_size = a->bitstream_bytes;
   pc->pic_type = a->idr ? 3 : 0;
   pc->ref_pic_flag = 1;
   pc->codec = 5;
   pc->ceil_log2_pic_size_in_ctbs = util_logbase2_ceil((cw / 32) * (ch / 32));
   pc->sao_luma_mode = pc->sao_chroma_mode = 31;
   pc->nal_unit_type = a->idr ? 19 : 1;
   pc->nuh_temporal_id_plus1 = 1;
   pc->e4byteStartCode = 1;
   pc->qpfifo = 1;
   pc->enable_source_image_padding = 1;
   pc->strips_in_frame = 1;
   pc->chroma_skip_threshold_4x4 = 8;
   pc->pic_order_cnt_lsb = a->poc & 255;
   if (!a->idr) {
      memset(pc->temp_dist_l0, -1, sizeof(pc->temp_dist_l0));
      memset(pc->temp_dist_l1, -1, sizeof(pc->temp_dist_l1));
      memset(pc->dist_scale_factor, -1, sizeof(pc->dist_scale_factor));
      pc->l0[0] = 2 * a->ref_slot;
      pc->poc_entry_l0[0] = 0;
      pc->temp_dist_l0[0] = 1;
      pc->short_term_rps.num_negative_pics = 1;
      pc->short_term_rps.used_by_curr_pic_s0_flag = 1;
      pc->short_term_rps.delta_poc_s0_minus1[0] = 0;
      pc->dist_scale_factor[0][0] = 256;
      pc->same_poc[0] = 1;
      pc->diff_pic_order_cnt_zero[0] = pc->diff_pic_order_cnt_zero[1] = 0xfefefefe;
   }
   p->gpTimer_timeout_val = 81000000;
   nvenc_h265_slice_control_s *slice = (void *)((uint8_t *)output + OFF_SLICE);
   slice->num_ctus = (cw / 32) * (ch / 32);
   slice->qp_avr = a->qp;
   /* NVENC uses pic-type numbering here (P=0, B=1, I=2). */
   slice->slice_type = a->idr ? 2 : 0;
   slice->slice_loop_filter_across_slices_enabled_flag = 1;
   slice->slice_sao_luma_flag = slice->slice_sao_chroma_flag = 1;
   if (!a->idr) {
      slice->num_ref_idx_active_override_flag = 1;
      slice->num_ref_idx_l0_active_minus1 = 0;
      slice->num_ref_idx_l1_active_minus1 = 0;
   }
   nvenc_h264_me_control_s *me = (void *)((uint8_t *)output + OFF_ME);
   me->refinement_mode = 1;
   me->refine_on_search_enable = 1;
   me->fps_mvcost = me->sps_mvcost = me->sps_cost_func = 1;
   me->predsrc.self_temporal_stamp_l0 = me->predsrc.self_temporal_stamp_l1 = 1;
   me->predsrc.self_temporal_search = me->predsrc.self_temporal_enable = 1;
   me->predsrc.self_spatial_search = me->predsrc.self_spatial_enable = 1;
   me->predsrc.const_mv_stamp_l0 = me->predsrc.const_mv_stamp_l1 = 2;
   me->predsrc.const_mv_search = me->predsrc.const_mv_enable = 1;
   me->predsrc.stamp_refidx1_stamp = me->predsrc.stamp_refidx2_stamp = me->predsrc.stamp_refidx3_stamp = 3;
   nvenc_h264_me_stamp_s *shapes = &me->shape0;
   shapes[0].bitmask[0] = shapes[2].bitmask[0] = 0xff0c0c00;
   shapes[0].bitmask[1] = shapes[2].bitmask[1] = 0xc0c;
   shapes[1].bitmask[0] = 0x1e1e0c00;
   shapes[1].bitmask[1] = shapes[3].bitmask[1] = 0xc;
   shapes[3].bitmask[0] = 0x1e0c0000;
   for (unsigned i = 0; i < 4; i++)
      shapes[i].hor_adjust = 1;
   shapes[1].ver_adjust = 1;
   me->partDecisionMadeByFPP = me->Cu16partDecisionMadeByFPP = 1;
   me->penalty_factor_FPP = 15;
   me->spatial_hint_pattern = 6;
   me->temporal_hint_pattern = 4;
   me->fbm_op_winner_num_p_frame = me->fbm_op_winner_num_b_frame_l0 = me->fbm_op_winner_num_b_frame_l1 = 1;
   me->fbm_select_best_cu32_parttype_num = me->sps_evaluate_merge_cand_num = 3;
   me->fps_quad_thresh_hold = 50;
   me->external_hint_order = me->coloc_hint_order = 1;
   me->hint_type1 = 1; me->hint_type2 = 2; me->hint_type3 = 3; me->hint_type4 = 4;
   nvenc_h265_md_control_s *md = (void *)((uint8_t *)output + OFF_MD);
   md->h265_intra_luma4x4_mode_enable = md->h265_intra_luma8x8_mode_enable = 0x55555557;
   md->h265_intra_luma16x16_mode_enable = md->h265_intra_luma32x32_mode_enable = 0x55555557;
   md->h265_intra_luma_mode_enable_leftbits = 0xb6d;
   md->cu16x16_part_2Nx2N_enable = md->cu16x16_part_2NxN_enable = md->cu16x16_part_Nx2N_enable = md->cu16x16_part_NxN_enable = 1;
   md->cu32x32_part_2Nx2N_enable = md->cu32x32_part_2NxN_enable = md->cu32x32_part_Nx2N_enable = 1;
   md->cu32x32_part_2NxnU_enable = md->cu32x32_part_2NxnD_enable = md->cu32x32_part_nLx2N_enable = md->cu32x32_part_nRx2N_enable = 1;
   md->intra_mode_4x4_enable = md->intra_mode_8x8_enable = md->intra_mode_16x16_enable = md->intra_mode_32x32_enable = 1;
   md->h265_intra_chroma_mode_enable = 0x1f;
   md->tusize_4x4_enable = md->tusize_8x8_enable = md->tusize_16x16_enable = md->tusize_32x32_enable = 1;
   md->pskip_enable = md->mv_cost_enable = 1;
   md->ip_search_mode = 7;
   md->tu_search_num_cu16 = 2;
   md->tu_search_num_cu32 = 1;
   md->intra_ssd_cnt_4x4 = md->intra_ssd_cnt_8x8 = md->intra_ssd_cnt_16x16 = md->intra_ssd_cnt_32x32 = 1;
   nvenc_h265_quant_control_s *q = (void *)((uint8_t *)output + OFF_Q);
   q->qpp_run_vector_4x4 = 0x56b;
   q->qpp_run_vector_8x8[0] = 0xaaff;
   q->qpp_run_vector_8x8[1] = 0xffaa;
   q->qpp_run_vector_8x8[2] = 0xf;
   q->qpp_luma8x8_cost = q->qpp_luma16x16_cost = q->qpp_chroma_cost = 15;
   return true;
}

struct bits { uint8_t data[192]; unsigned count; bool failed; };
static void
put(struct bits *b, uint32_t value, unsigned n)
{
   if (n > 32 || b->count + n > sizeof(b->data) * 8) { b->failed = true; return; }
   for (unsigned i = n; i; i--, b->count++)
      b->data[b->count / 8] |= ((value >> (i - 1)) & 1) << (7 - b->count % 8);
}
static void
ue(struct bits *b, unsigned value)
{
   unsigned x = value + 1, n = util_logbase2(x);
   put(b, 0, n); put(b, x, n + 1);
}
static size_t
emit(struct bits *b, unsigned type, uint8_t *out, size_t capacity)
{
   put(b, 1, 1);
   while (b->count % 8) put(b, 0, 1);
   if (b->failed || capacity < 6) return 0;
   memcpy(out, "\0\0\0\1", 4); out[4] = type << 1; out[5] = 1;
   size_t used = 6;
   unsigned zeros = 0;
   for (unsigned i = 0; i < b->count / 8; i++) {
      uint8_t v = b->data[i];
      if (used + 2 > capacity) return 0;
      if (zeros >= 2 && v <= 3) { out[used++] = 3; zeros = 0; }
      out[used++] = v; zeros = v ? 0 : zeros + 1;
   }
   return used;
}
static void
profile_tier(struct bits *b, const struct nvenc_hevc_params *a)
{
   uint64_t pixels = (uint64_t)a->width * a->height;
   uint64_t rate = a->rate_den ? pixels * a->rate_num / a->rate_den : pixels * 30;
   unsigned level = pixels <= 552960 && rate <= 33177600 ? 93 :
                    pixels <= 2228224 && rate <= 133693440 ? 123 : 153;
   put(b, 0, 2); put(b, 0, 1); put(b, a->bit_depth == 10 ? 2 : 1, 5);
   put(b, a->bit_depth == 10 ? 0x20000000 : 0x60000000, 32);
   put(b, 1, 1); put(b, 0, 1); put(b, 0, 1); put(b, 1, 1);
   put(b, 0, 32); put(b, 0, 11); put(b, 0, 1); put(b, level, 8);
}
size_t
nouveau_nvenc_hevc_headers(const struct nvenc_hevc_params *a, void *output, size_t capacity)
{
   if (!a || !output || capacity < 128 || !a->width || !a->height ||
       a->width > 3840 || a->height > 2160 || (a->width & 1) || (a->height & 1) ||
       (a->bit_depth != 8 && a->bit_depth != 10)) return 0;
   uint8_t *out = output;
   size_t used = 0, n;
   struct bits vps = {0}, sps = {0}, pps = {0};
   put(&vps, 0, 4); put(&vps, 1, 1); put(&vps, 1, 1);
   put(&vps, 0, 6); put(&vps, 0, 3); put(&vps, 1, 1); put(&vps, 0xffff, 16);
   profile_tier(&vps, a);
   put(&vps, 1, 1); ue(&vps, 1); ue(&vps, 0); ue(&vps, 0);
   put(&vps, 0, 6); ue(&vps, 0); put(&vps, 0, 1); put(&vps, 0, 1);
   n = emit(&vps, 32, out, capacity); if (!n) return 0; used += n;
   unsigned cw = align(a->width, 32), ch = align(a->height, 32);
   put(&sps, 0, 4); put(&sps, 0, 3); put(&sps, 1, 1); profile_tier(&sps, a);
   ue(&sps, 0); ue(&sps, 1); ue(&sps, cw); ue(&sps, ch);
   bool crop = cw != a->width || ch != a->height;
   put(&sps, crop, 1);
   if (crop) { ue(&sps, 0); ue(&sps, (cw - a->width) / 2); ue(&sps, 0); ue(&sps, (ch - a->height) / 2); }
   ue(&sps, a->bit_depth - 8); ue(&sps, a->bit_depth - 8); ue(&sps, 4);
   put(&sps, 1, 1); ue(&sps, 1); ue(&sps, 0); ue(&sps, 0);
   ue(&sps, 1); ue(&sps, 1); ue(&sps, 0); ue(&sps, 3); ue(&sps, 3); ue(&sps, 3);
   put(&sps, 0, 1); put(&sps, 1, 1); put(&sps, 1, 1); put(&sps, 0, 1);
   ue(&sps, 1); ue(&sps, 1); ue(&sps, 0); ue(&sps, 0); put(&sps, 0, 1);
   put(&sps, 0, 1); put(&sps, 0, 1); put(&sps, 0, 1);
   /* No VUI/HRD claims until their input controls are implemented. */
   put(&sps, 0, 1); put(&sps, 0, 1);
   n = emit(&sps, 33, out + used, capacity - used); if (!n) return 0; used += n;
   ue(&pps, 0); ue(&pps, 0); put(&pps, 0, 1); put(&pps, 0, 1); put(&pps, 0, 3);
   put(&pps, 0, 1); put(&pps, 1, 1); ue(&pps, 0); ue(&pps, 0); ue(&pps, 0);
   put(&pps, 0, 1); put(&pps, a->bit_depth == 8, 1); put(&pps, 1, 1); ue(&pps, 0);
   ue(&pps, 0); ue(&pps, 0); put(&pps, 0, 1);
   put(&pps, 0, 1); put(&pps, 0, 1); put(&pps, 0, 1);
   put(&pps, 0, 1); put(&pps, 0, 1); put(&pps, 1, 1);
   put(&pps, 1, 1); put(&pps, 0, 1); put(&pps, 0, 1); ue(&pps, 0); ue(&pps, 0);
   put(&pps, 0, 1); put(&pps, 0, 1); ue(&pps, 0); put(&pps, 0, 1); put(&pps, 0, 1);
   n = emit(&pps, 34, out + used, capacity - used); if (!n) return 0;
   return used + n;
}
