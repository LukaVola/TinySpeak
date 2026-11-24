package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

type micState struct {
	Muted    bool   `json:"muted,omitempty"`
	Speaking bool   `json:"speaking,omitempty"`
	Seq      uint64 `json:"mic_seq,omitempty"`
}

func (c *client) setMic(srv *server, muted, speaking bool) {
	if c.Muted == muted && c.Speaking == speaking {
		return
	}
	c.Muted, c.Speaking = muted, speaking
	c.Seq++
	for _, peer := range srv.members {
		// A pending full snapshot will include the current indicator.
		if !peer.indicators || peer.dirty {
			continue
		}
		if peer.pendingMic == nil {
			peer.pendingMic = make(map[string]micState)
		}
		peer.pendingMic[c.ID] = c.micState
		peer.notify()
	}
}

func (srv *server) voiceOccupancy(id string, exclude *client) int {
	used := 0
	for _, member := range srv.members {
		if member != exclude && member.Voice == id {
			used++
		}
	}
	return used
}

func (srv *server) hasVoiceSeat(r room, member *client) bool {
	limit := r.Limit
	if limit == 0 {
		limit = maxVoice
	}
	return srv.voiceOccupancy(r.ID, member) < limit
}

func (srv *server) firstText() string {
	for _, r := range srv.rooms {
		if r.Kind == "text" {
			return r.ID
		}
	}
	return ""
}

type knownProfile struct {
	Code  string `json:"profile_code"`
	Name  string `json:"name"`
	Color int    `json:"color"`
}

// A saved roster index matches its entry in files created by Patch 17.
type savedMember struct {
	knownProfile
	Key string `json:"member_key,omitempty"`
}

func (srv *server) keyForCode(code string) string {
	for key, p := range srv.known {
		if p.Code == code {
			return key
		}
	}
	return ""
}

func (srv *server) hasOnline(key string) bool {
	for _, c := range srv.members {
		if c.memberKey == key {
			return true
		}
	}
	return false
}

func (srv *server) hasSpace(c *client) bool {
	if len(srv.members) >= maxUsers {
		return false
	}
	if _, returning := srv.known[c.memberKey]; returning {
		return true
	}
	total := len(srv.known)
	for _, m := range srv.members {
		if m.memberKey == "" {
			total++
		}
	}
	if srv.owner != c.ID && srv.ownerKey != "" {
		if _, counted := srv.known[srv.ownerKey]; !counted {
			total++
		}
	}
	return total < maxUsers
}

func (srv *server) offline() []knownProfile {
	online := make(map[string]bool, len(srv.members))
	for _, c := range srv.members {
		online[c.memberKey] = true
	}
	var absent []knownProfile
	for code, p := range srv.known {
		if !online[code] {
			absent = append(absent, p)
		}
	}
	slices.SortFunc(absent, func(a, b knownProfile) int {
		if cmp := strings.Compare(a.Name, b.Name); cmp != 0 {
			return cmp
		}
		return strings.Compare(a.Code, b.Code)
	})
	return absent
}

func (s *service) rememberMember(srv *server, c *client) {
	if c.memberKey == "" || c.ProfileCode == "" {
		return
	}
	p := knownProfile{Code: c.ProfileCode, Name: c.Name, Color: c.Color}
	if p == srv.known[c.memberKey] {
		return
	}
	if srv.known == nil {
		srv.known = make(map[string]knownProfile)
	}
	srv.known[c.memberKey] = p
	if srv.archive {
		s.saveRoster(srv)
	}
}

// Only the portable owner host writes rosters. Shared relays keep them in memory.
func (s *service) loadRoster(srv *server) {
	if s.rosterDir == "" {
		return
	}
	data, err := os.ReadFile(filepath.Join(s.rosterDir, srv.Code+".members.json"))
	if err != nil {
		return
	}
	var profiles []savedMember
	if json.Unmarshal(data, &profiles) != nil || len(profiles) > maxUsers {
		return
	}
	srv.known = make(map[string]knownProfile, len(profiles))
	for _, p := range profiles {
		name, ok := cleanLabel(p.Name, 4, 32)
		if ok && name == p.Name && validID(p.Code, 16) && p.Color >= 0 && p.Color < 16 {
			key := p.Key
			if key == "" {
				key = p.Code
			}
			if validID(key, 16) {
				srv.known[key] = p.knownProfile
			}
		}
	}
}

func (s *service) saveRoster(srv *server) {
	if s.rosterDir == "" {
		return
	}
	path := filepath.Join(s.rosterDir, srv.Code+".members.json")
	if !srv.archive || len(srv.known) == 0 {
		_ = os.Remove(path)
		return
	}
	if os.MkdirAll(s.rosterDir, 0700) != nil {
		return
	}
	profiles := make([]savedMember, 0, len(srv.known))
	for key, p := range srv.known {
		profiles = append(profiles, savedMember{knownProfile: p, Key: key})
	}
	file, err := os.CreateTemp(s.rosterDir, ".members-*.tmp")
	if err != nil {
		return
	}
	defer os.Remove(file.Name())
	err = json.NewEncoder(file).Encode(profiles)
	if err == nil {
		err = file.Sync()
	}
	if closeErr := file.Close(); err == nil && closeErr == nil {
		_ = os.Rename(file.Name(), path)
	}
}
