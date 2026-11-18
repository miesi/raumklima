#!/bin/bash
# Start the local test receivers for the hub as the normal user (no root, no launchd):
#   go-carbon  Graphite plaintext, TCP+UDP 2003, HTTP read API 127.0.0.1:8080
#   rsyslogd   RFC 5424 syslog, UDP 1516 (RFC 5426) and TCP 1516 (RFC 6587)
# Usage: tools/receivers/start.sh [carbon|syslog|all]   default: all
# Runtime data, logs and pid files go to <repo>/.receivers/ (git-ignored).
# Overrides: GO_CARBON (binary), RSYSLOGD (binary), RSYSLOG_MODDIR (module dir),
#            RECEIVERS_RUNDIR (runtime dir).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"                 # tools/receivers
REPO="$(cd "$HERE/../.." && pwd)"                     # repository root
RUNDIR="${RECEIVERS_RUNDIR:-$REPO/.receivers}"
# go-carbon is built in a sibling checkout: <repo>/../go-carbon/go-carbon
GO_CARBON="${GO_CARBON:-$REPO/../go-carbon/go-carbon}"
# Homebrew puts rsyslogd in sbin, which is not always on PATH
RSYSLOGD="${RSYSLOGD:-$(command -v rsyslogd || echo /opt/homebrew/sbin/rsyslogd)}"
what="${1:-all}"

# Escape a string for use as sed replacement text with "|" as delimiter.
sed_escape() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }

# render <template> <output>: replace @RUNDIR@ and @CONFDIR@ in a config template
render() {
    sed -e "s|@RUNDIR@|$(sed_escape "$RUNDIR")|g" \
        -e "s|@CONFDIR@|$(sed_escape "$HERE")|g" "$1" >"$2"
}

# running <pidfile>: true if the process in the pid file is alive
running() { [ -s "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

start_carbon() {
    local pidf="$RUNDIR/carbon/go-carbon.pid"
    [ -x "$GO_CARBON" ] || { echo "go-carbon binary not found: $GO_CARBON (set GO_CARBON)"; return 1; }
    mkdir -p "$RUNDIR/carbon/whisper"
    if running "$pidf"; then echo "go-carbon already running, pid $(cat "$pidf")"; return 0; fi
    render "$HERE/go-carbon.conf.in" "$RUNDIR/go-carbon.conf"
    "$GO_CARBON" -config "$RUNDIR/go-carbon.conf" -check-config >"$RUNDIR/carbon/check-config.out" 2>&1 \
        || { echo "go-carbon config check failed:"; cat "$RUNDIR/carbon/check-config.out"; return 1; }
    # foreground process detached with nohup; go-carbon's -daemon mode is not used
    # because it wants to switch user. stdout/stderr catch panics before logging starts.
    nohup "$GO_CARBON" -config "$RUNDIR/go-carbon.conf" >>"$RUNDIR/carbon/stdout.log" 2>&1 </dev/null &
    echo $! >"$pidf"
    sleep 1
    if running "$pidf"; then echo "go-carbon started, pid $(cat "$pidf")"
    else echo "go-carbon died, see $RUNDIR/carbon/stdout.log and go-carbon.log"; return 1; fi
}

start_syslog() {
    local pidf="$RUNDIR/rsyslog/rsyslogd.pid" modopt=()
    [ -x "$RSYSLOGD" ] || { echo "rsyslogd not found: $RSYSLOGD (set RSYSLOGD)"; return 1; }
    # default module dir is compiled into the binary; override only if asked
    [ -n "${RSYSLOG_MODDIR:-}" ] && modopt=(-M "$RSYSLOG_MODDIR")
    mkdir -p "$RUNDIR/rsyslog" "$RUNDIR/syslog"
    if running "$pidf"; then echo "rsyslogd already running, pid $(cat "$pidf")"; return 0; fi
    render "$HERE/rsyslog.conf.in" "$RUNDIR/rsyslog.conf"
    # -N1: config check only
    "$RSYSLOGD" ${modopt[@]+"${modopt[@]}"} -N1 -f "$RUNDIR/rsyslog.conf" >"$RUNDIR/rsyslog/check-config.out" 2>&1 \
        || { echo "rsyslog config check failed:"; cat "$RUNDIR/rsyslog/check-config.out"; return 1; }
    # -n: stay in foreground (nohup detaches it), -i: own pid file instead of /var/run
    nohup "$RSYSLOGD" ${modopt[@]+"${modopt[@]}"} -n -f "$RUNDIR/rsyslog.conf" -i "$pidf" \
        >>"$RUNDIR/rsyslog/stdout.log" 2>&1 </dev/null &
    local pid=$!
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
        echo "$pid" >"$pidf"      # same pid rsyslogd writes; set it in case it has not yet
        echo "rsyslogd started, pid $pid"
    else echo "rsyslogd died, see $RUNDIR/rsyslog/stdout.log"; return 1; fi
}

rc=0
case "$what" in
    carbon) start_carbon || rc=1 ;;
    syslog) start_syslog || rc=1 ;;
    all)    start_carbon || rc=1; start_syslog || rc=1 ;;
    *)      echo "usage: $0 [carbon|syslog|all]"; exit 2 ;;
esac

# Show what is listening, so a port clash is visible right away.
echo "listening sockets:"
lsof -nP -iTCP:2003 -iTCP:8080 -iTCP:1516 -sTCP:LISTEN -iUDP:2003 -iUDP:1516 2>/dev/null \
    | awk 'NR == 1 || /go-carbon|rsyslogd/'
exit $rc
