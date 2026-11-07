# AUR packaging

`PKGBUILD` builds **`darmoshark-battery-dkms-git`**, following the Arch DKMS
conventions used by `nvidia-dkms` and other DKMS packages:

- the package installs the module **source** into
  `/usr/src/darmoshark-battery-<tag>/`;
- `depends=('dkms')`; the `dkms` package's pacman hooks build and install the
  module for every installed kernel (and rebuild it on kernel updates);
- there is **no** `.install` script and no kernel module built at package-build
  time — installing the `dkms.conf` path is what the hook acts on.

## Source and the local-only checkout

AUR requires a public source. This repository is currently local-only, so the
PKGBUILD points at the intended public URL:

```text
git+https://github.com/ayasaki-exs/darmoshark-battery.git
```

Publish the repository (or replace `url` with your own) before submitting to
the AUR. For a local test without publishing, change the `source` line to the
local checkout:

```bash
source=("git+file://${startdir}/../..")
```

and run `makepkg -si` from this directory. Revert that line before generating
the AUR `.SRCINFO`.

## Build and install

The DKMS build needs the headers of every kernel you want to support. They are
optdepends of `dkms` (for example `linux-headers`), and `linux-headers` pulls
`pahole`/`libelf`, so installing the matching headers is enough:

```sh
sudo pacman -S --needed linux-headers dkms
makepkg -si
```

The package ships:

- `/usr/src/darmoshark-battery-<tag>/` — module source and `dkms.conf`
- `/usr/lib/modules-load.d/darmoshark-battery.conf` — load at boot
- `/usr/share/licenses/darmoshark-battery-dkms-git/LICENSE`

Verify after install:

```sh
dkms status
cat /sys/class/power_supply/darmoshark_battery_0/capacity
upower -d | grep -A15 -i darmoshark
```

## AUR metadata

Generate `.SRCINFO` after changing the source to the public URL:

```sh
makepkg --printsrcinfo > .SRCINFO
```

The `-git` package uses `sha256sums=('SKIP')`, which is correct for a VCS
source. Once the project publishes a `v<version>` release tarball, a
release-versioned `darmoshark-battery-dkms` package can reuse the same
`package()` body with the tarball source and a real checksum.

## Notes

- `provides`/`conflicts` are set against `darmoshark-battery-dkms` so the git
  and release variants cannot be installed side by side.
- The module is GPL-2.0-or-later; the package installs the repository license
  under the package name.
- Loading at boot is also covered by the module's HID modalias alias; the
  `modules-load.d` file makes cold boot independent of udev timing.
