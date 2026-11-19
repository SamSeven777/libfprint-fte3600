/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Offline matcher lab: run captured PGM frames (FTE3600_DUMP_DIR output, or
 * preprocessed variants of them) through the driver's frozen BRISK extractor
 * and matcher and print the correspondence evidence for every ordered pair.
 * Geometry comes from the PGM header, so resampled variants can be compared.
 * "pass" applies the driver's diagnostic gates approximately (the real policy
 * lives in fte3600-brisk.c and is unchanged). Diagnostic tool only.
 *
 * Build (repo root, after ninja -C build):
 *   gcc -O1 -o build/fte3600-match-lab tools/fte3600-match-lab.c \
 *     -include fte3600-build-config.h -DFTE3600_ENABLE_PERSONAL_AUTH=1 -DFTE3600_ENABLE_IPA_AUTH=0 \
 *     -Ibuild/libfprint/drivers/fte3600 \
 *     -Ilibfprint/drivers/fte3600/sam7-brisk/libfprint/drivers \
 *     -Ilibfprint/drivers/fte3600/sam7-brisk/libfprint/matchers/brisk \
 *     $(pkg-config --cflags glib-2.0) build/libfprint/drivers/fte3600/libfprint-fte3600.a \
 *     $(pkg-config --libs glib-2.0) -lm
 * Usage: fte3600-match-lab [-v] frame.pgm...
 */
#include "brisk.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  const char        *name;
  guint8            *pixels;
  guint              width, height;
  FpiBriskStatus     extracted;
  FpiBriskFeatureSet features;
} Frame;

static gboolean
load_pgm (Frame *f, const char *path)
{
  g_autofree gchar *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents (path, &data, &length, NULL))
    return FALSE;
  guint maxval = 0;
  int consumed = 0;
  if (sscanf (data, "P5 %u %u %u%n", &f->width, &f->height, &maxval, &consumed) != 3)
    return FALSE;
  if (!f->width || !f->height || maxval != 255 ||
      f->width > G_MAXSIZE / f->height || (gsize) consumed >= length ||
      !isspace ((unsigned char) data[consumed]))
    return FALSE;
  consumed++;
  if (length < (gsize) consumed + (gsize) f->width * f->height)
    return FALSE;
  f->pixels = g_memdup2 (data + consumed, (gsize) f->width * f->height);
  f->name = path;
  return TRUE;
}

static gboolean
passes (const FpiBriskMatchEvidence *e, guint w, guint h)
{
  const gdouble aspect = w > h ? (gdouble) w / h : (gdouble) h / w;
  const gdouble qmax = e->query_covariance_xx + e->query_covariance_yy - e->query_min_variance;
  const gdouble rmax = e->reference_covariance_xx + e->reference_covariance_yy - e->reference_min_variance;
  return e->mutual_matches >= FPI_BRISK_MIN_MUTUAL_MATCHES && e->inliers >= 5 &&
         e->inlier_ratio >= 0.20 && e->inliers - e->competing_inliers >= 1 &&
         e->median_error < 1.25 && e->rms_error < 1.40 && e->mean_hamming <= 60.0 &&
         e->query_min_variance >= 3.0 && e->reference_min_variance >= 3.0 &&
         qmax >= 64.0 / 12.0 && rmax >= 64.0 / 12.0 &&
         MIN (1.0, e->query_anisotropy * aspect * aspect) >= 0.03 &&
         MIN (1.0, e->reference_anisotropy * aspect * aspect) >= 0.03;
}

int
main (int argc, char **argv)
{
  gboolean verbose = argc > 1 && strcmp (argv[1], "-v") == 0;
  const int first = verbose ? 2 : 1, n = argc - first;
  if (n < 1)
    {
      fprintf (stderr, "usage: %s [-v] frame.pgm...\n", argv[0]);
      return 2;
    }
  Frame *frames = g_new0 (Frame, n);
  for (int i = 0; i < n; i++)
    {
      Frame *f = &frames[i];
      if (!load_pgm (f, argv[i + first]))
        {
          fprintf (stderr, "cannot read %s\n", argv[i + first]);
          return 1;
        }
      FpiBriskImage image = { f->pixels, (gsize) f->width * f->height, f->width, f->height, f->width };
      f->extracted = fpi_brisk_extract (&image, &f->features);
      if (verbose || f->extracted != FPI_BRISK_OK)
        printf ("[%d] %s %ux%u extract st=%d features=%u\n", i, f->name, f->width, f->height,
                f->extracted, f->features.n_features);
    }
  printf ("%ux%u  cells: mutual/inliers, * = passes driver gates (query row x reference column)\n",
          frames[0].width, frames[0].height);
  printf ("     ");
  for (int j = 0; j < n; j++)
    printf ("%8d", j);
  printf ("\n");
  guint passing = 0, pairs = 0, total_inliers = 0;
  for (int i = 0; i < n; i++)
    {
      printf ("%3d: ", i);
      for (int j = 0; j < n; j++)
        {
          if (i == j || frames[i].extracted != FPI_BRISK_OK || frames[j].extracted != FPI_BRISK_OK)
            {
              printf ("%8s", i == j ? "." : "x");
              continue;
            }
          FpiBriskMatchEvidence e = { 0 };
          FpiBriskStatus st = fpi_brisk_match (&frames[i].features, frames[i].width, frames[i].height,
                                               &frames[j].features, frames[j].width, frames[j].height, &e);
          gboolean ok = st == FPI_BRISK_OK && passes (&e, frames[i].width, frames[i].height);
          char cell[16];
          snprintf (cell, sizeof cell, "%u/%u%s", e.mutual_matches, e.inliers, ok ? "*" : "");
          printf ("%8s", cell);
          pairs++;
          passing += ok;
          total_inliers += st == FPI_BRISK_OK ? e.inliers : 0;
          if (verbose && st == FPI_BRISK_OK)
            fprintf (stderr, "%d x %d st=%d mutual=%u inl=%u comp=%u med=%.2f rms=%.2f ham=%.1f shift=%.1f,%.1f ang=%.2f\n",
                     i, j, st, e.mutual_matches, e.inliers, e.competing_inliers, e.median_error,
                     e.rms_error, e.mean_hamming, e.translate_x, e.translate_y, e.angle);
        }
      printf ("\n");
    }
  printf ("summary: %u of %u ordered pairs pass, mean inliers %.1f\n", passing, pairs,
          pairs ? (gdouble) total_inliers / pairs : 0.0);
  return 0;
}
