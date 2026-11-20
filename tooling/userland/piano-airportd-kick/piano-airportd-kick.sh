#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-airportd-kick: airportd restarted once PianoWifi80211's interface exists (docs/drivers/wifi-menu.md section 5).
# Boot wm3c (2026-11-14): the first airportd, started by launchd before our interface registered, logged only "Did NOT find
# matching WiFi interface" and never adopted it; the airportd that `launchctl kickstart -k` started after the interface existed
# logged "Successfully created CWFApple80211" and "WiFi network interface added". So this job waits for PianoWifi80211 to answer
# the apple80211 requests and for its interface to have a BSD name, then restarts airportd once (kickstart -k, whether or not it
# runs), and says what airportd is before and after, each line with the time.
# The gate: the kernel argument piano-wifi-apple80211 as PianoWifi read it (its sysctl machdep.piano_wifi_apple80211: boot 10q
# showed that an argument of the ADT's tail is not in kern.bootargs). Without it the job ends at once; when Wi-Fi does not come
# up within 600 s it ends having done nothing. After the restart, publish a
# boot-specific receipt for the CoreWLAN fallback.
say() {
  /usr/bin/logger -t piano-airportd-kick "$*"
  echo "piano-airportd-kick: $(/bin/date +%T) $*" > /dev/console 2> /dev/null
  echo "piano-airportd-kick: $(/bin/date +%T) $*"
}
case "$(/usr/sbin/sysctl -n machdep.piano_wifi_apple80211 2> /dev/null)" in
[1-9]*) ;;
*) exit 0 ;;
esac
# the controller answers and its interface has a name (the BSD name on PianoWifi80211's interface child)
bsd=""
i=0
while [ $i -lt 600 ]; do
  tree=$(/usr/sbin/ioreg -l -r -c PianoWifi80211 -d 2 2> /dev/null)
  if echo "$tree" | /usr/bin/grep -q '"piano-wifi-apple80211-answering"'; then
    bsd=$(echo "$tree" | /usr/bin/sed -n 's/.*"BSD Name" = "\(en[0-9]*\)".*/\1/p' | /usr/bin/head -1)
    [ -n "$bsd" ] && break
  fi
  sleep 1
  i=$((i + 1))
done
[ -n "$bsd" ] || exit 0
state() { /bin/launchctl print system/com.apple.airportd 2> /dev/null | /usr/bin/grep -E '^[[:space:]]*(state|pid) = ' | /usr/bin/tr -s ' \t' ' ' | /usr/bin/tr '\n' ' '; }
say "PianoWifi80211 answers and its interface is $bsd (after ${i} s); airportd before:$(state)"
sleep 3
say "launchctl kickstart -k system/com.apple.airportd (the first airportd started before the interface existed)"
/bin/launchctl kickstart -k system/com.apple.airportd > /dev/null 2>&1
rc=$?
sleep 10
say "kickstart -k returned $rc; airportd now:$(state)"
[ "$rc" -eq 0 ] || exit "$rc"
boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid) || exit 1
[ -n "$boot" ] || exit 1
receipt=$(/usr/bin/mktemp /var/run/piano-airportd-kick.XXXXXXXX) || exit 1
if ! printf '%s\n' "$boot" > "$receipt" || ! /bin/mv -f "$receipt" /var/run/piano-airportd-kick; then
  /bin/rm -f "$receipt"
  exit 1
fi
say "PianoWifiAirportdKickV1 complete"
