#!/bin/bash
# Host Game through the menus, twice, with a second openbg process joining the second session.
#
# The host (a screenshot run driven by MOUSE) presses Host Game, backs out of Choose Session Master
# to the main menu (which must end the session and release the port), presses Host Game again,
# picks Last Man Standing, Choose Cast, Choose Pilot and Choose Creature; the guest joins once the
# second session is up and picks through its remote pages. Both pilots' AI fights. The host then
# leaves through the in-game menu's Main Menu and hosts a third time on the same port.
#
#   tools/net-lan/host-menu.sh <Data dir> [--port N] [--out dir] [--lan]
#
# --lan joins through Join Game's session browser (the LAN list) instead of --join's direct address.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
data=${1:?usage: host-menu.sh <Data dir> [--port N] [--out dir] [--lan]}
shift
port=20030 out=$(mktemp -d) lan=0
bin=${OPENBG:-$repo/engine/build/openbg}
while [ $# -gt 0 ]; do
    case $1 in
        --port) port=$2; shift ;;
        --out) out=$2; shift ;;
        --lan) lan=1 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
mkdir -p "$out"
click() { echo "$1@$2;down@$(($2 + 2));up@$(($2 + 4))"; }
host_game="0.2,0.8" back="0.22,0.917" next="0.77,0.917" lms_row="0.15,0.168" main_menu="0.5,0.732"
mouse="$(click $host_game 100);$(click $back 300);$(click $host_game 450);$(click $lms_row 650);$(click $next 700)"
mouse+=";$(click $next 900);$(click $next 1100);$(click $next 1300);esc@3600;$(click $main_menu 3650);$(click $host_game 3900)"
HOME=$(mktemp -d) OPENBG_ALL_AI=1 OPENBG_DEBUG_UI=1 SHOTS="${SHOTS:-}" MOUSE="$mouse" \
    OPENBG_SET="Network\\Player Name=Host;Network\\Server Name=host-menu test;Network\\Winsock\\Listener Port=$port" \
    "$bin" "$data" --screenshot "$out/host.png" --frames 4100 > "$out/host.log" 2>&1 &
host=$!
# The guest starts once the second session listens.
for _ in $(seq 1 600); do
    [ "$(grep -c "^net: hosting on port $port" "$out/host.log" 2>/dev/null || true)" -ge 2 ] && break
    kill -0 $host 2>/dev/null || break
    sleep 0.2
done
if [ $lan = 1 ]; then
    guest_mouse="$(click 0.2,0.666 100);$(click 0.15,0.178 900);$(click $next 950)"
    HOME=$(mktemp -d) OPENBG_ALL_AI=1 SHOTS="${CLIENT_SHOTS:-}" MOUSE="$guest_mouse" \
        OPENBG_SET="Network\\Player Name=Guest;SinglePlayerPlayed=1;Network\\Winsock\\Listener Port=$port" \
        "$bin" "$data" --choose "Lucas,Jaguar" --screenshot "$out/guest.png" --frames 2800 > "$out/guest.log" 2>&1 &
else
    HOME=$(mktemp -d) OPENBG_ALL_AI=1 OPENBG_JOIN_RETRY=1 OPENBG_SET="Network\\Player Name=Guest" \
        "$bin" "$data" --join "127.0.0.1:$port" --choose "Lucas,Jaguar" --headless 2800 > "$out/guest.log" 2>&1 &
fi
guest=$!
wait $host || true
wait $guest || true
echo "== $out"
grep -h "^net: \(hosting\|stopped\|cannot\|Guest\)" "$out/host.log" | sed "s/^/host: /" | grep -v reports || true
grep -h "^net: \(joined\|Connection\|Unable\)\|READY\|FIGHT" "$out/guest.log" | sed "s/^/guest: /" | head -6 || true
hosted=$(grep -c "^net: hosting on port $port" "$out/host.log" || true)
released=$(grep -c "^net: stopped hosting, port $port released" "$out/host.log" || true)
ok=1
[ "$hosted" -eq 3 ] || { echo "FAIL: hosted $hosted times, expected 3" >&2; ok=0; }
[ "$released" -ge 3 ] || { echo "FAIL: port released $released times, expected 3" >&2; ok=0; }
grep -q "^net: Guest joined" "$out/host.log" || { echo "FAIL: the guest did not join" >&2; ok=0; }
grep -q "UI \"Choose Creature\": chose Jaguar" "$out/guest.log" || { echo "FAIL: the guest did not get its remote pages" >&2; ok=0; }
grep -q "\[screen\] FIGHT\|^slot [0-9]*: Jaguar" "$out/guest.log" || { echo "FAIL: the guest's match did not start" >&2; ok=0; }
[ $ok = 1 ] && echo "PASS"
[ $ok = 1 ]
