/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Persistent enrollment/gallery wrapper for the approved Sam7 BRISK core.
 * This is personal test authentication, without population FAR/FRR claims.
 */
#include "fte3600_auth.h"
/* The independent device API and imported sensor enum both name UNKNOWN.
 * Keep their enum tags/types separate without editing the pinned header. */
#define FTE3600_SENSOR_UNKNOWN SAM7_FTE3600_SENSOR_UNKNOWN
#include "fte3600-brisk.h"
#undef FTE3600_SENSOR_UNKNOWN

#include <float.h>
#include <string.h>
#include <stdlib.h>

G_STATIC_ASSERT (FTE3600_ENABLE_PERSONAL_AUTH == 1);
G_STATIC_ASSERT (FTE3600_ENABLE_IPA_AUTH == 0);
G_STATIC_ASSERT (sizeof (gfloat) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128);

#define HEADER_SIZE 40u
#define DIGEST_SIZE 32u
#define FEATURE_SIZE (12u + FPI_BRISK_DESCRIPTOR_BYTES)
#define PROFILE_VERSION 1u
static const guint8 magic[8] = { 'F', 'T', 'A', 'U', 'T', 'H', '0', '1' };
static const guint8 query_magic[8] = { 'F', 'T', 'Q', 'U', 'E', 'R', 'Y', '1' };

struct _Fte3600AuthTemplate
{
  gatomicrefcount        references;
  Fte3600AuthIdentity    identity;
  Fte3600BriskFeatureSet samples[FTE3600_AUTH_ENROLL_SAMPLES];
};
struct _Fte3600AuthEnrollment
{
  Fte3600AuthTemplate print;
  guint               count;
  guint8              digests[FTE3600_AUTH_ENROLL_SAMPLES][DIGEST_SIZE];
};
struct _Fte3600AuthProbe
{
  Fte3600AuthIdentity    identity;
  Fte3600BriskFeatureSet features;
};

G_DEFINE_QUARK (fte3600 - auth - error - quark, fte3600_auth_error)

static gboolean
fail (GError **error, Fte3600AuthError code, const gchar *message)
{
  g_set_error_literal (error, FTE3600_AUTH_ERROR, code, message);
  return FALSE;
}

static gboolean
cancelled (Fte3600AuthCancelFunc cancel, gpointer cancel_data, GError **error)
{
  if (!cancel || !cancel (cancel_data))
    return FALSE;
  fail (error, FTE3600_AUTH_CANCELLED, "Authentication operation cancelled");
  return TRUE;
}

static guint16
backend_id (enum fte3600_factory_model model)
{
  switch (model)
    {
    case FTE3600_MODEL_UNKNOWN: return 0;

    case FTE3600_MODEL_FT9338: return 0x9338;

    case FTE3600_MODEL_FT9348: return 0x9348;

    case FTE3600_MODEL_FT9361: return 0x9361;

    case FTE3600_MODEL_FT9536: return 0x9536;

    case FTE3600_MODEL_FT9368: return 0x9368;

    case FTE3600_MODEL_FW9369: return 0x9369;

    case FTE3600_MODEL_FT9365: return 0x9365;

    case FTE3600_MODEL_FT9769: return 0x9769;

    default: return 0;
    }
}

static const Fte3600MatchProfile *
profile_for (const Fte3600AuthIdentity *identity)
{
  if (!identity || identity->processing_version != FTE3600_AUTH_PROCESSING_VERSION)
    return NULL;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_find (identity->chip_id);
  if (!profile || profile->width != identity->width || profile->height != identity->height)
    return NULL;
  switch (identity->backend_model)
    {
    case 0x9338:
    case 0x9348:
    case 0x9361:
    case 0x9536:
    case 0x9368:
    case 0x9369:
      return identity->backend_model == identity->chip_id ? profile : NULL;

    case 0x9365:
    case 0x9769:
      switch (identity->chip_id)
        {
        case 0x9349:
        case 0x9363:
        case 0x9365:
        case 0x9372:
        case 0x9391:
        case 0x9392:
        case 0x9395:
        case 0x9396:
        case 0x9397:
        case 0x9398:
          return profile;

        default: return NULL;
        }

    default: return NULL;
    }
}

gboolean
fte3600_auth_identity_from_status (const struct fte3600_device_status *status,
                                   Fte3600AuthIdentity *identity, GError **error)
{
  if (identity)
    memset (identity, 0, sizeof (*identity));
  if (!status || !identity)
    return fail (error, FTE3600_AUTH_INVALID_ARGUMENT, "Missing device status or identity output");
  Fte3600AuthIdentity value = { backend_id (status->model), status->chip_id,
                                status->width, status->height, FTE3600_AUTH_PROCESSING_VERSION };
  if (!status->startup_complete || !status->calibration_complete ||
      status->error != FTE3600_DEVICE_OK ||
      (status->state != FTE3600_DEVICE_IDLE && status->state != FTE3600_DEVICE_ARMED) ||
      !profile_for (&value))
    return fail (error, FTE3600_AUTH_UNSUPPORTED, "Device is not ready with a supported exact chip geometry");
  *identity = value;
  return TRUE;
}

gboolean
fte3600_auth_identity_equal (const Fte3600AuthIdentity *a, const Fte3600AuthIdentity *b)
{
  return a && b && a->backend_model == b->backend_model && a->chip_id == b->chip_id &&
         a->width == b->width && a->height == b->height && a->processing_version == b->processing_version;
}

static gboolean
extract (const Fte3600AuthIdentity *identity, const guint8 *pixels, gsize length,
         gsize stride, Fte3600BriskFeatureSet *features,
         Fte3600AuthCancelFunc cancel, gpointer cancel_data, GError **error)
{
  if (cancelled (cancel, cancel_data, error))
    return FALSE;
  const Fte3600MatchProfile *profile = profile_for (identity);
  if (!profile)
    return fail (error, FTE3600_AUTH_UNSUPPORTED, "Unsupported exact capture profile");
  FpiBriskImage image = { pixels, length, identity->width, identity->height, stride };
  const Fte3600BriskStatus status = fpi_fte3600_brisk_extract_for_profile (profile, &image, features);
  if (cancelled (cancel, cancel_data, error))
    return FALSE;
  if (status == FPI_BRISK_INVALID_ARGUMENT)
    return fail (error, FTE3600_AUTH_INVALID_ARGUMENT, "Invalid capture extent or row stride");
  if (status != FPI_BRISK_OK)
    return fail (error, FTE3600_AUTH_LOW_QUALITY, "Capture has insufficient contrast or features");
  /* A featureful but spatially degenerate first sample must not become an
   * enrollment anchor. Apply the existing frozen policy, without new gates. */
  Fte3600BriskMatchResult result;
  const Fte3600BriskStatus self_status = fpi_fte3600_brisk_match_for_profile (profile, features, features, &result);
  if (cancelled (cancel, cancel_data, error))
    return FALSE;
  if (self_status != FPI_BRISK_OK || !result.authentication_accepted)
    return fail (error, FTE3600_AUTH_LOW_QUALITY, "Capture fails the strict spatial evidence policy");
  return TRUE;
}

Fte3600AuthProbe *
fte3600_auth_probe_new_full (const Fte3600AuthIdentity *identity, const guint8 *pixels,
                             gsize length, gsize stride, Fte3600AuthCancelFunc cancel,
                             gpointer cancel_data, GError **error)
{
  g_autoptr(Fte3600AuthProbe) probe = g_new0 (Fte3600AuthProbe, 1);
  if (!extract (identity, pixels, length, stride, &probe->features, cancel, cancel_data, error))
    return NULL;
  probe->identity = *identity;
  return g_steal_pointer (&probe);
}

void
fte3600_auth_probe_free (Fte3600AuthProbe *probe)
{
  g_free (probe);
}
Fte3600AuthTemplate *
fte3600_auth_template_ref (Fte3600AuthTemplate *print)
{
  if (print)
    g_atomic_ref_count_inc (&print->references);
  return print;
}

void
fte3600_auth_template_free (Fte3600AuthTemplate *print)
{
  if (print && g_atomic_ref_count_dec (&print->references))
    g_free (print);
}
void
fte3600_auth_enrollment_free (Fte3600AuthEnrollment *enrollment)
{
  g_free (enrollment);
}

Fte3600AuthEnrollment *
fte3600_auth_enrollment_new (const Fte3600AuthIdentity *identity, GError **error)
{
  if (!profile_for (identity))
    {
      fail (error, FTE3600_AUTH_UNSUPPORTED, "Unsupported exact capture profile");
      return NULL;
    }
  Fte3600AuthEnrollment *enrollment = g_new0 (Fte3600AuthEnrollment, 1);
  enrollment->print.identity = *identity;
  return enrollment;
}

guint
fte3600_auth_enrollment_count (const Fte3600AuthEnrollment *enrollment)
{
  return enrollment ? enrollment->count : 0;
}

/* Only compare validated finite features. Numeric comparison preserves the
* former +0 == -0 semantics. Sorting borrowed views leaves encoded and matcher
* feature order untouched, and comparing all slots retains multiplicity. */
static gint
compare_feature (gconstpointer a, gconstpointer b)
{
  const Fte3600BriskFeature *x = *(const Fte3600BriskFeature * const *) a;
  const Fte3600BriskFeature *y = *(const Fte3600BriskFeature * const *) b;

  if (x->x != y->x)
    return x->x < y->x ? -1 : 1;
  if (x->y != y->y)
    return x->y < y->y ? -1 : 1;
  if (x->orientation != y->orientation)
    return x->orientation < y->orientation ? -1 : 1;
  return memcmp (x->descriptor, y->descriptor, FPI_BRISK_DESCRIPTOR_BYTES);
}

static gboolean
same_features (const Fte3600BriskFeatureSet *a, const Fte3600BriskFeatureSet *b)
{
  if (a->n_features != b->n_features)
    return FALSE;
  const Fte3600BriskFeature *ordered_a[FPI_BRISK_MAX_FEATURES];
  const Fte3600BriskFeature *ordered_b[FPI_BRISK_MAX_FEATURES];
  for (guint i = 0; i < a->n_features; i++)
    {
      ordered_a[i] = &a->features[i];
      ordered_b[i] = &b->features[i];
    }
  qsort (ordered_a, a->n_features, sizeof ordered_a[0], compare_feature);
  qsort (ordered_b, b->n_features, sizeof ordered_b[0], compare_feature);
  for (guint i = 0; i < a->n_features; i++)
    if (compare_feature (&ordered_a[i], &ordered_b[i]) != 0)
      return FALSE;
  return TRUE;
}

/* Closest rejected comparison, kept so an enrollment retry can say which
 * acceptance gate the capture missed. Diagnostic only; never decides. */
typedef struct
{
  guint                   reference;
  Fte3600BriskStatus      status;
  Fte3600BriskMatchResult result;
} ClosestComparison;

static gboolean
closer (const ClosestComparison *candidate, const ClosestComparison *best, guint count)
{
  if (best->reference >= count)
    return TRUE;
  if (candidate->result.inliers != best->result.inliers)
    return candidate->result.inliers > best->result.inliers;
  return candidate->result.mutual_matches > best->result.mutual_matches;
}

static gchar *
describe_closest (const Fte3600AuthTemplate *print, const Fte3600BriskFeatureSet *sample,
                  const ClosestComparison *closest, guint count)
{
  if (closest->reference >= count)
    return g_strdup_printf ("features %u; no accepted sample compared", sample->n_features);
  const Fte3600BriskMatchResult *r = &closest->result;
  return g_strdup_printf ("features %u; closest sample %u (features %u) status %d: mutual %u inliers %u"
                          " competing %u ratio %.2f median %.2f rms %.2f hamming %.1f spread q %.1f/%.1f"
                          " r %.1f/%.1f aniso %.3f/%.3f shift %.1f,%.1f angle %.2f",
                          sample->n_features, closest->reference,
                          print->samples[closest->reference].n_features, (int) closest->status,
                          r->mutual_matches, r->inliers, r->competing_inliers, r->inlier_ratio,
                          r->median_error, r->rms_error, r->mean_hamming,
                          r->query_min_variance, r->query_max_variance,
                          r->reference_min_variance, r->reference_max_variance,
                          r->normalized_query_anisotropy, r->normalized_reference_anisotropy,
                          r->translate_x, r->translate_y, r->angle);
}

/* Strip sensors (40 px wide and narrower) image about 4 mm of finger per
 * touch, so two honest touches of one finger usually share no skin at all:
 * on a 40x196 array, real touch pairs produce 1..8 mutual features unless
 * they land on the same band, and then 25..50. Chaining every new sample to
 * an already accepted one rejects exactly the touches that would widen the
 * template, and the finished template then covers one band and fails most
 * later presentations. For those profiles enrollment instead collects
 * distinct samples that each pass the extractor's own quality policy; the
 * matcher's acceptance policy for verification is unchanged. */
static gboolean
coverage_enrollment (const Fte3600MatchProfile *profile)
{
  return profile && profile->width <= 40;
}

static gboolean
consistent (const Fte3600AuthTemplate *print, const Fte3600BriskFeatureSet *sample,
            guint count, gboolean *accepted, ClosestComparison *closest,
            Fte3600AuthCancelFunc cancel, gpointer cancel_data, GError **error)
{
  const Fte3600MatchProfile *profile = profile_for (&print->identity);

  if (closest)
    closest->reference = count;
  *accepted = count == 0 || coverage_enrollment (profile);
  if (*accepted)
    return TRUE;
  for (guint i = 0; i < count; i++)
    {
      if (cancelled (cancel, cancel_data, error))
        return FALSE;
      ClosestComparison candidate = { .reference = i };
      candidate.status =
        fpi_fte3600_brisk_match_for_profile (profile, sample, &print->samples[i], &candidate.result);
      if (cancelled (cancel, cancel_data, error))
        return FALSE;
      if (candidate.status == FPI_BRISK_OK && candidate.result.authentication_accepted)
        {
          *accepted = TRUE;
          return TRUE;
        }
      if (closest && closer (&candidate, closest, count))
        *closest = candidate;
    }
  return TRUE;
}

gboolean
fte3600_auth_enrollment_add_full (Fte3600AuthEnrollment *enrollment, const guint8 *pixels,
                                  gsize length, gsize stride, guint *completed,
                                  Fte3600AuthCancelFunc cancel, gpointer cancel_data, GError **error)
{
  if (completed)
    *completed = enrollment ? enrollment->count : 0;
  if (!enrollment || enrollment->count >= FTE3600_AUTH_ENROLL_SAMPLES)
    return fail (error, FTE3600_AUTH_INVALID_ARGUMENT, "Missing enrollment or enrollment already complete");
  Fte3600BriskFeatureSet sample;
  if (!extract (&enrollment->print.identity, pixels, length, stride, &sample, cancel, cancel_data, error))
    return FALSE;
  guint8 digest[DIGEST_SIZE];
  gsize digest_length = sizeof (digest);
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  for (guint y = 0; y < enrollment->print.identity.height; y++)
    g_checksum_update (checksum, pixels + y * stride, enrollment->print.identity.width);
  g_checksum_get_digest (checksum, digest, &digest_length);
  for (guint i = 0; i < enrollment->count; i++)
    if (memcmp (digest, enrollment->digests[i], DIGEST_SIZE) == 0 ||
        same_features (&sample, &enrollment->print.samples[i]))
      return fail (error, FTE3600_AUTH_DUPLICATE, "Capture repeats an accepted enrollment sample");
  gboolean accepted;
  ClosestComparison closest;
  if (!consistent (&enrollment->print, &sample, enrollment->count, &accepted, &closest,
                   cancel, cancel_data, error) ||
      cancelled (cancel, cancel_data, error))
    return FALSE;
  if (!accepted)
    {
      g_autofree gchar *detail = describe_closest (&enrollment->print, &sample, &closest, enrollment->count);
      g_set_error (error, FTE3600_AUTH_ERROR, FTE3600_AUTH_INCONSISTENT,
                   "Capture does not strictly match an accepted enrollment sample (%s)", detail);
      return FALSE;
    }
  enrollment->print.samples[enrollment->count] = sample;
  memcpy (enrollment->digests[enrollment->count], digest, DIGEST_SIZE);
  enrollment->count++;
  if (completed)
    *completed = enrollment->count;
  return TRUE;
}

Fte3600AuthTemplate *
fte3600_auth_enrollment_finish (const Fte3600AuthEnrollment *enrollment, GError **error)
{
  if (!enrollment || enrollment->count != FTE3600_AUTH_ENROLL_SAMPLES)
    {
      fail (error, FTE3600_AUTH_INCOMPLETE, "Enrollment requires eight accepted distinct captures");
      return NULL;
    }
  Fte3600AuthTemplate *print = g_new (Fte3600AuthTemplate, 1);
  *print = enrollment->print;
  g_atomic_ref_count_init (&print->references);
  return print;
}

const Fte3600AuthIdentity *
fte3600_auth_template_identity (const Fte3600AuthTemplate *print)
{
  return print ? &print->identity : NULL;
}

static gboolean
better (const Fte3600AuthMatch *candidate, const Fte3600AuthMatch *current)
{
  if (!candidate->matched)
    return FALSE;
  return !current->matched || candidate->inliers > current->inliers ||
         (candidate->inliers == current->inliers &&
          (candidate->mean_hamming < current->mean_hamming ||
           (candidate->mean_hamming == current->mean_hamming && candidate->rms_error < current->rms_error)));
}

gboolean
fte3600_auth_match_full (const Fte3600AuthProbe *probe, const Fte3600AuthTemplate *print,
                         Fte3600AuthMatch *result, Fte3600AuthCancelFunc cancel,
                         gpointer cancel_data, GError **error)
{
  if (result)
    memset (result, 0, sizeof (*result));
  if (!probe || !print || !result)
    return fail (error, FTE3600_AUTH_INVALID_ARGUMENT, "Missing query, template or match result");
  if (!fte3600_auth_identity_equal (&probe->identity, &print->identity))
    return fail (error, FTE3600_AUTH_PROFILE_MISMATCH, "Query and template capture identities differ");
  const Fte3600MatchProfile *profile = profile_for (&probe->identity);
  Fte3600AuthMatch best = {0};
  guint mutual = 0;
  for (guint i = 0; i < FTE3600_AUTH_ENROLL_SAMPLES; i++)
    {
      if (cancelled (cancel, cancel_data, error))
        return FALSE;
      Fte3600BriskMatchResult evidence = {0};
      const Fte3600BriskStatus status =
        fpi_fte3600_brisk_match_for_profile (profile, &probe->features, &print->samples[i], &evidence);
      if (cancelled (cancel, cancel_data, error))
        return FALSE;
      /* The matcher reports correspondences before it rejects for consensus. */
      if (evidence.mutual_matches > mutual)
        mutual = evidence.mutual_matches;
      if (status != FPI_BRISK_OK)
        continue;
      Fte3600AuthMatch candidate = { .matched = evidence.authentication_accepted,
                                     .inliers = evidence.inliers,
                                     .mean_hamming = evidence.mean_hamming,
                                     .rms_error = evidence.rms_error };
      if (better (&candidate, &best))
        best = candidate;
    }
  best.mutual_matches = mutual;
  best.uninformative = !best.matched && coverage_enrollment (profile) &&
                       mutual < FTE3600_AUTH_OVERLAP_MUTUAL;
  *result = best;
  return TRUE;
}

gboolean
fte3600_auth_identify_full (const Fte3600AuthProbe *probe, const GPtrArray *prints,
                            gint *best_index, Fte3600AuthMatch *result,
                            Fte3600AuthCancelFunc cancel, gpointer cancel_data, GError **error)
{
  if (best_index)
    *best_index = -1;
  if (result)
    memset (result, 0, sizeof (*result));
  if (!probe || !prints || !best_index || !result || prints->len > G_MAXINT)
    return fail (error, FTE3600_AUTH_INVALID_ARGUMENT, "Invalid identification gallery or outputs");
  if (cancelled (cancel, cancel_data, error))
    return FALSE;
  for (guint i = 0; i < prints->len; i++)
    {
      if (cancelled (cancel, cancel_data, error))
        return FALSE;
      if (!g_ptr_array_index (prints, i))
        return fail (error, FTE3600_AUTH_INVALID_ARGUMENT, "Gallery contains a null template");
    }
  Fte3600AuthMatch best = {0};
  gint index = -1;
  guint compared = 0, mutual = 0;
  gboolean uninformative = TRUE;
  for (guint i = 0; i < prints->len; i++)
    {
      if (cancelled (cancel, cancel_data, error))
        return FALSE;
      const Fte3600AuthTemplate *print = g_ptr_array_index (prints, i);
      if (!fte3600_auth_identity_equal (&probe->identity, &print->identity))
        continue;
      Fte3600AuthMatch candidate;
      if (!fte3600_auth_match_full (probe, print, &candidate, cancel, cancel_data, error))
        return FALSE;
      compared++;
      if (candidate.mutual_matches > mutual)
        mutual = candidate.mutual_matches;
      if (!candidate.uninformative)
        uninformative = FALSE;
      if (better (&candidate, &best))
        {
          best = candidate;
          index = (gint) i;
        }
    }
  if (cancelled (cancel, cancel_data, error))
    return FALSE;
  /* The touch is uninformative only when it overlapped none of the gallery. */
  best.mutual_matches = mutual;
  best.uninformative = !best.matched && compared > 0 && uninformative;
  *result = best;
  *best_index = index;
  return TRUE;
}

static void
put16 (guint8 *p, guint16 value)
{
  p[0] = value >> 8;
  p[1] = value;
}
static void
put32 (guint8 *p, guint32 value)
{
  p[0] = value >> 24;
  p[1] = value >> 16;
  p[2] = value >> 8;
  p[3] = value;
}
static guint16
get16 (const guint8 *p)
{
  return ((guint16) p[0] << 8) | p[1];
}
static guint32
get32 (const guint8 *p)
{
  return ((guint32) p[0] << 24) | ((guint32) p[1] << 16) | ((guint32) p[2] << 8) | p[3];
}
static void
putfloat (guint8 *p, gfloat value)
{
  guint32 bits;

  memcpy (&bits, &value, sizeof (bits));
  put32 (p, bits);
}
static gfloat
getfloat (const guint8 *p)
{
  guint32 bits = get32 (p);
  gfloat value;

  memcpy (&value, &bits, sizeof (value));
  return value;
}

static void
hash_payload (const guint8 *data, gsize length, guint8 digest[DIGEST_SIZE])
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  gsize digest_length = DIGEST_SIZE;
  g_checksum_update (checksum, data, length);
  g_checksum_get_digest (checksum, digest, &digest_length);
}

static GBytes *
encode_samples (const Fte3600AuthIdentity *identity, const Fte3600BriskFeatureSet *samples,
                guint sample_count, const guint8 record_magic[8])
{
  if (!profile_for (identity))
    return NULL;
  gsize size = HEADER_SIZE + DIGEST_SIZE;
  for (guint i = 0; i < sample_count; i++)
    size += 4 + FEATURE_SIZE * samples[i].n_features;
  if (size > FTE3600_AUTH_MAX_TEMPLATE_BYTES)
    return NULL;
  guint8 *data = g_malloc0 (size);
  memcpy (data, record_magic, sizeof (magic));
  put16 (data + 8, FTE3600_AUTH_CODEC_VERSION);
  put16 (data + 10, HEADER_SIZE);
  put32 (data + 12, size);
  put16 (data + 16, FPI_BRISK_EXTRACTOR_SCHEMA_VERSION);
  put16 (data + 18, identity->processing_version);
  put16 (data + 20, FTE3600_BRISK_FAMILY_AUTHENTICATION_POLICY_VERSION);
  put16 (data + 22, PROFILE_VERSION);
  put16 (data + 24, identity->backend_model);
  put16 (data + 26, identity->chip_id);
  put16 (data + 28, identity->width);
  put16 (data + 30, identity->height);
  put16 (data + 32, sample_count);
  put16 (data + 34, FPI_BRISK_DESCRIPTOR_BYTES);
  put16 (data + 36, FEATURE_SIZE);
  gsize at = HEADER_SIZE;
  for (guint i = 0; i < sample_count; i++)
    {
      const Fte3600BriskFeatureSet *sample = &samples[i];
      put16 (data + at, sample->n_features);
      at += 4;
      for (guint j = 0; j < sample->n_features; j++)
        {
          const Fte3600BriskFeature *feature = &sample->features[j];
          putfloat (data + at, feature->x);
          putfloat (data + at + 4, feature->y);
          putfloat (data + at + 8, feature->orientation);
          memcpy (data + at + 12, feature->descriptor, FPI_BRISK_DESCRIPTOR_BYTES);
          at += FEATURE_SIZE;
        }
    }
  hash_payload (data, at, data + at);
  return g_bytes_new_take (data, size);
}

GBytes *
fte3600_auth_template_encode (const Fte3600AuthTemplate *print)
{
  return print ? encode_samples (&print->identity, print->samples, FTE3600_AUTH_ENROLL_SAMPLES, magic) : NULL;
}

GBytes *
fte3600_auth_probe_encode (const Fte3600AuthProbe *probe)
{
  return probe ? encode_samples (&probe->identity, &probe->features, 1, query_magic) : NULL;
}

Fte3600AuthTemplate *
fte3600_auth_template_decode_full (GBytes *bytes, Fte3600AuthCancelFunc cancel,
                                   gpointer cancel_data, GError **error)
{
  if (cancelled (cancel, cancel_data, error))
    return NULL;
  gsize size = 0;
  const guint8 *data = bytes ? g_bytes_get_data (bytes, &size) : NULL;
  if (!data || size < HEADER_SIZE + DIGEST_SIZE || size > FTE3600_AUTH_MAX_TEMPLATE_BYTES)
    goto invalid;
  if (memcmp (data, magic, sizeof (magic)) || get16 (data + 8) != FTE3600_AUTH_CODEC_VERSION ||
      get16 (data + 10) != HEADER_SIZE || get32 (data + 12) != size ||
      get16 (data + 16) != FPI_BRISK_EXTRACTOR_SCHEMA_VERSION ||
      get16 (data + 18) != FTE3600_AUTH_PROCESSING_VERSION ||
      get16 (data + 20) != FTE3600_BRISK_FAMILY_AUTHENTICATION_POLICY_VERSION ||
      get16 (data + 22) != PROFILE_VERSION ||
      get16 (data + 32) != FTE3600_AUTH_ENROLL_SAMPLES ||
      get16 (data + 34) != FPI_BRISK_DESCRIPTOR_BYTES || get16 (data + 36) != FEATURE_SIZE || get16 (data + 38))
    goto invalid;
  Fte3600AuthIdentity identity = { get16 (data + 24), get16 (data + 26),
                                   get16 (data + 28), get16 (data + 30), get16 (data + 18) };
  const Fte3600MatchProfile *profile = profile_for (&identity);
  if (!profile)
    goto invalid;
  guint8 digest[DIGEST_SIZE];
  const gsize end = size - DIGEST_SIZE;
  hash_payload (data, end, digest);
  if (memcmp (data + end, digest, sizeof (digest)))
    goto invalid;
  /* Do not jump into/out of an auto-cleanup scope with an uninitialized
   * pointer. All per-record errors explicitly release the bounded allocation. */
  Fte3600AuthTemplate *print = g_new0 (Fte3600AuthTemplate, 1);
  g_atomic_ref_count_init (&print->references);
  print->identity = identity;
  gsize at = HEADER_SIZE;
  for (guint i = 0; i < FTE3600_AUTH_ENROLL_SAMPLES; i++)
    {
      Fte3600BriskFeatureSet *sample = &print->samples[i];
      if (cancelled (cancel, cancel_data, error))
        goto cancelled_print;
      if (end - at < 4)
        goto invalid_print;
      sample->extractor_schema_version = FPI_BRISK_EXTRACTOR_SCHEMA_VERSION;
      sample->n_features = get16 (data + at);
      if (get16 (data + at + 2) || sample->n_features < FPI_BRISK_MIN_MUTUAL_MATCHES ||
          sample->n_features > FPI_BRISK_MAX_FEATURES)
        goto invalid_print;
      at += 4;
      if (sample->n_features > (end - at) / FEATURE_SIZE)
        goto invalid_print;
      for (guint j = 0; j < sample->n_features; j++)
        {
          Fte3600BriskFeature *feature = &sample->features[j];
          feature->x = getfloat (data + at);
          feature->y = getfloat (data + at + 4);
          feature->orientation = getfloat (data + at + 8);
          memcpy (feature->descriptor, data + at + 12, FPI_BRISK_DESCRIPTOR_BYTES);
          at += FEATURE_SIZE;
        }
      if (!fpi_fte3600_brisk_validate_feature_set_for_profile (profile, sample, NULL))
        goto invalid_print;
      for (guint j = 0; j < i; j++)
        if (same_features (sample, &print->samples[j]))
          goto invalid_print;
    }
  if (at != end)
    goto invalid_print;
  /* Parse every record before the more expensive strict policy checks. */
  for (guint i = 0; i < FTE3600_AUTH_ENROLL_SAMPLES; i++)
    {
      if (cancelled (cancel, cancel_data, error))
        goto cancelled_print;
      Fte3600BriskMatchResult self;
      const Fte3600BriskStatus status =
        fpi_fte3600_brisk_match_for_profile (profile, &print->samples[i], &print->samples[i], &self);
      if (cancelled (cancel, cancel_data, error))
        goto cancelled_print;
      if (status != FPI_BRISK_OK || !self.authentication_accepted)
        goto invalid_print;
      gboolean accepted;
      if (!consistent (print, &print->samples[i], i, &accepted, NULL, cancel, cancel_data, error))
        goto cancelled_print;
      if (!accepted)
        goto invalid_print;
    }
  if (cancelled (cancel, cancel_data, error))
    goto cancelled_print;
  return print;
cancelled_print:
  fte3600_auth_template_free (print);
  return NULL;
invalid_print:
  fte3600_auth_template_free (print);
invalid:
  fail (error, FTE3600_AUTH_INVALID_TEMPLATE, "Malformed or incompatible FTE3600 authentication template");
  return NULL;
}

/* Original entry points retain their ABI and synchronous behavior. */
Fte3600AuthProbe *
fte3600_auth_probe_new (const Fte3600AuthIdentity *identity, const guint8 *pixels,
                        gsize length, gsize stride, GError **error)
{
  return fte3600_auth_probe_new_full (identity, pixels, length, stride, NULL, NULL, error);
}

gboolean
fte3600_auth_enrollment_add (Fte3600AuthEnrollment *enrollment, const guint8 *pixels,
                             gsize length, gsize stride, guint *completed, GError **error)
{
  return fte3600_auth_enrollment_add_full (enrollment, pixels, length, stride, completed,
                                           NULL, NULL, error);
}

Fte3600AuthTemplate *
fte3600_auth_template_decode (GBytes *bytes, GError **error)
{
  return fte3600_auth_template_decode_full (bytes, NULL, NULL, error);
}

gboolean
fte3600_auth_match (const Fte3600AuthProbe *probe, const Fte3600AuthTemplate *print,
                    Fte3600AuthMatch *result, GError **error)
{
  return fte3600_auth_match_full (probe, print, result, NULL, NULL, error);
}

gboolean
fte3600_auth_identify (const Fte3600AuthProbe *probe, const GPtrArray *prints,
                       gint *best_index, Fte3600AuthMatch *result, GError **error)
{
  return fte3600_auth_identify_full (probe, prints, best_index, result, NULL, NULL, error);
}
