#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The thermostat's tests: thsim.c (the real thermostat.c in a simulated
# house) through the scenarios that went wrong once, each with its limits.
# Exit 0 when all pass. Needs a host C compiler and cJSON: CJSON_DIR with
# cJSON.c and cJSON.h (Buildroot's output/build/cjson-*), or a system
# libcjson.
#
#   test/run.sh            all scenarios (about a minute)
#   KEEP=1 test/run.sh     keep the work folder and its output
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
FAIL=0

if [ -n "${CJSON_DIR:-}" ] && [ -f "$CJSON_DIR/cJSON.c" ]; then
	mkdir -p "$W/inc/cjson"
	cp "$CJSON_DIR/cJSON.h" "$W/inc/cjson/"
	CJ="-I$W/inc -I$CJSON_DIR $CJSON_DIR/cJSON.c"
else
	CJ="-lcjson"
fi
mkdir -p "$W/data"
# shellcheck disable=SC2086
gcc -O1 -Wall -Wno-unused-function -DTH_TEST \
	-DCONF_FILE='"data/thermostat.json"' -DSAVE_FILE='"data/thermostat.state"' \
	-DVENDOR_FILE='"data/vendor.xml"' -DLOG_FILE='"data/changes.log"' \
	-DMEASURE_FILE='"data/measurements.csv"' \
	-o "$W/thsim" "$HERE/thsim.c" $CJ -lm || { echo "FAIL: does not build"; exit 1; }
cd "$W" || exit 1

run() {	# name mode days [VAR=value...]
	name=$1 mode=$2 days=$3
	shift 3
	env "$@" ./thsim "$mode" "$days" > "$name.out" 2>/dev/null
}
# value of a key on the last line that has it
val() {	# file key [line pattern]
	grep "${3:-.}" "$1" | tail -n 1 | tr ' ' '\n' | grep -A1 -x "$2" | tail -n 1
}
check() {	# what, value, awk condition with v (no value fails)
	if [ -n "$2" ] && awk -v v="$2" "BEGIN { exit !($3) }"; then
		echo "ok    $1 ($2)"
	else
		echo "FAIL  $1 ($2; wants $3)"
		FAIL=1
	fi
}

# 1. every way of heating, a week: comfort, starts, on time, what it learned
for m in 0 1 2 3; do
	run mode$m "$m" 7
	o=mode$m.out
	check "mode $m: room 10-22h at least 19.6" "$(val $o min '^day 7')" "v >= 19.6"
	check "mode $m: room 10-22h at most 20.45" "$(val $o max '^day 7')" "v <= 20.45"
	check "mode $m: 07:00 warm on day 7" "$(val $o at7 '^day 7')" "v >= 19.7"
	case $m in 0) lim=5 ;; 3) lim=15 ;; *) lim=30 ;; esac
	check "mode $m: starts a day" "$(val $o cycles '^day 7')" "v <= $lim"
	check "mode $m: k learned (house 0.0005)" "$(val $o k learned)" "v >= 0.0004 && v <= 0.0006"
	check "mode $m: factor learned" "$(val $o factor learned)" "v >= 0.0003 && v <= 0.0008"
done

# 2. a sensor that says "nan": no heat on it
run nan 1 2 NANH=8
check "nan reading: minutes heating on it" "$(val nan.out nanheat '^day 2')" "v == 0"
run nanraw 1 2 NANH=8 NANRAW=1
check "nan reading passed to the thermostat: minutes heating on it" "$(val nanraw.out nanheat '^day 2')" "v == 0"

# 3. a boiler whose maximum is at or below the start point still heats
for mx in 30 28; do
	run max$mx 0 3 MAXF=$mx
	check "boiler maximum $mx: heats" "$(val max$mx.out min '^day 3')" "v > 12"
done

# 4. another sensor (+0.9) or sun: k is not taken in
run jump 2 4 JUMP=0.9
check "sensor jump: k" "$(val jump.out k learned)" "v >= 0.0004 && v <= 0.0006"
run sun 2 5 SUN=0.004
check "sun 11-16 h: k" "$(val sun.out k learned)" "v >= 0.0004 && v <= 0.0006"

# 5. a sensor in 0.1 steps does not switch a modulating boiler often
run steps 0 3 Q=0.1
check "sensor in 0.1 steps: starts a day" "$(val steps.out cycles '^day 3')" "v <= 5"

# 6. after-heat learned already: no short cycling of a modulating boiler
run lag 0 3 "STATE=after=0.066
after_n=5
after_mode=1
"
check "after-heat learned, modulating: starts a day" "$(val lag.out cycles '^day 3')" "v <= 10"

# 7. pulses: thermostat off and on again starts at once (not a cycle later)
run pause 1 2 PAUSE=1
check "off and on again (pulses): seconds to heat" "$(val pause.out after resume)" "v <= 120"

# 7b. a maximum water temperature from Home Assistant: "max_flow 45" makes
# it a fixed one (kept in the settings), "max_flow 0" the boiler's own again
run maxcmd 0 1 MAXCMD="max_flow 45" MAXCMD2="max_flow 0"
check "max_flow 45: maximum in force" "$(val maxcmd.out now 'hour 1')" "v == 45"
check "max_flow 45: kept in the settings" "$(grep -c '"max_flow":[[:space:]]*45' data/thermostat.json)" "v == 1"
check "max_flow 0: the boiler's own again" "$(val maxcmd.out now 'hour 2')" "v == 0"
check "max_flow 0: kept in the settings" "$(grep -c '"max_flow_mode":[[:space:]]*"boiler"' data/thermostat.json)" "v == 1"
run maxbad 0 1 MAXCMD="max_flow 95"
check "max_flow 95: refused" "$(val maxbad.out now 'hour 1')" "v == 0"

# 7c. no boiler module: the starts that never warm up teach nothing
run noheat 0 4 NOHEAT=1
check "no heat could come: pre-heat correction stays" "$(val noheat.out preheat_extra learned)" "v == 0"

# 8. starting from the original software's values, once
cat > vendor.xml <<'XML'
<Config><outsideRate>-0.008000</outsideRate><heatingFactor>0.001000</heatingFactor><longTermOffset>-0.200</longTermOffset><measuredHeatingFactors>measuredHeatingFactors:[ 0.00050,0.00045,0.00052,0.00048,0.00047]</measuredHeatingFactors></Config>
XML
run vendor 0 1 VENDOR="$W/vendor.xml"
check "takeover of the original software's values" "$(val vendor.out from_vendor learned)" "v == 1"
# ... but not its offset (it belongs to its own room reading), and one taken
# over by an older version starts at 0 once
run vendor0 0 0 VENDOR="$W/vendor.xml"
check "takeover: not the original software's offset" "$(val vendor0.out offset learned)" "v == 0"
run oldoff 0 0 "STATE=from_vendor=1
offset=-0.840
"
check "offset taken over before: back to 0" "$(val oldoff.out offset learned)" "v == 0"
run ownoff 0 0 "STATE=from_vendor=1
offset=-0.400
own_offset=1
"
check "own offset learned since: kept" "$(val ownoff.out offset learned)" "v == -0.4"

# 8b. "Kraanwater voorverwarmen" off in the original software: tap water
# not kept warm, written to the settings once; on there: nothing written
sed 's#</Config>#<dhwEnabled>0</dhwEnabled></Config>#' vendor.xml > vendor-dhw0.xml
run dhw0 0 0 VENDOR="$W/vendor-dhw0.xml"
check "original software without keeping tap water warm: taken over" "$(grep -c '"keep_warm":[[:space:]]*false' data/thermostat.json)" "v == 1"
sed 's#</Config>#<dhwEnabled>1</dhwEnabled></Config>#' vendor.xml > vendor-dhw1.xml
run dhw1 0 0 VENDOR="$W/vendor-dhw1.xml"
check "original software keeping it warm: settings left alone" "$(grep -c 'keep_warm' data/thermostat.json)" "v == 0"
# ... and switched from Home Assistant
run dhwcmd 0 1 MAXCMD="hot_water_keep_warm off" MAXCMD2="hot_water_keep_warm on"
check "hot_water_keep_warm: kept in the settings" "$(grep -c '"keep_warm":[[:space:]]*true' data/thermostat.json)" "v == 1"

# 9. the measurement log: a line a minute, a header, the same columns
run measure 1 2 MEASURE=1
lines=$(cat data/measurements.csv.1 data/measurements.csv 2>/dev/null | grep -c '^2026-')
check "measurement log: lines in 2 days (written every 10 min)" "$lines" "v >= 2860 && v <= 2880"
check "measurement log: a header in each file" "$(cat data/measurements.csv.1 data/measurements.csv | grep -c '^time,')" "v == 2"
cols=$(cat data/measurements.csv.1 data/measurements.csv | awk -F, '!/^time,/ && NF != 23 { bad++ } END { print bad + 0 }')
check "measurement log: every line 23 columns" "$cols" "v == 0"

if [ -n "${KEEP:-}" ]; then
	echo "output kept in $W"
else
	rm -rf "$W"
fi
[ $FAIL = 0 ] && echo "all passed" || echo "SOME FAILED"
exit $FAIL
