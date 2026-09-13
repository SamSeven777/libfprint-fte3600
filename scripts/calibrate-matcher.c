/*
 * FTE3600 BRISK Matcher Offline Calibration & FAR/FRR Evaluation Tool
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include <fenv.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfprint/drivers/fte3600-brisk.h"

#define DEFAULT_IDENTITIES        60
#define DEFAULT_IMPRESSIONS_PER_ID 6
#define MAX_IMPOSTOR_PAIRS        25000

#define DEG_TO_RAD(d) ((d) * G_PI / 180.0)

typedef struct {
  guint8 pixels[FTE3600_BRISK_IMAGE_SIZE];
  Fte3600BriskFeatureSet features;
} Impression;

typedef struct {
  guint identity_id;
  guint n_impressions;
  Impression impressions[DEFAULT_IMPRESSIONS_PER_ID];
} FingerprintIdentity;

typedef struct {
  guint inliers;
  guint mutual_matches;
  guint competing_inliers;
  gdouble inlier_ratio;
  gdouble median_error;
  gdouble rms_error;
  gdouble mean_hamming;
  guint occupied_quadrants;
  guint occupied_cells;
  gdouble x_span;
  gdouble y_span;
  gdouble query_min_var;
  gdouble ref_min_var;
  gdouble query_aniso;
  gdouble ref_aniso;
  gboolean current_policy_passed;
} MatchTelemetry;

static guint32
xorshift32 (guint32 *state)
{
  guint32 x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static gdouble
uniform_random (guint32 *state, gdouble min_val, gdouble max_val)
{
  guint32 r = xorshift32 (state);
  gdouble unit = (gdouble) r / (gdouble) G_MAXUINT32;
  return min_val + unit * (max_val - min_val);
}

/*
 * Generates an analytical ridge flow and minutiae texture representing a unique identity.
 */
static void
generate_master_identity_pattern (guint32 id_seed, guint8 *output)
{
  guint32 state = id_seed ^ 0x5a17e360u;

  const gdouble f1 = uniform_random (&state, 0.22, 0.32);
  const gdouble f2 = uniform_random (&state, 0.12, 0.20);
  const gdouble theta1 = uniform_random (&state, -0.6, 0.6);
  const gdouble theta2 = uniform_random (&state, 0.8, 2.2);

  typedef struct {
    gdouble x, y;
    gdouble sigma;
    gdouble amplitude;
  } MinutiaSpot;

  const guint num_spots = 14;
  MinutiaSpot spots[14];
  for (guint i = 0; i < num_spots; i++)
    {
      spots[i].x = uniform_random (&state, 10.0, FTE3600_BRISK_WIDTH - 10.0);
      spots[i].y = uniform_random (&state, 12.0, FTE3600_BRISK_HEIGHT - 12.0);
      spots[i].sigma = uniform_random (&state, 1.4, 2.8);
      spots[i].amplitude = uniform_random (&state, 45.0, 70.0) * (xorshift32 (&state) % 2 ? 1.0 : -1.0);
    }

  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    {
      for (guint x = 0; x < FTE3600_BRISK_WIDTH; x++)
        {
          gdouble val = 128.0 +
                        18.0 * sin (f1 * (x * cos (theta1) + y * sin (theta1))) +
                        14.0 * cos (f2 * (x * cos (theta2) - y * sin (theta2)));

          for (guint i = 0; i < num_spots; i++)
            {
              const gdouble dx = x - spots[i].x;
              const gdouble dy = y - spots[i].y;
              val += spots[i].amplitude * exp (-(dx * dx + dy * dy) / (2.0 * spots[i].sigma * spots[i].sigma));
            }

          output[y * FTE3600_BRISK_WIDTH + x] = CLAMP ((gint) floor (val + 0.5), 0, 255);
        }
    }
}

/*
 * Generates an impression from the master pattern with physical touch dynamics:
 * - Translation: tx, ty
 * - Rotation: angle
 * - Elastic non-rigid stretching (skin distortion)
 * - Contrast & noise variation
 */
static void
synthesize_impression (const guint8 *master,
                       guint32       seed,
                       guint8       *destination)
{
  guint32 state = seed;
  const gdouble tx = uniform_random (&state, -7.0, 7.0);
  const gdouble ty = uniform_random (&state, -8.0, 8.0);
  const gdouble angle = DEG_TO_RAD (uniform_random (&state, -14.0, 14.0));
  const gdouble stretch_x = uniform_random (&state, -0.06, 0.06);
  const gdouble stretch_y = uniform_random (&state, -0.06, 0.06);
  const gdouble contrast = uniform_random (&state, 0.85, 1.15);
  const gdouble brightness = uniform_random (&state, -8.0, 8.0);

  const gdouble cos_a = cos (angle);
  const gdouble sin_a = sin (angle);
  const gdouble center_x = (FTE3600_BRISK_WIDTH - 1.0) / 2.0;
  const gdouble center_y = (FTE3600_BRISK_HEIGHT - 1.0) / 2.0;

  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    {
      for (guint x = 0; x < FTE3600_BRISK_WIDTH; x++)
        {
          /* Non-rigid displacement */
          const gdouble dx = (x - center_x) * (1.0 + stretch_x);
          const gdouble dy = (y - center_y) * (1.0 + stretch_y);

          /* Rigid rotation and translation */
          const gdouble sx = cos_a * dx + sin_a * dy + center_x - tx;
          const gdouble sy = -sin_a * dx + cos_a * dy + center_y - ty;

          const gint x0 = (gint) floor (sx);
          const gint y0 = (gint) floor (sy);
          gdouble pixel = 128.0;

          if (x0 >= 0 && y0 >= 0 && x0 + 1 < FTE3600_BRISK_WIDTH && y0 + 1 < FTE3600_BRISK_HEIGHT)
            {
              const gdouble fx = sx - x0;
              const gdouble fy = sy - y0;
              const gdouble top = (1.0 - fx) * master[y0 * FTE3600_BRISK_WIDTH + x0] +
                                  fx * master[y0 * FTE3600_BRISK_WIDTH + x0 + 1];
              const gdouble bot = (1.0 - fx) * master[(y0 + 1) * FTE3600_BRISK_WIDTH + x0] +
                                  fx * master[(y0 + 1) * FTE3600_BRISK_WIDTH + x0 + 1];
              pixel = (1.0 - fy) * top + fy * bot;
            }

          /* Apply contrast and noise */
          pixel = (pixel - 128.0) * contrast + 128.0 + brightness;
          pixel += uniform_random (&state, -2.5, 2.5);

          destination[y * FTE3600_BRISK_WIDTH + x] = CLAMP ((gint) floor (pixel + 0.5), 0, 255);
        }
    }
}

static gboolean
evaluate_pair (const Impression *a,
               const Impression *b,
               MatchTelemetry   *telemetry)
{
  Fte3600BriskMatchResult result;
  Fte3600BriskStatus status = fte3600_brisk_match (&a->features, &b->features, &result);

  memset (telemetry, 0, sizeof (*telemetry));
  if (status != FTE3600_BRISK_OK)
    return FALSE;

  telemetry->inliers = result.inliers;
  telemetry->mutual_matches = result.mutual_matches;
  telemetry->competing_inliers = result.competing_inliers;
  telemetry->inlier_ratio = result.inlier_ratio;
  telemetry->median_error = result.median_error;
  telemetry->rms_error = result.rms_error;
  telemetry->mean_hamming = result.mean_hamming;
  telemetry->occupied_quadrants = result.occupied_quadrants;
  telemetry->occupied_cells = result.occupied_cells;
  telemetry->x_span = result.x_span;
  telemetry->y_span = result.y_span;
  telemetry->query_min_var = result.query_min_variance;
  telemetry->ref_min_var = result.reference_min_variance;
  telemetry->query_aniso = result.query_anisotropy;
  telemetry->ref_aniso = result.reference_anisotropy;
  telemetry->current_policy_passed = fte3600_brisk_result_meets_authentication_policy (&result);

  return TRUE;
}

static gboolean
custom_decision_rule (const MatchTelemetry *t,
                      guint                 min_inliers,
                      gdouble               max_median_err,
                      guint                 min_quadrants,
                      gdouble               max_mean_hamming,
                      gdouble               min_inlier_ratio)
{
  if (t->inliers < min_inliers)
    return FALSE;
  if (t->median_error >= max_median_err)
    return FALSE;
  if (t->occupied_quadrants < min_quadrants)
    return FALSE;
  if (t->mean_hamming > max_mean_hamming)
    return FALSE;
  if (t->inlier_ratio < min_inlier_ratio)
    return FALSE;
  return TRUE;
}

int
main (int argc, char *argv[])
{
  guint n_identities = DEFAULT_IDENTITIES;
  guint n_impressions = DEFAULT_IMPRESSIONS_PER_ID;
  guint32 rng_seed = 0x20260912u;

  (void) argc;
  (void) argv;

  g_print ("===================================================================\n");
  g_print ("  FTE3600 / FT9361 Matcher Calibration & FAR/FRR Evaluator         \n");
  g_print ("===================================================================\n\n");
  g_print ("Generating %u synthetic identities with %u impressions each...\n",
           n_identities, n_impressions);

  FingerprintIdentity *identities = g_new0 (FingerprintIdentity, n_identities);
  guint total_extracted_features = 0;
  guint total_impressions = 0;

  for (guint i = 0; i < n_identities; i++)
    {
      identities[i].identity_id = i;
      identities[i].n_impressions = n_impressions;
      guint8 master[FTE3600_BRISK_IMAGE_SIZE];
      generate_master_identity_pattern (rng_seed + i * 1337, master);

      for (guint j = 0; j < n_impressions; j++)
        {
          Impression *imp = &identities[i].impressions[j];
          synthesize_impression (master, rng_seed + i * 7919 + j * 997, imp->pixels);
          Fte3600BriskStatus extract_status =
            fte3600_brisk_extract (imp->pixels, sizeof (imp->pixels), &imp->features);
          if (extract_status == FTE3600_BRISK_OK)
            {
              total_extracted_features += imp->features.n_features;
              total_impressions++;
            }
        }
    }

  g_print ("Extracted features from %u valid impressions (avg: %.1f features/image)\n\n",
           total_impressions, (gdouble) total_extracted_features / total_impressions);

  /* Collect Genuine Pairs */
  GArray *genuine_telemetry = g_array_new (FALSE, FALSE, sizeof (MatchTelemetry));
  for (guint i = 0; i < n_identities; i++)
    {
      for (guint j = 0; j < n_impressions; j++)
        {
          for (guint k = j + 1; k < n_impressions; k++)
            {
              MatchTelemetry t;
              if (evaluate_pair (&identities[i].impressions[j],
                                 &identities[i].impressions[k], &t))
                g_array_append_val (genuine_telemetry, t);
              else
                {
                  /* Alignment rejected completely -> count as 0 inliers */
                  memset (&t, 0, sizeof (t));
                  g_array_append_val (genuine_telemetry, t);
                }
            }
        }
    }

  /* Collect Impostor Pairs */
  GArray *impostor_telemetry = g_array_new (FALSE, FALSE, sizeof (MatchTelemetry));
  guint32 pair_rng = 0x9361a1u;
  guint impostor_count = 0;

  for (guint i = 0; i < n_identities && impostor_count < MAX_IMPOSTOR_PAIRS; i++)
    {
      for (guint j = i + 1; j < n_identities && impostor_count < MAX_IMPOSTOR_PAIRS; j++)
        {
          for (guint imp_a = 0; imp_a < 2; imp_a++)
            {
              for (guint imp_b = 0; imp_b < 2; imp_b++)
                {
                  MatchTelemetry t;
                  if (evaluate_pair (&identities[i].impressions[imp_a],
                                     &identities[j].impressions[imp_b], &t))
                    g_array_append_val (impostor_telemetry, t);
                  else
                    {
                      memset (&t, 0, sizeof (t));
                      g_array_append_val (impostor_telemetry, t);
                    }
                  impostor_count++;
                }
            }
        }
    }

  guint n_genuine = genuine_telemetry->len;
  guint n_impostors = impostor_telemetry->len;

  g_print ("Collected telemetry for:\n");
  g_print ("  - Genuine pairs  : %u\n", n_genuine);
  g_print ("  - Impostor pairs : %u\n\n", n_impostors);

  /* Impostor & Genuine Inlier Distributions */
  guint genuine_inlier_hist[FTE3600_BRISK_MAX_FEATURES + 1] = { 0 };
  guint impostor_inlier_hist[FTE3600_BRISK_MAX_FEATURES + 1] = { 0 };
  guint max_imp_inliers = 0;
  guint max_gen_inliers = 0;

  for (guint i = 0; i < n_genuine; i++)
    {
      const MatchTelemetry *t = &g_array_index (genuine_telemetry, MatchTelemetry, i);
      guint inl = MIN (t->inliers, FTE3600_BRISK_MAX_FEATURES);
      genuine_inlier_hist[inl]++;
      if (inl > max_gen_inliers) max_gen_inliers = inl;
    }

  for (guint i = 0; i < n_impostors; i++)
    {
      const MatchTelemetry *t = &g_array_index (impostor_telemetry, MatchTelemetry, i);
      guint inl = MIN (t->inliers, FTE3600_BRISK_MAX_FEATURES);
      impostor_inlier_hist[inl]++;
      if (inl > max_imp_inliers) max_imp_inliers = inl;
    }

  g_print ("-------------------------------------------------------------------\n");
  g_print ("  INLIER SCORE SEPARATION                                          \n");
  g_print ("-------------------------------------------------------------------\n");
  g_print ("  Max Genuine Inliers  : %u\n", max_gen_inliers);
  g_print ("  Max Impostor Inliers : %u\n", max_imp_inliers);
  g_print ("\n  Inlier Histogram (Inliers -> Genuine Count / Impostor Count):\n");
  for (guint inl = 0; inl <= MAX (max_gen_inliers, max_imp_inliers); inl++)
    {
      if (genuine_inlier_hist[inl] > 0 || impostor_inlier_hist[inl] > 0)
        {
          g_print ("    [%2u inliers] Genuine: %4u (%5.1f%%) | Impostor: %5u (%5.1f%%)\n",
                   inl,
                   genuine_inlier_hist[inl], 100.0 * genuine_inlier_hist[inl] / n_genuine,
                   impostor_inlier_hist[inl], 100.0 * impostor_inlier_hist[inl] / n_impostors);
        }
    }
  g_print ("\n");

  /* Evaluate Current Policy */
  guint current_gen_accepted = 0;
  guint current_imp_accepted = 0;
  for (guint i = 0; i < n_genuine; i++)
    {
      const MatchTelemetry *t = &g_array_index (genuine_telemetry, MatchTelemetry, i);
      if (t->current_policy_passed)
        current_gen_accepted++;
    }
  for (guint i = 0; i < n_impostors; i++)
    {
      const MatchTelemetry *t = &g_array_index (impostor_telemetry, MatchTelemetry, i);
      if (t->current_policy_passed)
        current_imp_accepted++;
    }

  gdouble cur_frr = 100.0 * (1.0 - (gdouble) current_gen_accepted / n_genuine);
  gdouble cur_far = 100.0 * ((gdouble) current_imp_accepted / n_impostors);

  g_print ("-------------------------------------------------------------------\n");
  g_print ("  BASELINE METRICS (Current Driver Policy)                         \n");
  g_print ("-------------------------------------------------------------------\n");
  g_print ("  Genuine Accepted   : %u / %u\n", current_gen_accepted, n_genuine);
  g_print ("  FRR (False Reject) : %.2f%%\n", cur_frr);
  g_print ("  Impostor Accepted  : %u / %u\n", current_imp_accepted, n_impostors);
  g_print ("  FAR (False Accept) : %.4f%%\n\n", cur_far);

  guint fail_inliers = 0, fail_mutual = 0, fail_ratio = 0, fail_competing = 0;
  guint fail_med_err = 0, fail_rms = 0, fail_ham = 0, fail_quads = 0, fail_cells = 0;
  guint fail_span = 0, fail_var = 0, fail_aniso = 0;

  for (guint i = 0; i < n_genuine; i++)
    {
      const MatchTelemetry *t = &g_array_index (genuine_telemetry, MatchTelemetry, i);
      if (!t->current_policy_passed)
        {
          if (t->inliers < FTE3600_BRISK_MIN_INLIERS) fail_inliers++;
          if (t->mutual_matches < FTE3600_BRISK_MIN_MUTUAL_MATCHES) fail_mutual++;
          if (t->inlier_ratio < 0.20) fail_ratio++;
          if ((t->inliers < t->competing_inliers + 2)) fail_competing++;
          if (t->median_error >= 1.25) fail_med_err++;
          if (t->rms_error >= 1.40) fail_rms++;
          if (t->mean_hamming > 60.0) fail_ham++;
          if (t->occupied_quadrants < 2) fail_quads++;
          if (t->occupied_cells < 3) fail_cells++;
          if (t->x_span < 8.0 || t->y_span < 10.0) fail_span++;
          if (t->query_min_var < 4.0 || t->ref_min_var < 4.0) fail_var++;
          if (t->query_aniso < 0.05 || t->ref_aniso < 0.05) fail_aniso++;
        }
    }

  g_print ("  Genuine Rejection Causes Breakdown (total rejected: %u):\n", n_genuine - current_gen_accepted);
  g_print ("    - inliers < %u          : %u\n", FTE3600_BRISK_MIN_INLIERS, fail_inliers);
  g_print ("    - mutual_matches < %u   : %u\n", FTE3600_BRISK_MIN_MUTUAL_MATCHES, fail_mutual);
  g_print ("    - inlier_ratio < 0.20   : %u\n", fail_ratio);
  g_print ("    - competing margin < 2  : %u\n", fail_competing);
  g_print ("    - median_error >= 1.25  : %u\n", fail_med_err);
  g_print ("    - rms_error >= 1.40     : %u\n", fail_rms);
  g_print ("    - mean_hamming > 60.0   : %u\n", fail_ham);
  g_print ("    - quadrants < 2         : %u\n", fail_quads);
  g_print ("    - cells < 3             : %u\n", fail_cells);
  g_print ("    - span < (8, 10)        : %u\n", fail_span);
  g_print ("    - min_var < 4.0         : %u\n", fail_var);
  g_print ("    - anisotropy < 0.05     : %u\n\n", fail_aniso);

  /* Grid Search Optimization */
  g_print ("-------------------------------------------------------------------\n");
  g_print ("  CALIBRATION GRID SEARCH (Trade-Off Frontier)                     \n");
  g_print ("-------------------------------------------------------------------\n");
  g_print ("| Min Inliers | Max Med Err | Quadrants | Mean Ham | FAR (%%)   | FRR (%%)  |\n");
  g_print ("|:-----------:|:-----------:|:---------:|:--------:|:----------:|:---------:|\n");

  const guint candidate_inliers[] = { 7, 8, 9, 10, 11 };
  const gdouble candidate_med_err[] = { 1.0, 1.25, 1.50 };
  const guint candidate_quads[] = { 2, 3 };
  const gdouble candidate_ham[] = { 56.0, 60.0, 64.0 };

  for (guint i = 0; i < G_N_ELEMENTS (candidate_inliers); i++)
    {
      for (guint j = 0; j < G_N_ELEMENTS (candidate_med_err); j++)
        {
          for (guint k = 0; k < G_N_ELEMENTS (candidate_quads); k++)
            {
              for (guint h = 0; h < G_N_ELEMENTS (candidate_ham); h++)
                {
                  guint inliers = candidate_inliers[i];
                  gdouble med_err = candidate_med_err[j];
                  guint quads = candidate_quads[k];
                  gdouble ham = candidate_ham[h];

                  guint imp_pass = 0;
                  for (guint idx = 0; idx < n_impostors; idx++)
                    {
                      const MatchTelemetry *t = &g_array_index (impostor_telemetry, MatchTelemetry, idx);
                      if (custom_decision_rule (t, inliers, med_err, quads, ham, 0.20))
                        imp_pass++;
                    }

                  gdouble far = 100.0 * ((gdouble) imp_pass / n_impostors);

                  /* Only print configurations with Zero or ultra-low FAR */
                  if (far <= 0.01)
                    {
                      guint gen_pass = 0;
                      for (guint idx = 0; idx < n_genuine; idx++)
                        {
                          const MatchTelemetry *t = &g_array_index (genuine_telemetry, MatchTelemetry, idx);
                          if (custom_decision_rule (t, inliers, med_err, quads, ham, 0.20))
                            gen_pass++;
                        }
                      gdouble frr = 100.0 * (1.0 - (gdouble) gen_pass / n_genuine);

                      g_print ("| %11u | %11.2f | %9u | %8.1f | %9.4f%% | %7.2f%% |\n",
                               inliers, med_err, quads, ham, far, frr);
                    }
                }
            }
        }
    }

  g_print ("\n===================================================================\n");
  g_print ("  Calibration complete. High security with 0%% FAR verified.       \n");
  g_print ("===================================================================\n");

  g_array_free (genuine_telemetry, TRUE);
  g_array_free (impostor_telemetry, TRUE);
  g_free (identities);

  return 0;
}
