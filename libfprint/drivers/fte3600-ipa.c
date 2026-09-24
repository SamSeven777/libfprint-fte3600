/*
 * 2D Invariant Point Attention (2D-IPA) Matcher for FocalTech FT9361 / FTE3600
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fte3600-ipa.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define IPA_GAMMA_DIST  0.002f
#define IPA_GAMMA_ANGLE 0.80f
#define IPA_INV_SQRT_D  0.1767767f /* 1.0 / sqrt(32) */

/* Orthogonal projection matrices for 2D-IPA self-attention */
static const gfloat W_q[1024] = {
    -0.103006f, 0.089458f, -0.057394f, 0.098485f, -0.228190f, 0.224160f, -0.198305f, 0.046927f,
    -0.087816f, 0.264779f, -0.016335f, -0.088612f, 0.218558f, -0.252037f, 0.220199f, 0.052601f,
    -0.247293f, 0.069792f, 0.171120f, -0.144463f, 0.063851f, 0.006935f, 0.016768f, -0.266209f,
    -0.085897f, 0.041697f, 0.220119f, 0.263889f, 0.134259f, 0.231922f, 0.285893f, 0.148560f,
    -0.049449f, 0.142071f, 0.230752f, 0.067332f, 0.169134f, -0.218131f, -0.210452f, -0.096417f,
    0.044146f, 0.003920f, -0.083398f, 0.131102f, 0.108480f, 0.161137f, 0.024505f, -0.014603f,
    -0.038167f, 0.229156f, 0.106579f, 0.127021f, -0.027063f, 0.169225f, -0.259972f, 0.195655f,
    0.347571f, 0.029851f, 0.203099f, -0.015038f, -0.048598f, 0.281878f, -0.076043f, -0.043322f,
    -0.122709f, 0.104084f, 0.248235f, 0.015822f, 0.158204f, 0.090620f, 0.067425f, 0.265691f,
    0.281898f, 0.200171f, -0.165275f, 0.087405f, 0.138612f, 0.091176f, -0.199042f, 0.104273f,
    -0.149258f, -0.211833f, 0.117366f, 0.297495f, 0.137839f, 0.008432f, -0.062835f, 0.109798f,
    -0.193155f, 0.002824f, 0.019472f, -0.108861f, -0.129211f, 0.033620f, -0.165038f, -0.022513f,
    -0.278385f, 0.040182f, 0.154130f, 0.113941f, 0.048995f, -0.153723f, -0.011832f, 0.222718f,
    0.160163f, -0.207914f, 0.037521f, -0.218559f, 0.129792f, 0.152434f, -0.146681f, -0.121961f,
    0.170668f, -0.177265f, 0.097092f, -0.284346f, 0.039642f, 0.277813f, -0.014589f, -0.174366f,
    0.164391f, -0.175516f, 0.124502f, 0.069415f, -0.167098f, 0.076878f, 0.216398f, -0.005118f,
    0.289139f, 0.219213f, -0.082728f, -0.101297f, 0.040410f, 0.020165f, 0.196901f, 0.068222f,
    -0.087799f, 0.087090f, 0.250570f, 0.129994f, -0.187383f, -0.209675f, 0.086438f, 0.284428f,
    -0.012586f, -0.212002f, 0.129219f, -0.052674f, 0.323568f, -0.130971f, -0.046830f, -0.097561f,
    0.117373f, -0.018261f, 0.222045f, -0.286801f, 0.009713f, -0.055808f, -0.064132f, 0.036070f,
    -0.104618f, 0.186676f, -0.104279f, -0.110599f, 0.042738f, 0.155462f, -0.081827f, 0.126487f,
    -0.002271f, -0.245844f, 0.199411f, 0.089851f, -0.141757f, 0.142079f, -0.187903f, 0.194291f,
    0.059286f, 0.105401f, -0.222271f, -0.043657f, 0.129532f, 0.223846f, 0.099195f, -0.076848f,
    -0.017326f, -0.274351f, -0.101861f, 0.088656f, 0.017772f, -0.198270f, 0.134543f, 0.062274f,
    0.281898f, 0.036980f, 0.186358f, -0.158498f, -0.024828f, -0.211425f, 0.142921f, 0.260596f,
    -0.105822f, -0.128792f, 0.045051f, 0.096357f, 0.008981f, -0.229158f, -0.231908f, 0.062831f,
    -0.062842f, 0.218779f, -0.134958f, -0.047528f, 0.241858f, -0.054366f, -0.108259f, 0.211429f,
    -0.028994f, -0.102288f, 0.210495f, -0.141129f, -0.182285f, -0.089452f, 0.129984f, 0.198305f,
    0.165275f, -0.088219f, -0.119420f, -0.177265f, -0.228190f, 0.088612f, -0.105822f, 0.042738f,
    -0.129219f, -0.218559f, 0.137839f, 0.124502f, 0.169134f, -0.198305f, 0.142921f, -0.260596f,
    0.046927f, 0.263889f, -0.062835f, -0.174366f, 0.067425f, 0.036070f, 0.194291f, 0.062831f,
    0.285893f, -0.043322f, -0.108861f, -0.005118f, 0.284428f, -0.076848f, 0.211429f, 0.198305f,
};

static const gfloat W_k[1024] = {
    0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f,
    0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f,
    0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f,
    0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f, 0.176777f, -0.176777f,
    -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f,
    -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f,
    -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f,
    -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f, -0.125000f, 0.216506f,
    0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f,
    0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f,
    0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f,
    0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f,
    0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f,
    0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f,
    0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f,
    0.000000f, 0.250000f, 0.000000f, -0.250000f, 0.000000f, 0.250000f, 0.000000f, -0.250000f,
    -0.216506f, -0.125000f, 0.216506f, 0.125000f, -0.216506f, -0.125000f, 0.216506f, 0.125000f,
    -0.216506f, -0.125000f, 0.216506f, 0.125000f, -0.216506f, -0.125000f, 0.216506f, 0.125000f,
    -0.216506f, -0.125000f, 0.216506f, 0.125000f, -0.216506f, -0.125000f, 0.216506f, 0.125000f,
    -0.216506f, -0.125000f, 0.216506f, 0.125000f, -0.216506f, -0.125000f, 0.216506f, 0.125000f,
    0.150000f, -0.150000f, -0.150000f, 0.150000f, 0.150000f, -0.150000f, -0.150000f, 0.150000f,
    0.150000f, -0.150000f, -0.150000f, 0.150000f, 0.150000f, -0.150000f, -0.150000f, 0.150000f,
    0.150000f, -0.150000f, -0.150000f, 0.150000f, 0.150000f, -0.150000f, -0.150000f, 0.150000f,
    0.150000f, -0.150000f, -0.150000f, 0.150000f, 0.150000f, -0.150000f, -0.150000f, 0.150000f,
    0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f,
    0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f,
    0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f,
    0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f,
    -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f,
    -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f,
    -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f,
    -0.100000f, 0.200000f, 0.100000f, -0.200000f, -0.100000f, 0.200000f, 0.100000f, -0.200000f,
};

static const gfloat W_v[1024] = {
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
};

static const gfloat W_o[1024] = {
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
    0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f, 0.000000f,
    0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.176777f,
};

static const gfloat probes_q[8] = { 6.00f, 0.00f, -6.00f, 0.00f, 0.00f, 6.00f, 0.00f, -6.00f };
static const gfloat probes_k[8] = { 6.00f, 0.00f, -6.00f, 0.00f, 0.00f, 6.00f, 0.00f, -6.00f };

static inline void
normalize_desc (gfloat *v, int dim)
{
  gfloat sum_sq = 0.0f;
  for (int i = 0; i < dim; i++)
    sum_sq += v[i] * v[i];
  gfloat norm = sqrtf (sum_sq);
  if (norm > 1e-6f)
    {
      gfloat inv_norm = 1.0f / norm;
      for (int i = 0; i < dim; i++)
        v[i] *= inv_norm;
    }
}

static void
ipa_forward_attention (Fte3600IpaFeatureSet *set)
{
  if (set == NULL || set->n_minutiae <= 1)
    return;
  guint N = set->n_minutiae;
  if (N > FTE3600_IPA_MAX_MINUTIAE)
    N = FTE3600_IPA_MAX_MINUTIAE;

  gfloat Q[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_DESC_DIM];
  gfloat K[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_DESC_DIM];
  gfloat V[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_DESC_DIM];

  gfloat gx_q[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_NUM_PROBES];
  gfloat gy_q[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_NUM_PROBES];
  gfloat gx_k[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_NUM_PROBES];
  gfloat gy_k[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_NUM_PROBES];

  for (guint i = 0; i < N; i++)
    {
      const gfloat *desc = set->minutiae[i].desc;
      gfloat theta = set->minutiae[i].theta;
      gfloat xi = set->minutiae[i].x;
      gfloat yi = set->minutiae[i].y;
      gfloat cos_t = cosf (theta);
      gfloat sin_t = sinf (theta);

      for (int d = 0; d < FTE3600_IPA_DESC_DIM; d++)
        {
          gfloat sum_q = 0.0f, sum_k = 0.0f, sum_v = 0.0f;
          for (int k = 0; k < FTE3600_IPA_DESC_DIM; k++)
            {
              sum_q += desc[k] * W_q[d * FTE3600_IPA_DESC_DIM + k];
              sum_k += desc[k] * W_k[d * FTE3600_IPA_DESC_DIM + k];
              sum_v += desc[k] * W_v[d * FTE3600_IPA_DESC_DIM + k];
            }
          Q[i][d] = sum_q;
          K[i][d] = sum_k;
          V[i][d] = sum_v;
        }

      for (int p = 0; p < FTE3600_IPA_NUM_PROBES; p++)
        {
          gfloat pq_x = probes_q[p * 2];
          gfloat pq_y = probes_q[p * 2 + 1];
          gx_q[i][p] = xi + cos_t * pq_x - sin_t * pq_y;
          gy_q[i][p] = yi + sin_t * pq_x + cos_t * pq_y;

          gfloat pk_x = probes_k[p * 2];
          gfloat pk_y = probes_k[p * 2 + 1];
          gx_k[i][p] = xi + cos_t * pk_x - sin_t * pk_y;
          gy_k[i][p] = yi + sin_t * pk_x + cos_t * pk_y;
        }
    }

  gfloat updated_desc[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_DESC_DIM];

  for (guint i = 0; i < N; i++)
    {
      gfloat logits[FTE3600_IPA_MAX_MINUTIAE];
      gfloat max_logit = -1e9f;

      for (guint j = 0; j < N; j++)
        {
          gfloat dot = 0.0f;
          for (int d = 0; d < FTE3600_IPA_DESC_DIM; d++)
            dot += Q[i][d] * K[j][d];
          gfloat val = dot * IPA_INV_SQRT_D;

          gfloat dist_sq = 0.0f;
          for (int p = 0; p < FTE3600_IPA_NUM_PROBES; p++)
            {
              gfloat dx = gx_q[i][p] - gx_k[j][p];
              gfloat dy = gy_q[i][p] - gy_k[j][p];
              dist_sq += dx * dx + dy * dy;
            }
          val -= (IPA_GAMMA_DIST * 0.5f) * dist_sq;

          gfloat d_theta = set->minutiae[j].theta - set->minutiae[i].theta;
          val += IPA_GAMMA_ANGLE * cosf (d_theta);

          logits[j] = val;
          if (val > max_logit)
            max_logit = val;
        }

      gfloat exp_sum = 0.0f;
      for (guint j = 0; j < N; j++)
        {
          logits[j] = expf (logits[j] - max_logit);
          exp_sum += logits[j];
        }
      gfloat inv_sum = 1.0f / (exp_sum + 1e-8f);

      gfloat h[FTE3600_IPA_DESC_DIM];
      memset (h, 0, sizeof (h));
      for (guint j = 0; j < N; j++)
        {
          gfloat weight = logits[j] * inv_sum;
          for (int d = 0; d < FTE3600_IPA_DESC_DIM; d++)
            h[d] += weight * V[j][d];
        }

      for (int d = 0; d < FTE3600_IPA_DESC_DIM; d++)
        {
          gfloat out_val = 0.0f;
          for (int k = 0; k < FTE3600_IPA_DESC_DIM; k++)
            out_val += h[k] * W_o[d * FTE3600_IPA_DESC_DIM + k];
          updated_desc[i][d] = set->minutiae[i].desc[d] + out_val;
        }
      normalize_desc (updated_desc[i], FTE3600_IPA_DESC_DIM);
    }

  for (guint i = 0; i < N; i++)
    memcpy (set->minutiae[i].desc, updated_desc[i], sizeof (gfloat) * FTE3600_IPA_DESC_DIM);
}

Fte3600IpaStatus
fpi_fte3600_ipa_extract (const guint8         *image,
                         gsize                 length,
                         Fte3600IpaFeatureSet *features)
{
  if (image == NULL || features == NULL || length < FTE3600_IPA_IMAGE_SIZE)
    return FTE3600_IPA_ERR_PARAM;

  features->n_minutiae = 0;

  const int width = FTE3600_IPA_WIDTH;
  const int height = FTE3600_IPA_HEIGHT;
  const int margin = 6;
  const gfloat harris_thresh = 180.0f;

  static gfloat s_gx[FTE3600_IPA_IMAGE_SIZE];
  static gfloat s_gy[FTE3600_IPA_IMAGE_SIZE];
  static gfloat s_sxx[FTE3600_IPA_IMAGE_SIZE];
  static gfloat s_syy[FTE3600_IPA_IMAGE_SIZE];
  static gfloat s_sxy[FTE3600_IPA_IMAGE_SIZE];
  static gfloat s_harris[FTE3600_IPA_IMAGE_SIZE];

  /* 1. Sobel Gradients */
  for (int y = 1; y < height - 1; y++)
    {
      for (int x = 1; x < width - 1; x++)
        {
          int gx = (image[(y - 1) * width + (x + 1)] - image[(y - 1) * width + (x - 1)]) +
                   2 * (image[y * width + (x + 1)] - image[y * width + (x - 1)]) +
                   (image[(y + 1) * width + (x + 1)] - image[(y + 1) * width + (x - 1)]);

          int gy = (image[(y + 1) * width + (x - 1)] - image[(y - 1) * width + (x - 1)]) +
                   2 * (image[(y + 1) * width + x] - image[(y - 1) * width + x]) +
                   (image[(y + 1) * width + (x + 1)] - image[(y - 1) * width + (x + 1)]);

          int p_idx = y * width + x;
          s_gx[p_idx] = (gfloat) gx;
          s_gy[p_idx] = (gfloat) gy;
        }
    }

  /* 2. 5x5 Box-Smoothed Structure Tensor */
  for (int y = 3; y < height - 3; y++)
    {
      for (int x = 3; x < width - 3; x++)
        {
          gfloat sum_xx = 0.0f, sum_yy = 0.0f, sum_xy = 0.0f;
          for (int dy = -2; dy <= 2; dy++)
            {
              for (int dx = -2; dx <= 2; dx++)
                {
                  int p_idx = (y + dy) * width + (x + dx);
                  gfloat gx = s_gx[p_idx];
                  gfloat gy = s_gy[p_idx];
                  sum_xx += gx * gx;
                  sum_yy += gy * gy;
                  sum_xy += gx * gy;
                }
            }
          int p_idx = y * width + x;
          s_sxx[p_idx] = sum_xx / 25.0f;
          s_syy[p_idx] = sum_yy / 25.0f;
          s_sxy[p_idx] = sum_xy / 25.0f;

          gfloat det = s_sxx[p_idx] * s_syy[p_idx] - s_sxy[p_idx] * s_sxy[p_idx];
          gfloat trace = s_sxx[p_idx] + s_syy[p_idx];
          s_harris[p_idx] = det - 0.04f * (trace * trace);
        }
    }

  /* 3. Non-Maximum Suppression (7x7 window) */
  typedef struct {
    gfloat score;
    int x, y;
    gfloat theta;
  } Candidate;
  Candidate candidates[250];
  int n_cands = 0;

  for (int y = margin; y < height - margin; y++)
    {
      for (int x = margin; x < width - margin; x++)
        {
          int p_idx = y * width + x;
          gfloat val = s_harris[p_idx];
          if (val <= harris_thresh)
            continue;

          /* Orientation coherence check to reject isotropic/flat background noise */
          gfloat diff_sq = (s_sxx[p_idx] - s_syy[p_idx]) * (s_sxx[p_idx] - s_syy[p_idx]);
          gfloat cross_sq = 4.0f * s_sxy[p_idx] * s_sxy[p_idx];
          gfloat trace_sq = (s_sxx[p_idx] + s_syy[p_idx]) * (s_sxx[p_idx] + s_syy[p_idx]);
          if (trace_sq > 1e-4f && (diff_sq + cross_sq) < 0.04f * trace_sq)
            continue; /* coherence < 0.20f */

          int is_local_max = 1;
          for (int dy = -3; dy <= 3 && is_local_max; dy++)
            {
              for (int dx = -3; dx <= 3; dx++)
                {
                  if (dy == 0 && dx == 0) continue;
                  if (s_harris[(y + dy) * width + (x + dx)] > val)
                    {
                      is_local_max = 0;
                      break;
                    }
                }
            }

          if (is_local_max && n_cands < 250)
            {
              gfloat theta = 0.5f * atan2f (2.0f * s_sxy[p_idx], s_sxx[p_idx] - s_syy[p_idx]) + (M_PI * 0.5f);
              while (theta > M_PI) theta -= 2.0f * M_PI;
              while (theta < -M_PI) theta += 2.0f * M_PI;

              candidates[n_cands].score = val;
              candidates[n_cands].x = x;
              candidates[n_cands].y = y;
              candidates[n_cands].theta = theta;
              n_cands++;
            }
        }
    }

  for (int i = 0; i < n_cands - 1; i++)
    {
      for (int j = i + 1; j < n_cands; j++)
        {
          if (candidates[j].score > candidates[i].score)
            {
              Candidate tmp = candidates[i];
              candidates[i] = candidates[j];
              candidates[j] = tmp;
            }
        }
    }

  /* 4. Spatial Grid Bucketing: 4x5 cells of 16x16 pixels */
  Candidate selected[FTE3600_IPA_MAX_MINUTIAE];
  guint n_selected = 0;
  guint cell_counts[4][5] = { { 0 } };
  guint8 used[250] = { 0 };

  /* First pass: take up to 2 highest-scoring candidates per 16x16 cell */
  for (int i = 0; i < n_cands && n_selected < FTE3600_IPA_MAX_MINUTIAE; i++)
    {
      int cell_x = candidates[i].x / 16;
      int cell_y = candidates[i].y / 16;
      if (cell_x >= 4) cell_x = 3;
      if (cell_y >= 5) cell_y = 4;

      if (cell_counts[cell_x][cell_y] < 2)
        {
          selected[n_selected++] = candidates[i];
          cell_counts[cell_x][cell_y]++;
          used[i] = 1;
        }
    }

  /* Second pass: if under budget, fill from remaining highest scoring candidates */
  for (int i = 0; i < n_cands && n_selected < FTE3600_IPA_MAX_MINUTIAE; i++)
    {
      if (!used[i])
        {
          selected[n_selected++] = candidates[i];
          used[i] = 1;
        }
    }

  features->n_minutiae = n_selected;

  for (guint i = 0; i < n_selected; i++)
    {
      Fte3600IpaMinutia *m = &features->minutiae[i];
      m->x = (gfloat) selected[i].x;
      m->y = (gfloat) selected[i].y;
      m->theta = selected[i].theta;

      gfloat cos_t = cosf (m->theta);
      gfloat sin_t = sinf (m->theta);

      gfloat ring_radii[3] = { 4.0f, 7.0f, 10.0f };
      int ring_points[3]  = { 8, 16, 8 };
      int desc_idx = 0;
      gfloat desc_sum = 0.0f;

      for (int r_idx = 0; r_idx < 3; r_idx++)
        {
          gfloat r = ring_radii[r_idx];
          int n_pts = ring_points[r_idx];
          for (int k = 0; k < n_pts; k++)
            {
              gfloat phi = k * (2.0f * M_PI / (gfloat) n_pts);
              gfloat lx = r * cosf (phi);
              gfloat ly = r * sinf (phi);

              int px = (int) roundf (m->x + cos_t * lx - sin_t * ly);
              int py = (int) roundf (m->y + sin_t * lx + cos_t * ly);

              gfloat p_val = 128.0f;
              if (px >= 0 && px < width && py >= 0 && py < height)
                p_val = (gfloat) image[py * width + px];
              m->desc[desc_idx] = p_val;
              desc_sum += p_val;
              desc_idx++;
            }
        }

      gfloat desc_mean = desc_sum / (gfloat) FTE3600_IPA_DESC_DIM;
      for (int d = 0; d < FTE3600_IPA_DESC_DIM; d++)
        m->desc[d] -= desc_mean;
      normalize_desc (m->desc, FTE3600_IPA_DESC_DIM);
    }

  return (features->n_minutiae >= 3) ? FTE3600_IPA_OK : FTE3600_IPA_ERR_TOO_FEW_POINTS;
}

static inline gfloat
diff_angle_pi (gfloat th1, gfloat th2)
{
  gfloat d2 = 2.0f * (th1 - th2);
  return 0.5f * atan2f (sinf (d2), cosf (d2));
}

Fte3600IpaStatus
fpi_fte3600_ipa_match (const Fte3600IpaFeatureSet *query,
                       const Fte3600IpaFeatureSet *reference,
                       Fte3600IpaMatchResult      *result)
{
  if (query == NULL || reference == NULL || result == NULL)
    return FTE3600_IPA_ERR_PARAM;

  memset (result, 0, sizeof (*result));

  if (query->n_minutiae < 3 || reference->n_minutiae < 3)
    return FTE3600_IPA_OK;

  Fte3600IpaFeatureSet q_ctx = *query;
  Fte3600IpaFeatureSet r_ctx = *reference;

  ipa_forward_attention (&q_ctx);
  ipa_forward_attention (&r_ctx);

  guint N = q_ctx.n_minutiae;
  guint M = r_ctx.n_minutiae;

  gfloat S[FTE3600_IPA_MAX_MINUTIAE][FTE3600_IPA_MAX_MINUTIAE];
  for (guint i = 0; i < N; i++)
    {
      for (guint j = 0; j < M; j++)
        {
          gfloat dot = 0.0f;
          for (int d = 0; d < FTE3600_IPA_DESC_DIM; d++)
            dot += q_ctx.minutiae[i].desc[d] * r_ctx.minutiae[j].desc[d];
          S[i][j] = dot;
        }
    }

  typedef struct {
    guint i, j;
    gfloat sim;
  } MatchPair;
  MatchPair matches[FTE3600_IPA_MAX_MINUTIAE];
  guint n_matches = 0;

  for (guint i = 0; i < N; i++)
    {
      int best_j = -1;
      gfloat best_sim = -1.0f;
      gfloat second_sim = -1.0f;
      for (guint j = 0; j < M; j++)
        {
          if (S[i][j] > best_sim)
            {
              second_sim = best_sim;
              best_sim = S[i][j];
              best_j = j;
            }
          else if (S[i][j] > second_sim)
            {
              second_sim = S[i][j];
            }
        }

      /* MNN with Lowe's ratio margin check (margin >= 0.01) */
      if (best_j >= 0 && best_sim >= 0.36f && (best_sim - second_sim >= 0.01f))
        {
          int rev_i = -1;
          gfloat rev_sim = -1.0f;
          for (guint k = 0; k < N; k++)
            {
              if (S[k][best_j] > rev_sim)
                {
                  rev_sim = S[k][best_j];
                  rev_i = k;
                }
            }
          if (rev_i == (int) i)
            {
              matches[n_matches].i = i;
              matches[n_matches].j = (guint) best_j;
              matches[n_matches].sim = best_sim;
              n_matches++;
            }
        }
    }

  result->n_matched_pairs = n_matches;
  if (n_matches < 4)
    return FTE3600_IPA_OK;

  int supported[FTE3600_IPA_MAX_MINUTIAE] = { 0 };

  for (guint m1 = 0; m1 < n_matches; m1++)
    {
      guint i1 = matches[m1].i;
      guint j1 = matches[m1].j;
      guint support_count = 0;

      for (guint m2 = 0; m2 < n_matches; m2++)
        {
          if (m1 == m2) continue;
          guint i2 = matches[m2].i;
          guint j2 = matches[m2].j;

          gfloat dx1 = q_ctx.minutiae[i1].x - q_ctx.minutiae[i2].x;
          gfloat dy1 = q_ctx.minutiae[i1].y - q_ctx.minutiae[i2].y;
          gfloat d1 = sqrtf (dx1 * dx1 + dy1 * dy1);

          gfloat dx2 = r_ctx.minutiae[j1].x - r_ctx.minutiae[j2].x;
          gfloat dy2 = r_ctx.minutiae[j1].y - r_ctx.minutiae[j2].y;
          gfloat d2 = sqrtf (dx2 * dx2 + dy2 * dy2);

          gfloat max_d = d1 > d2 ? d1 : d2;
          if (fabsf (d1 - d2) > (4.0f + 0.12f * max_d))
            continue;

          gfloat dth1 = q_ctx.minutiae[i1].theta - q_ctx.minutiae[i2].theta;
          gfloat dth2 = r_ctx.minutiae[j1].theta - r_ctx.minutiae[j2].theta;
          gfloat dth_diff = fabsf (atan2f (sinf (dth1 - dth2), cosf (dth1 - dth2)));
          if (dth_diff > 0.60f)
            continue;

          /* SE(2) Vector Bearing Consistency Check */
          gfloat b1 = atan2f (dy1, dx1);
          gfloat b2 = atan2f (dy2, dx2);
          gfloat delta_bearing = atan2f (sinf (b1 - b2), cosf (b1 - b2));
          gfloat rot1 = diff_angle_pi (r_ctx.minutiae[j1].theta, q_ctx.minutiae[i1].theta);
          gfloat rot2 = diff_angle_pi (q_ctx.minutiae[i2].theta, r_ctx.minutiae[j2].theta);
          gfloat delta_rot = 0.5f * (rot1 + rot2);
          gfloat bearing_error = fabsf (atan2f (sinf (delta_bearing - delta_rot), cosf (delta_bearing - delta_rot)));
          if (bearing_error <= 0.40f)
            support_count++;
        }

      if (support_count >= 2)
        supported[m1] = 1;
    }

  /* Fast Global Rigid Cluster Verification (Reprojection <= 4.5 px) */
  guint max_cluster_inliers = 0;
  gfloat best_sim_sum = 0.0f;
  guint best_inlier_indices[FTE3600_IPA_MAX_MINUTIAE];
  guint n_best = 0;

  for (guint m1 = 0; m1 < n_matches; m1++)
    {
      if (!supported[m1]) continue;
      guint i1 = matches[m1].i, j1 = matches[m1].j;
      gfloat rot = diff_angle_pi (r_ctx.minutiae[j1].theta, q_ctx.minutiae[i1].theta);
      gfloat cos_r = cosf (rot), sin_r = sinf (rot);

      gfloat tx = r_ctx.minutiae[j1].x - (cos_r * q_ctx.minutiae[i1].x - sin_r * q_ctx.minutiae[i1].y);
      gfloat ty = r_ctx.minutiae[j1].y - (sin_r * q_ctx.minutiae[i1].x + cos_r * q_ctx.minutiae[i1].y);

      guint cluster_inl = 0;
      gfloat cluster_sim = 0.0f;
      guint cur_indices[FTE3600_IPA_MAX_MINUTIAE];

      for (guint m2 = 0; m2 < n_matches; m2++)
        {
          if (!supported[m2]) continue;
          guint i2 = matches[m2].i, j2 = matches[m2].j;
          gfloat pred_x = cos_r * q_ctx.minutiae[i2].x - sin_r * q_ctx.minutiae[i2].y + tx;
          gfloat pred_y = sin_r * q_ctx.minutiae[i2].x + cos_r * q_ctx.minutiae[i2].y + ty;
          gfloat err = hypotf (r_ctx.minutiae[j2].x - pred_x, r_ctx.minutiae[j2].y - pred_y);
          if (err <= 4.5f)
            {
              cur_indices[cluster_inl] = m2;
              cluster_inl++;
              cluster_sim += matches[m2].sim;
            }
        }
      if (cluster_inl > max_cluster_inliers ||
          (cluster_inl == max_cluster_inliers && cluster_sim > best_sim_sum))
        {
          max_cluster_inliers = cluster_inl;
          best_sim_sum = cluster_sim;
          n_best = cluster_inl;
          for (guint k = 0; k < cluster_inl; k++)
            best_inlier_indices[k] = cur_indices[k];
        }
    }

  if (max_cluster_inliers >= 4)
    {
      gfloat min_x = 999.0f, max_x = -999.0f, min_y = 999.0f, max_y = -999.0f;
      for (guint k = 0; k < n_best; k++)
        {
          guint p_idx = best_inlier_indices[k];
          gfloat x = q_ctx.minutiae[matches[p_idx].i].x;
          gfloat y = q_ctx.minutiae[matches[p_idx].i].y;
          if (x < min_x) min_x = x;
          if (x > max_x) max_x = x;
          if (y < min_y) min_y = y;
          if (y > max_y) max_y = y;
        }
      result->x_span = max_x - min_x;
      result->y_span = max_y - min_y;
      gfloat raw_score = best_sim_sum / (gfloat) (max_cluster_inliers + 1);
      if (raw_score > 1.0f)
        raw_score = 1.0f;

      /* Overlap-aware consensus score: penalize excessive unmatched candidates in overlap span */
      guint overlap_query_pts = 0;
      for (guint i = 0; i < N; i++)
        {
          if (q_ctx.minutiae[i].x >= min_x - 1.5f && q_ctx.minutiae[i].x <= max_x + 1.5f &&
              q_ctx.minutiae[i].y >= min_y - 1.5f && q_ctx.minutiae[i].y <= max_y + 1.5f)
            overlap_query_pts++;
        }
      guint unmatched_overlap = overlap_query_pts > max_cluster_inliers ? (overlap_query_pts - max_cluster_inliers) : 0;
      gfloat penalty_factor = 1.0f / (1.0f + 0.04f * (gfloat) unmatched_overlap);
      result->consensus_score = raw_score * penalty_factor;
      result->n_supported_inliers = max_cluster_inliers;
    }
  else
    {
      result->n_supported_inliers = max_cluster_inliers;
      result->consensus_score = 0.0f;
    }

  result->authentication_accepted = fpi_fte3600_ipa_result_meets_policy (result);
  return FTE3600_IPA_OK;
}

gboolean
fpi_fte3600_ipa_result_meets_policy (const Fte3600IpaMatchResult *result)
{
  if (result == NULL)
    return FALSE;

  /* Gate 1: 5 or more rigid consensus inliers */
  if (result->n_supported_inliers >= FTE3600_IPA_POLICY_RELAXED_INLIERS &&
      result->consensus_score >= FTE3600_IPA_POLICY_RELAXED_SCORE)
    return TRUE;

  /* Gate 2: 4 rigid inliers with spatial dispersion */
  if (result->n_supported_inliers >= FTE3600_IPA_POLICY_MIN_INLIERS &&
      result->consensus_score >= FTE3600_IPA_POLICY_MIN_SCORE &&
      result->x_span >= FTE3600_IPA_POLICY_MIN_SPAN_X &&
      result->y_span >= FTE3600_IPA_POLICY_MIN_SPAN_Y)
    return TRUE;

  return FALSE;
}
