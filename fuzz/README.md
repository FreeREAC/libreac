# Fuzzing libreac

libreac decodes Ethernet frames a stagebox or a desk sends, the bytes it reads off a
capture file, and the rtnetlink replies libreac-transport reads back from the kernel. Every
function that reads those bytes has a libFuzzer harness here, built with
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

The harnesses call the decoders as they are. Where a property needs to know what a frame
should say, the harness works the answer out from what the protocol states, never from the
function it judges: a checksum is summed in the harness, a geometry is counted from the frame
length, the braid's byte map and the identity poll are written out. Every such constant or rule
carries a `contract:` line in its comment naming the header line or the `reac.ksy` field it
comes from (reac-protocol's `spec/reac.ksy`), so the protocol, not the harness, owns it. Where
two libreac functions must agree (the encoder and the decoder, a builder and its parser), the
harness requires that as well, beside the check against the contract.

The harnesses are Theorem Labs' rewrite of ours (theorem-labs/oss-verify, branch
libreac/harness-fixes), taken in 1.8.0 with their modification notice, and extended here.

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
| `fuzz_qdisc` | `reac_etf_qdisc_state`, `reac_etf_qdisc_stats_read` and the RTM_GETQDISC dump walker under them (`transport/src/reac_etf_qdisc.c`) | up to three reads, each a device index, which calls fail, and the reply chunks the fake kernel hands out, message by message |

`reac_scene_body.c` holds no parser: it is the 8904-byte body libreac ships. The code that
indexes into a scene body is the transfer framing in `reac_ctrlblk.c`, so that is what
`fuzz_scene` drives. The knock, the grant and the box registry read no frame either; what they
are handed (when a frame was heard, the base a box announced, the width it declared, its MAC)
comes off the wire one step earlier, so their harnesses are sequences of the calls a master
makes with those values chosen by the input.

`pcap_source_open()` takes a path. `fuzz_pcap` is linked with `-Wl,--wrap=fopen`: the one path
`<fuzz-input>` opens the input through `fmemopen()`, and every other path reaches the real
`fopen()`. The library is unchanged and opens no file.

`reac_etf_qdisc.c` talks to a netlink socket. For `fuzz_qdisc` that one file of libreac-transport
is compiled with `socket`, `send`, `poll`, `recv` and `close` renamed (the Makefile's `FUZZ_NET`)
to the fake kernel of `fuzz/netlink.h`, which hands the reply chunks the input spelled to each
`recv()`, says through `poll()` whether one is queued, keeps every request sent, and fills the
rest of the reader's buffer past each reply with error messages it never sent, so a walk that
runs past the bytes read answers an error the harness can see instead of whatever was there.
The source is unchanged; the three fuzz builds each have an object of it with the renames.

### What each harness checks

Beyond "no crash, no sanitizer report", each harness states properties of the code it drives.
The header comment of every harness lists them; in short:

- **decode**: a frame is valid exactly when it has 1492 bytes, EtherType 88 19 and the end
  marker, in both directions; a frame decodes exactly when it is valid and the geometry fits;
  a refused decode leaves its output untouched; decoding and re-encoding through
  `reac_braid_encode` gives back the audio region byte for byte, and every decoded sample is
  the three bytes the braid map of `reac_braid.h` names, written out in the harness; plain LE is
  a straight copy, sample by sample; a box return's width is its length's, at a box width only;
  `reac_frame_clean_len` only strips the two-byte FCS residue.
- **ctrl**: `reac_ctrl_parse` reads the addresses and counter where the frame carries them; the
  box classifier answers only for a Roland source that is not us and, FILLER aside, only when the
  block sums to zero (summed in the harness); an identity payload points inside its frame and
  comes only out of a record whose block and DT1 sums close; a port table is whole four-channel
  slots; a catalogue row is the first whose declaration is the frame's block; and what
  `reac_ctrl_build_as` emits for each catalogue row parses back as its kind, matches its row,
  declares the row's widths and reassembles to the row's own identity.
- **identity**: a well-formed record is accepted and an ignored one changes nothing; an accepted
  firmware is four decimal digits and prints as them; a model name is the payload's ASCII up to
  its first NUL, never empty, always terminated; the decode reads no byte past its length; the
  formatters print the exact strings the header describes; `reac_box_name`'s three strings are
  the expected ones, and the catalogue defect mask is exact.
- **disco**: a sighting is the frame's own source and never us, its geometry the length's; no
  table holds more than `REAC_DISCO_MAX` peers or a MAC twice; a box never turns into a master;
  every observe and age changes the table as the header says, and `seq` with it; a
  serialisation is one JSON array with one object per entry, each field as the header gives it.
- **scene**: the doors accept exactly an 8904-byte body and a step in range, and a refusal writes
  nothing; every step is a cd ea link-1 bulk block whose 32 bytes sum to zero and whose payload
  is the body's slice, the LAST step's trailer included; the declared lengths sum to the declared
  total and the payloads concatenated are the body; `reac_ctrl_parse` reads each step back as
  `SCENE_TRANSFER`.
- **boxreg**: at most five boxes, each an even width inside the fabric, no two overlapping, no MAC
  twice; every call gives the result the header states, worked out on the table before it; no
  call moves a box or rebinds a MAC; a pinned base kept, `add` idempotent, the zero MAC never a box.
- **knock**: the licence to drive comes at most once, only after `REAC_KNOCK_LISTEN_NS` of silence
  since the wire came up and since the last frame heard, and is not withheld once that is true;
  the state follows the header's model.
- **grant**: a box is admitted exactly when its slots lie in the head-amp space; a sweep is
  `REAC_GRANT_SWEEP_LEN(width)` rows in the header's order, each closed, its two head rows the
  measured frames, its group-A rows parsing back as the head-amp records for the right channels
  and values with their DT1 sums closed, its group B the identity poll for the six addresses the
  header names; an unset cell carries the header's default; the scheduler only emits cells the
  table can hold, and an unset cell at most once.
- **qdisc**: one request per read, a dump of every qdisc; the reader closes the socket it
  opened and uses no other; a dump that reached `NLMSG_DONE` answers 0 and exactly the sums over
  the device's `etf` qdiscs (`TCA_STATS2`, else `TCA_STATS`), another device, another kind and an
  unterminated kind counting nothing; a dump that failed answers its error and leaves the
  counters untouched, replies that stop are `-ETIMEDOUT` within 64 reads, and nothing past the
  bytes read is walked; the state is `UNREADABLE` when no reply arrived, `PRESENT` when an `etf`
  was seen anywhere on the device, else `NONE`, with the root qdisc named and cut to the buffer.
- **pcap**: only the two classic magics open; a record handed back fits the buffer and equals the
  file's record, its 802.1Q tag stripped exactly when it carried one; the timestamp is the
  header's; a record of 0 bytes is skipped like one larger than the buffer, and the walk ends with
  0 exactly at the end of the file (since 1.8.0: 0 means the end of the file and nothing else).

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
from the same seeds. The seeds a mutant record names (below) are written by the generator too,
under the names their records give them; a seed the generator could not derive would be a
committed file beside the rest, and today there is none.

## The coverage witness

`make fuzz-cov` builds each harness for `llvm-cov` (no sanitizers,
`-fprofile-instr-generate -fcoverage-mapping`), runs it once over its corpus (`-runs=0`) and
prints the line coverage of the parser functions it covers, then every line of them no seed
reached. The function lists are in `fuzz/coverage.sh`; a file's builders and socket code are not
a parser's and are not counted. `fuzz/coverage-floor.txt` holds the number of covered lines
each harness reaches today, and `make fuzz-cov` is red when a harness falls below it, so a lost
seed or a parser that grows lines its corpus never reaches cannot pass unnoticed; CI runs it.

The harnesses are held to a floor of their own. Every property check in `fuzz/fuzz_*.c`, an
`abort()` and the statement that guards it, must be executed by at least one seed:
`fuzz/checks.awk` finds each check in the `llvm-cov show` of the harness file, and `make
fuzz-cov` is red on any check whose guard no seed executed, naming the file and the line:

```
  CHECK: fuzz/fuzz_ctrl.c:136 no seed reaches (abort at line 137): if (reac_identity_ingest(id, addr, pl, pll) < 0)
make fuzz-cov: RED -- harness checks no seed reaches: ctrl
```

The guard of a check is its `if`. A brace-less body belongs to the nearest `if`, `else`, `case` or
`default` above it, never to the `for` headers above that, so a loop whose body never iterates
leaves its `if` unreached. A guard line with no count in the profile reads as not reached.
A `default: abort()` has no `if` of its own: its guard is the `switch`, and that the switch ran
is the only thing the floor can observe there. The arm is never taken by design, so the floor
holds no independent witness for it; the record above it names the breakage that makes it fire.
`make fuzz-checks-selftest` (run first by `make fuzz-cov`) feeds these shapes to `checks.awk` and
`coverage.sh` and requires the strict answer.

A check no input reaches is a property nobody tests, and the library's line count cannot see
it: a seed that was the only one to drive a harness branch can go without a parser line
falling. A check that needs a new kind of input gets a seed from `fuzz/gen-corpus.c`.

`make fuzz-cov` is also red on a check whose comment names no breakage (the record ratchet,
below):

```
  RECORD: fuzz/fuzz_knock.c:86 (abort at line 88) names no breakage and is not marked not a property: ...
make fuzz-cov: RED -- harness checks with no mutant record: knock
```

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
| qdisc | 160 | 98.12% | `qdisc_dump`: the `-EIO` for a `recv()` of 0 bytes, which a netlink socket never returns and the fake kernel does not either |

## The mutant records

Every property check names, in its comment, one or more breakages of libreac it catches and the
seed it catches each on, in the format OSS Verify reads
([build-contract.md](https://github.com/theorem-labs/oss-verify/blob/main/docs/build-contract.md#the-checks-of-a-fuzz-target)):

```
/* A refused reac_decode() writes nothing into the output.
 * mutant: src/reac_decode.c:72 `return -1;` -> `return (out[0] = 1, out[1] = 1, out[2] = 1), -1;`
 * input: downstream-program-custom-41x12 */
if (r < 0 && changed(out, olen))
	abort();
```

A stop that is not a property (the allocation of a harness buffer failed) says so on one line,
`not a property: <reason>`, instead. A mutant names a line of this tree, so a change that moves
a line a record names moves the record with it, in the same commit.

`make fuzz-records` (`fuzz/records.sh`) proves every record at the commit it runs on. For each
one alone it makes the change (the old text must occur exactly once on that line), builds the
plain harness (no sanitizer; `fuzz/records-main.c` as the driver; every abort() naming its own
line through `fuzz/records-abort.h`, which only the harness is compiled with), runs it on the
named seed, requires exit 134 from that record's own check, and puts the file back. Before any
change, every named seed must exit 0 on the unchanged harness. A record whose line moved, whose
seed no longer reaches the check, or whose breakage another check stops first is red, with what
happened instead:

```
  RECORD: fuzz/fuzz_knock.c:100: mutant src/reac_knock.c:47 `...` -> `...`, input desk-talking: exit 0
fuzz-records: 14/16 records met, on 7 checks (1 more marked not a property)
make fuzz-records: RED -- 2 failures
```

A met record prints nothing. `REC_VERBOSE=1 make fuzz-records` prints each one too, with the
line whose abort() stopped it:

```
  met: fuzz/fuzz_ctrl.c:348: mutant src/reac_ctrlblk.c:374 `| block[19]` -> ``, input identity-reply-addr-0011: fires at fuzz/fuzz_ctrl.c:348
```

THE RECORD RATCHET. A check whose comment has no record and no `not a property:` line is
refused by `make fuzz-records` before anything is built, and by `make fuzz-cov`
(`fuzz/checks.awk` reads each check's comment the way OSS Verify's record reader does: the
comment lines directly above the check and its abort, and those inside its `if`). A new check
lands with its breakage, or it does not land.

## The sabotage patches

`make fuzz-sabotage` proves each harness can find a bug in the code it covers by fuzzing, not
only on its records. For every harness, `fuzz/sabotage/<harness>.patch` is a one-line change to
the parser, with the check it breaks and the seed that catches it written above its
`diff --git` line (OSS Verify reads the directory as `mutants: fuzz/sabotage`); the target
applies it, rebuilds, fuzzes for at most 30 s from the corpus, restores the source with
`git checkout --`, and is red unless the fuzzer crashed. Each patch is also one of the records
above, so `make fuzz-records` proves its seed and its check.

| Harness | Mutant | Caught by |
| --- | --- | --- |
| decode | `reac_decode` drops the geometry bound (`nch * ns * 3 <= 1440`) | the seed `downstream-program-custom-42x12`: the decode property (ASan can report the read past the frame first) |
| ctrl | `reac_ctrl_parse` accepts a frame from 16 bytes instead of 50 | the seed `breakages`, the property that a frame under 50 bytes is `NONE` |
| identity | `store_model_name` copies `len` bytes after the kind byte instead of `len - 1` | the seed `name-no-nul-sabotage`: the guard-byte property (ASan in its builds) |
| disco | `reac_disco_table_observe` saturates at `n > MAX` instead of `n >= MAX` | the seed `nine-peers`, ASan (a ninth entry written past the table) or the table bound |
| scene | `reac_ctrl_build_scene_step` accepts any body of at least 24 bytes | the seed `short-body-8903`, the property that only an 8904-byte body is accepted |
| boxreg | `reac_boxreg_declare` drops its `n >= REAC_BOXREG_MAX_BOXES` check | the seed `no-room`, ASan (a sixth box written past the registry) or the row property |
| knock | `reac_knock_step` compares against `opened_ns` instead of `due_ns` | the seed `silent-wire`, the silence property |
| grant | `reac_grant_alloc_fits` lets a box's last slot reach `0x30` | the seed `box-base48-w1`, the admission property |
| pcap | `pcap_source_next` reads a record one byte larger than the buffer | the seed `record-one-byte-over-cap`: the fit property (ASan in `fread` in its builds) |
| qdisc | `qdisc_dump` subtracts the walk offset from the bytes read as `size_t`, so an unpadded last message wraps the remainder and the walk runs on into the buffer | the seed `unpadded-last`: the failure property reads the error the fake kernel planted past the reply |

Every one of the ten is caught by a seed on its first execution. The qdisc one is the bug the
harness found when it arrived in 1.10.0, kept as the mutant. Four of them (decode, identity,
grant, pcap) used to be found only by fuzzing, in under a second; each now has the seed that
catches it.

## Running

```
make fuzz                    # build every harness into build/fuzz/
make fuzz-run FUZZ_TIME=60   # fuzz each for 60 s; red on any crash
make fuzz-sabotage           # every mutant must be found
make fuzz-records            # every check's recorded breakage, proven at this commit
REC_VERBOSE=1 make fuzz-records   # the same, and the line each met record fires at
make fuzz-cov                # the coverage witness and the record ratchet
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
