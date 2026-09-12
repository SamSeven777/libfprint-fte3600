# Arch Linux and Omarchy

Arch/Omarchy is the only distribution combination currently verified on real
FTE3600 hardware.

## Build the local package

The package recipe enables `-Dfte3600_personal_auth=true`. Before installing,
check whether the machine already exposes fingerprint authentication through
global PAM files:

```sh
grep -R --line-number 'pam_fprintd.so' /etc/pam.d 2>/dev/null
```

Do not continue if fingerprint authentication is already wired into login,
`sudo`, polkit, or another unattended path. Remove that integration using the
distribution's supported PAM tool while a password fallback is still tested.

From a committed Git checkout of this branch:

```sh
cd packaging/arch
makepkg -si
sudo reboot
```

The package replaces the stock `libfprint`, installs the spidev buffer setting,
grants the sandboxed `fprintd` service GPIO-character-device access, installs
the exact-A1 SPI runtime-power workaround, and enables the experimental
personal policy. A reboot is required.

On the One-Netbook A1, the package starts the workaround before `fprintd`. It
keeps the sensor's Intel LPSS parent and pxa2xx child out of runtime suspend and
rebinds that controller once when required. The helper checks the complete
known topology first and does nothing on other DMI profiles or when a future
native FTE3600 kernel driver owns the device.

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
