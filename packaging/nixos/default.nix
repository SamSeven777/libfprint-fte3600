# SPDX-License-Identifier: LGPL-2.1-or-later
{ config, lib, pkgs, utils, ... }:
let
  cfg = config.hardware.fte3600;
  source = lib.cleanSourceWith {
    src = ../..;
    name = "source";
    filter = path: type:
      lib.cleanSourceFilter path type &&
      !(type == "directory" && lib.hasPrefix "build" (baseNameOf path));
  };
  driver = pkgs.stdenv.mkDerivation {
    pname = "libfprint-fte3600";
    version = "1.94.100";
    src = source;
    nativeBuildInputs = with pkgs; [ meson ninja pkg-config python3 ];
    buildInputs = with pkgs; [ glib gusb libgudev systemd ];
    mesonFlags = [
      "-Ddrivers=fte3600" "-Dgtk-examples=false" "-Ddoc=false"
      "-Dintrospection=false" "-Dinstalled-tests=false"
      "-Dudev_rules_dir=${placeholder "out"}/lib/udev/rules.d"
      "-Dudev_hwdb=disabled"
      "-Dfte3600-firmware-dir=${cfg.firmwareDirectory}"
    ];
    postInstall = ''
      install -Dm755 tools/fte3600-tool $out/bin/fte3600-tool
    '';
    meta.platforms = lib.platforms.linux;
  };
  kernel = config.boot.kernelPackages.kernel;
  module = pkgs.stdenv.mkDerivation {
    pname = "fte3600-kernel";
    version = "0.2-${kernel.modDirVersion}";
    src = source;
    sourceRoot = "source/kernel/fte3600";
    nativeBuildInputs = kernel.moduleBuildDependencies;
    makeFlags = [ "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build" ];
    installPhase = ''
      install -Dm644 fte3600.ko $out/lib/modules/${kernel.modDirVersion}/extra/fte3600.ko
    '';
    meta.platforms = lib.platforms.linux;
  };
  pair = pkgs.writeScript "fte3600-pair" (
    "#!${pkgs.python3}/bin/python3\n" +
    lib.concatStringsSep "\n" (lib.tail (lib.splitString "\n" (builtins.readFile ../../scripts/fte3600-pair.py)))
  );
  aliases = lib.concatMap (id: [
    { path = "/dev/fte3600-spi-${id}"; access = "rw"; }
    { path = "/dev/fte3600-gpio-${id}"; access = "rw"; }
    { path = "/dev/fte3600-irq-${id}"; access = "r"; }
  ]) cfg.devices;
  units = map (entry: "${utils.escapeSystemdPath entry.path}.device") aliases;
in {
  options.hardware.fte3600 = {
    enable = lib.mkEnableOption "FTE3600 SPI fingerprint support";
    devices = lib.mkOption {
      type = lib.types.listOf (lib.types.strMatching "FTE(3600|6600):[0-9a-fA-F]+");
      default = [ "FTE3600:00" ];
      example = [ "FTE3600:01" ];
      description = "ACPI instances used to grant fprintd access to paired device aliases.";
    };
    firmwareDirectory = lib.mkOption {
      type = lib.types.str;
      default = "/var/lib/fte3600";
      description = "Directory containing raw firmware payloads; may be a Nix store path.";
    };
  };
  config = lib.mkIf cfg.enable {
    assertions = [ {
      assertion = cfg.devices != [];
      message = "hardware.fte3600.devices must contain the sensor's ACPI instance.";
    } ];
    boot.extraModulePackages = [ module ];
    boot.kernelModules = [ "spidev" "fte3600" ];
    boot.extraModprobeConfig = "options spidev bufsiz=65536";
    services.udev.extraRules = lib.replaceStrings
      [ "/bin/sh" "/usr/libexec/fte3600-pair" ]
      [ "${pkgs.runtimeShell}" "${pair}" ]
      (builtins.readFile ../../config/udev/70-fte3600-acpi-spidev.rules);
    services.fprintd.enable = true;
    services.fprintd.package = pkgs.fprintd.override { libfprint = driver; };
    systemd.services.fprintd = {
      bindsTo = units;
      after = units;
      serviceConfig.DeviceAllow = map (entry: "${entry.path} ${entry.access}") aliases;
    };
    environment.systemPackages = [ driver ];
  };
}
