package lobby

import (
	"bytes"
	"context"
	"net"
	"net/netip"
	"testing"
	"time"
)

func start(t *testing.T, mod func(*Config)) *Server {
	t.Helper()
	c := DefaultConfig()
	c.Listen = "127.0.0.1:0"
	c.RelayFirst, c.RelayLast = 0, 0
	c.Logf = t.Logf
	if mod != nil {
		mod(&c)
	}
	s, err := New(c)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.Run(ctx); close(done) }()
	t.Cleanup(func() { cancel(); <-done })
	return s
}

type peer struct {
	t    *testing.T
	conn *net.UDPConn
}

func newPeer(t *testing.T) *peer {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &peer{t, conn}
}

func (p *peer) addr() netip.AddrPort { return p.conn.LocalAddr().(*net.UDPAddr).AddrPort() }

func (p *peer) send(to netip.AddrPort, b []byte) {
	if _, err := p.conn.WriteToUDPAddrPort(b, to); err != nil {
		p.t.Fatal(err)
	}
}

// recv waits for a datagram of type `want` (0: any), skipping others.
func (p *peer) recv(want MsgType) ([]byte, netip.AddrPort) {
	p.t.Helper()
	buf := make([]byte, 2048)
	deadline := time.Now().Add(2 * time.Second)
	for {
		p.conn.SetReadDeadline(deadline)
		n, from, err := p.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			p.t.Fatalf("waiting for message %d: %v", want, err)
		}
		if want == 0 || Type(buf[:n]) == want {
			return append([]byte(nil), buf[:n]...), from
		}
	}
}

func register(t *testing.T, h *peer, lobby netip.AddrPort, name string) uint32 {
	info := HostInfo{Protocol: 3, Version: "dev", ServerName: name, SessionName: "Last Man Standing", Players: 1, MaxPlayers: 16,
		Local: h.addr(), Identity: [32]byte{1, 2, 3}}
	h.send(lobby, Register(0, &info))
	d, _ := h.recv(MsgChallenge)
	_, r, _ := parse(d)
	cookie := r.u64()
	h.send(lobby, Register(cookie, &info))
	d, _ = h.recv(MsgRegistered)
	_, r, _ = parse(d)
	id := r.u32()
	r.u16()
	if pub := r.addr(); pub != h.addr() {
		t.Fatalf("public address %v, want %v", pub, h.addr())
	}
	return id
}

func TestRegisterListConnect(t *testing.T) {
	s := start(t, nil)
	host, client := newPeer(t), newPeer(t)
	id := register(t, host, s.Addr(), "Test Server")

	client.send(s.Addr(), List(7, 3, 0)[:20])
	client.send(s.Addr(), List(7, 3, 0))
	d, _ := client.recv(MsgListReply)
	page, ok := ParseListReply(d)
	if !ok || page.Seq != 7 || page.Total != 1 || len(page.Entries) != 1 {
		t.Fatalf("list %+v ok %v", page, ok)
	}
	e := page.Entries[0]
	if e.ID != id || e.Public != host.addr() || e.Info.ServerName != "Test Server" || e.Info.Identity[2] != 3 {
		t.Fatalf("entry %+v", e)
	}
	client.send(s.Addr(), List(8, 2, 0))
	d, _ = client.recv(MsgListReply)
	if page, _ = ParseListReply(d); page.Total != 0 {
		t.Fatalf("other protocol listed %+v", page)
	}

	client.send(s.Addr(), Connect(id, 99, client.addr()))
	d, _ = client.recv(MsgIntro)
	in, ok := ParseIntro(d, MsgIntro)
	if !ok || in.Status != StatusOK || in.Nonce != 99 || in.Public != host.addr() || in.Yours != client.addr() {
		t.Fatalf("intro %+v", in)
	}
	d, _ = host.recv(MsgPeer)
	pr, ok := ParseIntro(d, MsgPeer)
	if !ok || pr.Nonce != 99 || pr.Public != client.addr() || pr.Local != client.addr() {
		t.Fatalf("peer %+v", pr)
	}
	client.send(s.Addr(), Connect(id+1, 5, client.addr()))
	d, _ = client.recv(MsgIntro)
	if in, _ = ParseIntro(d, MsgIntro); in.Status != StatusUnknownHost {
		t.Fatalf("unknown host intro %+v", in)
	}
}

func TestSpoofedRegisterNeedsCookie(t *testing.T) {
	s := start(t, nil)
	h := newPeer(t)
	info := HostInfo{Protocol: 3, ServerName: "x"}
	h.send(s.Addr(), Register(12345, &info))
	h.recv(MsgChallenge)
	s.mu.Lock()
	n := len(s.hosts)
	s.mu.Unlock()
	if n != 0 {
		t.Fatal("listed without a valid cookie")
	}
}

func TestHostTimeoutAndUnregister(t *testing.T) {
	s := start(t, func(c *Config) { c.HostTimeout = 1500 * time.Millisecond })
	a, b := newPeer(t), newPeer(t)
	register(t, a, s.Addr(), "A")
	idB := register(t, b, s.Addr(), "B")
	b.send(s.Addr(), Unregister(idB, s.cookie(b.addr(), time.Now().UnixNano()/int64(cookieEpoch))))
	time.Sleep(200 * time.Millisecond)
	s.mu.Lock()
	n := len(s.hosts)
	s.mu.Unlock()
	if n != 1 {
		t.Fatalf("%d hosts after unregister", n)
	}
	time.Sleep(3 * time.Second)
	s.mu.Lock()
	n = len(s.hosts)
	s.mu.Unlock()
	if n != 0 {
		t.Fatalf("%d hosts after timeout", n)
	}
}

func TestListPages(t *testing.T) {
	s := start(t, nil)
	var hosts []*peer
	for i := 0; i < 14; i++ {
		h := newPeer(t)
		register(t, h, s.Addr(), "A server with a fairly long name to fill the page up "+string(rune('a'+i)))
		hosts = append(hosts, h)
	}
	c := newPeer(t)
	seen := 0
	for first := 0; ; {
		c.send(s.Addr(), List(1, 3, first))
		d, _ := c.recv(MsgListReply)
		if len(d) > maxPage+64 {
			t.Fatalf("page of %d bytes", len(d))
		}
		page, ok := ParseListReply(d)
		if !ok || page.First != first || page.Total != 14 {
			t.Fatalf("page %+v", page)
		}
		seen += len(page.Entries)
		first += len(page.Entries)
		if first >= page.Total || len(page.Entries) == 0 {
			break
		}
	}
	if seen != 14 {
		t.Fatalf("saw %d entries", seen)
	}
}

func TestRelay(t *testing.T) {
	s := start(t, func(c *Config) { c.RelayFirst, c.RelayLast = 27981, 27989 })
	host, client := newPeer(t), newPeer(t)
	id := register(t, host, s.Addr(), "Relay Test")
	client.send(s.Addr(), RelayRequest(id, 42))
	d, _ := client.recv(MsgRelay)
	rc, ok := ParseRelay(d)
	if !ok || rc.Status != StatusOK || rc.Nonce != 42 || rc.Port == 0 {
		t.Fatalf("client relay %+v", rc)
	}
	d, _ = host.recv(MsgRelay)
	rh, ok := ParseRelay(d)
	if !ok || rh.Port != rc.Port || rh.Token == rc.Token || rh.Client != client.addr() {
		t.Fatalf("host relay %+v", rh)
	}
	at := netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), rc.Port)
	client.send(at, Bind(rc.Token))
	client.recv(MsgBound)
	host.send(at, Bind(rh.Token))
	host.recv(MsgBound)
	client.send(at, []byte("encrypted game bytes"))
	got, from := host.recv(0)
	if !bytes.Equal(got, []byte("encrypted game bytes")) || from != at {
		t.Fatalf("host got %q from %v", got, from)
	}
	host.send(at, []byte("reply"))
	if got, _ = client.recv(0); string(got) != "reply" {
		t.Fatalf("client got %q", got)
	}
	stranger := newPeer(t)
	stranger.send(at, []byte("inject"))
	stranger.send(at, Bind(rc.Token+1))
	host.send(at, []byte("after"))
	if got, _ = client.recv(0); string(got) != "after" {
		t.Fatalf("client got %q", got)
	}
	// The same request again is answered with the same relay.
	client.send(s.Addr(), RelayRequest(id, 42))
	d, _ = client.recv(MsgRelay)
	if again, _ := ParseRelay(d); again.Port != rc.Port || again.Token != rc.Token {
		t.Fatalf("repeated request got %+v", again)
	}
}

func TestRelayOff(t *testing.T) {
	s := start(t, nil)
	host, client := newPeer(t), newPeer(t)
	id := register(t, host, s.Addr(), "No Relay")
	client.send(s.Addr(), RelayRequest(id, 1))
	d, _ := client.recv(MsgRelay)
	if r, _ := ParseRelay(d); r.Status != StatusNoRelay {
		t.Fatalf("relay %+v", r)
	}
}
