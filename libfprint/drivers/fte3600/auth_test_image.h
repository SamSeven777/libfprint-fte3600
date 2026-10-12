/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Mathematical spot fields only; no biometric or hardware data.
 * Shared by auth tests and the actual libfprint fake-backend integration test.
 */
#ifndef FTE3600_AUTH_TEST_IMAGE_H
#define FTE3600_AUTH_TEST_IMAGE_H
#include <glib.h>
#include <math.h>

#define FTE3600_AUTH_TEST_SEED 0x1296a73bu
#define FTE3600_AUTH_TEST_OTHER_SEED 0xb8a30751u

static inline guint32
fte3600_auth_test_random (guint32 *state)
{
  *state ^= *state << 13;
  *state ^= *state >> 17;
  *state ^= *state << 5;
  return *state;
}

/* frames 0..7 are distinct enrollment translations; 8 is held out for query.
* Every dimension is native, and the seed defines a distinct source field. */
static inline void
fte3600_auth_test_image_shift (guint8 *pixels, guint width, guint height,
                               guint32 seed, gint shift_x, gint shift_y)
{
  struct { gdouble x, y, sigma, amplitude;
  } spots[96];
  for (guint i = 0; i < G_N_ELEMENTS (spots); i++)
    {
      spots[i].x = fte3600_auth_test_random (&seed) % width;
      spots[i].y = fte3600_auth_test_random (&seed) % height;
      spots[i].sigma = 1.1 + (fte3600_auth_test_random (&seed) % 240) / 100.0;
      spots[i].amplitude = (fte3600_auth_test_random (&seed) & 1) ? 52 : -52;
    }
  for (guint y = 0; y < height; y++)
    for (guint x = 0; x < width; x++)
      {
        gdouble value = 128;
        for (guint i = 0; i < G_N_ELEMENTS (spots); i++)
          {
            const gdouble dx = (gdouble) x - shift_x - spots[i].x;
            const gdouble dy = (gdouble) y - shift_y - spots[i].y;
            value += spots[i].amplitude * exp (-(dx * dx + dy * dy) /
                                               (2 * spots[i].sigma * spots[i].sigma));
          }
        pixels[y * width + x] = CLAMP (floor (value + 0.5), 2, 253);
      }
}

static inline void
fte3600_auth_test_image (guint8 *pixels, guint width, guint height,
                         guint32 seed, guint frame)
{
  static const gint shifts[9][2] = {
    {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {2, 1}
  };

  g_assert (frame < G_N_ELEMENTS (shifts));
  fte3600_auth_test_image_shift (pixels, width, height, seed, shifts[frame][0], shifts[frame][1]);
}
#endif
