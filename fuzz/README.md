# Fuzzing libreac

libreac decodes Ethernet frames a stagebox or a desk sends, and the bytes it reads off a
capture file. Every function that reads those bytes has a libFuzzer harness here, built with
clang under AddressSanitizer and UndefinedBehaviorSanitizer and run in CI on every push and
pull request.

Each harness is one plain C file defining `LLVMFuzzerTestOneInput`. It reads its input as a
small program: a few header bytes that set up whatever state the parser needs (our MAC, a
mode descriptor, a buffer capacity), then a sequence of frames or operations, until the input
ends. Every input means something, and the end of the input ends it. No harness reads a clock,
the environment, a file or the network, starts a thread or installs a signal handler; a time
a module needs is a time the input chooses. Properties are checked with `if (!cond) abort();`,
never `assert()`, every outcome an input can choose returns 0, and an input of 64 MiB or more
is refused. Each frame is copied into an allocation of exactly its length, so a read one byte
past it is a sanitizer report instead of a quiet read of the next frame.

The harnesses call the decoders as they are. They do not re-implement the protocol: where a
property needs to know what a frame should say, it asks a second libreac function (the encoder
for the decoder, the builder for the parser) and requires the two to agree.

## The harnesses

| Harness | Parsers it feeds | Input |
| --- | --- | --- |
| `fuzz_decode` | `reac_frame_inspect`, `reac_decode`, `reac_decode_plain_le` (`src/reac_decode.c`); `reac_upstream_channels`, `reac_upstream_decode` (`src/reac_upstream.c`); the frame helpers of `src/reac.c` | a mode selector (three shipped descriptors, or one built from the next two bytes), then one frame |
| `fuzz_ctrl` | `reac_ctrl_parse`, `reac_ctrl_identity_reply`, `reac_ctrl_identity_fragment`, `reac_ctrl_headamp_record_verify`, `reac_box_catalogue_match`, the two checksums (`src/reac_ctrlblk.c`); `reac_ctrl_classify_box_frame` (`src/reac_ctrl.c`); `reac_ports_parse`, `reac_ports_unknown` (`src/reac_ports.c`) | our MAC, a flag for the builder round trip, then up to 256 length-prefixed frames, with the identity fragments reassembled across them |
| `fuzz_identity` | `reac_identity_ingest` and the two formatters (`src/reac_identity.c`); `reac_box_family_of`, `reac_box_name`, `reac_box_catalogue_defect` (`src/reac_box_facts.c`) | up to 64 identity-page replies (address, length, payload), then widths and output capacities |
| `fuzz_disco` | `reac_disco_classify`, `reac_disco_classify_on_segment`, the announce gate, the table and its JSON (`src/reac_disco.c`); `reac_hunt_observe`, `reac_hunt_step` (`src/reac_hunt.c`) | our MAC, a pin, then up to 512 timed operations: a frame through one of three doors, or an age, serialise and step |
| `fuzz_scene` | `reac_ctrl_build_scene_step`, `reac_ctrl_scene_set_mac`, `reac_ctrl_scene_build` (`src/reac_ctrlblk.c`) over the body in `src/reac_scene_body.c` | a body length, a step index, a MAC, and bytes XORed over the shipped scene body |
| `fuzz_boxreg` | `reac_boxreg_declare`, `_add`, `_find`, `_set_name` (`src/reac_boxreg.c`) | a fabric size, then up to 64 calls with their MACs, widths, bases and names |
| `fuzz_knock` | `reac_knock_init`, `_heard`, `_step` (`src/reac_knock.c`) | a start time, then up to 4096 events: the clock advances, a frame is heard, the knock is stepped |
| `fuzz_grant` | `reac_grant_alloc_fits`, `reac_grant_allocate`, `reac_grant_build_sweep` (`src/reac_grant.c`); the head-amp table and scheduler (`src/reac_headamp_tx.c`) | a base, a width, a sweep buffer size, head-amp cells, a hand-built allocation, a scene replay, and scheduler frames |
| `fuzz_pcap` | `pcap_source_open`, `pcap_source_next`, `pcap_source_close` (`src/pcap_source.c`) | a record-buffer capacity, then the pcap file itself |

`reac_scene_body.c` holds no parser: it is the 8904-byte body libreac ships. The code that
indexes into a scene body is the transfer framing in `reac_ctrlblk.c`, so that is what
`fuzz_scene` drives. The knock, the grant and the box registry read no frame either; what they
are handed (when a frame was heard, the base a box announced, the width it declared, its MAC)
comes off the wire one step earlier, so their harnesses are sequences of the calls a master
makes with those values chosen by the input.

`pcap_source_open()` takes a path. `fuzz_pcap` is linked with `-Wl,--wrap=fopen`: the one path
`<fuzz-input>` opens the input through `fmemopen()`, and every other path reaches the real
`fopen()`. The library is unchanged and opens no file.

### What each harness checks

Beyond "no crash, no sanitizer report", each harness states properties of the code it drives.
The header comment of every harness lists them; in short:

- **decode**: a frame decodes exactly when `reac_frame_inspect` calls it valid and the geometry
  fits; decoding and re-encoding through `reac_braid_encode` gives back the audio region byte
  for byte, downstream and upstream; `reac_frame_clean_len` only strips the two-byte FCS residue.
- **ctrl**: `reac_ctrl_parse` reads the addresses and counter where the frame carries them; the
  box classifier answers only for a Roland source that is not us and, FILLER aside, only when the
  block checksum closes; an identity payload points inside its frame and comes only out of a
  record whose checksums close; a port table is whole four-channel slots; and what
  `reac_ctrl_build_as` emits for each catalogue row parses back as its kind, matches its row,
  declares the row's widths and reassembles to the row's own identity.
- **identity**: an accepted firmware is four decimal digits and prints as them; a model name is the
  payload's ASCII up to its first NUL, never empty, always terminated; every formatter refuses or
  writes a terminated string inside its capacity; `reac_box_name`'s three strings agree.
- **disco**: a sighting is the frame's own source and never us; no table holds more than
  `REAC_DISCO_MAX` peers or a MAC twice; a box never turns into a master; `seq` moves on every
  change and never back; a full table always fits `REAC_DISCO_JSON_MAX`.
- **scene**: the doors accept exactly an 8904-byte body and a step in range; every step's block
  sums to zero; the declared lengths sum to the declared total and the payloads concatenated are
  the body; `reac_ctrl_parse` reads each step back as `SCENE_TRANSFER`.
- **boxreg**: at most five boxes, each an even width inside the fabric, no two overlapping, no MAC
  twice, a pinned base kept, `add` idempotent, the zero MAC never a box.
- **knock**: the licence to drive comes at most once, only after `REAC_KNOCK_LISTEN_NS` of silence
  since the wire came up and since the last frame heard, and is not withheld once that is true.
- **grant**: a box is admitted exactly when its slots lie in the head-amp space; a sweep is
  `REAC_GRANT_SWEEP_LEN(width)` rows, each closed, its group-A rows parsing back as the head-amp
  records for the right channels and values, its group B the identity poll; the scheduler only
  emits cells the table can hold.
- **pcap**: only the two classic magics open; a record handed back fits the buffer and equals the
  file's record, its 802.1Q tag stripped exactly when it carried one; the timestamp is the
  header's.

## The seed corpus

`fuzz/corpus/<harness>/` is written by `fuzz/gen-corpus.c` (`make fuzz-corpus`) from what the
repository already holds: the real control blocks of `tests/ctrl_fixtures.inc`, the
S-4000H-0832 session of `tests/box_0832_fixtures.inc`, the captured box returns of
`tests/upstream_fixtures.inc`, the scene body libreac ships, and frames libreac's own builders
emit for every catalogue row. Beside the well-formed frames it writes one frame per refusal a
parser makes (a short frame, a wrong EtherType, a checksum that does not close, a SysEx without
its `f7`, a fragment from the wrong box, a record cut short...), so the corpus reaches the
refusal paths as well as the accepting ones. Nothing is fetched and no private capture is read;
the generator is deterministic, and the corpus is committed so CI and a release tarball fuzz
from the same seeds.

## The coverage witness

`make fuzz-cov` builds each harness for `llvm-cov` (no sanitizers,
`-fprofile-instr-generate -fcoverage-mapping`), runs it once over its corpus (`-runs=0`) and
prints the line coverage of the parser functions it covers, then every line of them no seed
reached. The function lists are in `fuzz/coverage.sh`; a file's builders and socket code are not
a parser's and are not counted. `fuzz/coverage-floor.txt` holds the number of covered lines
each harness reaches today, and `make fuzz-cov` is red when a harness falls below it, so a lost
seed or a parser that grows lines its corpus never reaches cannot pass unnoticed; CI runs it.

| Harness | Parser lines | Covered | Lines no input can reach |
| --- | ---: | ---: | --- |
| decode | 104 | 100.00% | |
| ctrl | 277 | 99.64% | `reac_box_catalogue_match`: the `continue` for a row that synthesises no declaration; every row does |
| identity | 195 | 100.00% | |
| disco | 355 | 99.15% | `box_present`, `desk_geometry_live`: the `continue`s for a stale entry and for our own MAC; the hunt ages its table before it decides, and our own frames never enter it |
| scene | 65 | 100.00% | |
| boxreg | 87 | 100.00% | |
| knock | 28 | 100.00% | |
| grant | 142 | 99.30% | `reac_grant_build_sweep`: the width re-check after `reac_grant_alloc_fits` has already refused that width |
| pcap | 78 | 100.00% | |

## The mutants

`make fuzz-sabotage` proves each harness can find a bug in the code it covers. For every harness,
`fuzz/sabotage/<harness>.patch` is a one-line change to the parser; the target applies it,
rebuilds, fuzzes for at most 30 s from the corpus, restores the source with `git checkout --`,
and is red unless the fuzzer crashed.

| Harness | Mutant | Found by |
| --- | --- | --- |
| decode | `reac_decode` drops the geometry bound (`nch * ns * 3 <= 1440`) | ASan, heap overflow reading past the frame, within the first second |
| ctrl | `reac_ctrl_parse` accepts a frame from 16 bytes instead of 50 | the short-frame seed, the property that a frame under 50 bytes is `NONE` |
| identity | `store_model_name` copies `len` bytes after the kind byte instead of `len - 1` | ASan, heap overflow reading past the payload, within the first second |
| disco | `reac_disco_table_observe` saturates at `n > MAX` instead of `n >= MAX` | the nine-peer seed, ASan (a ninth entry written past the table) |
| scene | `reac_ctrl_build_scene_step` accepts any body of at least 24 bytes | the short-body seeds: ASan on the read past the body, or the property that only an 8904-byte body is accepted |
| boxreg | `reac_boxreg_declare` drops its `n >= REAC_BOXREG_MAX_BOXES` check | the no-room seed, ASan (a sixth box written past the registry) |
| knock | `reac_knock_step` compares against `opened_ns` instead of `due_ns` | the silent-wire seed, the silence property |
| grant | `reac_grant_alloc_fits` lets a box's last slot reach `0x30` | the admission property, within the first second |
| pcap | `pcap_source_next` reads a record one byte larger than the buffer | ASan, heap overflow in `fread`, within the first second |

Five of the nine are caught by a seed on its first execution: those seeds exist to reach the
refusal the mutant removes, which is what makes them the coverage witness for it. The other four
are found by fuzzing, in under a second.

## Running

```
make fuzz                    # build every harness into build/fuzz/
make fuzz-run FUZZ_TIME=60   # fuzz each for 60 s; red on any crash
make fuzz-sabotage           # every mutant must be found
make fuzz-cov                # the coverage witness
make fuzz-corpus             # regenerate fuzz/corpus/ from the fixtures
```

They need clang with libFuzzer and, for `fuzz-cov`, `llvm-profdata` and `llvm-cov`. The harnesses
link a `libreac.a` of their own, compiled from the same sources with the sanitizers into
`build/fuzz/obj/`, so the ordinary `make` and `make test` builds are never touched. libc and the
sanitizer runtimes stay shared, because AddressSanitizer does not support a fully static
executable; a static riscv64 build for verification links the same harness sources against a
static `libreac.a` of its own.

One harness by hand, with its corpus and a scratch directory for what it finds:

```
mkdir -p /tmp/new && build/fuzz/fuzz_ctrl -max_total_time=60 /tmp/new fuzz/corpus/ctrl
```

## Reproducing a finding

`make fuzz-run` writes the input that crashed a harness to
`fuzz/findings/<harness>-crash-<sha1>` (or `leak-`, `timeout-`, `oom-`), with the harness's log
beside it as `fuzz/findings/<harness>.log`; CI uploads the directory as the `fuzz-findings`
artifact. To replay it:

```
make fuzz
build/fuzz/fuzz_<harness> fuzz/findings/<harness>-crash-<sha1>
```

prints the sanitizer report or the aborted property with its stack. Once the bug is fixed, copy
the input into `fuzz/corpus/<harness>/` under a name that says what it is, so every later run
replays it first and the fix stays pinned.
`fuzz/corpus/grant/replay-base-past-table` is one: the input with which `fuzz_grant` found
`reac_headamp_tx_arm_scene` wrapping its clamp for a base past the head-amp table.
