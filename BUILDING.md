# Building libreac

A hand-kept Makefile, no build system to configure. You need a C compiler, `make` and
`python3` (the test suite generates a header).

## The two libraries

```
make                                            # libreac.a
make test                                       # libreac's own suite
make transport                                  # libreac-transport.a
make test-transport
```

`libreac-transport` builds from this tree alone: the rate and role vocabulary its headers
use (the refusal enums, the drivable-rate masks, the role answers) is declared in
`include/reac/reac_cfg.h`, and no `reac-pw` header is needed.

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

## Fuzzing

Every parser of wire bytes has a libFuzzer harness under `fuzz/`, built with clang under
AddressSanitizer and UndefinedBehaviorSanitizer against a `libreac.a` of its own:

```
make fuzz                    # build the harnesses
make fuzz-run FUZZ_TIME=60   # fuzz each one; red on any crash
make fuzz-sabotage           # each planted one-line bug must be found
make fuzz-cov                # line coverage of each parser over its seed corpus
```

CI runs the first three on every push and pull request. `fuzz/README.md` says what each
harness feeds and checks, where its corpus comes from, and how to reproduce a finding.

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
`tests/conformance-packaging.sh` (part of `make test`) refuses a spec or `debian/`
that disagrees. libreac-transport's soname is its own, in `%global tabi` of the spec and `TABI` of
`debian/rules`, and names the `libreac-transport7` package.

### Releasing

Set the version in `include/reac/reac.h`, `Version:` in the spec and the top entry of
`debian/changelog`, then tag `vX.Y.Z`. The tag runs
`.github/workflows/release.yml`, which calls the shared `build-rpm.yml` and `build-deb.yml` workflows of
FreeMixer/.github: signed RPMs for Fedora 44 (x86_64, aarch64) and DEBs for Debian bookworm and trixie
(amd64, arm64) are published to the FreeMixer channel and attached to the GitHub release. Git history is the changelog; no changelog file is maintained. A pull request or a branch runs the same workflows as a dry run that builds, lints
and publishes nothing.

Publish a libreac release before the reac-pw release that builds against it.
