#!/bin/sh
# Toon web interface: the small API behind the pages in /usr/share/toon/web.
# Runs under lighttpd (basic auth for everything, see S51toon-web),
# started by cgi-bin/api (src/toon-api.c), which answers GET /status,
# /settings and /thermostat/state itself and passes every other request on
# to this script. Change those three there too.
# Changes need "Content-Type: application/x-toon", which a form on another
# site cannot send, so a foreign page cannot make the browser change things.
DIR=/mnt/data/toon-os/config
DEFAULT=/usr/share/toon/default-dashboard.json

reply() {	# status, content type
	printf 'Status: %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n\r\n' "$1" "$2"
}
fail() {
	reply "${2:-400 Bad Request}" "text/plain"
	echo "$1"
	exit 0
}
ok() {
	reply "200 OK" "application/json"
	echo '{"ok":true}'
	exit 0
}
# esc and val stay inside the shell: starting sed/tr for every value made
# GET /settings take seconds on a slow Toon
CR=$(printf '\r')
NL='
'
esc() {
	e=${1//\\/\\\\}; e=${e//\"/\\\"}; e=${e//$CR/}; printf '%s' "${e//$NL/}"
}
# The programs' states are on the Toon's own MQTT broker (retoon/<name>);
# /run/toon/<name> below means that state, fetched once per request.
BUS=/tmp/api-bus.$$
trap 'rm -f $BUS' EXIT
bus_state() {	# name: its key=value lines; status 1 when there is no such state
	[ -f $BUS ] || toon-bus dump > $BUS 2>/dev/null
	f=; while IFS= read -r l || [ -n "$l" ]; do
		case $l in "$1/"*) printf '%s\n' "${l#"$1/"}"; f=1 ;; esac
	done < $BUS
	[ -n "$f" ]
}
val() {	# file, key: the last value of key= in the file (or state)
	case $1 in
	/run/toon/boiler|/run/toon/climate|/run/toon/control|/run/toon/esphome|/run/toon/m4|\
	/run/toon/meter|/run/toon/meters|/run/toon/opentherm|/run/toon/thread|/run/toon/ui|\
	/run/toon/voc|/run/toon/zwave)
		t=$(bus_state "${1#/run/toon/}") || return 1
		v=; while IFS= read -r l; do
			case $l in "$2="*) v=${l#"$2="} ;; esac
		done <<EOT
$t
EOT
		printf '%s\n' "$v"
		return 0 ;;
	esac
	[ -f "$1" ] || return 1
	v=; while IFS= read -r l || [ -n "$l" ]; do
		case $l in "$2="*) v=${l#"$2="} ;; esac
	done < "$1"
	printf '%s\n' "$v"
}
body() { head -c "${CONTENT_LENGTH:-0}"; }
rw() { mount -o remount,rw /mnt/data && mkdir -p $DIR && chmod 700 $DIR; }
# not while toon-update is writing its download there
ro() { sync; [ -d /run/toon/update.lock ] || mount -o remount,ro /mnt/data 2>/dev/null; }

# Which system the next start runs: retoon or vendor (the original software). The Toon 2 does it
# with boot.scr on p1, the Toon 1 through the U-Boot environment (toon-boot).
next_boot() {
	if [ -x /usr/sbin/toon-boot ]; then
		toon-boot "$1" >/dev/null 2>&1
		return
	fi
	mkdir -p /mnt/boot
	mount -t vfat /dev/mmcblk3p1 /mnt/boot 2>/dev/null || return 1
	if [ "$1" = retoon ]; then
		cp /mnt/data/toon-os/boot.scr /mnt/boot/boot.new && sync && mv /mnt/boot/boot.new /mnt/boot/boot.scr
	else
		rm -f /mnt/boot/boot.scr
	fi
	sync
	umount /mnt/boot
}

# Pass on what toon-ha wrote to /tmp/toon-ha.$$: JSON, or its error= line.
ha_reply() {	# first character of a good answer
	f=/tmp/toon-ha.$$
	if [ "$(head -c 1 $f)" = "$1" ]; then
		reply "200 OK" "application/json"
		cat $f
		rm -f $f
	else
		msg=$(sed -n 's/^error=//p' $f | head -n 1)
		rm -f $f
		fail "${msg:-Home Assistant is not connected yet (see Settings)}" "502 Bad Gateway"
	fi
}

# Replace a key=value file with the allowed keys from the request body.
save_conf() {	# file, allowed keys...
	file=$1; shift
	tmp=/tmp/toon-conf.$$
	body | tr -d '\r' > $tmp.in
	: > $tmp
	for key in "$@"; do
		line=$(grep -m 1 "^$key=" $tmp.in) && echo "$line" >> $tmp
	done
	rm -f $tmp.in
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	cp $tmp "$file" && chmod 600 "$file"
	rm -f $tmp
	ro
}

if [ "$REQUEST_METHOD" = POST ]; then
	case "$CONTENT_TYPE" in
	application/x-toon*) ;;
	*) fail "Wrong content type" "403 Forbidden" ;;
	esac
	[ "${CONTENT_LENGTH:-0}" -le 2000000 ] || fail "Too large" "413 Payload Too Large"
fi

# the Z-Wave network keys of the original software, taken over by the setup
# app (config/zwave-keys.xml) or still in its own place on the data partition:
# "S0 <key> S2U <key> S2A <key> S2C <key>", for Z-Wave JS
zwave_keys() {
	f=$DIR/zwave-keys.xml
	[ -s $f ] || f=/mnt/data/qmf/var/hdrv_zwave/s2keystore.xml
	[ -s $f ] || return 0
	sed -e 's/<s0NetworkKey>\([0-9A-Fa-f]*\)</S0 \1 </' -e 's/<s2NetworkKeyUnauthenticated>\([0-9A-Fa-f]*\)</S2U \1 </' \
	    -e 's/<s2NetworkKeyAuthenticated>\([0-9A-Fa-f]*\)</S2A \1 </' -e 's/<s2NetworkKeyAccess>\([0-9A-Fa-f]*\)</S2C \1 </' \
	    -e 's/<ecdhPrivateKey>[^<]*</</' -e 's/<[^>]*>//g' $f | tr -s ' \n' '  '
}
case "$REQUEST_METHOD $PATH_INFO" in
"GET /apps")
	# the installed apps: manifest and saved settings of each
	reply "200 OK" "application/json"
	printf '['
	sep=
	for m in /mnt/data/toon-os/apps/*/manifest.json; do
		[ -f "$m" ] || continue
		id=$(basename "$(dirname "$m")")
		printf '%s{"dir":"%s","manifest":' "$sep" "$(esc "$id")"
		cat "$m"
		printf ',"values":'
		if [ -s "$DIR/apps/$id.json" ]; then cat "$DIR/apps/$id.json"; else printf '{}'; fi
		printf '}'
		sep=,
	done
	printf ']'
	;;
"GET /apps/catalogue")
	# what the app catalogue offers, and the app level this ReToon has
	tmp=/tmp/toon-apps.$$
	if ! toon-apps list > $tmp 2> $tmp.err; then
		msg=$(cat $tmp.err); rm -f $tmp $tmp.err
		fail "$msg" "502 Bad Gateway"
	fi
	reply "200 OK" "application/json"
	printf '{"api":%s,"index":' "$(cat /usr/share/toon/apps-api 2>/dev/null || echo 1)"
	cat $tmp
	printf '}'
	rm -f $tmp $tmp.err
	;;
"POST /apps/"*"/install"|"POST /apps/"*"/remove")
	id=${PATH_INFO#/apps/}
	what=${id##*/}
	id=${id%/*}
	msg=$(toon-apps $what "$id" 2>&1 >/dev/null) || fail "$msg" "500 Internal Server Error"
	# the screen loads its apps when it starts
	(/etc/init.d/S50toon-ui restart >/dev/null 2>&1 </dev/null &)
	ok
	;;
"POST /apps/"*"/settings")
	id=${PATH_INFO#/apps/}
	id=${id%/settings}
	case "$id" in ""|*[!a-z0-9_-]*) fail "Not an app name" ;; esac
	[ -f /mnt/data/toon-os/apps/$id/manifest.json ] || fail "No such app" "404 Not Found"
	tmp=/tmp/toon-app.$$
	body > $tmp
	head -c 1 $tmp | grep -q '{' || { rm -f $tmp; fail "Not a settings object"; }
	rw && mkdir -p $DIR/apps || fail "Cannot write the settings" "500 Internal Server Error"
	cp $tmp $DIR/apps/$id.json.new && chmod 600 $DIR/apps/$id.json.new && mv $DIR/apps/$id.json.new $DIR/apps/$id.json
	rm -f $tmp
	ro
	ok
	;;
"GET /dashboard")
	reply "200 OK" "application/json"
	if [ -s $DIR/dashboard.json ]; then cat $DIR/dashboard.json; else cat $DEFAULT; fi
	;;
"POST /dashboard")
	tmp=/tmp/toon-dash.$$
	body > $tmp
	# a dashboard is a JSON object with views; anything else is a mistake
	head -c 1 $tmp | grep -q '{' && grep -q '"views"' $tmp || { rm -f $tmp; fail "Not a dashboard"; }
	rw || fail "Cannot write the dashboard" "500 Internal Server Error"
	cp $tmp $DIR/dashboard.json.new && mv $DIR/dashboard.json.new $DIR/dashboard.json
	rm -f $tmp
	ro
	ok
	;;
"POST /dashboard/reset")
	rw && rm -f $DIR/dashboard.json
	ro
	ok
	;;
"GET /settings")
	reply "200 OK" "application/json"
	auth=none
	[ -n "$(val $DIR/ha.conf refresh_token)" ] && auth=login
	[ -n "$(val $DIR/ha.conf token)" ] && auth=token
	[ "$(val $DIR/ha.conf made_by)" = login ] && auth=login
	pw=0
	[ -n "$(val $DIR/mqtt.conf password)" ] && pw=1
	bpw=0
	[ -n "$(val $DIR/broker.conf password)" ] && bpw=1
	radio=$(val $DIR/radio.conf mode)
	[ -x /usr/bin/gp711-rcpd ] || radio=none
	T=/tmp/toon-thread.$$
	: > $T
	[ "$radio" = thread ] && toon-thread status > $T 2>/dev/null
	cat <<JSON
{"hostname":"$(esc "$(hostname)")","own_hostname":"$(esc "$(head -n 1 $DIR/hostname 2>/dev/null)")",
"default_hostname":"toon-$(esc "$(val /run/toon/id id)")",
"autoboot":"$([ -e $DIR/autoboot ] && echo 1 || echo 0)",
"root_pw_own":$([ -s $DIR/root.hash ] && echo 1 || echo 0),
"zwave_mode":"$(esc "$(val $DIR/zwave.conf mode)")","zwave_allow":"$(esc "$(val $DIR/zwave.conf allow)")",
"zwave_state":"$(esc "$(val /run/toon/zwave state)")","zwave_client":"$(esc "$(val /run/toon/zwave client)")",
"zwave_meters":"$(esc "$(val $DIR/zwave.conf meters)")","zwave_found":"$(esc "$(val /run/toon/meters found)")",
"update_auto":"$(esc "$(val $DIR/update.conf auto)")","apps_auto":$([ -e $DIR/apps-autoupdate ] && echo 1 || echo 0),
"board":"$(grep -q 'Toon 1' /proc/device-tree/model 2>/dev/null && echo toon1 || echo toon2)",
"zwave_keys":"$(esc "$(zwave_keys)")",
"sound_available":"$([ -x /usr/bin/toon-sound ] && echo 1 || echo 0)","sound_volume":"$(esc "$(v=$(val $DIR/sound.conf volume); [ -n "$v" ] || { grep -q 'Toon 1' /proc/device-tree/model 2>/dev/null && v=30 || v=70; }; echo $v)")",
"sound_ready":"$( { [ -e /dev/snd/pcmC0D0p ] || [ "$(val /run/toon/m4 sound)" = on ]; } && echo 1 || echo 0)",
"esphome_available":"$([ -x /usr/bin/toon-esphomed ] && echo 1 || echo 0)",
"esphome_mode":"$(esc "$(val $DIR/esphome.conf mode)")","esphome_allow":"$(esc "$(val $DIR/esphome.conf allow)")",
"esphome_state":"$(esc "$(val /run/toon/esphome state)")",
"esphome_client":"$(esc "$(val /run/toon/esphome client)")","esphome_scanning":"$(esc "$(val /run/toon/esphome scanning)")",
"esphome_adverts":"$(esc "$(val /run/toon/esphome adverts_per_minute)")",
"radio_mode":"$(esc "${radio:-zigbee}")","thread_state":"$(esc "$(val $T state)")",
"thread_name":"$(esc "$(val $T name)")","thread_channel":"$(esc "$(val $T channel)")",
"thread_children":"$(esc "$(val $T children)")","thread_routers":"$(esc "$(val $T routers)")",
"ip":"$(esc "$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')")",
"ha_url":"$(esc "$(val $DIR/ha.conf url)")","ha_auth":"$auth","ha_state":"$(esc "$(val /run/toon/ui ha)")","ha_addr":"$(esc "$(val /run/toon/ui ha_addr)")",
"ha_user":"$(esc "$(val $DIR/ha.conf user)")","ha_made":"$(esc "$(val $DIR/ha.conf made)")","ha_verify":"$(esc "$(val $DIR/ha.conf verify)")",
"web_http":"$(esc "$(val $DIR/web.conf http)")","web_fingerprint":"$(esc "$(val /run/toon/web fingerprint)")",
"mqtt_host":"$(esc "$(val $DIR/mqtt.conf host)")","mqtt_port":"$(esc "$(val $DIR/mqtt.conf port)")",
"mqtt_username":"$(esc "$(val $DIR/mqtt.conf username)")","mqtt_password_set":$pw,
"mqtt_discovery_prefix":"$(esc "$(val $DIR/mqtt.conf discovery_prefix)")",
"broker_lan":"$(esc "$(val $DIR/broker.conf lan)")","broker_username":"$(esc "$(val $DIR/broker.conf username)")",
"broker_password_set":$bpw,"broker_running":$(pidof mosquitto >/dev/null && echo 1 || echo 0),
"broker_listening":"$(esc "$(cut -d' ' -f2 /run/toon/broker.ips 2>/dev/null | tr '\n' ' ')")",
"brightness":"$(esc "$(val $DIR/ui.conf brightness)")","dim_after":"$(esc "$(val $DIR/ui.conf dim_after)")",
"dim_level":"$(esc "$(val $DIR/ui.conf dim_level)")","dim_grey":"$(esc "$(val $DIR/ui.conf dim_grey)")",
"home_auto":"$(esc "$(val $DIR/ui.conf home_auto)")","home_after":"$(esc "$(val $DIR/ui.conf home_after)")",
"page_anim":"$(esc "$(v=$(val $DIR/ui.conf page_anim); [ -n "$v" ] || { grep -q 'Toon 1' /proc/device-tree/model 2>/dev/null && v=0 || v=1; }; echo $v)")",
"thermostat_buttons":"$(esc "$(val $DIR/ui.conf thermostat_buttons)")",
"sleep_off":"$(esc "$(val $DIR/ui.conf sleep_off)")","off_from":"$(esc "$(val $DIR/ui.conf off_from)")",
"off_to":"$(esc "$(val $DIR/ui.conf off_to)")",
"language":"$(esc "$(val $DIR/language lang || echo nl)")"}
JSON
	rm -f $T
	;;
"POST /settings/ui")
	save_conf $DIR/ui.conf brightness dim_after dim_level dim_grey home_auto home_after page_anim thermostat_buttons \
		sleep_off off_from off_to
	ok
	;;
"POST /settings/language")
	# nl (the default) or en, for this page and the screen; the screen
	# starts again in the new language
	lang=$(body | tr -d '\r' | sed -n 's/^lang=//p' | head -n 1)
	case "$lang" in nl|en) ;; *) fail "Unknown language" ;; esac
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	echo "lang=$lang" > $DIR/language
	ro
	(/etc/init.d/S50toon-ui restart >/dev/null 2>&1 &)
	ok
	;;
"POST /settings/mqtt")
	old=$(val $DIR/mqtt.conf password)
	save_conf $DIR/mqtt.conf host port username password discovery_prefix keep_password
	if grep -q '^keep_password=1' $DIR/mqtt.conf; then
		rw
		grep -v -e '^password=' -e '^keep_password=' $DIR/mqtt.conf > /tmp/toon-mqtt.$$
		[ -n "$old" ] && echo "password=$old" >> /tmp/toon-mqtt.$$
		cp /tmp/toon-mqtt.$$ $DIR/mqtt.conf
		rm -f /tmp/toon-mqtt.$$
		ro
	fi
	[ -n "$(val $DIR/mqtt.conf host)" ] || { rw; rm -f $DIR/mqtt.conf; ro; }
	(/etc/init.d/S70toon-mqttd restart >/dev/null 2>&1 &)
	ok
	;;
"POST /settings/broker")
	# the Toon's own MQTT broker, open to the network or not (on 127.0.0.1
	# it always is, without a password)
	old=$(val $DIR/broker.conf password)
	save_conf $DIR/broker.conf lan username password keep_password
	if grep -q '^keep_password=1' $DIR/broker.conf; then
		rw
		grep -v -e '^password=' -e '^keep_password=' $DIR/broker.conf > /tmp/toon-broker.$$
		[ -n "$old" ] && echo "password=$old" >> /tmp/toon-broker.$$
		cp /tmp/toon-broker.$$ $DIR/broker.conf
		rm -f /tmp/toon-broker.$$
		ro
	fi
	if [ "$(val $DIR/broker.conf lan)" = 1 ]; then
		u=$(val $DIR/broker.conf username) why=
		echo "$u" | grep -q '[^A-Za-z0-9._-]' && why="Use letters, digits and . _ - in the user name"
		[ -n "$u" ] && [ -n "$(val $DIR/broker.conf password)" ] ||
			why="The network needs a user name and a password"
		if [ -n "$why" ]; then	# saved, but not opened
			rw; sed -i 's/^lan=1$/lan=0/' $DIR/broker.conf; ro
			fail "$why"
		fi
	fi
	(/etc/init.d/S41toon-broker restart >/dev/null 2>&1 &)
	ok
	;;
"POST /settings/password")
	new=$(body | tr -d '\r' | sed -n 's/^password=//p' | head -n 1)
	[ -n "$new" ] || fail "Fill in a password"
	case "$new" in *:*) fail "A colon cannot be used in the password" ;; esac
	http=$(val $DIR/web.conf http)
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	echo "password=$new" > $DIR/web.conf
	[ -n "$http" ] && echo "http=$http" >> $DIR/web.conf
	chmod 600 $DIR/web.conf
	ro
	# lighttpd reads it at every login check
	(umask 077; echo "admin:$new" > /run/toon/web-users.new) &&
		mv /run/toon/web-users.new /run/toon/web-users
	ok
	;;
"POST /settings/update")
	# updating by itself (toon-autoupdate): off, hourly, nightly, weekly
	save_conf $DIR/update.conf auto
	case "$(val $DIR/update.conf auto)" in
	hourly|nightly|weekly) ;;
	*) rw; rm -f $DIR/update.conf; ro ;;
	esac
	ok
	;;
"POST /settings/apps-update")
	# the apps updated by themselves, with ReToon's own update (toon-autoupdate)
	on=$(body | tr -d '\r' | sed -n 's/^on=//p' | head -n 1)
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	if [ "$on" = 1 ]; then : > $DIR/apps-autoupdate; else rm -f $DIR/apps-autoupdate; fi
	ro
	ok
	;;
"POST /settings/zwave")
	# the Z-Wave controller on TCP port 6639 (toon-zwaved): off, or for
	# Z-Wave JS in Home Assistant; allow = the one address that may connect
	# meters = the meter adapters (nodes) whose readings it reads on the side
	was="$(val $DIR/zwave.conf mode) $(val $DIR/zwave.conf allow)"
	save_conf $DIR/zwave.conf mode allow meters
	case "$(val $DIR/zwave.conf mode)" in
	remote|local) ;;
	*) rw; rm -f $DIR/zwave.conf; ro ;;
	esac
	if val $DIR/zwave.conf allow | grep -q '[^0-9.]'; then
		rw; rm -f $DIR/zwave.conf; ro
		fail "The address has to look like 192.168.1.10"
	fi
	if val $DIR/zwave.conf meters | grep -q '[^0-9 ]'; then
		rw; rm -f $DIR/zwave.conf; ro
		fail "Wrong meter adapter"
	fi
	# only the meters changed: toon-zwaved reads them itself, and the
	# client (Z-Wave JS) keeps its connection
	[ "$was" = "$(val $DIR/zwave.conf mode) $(val $DIR/zwave.conf allow)" ] ||
		(/etc/init.d/S61toon-zwaved restart >/dev/null 2>&1 &)
	ok
	;;
"POST /settings/sound")
	# toon-sound's volume (0..100), both Toons
	save_conf $DIR/sound.conf volume
	case "$(val $DIR/sound.conf volume)" in
	[0-9]|[1-9][0-9]|100) ;;
	*) rw; rm -f $DIR/sound.conf; ro; fail "The volume has to be 0 to 100" ;;
	esac
	ok
	;;
"POST /sound/test")
	v=$(body | tr -d '\r' | sed -n 's/^volume=\([0-9]*\)$/\1/p' | head -n 1)
	[ -n "$v" ] && [ "$v" -le 100 ] 2>/dev/null || v=
	(toon-sound ${v:+-v $v} test >/dev/null 2>&1 &)
	ok
	;;
"POST /settings/esphome")
	# the Bluetooth proxy for Home Assistant (toon-esphomed, ESPHome native
	# API on TCP port 6053): on (the default) or off; allow = the one address
	# that may connect. Home Assistant sets passive/active scanning itself;
	# scan= in the file is only the fallback and is kept if present.
	save_conf $DIR/esphome.conf mode allow scan
	case "$(val $DIR/esphome.conf mode)" in
	on|off) ;;
	*) rw; rm -f $DIR/esphome.conf; ro; fail "mode has to be on or off" ;;
	esac
	if val $DIR/esphome.conf allow | grep -q '[^0-9.]'; then
		rw; rm -f $DIR/esphome.conf; ro
		fail "The address has to look like 192.168.1.10"
	fi
	case "$(val $DIR/esphome.conf scan)" in
	""|passive|active) ;;
	*) rw; rm -f $DIR/esphome.conf; ro; fail "scan has to be passive or active" ;;
	esac
	(/etc/init.d/S71toon-esphomed restart >/dev/null 2>&1 &)
	ok
	;;
"GET /boiler/esphome")
	# the OpenTherm module on an ESP (toon-boilerd, ReToon-esphome): its
	# settings, with the API key and OTA password the YAML needs
	F=$DIR/boiler-esphome
	reply "200 OK" "application/json"
	printf '{"on":%s' "$([ -s $F ] && echo true || echo false)"
	for k in host name board in_pin out_pin one_wire_pin key ota; do
		printf ',"%s":"%s"' $k "$(esc "$(val $F $k)")"
	done
	echo '}'
	;;
"POST /boiler/esphome")
	# "off": back to the boiler module on the cable. Otherwise name= board=
	# in_pin= out_pin= (host= optional, default <name>.local; one_wire_pin=
	# for a 1-wire temperature sensor on the module, empty: none): saved, with
	# a new API key and OTA password only the first time, so a flashed ESP
	# keeps working after a change here; a port= line put in by hand stays.
	# toon-boilerd starts again with it.
	F=$DIR/boiler-esphome
	in=$(body | tr -d '\r')
	if [ "$in" = off ]; then
		rw; rm -f $F; ro
		(/etc/init.d/S46toon-boilerd restart >/dev/null 2>&1 &)
		ok
	fi
	get() { printf '%s\n' "$in" | sed -n "s/^$1=//p" | head -n 1; }
	name=$(get name); board=$(get board); in_pin=$(get in_pin); out_pin=$(get out_pin); host=$(get host)
	one_wire_pin=$(get one_wire_pin)
	echo "$name" | grep -q -E '^[a-z0-9][a-z0-9-]{0,30}$' || fail "The name may only have lower-case letters, digits and -"
	case "$board" in esp32|esp8266) ;; *) fail "Choose the board" ;; esac
	for p in "$in_pin" "$out_pin" ${one_wire_pin:+"$one_wire_pin"}; do
		echo "$p" | grep -q -E '^GPIO[0-9]{1,2}$' || fail "Pins look like GPIO4"
	done
	[ -z "$one_wire_pin" ] || { [ "$one_wire_pin" != "$in_pin" ] && [ "$one_wire_pin" != "$out_pin" ]; } ||
		fail "The temperature sensor needs a pin of its own"
	[ -n "$host" ] || host=$name.local
	echo "$host" | grep -q -E '^[A-Za-z0-9.-]{1,100}$' || fail "The address has to be a name or an IP address"
	key=$(val $F key); ota=$(val $F ota); port=$(val $F port)
	[ -n "$key" ] || key=$(head -c 32 /dev/urandom | base64)
	[ -n "$ota" ] || ota=$(head -c 300 /dev/urandom | tr -dc 'a-zA-Z0-9' | head -c 16)
	[ ${#key} = 44 ] && [ ${#ota} -ge 8 ] || fail "Could not make a key" "500 Internal Server Error"
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	printf 'host=%s\nname=%s\nboard=%s\nin_pin=%s\nout_pin=%s\nkey=%s\nota=%s\n' \
		"$host" "$name" "$board" "$in_pin" "$out_pin" "$key" "$ota" > $F.new &&
		{ [ -z "$one_wire_pin" ] || echo "one_wire_pin=$one_wire_pin" >> $F.new; } &&
		{ [ -z "$port" ] || echo "port=$port" >> $F.new; } && chmod 600 $F.new && mv $F.new $F
	ro
	(/etc/init.d/S46toon-boilerd restart >/dev/null 2>&1 &)
	ok
	;;
"POST /settings/radio")
	# the GP711 radio: zigbee (zigbee2mqtt on TCP port 6638), thread
	# (border router, S62toon-thread) or off
	save_conf $DIR/radio.conf mode
	case "$(val $DIR/radio.conf mode)" in
	thread|off) ;;
	*) rw; rm -f $DIR/radio.conf; ro ;;
	esac
	(/etc/init.d/S62toon-thread stop; /etc/init.d/S60gp711-rcpd restart
	 /etc/init.d/S62toon-thread start) >/dev/null 2>&1 </dev/null &
	ok
	;;
"POST /settings/thread")
	# join another border router's network (its dataset in hex), or
	# start a new one of this Toon's own
	[ "$(val $DIR/radio.conf mode)" = thread ] || fail "The radio is not set to Thread"
	req=$(body | tr -d '\r')
	dataset=$(echo "$req" | sed -n 's/^dataset=//p' | head -n 1 | tr -d ' ')
	if [ "$(echo "$req" | sed -n 's/^action=//p' | head -n 1)" = new ]; then
		toon-thread form
	else
		msg=$(toon-thread join "$dataset" 2>&1) || fail "${msg:-The dataset was not accepted}"
	fi
	toon-thread save
	ok
	;;
"POST /settings/rootpw")
	# password of root for SSH; only its hash is kept (S39toon-rootpw puts
	# it back at every start). The password comes in on stdin, not as an
	# argument, so it never shows in the process list.
	new=$(body | tr -d '\r' | sed -n 's/^password=//p' | head -n 1)
	[ -n "$new" ] || fail "Fill in a password"
	hash=$(printf '%s' "$new" | mkpasswd -m sha512 -P 0 2>/dev/null)
	case "$hash" in '$6$'*) ;; *) fail "Cannot make the password" "500 Internal Server Error" ;; esac
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	echo "$hash" > $DIR/root.hash.new && chmod 600 $DIR/root.hash.new && mv $DIR/root.hash.new $DIR/root.hash
	ro
	/etc/init.d/S39toon-rootpw start
	ok
	;;
"POST /settings/factory")
	# Back to how a new ReToon starts: everything in the configuration
	# directory goes (WiFi, Home Assistant, MQTT, pages, thermostat, the
	# passwords, the certificate). The system itself and the radio firmware
	# stay. Then restart; the Toon comes back on its WiFi screen.
	[ "$(body | tr -d '\r' | head -n 1)" = "confirm=factory-defaults" ] || fail "Bad request"
	# the restart has to come back to ReToon, also when "Always start" was off
	next_boot retoon || fail "Cannot reach the boot partition" "500 Internal Server Error"
	reply "200 OK" "application/json"
	echo '{"ok":true}'
	# stop what writes its own state there, then empty it and restart
	(sleep 1
	 for s in S70toon-mqttd S50toon-ui S46toon-boilerd; do /etc/init.d/$s stop; done
	 sleep 1
	 mount -o remount,rw /mnt/data &&
		find $DIR -mindepth 1 -maxdepth 1 -exec rm -rf {} +
	 sync
	 mount -o remount,ro /mnt/data
	 reboot) >/dev/null 2>&1 &
	exit 0
	;;
"POST /settings/hostname")
	# the name on the network: toon-<id> again when left empty
	new=$(body | tr -d '\r' | sed -n 's/^hostname=//p' | head -n 1)
	echo "$new" | grep -q '[^a-z0-9-]' && fail "Use small letters, digits and - only"
	case "$new" in -*|*-) fail "The name cannot start or end with -" ;; esac
	[ ${#new} -le 32 ] || fail "Use at most 32 characters"
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	if [ -n "$new" ]; then echo "$new" > $DIR/hostname; else rm -f $DIR/hostname; fi
	# a certificate carries the name, so a new one is made
	rm -f $DIR/web-cert.pem $DIR/web-key.pem
	ro
	(sleep 1
	 /etc/init.d/S38toon-hostname start
	 /etc/init.d/S50avahi-daemon stop; sleep 1; /etc/init.d/S50avahi-daemon start
	 /etc/init.d/S70toon-mqttd restart
	 /etc/init.d/S51toon-web restart) >/dev/null 2>&1 &
	ok
	;;
"POST /settings/boot")
	# Start this system at every boot, or go back to the original software.
	# Toon 2: boot.scr on p1 is what makes the bootloader come here; the
	# initrd removes it at every start and S98toon-keep-boot puts it back when
	# config/autoboot exists (off by default). Toon 1: the same through the
	# U-Boot environment (toon-boot).
	on=$(body | tr -d '\r' | sed -n 's/^autoboot=//p' | head -n 1)
	case "$on" in 0|1) ;; *) fail "Bad request" ;; esac
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	rm -f $DIR/no-autoboot	# the old way of saying "off"
	if [ "$on" = 1 ]; then : > $DIR/autoboot; else rm -f $DIR/autoboot; fi
	ro
	if [ "$on" = 1 ]; then next_boot retoon; else next_boot vendor; fi ||
		fail "Cannot reach the boot partition" "500 Internal Server Error"
	# U-Boot's start-up picture follows it (in the background: a moment's work)
	(toon-bootsplash sync >/dev/null 2>&1 &)
	ok
	;;
"POST /settings/web")
	# plain http next to https, or only the redirect to https
	http=$(body | tr -d '\r' | sed -n 's/^http=//p' | head -n 1)
	case "$http" in on|redirect) ;; *) fail "Bad request" ;; esac
	pw=$(val $DIR/web.conf password)
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	printf 'password=%s\nhttp=%s\n' "$pw" "$http" > $DIR/web.conf
	chmod 600 $DIR/web.conf
	ro
	(sleep 1; /etc/init.d/S51toon-web restart) >/dev/null 2>&1 &
	ok
	;;
"POST /ha/token")
	# a token made by hand: try it on Home Assistant before keeping it, so
	# a wrong address or token is said here, not only later on the card
	req=$(body | tr -d '\r')
	url=$(echo "$req" | sed -n 's/^url=//p' | head -n 1 | sed -E 's#^(https?://[^/]+).*#\1#')
	token=$(echo "$req" | sed -n 's/^token=//p' | head -n 1)
	verify=$(echo "$req" | sed -n 's/^verify=//p' | head -n 1)
	case "$url" in http://?*|https://?*) ;; *) fail "Enter the address of Home Assistant" ;; esac
	echo "$url" | grep -q '[^A-Za-z0-9:/._-]' && fail "Bad Home Assistant address"
	[ -n "$token" ] || fail "Paste the token first"
	echo "$token" | grep -q '[^A-Za-z0-9._-]' && fail "That is not an access token: copy it again from Home Assistant"
	[ "$verify" = 1 ] || verify=0
	k=-k
	[ $verify = 1 ] && k=
	hdr=/tmp/toon-ha-hdr.$$
	(umask 077; printf 'Authorization: Bearer %s\n' "$token" > $hdr)
	code=$(curl -s $k -m 10 -o /dev/null -w '%{http_code}' -H @$hdr "$url/api/")
	rm -f $hdr
	case "$code" in
	200|201) ;;
	401|403) fail "Home Assistant refused this token" "502 Bad Gateway" ;;
	000) fail "The Toon cannot reach Home Assistant at ${url#*://}" "502 Bad Gateway" ;;
	*) fail "That address is not Home Assistant" "502 Bad Gateway" ;;
	esac
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	printf 'url=%s\ntoken=%s\nverify=%s\n' "$url" "$token" "$verify" > $DIR/ha.conf
	chmod 600 $DIR/ha.conf
	ro
	ok
	;;
"POST /ha/logout")
	rw && rm -f $DIR/ha.conf
	ro
	ok
	;;
"POST /ha/login")
	# finish the Home Assistant login: trade the one-time code, then have
	# Home Assistant make a long-lived access token named after this Toon.
	# Only that token is kept, on the Toon.
	req=$(body | tr -d '\r')
	url=$(echo "$req" | sed -n 's/^url=//p' | head -n 1)
	code=$(echo "$req" | sed -n 's/^code=//p' | head -n 1)
	client=$(echo "$req" | sed -n 's/^client_id=//p' | head -n 1)
	verify=$(echo "$req" | sed -n 's/^verify=//p' | head -n 1)
	case "$url" in http://*|https://*) ;; *) fail "Enter the address of Home Assistant" ;; esac
	echo "$code$client$url" | grep -q '[^A-Za-z0-9:/._-]' && fail "Bad request"
	[ "$verify" = 1 ] || verify=0
	out=$(HA_VERIFY=$verify /usr/bin/toon-ha login "${url%/}" "$code" "$client")
	token=$(echo "$out" | sed -n 's/^token=//p')
	[ -n "$token" ] || fail "$(echo "$out" | sed -n 's/^error=//p' | grep . || echo "Home Assistant did not accept the login")" "502 Bad Gateway"
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	user=$(echo "$out" | sed -n 's/^user=//p' | head -n 1)
	printf 'url=%s\ntoken=%s\nverify=%s\nmade_by=login\nuser=%s\nmade=%s\n' "${url%/}" "$token" "$verify" \
		"$user" "$(date +%Y-%m-%d)" > $DIR/ha.conf
	chmod 600 $DIR/ha.conf
	ro
	ok
	;;
"GET /ha/entities")
	# what Home Assistant has, asked by the Toon so no token leaves it
	/usr/bin/toon-ha entities > /tmp/toon-ha.$$
	ha_reply '['
	;;
"GET /ha/dashboards"|"GET /ha/dashboard")
	if [ "$PATH_INFO" = /ha/dashboards ]; then
		cmd='{"type":"lovelace/dashboards/list"}'
	else
		path=$(echo "$QUERY_STRING" | sed -n 's/^path=//p')
		echo "$path" | grep -q '[^A-Za-z0-9_-]' && fail "Bad request"
		if [ -n "$path" ]; then
			cmd="{\"type\":\"lovelace/config\",\"url_path\":\"$path\",\"force\":false}"
		else
			cmd='{"type":"lovelace/config","url_path":null,"force":false}'
		fi
	fi
	/usr/bin/toon-ha ws "$cmd" > /tmp/toon-ha.$$
	ha_reply '{'
	;;
"GET /thermostat")
	reply "200 OK" "application/json"
	if [ -s $DIR/thermostat.json ]; then cat $DIR/thermostat.json; else echo '{}'; fi
	;;
"POST /thermostat")
	# sensors, presets, week program, smart options: read by toon-boilerd
	# (and by toon-ui for the sensors) when the file changes
	tmp=/tmp/toon-th.$$
	body > $tmp
	head -c 1 $tmp | grep -q '{' && grep -q '"presets"' $tmp || { rm -f $tmp; fail "Not thermostat settings"; }
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	cp $tmp $DIR/thermostat.json.new && mv $DIR/thermostat.json.new $DIR/thermostat.json
	rm -f $tmp
	ro
	ok
	;;
"GET /thermostat/state")
	reply "200 OK" "application/json"
	printf '{'
	for k in hvac target room room_source preset program hold preheat frost next until eff_target \
		heat heat_rate cool_rate measured flow link mode kloss_n from_vendor cool_per_10 cool_rate_now after_10 preheat_extra preheat_n; do
		printf '"%s":"%s",' $k "$(esc "$(val /run/toon/boiler $k)")"
	done
	printf '"sensors":"%s","of":"%s","own":"%s","own_uncorrected":"%s"}\n' "$(esc "$(val /run/toon/control sensors)")" \
		"$(esc "$(val /run/toon/control of)")" "$(esc "$(val /run/toon/climate temperature)")" "$(esc "$(val /run/toon/climate uncorrected)")"
	;;
"GET /thermostat/measurements")
	# the measurement log (toon-boilerd), older file first, one header
	printf 'Status: 200 OK\r\nContent-Type: text/csv; charset=utf-8\r\n'
	printf 'Content-Disposition: attachment; filename="retoon-measurements.csv"\r\nCache-Control: no-store\r\n\r\n'
	M=/mnt/data/toon-os/thermostat-measurements.csv
	if [ -s $M.1 ]; then
		cat $M.1
		[ -s $M ] && tail -n +2 $M
	else
		cat $M 2>/dev/null
	fi
	;;
"GET /thermostat/log")
	reply "200 OK" "text/plain; charset=utf-8"
	cat /mnt/data/toon-os/thermostat-changes.log.1 /mnt/data/toon-os/thermostat-changes.log 2>/dev/null
	;;
"POST /thermostat/cmd")
	cmd=$(body | tr -d '\r' | head -n 1)
	echo "$cmd" | grep -q -E '^(hvac (heat|off)|mode (ot|onoff)|program (resume|on|off)|preset (comfort|home|sleep|away)|target [0-9]+(\.[0-9])?)$' || fail "Unknown command"
	val /run/toon/boiler link >/dev/null || fail "The thermostat is not running" "503 Service Unavailable"
	toon-bus send retoon/boiler/set "@web $cmd" || fail "The thermostat is not running" "503 Service Unavailable"
	ok
	;;
"GET /mqtt/entities")
	# what the screen found on the MQTT broker (discovery) plus the user's own topics
	reply "200 OK" "application/json"
	if [ -s /run/toon/mqtt-entities.json ]; then cat /run/toon/mqtt-entities.json; else echo '[]'; fi
	;;
"GET /mqtt/topics")
	reply "200 OK" "application/json"
	if [ -s $DIR/mqtt-topics.json ]; then cat $DIR/mqtt-topics.json; else echo '[]'; fi
	;;
"POST /mqtt/topics")
	tmp=/tmp/toon-topics.$$
	body > $tmp
	head -c 1 $tmp | grep -q '\[' || { rm -f $tmp; fail "Not a list"; }
	rw || fail "Cannot write the settings" "500 Internal Server Error"
	cp $tmp $DIR/mqtt-topics.json.new && mv $DIR/mqtt-topics.json.new $DIR/mqtt-topics.json
	chmod 600 $DIR/mqtt-topics.json
	rm -f $tmp
	ro
	ok
	;;
"GET /status")
	reply "200 OK" "application/json"
	ip=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
	sig=$(iw dev wlan0 link 2>/dev/null | sed -n 's/^\tsignal: \(-*[0-9]*\) dBm.*/\1/p')
	cat <<JSON
{"hostname":"$(esc "$(hostname)")","ip":"$(esc "$ip")","wifi_signal":"$(esc "$sig")",
"uptime":$(cut -d. -f1 /proc/uptime),
"ha":"$(esc "$(val /run/toon/ui ha)")","view":"$(esc "$(val /run/toon/ui view)")",
"boiler_link":"$(esc "$(val /run/toon/boiler link)")","boiler_mode":"$(esc "$(val /run/toon/boiler mode)")",
"boiler_message":"$(esc "$(val /run/toon/boiler message)")",
"temperature":"$(esc "$(val /run/toon/climate temperature)")","humidity":"$(esc "$(val /run/toon/climate humidity)")",
"mqtt":"$(esc "$(val /run/toon/ui mqtt)")",
"m4":"$(esc "$(val /run/toon/m4 text)")","air":"$(esc "$([ "$(val /run/toon/voc state)" = warmup ] && echo measuring || val /run/toon/voc quality)")","air_last":"$(esc "$(val /run/toon/voc quality_last)")",
"air_trend":"$(esc "$(val /run/toon/voc quality_trend)")","air_learning":"$(esc "$(val /run/toon/voc quality_learning)")",
"version":"$(esc "$(cat /etc/retoon-version 2>/dev/null)")",
"system":"$(esc "$(sed -n 's/^VERSION_ID=//p' /etc/os-release) $(uname -r)")"}
JSON
	;;
"GET /ws-token")
	# a token for the websocket /ws (toon-bus serve), valid once, for a
	# minute: another site's page can make the browser ask, never read it
	t=/run/toon/ws-tokens
	mkdir -p $t && chmod 700 $t
	find $t -type f -mmin +2 -exec rm -f {} \; 2>/dev/null
	tok=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
	[ ${#tok} = 32 ] || fail "No token" "500 Internal Server Error"
	: > $t/$tok
	reply "200 OK" "application/json"
	printf '{"token":"%s"}\n' "$tok"
	;;
"GET /meter")
	# the readings of the meter adapters that are read (toon-zwaved)
	reply "200 OK" "application/json"
	printf '{'
	bus_state meter | awk -F= 'NF == 2 && $1 != "time" { printf "%s\"%s\":\"%s\"", n++ ? "," : "", $1, $2 }'
	printf '}\n'
	;;
"GET /update")
	# what toon-update found or is doing
	reply "200 OK" "application/json"
	if [ -s /run/toon/update ]; then cat /run/toon/update; else echo '{"state":"none"}'; fi
	;;
"POST /update/check"|"POST /update/install")
	[ -d /run/toon/update.lock ] && fail "Already busy with an update" "409 Conflict"
	what=${PATH_INFO#/update/}
	# toon-update writes its own progress; this first line is for the page's next look
	if [ $what = check ]; then t="Looking for an update"; s=checking; else t="Starting the update"; s=working; fi
	printf '{"state":"%s","text":"%s"}\n' $s "$t" > /run/toon/update
	(/usr/bin/toon-update $what >/dev/null 2>&1 &)
	ok
	;;
"POST /restart")
	# the same as the Restart button on the screen
	[ -d /run/toon/update.lock ] && fail "An update is busy; restart when it is done" "409 Conflict"
	reply "200 OK" "application/json"
	echo '{"ok":true}'
	(sleep 1; sync; reboot) >/dev/null 2>&1 &
	exit 0
	;;
"GET /screen")
	# the panel as it is now, gzip: 1024x600 BGRX on the Toon 2, 800x480
	# RGB565 on the Toon 1 (X-Screen: width height bits per pixel)
	w=$(cut -d, -f1 /sys/class/graphics/fb0/virtual_size); h=$(cut -d, -f2 /sys/class/graphics/fb0/virtual_size)
	bpp=$(cat /sys/class/graphics/fb0/bits_per_pixel)
	printf 'Status: 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Encoding: gzip\r\nX-Screen: %s %s %s\r\nCache-Control: no-store\r\n\r\n' $w $h $bpp
	head -c $((w * h * bpp / 8)) /dev/fb0 | gzip -1
	;;
"POST /notify")
	# a message on the Toon's screen: JSON (see notify.c), on one line
	msg=$(body | tr '\r\n\t' '   ')
	msg=${msg%"${msg##*[! ]}"}
	case "$msg" in "{"*"}") ;; *) fail "A JSON object is needed" ;; esac
	[ ${#msg} -le 3800 ] || fail "Too long" "413 Payload Too Large"
	toon-bus send retoon/notify/set "$msg"
	ok
	;;
"POST /ui")
	cmd=$(body | tr -d '\r' | head -n 1)
	echo "$cmd" | grep -q -E '^(tap [0-9]+ [0-9]+|swipe [0-9]+ [0-9]+ [0-9]+ [0-9]+|view [0-9]+|view dim|close|calibrate|calibrate reset)$' || fail "Unknown command"
	toon-bus send retoon/ui/set "$cmd"
	ok
	;;
*)
	fail "Not found" "404 Not Found"
	;;
esac
