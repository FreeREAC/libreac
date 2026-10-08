# Building libreac

A hand-kept Makefile, no build system to configure. You need a C compiler, `make` and
`python3` (the test suite generates a header).

## The two libraries

```
make                                            # libreac.a
make test                                       # libreac's own suite
make transport REACPW_INCLUDE=<reac-pw>/src     # libreac-transport.a
make test-transport REACPW_INCLUDE=packaging/vendor/reac-pw-headers
```

`libreac-transport` builds standalone for everything except two headers
(`reac_pacer.h`, `reac_role_swap.h`) that still `#include` two pure-declaration headers from
`reac-pw`'s tree (`reac_rate_cfg.h`, `reac_role_cfg.h`). `REACPW_INCLUDE` points the build at a
`reac-pw` checkout's `src/` for those two, or at the vendored snapshot under
`packaging/vendor/reac-pw-headers` (see `packaging/vendor/README.md`); unset, every other object
still builds and only those two fail, loudly, at compile time.

`make test` and `make test-transport` are exactly what CI runs (`.github/workflows/ci.yml`).
Every test runs through `tests/run-test.sh`, which gives it a timeout; a test that exits 77
proved this machine cannot host it (a network-namespace test without `unshare`) and is
reported as SKIP, never as a pass.

## Tools

The wire-analysis tools under `tools/` (`headamp_trace`, `wire_census`, `ctrl_delta`, ...):

```
make wire-tools
```

or name one target (`make corpus_check`, `make headamp_trace`, ...). `make corpus` decodes the
FreeREAC capture corpus and diffs it against `tests/corpus-baseline.txt`; the corpus is private,
so this target is for maintainers who have it.

## Packages

One source builds two libraries, `libreac` and `libreac-transport`, as four binary packages:

| | Fedora | Debian |
| --- | --- | --- |
| libreac | `libreac`, `libreac-devel` | `libreac6`, `libreac-dev` |
| libreac-transport | `libreac-transport`, `libreac-transport-devel` | `libreac-transport7`, `libreac-transport-dev` |

`packaging/libreac.spec` and `debian/` describe them; both build the shared objects from every
`src/*.c` and `transport/src/*.c` and run `make test`. The pkg-config files are the templates
`packaging/*.pc.in`. `openwrt/libreac/Makefile` is the OpenWrt package.

The version and libreac's soname are defined once, in `include/reac/reac.h`;
`tests/conformance-packaging.sh` (part of `make test`) refuses a spec, `debian/` or `CHANGELOG.md`
that disagrees. libreac-transport's soname is its own, in `%global tabi` of the spec and `TABI` of
`debian/rules`, and names the `libreac-transport7` package.

### Changelog

`CHANGELOG.md` is the one changelog. The spec's `%changelog` and `debian/changelog` are generated
from it with `changelog.sh` of [FreeMixer/.github](https://github.com/FreeMixer/.github)
(`.github/actions/changelog/changelog.sh`), and CI refuses a copy that was edited by hand:

```
changelog.sh sync                # rewrite the spec's %changelog and debian/changelog
changelog.sh check -t vX.Y.Z     # what CI runs; the tag must be the newest entry
```

### Releasing

Add the version's entry to `CHANGELOG.md`, run `changelog.sh sync`, set the version in
`include/reac/reac.h` and `Version:` in the spec, then tag `vX.Y.Z`. The tag runs
`.github/workflows/release.yml`, which calls the shared `build-rpm.yml` and `build-deb.yml` workflows of
FreeMixer/.github: signed RPMs for Fedora 44 (x86_64, aarch64) and DEBs for Debian bookworm and trixie
(amd64, arm64) are published to the FreeMixer channel and attached to the GitHub release, whose notes are
the changelog entry. A pull request or a branch runs the same workflows as a dry run that builds, lints
and publishes nothing.

Publish a libreac release before the reac-pw release that builds against it.
