/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_AUTH_H
#define FTE3600_AUTH_H

#include <glib.h>
#include "fte3600_device_session.h"

G_BEGIN_DECLS

#define FTE3600_AUTH_ENROLL_SAMPLES 8u
#define FTE3600_AUTH_PROCESSING_VERSION 0x1001u
#define FTE3600_AUTH_CODEC_VERSION 1u
#define FTE3600_AUTH_MAX_TEMPLATE_BYTES 60000u
/* Fewest descriptor correspondences against any enrolled sample before a
 * strip touch is considered to show enrolled skin at all. Non-overlapping
 * touches of the same finger produce 1-8; same-band touches 15-50. */
#define FTE3600_AUTH_OVERLAP_MUTUAL 8u
#define FTE3600_AUTH_ERROR (fte3600_auth_error_quark ())

typedef enum {
  FTE3600_AUTH_INVALID_ARGUMENT,
  FTE3600_AUTH_UNSUPPORTED,
  FTE3600_AUTH_LOW_QUALITY,
  FTE3600_AUTH_DUPLICATE,
  FTE3600_AUTH_INCONSISTENT,
  FTE3600_AUTH_INCOMPLETE,
  FTE3600_AUTH_INVALID_TEMPLATE,
  FTE3600_AUTH_PROFILE_MISMATCH,
  FTE3600_AUTH_CANCELLED
} Fte3600AuthError;

/* Backend model is the stable hexadecimal name (e.g. 0x9769), not the
 * factory enum. chip_id is the exact inner identity, never that backend's
 * default geometry. All five fields form template compatibility. */
typedef struct
{
  guint16 backend_model, chip_id, width, height, processing_version;
} Fte3600AuthIdentity;

typedef struct _Fte3600AuthEnrollment Fte3600AuthEnrollment;
typedef struct _Fte3600AuthProbe      Fte3600AuthProbe;
typedef struct _Fte3600AuthTemplate   Fte3600AuthTemplate;

typedef struct
{
  gboolean matched;
  guint    inliers;
  gdouble  mean_hamming, rms_error;
  /* Highest correspondence count against any compared sample, including
   * comparisons the matcher rejected. uninformative marks a non-match on a
   * coverage (strip) profile whose best overlap stayed below
   * FTE3600_AUTH_OVERLAP_MUTUAL: the touch showed skin the template does not
   * hold, so it carries no evidence for or against the finger. */
  guint    mutual_matches;
  gboolean uninformative;
} Fte3600AuthMatch;

GQuark fte3600_auth_error_quark (void);
gboolean fte3600_auth_identity_from_status (const struct fte3600_device_status *status,
                                            Fte3600AuthIdentity                *identity,
                                            GError                            **error);
gboolean fte3600_auth_identity_equal (const Fte3600AuthIdentity *a,
                                      const Fte3600AuthIdentity *b);

Fte3600AuthEnrollment *fte3600_auth_enrollment_new (const Fte3600AuthIdentity *identity,
                                                    GError                   **error);
void fte3600_auth_enrollment_free (Fte3600AuthEnrollment *enrollment);
guint fte3600_auth_enrollment_count (const Fte3600AuthEnrollment *enrollment);
/* Original session grayscale, no additional normalization. One extraction per
 * call. Rejected frames never advance enrollment; completed receives current
 * accepted count even on rejection. Caller must enforce finger lift/capture
 * sequencing. Exact active pixels and equal feature multisets (including multiplicity,
 * independent of ordering) are rejected. */
gboolean fte3600_auth_enrollment_add (Fte3600AuthEnrollment *enrollment,
                                      const guint8          *pixels,
                                      gsize                  length,
                                      gsize                  stride,
                                      guint                 *completed,
                                      GError               **error);
Fte3600AuthTemplate *fte3600_auth_enrollment_finish (const Fte3600AuthEnrollment *enrollment,
                                                     GError                     **error);

Fte3600AuthProbe *fte3600_auth_probe_new (const Fte3600AuthIdentity *identity,
                                          const guint8              *pixels,
                                          gsize                      length,
                                          gsize                      stride,
                                          GError                   **error);
void fte3600_auth_probe_free (Fte3600AuthProbe *probe);
/* Ephemeral scanned-print data for libfprint identify reports. Distinct query
 * magic and sample count; template_decode deliberately rejects this encoding.
 * It never upgrades a single query into an enrollment. */
GBytes *fte3600_auth_probe_encode (const Fte3600AuthProbe *probe);

/* The byte codec is independent of FpDevice. A libfprint driver can place its
 * bytes in an ay GVariant as raw FpPrint fpi-data. No biometric pixels are
 * serialized. Decode validates versions, exact lengths, identity, features,
 * duplicate samples and strict enrollment consistency before returning. */
GBytes *fte3600_auth_template_encode (const Fte3600AuthTemplate *print);
Fte3600AuthTemplate *fte3600_auth_template_decode (GBytes  *bytes,
                                                   GError **error);
const Fte3600AuthIdentity *fte3600_auth_template_identity (const Fte3600AuthTemplate *print);
/* References permit bounded owner-scoped reuse of immutable validated data.
 * free releases one reference; the final reference destroys the object. */
Fte3600AuthTemplate *fte3600_auth_template_ref (Fte3600AuthTemplate *print);
void fte3600_auth_template_free (Fte3600AuthTemplate *print);

/* TRUE means evaluation succeeded, including an ordinary non-match. FALSE is
 * an invalid argument or incompatible profile; result is cleared first. */
gboolean fte3600_auth_match (const Fte3600AuthProbe    *probe,
                             const Fte3600AuthTemplate *print,
                             Fte3600AuthMatch          *result,
                             GError                   **error);
/* Array contains borrowed Fte3600AuthTemplate pointers. Other valid profiles
 * are skipped. The best accepted match wins: inliers, then lower hamming,
 * then lower RMS error; exact ties retain the first. -1 means no match.
 * Query extraction is performed by probe_new once for the whole gallery. */
gboolean fte3600_auth_identify (const Fte3600AuthProbe *probe,
                                const GPtrArray        *prints,
                                gint                   *best_index,
                                Fte3600AuthMatch       *result,
                                GError                **error);

/* Optional cooperative cancellation. TRUE means cancel; called in the
 * operation's thread, between bounded unchanged extractor/matcher calls.
 * Return FTE3600_AUTH_CANCELLED, clear match/index outputs, and leave accepted
 * enrollment state untouched on cancellation. The callback and user data must
 * remain valid for the call, and must not mutate/free operation inputs.
 * Template objects are immutable after finish/decode and may be reused while
 * their owner retains them. No process-global template cache is maintained. */
typedef gboolean (*Fte3600AuthCancelFunc) (gpointer user_data);
Fte3600AuthProbe *fte3600_auth_probe_new_full (const Fte3600AuthIdentity *identity,
                                               const guint8              *pixels,
                                               gsize                      length,
                                               gsize                      stride,
                                               Fte3600AuthCancelFunc      cancel,
                                               gpointer                   cancel_data,
                                               GError                   **error);
gboolean fte3600_auth_enrollment_add_full (Fte3600AuthEnrollment *enrollment,
                                           const guint8          *pixels,
                                           gsize                  length,
                                           gsize                  stride,
                                           guint                 *completed,
                                           Fte3600AuthCancelFunc  cancel,
                                           gpointer               cancel_data,
                                           GError               **error);
Fte3600AuthTemplate *fte3600_auth_template_decode_full (GBytes               *bytes,
                                                        Fte3600AuthCancelFunc cancel,
                                                        gpointer              cancel_data,
                                                        GError              **error);
gboolean fte3600_auth_match_full (const Fte3600AuthProbe    *probe,
                                  const Fte3600AuthTemplate *print,
                                  Fte3600AuthMatch          *result,
                                  Fte3600AuthCancelFunc      cancel,
                                  gpointer                   cancel_data,
                                  GError                   **error);
gboolean fte3600_auth_identify_full (const Fte3600AuthProbe *probe,
                                     const GPtrArray        *prints,
                                     gint                   *best_index,
                                     Fte3600AuthMatch       *result,
                                     Fte3600AuthCancelFunc   cancel,
                                     gpointer                cancel_data,
                                     GError                **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Fte3600AuthEnrollment, fte3600_auth_enrollment_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Fte3600AuthProbe, fte3600_auth_probe_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Fte3600AuthTemplate, fte3600_auth_template_free)
G_END_DECLS
#endif
