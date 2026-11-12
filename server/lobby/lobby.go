package lobby

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/netip"
	"sort"
	"sync"
	"time"
)

type Config struct {
	Listen        string        // UDP address for listings, introductions and relay requests
	RelayFirst    int           // relay ports, inclusive; 0 turns the relay off
	RelayLast     int           //
	RelayRate     int           // bytes per second a relay forwards, both directions together
	RelayIdle     time.Duration // a relay with no traffic this long is closed
	MaxRelays     int
	MaxHosts      int
	MaxHostsPerIP int
	HostTimeout   time.Duration // a host silent this long leaves the list
	Heartbeat     time.Duration // what hosts are told to send at
	HTTP          string        // optional: /healthz and /servers
	Logf          func(format string, args ...any)
}

func DefaultConfig() Config {
	return Config{
		Listen:        ":27900",
		RelayFirst:    27901,
		RelayLast:     27964,
		RelayRate:     96 * 1024,
		RelayIdle:     30 * time.Second,
		MaxRelays:     64,
		MaxHosts:      1000,
		MaxHostsPerIP: 16,
		HostTimeout:   20 * time.Second,
		Heartbeat:     5 * time.Second,
		Logf:          log.Printf,
	}
}

const (
	minListRequest = 128  // a list request is padded so its reply amplifies little
	maxPage        = 1100 // bytes of entries in one reply
	cookieEpoch    = 30 * time.Second
)

type host struct {
	id     uint32
	public netip.AddrPort
	info   HostInfo
	seen   time.Time
	peers  bucket // introductions sent to it
}

type Server struct {
	cfg    Config
	conn   *net.UDPConn
	ip     netip.Addr // the address relays bind to
	secret [32]byte

	mu      sync.Mutex
	hosts   map[netip.AddrPort]*host
	byID    map[uint32]*host
	relays  map[relayKey]*relay
	ports   []int
	limits  map[netip.Addr]*bucket
	started time.Time
}

type relayKey struct {
	client netip.AddrPort
	nonce  uint64
}

// bucket is a token bucket: rate per second, burst at most.
type bucket struct {
	tokens float64
	last   time.Time
}

func (b *bucket) take(now time.Time, rate, burst, n float64) bool {
	if b.last.IsZero() {
		b.tokens = burst
	} else {
		b.tokens = min(burst, b.tokens+now.Sub(b.last).Seconds()*rate)
	}
	b.last = now
	if b.tokens < n {
		return false
	}
	b.tokens -= n
	return true
}

func New(cfg Config) (*Server, error) {
	if cfg.Logf == nil {
		cfg.Logf = func(string, ...any) {}
	}
	ua, err := net.ResolveUDPAddr("udp4", cfg.Listen)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp4", ua)
	if err != nil {
		return nil, err
	}
	s := &Server{cfg: cfg, conn: conn, hosts: map[netip.AddrPort]*host{}, byID: map[uint32]*host{},
		relays: map[relayKey]*relay{}, limits: map[netip.Addr]*bucket{}, started: time.Now()}
	if _, err := rand.Read(s.secret[:]); err != nil {
		conn.Close()
		return nil, err
	}
	s.ip = conn.LocalAddr().(*net.UDPAddr).AddrPort().Addr()
	if cfg.RelayFirst > 0 && cfg.RelayLast >= cfg.RelayFirst {
		for p := cfg.RelayFirst; p <= cfg.RelayLast; p++ {
			s.ports = append(s.ports, p)
		}
	}
	return s, nil
}

// Addr is the UDP address the lobby listens on.
func (s *Server) Addr() netip.AddrPort { return s.conn.LocalAddr().(*net.UDPAddr).AddrPort() }

// Run serves until the context ends.
func (s *Server) Run(ctx context.Context) error {
	var httpSrv *http.Server
	if s.cfg.HTTP != "" {
		mux := http.NewServeMux()
		mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) { fmt.Fprintln(w, "ok") })
		mux.HandleFunc("/servers", s.serveList)
		httpSrv = &http.Server{Addr: s.cfg.HTTP, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
		go func() {
			if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
				s.cfg.Logf("lobby: http: %v", err)
			}
		}()
	}
	go func() {
		<-ctx.Done()
		s.conn.Close()
		if httpSrv != nil {
			httpSrv.Close()
		}
		s.mu.Lock()
		for _, r := range s.relays {
			r.conn.Close()
		}
		s.mu.Unlock()
	}()
	go s.janitor(ctx)
	s.cfg.Logf("lobby: listening on %v, relay ports %d", s.Addr(), len(s.ports))
	buf := make([]byte, 2048)
	for {
		n, from, err := s.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() {
				continue
			}
			return err
		}
		s.handle(buf[:n], netip.AddrPortFrom(from.Addr().Unmap(), from.Port()), time.Now())
	}
}

func (s *Server) janitor(ctx context.Context) {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-t.C:
			s.mu.Lock()
			for a, h := range s.hosts {
				if now.Sub(h.seen) > s.cfg.HostTimeout {
					s.cfg.Logf("lobby: %q at %v timed out", h.info.ServerName, a)
					delete(s.hosts, a)
					delete(s.byID, h.id)
				}
			}
			for ip, b := range s.limits {
				if now.Sub(b.last) > time.Minute {
					delete(s.limits, ip)
				}
			}
			s.mu.Unlock()
		}
	}
}

func (s *Server) send(to netip.AddrPort, b []byte) {
	s.conn.WriteToUDPAddrPort(b, to)
}

func (s *Server) cookie(a netip.AddrPort, epoch int64) uint64 {
	m := hmac.New(sha256.New, s.secret[:])
	b, _ := a.MarshalBinary()
	m.Write(b)
	binary.Write(m, binary.LittleEndian, epoch)
	return binary.LittleEndian.Uint64(m.Sum(nil))
}

func (s *Server) cookieOK(a netip.AddrPort, c uint64, now time.Time) bool {
	e := now.UnixNano() / int64(cookieEpoch)
	return c != 0 && (c == s.cookie(a, e) || c == s.cookie(a, e-1))
}

func (s *Server) handle(d []byte, from netip.AddrPort, now time.Time) {
	t, r, ok := parse(d)
	if !ok || !from.Addr().Is4() {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	lim := s.limits[from.Addr()]
	if lim == nil {
		lim = &bucket{}
		s.limits[from.Addr()] = lim
	}
	if !lim.take(now, 40, 80, 1) {
		return
	}
	switch t {
	case MsgRegister:
		c := r.u64()
		info := r.info()
		if r.err != nil {
			return
		}
		if !s.cookieOK(from, c, now) {
			w := newWriter(MsgChallenge)
			w.u64(s.cookie(from, now.UnixNano()/int64(cookieEpoch)))
			s.send(from, w.b)
			return
		}
		h := s.hosts[from]
		if h == nil {
			if len(s.hosts) >= s.cfg.MaxHosts || s.hostsFrom(from.Addr()) >= s.cfg.MaxHostsPerIP {
				s.refuse(from, "The lobby is full.")
				return
			}
			h = &host{id: s.newID(), public: from}
			s.hosts[from] = h
			s.byID[h.id] = h
			s.cfg.Logf("lobby: %q listed at %v as %d", info.ServerName, from, h.id)
		}
		h.info = info
		h.seen = now
		w := newWriter(MsgRegistered)
		w.u32(h.id)
		w.u16(uint16(s.cfg.Heartbeat / time.Millisecond))
		w.addr(from)
		s.send(from, w.b)
	case MsgUnregister:
		id, c := r.u32(), r.u64()
		if h := s.hosts[from]; r.err == nil && h != nil && h.id == id && s.cookieOK(from, c, now) {
			s.cfg.Logf("lobby: %q at %v left", h.info.ServerName, from)
			delete(s.hosts, from)
			delete(s.byID, id)
		}
	case MsgList:
		seq, protocol, first := r.u32(), r.u16(), int(r.u16())
		if r.err != nil || len(d) < minListRequest {
			return
		}
		s.sendList(from, seq, protocol, first)
	case MsgConnect:
		id, nonce, local := r.u32(), r.u64(), r.addr()
		if r.err != nil {
			return
		}
		h := s.byID[id]
		w := newWriter(MsgIntro)
		w.u64(nonce)
		if h == nil {
			w.u8(StatusUnknownHost)
			w.addr(netip.AddrPort{})
			w.addr(netip.AddrPort{})
			w.addr(from)
			s.send(from, w.b)
			return
		}
		w.u8(StatusOK)
		w.addr(h.public)
		w.addr(h.info.Local)
		w.addr(from)
		s.send(from, w.b)
		if h.peers.take(now, 10, 20, 1) {
			p := newWriter(MsgPeer)
			p.u64(nonce)
			p.addr(from)
			p.addr(local)
			s.send(h.public, p.b)
		}
	case MsgRelayRequest:
		id, nonce := r.u32(), r.u64()
		if r.err != nil {
			return
		}
		s.relayFor(from, id, nonce, now)
	}
}

func (s *Server) refuse(to netip.AddrPort, why string) {
	w := newWriter(MsgError)
	w.u8(1)
	w.str(why)
	s.send(to, w.b)
}

func (s *Server) hostsFrom(ip netip.Addr) int {
	n := 0
	for a := range s.hosts {
		if a.Addr() == ip {
			n++
		}
	}
	return n
}

func (s *Server) newID() uint32 {
	var b [4]byte
	for {
		rand.Read(b[:])
		id := binary.LittleEndian.Uint32(b[:])
		if id != 0 && s.byID[id] == nil {
			return id
		}
	}
}

func (s *Server) sorted(protocol uint16) []*host {
	var hs []*host
	for _, h := range s.hosts {
		if protocol == 0 || h.info.Protocol == protocol {
			hs = append(hs, h)
		}
	}
	sort.Slice(hs, func(i, j int) bool { return hs[i].id < hs[j].id })
	return hs
}

func (s *Server) sendList(to netip.AddrPort, seq uint32, protocol uint16, first int) {
	hs := s.sorted(protocol)
	w := newWriter(MsgListReply)
	w.u32(seq)
	w.u16(uint16(min(len(hs), 0xffff)))
	w.u16(uint16(min(first, 0xffff)))
	countAt := len(w.b)
	w.u8(0)
	n := 0
	for i := first; i < len(hs) && n < 255; i++ {
		e := newWriter(0)
		e.b = e.b[:0]
		e.u32(hs[i].id)
		e.addr(hs[i].public)
		e.info(&hs[i].info)
		if n > 0 && len(w.b)+len(e.b) > maxPage {
			break
		}
		w.b = append(w.b, e.b...)
		n++
	}
	w.b[countAt] = byte(n)
	s.send(to, w.b)
}

type listed struct {
	Server   string `json:"server"`
	Session  string `json:"session"`
	Version  string `json:"version"`
	Players  int    `json:"players"`
	Max      int    `json:"max_players"`
	Password bool   `json:"password"`
	Playing  bool   `json:"playing"`
}

func (s *Server) serveList(w http.ResponseWriter, _ *http.Request) {
	s.mu.Lock()
	out := []listed{}
	for _, h := range s.sorted(0) {
		out = append(out, listed{h.info.ServerName, h.info.SessionName, h.info.Version, int(h.info.Players), int(h.info.MaxPlayers),
			h.info.Flags&FlagPassword != 0, h.info.Flags&FlagPlaying != 0})
	}
	s.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(out)
}
