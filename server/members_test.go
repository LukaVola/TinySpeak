package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
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
	if s.command(a, request{Op: "room_limit", Code: code, Room: voice.ID, Limit: 2}).Error == "" {
		t.Fatal("guest changed voice limit")
	}
	for _, n := range []int{-1, 0, 17} {
		if s.command(host, request{Op: "room_limit", Code: code, Room: voice.ID, Limit: n}).Error == "" {
			t.Fatal("accepted invalid limit", n)
		}
	}
	for _, r := range s.snapshot(host).Rooms {
		if r.Kind == "text" && r.Limit != 0 {
			t.Fatal("old text limit survived migration")
		}
	}
	if s.command(host, request{Op: "room_limit", Code: code, Room: general.ID, Limit: 3}).Error == "" {
		t.Fatal("created a limit on a text channel")
	}
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
		if win == nil || lose == nil || s.servers[code].voiceOccupancy(r.ID, nil) != 1 {
			t.Fatal("last seat was not allocated exactly once", op)
		}
		return win, lose
	}
	speaker, waiting := race("voice", voice)
	mustCommand(t, s, speaker, request{Op: "voice", Code: code, Room: voice.ID})
	mustCommand(t, s, host, request{Op: "room_limit", Code: code, Room: voice.ID, Limit: 2})
	mustCommand(t, s, waiting, request{Op: "voice", Code: code, Room: voice.ID})
	if s.command(host, request{Op: "room_limit", Code: code, Room: voice.ID, Limit: 1}).Error == "" {
		t.Fatal("evicted active speakers")
	}
	for i := 0; i < 20; i++ {
		guest := s.testClient(fmt.Sprintf("Guest %d", i))
		if got := mustCommand(t, s, guest, request{Op: "join", Code: code}).State; got.Text != general.ID {
			t.Fatal("default text channel filled up")
		}
		mustCommand(t, s, guest, request{Op: "text_enter", Code: code, Room: small.ID})
	}
	mustCommand(t, s, speaker, request{Op: "text_enter", Code: code, Room: small.ID})
	mustCommand(t, s, speaker, request{Op: "chat", Code: code, Room: small.ID, Text: "hello"})
	if len(waiting.pending) != 1 || len(host.pending) != 1 || len(speaker.pending) != 1 {
		t.Fatal("unlimited text channel failed to deliver live messages")
	}
	mustCommand(t, s, host, request{Op: "room_delete", Code: code, Room: small.ID})
	if speaker.textID != general.ID || waiting.Voice != voice.ID {
		t.Fatal("deleting text room displaced voice")
	}
	added := mustCommand(t, s, host, request{Op: "room_add", Code: code, Kind: "text", Name: "Extra"}).Room
	if added.Limit != 0 {
		t.Fatal("new text channel has a limit")
	}
	newVoice := mustCommand(t, s, host, request{Op: "room_add", Code: code, Kind: "voice", Name: "Second voice"}).Room
	if newVoice.Limit != 16 {
		t.Fatal("new voice room default changed")
	}
}

func TestServerRenameKeepsInviteMembersAndRooms(t *testing.T) {
	s := testService()
	host, guest := s.testClient("Owner"), s.testClient("Guest")
	voice := room{ID: "VoiceLounge1", Kind: "voice", Name: "Lounge", Limit: 6}
	code := publishRooms(t, s, host, randomString(40), true, []room{voice})
	for _, member := range []*client{host, guest} {
		mustCommand(t, s, member, request{Op: "join", Code: code})
	}
	mustCommand(t, s, guest, request{Op: "voice", Code: code, Room: voice.ID})
	for _, name := range []string{" ", strings.Repeat("a", 65), "Bad\nname"} {
		if s.command(host, request{Op: "server_rename", Code: code, Name: name}).Error == "" {
			t.Fatal("invalid server name accepted")
		}
	}
	if s.command(guest, request{Op: "server_rename", Code: code, Name: "Other"}).Error == "" {
		t.Fatal("non-owner renamed server")
	}
	mustCommand(t, s, host, request{Op: "server_rename", Code: code, Name: "  Novi Čat  "})
	seen := s.snapshot(guest)
	if seen.Name != "Novi Čat" || seen.Code != code || seen.Voice != voice.ID || len(seen.Users) != 2 || seen.Rooms[0] != voice {
		t.Fatal("rename lost server state", seen)
	}
	list := mustCommand(t, s, guest, request{Op: "catalog", Codes: []string{code}}).Catalog
	if len(list) != 1 || list[0] != seen.advert {
		t.Fatal("catalog kept the old server name")
	}

}

func TestOfflineRosterAndLocalPersistence(t *testing.T) {
	s := testService()
	s.rosterDir = t.TempDir()
	owner, guest := s.testClient("Owner"), s.testClient("Guest")
	ownerSecret, guestSecret, serverSecret := randomString(40), randomString(40), randomString(40)
	owner.memberKey, guest.memberKey = profileCode(ownerSecret), profileCode(guestSecret)
	owner.ProfileCode, guest.ProfileCode = runProfileCode(ownerSecret, randomString(40)), runProfileCode(guestSecret, randomString(40))
	rooms := []room{{ID: "GeneralRoom1", Kind: "text", Name: "General", Limit: 16}}
	first := publishRooms(t, s, owner, serverSecret, true, rooms)
	second := publishRooms(t, s, owner, randomString(40), false, rooms)
	for _, c := range []*client{owner, guest} {
		mustCommand(t, s, c, request{Op: "join", Code: first})
	}
	duplicate := s.testClient("Other name")
	duplicate.memberKey, duplicate.ProfileCode = guest.memberKey, runProfileCode(guestSecret, randomString(40))
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
	again.memberKey, again.ProfileCode = profileCode(ownerSecret), runProfileCode(ownerSecret, randomString(40))
	publishRooms(t, fresh, again, serverSecret, true, rooms)
	reloaded := mustCommand(t, fresh, again, request{Op: "join", Code: first}).State
	if len(reloaded.Offline) != 1 || reloaded.Offline[0] != absent[0] {
		t.Fatal("restart lost membership")
	}
	returned := fresh.testClient("Renamed")
	returned.memberKey, returned.ProfileCode = profileCode(guestSecret), runProfileCode(guestSecret, randomString(40))
	mustCommand(t, fresh, returned, request{Op: "join", Code: first})
	if len(fresh.snapshot(again).Offline) != 0 {
		t.Fatal("returned profile still offline")
	}
	if fresh.command(again, request{Op: "forget_member", Code: first, Target: returned.ProfileCode}).Error == "" {
		t.Fatal("removed active profile")
	}
	mustCommand(t, fresh, returned, request{Op: "leave"})
	if rows := fresh.snapshot(again).Offline; len(rows) != 1 || rows[0].Code != returned.ProfileCode || rows[0].Code == absent[0].Code {
		t.Fatal("member code did not rotate in the same roster row", rows)
	}
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

func TestLegacyMemberFileUpgradesWithoutDuplicates(t *testing.T) {
	s := testService()
	s.rosterDir = t.TempDir()
	host := s.testClient("Owner")
	secret, userSecret := randomString(40), randomString(40)
	code := inviteCode(secret)
	key := profileCode(userSecret)
	data, _ := json.Marshal([]knownProfile{{key, "Previous name", 2}})
	if err := os.WriteFile(filepath.Join(s.rosterDir, code+".members.json"), data, 0600); err != nil {
		t.Fatal(err)
	}
	publishRooms(t, s, host, secret, true, []room{{ID: "GeneralRoom1", Kind: "text", Name: "General"}})
	mustCommand(t, s, host, request{Op: "join", Code: code})
	user := s.testClient("Current name")
	user.memberKey, user.ProfileCode = key, runProfileCode(userSecret, randomString(40))
	mustCommand(t, s, user, request{Op: "join", Code: code})
	mustCommand(t, s, user, request{Op: "leave"})
	list := s.snapshot(host).Offline
	if len(list) != 1 || list[0].Name != user.Name || list[0].Code != user.ProfileCode {
		t.Fatal("legacy roster was duplicated", list)
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
