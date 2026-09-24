/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <glib/gstdio.h>
#include "../tools/medion-power.h"

typedef enum
{
  WRITE_NORMAL,
  WRITE_DENIED,
  WRITE_SHORT,
  WRITE_IGNORED,
  WRITE_INTERRUPTED,
} WriteFailure;

typedef struct
{
  gchar *root;
  gchar *parent;
  gchar *leaf;
  GPtrArray *writes;
  MedionPower power;
  guint fail_at;
  WriteFailure failure;
  gboolean stay_suspended;
} Fixture;

static void
set_attribute (const gchar *node, const gchar *name, const gchar *value)
{
  g_autofree gchar *filename = g_build_filename (node, "power", name, NULL);
  g_assert_true (g_file_set_contents (filename, value, -1, NULL));
}

static void
assert_policy (const gchar *node, const gchar *value)
{
  g_autofree gchar *actual = medion_power_read (node, "control", NULL);
  g_assert_cmpstr (actual, ==, value);
}

static ssize_t
fixture_write (int fd, const void *data, size_t size, gpointer user_data)
{
  Fixture *fixture = user_data;
  g_autofree gchar *link = g_strdup_printf ("/proc/self/fd/%d", fd);
  g_autofree gchar *filename = g_file_read_link (link, NULL);
  g_autofree gchar *powerdir = g_path_get_dirname (filename);
  g_autofree gchar *node = g_path_get_dirname (powerdir);
  g_autofree gchar *value = g_strndup (data, size);
  g_strstrip (value);
  g_ptr_array_add (fixture->writes, g_strdup_printf ("%s=%s", node, value));
  if (fixture->writes->len == fixture->fail_at)
    {
      switch (fixture->failure)
        {
        case WRITE_DENIED:
          errno = EACCES;
          return -1;
        case WRITE_SHORT:
          return write (fd, data, 1);
        case WRITE_IGNORED:
          return size;
        case WRITE_INTERRUPTED:
          errno = EINTR;
          return -1;
        case WRITE_NORMAL:
          break;
        }
    }
  /* Ordinary fixture files emulate sysfs replacing the attribute value. */
  g_assert_cmpint (ftruncate (fd, 0), ==, 0);
  ssize_t result = write (fd, data, size);
  g_autofree gchar *status = medion_power_read (node, "runtime_status", NULL);
  if (!fixture->stay_suspended && !g_str_equal (status, "unsupported"))
    set_attribute (node, "runtime_status", g_str_equal (value, "on") ? "active\n" : "suspended\n");
  return result;
}

static void
fixture_setup (Fixture *fixture, gconstpointer data)
{
  (void) data;
  fixture->root = g_dir_make_tmp ("medion-power-test-XXXXXX", NULL);
  g_assert_nonnull (fixture->root);
  fixture->parent = g_build_filename (fixture->root, "pci0000:00", "0000:00:19.0", NULL);
  fixture->leaf = g_build_filename (fixture->parent, "pxa2xx-spi.12", "spi_master", "spi1", "spi-FTE3600:00", NULL);
  const gchar *nodes[] = { fixture->parent, fixture->leaf };
  for (guint i = 0; i < G_N_ELEMENTS (nodes); i++)
    {
      g_autofree gchar *directory = g_build_filename (nodes[i], "power", NULL);
      g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
      set_attribute (nodes[i], "control", "auto\n");
      set_attribute (nodes[i], "runtime_status", i == 0 ? "suspended\n" : "unsupported\n");
    }
  fixture->writes = g_ptr_array_new_with_free_func (g_free);
  fixture->power.io_write = fixture_write;
  fixture->power.user_data = fixture;
  fixture->power.timeout_ms = 10;
}

static void
fixture_teardown (Fixture *fixture, gconstpointer data)
{
  (void) data;
  g_assert_true (medion_power_restore (&fixture->power, NULL));
  medion_power_clear (&fixture->power);
  const gchar *nodes[] = { fixture->parent, fixture->leaf };
  const gchar *attributes[] = { "control", "runtime_status", "runtime_error" };
  for (guint i = 0; i < G_N_ELEMENTS (nodes); i++)
    {
      for (guint j = 0; j < G_N_ELEMENTS (attributes); j++)
        {
          g_autofree gchar *filename = g_build_filename (nodes[i], "power", attributes[j], NULL);
          g_unlink (filename);
        }
      g_autofree gchar *directory = g_build_filename (nodes[i], "power", NULL);
      g_assert_cmpint (g_rmdir (directory), ==, 0);
    }
  gchar *cursor = g_strdup (fixture->leaf);
  while (g_str_has_prefix (cursor, fixture->root))
    {
      g_assert_cmpint (g_rmdir (cursor), ==, 0);
      if (g_str_equal (cursor, fixture->root))
        break;
      gchar *parent = g_path_get_dirname (cursor);
      g_free (cursor);
      cursor = parent;
    }
  g_free (cursor);
  g_ptr_array_unref (fixture->writes);
  g_free (fixture->parent);
  g_free (fixture->leaf);
  g_free (fixture->root);
}

static void
test_round_trip (Fixture *fixture, gconstpointer data)
{
  (void) data;
  g_assert_true (medion_power_prepare (&fixture->power, fixture->leaf, fixture->root, NULL));
  g_assert_cmpuint (fixture->power.nodes->len, ==, 2);
  assert_policy (fixture->parent, "on");
  assert_policy (fixture->leaf, "on");
  g_assert_true (medion_power_restore (&fixture->power, NULL));
  assert_policy (fixture->parent, "auto");
  assert_policy (fixture->leaf, "auto");
  g_autofree gchar *parent_on = g_strconcat (fixture->parent, "=on", NULL);
  g_autofree gchar *leaf_on = g_strconcat (fixture->leaf, "=on", NULL);
  g_autofree gchar *leaf_auto = g_strconcat (fixture->leaf, "=auto", NULL);
  g_autofree gchar *parent_auto = g_strconcat (fixture->parent, "=auto", NULL);
  g_assert_cmpstr (g_ptr_array_index (fixture->writes, 0), ==, parent_on);
  g_assert_cmpstr (g_ptr_array_index (fixture->writes, 1), ==, leaf_on);
  g_assert_cmpstr (g_ptr_array_index (fixture->writes, 2), ==, leaf_auto);
  g_assert_cmpstr (g_ptr_array_index (fixture->writes, 3), ==, parent_auto);
}

static void
test_write_failure (Fixture *fixture, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  fixture->failure = GPOINTER_TO_INT (data);
  fixture->fail_at = 2;
  g_assert_false (medion_power_prepare (&fixture->power, fixture->leaf, fixture->root, &error));
  g_assert_nonnull (error);
  assert_policy (fixture->parent, "auto");
  assert_policy (fixture->leaf, "auto");
  g_assert_cmpuint (fixture->writes->len, ==, 4);
}

static void
test_interrupted_write (Fixture *fixture, gconstpointer data)
{
  (void) data;
  fixture->failure = WRITE_INTERRUPTED;
  fixture->fail_at = 1;
  g_assert_true (medion_power_prepare (&fixture->power, fixture->leaf, fixture->root, NULL));
  g_assert_cmpuint (fixture->writes->len, ==, 3);
}

static void
test_suspended_timeout (Fixture *fixture, gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  fixture->stay_suspended = TRUE;
  g_assert_false (medion_power_prepare (&fixture->power, fixture->leaf, fixture->root, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_IO);
  g_assert_nonnull (strstr (error->message, "suspended"));
  assert_policy (fixture->parent, "auto");
  assert_policy (fixture->leaf, "auto");
  g_assert_cmpuint (fixture->writes->len, ==, 2);
}

static void
test_existing_on (Fixture *fixture, gconstpointer data)
{
  (void) data;
  set_attribute (fixture->parent, "control", "on\n");
  set_attribute (fixture->parent, "runtime_status", "active\n");
  g_assert_true (medion_power_prepare (&fixture->power, fixture->leaf, fixture->root, NULL));
  g_assert_true (medion_power_restore (&fixture->power, NULL));
  assert_policy (fixture->parent, "on");
  assert_policy (fixture->leaf, "auto");
  g_assert_cmpuint (fixture->writes->len, ==, 2);
}

static void
test_restore_failure (Fixture *fixture, gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  g_assert_true (medion_power_prepare (&fixture->power, fixture->leaf, fixture->root, NULL));
  fixture->failure = WRITE_DENIED;
  fixture->fail_at = 3;
  g_assert_false (medion_power_restore (&fixture->power, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  assert_policy (fixture->parent, "auto");
  assert_policy (fixture->leaf, "on");
  /* A failed restore remains tracked and can be retried. */
  g_assert_true (medion_power_restore (&fixture->power, NULL));
  assert_policy (fixture->leaf, "auto");
}

static void
test_outside_root (Fixture *fixture, gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  g_assert_false (medion_power_prepare (&fixture->power, "/tmp", fixture->root, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_assert_cmpuint (fixture->writes->len, ==, 0);
}

static void
test_direct_writer (Fixture *fixture, gconstpointer data)
{
  (void) data;
  MedionPower direct = {0};
  g_autofree gchar *control = g_build_filename (fixture->parent, "power", "control", NULL);
  struct stat before, after;
  g_assert_cmpint (stat (control, &before), ==, 0);
  g_assert_true (medion_power_write (&direct, fixture->parent, "on", NULL));
  g_assert_cmpint (stat (control, &after), ==, 0);
  g_assert_cmpuint (before.st_ino, ==, after.st_ino);
  g_assert_cmpint (after.st_size, ==, before.st_size); /* No rename or truncation. */
  g_assert_true (medion_power_write (&direct, fixture->parent, "auto", NULL));
  g_assert_cmpint (g_unlink (control), ==, 0);
  g_autoptr (GError) error = NULL;
  g_assert_false (medion_power_write (&direct, fixture->parent, "on", &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_assert_false (g_file_test (control, G_FILE_TEST_EXISTS)); /* No O_CREAT. */
  set_attribute (fixture->parent, "control", "auto\n");
}

static void
test_symlink (Fixture *fixture, gconstpointer data)
{
  gboolean escape = GPOINTER_TO_INT (data);
  g_autofree gchar *link = g_build_filename (fixture->root, "selected-spi", NULL);
  g_autoptr (GError) error = NULL;
  g_assert_cmpint (symlink (escape ? "/tmp" : fixture->leaf, link), ==, 0);
  if (escape)
    {
      g_assert_false (medion_power_prepare (&fixture->power, link, fixture->root, &error));
      g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
      g_assert_cmpuint (fixture->writes->len, ==, 0);
    }
  else
    {
      g_assert_true (medion_power_prepare (&fixture->power, link, fixture->root, &error));
      g_assert_no_error (error);
      g_assert_cmpuint (fixture->power.nodes->len, ==, 2);
    }
  g_assert_cmpint (g_unlink (link), ==, 0);
}

static void
test_runtime_error (Fixture *fixture, gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  fixture->stay_suspended = TRUE;
  set_attribute (fixture->parent, "runtime_status", "error\n");
  set_attribute (fixture->parent, "runtime_error", "-5\n");
  g_assert_false (medion_power_prepare (&fixture->power, fixture->leaf, fixture->root, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_IO);
  assert_policy (fixture->parent, "auto");
  assert_policy (fixture->leaf, "auto");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/medion-power/round-trip", Fixture, NULL, fixture_setup, test_round_trip, fixture_teardown);
  g_test_add ("/medion-power/denied-write", Fixture, GINT_TO_POINTER (WRITE_DENIED), fixture_setup, test_write_failure, fixture_teardown);
  g_test_add ("/medion-power/short-write", Fixture, GINT_TO_POINTER (WRITE_SHORT), fixture_setup, test_write_failure, fixture_teardown);
  g_test_add ("/medion-power/readback-mismatch", Fixture, GINT_TO_POINTER (WRITE_IGNORED), fixture_setup, test_write_failure, fixture_teardown);
  g_test_add ("/medion-power/interrupted-write", Fixture, NULL, fixture_setup, test_interrupted_write, fixture_teardown);
  g_test_add ("/medion-power/suspended-timeout", Fixture, NULL, fixture_setup, test_suspended_timeout, fixture_teardown);
  g_test_add ("/medion-power/existing-on", Fixture, NULL, fixture_setup, test_existing_on, fixture_teardown);
  g_test_add ("/medion-power/restore-failure", Fixture, NULL, fixture_setup, test_restore_failure, fixture_teardown);
  g_test_add ("/medion-power/outside-root", Fixture, NULL, fixture_setup, test_outside_root, fixture_teardown);
  g_test_add ("/medion-power/direct-writer", Fixture, NULL, fixture_setup, test_direct_writer, fixture_teardown);
  g_test_add ("/medion-power/device-symlink", Fixture, GINT_TO_POINTER (FALSE), fixture_setup, test_symlink, fixture_teardown);
  g_test_add ("/medion-power/escape-symlink", Fixture, GINT_TO_POINTER (TRUE), fixture_setup, test_symlink, fixture_teardown);
  g_test_add ("/medion-power/runtime-error", Fixture, NULL, fixture_setup, test_runtime_error, fixture_teardown);
  return g_test_run ();
}
