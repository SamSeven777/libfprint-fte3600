# Distribution packages

## Arch Linux

Install `base-devel`, Git, and headers matching your kernel (`linux-headers`,
`linux-lts-headers`, or the equivalent for your kernel). Then:

```sh
git clone https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600/packaging/arch
makepkg -si
sudo pacman -S fprintd
```

The package fetches the main branch, replaces `libfprint`, and installs the
DKMS module, pairing rules and `fte3600-tool`. DKMS builds for kernels with
installed headers. Reboot to load the module and apply the SPI buffer setting.

After reboot:

```sh
sudo fte3600-device-allow --install
sudo fte3600-tool probe /dev/spidevB.C
```

Replace `/dev/spidevB.C` with the sensor's device path. Stop fprintd before
probing if it is using the device. Install any required firmware with
`fte3600-install-firmware --help` as described in the
[installation guide](../docs/fte3600/install.md#firmware), then:

```sh
sudo systemctl restart fprintd.service
fprintd-enroll
fprintd-verify
```

To return to the distribution library, remove the custom package and install
`libfprint`. Remove the generated fprintd drop-in with
`sudo fte3600-device-allow --remove` before removing this package.

## NixOS

Import `packaging/nixos` from a checkout into your system configuration. Keep
the entire checkout available because the module builds its source tree.
For example, with the repository at `/etc/nixos/libfprint-fte3600`:

```nix
{
  imports = [ ./libfprint-fte3600/packaging/nixos ];
  hardware.fte3600 = {
    enable = true;
    devices = [ "FTE3600:01" ];
  };
}
```

Set `devices` to your ACPI instance (`FTE3600:00`, `FTE3600:01`, or
`FTE6600:00`, for example). Inspect `/sys/bus/acpi/devices/` or run the
checkout's `scripts/fte3600-tool.sh info` to find it.

The module builds against `boot.kernelPackages.kernel`, enables spidev with a
65536-byte buffer, installs pairing rules, and rebuilds fprintd against this
libfprint. fprintd receives access to the named sensor's SPI/GPIO/IRQ aliases.
The selected libfprint build includes the FTE3600 driver. Configure other
readers separately if needed. PAM follows the standard NixOS fprintd options.

```sh
sudo nixos-rebuild build
sudo nixos-rebuild switch
sudo reboot
```

The default firmware directory is `/var/lib/fte3600`. For chips
requiring firmware, extract the files using `scripts/install-firmware.sh`
with `--destdir /var/lib`. Alternatively set:

```nix
hardware.fte3600.firmwareDirectory = "${myFirmwarePackage}/lib/firmware/fte3600";
```

FT9365, FW9369 and FT9769 use no external payload in these backends.
After reboot, run `sudo fte3600-tool probe /dev/spidevB.C` with the actual
sensor path, followed by `fprintd-enroll` and `fprintd-verify`.

Pin the checkout to a tested commit for reproducible deployments. If the NixOS
configuration itself is a Git-backed flake, include the imported checkout in
its tracked source or use a pinned non-flake source input. Disable
`hardware.fte3600.enable` and rebuild to remove this integration.
