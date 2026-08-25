# Arch Linux and Omarchy

Arch/Omarchy is the only distribution combination currently verified on real
FTE3600 hardware.

## Build the local package

From a committed checkout of this branch:

```sh
cd packaging/arch
makepkg -si
sudo reboot
```

The package replaces the stock `libfprint`, installs the spidev buffer setting,
and grants the sandboxed `fprintd` service GPIO-character-device access. A
reboot is required.

After reboot, enroll and verify before changing any lock-screen integration:

```sh
fprintd-enroll -f left-index-finger "$USER"
fprintd-verify -f left-index-finger "$USER"
```

## Enable only the Omarchy lock screen

The Omarchy shell has separate password and fingerprint PAM flows. Once the
direct verification above succeeds, return to the repository root and install
only the dedicated example:

```sh
sudo install -Dm644 config/pam.d/omarchy-lock-fingerprint \
  /etc/pam.d/omarchy-lock-fingerprint
omarchy restart shell
omarchy system lock
```

Test the enrolled finger, an unenrolled finger, and the password fallback.

Do **not** run `omarchy setup security fingerprint` for this experimental
driver. That general-purpose command installs stock `libfprint` and enables
fingerprint authentication in `sudo` and polkit, exceeding this project's
lock-screen-only safety boundary.

## Roll back

Keep the password available. Remove only the optional lock-screen PAM file,
restore Arch's stock library, and reboot:

```sh
sudo rm /etc/pam.d/omarchy-lock-fingerprint
sudo pacman -S libfprint
sudo reboot
```

Existing host-side templates can be removed separately with
`fprintd-delete "$USER"`; that deletion is not reversible.
