#!/bin/sh

set -u

log_file="${TMPDIR:-/tmp}/olive_press_shutdown_$$.log"
trap 'rm -f "$log_file"' EXIT HUP INT TERM

./olive_press >"$log_file" 2>&1 &
supervisor_pid=$!

attempt=0
while kill -0 "$supervisor_pid" 2>/dev/null; do
    if grep -q "Started Quality Control" "$log_file"; then
        break
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 10000 ]; then
        echo "FAIL: supervisor startup was not observed" >&2
        kill -TERM "$supervisor_pid" 2>/dev/null || true
        wait "$supervisor_pid" 2>/dev/null || true
        exit 1
    fi
done

if ! kill -INT "$supervisor_pid" 2>/dev/null; then
    echo "FAIL: supervisor finished before shutdown could be requested" >&2
    wait "$supervisor_pid" 2>/dev/null || true
    exit 1
fi

wait "$supervisor_pid"
exit_status=$?

if [ "$exit_status" -eq 0 ]; then
    echo "FAIL: interrupted supervisor returned success" >&2
    exit 1
fi
if ! grep -q "shutdown requested by signal" "$log_file"; then
    echo "FAIL: supervisor did not report signal shutdown" >&2
    exit 1
fi
if ! grep -q "Supervisor child-process table" "$log_file"; then
    echo "FAIL: supervisor did not reap and report its children" >&2
    exit 1
fi

echo "PASS: signal shutdown terminated and reaped all started children"
