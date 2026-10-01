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

`packaging/build-rpm.sh` builds every `*.spec` under `packaging/` — today `libreac.spec` and
`libreac-transport.spec` — from the one tarball `packaging/make-tarball.sh` produces, so both
RPMs always ship the same source snapshot. `openwrt/libreac/Makefile` is the OpenWrt package.

`packaging/publish-repo.sh` assembles the signed dnf tree published at
[freereac.github.io/rpm](https://freereac.github.io/rpm), beside reac-pw's packages. A tagged
release runs it through `.github/workflows/release-rpm.yml`:

```
gh workflow run release-rpm.yml -f tag=vX.Y.Z -f sign=true
```

If the workflow cannot run, publish by hand:

```
packaging/publish-repo.sh --rpm-dir DIR --out <checkout of freereac.github.io> --key-id A14B3E1E1F69EBF4
```

then commit and push `rpm/` in that checkout.
