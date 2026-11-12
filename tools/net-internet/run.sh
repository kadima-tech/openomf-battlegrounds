#!/bin/bash
# A network match through the lobby, all on this machine: the lobby server (server/, built with
# Go), a headless host that lists its session there, and a headless client that finds it in the
# list and joins through the lobby's introduction (a hole punch) or its relay. Afterwards
# compare.py sets the two machines' views of each fighter side by side and wirescan.py checks that
# no game datagram either side received holds readable game data.
#
#   tools/net-internet/run.sh <Data dir> [--ping ms] [--loss fraction] [--nat cone|symmetric]
#       [--relay] [--frames N] [--port N] [--lobby-port N] [--out dir]
#
# --nat emulates NAT filtering on both game sockets (OPENBG_NAT): cone takes only datagrams from
# addresses the socket sent to (the punch is needed), symmetric only the lobby's and its relays'
# (the punch fails and the relay carries the match). --relay skips the punch (OPENBG_FORCE_RELAY).
# --loss drops that share of what each game socket receives (OPENBG_NET_LOSS); --ping is the
# round trip the delay simulator adds. LOBBY=<binary> uses a lobby already built.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
data=${1:?usage: run.sh <Data dir> [--ping ms] [--loss f] [--nat cone|symmetric] [--relay] [--frames N] [--port N] [--lobby-port N] [--out dir]}
shift
ping=0 loss=0 nat="" relay=0 frames=4000 port=20040 lport=27940 out=$(mktemp -d)
bin=${OPENBG:-$repo/engine/build/openbg}
while [ $# -gt 0 ]; do
    case $1 in
        --ping) ping=$2; shift ;;
        --loss) loss=$2; shift ;;
        --nat) nat=$2; shift ;;
        --relay) relay=1 ;;
        --frames) frames=$2; shift ;;
        --port) port=$2; shift ;;
        --lobby-port) lport=$2; shift ;;
        --out) out=$2; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
mkdir -p "$out"
lobby=${LOBBY:-}
if [ -z "$lobby" ]; then
    lobby="$out/openbg-lobby"
    (cd "$repo/server" && go build -o "$lobby" ./cmd/openbg-lobby)
fi
trap 'kill $(jobs -p) 2>/dev/null || true' EXIT
"$lobby" -listen "127.0.0.1:$lport" -relay-ports "$((lport + 1))-$((lport + 8))" > "$out/lobby.log" 2>&1 &
half=$((ping / 2))
set_common="Network\\Winsock\\Delay Packet Time=$half"
cond=(OPENBG_NET_LOSS=$loss)
[ -n "$nat" ] && cond+=(OPENBG_NAT=$nat)
[ "$relay" = 1 ] && cond+=(OPENBG_FORCE_RELAY=1)
rm -f "$out/host.wire" "$out/client.wire"
HOME=$(mktemp -d) OPENBG_ALL_AI=1 OPENBG_NET_TRACE="$out/host.trace" OPENBG_NET_WIRE="$out/host.wire" \
    OPENBG_SET="Network\\Player Name=Host;Network\\Server Name=net-internet test;$set_common" \
    env "${cond[@]}" ${HOST_ENV:-} "$bin" "$data" --host "Last Man Standing" --choose "The Desert,Elizabeth,Katana,One Win" \
    --headless "$frames" --port "$port" --lobby "127.0.0.1:$lport" > "$out/host.log" 2>&1 &
host=$!
sleep 0.2
HOME=$(mktemp -d) OPENBG_JOIN_RETRY=1 OPENBG_ALL_AI=1 OPENBG_NET_TRACE="$out/client.trace" OPENBG_NET_WIRE="$out/client.wire" \
    OPENBG_SET="Network\\Player Name=Guest;$set_common" \
    env "${cond[@]}" ${CLIENT_ENV:-} "$bin" "$data" --join "lobby:net-internet test" --lobby "127.0.0.1:$lport" \
    --choose "Lucas,Jaguar" --headless "$frames" > "$out/client.log" 2>&1 &
client=$!
wait $host || true
wait $client || true
echo "== $out (round trip +$ping ms, loss $loss, nat ${nat:-none}$([ "$relay" = 1 ] && echo ", relay forced"))"
grep -h "lobby:" "$out/lobby.log" | sed "s/^/lobby: /" || true
for side in host client; do
    grep -h "net: \(Guest\|status\|refused\|.*left\|listed\|listing\|joining\|connecting\|connected\|relay\|no direct\|Unable\)\|net summary\|Connection Lost" "$out/$side.log" | sed "s/^/$side: /" | grep -v "reports\|status Await" || true
    grep -h "\[screen\] .*\(Wins\|FIGHT\)" "$out/$side.log" | sed "s/^/$side: /" | tail -3 || true
done
python3 "$repo/tools/net-lan/compare.py" "$out/host.trace" "$out/client.trace"
python3 "$here/wirescan.py" "$out/host.wire" "$out/client.wire" Host Guest "net-internet test" Katana Jaguar Elizabeth Lucas "Last Man Standing" OMFBG-NET
