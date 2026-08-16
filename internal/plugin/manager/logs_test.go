package manager

import (
	"context"
	"testing"
	"time"
)

func TestLogBuffer_TailReturnsLastN(t *testing.T) {
	b := newLogBuffer(4)
	for i := 0; i < 6; i++ {
		b.push(LogLine{Line: string(rune('a' + i))})
	}
	// After 6 pushes into a cap-4 ring, tail(3) should return c, d, e, f — last 3.
	got := b.tail(3)
	if len(got) != 3 {
		t.Fatalf("want 3, got %d", len(got))
	}
	if got[0].Line != "d" || got[2].Line != "f" {
		t.Fatalf("wrong window: %+v", got)
	}
}

func TestLogBuffer_Subscribe(t *testing.T) {
	b := newLogBuffer(8)
	ch, cancel := b.subscribe()
	defer cancel()

	b.push(LogLine{Line: "one"})
	select {
	case l := <-ch:
		if l.Line != "one" {
			t.Fatalf("got %v", l)
		}
	case <-time.After(time.Second):
		t.Fatal("no line delivered")
	}
}

func TestManager_LogsAndFollow(t *testing.T) {
	m := &Manager{logs: map[string]*logBuffer{}}
	sink := m.captureLine("stub", "stdout")
	sink("hello")
	sink("world")

	got := m.Logs("stub", 10)
	if len(got) != 2 || got[0].Line != "hello" || got[1].Line != "world" {
		t.Fatalf("wrong buffer: %+v", got)
	}

	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	ch, err := m.Follow(ctx, "stub")
	if err != nil {
		t.Fatal(err)
	}
	sink("live")
	select {
	case l := <-ch:
		if l.Line != "live" {
			t.Fatalf("got %v", l)
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("live line not delivered")
	}
}
