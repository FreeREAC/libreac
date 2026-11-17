#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE BOX BUILDERS THAT TAKE A WIDTH, READ FROM THE HEADER THAT DECLARES THEM.
#
#   tools/gen-box-builders.sh include/reac/reac_ctrlblk.h > build/box_builders.inc
#
# tests/test_box_width.c holds every such builder to the width rule (even, 2..40).
# It used to carry its own list of them, so a builder added to the header was not
# checked until somebody remembered the list; two were missing. This writes the
# list from the declarations instead, one line per builder:
#
#   BOX_BUILDER_PLAIN(name)   (out, dst, src, counter, int width)
#   BOX_BUILDER_AUDIO(name)   (out, dst, src, counter, int width, planar, ns)
#
# A builder is `size_t reac_ctrl_build_*` whose first four parameters are the
# frame, the two MACs and the counter and whose fifth is an int: the width. One
# whose fifth parameter is something else (a model row, a head-amp channel) takes
# no width and is left out. A width-taking builder in a shape the test cannot call
# is refused here by name, and so is a header with no builder in it: an empty list
# would let the test pass having checked nothing.
set -eu
[ $# -eq 1 ] && [ -r "$1" ] || { echo "usage: gen-box-builders.sh HEADER" >&2; exit 2; }

awk '
# Comments out first: prose in this header quotes declarations.
{
	line = $0
	out = ""
	while (length(line) > 0) {
		if (in_comment) {
			i = index(line, "*/")
			if (i == 0) { line = ""; break }
			line = substr(line, i + 2); in_comment = 0
		} else {
			i = index(line, "/*"); j = index(line, "//")
			if (j > 0 && (i == 0 || j < i)) { out = out substr(line, 1, j - 1); line = ""; break }
			if (i == 0) { out = out line; line = ""; break }
			out = out substr(line, 1, i - 1); line = substr(line, i + 2); in_comment = 1
		}
	}
	text = text " " out
}
END {
	n = split(text, stmt, ";")
	found = 0; bad = 0
	for (s = 1; s <= n; s++) {
		d = stmt[s]
		gsub(/[ \t\r\n]+/, " ", d)
		if (d !~ /size_t reac_ctrl_build_[a-z0-9_]+ ?\(/) continue
		name = d; sub(/^.*size_t /, "", name); sub(/ ?\(.*$/, "", name)
		args = d; sub(/^[^(]*\( ?/, "", args); sub(/ ?\)[^)]*$/, "", args)
		np = split(args, p, / ?, ?/)
		if (np < 5) continue
		if (p[1] !~ /^uint8_t ?\* ?[a-z_]+$/ || p[2] !~ /^const uint8_t [a-z_]+\[6\]$/ ||
		    p[3] !~ /^const uint8_t [a-z_]+\[6\]$/ || p[4] !~ /^uint16_t [a-z_]+$/ ||
		    p[5] !~ /^int [a-z_]+$/) continue
		if (np == 5) {
			printf "BOX_BUILDER_PLAIN(%s)\n", name
		} else if (np == 7 && p[6] ~ /^float ?\* ?const ?\* ?[a-z_]+$/ && p[7] ~ /^int [a-z_]+$/) {
			printf "BOX_BUILDER_AUDIO(%s)\n", name
		} else {
			printf "gen-box-builders.sh: %s takes a width in a shape tests/test_box_width.c cannot call: (%s)\n", name, args > "/dev/stderr"
			bad = 1
		}
		found++
	}
	if (found == 0) {
		print "gen-box-builders.sh: no width-taking box builder declared in " FILENAME ": nothing would be checked" > "/dev/stderr"
		exit 1
	}
	exit bad
}' "$1"
