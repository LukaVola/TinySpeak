package main

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"net/http"
	"os"
	"os/signal"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"
)

const (
	maxUsers = 256
	maxRooms = 32
	maxVoice = 16
	pollTime = 10 * time.Second
	grace    = 30 * time.Second
	alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
)

type room struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Kind  string `json:"kind"`
	Limit int    `json:"limit,omitempty"`
}

type user struct {
	micState
	ProfileCode string `json:"profile_code,omitempty"`
	ID          string `json:"id"`
	ProfileID   string `json:"profile_id,omitempty"`
	Name        string `json:"name"`
	Color       int    `json:"color"`
	Voice       string `json:"voice,omitempty"`
}

type advert struct {
	Code string `json:"code"`
	Name string `json:"name"`
}

type event struct {
	Type   string `json:"type"`
	Code   string `json:"code"`
	Room   string `json:"room,omitempty"`
	Name   string `json:"name,omitempty"`
	Sender string `json:"sender,omitempty"`
	Text   string `json:"text,omitempty"`
	At     string `json:"at,omitempty"`
}

type client struct {
	user
	key, active, voiceID, textID string
	indicators                   bool
	pendingMic                   map[string]micState
	invited                      string
	seen, lastMessage            time.Time
	pending                      []event
	wake                         chan struct{}
	dirty, closed                bool
	version                      uint64
}

type server struct {
	advert
	owner, secret, ownerCode string
	rooms                    []room
	members                  map[string]*client
	known                    map[string]knownProfile
	waking, archive          bool
}

type request struct {
	Op         string   `json:"op"`
	Profile    string   `json:"profile"`
	Code       string   `json:"code"`
	Codes      []string `json:"codes"`
	Secret     string   `json:"secret"`
	Name       string   `json:"name"`
	Room       string   `json:"room"`
	Kind       string   `json:"kind"`
	Text       string   `json:"text"`
	Target     string   `json:"target"`
	Color      int      `json:"color"`
	Archive    bool     `json:"archive"`
	Limit      int      `json:"limit"`
	Muted      bool     `json:"muted"`
	Speaking   bool     `json:"speaking"`
	Indicators bool     `json:"voice_indicators"`
	Rooms      []room   `json:"rooms"`
}

type state struct {
	advert
	Owner      string         `json:"owner"`
	Rooms      []room         `json:"rooms"`
	Users      []user         `json:"users"`
	Offline    []knownProfile `json:"offline,omitempty"`
	Text       string         `json:"text_room"`
	Indicators bool           `json:"voice_indicators"`
	Voice      string         `json:"voice"`
	Archive    bool           `json:"archive"`
}

type response struct {
	Error        string              `json:"error,omitempty"`
	Key          string              `json:"key,omitempty"`
	User         *user               `json:"user,omitempty"`
	State        *state              `json:"state,omitempty"`
	Events       []event             `json:"events,omitempty"`
	Catalog      []advert            `json:"catalog,omitempty"`
	URL          string              `json:"url,omitempty"`
	Token        string              `json:"token,omitempty"`
	TurnUser     string              `json:"turnUser,omitempty"`
	TurnPassword string              `json:"turnPassword,omitempty"`
	Room         *room               `json:"room,omitempty"`
	Retry        bool                `json:"retry,omitempty"`
	Reset        bool                `json:"reset,omitempty"`
	Version      uint64              `json:"version,omitempty"`
	MicCode      string              `json:"mic_code,omitempty"`
	Mic          map[string]micState `json:"mic,omitempty"`
}

type voiceConfig struct {
	URL, Admin, Key, Secret string
	turnSecret              string
}

type service struct {
	mu        sync.Mutex
	clients   map[string]*client
	servers   map[string]*server
	voice     voiceConfig
	http      *http.Client
	hosted    bool
	rosterDir string
}

func randomString(length int) string {
	result := make([]byte, length)
	for i := range result {
		n, err := rand.Int(rand.Reader, big.NewInt(int64(len(alphabet))))
		if err != nil {
			panic(err)
		}
		result[i] = alphabet[n.Int64()]
	}
	return string(result)
}

func inviteCode(secret string) string { return shortCode("Kanal invite v1:", secret) }

func profileCode(secret string) string {
	if !validID(secret, 40) {
		return ""
	}
	return shortCode("TinySpeak profile code v1:", secret)
}

func shortCode(domain, secret string) string {
	code := make([]byte, 0, 16)
	for counter := 0; len(code) < 16; counter++ {
		digest := sha256.Sum256([]byte(domain + secret + ":" + strconv.Itoa(counter)))
		for _, b := range digest {
			if b < 248 {
				code = append(code, alphabet[b%62])
			}
			if len(code) == 16 {
				break
			}
		}
	}
	return string(code)
}

func profileID(secret string) string {
	if !validID(secret, 40) {
		return ""
	}
	digest := sha256.Sum256([]byte("TinySpeak profile v1:" + secret))
	return hex.EncodeToString(digest[:16])
}

func validID(value string, length int) bool {
	return len(value) == length && strings.IndexFunc(value, func(r rune) bool {
		return !(r >= '0' && r <= '9' || r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z')
	}) < 0
}

func cleanLabel(name string, min, max int) (string, bool) {
	name = strings.TrimSpace(name)
	n := utf8.RuneCountInString(name)
	return name, n >= min && n <= max && strings.IndexFunc(name, func(r rune) bool {
		return unicode.IsControl(r) || unicode.Is(unicode.Cf, r)
	}) < 0
}

func (c *client) notify() {
	select {
	case c.wake <- struct{}{}:
	default:
	}
}

func (c *client) send(e event) {
	if len(c.pending) == 512 {
		c.pending = c.pending[1:]
	}
	c.pending = append(c.pending, e)
	c.notify()
}

func (s *server) changed() {
	for _, c := range s.members {
		c.version++
		c.dirty = true
		c.notify()
	}
}

func (s *service) snapshot(c *client) *state {
	srv := s.servers[c.active]
	if srv == nil {
		return nil
	}
	result := &state{advert: srv.advert, Owner: srv.owner, Voice: c.Voice, Text: c.textID, Indicators: true, Offline: srv.offline(), Archive: srv.archive,
		Rooms: append([]room{}, srv.rooms...), Users: make([]user, 0, len(srv.members))}
	for _, member := range srv.members {
		result.Users = append(result.Users, member.user)
	}
	slices.SortFunc(result.Users, func(a, b user) int { return strings.Compare(a.Name, b.Name) })
	return result
}

func (s *service) stopVoice(c *client) {
	if c.Voice == "" {
		return
	}
	if s.voice.Admin != "" {
		go s.removeVoice(c.active+"-"+c.Voice, c.voiceID)
	}
	c.Voice, c.voiceID = "", ""
	c.Speaking = false
	c.Seq++
}

func (s *service) leave(c *client) {
	s.stopVoice(c)
	if srv := s.servers[c.active]; srv != nil {
		delete(srv.members, c.ID)
		if len(srv.members) == 0 {
			srv.rooms, srv.waking = nil, false
		}
		srv.changed()
	}
	c.active, c.textID, c.pendingMic = "", "", nil
	c.version++
	c.dirty = true
	c.notify()
}

func (s *service) removeServer(code string) {
	srv := s.servers[code]
	for _, member := range srv.members {
		s.stopVoice(member)
		member.active, member.textID, member.pendingMic = "", "", nil
		member.version++
		member.dirty = true
		member.send(event{Type: "closed", Code: code})
	}
	delete(s.servers, code)
}

func (s *service) closeClient(c *client) {
	s.leave(c)
	for code, srv := range s.servers {
		if srv.owner == c.ID {
			s.removeServer(code)
		}
	}
	c.closed = true
	c.notify()
	delete(s.clients, c.key)
}

func (s *service) expire(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			s.mu.Lock()
			for _, c := range s.clients {
				if now.Sub(c.seen) > grace {
					s.closeClient(c)
				}
			}
			s.mu.Unlock()
		}
	}
}

func fail(message string) response { return response{Error: message} }

func nameTaken(srv *server, name, ownID string) bool {
	for _, c := range srv.members {
		if c.ID != ownID && strings.EqualFold(name, c.Name) {
			return true
		}
	}
	return false
}

func validRooms(rooms []room) bool {
	counts := map[string]int{}
	ids := map[string]bool{}
	for _, r := range rooms {
		name, ok := cleanLabel(r.Name, 1, 40)
		if !ok || name != r.Name || !validID(r.ID, 12) || ids[r.ID] || r.Limit < 0 || r.Limit > maxVoice || (r.Kind != "text" && r.Kind != "voice") {
			return false
		}
		ids[r.ID] = true
		counts[r.Kind]++
	}
	return counts["text"] <= maxRooms && counts["voice"] <= maxRooms
}

func (s *service) command(c *client, q request) response {
	srv := s.servers[q.Code]
	active := s.servers[c.active]
	switch q.Op {
	case "catalog":
		if len(q.Codes) > 512 {
			return fail("Too many invite codes.")
		}
		list := make([]advert, 0, len(q.Codes))
		for _, code := range q.Codes {
			if found := s.servers[code]; found != nil {
				list = append(list, found.advert)
			}
		}
		return response{Catalog: list}
	case "advertise":
		name, ok := cleanLabel(q.Name, 1, 64)
		if !ok || !validID(q.Code, 16) || !validID(q.Secret, 40) || inviteCode(q.Secret) != q.Code {
			return fail("Invalid server details.")
		}
		if srv == nil {
			if len(s.servers) >= 8192 {
				return fail("The relay is at capacity.")
			}
			srv = &server{advert: advert{q.Code, name}, owner: c.ID, ownerCode: c.ProfileCode, secret: q.Secret, members: map[string]*client{}}
			s.servers[q.Code] = srv
			if q.Archive {
				s.loadRoster(srv)
			}
		}
		if srv.owner != c.ID || srv.secret != q.Secret {
			return fail("The invite code is already in use.")
		}
		if srv.archive != q.Archive {
			srv.archive = q.Archive
			s.saveRoster(srv)
			srv.changed()
		}
		return response{}
	case "join":
		if srv == nil || (c.invited != "" && c.invited != q.Code) {
			return fail("The server is currently unavailable.")
		}
		if c.active == q.Code {
			return response{State: s.snapshot(c)}
		}
		if nameTaken(srv, c.Name, c.ID) {
			return fail("That username is already in use on this server.")
		}
		if c.ProfileCode != "" && srv.hasOnline(c.ProfileCode) {
			return fail("This profile is already online in this server.")
		}
		if !srv.hasSpace(c) {
			return fail("The server is full (256 users).")
		}
		if srv.rooms == nil {
			if !srv.waking {
				for _, owner := range s.clients {
					if owner.ID == srv.owner {
						owner.send(event{Type: "load", Code: q.Code})
						srv.waking = true
						break
					}
				}
			}
			return response{Retry: true}
		}
		s.leave(c)
		c.active = q.Code
		srv.members[c.ID] = c
		c.textID = srv.firstText()
		s.rememberMember(srv, c)
		srv.changed()
		return response{State: s.snapshot(c)}
	case "hydrate":
		if srv == nil || srv.owner != c.ID || !validRooms(q.Rooms) || len(q.Rooms) == 0 {
			return fail("The rooms could not be loaded.")
		}
		if srv.rooms == nil {
			srv.rooms, srv.waking = q.Rooms, false
		}
		return response{}
	case "leave":
		s.leave(c)
		return response{Reset: true}
	case "delete":
		if srv == nil || srv.owner != c.ID {
			return fail("Only the owner can delete the server.")
		}
		srv.archive = false
		s.saveRoster(srv)
		s.removeServer(q.Code)
		return response{State: s.snapshot(c), Reset: c.active == ""}
	case "profile":
		name, ok := cleanLabel(q.Name, 4, 32)
		if !ok || q.Color < 0 || q.Color >= 16 {
			return fail("Usernames must be 4–32 characters.")
		}
		if active != nil && nameTaken(active, name, c.ID) {
			return fail("That username is already in use on this server.")
		}
		c.Name, c.Color = name, q.Color
		if active != nil {
			s.rememberMember(active, c)
			active.changed()
		}
		return response{User: &c.user, State: s.snapshot(c)}
	case "quit":
		s.closeClient(c)
		return response{}
	}

	if active == nil || active.Code != q.Code {
		return fail("Join the server first.")
	}
	index := slices.IndexFunc(active.rooms, func(r room) bool { return r.ID == q.Room })
	switch q.Op {
	case "sync":
		return response{State: s.snapshot(c)}
	case "mic_status":
		if !c.indicators || q.Target != "" {
			return fail("Invalid microphone update.")
		}
		c.setMic(active, q.Muted, q.Speaking && !q.Muted && c.Voice != "" && q.Room == c.Voice)
		return response{}
	case "text_enter":
		if index < 0 || active.rooms[index].Kind != "text" {
			return fail("Select a text room.")
		}
		if !active.hasRoom(active.rooms[index], c) {
			return fail("The text room is full.")
		}
		c.textID = q.Room
		c.version++
		return response{State: s.snapshot(c)}
	case "text_leave":
		c.textID = ""
		c.version++
		return response{State: s.snapshot(c)}
	case "room_limit":
		if active.owner != c.ID || index < 0 {
			return fail("Only the owner can set a room limit.")
		}
		if q.Limit < 1 || q.Limit > maxVoice {
			return fail("Choose a room limit from 1 to 16.")
		}
		if active.occupancy(active.rooms[index], nil) > q.Limit {
			return fail("More people are in this room than the selected limit. Choose a higher limit or wait for someone to leave.")
		}
		active.rooms[index].Limit = q.Limit
		active.changed()
		return response{State: s.snapshot(c)}
	case "forget_member":
		if active.owner != c.ID || active.hasOnline(q.Target) {
			return fail("Only the owner can remove an offline member.")
		}
		if _, ok := active.known[q.Target]; !ok {
			return fail("This profile is no longer offline.")
		}
		delete(active.known, q.Target)
		s.saveRoster(active)
		active.changed()
		return response{State: s.snapshot(c)}
	case "room_add":
		name, ok := cleanLabel(q.Name, 1, 40)
		if active.owner != c.ID || !ok || (q.Kind != "text" && q.Kind != "voice") {
			return fail("Only the owner can create rooms. Enter a valid room name.")
		}
		count := 0
		for _, r := range active.rooms {
			if r.Kind == q.Kind {
				count++
			}
		}
		if count == maxRooms {
			return fail("The limit of 32 rooms of this type has been reached.")
		}
		r := room{ID: randomString(12), Name: name, Kind: q.Kind, Limit: maxVoice}
		active.rooms = append(active.rooms, r)
		active.changed()
		return response{State: s.snapshot(c), Room: &r}
	case "room_delete":
		if active.owner != c.ID || index < 0 || len(active.rooms) == 1 {
			return fail("Keep at least one room on the server.")
		}
		for _, member := range active.members {
			if member.Voice == q.Room {
				s.stopVoice(member)
			}
		}
		active.rooms = slices.Delete(active.rooms, index, index+1)
		for _, member := range active.members {
			if member.textID == q.Room {
				member.textID = active.firstText()
			}
		}
		active.changed()
		return response{State: s.snapshot(c)}
	case "room_rename":
		name, ok := cleanLabel(q.Name, 1, 40)
		if active.owner != c.ID || index < 0 || !ok {
			return fail("Only the owner can rename rooms. Enter a valid room name.")
		}
		active.rooms[index].Name = name
		active.changed()
		return response{State: s.snapshot(c)}
	case "chat":
		if index < 0 || active.rooms[index].Kind != "text" {
			return fail("Select a text room.")
		}
		if c.textID != q.Room && (active.rooms[index].Limit > 0 || c.indicators) {
			return fail("Join the text room first.")
		}
		body := strings.TrimSpace(q.Text)
		if utf8.RuneCountInString(body) < 1 || utf8.RuneCountInString(body) > 2000 ||
			strings.IndexFunc(body, unicode.IsControl) >= 0 {
			return fail("Messages must be 1–2,000 characters.")
		}
		if time.Since(c.lastMessage) < 150*time.Millisecond {
			return fail("Wait a moment before sending another message.")
		}
		c.lastMessage = time.Now()
		e := event{Type: "chat", Code: c.active, Room: q.Room, Name: active.rooms[index].Name,
			Sender: c.Name, Text: body, At: time.Now().UTC().Format(time.RFC3339)}
		for _, member := range active.members {
			if active.rooms[index].Limit == 0 || member.textID == q.Room {
				member.send(e)
			} else if member.ID == active.owner && active.archive {
				archive := e
				archive.Type = "archive_chat"
				member.send(archive)
			}
		}
		if active.archive && active.members[active.owner] == nil {
			for _, owner := range s.clients {
				if owner.ID == active.owner {
					owner.send(e)
					break
				}
			}
		}
		return response{}
	case "voice":
		if index < 0 || active.rooms[index].Kind != "voice" {
			return fail("Select a voice room.")
		}
		if s.voice.URL == "" {
			return fail("The voice service has not been configured.")
		}
		if !active.hasRoom(active.rooms[index], c) {
			return fail("The voice room is full.")
		}
		if c.Voice != q.Room {
			s.stopVoice(c)
			c.Voice, c.voiceID = q.Room, c.ID+"-"+randomString(12)
			active.changed()
		}
		grant := map[string]any{"roomJoin": true, "room": c.active + "-" + q.Room, "canPublish": true,
			"canPublishSources": []string{"microphone"}, "canPublishData": false, "canSubscribe": true}
		result := response{State: s.snapshot(c), URL: s.voice.URL, Token: s.voice.jwt(c.voiceID, grant)}
		if s.voice.turnSecret != "" {
			result.TurnUser, result.TurnPassword = voiceCredentials(s.voice.turnSecret, c.voiceID)
		}
		return result
	case "voice_leave":
		s.stopVoice(c)
		active.changed()
		return response{State: s.snapshot(c)}
	case "voice_kick":
		target := active.members[q.Target]
		if active.owner != c.ID || target == nil || target.ID == c.ID || target.Voice == "" || target.Voice != q.Room {
			return fail("Only the owner can remove another participant from a voice room.")
		}
		s.stopVoice(target)
		target.send(event{Type: "voice_kicked", Code: q.Code, Room: q.Room})
		active.changed()
		return response{State: s.snapshot(c)}
	case "kick":
		target := active.members[q.Target]
		if active.owner != c.ID || target == nil || target.ID == c.ID {
			return fail("Only the owner can remove another user.")
		}
		delete(active.known, target.ProfileCode)
		s.saveRoster(active)
		s.leave(target)
		target.send(event{Type: "kicked", Code: q.Code})
		return response{}
	}
	return fail("Unknown command.")
}

func (v voiceConfig) jwt(identity string, grant map[string]any) string {
	claims, _ := json.Marshal(map[string]any{"iss": v.Key, "sub": identity, "nbf": time.Now().Add(-5 * time.Second).Unix(),
		"exp": time.Now().Add(time.Minute).Unix(), "video": grant})
	enc := base64.RawURLEncoding
	signed := enc.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`)) + "." + enc.EncodeToString(claims)
	mac := hmac.New(sha256.New, []byte(v.Secret))
	mac.Write([]byte(signed))
	return signed + "." + enc.EncodeToString(mac.Sum(nil))
}

func (s *service) removeVoice(roomName, identity string) {
	body, _ := json.Marshal(map[string]string{"room": roomName, "identity": identity})
	token := s.voice.jwt("kanal-control", map[string]any{"roomAdmin": true, "room": roomName})
	req, err := http.NewRequest(http.MethodPost, strings.TrimRight(s.voice.Admin, "/")+"/twirp/livekit.RoomService/RemoveParticipant", bytes.NewReader(body))
	if err != nil {
		return
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")
	resp, err := s.http.Do(req)
	if err == nil {
		resp.Body.Close()
	}
}

func (s *service) poll(ctx context.Context, c *client) response {
	timeout := time.NewTimer(pollTime)
	defer timeout.Stop()
	for {
		s.mu.Lock()
		if c.closed {
			s.mu.Unlock()
			return fail("Your session expired.")
		}
		if c.dirty || len(c.pending) != 0 || len(c.pendingMic) != 0 {
			result := response{Events: c.pending, Version: c.version}
			c.pending = nil
			if c.dirty {
				result.State, result.Reset = s.snapshot(c), c.active == ""
				c.dirty = false
			} else if len(c.pendingMic) != 0 {
				result.MicCode, result.Mic = c.active, c.pendingMic
			}
			c.pendingMic = nil
			s.mu.Unlock()
			return result
		}
		s.mu.Unlock()
		select {
		case <-c.wake:
		case <-ctx.Done():
			return response{}
		case <-timeout.C:
			return response{}
		}
	}
}

func (s *service) handle(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	write := func(status int, a response) {
		w.WriteHeader(status)
		json.NewEncoder(w).Encode(a)
	}
	if r.Method != http.MethodPost {
		write(http.StatusMethodNotAllowed, fail("Use POST for this endpoint."))
		return
	}
	var q request
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10))
	if err := decoder.Decode(&q); err != nil || decoder.Decode(new(any)) != io.EOF {
		write(http.StatusBadRequest, fail("Invalid request."))
		return
	}
	s.mu.Lock()
	if q.Op == "register" {
		invited := ""
		if s.hosted && r.TLS != nil {
			invited = r.Header.Get("X-TinySpeak-Invite")
			if !validID(invited, 16) || s.servers[invited] == nil {
				s.mu.Unlock()
				write(http.StatusOK, fail("The host is online, but this server is unavailable."))
				return
			}
		}
		name, ok := cleanLabel(q.Name, 4, 32)
		if !ok || q.Color < 0 || q.Color >= 16 || len(s.clients) >= 65536 {
			s.mu.Unlock()
			write(http.StatusOK, fail("Usernames must be 4–32 characters."))
			return
		}
		c := &client{user: user{ID: randomString(24), ProfileID: profileID(q.Profile), ProfileCode: profileCode(q.Profile), micState: micState{Muted: q.Muted}, Name: name, Color: q.Color}, key: randomString(40),
			seen: time.Now(), wake: make(chan struct{}, 1), invited: invited, indicators: q.Indicators}
		s.clients[c.key] = c
		u := c.user
		s.mu.Unlock()
		write(http.StatusOK, response{Key: c.key, User: &u})
		return
	}
	c := s.clients[strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")]
	if c == nil {
		s.mu.Unlock()
		write(http.StatusUnauthorized, fail("Your session expired."))
		return
	}
	if s.hosted && r.TLS != nil && (c.invited == "" || q.Op == "advertise") {
		s.mu.Unlock()
		write(http.StatusForbidden, fail("This command is reserved for the host."))
		return
	}
	c.seen = time.Now()
	if q.Op == "poll" {
		s.mu.Unlock()
		write(http.StatusOK, s.poll(r.Context(), c))
		return
	}
	a := s.command(c, q)
	a.Version = c.version
	if a.User != nil {
		u := *a.User
		a.User = &u
	}
	s.mu.Unlock()
	write(http.StatusOK, a)
}

func env(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func main() {
	if len(os.Args) > 1 {
		if err := portableMain(os.Args[1:]); err != nil {
			json.NewEncoder(os.Stdout).Encode(map[string]string{"error": err.Error()})
			os.Exit(1)
		}
		return
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	service := &service{
		clients: map[string]*client{}, servers: map[string]*server{}, http: &http.Client{Timeout: 5 * time.Second},
		voice: voiceConfig{URL: os.Getenv("KANAL_VOICE_URL"), Admin: os.Getenv("KANAL_VOICE_ADMIN"),
			Key: os.Getenv("KANAL_VOICE_KEY"), Secret: os.Getenv("KANAL_VOICE_SECRET")},
	}
	if service.voice.URL != "" && (service.voice.Admin == "" || service.voice.Key == "" || service.voice.Secret == "") {
		log.Fatal("Set KANAL_VOICE_ADMIN, KANAL_VOICE_KEY and KANAL_VOICE_SECRET.")
	}
	go service.expire(ctx)
	mux := http.NewServeMux()
	mux.HandleFunc("/api", service.handle)
	mux.HandleFunc("/health", func(w http.ResponseWriter, _ *http.Request) { fmt.Fprintln(w, "TinySpeak") })
	addr := env("KANAL_LISTEN", "127.0.0.1:8090")
	server := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout: 5 * time.Second, WriteTimeout: 20 * time.Second, IdleTimeout: 45 * time.Second, MaxHeaderBytes: 8192}
	go func() {
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		server.Shutdown(shutdown)
	}()
	log.Printf("TinySpeak on %s", addr)
	if err := server.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
		log.Fatal(err)
	}
}
