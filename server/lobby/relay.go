package lobby

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"net"
	"net/netip"
	"sync"
	"time"
)

// A relay is one UDP port that forwards between a client and a host once both have bound to it
// with their tokens. It sees only the game's encrypted datagrams.
type relay struct {
	s      *Server
	key    relayKey
	port   int
	conn   *net.UDPConn
	tokens [2]uint64 // client, host
	mu     sync.Mutex
	sides  [2]netip.AddrPort
	budget bucket
	msgs   [2][]byte // the Relay messages, re-sent on a repeated request
	hostAt netip.AddrPort
}

func (s *Server) relayFor(from netip.AddrPort, id uint32, nonce uint64, now time.Time) {
	key := relayKey{from, nonce}
	if r := s.relays[key]; r != nil {
		s.send(from, r.msgs[0])
		s.send(r.hostAt, r.msgs[1])
		return
	}
	h := s.byID[id]
	refuse := func(status uint8) {
		w := newWriter(MsgRelay)
		w.u64(nonce)
		w.u8(status)
		w.u16(0)
		w.u64(0)
		w.addr(from)
		s.send(from, w.b)
	}
	if h == nil {
		refuse(StatusUnknownHost)
		return
	}
	if len(s.ports) == 0 || len(s.relays) >= s.cfg.MaxRelays {
		refuse(StatusNoRelay)
		return
	}
	var r *relay
	for i, p := range s.ports {
		conn, err := net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(s.ip, uint16(p))))
		if err != nil {
			continue
		}
		s.ports = append(s.ports[:i:i], s.ports[i+1:]...)
		r = &relay{s: s, key: key, port: p, conn: conn, hostAt: h.public}
		break
	}
	if r == nil {
		refuse(StatusNoRelay)
		return
	}
	var b [16]byte
	rand.Read(b[:])
	r.tokens[0], r.tokens[1] = binary.LittleEndian.Uint64(b[:8]), binary.LittleEndian.Uint64(b[8:])
	for side := 0; side < 2; side++ {
		w := newWriter(MsgRelay)
		w.u64(nonce)
		w.u8(StatusOK)
		w.u16(uint16(r.port))
		w.u64(r.tokens[side])
		w.addr(from)
		r.msgs[side] = w.b
	}
	s.relays[key] = r
	s.cfg.Logf("lobby: relay on port %d between %v and %q at %v", r.port, from, h.info.ServerName, h.public)
	s.send(from, r.msgs[0])
	s.send(h.public, r.msgs[1])
	go r.run()
}

func (r *relay) run() {
	defer func() {
		r.conn.Close()
		r.s.mu.Lock()
		delete(r.s.relays, r.key)
		r.s.ports = append(r.s.ports, r.port)
		r.s.mu.Unlock()
		r.s.cfg.Logf("lobby: relay on port %d closed", r.port)
	}()
	buf := make([]byte, 2048)
	for {
		r.conn.SetReadDeadline(time.Now().Add(r.s.cfg.RelayIdle))
		n, from, err := r.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() || errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
		d := buf[:n]
		if t, rd, ok := parse(d); ok && t == MsgBind {
			tok := rd.u64()
			for side := 0; side < 2; side++ {
				if rd.err == nil && tok == r.tokens[side] {
					r.mu.Lock()
					r.sides[side] = from
					r.mu.Unlock()
					w := newWriter(MsgBound)
					w.u64(tok)
					r.conn.WriteToUDPAddrPort(w.b, from)
				}
			}
			continue
		}
		r.mu.Lock()
		var to netip.AddrPort
		switch from {
		case r.sides[0]:
			to = r.sides[1]
		case r.sides[1]:
			to = r.sides[0]
		}
		ok := to.IsValid() && r.budget.take(time.Now(), float64(r.s.cfg.RelayRate), float64(r.s.cfg.RelayRate), float64(n))
		r.mu.Unlock()
		if ok {
			r.conn.WriteToUDPAddrPort(d, to)
		}
	}
}
