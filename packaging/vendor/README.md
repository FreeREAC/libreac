SPDX-License-Identifier: GPL-3.0-or-later

# Vendored headers — a stopgap, not a design

`reac-pw-headers/reac_rate_cfg.h` and `reac_role_cfg.h` are a **snapshot copy** of the two
reac-pw headers that `include/reac/transport/reac_pacer.h` and `reac_role_swap.h` still
`#include` for their pure declarations (`docs/design/specs/2026-09-11-reac-transport-library.md`
§2/§5 names this seam explicitly — it is not an oversight). They exist here only so
`packaging/libreac-transport.spec`'s `%build` has something to point `REACPW_INCLUDE` at without
requiring a reac-pw source checkout inside the libreac SRPM, which would be a real circular build
dependency (reac-pw's own spec requires `libreac-transport-devel`).

**This is a stopgap.** The honest fix is the header split the design spec already names as open:
carve the pure declarations (`REAC_RATE_REFUSE_*`, `REAC_CFG_ROLE_*` and friends) into their own
transport-owned header, leaving only the `spa_pod`-touching declaration behind in reac-pw. Until
that lands, whoever changes `reac-pw/src/reac_rate_cfg.h` or `reac_role_cfg.h` must refresh the
copy here by hand — there is no automated drift check yet, which is itself a gap the header split
would close for free (the transport header would no longer need a copy of anything).

## The cfg vocabulary is not theirs to spell (2026-09-25)

Both headers now `#include <reac/reac_cfg.h>` and name its macros; every key, state
string and refusal code they used to type out is an alias of libreac's declaration
(`REAC_PROP_RATE` -> `REAC_RATE_PROP`, ...), and the refusal codes reach
`reac_rate_refuse_code()` / `reac_role_refuse_code()` as the enum-indexed
`REAC_RATE_REFUSE_CODES_INIT` / `REAC_ROLE_REFUSE_CODES_INIT` tables. The two copies had
drifted — `""` here, `"none"` in reac-pw for the idle refusal — and
`tests/conformance-cfg-declared-once.sh` now refuses a string typed a second time.
**reac-pw's own `src/reac_rate_cfg.h` / `reac_role_cfg.h` must take the same edit**, and
its `*_refuse_code()` must return from the tables instead of its own literals; until it
does, this snapshot is ahead of its source.
