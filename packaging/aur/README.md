# AUR packaging

`PKGBUILD` builds **`darmoshark-battery-dkms-git`**, following the Arch DKMS
conventions used by `nvidia-dkms` and other DKMS packages:

- the package installs the module **source** into
  `/usr/src/darmoshark-battery-<tag>/`;
- `depends=('dkms')`; the `dkms` package's pacman hooks build and install the
  module for every installed kernel (and rebuild it on kernel updates);
- there is **no** `.install` script and no kernel module built at package-build
  time — installing the `dkms.conf` path is what the hook acts on.

## Source

AUR requires a public source. The PKGBUILD points at the public repository:

```text
git+https://github.com/ayasaki-exs/darmoshark-battery.git
```

The `v0.1.0` tag is pushed; `pkgver()` uses `git describe` and the DKMS source
directory keeps the last tag version. If you work on a local-only checkout,
replace the `source` line with the absolute path to it:

```bash
source=("git+file:///home/akuma/repos/darmoshark-battery")
```

and revert the line (plus regenerate `.SRCINFO`) before submitting.

## Build and install

The package declares `depends=('dkms' 'pahole')`: the DKMS build happens at
install time through the pacman hook, so the BTF generator is a runtime
dependency, not a `makedepends` one. The headers of every kernel you want to
support are optdepends of `dkms` (for example `linux-headers`); installing the
matching headers also pulls `pahole` and `libelf`. `bc` is not needed: external
module builds reuse the pre-generated kernel headers; it is only required when
preparing a kernel tree from source.

`linux-headers` is deliberately not a hard dependency: DKMS builds for every
installed kernel, and the headers are per-flavor optdepends of `dkms`
(`linux-headers`, `linux-lts-headers`, `linux-zen-headers`,
`linux-hardened-headers`). Install the ones matching the kernels you use.

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

Generate `.SRCINFO` after any PKGBUILD change:

```sh
makepkg --printsrcinfo > .SRCINFO
```

The `-git` package uses `sha256sums=('SKIP')`, which is correct for a VCS
source. Now that `v0.1.0` is tagged on GitHub, a release-versioned
`darmoshark-battery-dkms` package can use the tag archive and a real checksum
instead.

## Submitting to the AUR

AUR uses its own git remote and an AUR account with an SSH key. For the first
submission, initialize a repository with the AUR remote, add `PKGBUILD` and
`.SRCINFO`, and push to `master`:

```sh
git init darmoshark-battery-dkms-git
cd darmoshark-battery-dkms-git
cp /path/to/darmoshark-battery/packaging/aur/{PKGBUILD,.SRCINFO} .
git add PKGBUILD .SRCINFO
git commit -m "Initial import"
git remote add origin ssh://aur@aur.archlinux.org/darmoshark-battery-dkms-git.git
git push origin master
```

For later updates, clone the AUR repository, refresh `.SRCINFO`, commit and
push.

## Notes

- `provides`/`conflicts` are set against `darmoshark-battery-dkms` so the git
  and release variants cannot be installed side by side.
- The module is GPL-2.0-or-later; the package installs the repository license
  under the package name.
- Loading at boot is also covered by the module's HID modalias alias; the
  `modules-load.d` file makes cold boot independent of udev timing.
