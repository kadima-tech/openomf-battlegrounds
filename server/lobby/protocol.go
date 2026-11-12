// Package lobby is OpenBG's lobby server: the session list the original kept on GameSpy's master
// server, UDP hole-punch introductions, and a relay for players whose NATs cannot be punched.
// docs/NETWORK_PLAN.md section 7.8 describes the wire format; engine/src/net/lobby.cpp is the
// game's side of it.
package lobby

import (
	"encoding/binary"
	"errors"
	"net/netip"
)

// Every lobby datagram starts with the magic and the version.
var Magic = [4]byte{'O', 'B', 'G', 'L'}

const Version = 1

type MsgType uint8

const (
	MsgRegister     MsgType = 1  // host -> lobby, from its game socket: listing and heartbeat
	MsgChallenge    MsgType = 2  // lobby -> host: echo this cookie
	MsgRegistered   MsgType = 3  // lobby -> host: its id, heartbeat interval and public address
	MsgUnregister   MsgType = 4  // host -> lobby
	MsgList         MsgType = 5  // client -> lobby
	MsgListReply    MsgType = 6  // lobby -> client, one page of entries
	MsgConnect      MsgType = 7  // client -> lobby, from its game socket: introduce me to a host
	MsgIntro        MsgType = 8  // lobby -> client: the host's addresses
	MsgPeer         MsgType = 9  // lobby -> host: a client's addresses, punch towards them
	MsgPunch        MsgType = 10 // peer <-> peer: opens the NAT mappings, otherwise ignored
	MsgRelayRequest MsgType = 11 // client -> lobby: the punch failed
	MsgRelay        MsgType = 12 // lobby -> client and host: a relay port and this side's token
	MsgBind         MsgType = 13 // peer -> relay port: this address is the token's side
	MsgBound        MsgType = 14 // relay port -> peer
	MsgError        MsgType = 15 // lobby -> any: refused, with a reason
)

// Status codes in Intro and Relay.
const (
	StatusOK          = 0
	StatusUnknownHost = 1
	StatusNoRelay     = 2
)

// Flags in a listing, as the game's LAN answer carries them.
const (
	FlagPassword = 1
	FlagPlaying  = 2
)

// HostInfo is what a host lists: the GameSpy fields the original sent, plus the data hash and the
// host's identity key, which the game checks in its encrypted handshake.
type HostInfo struct {
	Protocol    uint16
	Version     string
	DataHash    [16]byte
	ServerName  string
	SessionName string
	Players     uint8
	MaxPlayers  uint8
	Flags       uint8
	Local       netip.AddrPort // the host's own address, for players behind the same NAT
	Identity    [32]byte       // Ed25519 public key
	Instance    uint32
}

type Entry struct {
	ID     uint32
	Public netip.AddrPort
	Info   HostInfo
}

var errShort = errors.New("short datagram")

type writer struct{ b []byte }

func newWriter(t MsgType) *writer {
	w := &writer{b: make([]byte, 0, 256)}
	w.b = append(w.b, Magic[:]...)
	w.b = append(w.b, Version, byte(t))
	return w
}
func (w *writer) u8(v uint8)   { w.b = append(w.b, v) }
func (w *writer) u16(v uint16) { w.b = binary.LittleEndian.AppendUint16(w.b, v) }
func (w *writer) u32(v uint32) { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *writer) u64(v uint64) { w.b = binary.LittleEndian.AppendUint64(w.b, v) }
func (w *writer) bytes(p []byte) {
	w.b = append(w.b, p...)
}
func (w *writer) str(s string) {
	if len(s) > 255 {
		s = s[:255]
	}
	w.u16(uint16(len(s)))
	w.b = append(w.b, s...)
}

// addr writes an IPv4 address (four bytes in network order) and a port; anything else as zeros.
func (w *writer) addr(a netip.AddrPort) {
	ip := a.Addr().Unmap()
	if ip.Is4() {
		b := ip.As4()
		w.b = append(w.b, b[:]...)
		w.u16(a.Port())
		return
	}
	w.b = append(w.b, 0, 0, 0, 0, 0, 0)
}

type reader struct {
	b   []byte
	err error
}

func (r *reader) take(n int) []byte {
	if r.err != nil || len(r.b) < n {
		r.err = errShort
		return make([]byte, n)
	}
	p := r.b[:n]
	r.b = r.b[n:]
	return p
}
func (r *reader) u8() uint8   { return r.take(1)[0] }
func (r *reader) u16() uint16 { return binary.LittleEndian.Uint16(r.take(2)) }
func (r *reader) u32() uint32 { return binary.LittleEndian.Uint32(r.take(4)) }
func (r *reader) u64() uint64 { return binary.LittleEndian.Uint64(r.take(8)) }
func (r *reader) str() string {
	n := int(r.u16())
	return string(r.take(n))
}
func (r *reader) addr() netip.AddrPort {
	b := r.take(4)
	port := r.u16()
	return netip.AddrPortFrom(netip.AddrFrom4([4]byte{b[0], b[1], b[2], b[3]}), port)
}

// parse checks the magic and version and returns the type and the body.
func parse(d []byte) (MsgType, *reader, bool) {
	if len(d) < 6 || [4]byte(d[:4]) != Magic || d[4] != Version {
		return 0, nil, false
	}
	return MsgType(d[5]), &reader{b: d[6:]}, true
}

func (w *writer) info(i *HostInfo) {
	w.u16(i.Protocol)
	w.str(i.Version)
	w.bytes(i.DataHash[:])
	w.str(i.ServerName)
	w.str(i.SessionName)
	w.u8(i.Players)
	w.u8(i.MaxPlayers)
	w.u8(i.Flags)
	w.addr(i.Local)
	w.bytes(i.Identity[:])
	w.u32(i.Instance)
}

func (r *reader) info() HostInfo {
	var i HostInfo
	i.Protocol = r.u16()
	i.Version = r.str()
	copy(i.DataHash[:], r.take(16))
	i.ServerName = r.str()
	i.SessionName = r.str()
	i.Players = r.u8()
	i.MaxPlayers = r.u8()
	i.Flags = r.u8()
	i.Local = r.addr()
	copy(i.Identity[:], r.take(32))
	i.Instance = r.u32()
	return i
}

// The messages a game sends, built here for the tests and for anyone writing another client.

func Register(cookie uint64, i *HostInfo) []byte {
	w := newWriter(MsgRegister)
	w.u64(cookie)
	w.info(i)
	return w.b
}

// List asks for the page of entries from `first` on; the request is padded to its minimum size.
func List(seq uint32, protocol uint16, first int) []byte {
	w := newWriter(MsgList)
	w.u32(seq)
	w.u16(protocol)
	w.u16(uint16(first))
	for len(w.b) < minListRequest {
		w.u8(0)
	}
	return w.b
}

func Connect(host uint32, nonce uint64, local netip.AddrPort) []byte {
	w := newWriter(MsgConnect)
	w.u32(host)
	w.u64(nonce)
	w.addr(local)
	return w.b
}

func RelayRequest(host uint32, nonce uint64) []byte {
	w := newWriter(MsgRelayRequest)
	w.u32(host)
	w.u64(nonce)
	return w.b
}

func Bind(token uint64) []byte {
	w := newWriter(MsgBind)
	w.u64(token)
	return w.b
}

func Unregister(host uint32, cookie uint64) []byte {
	w := newWriter(MsgUnregister)
	w.u32(host)
	w.u64(cookie)
	return w.b
}

// Decoded replies, for the tests.

type ListPage struct {
	Seq     uint32
	Total   int
	First   int
	Entries []Entry
}

func ParseListReply(d []byte) (ListPage, bool) {
	t, r, ok := parse(d)
	if !ok || t != MsgListReply {
		return ListPage{}, false
	}
	var p ListPage
	p.Seq = r.u32()
	p.Total = int(r.u16())
	p.First = int(r.u16())
	n := int(r.u8())
	for i := 0; i < n && r.err == nil; i++ {
		var e Entry
		e.ID = r.u32()
		e.Public = r.addr()
		e.Info = r.info()
		p.Entries = append(p.Entries, e)
	}
	return p, r.err == nil
}

type IntroMsg struct {
	Nonce         uint64
	Status        uint8
	Public, Local netip.AddrPort
	Yours         netip.AddrPort // Intro only: the client's address as the lobby sees it
}

func ParseIntro(d []byte, want MsgType) (IntroMsg, bool) {
	t, r, ok := parse(d)
	if !ok || t != want {
		return IntroMsg{}, false
	}
	var m IntroMsg
	m.Nonce = r.u64()
	if want == MsgIntro {
		m.Status = r.u8()
	}
	m.Public = r.addr()
	m.Local = r.addr()
	if want == MsgIntro {
		m.Yours = r.addr()
	}
	return m, r.err == nil
}

type RelayMsg struct {
	Nonce  uint64
	Status uint8
	Port   uint16
	Token  uint64
	Client netip.AddrPort
}

func ParseRelay(d []byte) (RelayMsg, bool) {
	t, r, ok := parse(d)
	if !ok || t != MsgRelay {
		return RelayMsg{}, false
	}
	var m RelayMsg
	m.Nonce = r.u64()
	m.Status = r.u8()
	m.Port = r.u16()
	m.Token = r.u64()
	m.Client = r.addr()
	return m, r.err == nil
}

// Type returns a datagram's lobby message type, or 0.
func Type(d []byte) MsgType {
	t, _, ok := parse(d)
	if !ok {
		return 0
	}
	return t
}
