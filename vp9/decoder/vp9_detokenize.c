/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "./vpx_config.h"

#include "vpx_mem/vpx_mem.h"
#include "vpx_ports/mem.h"

#include "vp9/common/vp9_blockd.h"
#include "vp9/common/vp9_common.h"
#include "vp9/common/vp9_idct.h"

#include "vp9/decoder/vp9_detokenize.h"

#define EOB_CONTEXT_NODE            0
#define ZERO_CONTEXT_NODE           1
#define ONE_CONTEXT_NODE            2
#define LOW_VAL_CONTEXT_NODE        0
#define TWO_CONTEXT_NODE            1
#define THREE_CONTEXT_NODE          2
#define HIGH_LOW_CONTEXT_NODE       3
#define CAT_ONE_CONTEXT_NODE        4
#define CAT_THREEFOUR_CONTEXT_NODE  5
#define CAT_THREE_CONTEXT_NODE      6
#define CAT_FIVE_CONTEXT_NODE       7

#define CAT1_MIN_VAL    5
#define CAT2_MIN_VAL    7
#define CAT3_MIN_VAL   11
#define CAT4_MIN_VAL   19
#define CAT5_MIN_VAL   35
#define CAT6_MIN_VAL   67

#define INCREMENT_COUNT(token)                              \
  do {                                                      \
     if (!cm->frame_parallel_decoding_mode)                 \
       ++coef_counts[band][ctx][token];                     \
  } while (0)

#define WRITE_COEF_CONTINUE(val, token)                  \
  {                                                      \
    v = (val * dqv) >> dq_shift;                         \
    dqcoeff[scan[c]] = vp9_read_bit(r) ? -v : v;         \
    token_cache[scan[c]] = vp9_pt_energy_class[token];   \
    ++c;                                                 \
    ctx = get_coef_context(nb, token_cache, c);          \
    dqv = dq[1];                                         \
    continue;                                            \
  }

#define ADJUST_COEF(prob, bits_count)                   \
  do {                                                  \
    val += (vp9_read(r, prob) << bits_count);           \
  } while (0)

static int decode_coefs(VP9_COMMON *cm, const MACROBLOCKD *xd, PLANE_TYPE type,
                       tran_low_t *dqcoeff, TX_SIZE tx_size, const int16_t *dq,
                       int ctx, const int16_t *scan, const int16_t *nb,
                       vp9_reader *r) {
  const int max_eob = 16 << (tx_size << 1);
  const FRAME_CONTEXT *const fc = &cm->fc;
  FRAME_COUNTS *const counts = &cm->counts;
  const int ref = is_inter_block(&xd->mi[0]->mbmi);
  int band, c = 0;
  const vp9_prob (*coef_probs)[COEFF_CONTEXTS][UNCONSTRAINED_NODES] =
      fc->coef_probs[tx_size][type][ref];
  const vp9_prob *prob;
  unsigned int (*coef_counts)[COEFF_CONTEXTS][UNCONSTRAINED_NODES + 1] =
      counts->coef[tx_size][type][ref];
  unsigned int (*eob_branch_count)[COEFF_CONTEXTS] =
      counts->eob_branch[tx_size][type][ref];
  uint8_t token_cache[32 * 32];
  const uint8_t *cat1_ptr, *cat2_ptr, *cat3_ptr, *cat4_ptr, *cat5_ptr;
  const uint8_t *cat6_ptr;
  const uint8_t *cat6;
  const uint8_t *band_translate = get_band_translate(tx_size);
  const int dq_shift = (tx_size == TX_32X32);
  int v;
  int16_t dqv = dq[0];

#if CONFIG_VP9_HIGH && CONFIG_HIGH_TRANSFORMS && CONFIG_HIGH_QUANT
  if (cm->use_high) {
    if (cm->bit_depth == VPX_BITS_10) {
      cat1_ptr = vp9_cat1_prob_high10;
      cat2_ptr = vp9_cat2_prob_high10;
      cat3_ptr = vp9_cat3_prob_high10;
      cat4_ptr = vp9_cat4_prob_high10;
      cat5_ptr = vp9_cat5_prob_high10;
      cat6_ptr = vp9_cat6_prob_high10;
    } else {
      cat1_ptr = vp9_cat1_prob_high12;
      cat2_ptr = vp9_cat2_prob_high12;
      cat3_ptr = vp9_cat3_prob_high12;
      cat4_ptr = vp9_cat4_prob_high12;
      cat5_ptr = vp9_cat5_prob_high12;
      cat6_ptr = vp9_cat6_prob_high12;
    }
  } else {
    cat1_ptr = vp9_cat1_prob;
    cat2_ptr = vp9_cat2_prob;
    cat3_ptr = vp9_cat3_prob;
    cat4_ptr = vp9_cat4_prob;
    cat5_ptr = vp9_cat5_prob;
    cat6_ptr = vp9_cat6_prob;
  }
#else
  cat1_ptr = vp9_cat1_prob;
  cat2_ptr = vp9_cat2_prob;
  cat3_ptr = vp9_cat3_prob;
  cat4_ptr = vp9_cat4_prob;
  cat5_ptr = vp9_cat5_prob;
  cat6_ptr = vp9_cat6_prob;
#endif

  while (c < max_eob) {
    int val;
    band = *band_translate++;
    prob = coef_probs[band][ctx];
    if (!cm->frame_parallel_decoding_mode)
      ++eob_branch_count[band][ctx];
    if (!vp9_read(r, prob[EOB_CONTEXT_NODE])) {
      INCREMENT_COUNT(EOB_MODEL_TOKEN);
      break;
    }

    while (!vp9_read(r, prob[ZERO_CONTEXT_NODE])) {
      INCREMENT_COUNT(ZERO_TOKEN);
      dqv = dq[1];
      token_cache[scan[c]] = 0;
      ++c;
      if (c >= max_eob)
        return c;  // zero tokens at the end (no eob token)
      ctx = get_coef_context(nb, token_cache, c);
      band = *band_translate++;
      prob = coef_probs[band][ctx];
    }

    // ONE_CONTEXT_NODE_0_
    if (!vp9_read(r, prob[ONE_CONTEXT_NODE])) {
      INCREMENT_COUNT(ONE_TOKEN);
      WRITE_COEF_CONTINUE(1, ONE_TOKEN);
    }

    INCREMENT_COUNT(TWO_TOKEN);

    prob = vp9_pareto8_full[prob[PIVOT_NODE] - 1];

    if (!vp9_read(r, prob[LOW_VAL_CONTEXT_NODE])) {
      if (!vp9_read(r, prob[TWO_CONTEXT_NODE])) {
        WRITE_COEF_CONTINUE(2, TWO_TOKEN);
      }
      if (!vp9_read(r, prob[THREE_CONTEXT_NODE])) {
        WRITE_COEF_CONTINUE(3, THREE_TOKEN);
      }
      WRITE_COEF_CONTINUE(4, FOUR_TOKEN);
    }

    if (!vp9_read(r, prob[HIGH_LOW_CONTEXT_NODE])) {
      if (!vp9_read(r, prob[CAT_ONE_CONTEXT_NODE])) {
        val = CAT1_MIN_VAL;
        ADJUST_COEF(cat1_ptr[0], 0);
        WRITE_COEF_CONTINUE(val, CATEGORY1_TOKEN);
      }
      val = CAT2_MIN_VAL;
      ADJUST_COEF(cat2_ptr[0], 1);
      ADJUST_COEF(cat2_ptr[1], 0);
      WRITE_COEF_CONTINUE(val, CATEGORY2_TOKEN);
    }

    if (!vp9_read(r, prob[CAT_THREEFOUR_CONTEXT_NODE])) {
      if (!vp9_read(r, prob[CAT_THREE_CONTEXT_NODE])) {
        val = CAT3_MIN_VAL;
        ADJUST_COEF(cat3_ptr[0], 2);
        ADJUST_COEF(cat3_ptr[1], 1);
        ADJUST_COEF(cat3_ptr[2], 0);
        WRITE_COEF_CONTINUE(val, CATEGORY3_TOKEN);
      }
      val = CAT4_MIN_VAL;
      ADJUST_COEF(cat4_ptr[0], 3);
      ADJUST_COEF(cat4_ptr[1], 2);
      ADJUST_COEF(cat4_ptr[2], 1);
      ADJUST_COEF(cat4_ptr[3], 0);
      WRITE_COEF_CONTINUE(val, CATEGORY4_TOKEN);
    }

    if (!vp9_read(r, prob[CAT_FIVE_CONTEXT_NODE])) {
      val = CAT5_MIN_VAL;
      ADJUST_COEF(cat5_ptr[0], 4);
      ADJUST_COEF(cat5_ptr[1], 3);
      ADJUST_COEF(cat5_ptr[2], 2);
      ADJUST_COEF(cat5_ptr[3], 1);
      ADJUST_COEF(cat5_ptr[4], 0);
      WRITE_COEF_CONTINUE(val, CATEGORY5_TOKEN);
    }
    val = 0;

    cat6 = cat6_ptr;
    while (*cat6)
      val = (val << 1) | vp9_read(r, *cat6++);
    val += CAT6_MIN_VAL;

    WRITE_COEF_CONTINUE(val, CATEGORY6_TOKEN);
  }

  return c;
}

int vp9_decode_block_tokens(VP9_COMMON *cm, MACROBLOCKD *xd,
                            int plane, int block, BLOCK_SIZE plane_bsize,
                            int x, int y, TX_SIZE tx_size, vp9_reader *r) {
  struct macroblockd_plane *const pd = &xd->plane[plane];
  const int ctx = get_entropy_context(tx_size, pd->above_context + x,
                                               pd->left_context + y);
  const scan_order *so = get_scan(xd, tx_size, pd->plane_type, block);
  const int eob = decode_coefs(cm, xd, pd->plane_type,
                               BLOCK_OFFSET(pd->dqcoeff, block), tx_size,
                               pd->dequant, ctx, so->scan, so->neighbors, r);
  vp9_set_contexts(xd, pd, plane_bsize, tx_size, eob > 0, x, y);
  return eob;
}


