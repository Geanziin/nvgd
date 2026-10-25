/* SPDX-License-Identifier: MIT */
#ifndef NOUVEAU_NVDEC_AV1_H
#define NOUVEAU_NVDEC_AV1_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pipe/p_video_state.h"
#include "nvdec_drv.h"

#define NVDEC_AV1_CDF_SIZE 0x2fc0

struct nouveau_nvdec_av1_ref {
   unsigned width, height, coded_width;
   unsigned pitch[2];
   unsigned order_hint;
   unsigned saved_order_hints[7];
   bool intra, segmented;
};

struct nouveau_nvdec_av1_frame {
   nvdec_av1_pic_s pic;
   AV1TileInfo_OLD tiles[AV1_MAX_TILES];
   uint8_t global_motion[7 * 32];
   AV1FilmGrainMemory grain;
   int primary;
   int projections[3];
   uint8_t reference_map[7];
   /* Borrowed Gallium display planes; reference planes remain grain-free. */
   struct pipe_resource *display[2];
};

void nouveau_nvdec_av1_default_cdf(unsigned qctx, void *table);
bool nouveau_nvdec_av1_picture(const struct pipe_av1_picture_desc *picture,
                               size_t stream_size, unsigned pitch_y, unsigned pitch_uv,
                               unsigned storage_width, unsigned storage_height,
                               const struct nouveau_nvdec_av1_ref *const refs[8],
                               struct nouveau_nvdec_av1_frame *out);
unsigned nouveau_nvdec_av1_filter_size(unsigned width, unsigned height);
bool nouveau_nvdec_av1_film_grain(const struct pipe_av1_picture_desc *picture,
                                  av1_fgs_cfg_t *config, AV1FilmGrainMemory *memory);

#endif
