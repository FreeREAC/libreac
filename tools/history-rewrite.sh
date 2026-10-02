#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PUBLIC HISTORY CARRIES NO INTERNAL DOCUMENT. tools/history-drop-paths.txt lists the
# internal documents libreac's history drops; their home is freereac-ops/libreac/<path>.
# This is the one recipe for the rewrite, so it can be run again and give the same commits:
#
#   tools/history-rewrite.sh rewrite <source> <branch> <backup-tag> <out-dir>
#       A fresh clone of <source>'s <branch> (and the tags in it) into <out-dir>, the listed
#       paths dropped from every commit with git filter-repo, then `verify`. Refuses unless
#       <backup-tag> exists on <source> and names <branch>'s head. Pushes nothing; prints
#       `REWRITE old=<head> new=<head> ...`. Exit 77 where git-filter-repo is not installed.
#   tools/history-rewrite.sh verify <repo> <rev>...
#       No commit reachable from <rev> carries a listed path.
#   tools/history-rewrite.sh confirm-ops [<repo root>]
#       Every listed path exists in the ops checkout under libreac/<path>. The ops checkout is
#       found by tools/ops-slugs.sh's resolver (FREEREAC_OPS, ../freereac-ops); none is
#       OPS-ABSENT, exit 77, unless FREEREAC_REQUIRE_OPS=1, which makes it a failure.
#   tools/history-rewrite.sh --self-test          verify, confirm-ops and the backup refusal
#   tools/history-rewrite.sh --self-test-rewrite  a planted rewrite (exit 77 without filter-repo)
#
# Exit 0 conforms, 1 an offence or a refusal, 2 NOT A RESULT or usage, 77 nothing tested.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SELF="$ROOT/tools/history-rewrite.sh"

# drop_paths <repo root>: the listed paths, one per line.
drop_paths() {
	grep -v '^#' "$1/tools/history-drop-paths.txt" | grep .
}

# verify <repo> <rev>...: prints a verdict line, returns 0 / 1.
verify() {
	repo=$1; shift
	# shellcheck disable=SC2046 # one path per line, none with a space
	hits=$(git -C "$repo" log --full-history --format='%h %s' --name-only "$@" -- $(drop_paths "$ROOT"))
	if [ -n "$hits" ]; then
		echo "history-rewrite verify: FAIL -- commits reachable from $* carry a dropped path:"
		printf '%s\n' "$hits" | grep . | sed 's/^/  /'
		return 1
	fi
	echo "history-rewrite verify: PASS -- no commit reachable from $* carries a dropped path"
	return 0
}

# confirm_ops <repo root>: prints a verdict line, returns 0 / 1 / 77.
confirm_ops() {
	ops=$(OPS_SLUGS_LIB=1; . "$ROOT/tools/ops-slugs.sh"; ops_root "$1") || {
		if [ "${FREEREAC_REQUIRE_OPS:-0}" = 1 ]; then
			echo "history-rewrite confirm-ops: FAIL -- FREEREAC_REQUIRE_OPS=1 and no ops checkout (set FREEREAC_OPS or clone ../freereac-ops)"
			return 1
		fi
		echo "OPS-ABSENT history-rewrite confirm-ops: no ops checkout (FREEREAC_OPS, ../freereac-ops); nothing confirmed"
		return 77
	}
	n=0 missing=""
	for p in $(drop_paths "$1"); do
		n=$((n + 1))
		[ -f "$ops/libreac/$p" ] || missing="$missing $p"
	done
	if [ -n "$missing" ]; then
		echo "history-rewrite confirm-ops: FAIL -- not in $ops/libreac/ (copy each from the backup tag first):"
		printf '  %s\n' $missing
		return 1
	fi
	echo "history-rewrite confirm-ops: PASS -- all $n dropped paths are in $ops/libreac/"
	return 0
}

# rewrite <source> <branch> <backup-tag> <out-dir>
rewrite() {
	src=$1 branch=$2 tag=$3 out=$4
	if [ -e "$out" ]; then
		echo "history-rewrite: REFUSED -- $out exists; the rewrite runs in a fresh clone"
		return 1
	fi
	old=$(git ls-remote "$src" "refs/heads/$branch" | cut -f1)
	backup=$(git ls-remote "$src" "refs/tags/$tag" "refs/tags/$tag^{}" | sort -k2 | tail -1 | cut -f1)
	if [ -z "$old" ] || [ "$backup" != "$old" ]; then
		echo "history-rewrite: REFUSED -- BACKUP-MISSING: tag $tag on $src does not name $branch's head (${old:-no such branch})"
		return 1
	fi
	if ! git filter-repo --version >/dev/null 2>&1; then
		echo "history-rewrite: git-filter-repo is not installed; nothing rewritten"
		return 77
	fi
	git clone -q --no-local --single-branch --branch "$branch" "$src" "$out" || return 1
	list=$(mktemp)
	drop_paths "$ROOT" > "$list"
	(cd "$out" && git filter-repo --quiet --invert-paths --paths-from-file "$list") || { rm -f "$list"; return 1; }
	rm -f "$list"
	verify "$out" --all || return 1
	echo "REWRITE old=$old new=$(git -C "$out" rev-parse "refs/heads/$branch") branch=$branch backup=$tag" \
		"commits=$(git -C "$out" rev-list --count "refs/heads/$branch") tags=$(git -C "$out" tag | wc -l)"
	return 0
}

# A planted origin: src.c, then a listed path added (with a tag on that commit), then
# deleted. g runs git in it with a throwaway identity and no signing.
g() { r=$1; shift; git -C "$r" -c user.name=t -c user.email=t@example.com -c commit.gpgsign=false -c tag.gpgsign=false "$@"; }
plant() { # <dir>
	git init -q -b main "$1"
	mkdir -p "$1/docs" "$1/tools"
	cp "$ROOT/tools/history-drop-paths.txt" "$1/tools/"
	echo 'int x;' > "$1/src.c"
	g "$1" add -A && g "$1" commit -qm seed
	echo internal > "$1/docs/layering.md"
	echo 'int y;' >> "$1/src.c"
	g "$1" add -A && g "$1" commit -qm "a note and some code"
	g "$1" tag -a v1 -m v1
	g "$1" rm -q docs/layering.md && g "$1" commit -qm "the note leaves"
}

self_test() {
	if ! command -v git >/dev/null 2>&1; then
		echo "history-rewrite self-test: no git here (a package build); NOTHING TESTED"
		return 77
	fi
	T=$(mktemp -d)
	trap 'rm -rf "$T"' EXIT
	ok=1
	expect() { # <want rc> <what> <cmd>...
		want=$1 what=$2; shift 2
		out=$("$@" 2>&1); rc=$?
		if [ "$rc" = "$want" ]; then echo "  ok   $what"; else echo "  FAIL $what (rc $rc, want $want): $out"; ok=0; fi
	}
	plant "$T/old"
	expect 1 "a history that once carried a dropped path fails verify" verify "$T/old" HEAD
	expect 0 "the history before it passes verify" verify "$T/old" HEAD~2
	expect 1 "a tag into that history fails verify" verify "$T/old" v1
	expect 1 "no backup tag refuses the rewrite" rewrite "$T/old" main backup/x "$T/out"
	g "$T/old" tag backup/x HEAD~1
	expect 1 "a backup tag on an older commit refuses the rewrite" rewrite "$T/old" main backup/x "$T/out"
	mkdir "$T/out"
	g "$T/old" tag -f backup/x HEAD >/dev/null
	expect 1 "an existing out-dir refuses the rewrite" rewrite "$T/old" main backup/x "$T/out"

	mkdir -p "$T/repo/tools"
	cp "$ROOT/tools/history-drop-paths.txt" "$T/repo/tools/"
	for p in $(drop_paths "$T/repo"); do mkdir -p "$T/freereac-ops/libreac/${p%/*}"; touch "$T/freereac-ops/libreac/$p"; done
	expect 0 "the sibling ops checkout holds every dropped path" env FREEREAC_OPS= sh "$SELF" confirm-ops "$T/repo"
	rm "$T/freereac-ops/libreac/docs/layering.md"
	expect 1 "a dropped path missing from ops fails" env FREEREAC_OPS= sh "$SELF" confirm-ops "$T/repo"
	mv "$T/freereac-ops" "$T/elsewhere"
	expect 77 "no ops checkout is OPS-ABSENT" env FREEREAC_OPS= sh "$SELF" confirm-ops "$T/repo"
	expect 1 "no ops checkout with FREEREAC_REQUIRE_OPS=1 fails" env FREEREAC_OPS= FREEREAC_REQUIRE_OPS=1 sh "$SELF" confirm-ops "$T/repo"
	touch "$T/elsewhere/libreac/docs/layering.md"
	expect 0 "FREEREAC_OPS names the checkout" env FREEREAC_OPS="$T/elsewhere" sh "$SELF" confirm-ops "$T/repo"
	if [ $ok = 1 ]; then echo "history-rewrite self-test: PASS"; return 0; fi
	echo "history-rewrite self-test: FAILED"; return 2
}

self_test_rewrite() {
	if ! command -v git >/dev/null 2>&1 || ! git filter-repo --version >/dev/null 2>&1; then
		echo "history-rewrite self-test-rewrite: git-filter-repo is not installed; NOTHING TESTED"
		return 77
	fi
	T=$(mktemp -d)
	trap 'rm -rf "$T"' EXIT
	plant "$T/old"
	g "$T/old" tag backup/x HEAD
	bad=""
	out=$(rewrite "$T/old" main backup/x "$T/a" 2>&1) || bad="$bad rewrite-failed($out)"
	verify "$T/a" --all >/dev/null || bad="$bad dropped-path-left"
	verify "$T/old" HEAD >/dev/null && bad="$bad source-changed"
	[ "$(git -C "$T/a" show main:src.c)" = "$(git -C "$T/old" show main:src.c)" ] || bad="$bad code-lost"
	[ "$(git -C "$T/a" rev-list --count main)" = 2 ] || bad="$bad commit-count"
	git -C "$T/a" rev-parse -q --verify 'v1^{commit}' >/dev/null || bad="$bad tag-lost"
	[ "$(git -C "$T/a" rev-parse main)" != "$(git -C "$T/old" rev-parse main)" ] || bad="$bad not-rewritten"
	rewrite "$T/old" main backup/x "$T/b" >/dev/null 2>&1
	[ "$(git -C "$T/b" rev-parse main 2>/dev/null)" = "$(git -C "$T/a" rev-parse main)" ] || bad="$bad not-reproducible"
	if [ -z "$bad" ]; then
		echo "history-rewrite self-test-rewrite: PASS -- the planted path is gone from every commit and tag, the commit that only deleted it is pruned, the code and the tag stay, a second run gives the same head"
		return 0
	fi
	echo "history-rewrite self-test-rewrite: FAILED:$bad"
	return 2
}

case "${1:-}" in
	rewrite) [ $# -eq 5 ] || { echo "usage: tools/history-rewrite.sh rewrite <source> <branch> <backup-tag> <out-dir>" >&2; exit 2; }
		rewrite "$2" "$3" "$4" "$5"; exit $? ;;
	verify) [ $# -ge 3 ] || { echo "usage: tools/history-rewrite.sh verify <repo> <rev>..." >&2; exit 2; }
		shift; verify "$@"; exit $? ;;
	confirm-ops) confirm_ops "${2:-$ROOT}"; exit $? ;;
	--self-test) self_test; exit $? ;;
	--self-test-rewrite) self_test_rewrite; exit $? ;;
	*) echo "usage: tools/history-rewrite.sh rewrite|verify|confirm-ops|--self-test|--self-test-rewrite" >&2; exit 2 ;;
esac
