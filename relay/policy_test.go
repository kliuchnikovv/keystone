package main

import (
	"testing"
	"time"
)

func TestSecretMatches(t *testing.T) {
	if !SecretMatches("correct horse battery staple", "correct horse battery staple") {
		t.Fatal("the same secret must match")
	}
	if SecretMatches("secret-one", "secret-two") {
		t.Fatal("different secrets must not match")
	}
	if SecretMatches("secret", "secret-and-more") {
		t.Fatal("a prefix must not match")
	}
	if SecretMatches("", "secret") {
		t.Fatal("an empty secret must not match")
	}
	if !SecretMatches("", "") {
		t.Fatal("two empty secrets match")
	}
}

func TestHubIDValid(t *testing.T) {
	for _, id := range []string{"home", "Home-2", "home_3"} {
		if !HubIDValid(id) {
			t.Fatalf("%q must be a valid hub id", id)
		}
	}
	for _, id := range []string{"", "../etc", "home/2", "home 2", "home\n2", "-home"} {
		if HubIDValid(id) {
			t.Fatalf("%q must not be a valid hub id", id)
		}
	}
	long := ""
	for i := 0; i < 65; i++ {
		long += "a"
	}
	if HubIDValid(long) {
		t.Fatal("a 65-character id must not be valid")
	}
}

func TestOriginAllowed(t *testing.T) {
	const host = "relay.example"
	if !OriginAllowed("", nil, host) {
		t.Fatal("a native client sends no Origin and must be allowed")
	}
	if OriginAllowed("https://evil.example", nil, host) {
		t.Fatal("a browser origin that is not listed must be refused")
	}
	if !OriginAllowed("https://app.example", []string{"https://app.example"}, host) {
		t.Fatal("a listed origin must be allowed")
	}
	if !OriginAllowed("HTTPS://APP.EXAMPLE", []string{"https://app.example"}, host) {
		t.Fatal("the comparison must not care about the case")
	}
	if !OriginAllowed("https://app.example", []string{"https://app.example/"}, host) {
		t.Fatal("a trailing slash must not make a listed origin different")
	}
	if OriginAllowed("https://app.example", []string{"https://app.example.evil"}, host) {
		t.Fatal("a listed origin must match exactly")
	}
	// The phone app on iOS is SocketRocket, and it mirrors the address it dials
	// into the Origin header: the relay's own origin is not a foreign site.
	if !OriginAllowed("https://relay.example", nil, host) {
		t.Fatal("the relay's own origin must be allowed")
	}
	if !OriginAllowed("https://relay.example", nil, "relay.example:8443") {
		t.Fatal("the port must not make the relay's own origin different")
	}
	if OriginAllowed("https://relay.example.evil", nil, host) {
		t.Fatal("a host that only starts the same must be refused")
	}
	if OriginAllowed("https://relay.example@evil.example", nil, host) {
		t.Fatal("an origin with a user name must not slip through")
	}
	if OriginAllowed("null", nil, host) {
		t.Fatal("an origin of null must be refused")
	}
}

func TestParseOriginAllowlist(t *testing.T) {
	got := ParseOriginAllowlist("https://a.example, https://b.example")
	if len(got) != 2 || got[0] != "https://a.example" || got[1] != "https://b.example" {
		t.Fatalf("unexpected allowlist: %v", got)
	}
	if len(ParseOriginAllowlist("")) != 0 {
		t.Fatal("an empty value means no origins")
	}
	if len(ParseOriginAllowlist(" , ")) != 0 {
		t.Fatal("blank entries are dropped")
	}
}

func TestParseHubSecrets(t *testing.T) {
	got := ParseHubSecrets("home:secret-one, kitchen:secret-two")
	if len(got) != 2 || got["home"] != "secret-one" || got["kitchen"] != "secret-two" {
		t.Fatalf("unexpected secrets: %v", got)
	}
	if len(ParseHubSecrets("")) != 0 {
		t.Fatal("an empty value means no hub has a secret of its own")
	}
	if len(ParseHubSecrets("home:")) != 0 {
		t.Fatal("a hub without a secret is not a hub with a secret")
	}
	if len(ParseHubSecrets("bad id:secret")) != 0 {
		t.Fatal("an id with a space is not a hub id")
	}
	if len(ParseHubSecrets("home-without-a-secret")) != 0 {
		t.Fatal("an entry without a colon is not a pair")
	}
	// The secret itself may contain a colon: only the first one separates.
	withColon := ParseHubSecrets("home:abc:def")
	if len(withColon) != 1 || withColon["home"] != "abc:def" {
		t.Fatalf("unexpected secret: %v", withColon)
	}
}

func TestAuthLimiterBlocksAfterTooManyFailures(t *testing.T) {
	l := NewAuthLimiter(3, time.Minute, time.Minute)
	now := time.Unix(1_000_000, 0)
	if l.Blocked("1.2.3.4", now) {
		t.Fatal("a fresh source is not blocked")
	}
	l.Fail("1.2.3.4", now)
	l.Fail("1.2.3.4", now)
	if l.Blocked("1.2.3.4", now) {
		t.Fatal("two failures must not block yet")
	}
	l.Fail("1.2.3.4", now)
	if !l.Blocked("1.2.3.4", now) {
		t.Fatal("the third failure must block")
	}
}

func TestAuthLimiterCountsEachSourceSeparately(t *testing.T) {
	l := NewAuthLimiter(2, time.Minute, time.Minute)
	now := time.Unix(1_000_000, 0)
	l.Fail("a", now)
	l.Fail("a", now)
	if !l.Blocked("a", now) {
		t.Fatal("the source must be blocked")
	}
	if l.Blocked("b", now) {
		t.Fatal("another source is unaffected")
	}
}

func TestAuthLimiterForgetsAfterSuccess(t *testing.T) {
	l := NewAuthLimiter(2, time.Minute, time.Minute)
	now := time.Unix(1_000_000, 0)
	l.Fail("a", now)
	l.Success("a")
	l.Fail("a", now)
	if l.Blocked("a", now) {
		t.Fatal("a successful authentication clears the counter")
	}
}

func TestAuthLimiterLetsASourceBackIn(t *testing.T) {
	l := NewAuthLimiter(1, time.Minute, 5*time.Second)
	now := time.Unix(1_000_000, 0)
	l.Fail("a", now)
	if !l.Blocked("a", now) {
		t.Fatal("the first failure blocks when the limit is one")
	}
	if l.Blocked("a", now.Add(6*time.Second)) {
		t.Fatal("the block expires")
	}
}

func TestAuthLimiterForgetsOldFailures(t *testing.T) {
	l := NewAuthLimiter(2, time.Second, time.Minute)
	now := time.Unix(1_000_000, 0)
	l.Fail("a", now)
	l.Fail("a", now.Add(2*time.Second)) // the window passed: a fresh first failure
	if l.Blocked("a", now.Add(2*time.Second)) {
		t.Fatal("failures outside the window do not count")
	}
}

func TestAuthLimiterSweeps(t *testing.T) {
	l := NewAuthLimiter(2, time.Second, time.Minute)
	now := time.Unix(1_000_000, 0)
	l.Fail("a", now)
	l.Sweep(now.Add(10 * time.Second))
	if l.Blocked("a", now.Add(10*time.Second)) {
		t.Fatal("a swept entry is forgotten")
	}
}

func TestStatusPageBody(t *testing.T) {
	counts := map[string]int{"home": 2}
	body := StatusPageBody([]string{"home"}, counts, false, "relay.example.com")
	if !contains(body, "1 hub(s) connected") {
		t.Fatalf("the counters must be there: %q", body)
	}
	if contains(body, "home") {
		t.Fatalf("without a token the hub ids must be hidden: %q", body)
	}
	body = StatusPageBody([]string{"home"}, counts, true, "relay.example.com")
	if !contains(body, "home: 2 client(s)") {
		t.Fatalf("the token holder sees the ids: %q", body)
	}
	// The address the app has to use - a person copies it from here.
	if !contains(body, "wss://relay.example.com/client/home") {
		t.Fatalf("the page must say where the app points: %q", body)
	}
}

func TestStatusTokenOK(t *testing.T) {
	if !StatusTokenOK("", "") {
		t.Fatal("without a configured token anything goes")
	}
	if StatusTokenOK("", "t0ken") {
		t.Fatal("a missing token must be refused")
	}
	if StatusTokenOK("wrong", "t0ken") {
		t.Fatal("a wrong token must be refused")
	}
	if !StatusTokenOK("t0ken", "t0ken") {
		t.Fatal("the right token must pass")
	}
}

func TestTheLimits(t *testing.T) {
	if MaxFrameBytes <= 4*1024 {
		t.Fatal("a frame must have room for a whole snapshot part")
	}
	if MaxBufferedBytes > 1024*1024 {
		t.Fatal("the buffer must stay small")
	}
}

func contains(haystack, needle string) bool {
	return len(haystack) >= len(needle) && (func() bool {
		for i := 0; i+len(needle) <= len(haystack); i++ {
			if haystack[i:i+len(needle)] == needle {
				return true
			}
		}
		return false
	})()
}
