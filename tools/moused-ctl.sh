#!/usr/bin/env bash
# Control surface for the moused waybar module.
#
#   moused-ctl toggle   start or stop the daemon, then refresh the bar
#   moused-ctl show     print full status and wait for a keypress
#   moused-ctl refresh  just refresh the bar
#
# The waybar module is wired to `toggle` on left click and `show` on right.

set -uo pipefail

SERVICE=moused
SIGNAL=9 # must match "signal" in the waybar module
STATUS="$(dirname "$(readlink -f "$0")")/moused-status"
[ -x "$STATUS" ] || STATUS="$HOME/.local/bin/moused-status"

refresh() {
	# Waybar reruns a custom module's exec when it receives RTMIN+<signal>.
	# Without this the bar would show stale state until the next interval.
	pkill -RTMIN+$SIGNAL waybar 2>/dev/null || true
}

case "${1:-show}" in
toggle)
	if systemctl --user is-active --quiet "$SERVICE"; then
		systemctl --user stop "$SERVICE"
		notify-send -a moused "moused stopped" "Input goes straight to the compositor." 2>/dev/null || true
	else
		systemctl --user start "$SERVICE"
		notify-send -a moused "moused started" "Input is routed through the virtual devices." 2>/dev/null || true
	fi
	# Give the daemon a moment to create or tear down its virtual devices,
	# otherwise the refresh reads the state we just left.
	sleep 1
	refresh
	;;
show)
	"$STATUS" --plain
	echo
	systemctl --user status "$SERVICE" --no-pager --lines=10 2>/dev/null | head -20
	echo
	read -rsn1 -p "press any key to close..."
	;;
refresh)
	refresh
	;;
*)
	echo "usage: $(basename "$0") {toggle|show|refresh}" >&2
	exit 1
	;;
esac
