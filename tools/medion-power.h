/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
  gchar *path;
  gchar *saved_control;
  gboolean changed;
} MedionPowerNode;

typedef struct
{
  GPtrArray *nodes; /* Parent first; restoration runs in reverse. */
  /* Optional test backend; production uses write(2). */
  ssize_t (*io_write) (int fd, const void *data, size_t size, gpointer user_data);
  gpointer user_data;
  guint timeout_ms; /* Zero selects the production default of one second. */
} MedionPower;

static inline void
medion_power_node_free (gpointer data)
{
  MedionPowerNode *node = data;
  g_free (node->path);
  g_free (node->saved_control);
  g_free (node);
}

static inline gchar *
medion_power_read (const gchar *path, const gchar *attribute, GError **error)
{
  g_autofree gchar *filename = g_build_filename (path, "power", attribute, NULL);
  gchar *value = NULL;
  if (!g_file_get_contents (filename, &value, NULL, error))
    return NULL;
  return g_strstrip (value);
}

static inline gboolean
medion_power_write (MedionPower *power, const gchar *path,
                    const gchar *value, GError **error)
{
  g_autofree gchar *filename = g_build_filename (path, "power", "control", NULL);
  g_autofree gchar *payload = g_strconcat (value, "\n", NULL);
  gsize size = strlen (payload);
  int fd = open (filename, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Cannot open %s: %s", filename, g_strerror (saved_errno));
      return FALSE;
    }

  ssize_t written;
  do
    written = power->io_write ? power->io_write (fd, payload, size, power->user_data)
                              : write (fd, payload, size);
  while (written < 0 && errno == EINTR);

  int saved_errno = errno;
  int close_result = close (fd);
  int close_errno = errno;
  /* Each sysfs write is a complete command: do not append after a short write. */
  if (written != (ssize_t) size)
    {
      g_set_error (error, G_FILE_ERROR,
                   written < 0 ? g_file_error_from_errno (saved_errno) : G_FILE_ERROR_IO,
                   "Cannot write %s: %s (%zd of %zu bytes)", filename,
                   written < 0 ? g_strerror (saved_errno) : "short write", written, size);
      return FALSE;
    }
  if (close_result < 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (close_errno),
                   "Cannot close %s: %s", filename, g_strerror (close_errno));
      return FALSE;
    }
  return TRUE;
}

static inline void
medion_power_report (MedionPower *power, const gchar *tag)
{
  for (guint i = 0; power->nodes && i < power->nodes->len; i++)
    {
      MedionPowerNode *node = g_ptr_array_index (power->nodes, i);
      g_autofree gchar *control = medion_power_read (node->path, "control", NULL);
      g_autofree gchar *status = medion_power_read (node->path, "runtime_status", NULL);
      g_autofree gchar *runtime_error = medion_power_read (node->path, "runtime_error", NULL);
      g_autofree gchar *pci_state_path = g_build_filename (node->path, "power_state", NULL);
      g_autofree gchar *pci_state = NULL;
      if (g_file_get_contents (pci_state_path, &pci_state, NULL, NULL))
        g_strstrip (pci_state);
      printf ("  Power [%s]: %s control=%s runtime_status=%s runtime_error=%s\n",
              tag, node->path, control ? control : "unreadable",
              status ? status : "unreadable", runtime_error ? runtime_error : "not exposed");
      if (pci_state)
        printf ("    PCI power_state=%s\n", pci_state);
    }
}

static inline gboolean
medion_power_verify_control (MedionPowerNode *node, const gchar *expected, GError **error)
{
  g_autofree gchar *actual = medion_power_read (node->path, "control", error);
  if (!actual)
    return FALSE;
  if (g_strcmp0 (actual, expected) == 0)
    return TRUE;
  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
               "%s/power/control read back '%s', expected '%s'", node->path, actual, expected);
  return FALSE;
}

static inline gboolean
medion_power_restore (MedionPower *power, GError **error)
{
  gboolean success = TRUE;
  for (guint i = power->nodes ? power->nodes->len : 0; i > 0; i--)
    {
      MedionPowerNode *node = g_ptr_array_index (power->nodes, i - 1);
      g_autoptr (GError) local_error = NULL;
      if (!node->changed)
        continue;
      if (!medion_power_write (power, node->path, node->saved_control, &local_error) ||
          !medion_power_verify_control (node, node->saved_control, &local_error))
        {
          g_printerr ("Power policy restoration failed: %s\n", local_error->message);
          if (success && error)
            g_propagate_error (error, g_steal_pointer (&local_error));
          success = FALSE;
          continue;
        }
      node->changed = FALSE;
    }
  return success;
}

static inline void
medion_power_clear (MedionPower *power)
{
  g_clear_pointer (&power->nodes, g_ptr_array_unref);
}

static inline gboolean
medion_power_wait (MedionPower *power, MedionPowerNode *node, GError **error)
{
  gint64 deadline = g_get_monotonic_time () +
                    (power->timeout_ms ? power->timeout_ms : 1000) * (gint64) 1000;
  for (;;)
    {
      g_autofree gchar *status = medion_power_read (node->path, "runtime_status", error);
      if (!status)
        return FALSE;
      /* "unsupported" means runtime PM is disabled, not that the rail is off. */
      if (g_str_equal (status, "active") || g_str_equal (status, "unsupported"))
        return TRUE;
      if (g_str_equal (status, "error") || g_get_monotonic_time () >= deadline)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
                       "%s runtime_status is '%s' after requesting control=on",
                       node->path, status);
          return FALSE;
        }
      g_usleep (10000);
    }
}

/* device_path must be the SPI device selected by the caller, not a guessed bus
 * number. sysfs_devices_root is /sys/devices in production, a fixture in tests. */
static inline gboolean
medion_power_prepare (MedionPower *power, const gchar *device_path,
                      const gchar *sysfs_devices_root, GError **error)
{
  g_autofree gchar *device = realpath (device_path, NULL);
  g_autofree gchar *root = realpath (sysfs_devices_root, NULL);
  g_autofree gchar *root_prefix = root ? g_strconcat (root, "/", NULL) : NULL;
  g_autofree gchar *cursor = NULL;
  g_autoptr (GError) local_error = NULL;
  if (power->nodes)
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                           "Power context already prepared; restore and clear it first");
      return FALSE;
    }
  if (!device || !root || !g_str_has_prefix (device, root_prefix))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "SPI device '%s' is not inside '%s'", device_path, sysfs_devices_root);
      return FALSE;
    }

  power->nodes = g_ptr_array_new_with_free_func (medion_power_node_free);
  cursor = g_strdup (device);
  for (;;)
    {
      g_autofree gchar *control = g_build_filename (cursor, "power", "control", NULL);
      struct stat st;
      if (stat (control, &st) == 0)
        {
          MedionPowerNode *node = g_new0 (MedionPowerNode, 1);
          node->path = g_strdup (cursor);
          node->saved_control = medion_power_read (cursor, "control", &local_error);
          if (!node->saved_control)
            {
              medion_power_node_free (node);
              goto fail;
            }
          if (!g_str_equal (node->saved_control, "on") &&
              !g_str_equal (node->saved_control, "auto"))
            {
              g_set_error (&local_error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                           "%s contains invalid policy '%s'", control, node->saved_control);
              medion_power_node_free (node);
              goto fail;
            }
          g_ptr_array_insert (power->nodes, 0, node);
        }
      else if (errno != ENOENT)
        {
          int saved_errno = errno;
          g_set_error (&local_error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                       "Cannot inspect %s: %s", control, g_strerror (saved_errno));
          goto fail;
        }
      if (g_str_equal (cursor, root))
        break;
      gchar *parent = g_path_get_dirname (cursor);
      g_free (cursor);
      cursor = parent;
    }
  if (power->nodes->len == 0)
    {
      g_set_error_literal (&local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                           "No runtime power controls found on the selected SPI hierarchy");
      goto fail;
    }

  medion_power_report (power, "before");
  for (guint i = 0; i < power->nodes->len; i++)
    {
      MedionPowerNode *node = g_ptr_array_index (power->nodes, i);
      if (!g_str_equal (node->saved_control, "on"))
        {
          /* Record before writing: even a failed/short write may change sysfs. */
          node->changed = TRUE;
          if (!medion_power_write (power, node->path, "on", &local_error))
            goto fail;
        }
      if (!medion_power_verify_control (node, "on", &local_error) ||
          !medion_power_wait (power, node, &local_error))
        goto fail;
    }
  medion_power_report (power, "held on");
  return TRUE;

fail:
  medion_power_report (power, "failed");
  medion_power_restore (power, NULL);
  g_propagate_error (error, g_steal_pointer (&local_error));
  return FALSE;
}
