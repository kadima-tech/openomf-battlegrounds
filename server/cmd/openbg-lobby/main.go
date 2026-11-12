// openbg-lobby: the lobby server for OpenBG's internet play. Hosts list their sessions here, players
// find them, and the lobby introduces the two for a UDP hole punch, or relays their (encrypted)
// traffic when the punch fails. Every flag can also be set with an environment variable.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/kadima-tech/openomf-battlegrounds/server/lobby"
)

func env(name, def string) string {
	if v, ok := os.LookupEnv(name); ok {
		return v
	}
	return def
}

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func envDur(name string, def time.Duration) time.Duration {
	if v, err := time.ParseDuration(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func main() {
	c := lobby.DefaultConfig()
	relayPorts := fmt.Sprintf("%d-%d", c.RelayFirst, c.RelayLast)
	flag.StringVar(&c.Listen, "listen", env("OPENBG_LOBBY_LISTEN", c.Listen), "UDP address to listen on (OPENBG_LOBBY_LISTEN)")
	flag.StringVar(&relayPorts, "relay-ports", env("OPENBG_LOBBY_RELAY_PORTS", relayPorts), "UDP port range for relays, \"first-last\", or \"off\" (OPENBG_LOBBY_RELAY_PORTS)")
	flag.IntVar(&c.RelayRate, "relay-rate", envInt("OPENBG_LOBBY_RELAY_RATE", c.RelayRate), "bytes per second one relay forwards (OPENBG_LOBBY_RELAY_RATE)")
	flag.IntVar(&c.MaxRelays, "max-relays", envInt("OPENBG_LOBBY_MAX_RELAYS", c.MaxRelays), "relays open at once (OPENBG_LOBBY_MAX_RELAYS)")
	flag.IntVar(&c.MaxHosts, "max-hosts", envInt("OPENBG_LOBBY_MAX_HOSTS", c.MaxHosts), "sessions listed at once (OPENBG_LOBBY_MAX_HOSTS)")
	flag.IntVar(&c.MaxHostsPerIP, "max-hosts-per-ip", envInt("OPENBG_LOBBY_MAX_HOSTS_PER_IP", c.MaxHostsPerIP), "sessions one address may list (OPENBG_LOBBY_MAX_HOSTS_PER_IP)")
	flag.DurationVar(&c.HostTimeout, "host-timeout", envDur("OPENBG_LOBBY_HOST_TIMEOUT", c.HostTimeout), "a silent host leaves the list after this (OPENBG_LOBBY_HOST_TIMEOUT)")
	flag.DurationVar(&c.RelayIdle, "relay-idle", envDur("OPENBG_LOBBY_RELAY_IDLE", c.RelayIdle), "an idle relay closes after this (OPENBG_LOBBY_RELAY_IDLE)")
	flag.StringVar(&c.HTTP, "http", env("OPENBG_LOBBY_HTTP", ""), "optional TCP address for /healthz and /servers (OPENBG_LOBBY_HTTP)")
	flag.Parse()
	if relayPorts == "off" || relayPorts == "" {
		c.RelayFirst, c.RelayLast = 0, 0
	} else if a, b, ok := strings.Cut(relayPorts, "-"); ok {
		first, err1 := strconv.Atoi(a)
		last, err2 := strconv.Atoi(b)
		if err1 != nil || err2 != nil || first <= 0 || last < first || last > 65535 {
			log.Fatalf("bad -relay-ports %q", relayPorts)
		}
		c.RelayFirst, c.RelayLast = first, last
	} else {
		log.Fatalf("bad -relay-ports %q", relayPorts)
	}
	s, err := lobby.New(c)
	if err != nil {
		log.Fatal(err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := s.Run(ctx); err != nil {
		log.Fatal(err)
	}
}
