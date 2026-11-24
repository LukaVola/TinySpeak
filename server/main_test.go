package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestVoiceProfileSurvivesReconnectAndRename(t *testing.T) {
	s := testService()
	secret := randomString(40)
	register := func(name, secret, nonce string) response {
		t.Helper()
		body, err := json.Marshal(request{Op: "register", Name: name, Profile: secret, RunNonce: nonce})
		if err != nil {
			t.Fatal(err)
		}
		rec := httptest.NewRecorder()
		s.handle(rec, httptest.NewRequest(http.MethodPost, "/api", bytes.NewReader(body)))
		var result response
		if err := json.Unmarshal(rec.Body.Bytes(), &result); err != nil || result.Error != "" || result.User == nil {
			t.Fatalf("register failed: %s", rec.Body.String())
		}
		return result
	}
	nonce := randomString(40)
	first := register("First name", secret, nonce)
	updated := s.command(s.clients[first.Key], request{Op: "profile", Name: "Second name"})
	again := register("Third name", secret, nonce)
	restarted := register("Third name", secret, randomString(40))
	older := register("Older client", secret, "")
	other := register("Second name", randomString(40), nonce)
	legacy := register("Legacy", "", "")
	if first.User.ProfileID == "" || first.User.ProfileID != updated.User.ProfileID ||
		first.User.ProfileID != again.User.ProfileID || again.User.ID == first.User.ID {
		t.Fatal("profile identity changed with the username or session")
	}
	if !validID(first.User.ProfileCode, 16) || first.User.ProfileCode != again.User.ProfileCode || first.User.ProfileCode != updated.User.ProfileCode ||
		first.User.ProfileCode == other.User.ProfileCode || legacy.User.ProfileCode != "" || profileCode(first.User.ProfileCode) != "" {
		t.Fatal("public profile code was unstable, shared, or accepted as the private identity")
	}
	if restarted.User.ProfileCode == first.User.ProfileCode || restarted.User.ProfileID != first.User.ProfileID || older.User.ProfileCode != profileCode(secret) {
		t.Fatal("public code failed to rotate independently of private playback identity")
	}
	if other.User.ProfileID == first.User.ProfileID || legacy.User.ProfileID != "" {
		t.Fatal("independent or legacy clients shared the voice profile identity")
	}
}

func testService() *service {
	return &service{clients: map[string]*client{}, servers: map[string]*server{}, http: &http.Client{},
		voice: voiceConfig{URL: "ws://127.0.0.1:7880", Key: "devkey", Secret: "secret"}}
}

func (s *service) testClient(name string) *client {
	c := &client{user: user{ID: randomString(24), Name: name}, key: randomString(40),
		wake: make(chan struct{}, 1), seen: time.Now()}
	s.clients[c.key] = c
	return c
}

func TestServerLifecycleKeepsOnlyLiveMessages(t *testing.T) {
	s := testService()
	host, guest, late := s.testClient("Luka"), s.testClient("Guest"), s.testClient("Latecomer")
	secret, textRoom := randomString(40), randomString(12)
	code := inviteCode(secret)
	q := request{Op: "advertise", Code: code, Secret: secret, Name: "Ekipa"}
	if got := s.command(host, q); got.Error != "" {
		t.Fatal(got.Error)
	}
	if got := s.command(guest, request{Op: "join", Code: code}); !got.Retry || len(host.pending) != 1 || host.pending[0].Type != "load" {
		t.Fatal("the guest did not wake the unoccupied server")
	}
	s.command(host, request{Op: "hydrate", Code: code, Rooms: []room{{ID: textRoom, Name: "tekst", Kind: "text"}}})
	s.command(host, request{Op: "join", Code: code})
	s.command(guest, request{Op: "join", Code: code})
	if got := s.command(guest, request{Op: "profile", Name: "luka", Color: 2}); got.Error == "" {
		t.Fatal("accepted a previously taken username in different case")
	}
	if got := s.command(guest, request{Op: "room_add", Code: code, Kind: "text", Name: "nova"}); got.Error == "" {
		t.Fatal("member acquired owner rights")
	}
	s.command(guest, request{Op: "chat", Code: code, Room: textRoom, Text: "Bok!"})
	if len(guest.pending) != 1 || guest.pending[0].Text != "Bok!" {
		t.Fatal("live text was not relayed")
	}
	s.command(late, request{Op: "join", Code: code})
	if len(late.pending) > 0 {
		t.Fatal("late member acquired old chat")
	}
	s.command(host, request{Op: "leave"})
	if s.servers[code] == nil || guest.active != code {
		t.Fatal("host switching servers removed the session")
	}
	s.command(guest, request{Op: "leave"})
	s.command(late, request{Op: "leave"})
	if s.servers[code].rooms != nil {
		t.Fatal("empty server kept its room layout loaded")
	}
	s.closeClient(host)
	if s.servers[code] != nil {
		t.Fatal("host shutdown did not unpublish server")
	}
	q.Secret = randomString(40)
	if s.command(guest, q).Error == "" {
		t.Fatal("the invite code alone let the guest claim ownership")
	}
}

func TestLimitsAndRoomScopedVoice(t *testing.T) {
	s := testService()
	host := s.testClient("Host")
	secret, voiceRoom := randomString(40), randomString(12)
	code := inviteCode(secret)
	s.command(host, request{Op: "advertise", Code: code, Secret: secret, Name: "Test"})
	s.command(host, request{Op: "hydrate", Code: code, Rooms: []room{{ID: voiceRoom, Name: "Glas", Kind: "voice"}}})
	s.command(host, request{Op: "join", Code: code})
	for i := 0; i < maxRooms; i++ {
		if s.command(host, request{Op: "room_add", Code: code, Kind: "text", Name: "Soba"}).Error != "" {
			t.Fatal("valid text room was rejected")
		}
	}
	if s.command(host, request{Op: "room_add", Code: code, Kind: "text", Name: "Soba"}).Error == "" {
		t.Fatal("created 33rd text room")
	}
	for i := 1; i < maxRooms; i++ {
		if s.command(host, request{Op: "room_add", Code: code, Kind: "voice", Name: "Glas"}).Error != "" {
			t.Fatal("valid voice room was rejected")
		}
	}
	if s.command(host, request{Op: "room_add", Code: code, Kind: "voice", Name: "Glas"}).Error == "" {
		t.Fatal("created 33rd voice room")
	}
	for i := 1; i <= maxUsers; i++ {
		guest := s.testClient("Gost" + randomString(12))
		joined := s.command(guest, request{Op: "join", Code: code})
		if i == maxUsers && joined.Error == "" || i < maxUsers && joined.Error != "" {
			t.Fatal("member count was not limited to 256")
		}
		if i > maxVoice+1 {
			continue
		}
		got := s.command(guest, request{Op: "voice", Code: code, Room: voiceRoom})
		if i <= maxVoice && (got.Error != "" || got.Token == "") || i > maxVoice && got.Error == "" {
			t.Fatal("voice count was not limited to 16")
		}
	}
}
