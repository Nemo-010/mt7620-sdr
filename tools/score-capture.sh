#!/bin/sh
# score-capture.sh - is a capture real receiver output, or unwritten SRAM?
#
#   tools/score-capture.sh [file]     (default: the debugfs capture node)
#
# Prints, for chain 0's I stream:
#
#   mean   DC offset
#   sd     standard deviation
#   lag1   normalised lag-1 autocorrelation
#
# Real receiver output here is oversampled, so lag1 comes out near +0.5 to
# +0.7. Unwritten SRAM is uniform white: lag1 near 0.0. Use lag1 as the verdict.
#
# Do NOT rank captures by energy. Uniform noise at sd ~71 has far more energy
# than real noise at sd ~7, so "keep the loudest" always selects the garbage.
# Gate on lag1 first, then prefer the loudest among the real ones.
set -eu

src=${1:-/sys/kernel/debug/mt7620sdr/capture}

if [ -e "$src" ]; then
	tmp=/tmp/.mt7620-score.bin
	cat "$src" > "$tmp"
	file=$tmp
else
	file=$src
fi

# every 6th byte is chain 0's I (sample-major, chain-minor, I then Q), and the
# first 44 bytes are the header.
tail -c +45 "$file" | hexdump -v -e '1/1 "%u\n"' | awk '
	{ if (NR % 6 == 1) {
		v = ($1 > 127) ? $1 - 256 : $1
		if (p != "") { num += (v - p) * (v - p) }
		den += v * v
		s += v * v
		m += v
		n++
		p = v
	} }
	END {
		if (n < 2) { print "empty or truncated"; exit }
		mu = m / n
		lag1 = 1 - num / (2 * den)
		printf "samples %d  mean %+.2f  sd %.2f  lag1 %+.3f\n", n, mu, sqrt(s / n - mu * mu), lag1
		if (lag1 > 0.4)
			print "  -> REAL (band-limited and correlated)"
		else
			print "  -> GARBAGE or unwritten SRAM (white, lag1 ~ 0)"
	}'
