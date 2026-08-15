# linuwu_sense — vendored

This is the `linuwu_sense` driver from
[Linuwu-Sense](https://github.com/0x7375646F/Linuwu-Sense), GPL-3.0, vendored
at upstream commit `73a25ec243a44ba2b1703e8d0a76fa2735062506`.

It is a fork of the in-tree `acer_wmi` driver that adds the WMI calls
PredatorSense uses on Windows. The in-tree driver exposes none of them:

| | in-tree `acer_wmi` | `linuwu_sense` |
|---|---|---|
| performance profiles | yes | yes |
| fan speed | no | `predator_sense/fan_speed` |
| battery charge limit | no | `predator_sense/battery_limiter` |
| keyboard RGB | no | `four_zoned_kb/per_zone_mode` |
| USB charging, backlight timeout | no | yes |

`Predator PHN16-71` is the model upstream lists as fully supported, and it is
matched by DMI in `linuwu_sense.c`.

## Why it is repackaged here

Two changes to how upstream ships it:

- **DKMS instead of a raw `.ko`.** Upstream's `make install` copies the module
  straight into `/lib/modules/$(uname -r)`, so it disappears on the next
  kernel upgrade. DKMS rebuilds it for every kernel.
- **Signed with the machine's MOK.** Under Secure Boot an unsigned module is
  refused with `Key was rejected by service`. DKMS signs automatically with
  the enrolled key.

Because it replaces `acer_wmi`, the package blacklists that module. Removing
this package undoes the blacklist and the in-tree driver comes back.

## Updating

Re-copy `linuwu_sense.c` from upstream and note the new commit here. The
`Makefile` and `dkms.conf` alongside it are ours, not upstream's.
