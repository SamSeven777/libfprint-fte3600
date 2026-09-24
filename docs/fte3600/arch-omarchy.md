# Omarchy lock screen

Complete [installation and direct verification](install.md) first, with a
working password fallback. From the repository root, enable only the separate
fingerprint PAM flow:

```sh
sudo install -Dm644 config/pam.d/omarchy-lock-fingerprint \
  /etc/pam.d/omarchy-lock-fingerprint
omarchy restart shell
omarchy system lock
```

Test the enrolled finger, an unenrolled finger, and the password fallback.
Avoid `omarchy setup security fingerprint`: it replaces this fork with stock
libfprint and enables broader PAM integration.

To undo the lock-screen integration and restore Arch's library:

```sh
sudo rm /etc/pam.d/omarchy-lock-fingerprint
sudo pacman -S libfprint
sudo reboot
```
