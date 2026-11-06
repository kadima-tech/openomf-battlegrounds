#!/bin/bash
# A network match between two openbg processes on this machine, headless, each with its own
# scratch HOME: one hosts Last Man Standing, the other joins over localhost; both pick their pilot
# and robot through the game's own pages (the client's opened remotely by the host), the pilots'
# AI fights, and compare.py sets the two machines' views of each fighter side by side.
#
#   tools/net-lan/run.sh <Data dir> [--ping ms] [--frames N] [--port N] [--out dir] [--wins "One Win"]
#       [--arena "The Desert"] [--host-fighter Elizabeth,Katana] [--client-fighter Lucas,Jaguar]
#
# HOST_ENV / CLIENT_ENV add environment settings to one side (e.g. CLIENT_ENV=OPENBG_DEBUG_NETSTATES=1).
# --ping is the round trip the delay simulator adds (Delay Packet Time, half on each side).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
data=${1:?usage: run.sh <Data dir> [--ping ms] [--frames N] [--port N] [--out dir] [--wins name]}
shift
ping=0 frames=6000 port=20020 out=$(mktemp -d) wins="One Win" arena="The Desert" hostf="Elizabeth,Katana" clientf="Lucas,Jaguar"
bin=${OPENBG:-$repo/engine/build/openbg}
while [ $# -gt 0 ]; do
    case $1 in
        --ping) ping=$2; shift ;;
        --frames) frames=$2; shift ;;
        --port) port=$2; shift ;;
        --out) out=$2; shift ;;
        --wins) wins=$2; shift ;;
        --arena) arena=$2; shift ;;
        --host-fighter) hostf=$2; shift ;;
        --client-fighter) clientf=$2; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
mkdir -p "$out"
half=$((ping / 2))
delay="Network\\Winsock\\Delay Packet Time=$half"
HOME=$(mktemp -d) OPENBG_ALL_AI=1 OPENBG_NET_TRACE="$out/host.trace" \
    OPENBG_SET="Network\\Player Name=Host;Network\\Server Name=net-lan test;$delay" \
    env ${HOST_ENV:-} "$bin" "$data" --host "Last Man Standing" --choose "$arena,$hostf,$wins" --headless "$frames" --port "$port" > "$out/host.log" 2>&1 &
host=$!
HOME=$(mktemp -d) OPENBG_JOIN_RETRY=1 OPENBG_ALL_AI=1 OPENBG_NET_TRACE="$out/client.trace" \
    OPENBG_SET="Network\\Player Name=Guest;$delay" \
    env ${CLIENT_ENV:-} "$bin" "$data" --join "127.0.0.1:$port" --choose "$clientf" --headless "$frames" > "$out/client.log" 2>&1 &
client=$!
wait $host || true
wait $client || true
echo "== $out (round trip +$ping ms)"
for side in host client; do
    grep -h "net: \(Guest\|status\|refused\|.*left\|.*interrupted\)\|net summary\|Unable\|Connection Lost" "$out/$side.log" | sed "s/^/$side: /" | grep -v "reports\|status Await" || true
    grep -h "\[screen\] .*\(Wins\|Round [0-9]\|FIGHT\)" "$out/$side.log" | sed "s/^/$side: /" | tail -4 || true
done
python3 "$here/compare.py" "$out/host.trace" "$out/client.trace"
