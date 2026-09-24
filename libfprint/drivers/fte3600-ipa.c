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

/* Schema v2 adds local contrast normalization and sub-pixel locations to
 * the v1 design. Both use a complete, reproducible orthonormal H32 transform for
 * Q, K, V and O. There are no learned or partially initialized coefficients.
 * This changes the experimental model; unversioned Wire V2 is not accepted. */
gfloat
fpi_fte3600_ipa_projection_coefficient (guint row, guint column)
{
  if (row >= FTE3600_IPA_DESC_DIM || column >= FTE3600_IPA_DESC_DIM)
    return 0.0f;
  return (__builtin_popcount (row & column) & 1) ?
         -IPA_INV_SQRT_D : IPA_INV_SQRT_D;
}

static void
ipa_secure_clear (gpointer data, gsize size)
{
  volatile guint8 *bytes = data;

  while (size-- > 0)
    *bytes++ = 0;
}

typedef struct
{
  gfloat gx[FTE3600_IPA_IMAGE_SIZE];
  gfloat gy[FTE3600_IPA_IMAGE_SIZE];
  gfloat sxx[FTE3600_IPA_IMAGE_SIZE];
  gfloat syy[FTE3600_IPA_IMAGE_SIZE];
  gfloat sxy[FTE3600_IPA_IMAGE_SIZE];
  gfloat harris[FTE3600_IPA_IMAGE_SIZE];
  guint8 norm_image[FTE3600_IPA_IMAGE_SIZE];
  gint32 sat1[FTE3600_IPA_HEIGHT + 1][FTE3600_IPA_WIDTH + 1];
  guint32 sat2[FTE3600_IPA_HEIGHT + 1][FTE3600_IPA_WIDTH + 1];
} IpaExtractWorkspace;

static void
ipa_extract_workspace_free (IpaExtractWorkspace *workspace)
{
  if (workspace == NULL)
    return;
  ipa_secure_clear (workspace, sizeof (*workspace));
  g_free (workspace);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (IpaExtractWorkspace, ipa_extract_workspace_free)

static void
ipa_feature_set_clear (Fte3600IpaFeatureSet *features)
{
  ipa_secure_clear (features, sizeof (*features));
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (Fte3600IpaFeatureSet, ipa_feature_set_clear)

gboolean
fpi_fte3600_ipa_validate_feature_set (const Fte3600IpaFeatureSet *features)
{
  if (features == NULL ||
      features->extractor_schema_version != FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION ||
      features->n_minutiae > FTE3600_IPA_MAX_MINUTIAE)
    return FALSE;

  for (guint i = 0; i < features->n_minutiae; i++)
    {
      const Fte3600IpaMinutia *point = &features->minutiae[i];
      gdouble norm = 0.0;

      if (!isfinite (point->x) || point->x < 0.0f || point->x >= FTE3600_IPA_WIDTH ||
          !isfinite (point->y) || point->y < 0.0f || point->y >= FTE3600_IPA_HEIGHT ||
          !isfinite (point->theta) ||
          fabsf (point->theta) > FTE3600_IPA_ORIENTATION_LIMIT)
        return FALSE;
      for (guint d = 0; d < FTE3600_IPA_DESC_DIM; d++)
        {
          if (!isfinite (point->desc[d]) || fabsf (point->desc[d]) > 1.0001f)
            return FALSE;
          norm += (gdouble) point->desc[d] * point->desc[d];
        }
      /* Descriptors are normalized, not arbitrary numeric vectors. */
      if (fabs (norm - 1.0) > 0.001)
        return FALSE;
      for (guint j = 0; j < i; j++)
        if (point->x == features->minutiae[j].x &&
            point->y == features->minutiae[j].y)
          return FALSE;
    }
  return TRUE;
}

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
              const gfloat coefficient = fpi_fte3600_ipa_projection_coefficient (d, k);
              sum_q += desc[k] * coefficient;
              sum_k += desc[k] * coefficient;
              sum_v += desc[k] * coefficient;
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
          gfloat flipped_dist_sq = 0.0f;
          for (int p = 0; p < FTE3600_IPA_NUM_PROBES; p++)
            {
              gfloat dx = gx_q[i][p] - gx_k[j][p];
              gfloat dy = gy_q[i][p] - gy_k[j][p];
              gfloat flipped_dx = gx_q[i][p] - (2.0f * set->minutiae[j].x - gx_k[j][p]);
              gfloat flipped_dy = gy_q[i][p] - (2.0f * set->minutiae[j].y - gy_k[j][p]);
              dist_sq += dx * dx + dy * dy;
              flipped_dist_sq += flipped_dx * flipped_dx + flipped_dy * flipped_dy;
            }
          dist_sq = MIN (dist_sq, flipped_dist_sq);
          val -= (IPA_GAMMA_DIST * 0.5f) * dist_sq;

          gfloat d_theta = set->minutiae[j].theta - set->minutiae[i].theta;
          val += IPA_GAMMA_ANGLE * cosf (2.0f * d_theta);

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
            out_val += h[k] * fpi_fte3600_ipa_projection_coefficient (d, k);
          updated_desc[i][d] = set->minutiae[i].desc[d] + out_val;
        }
      normalize_desc (updated_desc[i], FTE3600_IPA_DESC_DIM);
      ipa_secure_clear (h, sizeof (h));
      ipa_secure_clear (logits, sizeof (logits));
    }

  for (guint i = 0; i < N; i++)
    memcpy (set->minutiae[i].desc, updated_desc[i], sizeof (gfloat) * FTE3600_IPA_DESC_DIM);
  ipa_secure_clear (Q, sizeof (Q));
  ipa_secure_clear (K, sizeof (K));
  ipa_secure_clear (V, sizeof (V));
  ipa_secure_clear (gx_q, sizeof (gx_q));
  ipa_secure_clear (gy_q, sizeof (gy_q));
  ipa_secure_clear (gx_k, sizeof (gx_k));
  ipa_secure_clear (gy_k, sizeof (gy_k));
  ipa_secure_clear (updated_desc, sizeof (updated_desc));
}

static void
normalize_image_contrast (const guint8 *src,
                          guint8       *dst,
                          int           width,
                          int           height,
                          IpaExtractWorkspace *workspace)
{
  const int r = 6; /* 13x13 local window (~1.5 ridge wavelengths) */

  for (int x = 0; x <= width; x++)
    {
      workspace->sat1[0][x] = 0;
      workspace->sat2[0][x] = 0;
    }

  for (int y = 0; y < height; y++)
    {
      workspace->sat1[y + 1][0] = 0;
      workspace->sat2[y + 1][0] = 0;
      gint32 row_sum1 = 0;
      guint32 row_sum2 = 0;
      for (int x = 0; x < width; x++)
        {
          gint32 val = (gint32) src[y * width + x] - 128;
          row_sum1 += val;
          row_sum2 += (guint32) (val * val);
          workspace->sat1[y + 1][x + 1] = workspace->sat1[y][x + 1] + row_sum1;
          workspace->sat2[y + 1][x + 1] = workspace->sat2[y][x + 1] + row_sum2;
        }
    }

  for (int y = 0; y < height; y++)
    {
      int y0 = (y - r < 0) ? 0 : y - r;
      int y1 = (y + r >= height) ? height - 1 : y + r;
      for (int x = 0; x < width; x++)
        {
          int x0 = (x - r < 0) ? 0 : x - r;
          int x1 = (x + r >= width) ? width - 1 : x + r;
          int area = (x1 - x0 + 1) * (y1 - y0 + 1);

          gint32 sum1 = workspace->sat1[y1 + 1][x1 + 1] - workspace->sat1[y0][x1 + 1] - workspace->sat1[y1 + 1][x0] + workspace->sat1[y0][x0];
          guint32 sum2 = workspace->sat2[y1 + 1][x1 + 1] - workspace->sat2[y0][x1 + 1] - workspace->sat2[y1 + 1][x0] + workspace->sat2[y0][x0];

          gfloat mean = (gfloat) sum1 / (gfloat) area;
          gfloat variance = ((gfloat) sum2 / (gfloat) area) - (mean * mean);
          gfloat std_dev = variance > 0.0f ? sqrtf (variance) : 0.0f;

          int p_idx = y * width + x;
          gint32 cur_val = (gint32) src[p_idx] - 128;

          if (std_dev < 1.0f)
            {
              dst[p_idx] = 128;
            }
          else
            {
              gfloat norm_val = 128.0f + 36.0f * ((gfloat) cur_val - mean) / (std_dev + 4.0f);
              dst[p_idx] = (guint8) CLAMP ((int) roundf (norm_val), 0, 255);
            }
        }
    }
}

Fte3600IpaStatus
fpi_fte3600_ipa_extract (const guint8         *image,
                         gsize                 length,
                         Fte3600IpaFeatureSet *features)
{
  if (features != NULL)
    memset (features, 0, sizeof (*features));
  if (image == NULL || features == NULL || length != FTE3600_IPA_IMAGE_SIZE)
    return FTE3600_IPA_ERR_PARAM;

  features->extractor_schema_version = FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION;
  g_autoptr(IpaExtractWorkspace) workspace = g_new0 (IpaExtractWorkspace, 1);

  const int width = FTE3600_IPA_WIDTH;
  const int height = FTE3600_IPA_HEIGHT;
  const int margin = 6;
  const gfloat harris_thresh = 180.0f;


  /* 0. Integral Image Local Contrast Normalization */
  normalize_image_contrast (image, workspace->norm_image, width, height, workspace);

  /* 1. Sobel Gradients on Contrast-Normalized Frame */
  for (int y = 1; y < height - 1; y++)
    {
      for (int x = 1; x < width - 1; x++)
        {
          int gx = (workspace->norm_image[(y - 1) * width + (x + 1)] - workspace->norm_image[(y - 1) * width + (x - 1)]) +
                   2 * (workspace->norm_image[y * width + (x + 1)] - workspace->norm_image[y * width + (x - 1)]) +
                   (workspace->norm_image[(y + 1) * width + (x + 1)] - workspace->norm_image[(y + 1) * width + (x - 1)]);

          int gy = (workspace->norm_image[(y + 1) * width + (x - 1)] - workspace->norm_image[(y - 1) * width + (x - 1)]) +
                   2 * (workspace->norm_image[(y + 1) * width + x] - workspace->norm_image[(y - 1) * width + x]) +
                   (workspace->norm_image[(y + 1) * width + (x + 1)] - workspace->norm_image[(y - 1) * width + (x + 1)]);

          int p_idx = y * width + x;
          workspace->gx[p_idx] = (gfloat) gx;
          workspace->gy[p_idx] = (gfloat) gy;
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
                  gfloat gx = workspace->gx[p_idx];
                  gfloat gy = workspace->gy[p_idx];
                  sum_xx += gx * gx;
                  sum_yy += gy * gy;
                  sum_xy += gx * gy;
                }
            }
          int p_idx = y * width + x;
          workspace->sxx[p_idx] = sum_xx / 25.0f;
          workspace->syy[p_idx] = sum_yy / 25.0f;
          workspace->sxy[p_idx] = sum_xy / 25.0f;

          gfloat det = workspace->sxx[p_idx] * workspace->syy[p_idx] - workspace->sxy[p_idx] * workspace->sxy[p_idx];
          gfloat trace = workspace->sxx[p_idx] + workspace->syy[p_idx];
          workspace->harris[p_idx] = det - 0.04f * (trace * trace);
        }
    }

  /* 3. Non-Maximum Suppression (7x7 window) with Sub-Pixel Refinement */
  typedef struct {
    gfloat score;
    gfloat x, y;
    gfloat theta;
  } Candidate;
  Candidate candidates[250];
  int n_cands = 0;

  for (int y = margin; y < height - margin; y++)
    {
      for (int x = margin; x < width - margin; x++)
        {
          int p_idx = y * width + x;
          gfloat val = workspace->harris[p_idx];
          if (val <= harris_thresh)
            continue;

          /* Orientation coherence check to reject isotropic/flat background noise */
          gfloat diff_sq = (workspace->sxx[p_idx] - workspace->syy[p_idx]) * (workspace->sxx[p_idx] - workspace->syy[p_idx]);
          gfloat cross_sq = 4.0f * workspace->sxy[p_idx] * workspace->sxy[p_idx];
          gfloat trace_sq = (workspace->sxx[p_idx] + workspace->syy[p_idx]) * (workspace->sxx[p_idx] + workspace->syy[p_idx]);
          if (trace_sq > 1e-4f && (diff_sq + cross_sq) < 0.04f * trace_sq)
            continue; /* coherence < 0.20f */

          int is_local_max = 1;
          for (int dy = -3; dy <= 3 && is_local_max; dy++)
            {
              for (int dx = -3; dx <= 3; dx++)
                {
                  if (dy == 0 && dx == 0) continue;
                  if (workspace->harris[(y + dy) * width + (x + dx)] > val)
                    {
                      is_local_max = 0;
                      break;
                    }
                }
            }

          if (is_local_max && n_cands < 250)
            {
              gfloat theta = 0.5f * atan2f (2.0f * workspace->sxy[p_idx], workspace->sxx[p_idx] - workspace->syy[p_idx]) + (M_PI * 0.5f);
              while (theta > M_PI) theta -= 2.0f * M_PI;
              while (theta < -M_PI) theta += 2.0f * M_PI;

              /* Sub-pixel quadratic peak interpolation */
              gfloat h_xm = workspace->harris[y * width + (x - 1)];
              gfloat h_xp = workspace->harris[y * width + (x + 1)];
              gfloat h_ym = workspace->harris[(y - 1) * width + x];
              gfloat h_yp = workspace->harris[(y + 1) * width + x];

              gfloat denom_x = 2.0f * (h_xm - 2.0f * val + h_xp);
              gfloat denom_y = 2.0f * (h_ym - 2.0f * val + h_yp);

              gfloat delta_x = 0.0f;
              gfloat delta_y = 0.0f;

              if (fabsf (denom_x) > 1e-5f)
                {
                  delta_x = (h_xm - h_xp) / denom_x;
                  delta_x = CLAMP (delta_x, -0.5f, 0.5f);
                }
              if (fabsf (denom_y) > 1e-5f)
                {
                  delta_y = (h_ym - h_yp) / denom_y;
                  delta_y = CLAMP (delta_y, -0.5f, 0.5f);
                }

              candidates[n_cands].score = val;
              candidates[n_cands].x = (gfloat) x + delta_x;
              candidates[n_cands].y = (gfloat) y + delta_y;
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
      int cell_x = (int) candidates[i].x / 16;
      int cell_y = (int) candidates[i].y / 16;
      if (cell_x < 0) cell_x = 0;
      if (cell_x >= 4) cell_x = 3;
      if (cell_y < 0) cell_y = 0;
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

  features->n_minutiae = 0;

  for (guint i = 0; i < n_selected; i++)
    {
      Fte3600IpaMinutia *m = &features->minutiae[features->n_minutiae];
      m->x = selected[i].x;
      m->y = selected[i].y;
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
      gfloat norm = 0.0f;
      for (guint d = 0; d < FTE3600_IPA_DESC_DIM; d++)
        norm += m->desc[d] * m->desc[d];
      if (norm > 0.5f)
        features->n_minutiae++;
      else
        ipa_secure_clear (m, sizeof (*m));
    }

  ipa_secure_clear (candidates, sizeof (candidates));
  ipa_secure_clear (selected, sizeof (selected));
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
  if (result != NULL)
    memset (result, 0, sizeof (*result));
  if (result == NULL ||
      !fpi_fte3600_ipa_validate_feature_set (query) ||
      !fpi_fte3600_ipa_validate_feature_set (reference))
    return FTE3600_IPA_ERR_PARAM;

  if (query->n_minutiae < 3 || reference->n_minutiae < 3)
    return FTE3600_IPA_OK;

  g_auto(Fte3600IpaFeatureSet) q_ctx = *query;
  g_auto(Fte3600IpaFeatureSet) r_ctx = *reference;

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
          gfloat dth_diff = fabsf (diff_angle_pi (dth1, dth2));
          if (dth_diff > 0.60f)
            continue;

          /* SE(2) Vector Bearing Consistency Check */
          gfloat b1 = atan2f (dy1, dx1);
          gfloat b2 = atan2f (dy2, dx2);
          gfloat delta_bearing = atan2f (sinf (b1 - b2), cosf (b1 - b2));
          gfloat rot1 = diff_angle_pi (q_ctx.minutiae[i1].theta, r_ctx.minutiae[j1].theta);
          gfloat rot2 = diff_angle_pi (q_ctx.minutiae[i2].theta, r_ctx.minutiae[j2].theta);
          /* Both rotations map reference to query. Average axial angles on
           * the doubled circle rather than cancelling opposite directions. */
          gfloat delta_rot = 0.5f * atan2f (sinf (2.0f * rot1) + sinf (2.0f * rot2),
                                          cosf (2.0f * rot1) + cosf (2.0f * rot2));
          gfloat bearing_error = fabsf (diff_angle_pi (delta_bearing, delta_rot));
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

  /* 1. Evaluate single-minutia rotation hypotheses */
  for (guint m1 = 0; m1 < n_matches; m1++)
    {
      if (!supported[m1]) continue;
      guint i1 = matches[m1].i, j1 = matches[m1].j;
      /* Ridge orientations are axial. Test both directed rotations so that
       * an equivalent theta + pi does not discard the true rigid transform. */
      const gfloat axial_rot =
        diff_angle_pi (r_ctx.minutiae[j1].theta, q_ctx.minutiae[i1].theta);
      for (guint half_turn = 0; half_turn < 2; half_turn++)
        {
          gfloat rot = axial_rot + half_turn * (gfloat) M_PI;
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
    }

  /* 2. Evaluate two-point spatial vector rotation hypotheses */
  for (guint m1 = 0; m1 < n_matches; m1++)
    {
      if (!supported[m1]) continue;
      guint i1 = matches[m1].i, j1 = matches[m1].j;

      for (guint m2 = m1 + 1; m2 < n_matches; m2++)
        {
          if (!supported[m2]) continue;
          guint i2 = matches[m2].i, j2 = matches[m2].j;

          gfloat dx1 = q_ctx.minutiae[i2].x - q_ctx.minutiae[i1].x;
          gfloat dy1 = q_ctx.minutiae[i2].y - q_ctx.minutiae[i1].y;
          gfloat d1 = hypotf (dx1, dy1);
          if (d1 < 12.0f)
            continue;

          gfloat dx2 = r_ctx.minutiae[j2].x - r_ctx.minutiae[j1].x;
          gfloat dy2 = r_ctx.minutiae[j2].y - r_ctx.minutiae[j1].y;
          gfloat d2 = hypotf (dx2, dy2);
          if (d2 < 12.0f || fabsf (d1 - d2) > (3.0f + 0.08f * d1))
            continue;

          /* Spatial vector rotation from relative bearings */
          gfloat b1 = atan2f (dy1, dx1);
          gfloat b2 = atan2f (dy2, dx2);
          gfloat rot = atan2f (sinf (b2 - b1), cosf (b2 - b1));

          /* Consistency check with local orientation differences */
          gfloat rot1 = diff_angle_pi (r_ctx.minutiae[j1].theta, q_ctx.minutiae[i1].theta);
          if (fabsf (diff_angle_pi (rot, rot1)) > 0.40f)
            continue;

          gfloat cos_r = cosf (rot), sin_r = sinf (rot);

          /* Midpoint translation */
          gfloat mid_q_x = 0.5f * (q_ctx.minutiae[i1].x + q_ctx.minutiae[i2].x);
          gfloat mid_q_y = 0.5f * (q_ctx.minutiae[i1].y + q_ctx.minutiae[i2].y);
          gfloat mid_r_x = 0.5f * (r_ctx.minutiae[j1].x + r_ctx.minutiae[j2].x);
          gfloat mid_r_y = 0.5f * (r_ctx.minutiae[j1].y + r_ctx.minutiae[j2].y);

          gfloat tx = mid_r_x - (cos_r * mid_q_x - sin_r * mid_q_y);
          gfloat ty = mid_r_y - (sin_r * mid_q_x + cos_r * mid_q_y);

          guint cluster_inl = 0;
          gfloat cluster_sim = 0.0f;
          guint cur_indices[FTE3600_IPA_MAX_MINUTIAE];

          for (guint m3 = 0; m3 < n_matches; m3++)
            {
              if (!supported[m3]) continue;
              guint i3 = matches[m3].i, j3 = matches[m3].j;
              gfloat pred_x = cos_r * q_ctx.minutiae[i3].x - sin_r * q_ctx.minutiae[i3].y + tx;
              gfloat pred_y = sin_r * q_ctx.minutiae[i3].x + cos_r * q_ctx.minutiae[i3].y + ty;
              gfloat err = hypotf (r_ctx.minutiae[j3].x - pred_x, r_ctx.minutiae[j3].y - pred_y);
              if (err <= 4.5f)
                {
                  cur_indices[cluster_inl] = m3;
                  cluster_inl++;
                  cluster_sim += matches[m3].sim;
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

  result->diagnostic_policy_passed = fpi_fte3600_ipa_result_meets_policy (result);
  result->authentication_accepted =
    FTE3600_ENABLE_IPA_AUTH && result->diagnostic_policy_passed;
  return FTE3600_IPA_OK;
}

gboolean
fpi_fte3600_ipa_result_meets_policy (const Fte3600IpaMatchResult *result)
{
  if (result == NULL ||
      result->n_matched_pairs > FTE3600_IPA_MAX_MINUTIAE ||
      result->n_supported_inliers > result->n_matched_pairs ||
      !isfinite (result->consensus_score) ||
      result->consensus_score < 0.0f || result->consensus_score > 1.0f ||
      !isfinite (result->x_span) || result->x_span < 0.0f ||
      result->x_span >= FTE3600_IPA_WIDTH ||
      !isfinite (result->y_span) || result->y_span < 0.0f ||
      result->y_span >= FTE3600_IPA_HEIGHT)
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
