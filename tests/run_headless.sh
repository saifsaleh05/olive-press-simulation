#!/bin/sh
# End to end checks on the real binary, no display needed.
# usage: tests/run_headless.sh ./olive_press

BIN=${1:-./olive_press}
TMP=$(mktemp -d)
FAILED=0
trap 'rm -rf "$TMP"' EXIT

pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; FAILED=1; }

# true if some simulator process is still around after 2 seconds
leftovers() {
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        pgrep -f "olive_press tests/fast" >/dev/null || return 1
        sleep 0.2
    done
    return 0
}

echo "end to end:"

# 1. full run, every batch out, mass conserved
if "$BIN" tests/fast.cfg --headless </dev/null >"$TMP/a.log" 2>&1 &&
   grep -q "summary: 6/6 batches" "$TMP/a.log"; then
    pass "normal run completes all batches"
else
    fail "normal run completes all batches"
fi

grep -q "WARNING" "$TMP/a.log" && fail "mass balance holds per batch" || pass "mass balance holds per batch"

# 2. same seed, same production figures
"$BIN" tests/fast.cfg --headless </dev/null >"$TMP/b.log" 2>&1
sed -n 's/^\[[ 0-9.]*\] \(batch [0-9]* done.*\)/\1/p' "$TMP/a.log" | sort >"$TMP/a.out"
sed -n 's/^\[[ 0-9.]*\] \(batch [0-9]* done.*\)/\1/p' "$TMP/b.log" | sort >"$TMP/b.out"
if [ -s "$TMP/a.out" ] && cmp -s "$TMP/a.out" "$TMP/b.out"; then
    pass "runs with the same seed are repeatable"
else
    fail "runs with the same seed are repeatable"
fi

# 3. QC alarm, automatic fault response and recovery
if "$BIN" tests/fast_hot.cfg --headless </dev/null >"$TMP/hot.log" 2>&1 &&
   grep -q "QC ALARM Malaxation" "$TMP/hot.log" &&
   grep -q "Malaxation fault cleared" "$TMP/hot.log"; then
    pass "over temperature alarm is raised and recovered"
else
    fail "over temperature alarm is raised and recovered"
fi

# 4. operator controls while running
( sleep 0.4; echo p; sleep 0.5; echo r; sleep 0.3; echo f 4; sleep 0.5; echo c 4 ) |
    "$BIN" tests/fast.cfg --headless >"$TMP/ctl.log" 2>&1
if grep -q "operator: pause" "$TMP/ctl.log" && grep -q "operator: resume" "$TMP/ctl.log" &&
   grep -q "Crushing: OPERATING -> ERROR\|Crushing: IDLE -> ERROR" "$TMP/ctl.log" &&
   grep -q "summary: 6/6 batches" "$TMP/ctl.log"; then
    pass "pause / resume / fault / clear, line still finishes"
else
    fail "pause / resume / fault / clear, line still finishes"
fi

# 5. operator stop
( sleep 0.5; echo x ) | "$BIN" tests/fast.cfg --headless >"$TMP/stop.log" 2>&1
if grep -q "line finished" "$TMP/stop.log" && ! leftovers; then
    pass "stop shuts every child down"
else
    fail "stop shuts every child down"
fi

# 6. a child dies under us
"$BIN" tests/fast.cfg --headless </dev/null >"$TMP/kill.log" 2>&1 &
sleep 0.5
victim=$(pgrep -P $! | sed -n 4p)
[ -n "$victim" ] && kill -9 "$victim"
wait $! 2>/dev/null
if grep -q "killed by signal 9" "$TMP/kill.log" && grep -q "stopping line" "$TMP/kill.log" &&
   ! leftovers; then
    pass "crashed child is detected and the line stops safely"
else
    fail "crashed child is detected and the line stops safely"
fi

# 7. bad config is refused before anything forks
if ! "$BIN" /nonexistent.cfg --headless </dev/null >/dev/null 2>&1; then
    pass "missing config file is rejected"
else
    fail "missing config file is rejected"
fi

[ $FAILED -eq 0 ] && echo "all end to end checks passed"
exit $FAILED
