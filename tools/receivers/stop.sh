#!/bin/bash
# Stop the local test receivers started by start.sh.
# Usage: tools/receivers/stop.sh [carbon|syslog|all]   default: all
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"                 # tools/receivers
REPO="$(cd "$HERE/../.." && pwd)"                     # repository root
RUNDIR="${RECEIVERS_RUNDIR:-$REPO/.receivers}"
what="${1:-all}"

# stop_pid <name> <pidfile>: SIGTERM, wait up to 10 s, then SIGKILL
stop_pid() {
    local name="$1" pidf="$2" pid _
    if [ ! -s "$pidf" ]; then echo "$name: no pid file"; return 0; fi
    pid="$(cat "$pidf")"
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "$name: not running (stale pid $pid)"; rm -f "$pidf"; return 0
    fi
    kill -TERM "$pid"
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "$name: still running after 10 s, sending SIGKILL"; kill -KILL "$pid"
    fi
    rm -f "$pidf"
    echo "$name: stopped (pid $pid)"
}

case "$what" in
    carbon) stop_pid go-carbon "$RUNDIR/carbon/go-carbon.pid" ;;
    syslog) stop_pid rsyslogd "$RUNDIR/rsyslog/rsyslogd.pid" ;;
    all)    stop_pid go-carbon "$RUNDIR/carbon/go-carbon.pid"
            stop_pid rsyslogd "$RUNDIR/rsyslog/rsyslogd.pid" ;;
    *)      echo "usage: $0 [carbon|syslog|all]"; exit 2 ;;
esac
