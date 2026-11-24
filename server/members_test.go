package main

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func mustCommand(t *testing.T, s *service, c *client, q request) response {
	t.Helper()
	r := s.command(c, q)
	if r.Error != "" || r.Retry {
		t.Fatalf("%s: %s (retry %v)", q.Op, r.Error, r.Retry)
	}
	return r
}

func publishRooms(t *testing.T, s *service, host *client, secret string, save bool, rooms []room) string {
	t.Helper()
	code := inviteCode(secret)
	mustCommand(t, s, host, request{Op: "advertise", Code: code, Secret: secret, Name: "Test", Archive: save})
	mustCommand(t, s, host, request{Op: "hydrate", Code: code, Rooms: rooms})
	return code
}

func TestRoomSeatsAcrossConcurrentRequests(t *testing.T) {
	s := testService()
	host, a, b := s.testClient("Owner"), s.testClient("Alpha"), s.testClient("Bravo")
	general := room{ID: "GeneralRoom1", Name: "General", Kind: "text", Limit: 16}
	small := room{ID: "PrivateChat1", Name: "Small", Kind: "text", Limit: 1}
	voice := room{ID: "VoiceLounge1", Name: "Lounge", Kind: "voice", Limit: 1}
	code := publishRooms(t, s, host, randomString(40), false, []room{general, small, voice})
	for _, c := range []*client{host, a, b} {
		mustCommand(t, s, c, request{Op: "join", Code: code})
	}
	for _, r := range []room{general, voice} {
		if s.command(a, request{Op: "room_limit", Code: code, Room: r.ID, Limit: 2}).Error == "" {
			t.Fatal("guest changed limit")
		}
		for _, n := range []int{-1, 0, 17} {
			if s.command(host, request{Op: "room_limit", Code: code, Room: r.ID, Limit: n}).Error == "" {
				t.Fatal("accepted invalid limit", n)
			}
		}
		if s.command(host, request{Op: "room_limit", Code: code, Room: r.ID, Limit: 16}).State == nil {
			t.Fatal("owner unable to set limit")
		}
	}
	if s.command(host, request{Op: "room_limit", Code: code, Room: general.ID, Limit: 2}).Error == "" {
		t.Fatal("evicted occupants to lower a limit")
	}
	mustCommand(t, s, host, request{Op: "room_limit", Code: code, Room: voice.ID, Limit: 1})
	race := func(op string, r room) (*client, *client) {
		t.Helper()
		start := make(chan struct{})
		type result struct {
			c     *client
			reply response
		}
		done := make(chan result, 2)
		for _, c := range []*client{a, b} {
			go func() {
				<-start
				body, _ := json.Marshal(request{Op: op, Code: code, Room: r.ID})
				req := httptest.NewRequest("POST", "/api", bytes.NewReader(body))
				req.Header.Set("Authorization", "Bearer "+c.key)
				w := httptest.NewRecorder()
				s.handle(w, req)
				var reply response
				if err := json.Unmarshal(w.Body.Bytes(), &reply); err != nil {
					reply.Error = err.Error()
				}
				done <- result{c, reply}
			}()
		}
		close(start)
		var win, lose *client
		for i := 0; i < 2; i++ {
			x := <-done
			if x.reply.Error != "" {
				lose = x.c
			} else if x.reply.State != nil {
				win = x.c
			}
		}
		if win == nil || lose == nil || s.servers[code].occupancy(r, nil) != 1 {
			t.Fatal("last seat was not allocated exactly once", op)
		}
		return win, lose
	}
	reader, outside := race("text_enter", small)
	if outside.textID != general.ID {
		t.Fatal("failed switch lost original seat")
	}
	mustCommand(t, s, reader, request{Op: "chat", Code: code, Room: small.ID, Text: "inside"})
	if len(outside.pending) != 0 || len(host.pending) != 0 || len(reader.pending) != 1 {
		t.Fatal("limited chat reached a non-member")
	}
	if s.command(outside, request{Op: "chat", Code: code, Room: small.ID, Text: "outside"}).Error == "" {
		t.Fatal("outside user posted to limited room")
	}
	speaker, waiting := race("voice", voice)
	mustCommand(t, s, speaker, request{Op: "voice", Code: code, Room: voice.ID})
	if s.snapshot(reader).Text != small.ID {
		t.Fatal("voice activity displaced text")
	}
	mustCommand(t, s, speaker, request{Op: "voice_leave", Code: code})
	mustCommand(t, s, waiting, request{Op: "voice", Code: code, Room: voice.ID})
	mustCommand(t, s, reader, request{Op: "text_leave", Code: code})
	mustCommand(t, s, outside, request{Op: "text_enter", Code: code, Room: small.ID})
	mustCommand(t, s, host, request{Op: "room_delete", Code: code, Room: small.ID})
	if outside.textID != general.ID || waiting.Voice != voice.ID {
		t.Fatal("deletion affected unrelated seats")
	}
	mustCommand(t, s, host, request{Op: "room_limit", Code: code, Room: general.ID, Limit: 2})
	newcomer := s.testClient("Newcomer")
	r := mustCommand(t, s, newcomer, request{Op: "join", Code: code})
	if r.State.Text != "" {
		t.Fatal("server join exceeded capacity of the only text channel")
	}
	added := mustCommand(t, s, host, request{Op: "room_add", Code: code, Kind: "text", Name: "Extra"}).Room
	if added.Limit != 16 {
		t.Fatal("unexpected default for a new room")
	}
	mustCommand(t, s, newcomer, request{Op: "text_enter", Code: code, Room: added.ID})
}

func TestOfflineRosterAndLocalPersistence(t *testing.T) {
	s := testService()
	s.rosterDir = t.TempDir()
	owner, guest := s.testClient("Owner"), s.testClient("Guest")
	ownerSecret, guestSecret, serverSecret := randomString(40), randomString(40), randomString(40)
	owner.ProfileCode, guest.ProfileCode = profileCode(ownerSecret), profileCode(guestSecret)
	rooms := []room{{ID: "GeneralRoom1", Kind: "text", Name: "General", Limit: 16}}
	first := publishRooms(t, s, owner, serverSecret, true, rooms)
	second := publishRooms(t, s, owner, randomString(40), false, rooms)
	for _, c := range []*client{owner, guest} {
		mustCommand(t, s, c, request{Op: "join", Code: first})
	}
	duplicate := s.testClient("Other name")
	duplicate.ProfileCode = guest.ProfileCode
	if s.command(duplicate, request{Op: "join", Code: first}).Error == "" {
		t.Fatal("same profile joined twice")
	}
	mustCommand(t, s, guest, request{Op: "profile", Name: "Renamed", Color: 7})
	mustCommand(t, s, guest, request{Op: "join", Code: second})
	absent := s.snapshot(owner).Offline
	if len(absent) != 1 || absent[0] != (knownProfile{guest.ProfileCode, "Renamed", 7}) || len(s.snapshot(guest).Offline) != 0 {
		t.Fatal("presence leaked across servers", absent)
	}
	newObserver := s.testClient("Observer")
	mustCommand(t, s, newObserver, request{Op: "join", Code: first})
	if len(s.snapshot(newObserver).Offline) != 1 {
		t.Fatal("new observer lost offline roster")
	}
	path := filepath.Join(s.rosterDir, first+".members.json")
	data, err := os.ReadFile(path)
	if err != nil || bytes.Contains(data, []byte(guestSecret)) || !bytes.Contains(data, []byte(guest.ProfileCode)) {
		t.Fatal("roster persistence", err)
	}
	if _, err = os.Stat(filepath.Join(s.rosterDir, second+".members.json")); !os.IsNotExist(err) {
		t.Fatal("unsaved server wrote a roster")
	}
	s.closeClient(owner)
	fresh := testService()
	fresh.rosterDir = s.rosterDir
	again := fresh.testClient("Owner")
	again.ProfileCode = profileCode(ownerSecret)
	publishRooms(t, fresh, again, serverSecret, true, rooms)
	reloaded := mustCommand(t, fresh, again, request{Op: "join", Code: first}).State
	if len(reloaded.Offline) != 1 || reloaded.Offline[0] != absent[0] {
		t.Fatal("restart lost membership")
	}
	returned := fresh.testClient("Renamed")
	returned.ProfileCode = profileCode(guestSecret)
	mustCommand(t, fresh, returned, request{Op: "join", Code: first})
	if len(fresh.snapshot(again).Offline) != 0 {
		t.Fatal("returned profile still offline")
	}
	if fresh.command(again, request{Op: "forget_member", Code: first, Target: returned.ProfileCode}).Error == "" {
		t.Fatal("removed active profile")
	}
	mustCommand(t, fresh, returned, request{Op: "leave"})
	mustCommand(t, fresh, again, request{Op: "forget_member", Code: first, Target: returned.ProfileCode})
	if len(fresh.snapshot(again).Offline) != 0 {
		t.Fatal("removed member still shown")
	}
	mustCommand(t, fresh, again, request{Op: "advertise", Code: first, Secret: serverSecret, Name: "Test", Archive: false})
	if _, err = os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("disabling local saves kept roster")
	}
	mustCommand(t, fresh, again, request{Op: "advertise", Code: first, Secret: serverSecret, Name: "Test", Archive: true})
	mustCommand(t, fresh, again, request{Op: "delete", Code: first})
	if _, err = os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("deleting server kept roster")
	}
}

func TestMicChangesAreScopedAndCoalesced(t *testing.T) {
	s := testService()
	owner, speaker := s.testClient("Owner"), s.testClient("Speaker")
	owner.indicators, speaker.indicators = true, true
	voice := room{ID: "VoiceLounge1", Kind: "voice", Name: "Lounge"}
	code := publishRooms(t, s, owner, randomString(40), false, []room{voice})
	for _, c := range []*client{owner, speaker} {
		mustCommand(t, s, c, request{Op: "join", Code: code})
	}
	q := request{Op: "mic_status", Code: code, Room: voice.ID, Speaking: true}
	mustCommand(t, s, speaker, q)
	if speaker.Speaking {
		t.Fatal("reported speech without joining voice")
	}
	mustCommand(t, s, speaker, request{Op: "voice", Code: code, Room: voice.ID})
	s.poll(context.Background(), owner)
	mustCommand(t, s, speaker, q)
	if !speaker.Speaking {
		t.Fatal("no speaking indicator")
	}
	q.Muted = true
	mustCommand(t, s, speaker, q)
	change := s.poll(context.Background(), owner)
	m := change.Mic[speaker.ID]
	if change.State != nil || change.MicCode != code || len(change.Mic) != 1 || !m.Muted || m.Speaking || m.Seq < 2 {
		t.Fatal("expected one muted delta, not the whole member list", change)
	}
	q.Target = owner.ID
	if s.command(speaker, q).Error == "" {
		t.Fatal("member changed another member's microphone")
	}
	q.Target = ""
	q.Muted = false
	mustCommand(t, s, speaker, q)
	mustCommand(t, s, speaker, request{Op: "voice_leave", Code: code})
	if speaker.Speaking || s.snapshot(owner).Users[1].Speaking {
		t.Fatal("speaker stayed active after leaving voice")
	}
	mustCommand(t, s, speaker, request{Op: "leave"})
	if s.command(speaker, q).Error == "" {
		t.Fatal("status published across servers")
	}
}
