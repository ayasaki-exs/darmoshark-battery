# darmoshark-battery

A minimal, standalone Linux kernel module (DKMS) that exposes a **Darmoshark
(MOJI)** mouse battery as a standard `power_supply` device.

Logitech mice show their battery in desktop battery widgets because the kernel's
`hid-logitech-hidpp` driver registers `hidpp_battery_N`; UPower then hands it to
Solid/KDE. Darmoshark's receiver has no such kernel driver, and UPower has no
userspace API for adding a device. This module speaks the vendor's 64-byte
power query on the configuration collection (`usage page 0xff0a`) and
registers a peripheral `power_supply`, so the battery appears in the same
places:

```text
mouse -> vendor HID channel -> darmoshark_battery -> power_supply -> UPower -> Solid -> KDE Power & Battery
```

## Supported hardware

| USB ID | Device |
|---|---|
| `1915:073c` | Darmoshark M3S Pro (wired) |
| `1915:0723` | Darmoshark M3S Pro (2.4 GHz, 1K NRF receiver) |

## Requirements

- Linux with `CONFIG_HIDRAW` (the module keeps the configuration interface's
  hidraw node available for other tools).
- Kernel headers for the running kernel and DKMS.
- On Arch: `sudo pacman -S --needed linux-headers dkms bc pahole`.

The module is GPL-2.0-or-later.

## Install (DKMS)

Run from this repository (the directory is named `darmoshark-battery`, which is
the DKMS package name):

```sh
make test          # protocol vectors, host only
make               # build the module (needs linux-headers)

sudo dkms add .
sudo dkms build darmoshark-battery/0.1.0
sudo dkms install darmoshark-battery/0.1.0
sudo modprobe darmoshark_battery
```

Load it at boot:

```sh
printf 'darmoshark_battery\n' | sudo tee /etc/modules-load.d/darmoshark-battery.conf
```

The module takes the receiver's configuration interface over from `hid-generic`
without a reboot; only that interface is reprobed. The mouse/keyboard/consumer
interfaces of the same VID/PID keep working.

### Arch Linux (AUR)

`packaging/aur/` provides the `darmoshark-battery-dkms-git` package. It installs
the module source into `/usr/src/darmoshark-battery-<tag>/` and lets the Arch
`dkms` pacman hook build it for every installed kernel — no `.install` script.
The package source is <https://github.com/ayasaki-exs/darmoshark-battery>; see
[packaging/aur/README.md](packaging/aur/README.md) for building and submitting
it.

## Verify

```sh
journalctl -k | tail -30
cat /sys/class/power_supply/darmoshark_battery_0/{capacity,status,scope,model_name}
upower -e | grep darmoshark
upower -d
```

`upower -d` should list a device whose `model` is `Darmoshark M3S Pro ...` and
whose `percentage` matches the mouse. KDE's tray **Power and Battery** applet
then lists it.

The percentage is the only mapped field: the protocol's charging-state byte is
not decoded yet, so `status` stays `unknown`.

## Protocol diagnostic

Read the device directly, without the module:

```sh
make probe
sudo ./build/darmoshark-probe
```

It finds the configuration hidraw node (`0xff0a` usage page), sends the power
query, and prints the percentage and the `dms`/`dms_v2` layout. It needs write
access to the node (root, or a hidraw udev rule for `1915:0723`/`1915:073c`).

## Uninstall

```sh
sudo modprobe -r darmoshark_battery
sudo rm /etc/modules-load.d/darmoshark-battery.conf
sudo dkms remove darmoshark-battery/0.1.0 --all
```

## Limits

- Linux only; this is a bridge to UPower, not a cross-platform driver.
- Battery percentage only; charging state is unknown.
- The module binds the receiver's configuration interface and handles the other
  interfaces of the same VID/PID like `hid-generic` (input still works).
- The vendor channel is shared with other tools (for example OpenLogi's
  Darmoshark DPI support). Polling is once a minute and filters responses; an
  occasional colliding exchange is retried by both sides.
- DKMS rebuilds the module on kernel updates; secure-boot setups need a signed
  module like any out-of-tree driver.

## Protocol reference

The frame layout, checksum and `dms`/`dms_v2` response offsets were
reverse-engineered and verified in the OpenLogi Darmoshark branch:

- `crates/openlogi-device/src/darmoshark.rs`
- `docs/DARMOSHARK.md`
- <https://github.com/ayasaki-exs/OpenLogi/tree/feat/darmoshark-driver>

`darmoshark_protocol.h` is the single C implementation here; `make test` pins
the same wire facts the Rust tests assert. If the protocol changes, update both.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
