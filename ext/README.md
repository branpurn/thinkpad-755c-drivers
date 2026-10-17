# Extension definitions

One `<name>.ext` file per `.tcz`. They are sourced by `scripts/mktcz.sh`:

| variable | meaning |
|---|---|
| `DESC` | one-line description (goes into `<name>.tcz.info`) |
| `MODS` | modules to load, by module name; their `depends:` are resolved from `modinfo` and loaded first |
| `ARGS_<mod>` | default parameters for `<mod>` (dashes become underscores), e.g. 755C defaults |
| `DEPS` | other `.tcz` this one needs (written to `<name>.tcz.dep`) |
| `OPTIONAL=1` | a module that fails to load (no hardware) is reported, not fatal |
| `BINS` | static tools from `tools/<name>/` (built into `out/bin/`) shipped in `usr/local/bin/` |
| `FALLBACK` | modules to load instead if any of `MODS` fails (e.g. vga16fb when the native driver declines the chip) |

At load time the extension's `/usr/local/tce.installed/<name>` inserts the
modules in dependency order, skipping ones already in `/sys/module`. Options
on the kernel command line in the usual `module.param=value` form override
the defaults (e.g. `snd_ad1848.port=0x534 snd_ad1848.irq=9`), the same
convention `modprobe` follows on full distributions.

Every extension is self-contained (it carries its own copies of shared
modules such as `snd.ko`), so load order between extensions does not matter
except that `modutils.tcz` must come first.
