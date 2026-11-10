#!/bin/sh
# Runs on the QNX target. Replays every fault scenario through the real
# safety_monitor and controller processes (QNX message passing + timer-pulse
# watchdog) and prints one PASS/FAIL line per scenario.
#
#   ./scenarios_on_target.sh [repeats] [profile]
#
# The profile (default: qemu) sets the heartbeat deadline for both processes
# and how far timing scenarios stay from it.
#
# Both processes run at raised priority (monitor above controller) with QNX's
# `on -p`, so ordinary system activity cannot delay them. Exit status is the
# number of failing runs (0 = all passed).

REPEATS=${1:-3}
PROFILE=${2:-qemu}
MON_PRIO=60
CTL_PRIO=50
cd "$(dirname "$0")" || exit 99

# Machine-readable results for tools/qnx_report.py: one summary row per run
# and one per-cycle trace CSV per run.
RESULTS=results
rm -rf "$RESULTS"
mkdir -p "$RESULTS"
{
    echo "profile=$PROFILE"
    echo "repeats=$REPEATS"
    echo "uname=$(uname -a)"
    echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULTS/meta.txt"
echo "run,ok,monitor_watchdog,scenario,profile,expected,got,stop_step,steps,avg_late_ms,max_late_ms,late_over_2ms,max_rtt_ms" \
    > "$RESULTS/summary.csv"

total_fail=0
for s in $(./controller --list | awk '{print $1}'); do
    pass=0
    fail=0
    detail=""
    i=1
    while [ "$i" -le "$REPEATS" ]; do
        on -p $MON_PRIO ./safety_monitor --profile "$PROFILE" > mon.log 2>&1 &
        mon=$!
        sleep 1
        on -p $CTL_PRIO ./controller --scenario "$s" --seed "$i" \
            --profile "$PROFILE" --trace "$RESULTS/${s}_$i.csv" > ctl.log 2>&1
        sleep 0.1
        kill $mon 2> /dev/null
        wait $mon 2> /dev/null

        if [ "$s" = crash ]; then
            # The controller just goes silent; the monitor must catch it.
            grep -q "WATCHDOG TIMEOUT" mon.log && ok=1 || ok=0
        else
            grep -q "AS EXPECTED" ctl.log && ok=1 || ok=0
        fi
        grep -q "WATCHDOG TIMEOUT" mon.log && monwd=1 || monwd=0
        echo "$i,$ok,$monwd,$(grep -h '^RESULT,' ctl.log | cut -d, -f2-)" \
            >> "$RESULTS/summary.csv"
        if [ $ok = 1 ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
        fi
        detail=$(grep -h "SAFE_STOP" ctl.log mon.log | head -1)
        timing=$(grep -h "^TIMING" ctl.log | sed 's/^TIMING //')
        [ $ok = 1 ] || echo "     run $i: $(grep -h "EXPECTED" ctl.log) | $timing"
        i=$((i + 1))
    done
    total_fail=$((total_fail + fail))
    if [ $fail = 0 ]; then verdict=PASS; else verdict=FAIL; fi
    printf "%-4s %-16s %d/%d  %s\n" "$verdict" "$s" "$pass" "$REPEATS" "$detail"
    [ -n "$timing" ] && echo "     last run timing: $timing"
done
rm -f mon.log ctl.log
echo
echo "Timing profile: $PROFILE"
if [ $total_fail = 0 ]; then
    echo "ALL SCENARIOS PASSED ON QNX"
else
    echo "$total_fail FAILING RUN(S) ON QNX"
fi
exit $total_fail
