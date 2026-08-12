package registry

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// TestKeyedLock_SerialisesSameKey verifies that two goroutines contending on
// the same DeviceID execute strictly one at a time.
func TestKeyedLock_SerialisesSameKey(t *testing.T) {
	kl := NewKeyedLock()
	id := domain.DeviceID("device-a")

	var running int32
	var maxObserved int32
	var wg sync.WaitGroup

	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			release := kl.Acquire(id)
			defer release()

			n := atomic.AddInt32(&running, 1)
			if n > atomic.LoadInt32(&maxObserved) {
				atomic.StoreInt32(&maxObserved, n)
			}
			time.Sleep(1 * time.Millisecond)
			atomic.AddInt32(&running, -1)
		}()
	}
	wg.Wait()

	if got := atomic.LoadInt32(&maxObserved); got != 1 {
		t.Fatalf("expected at most 1 concurrent holder for same key, saw %d", got)
	}
}

// TestKeyedLock_DifferentKeysParallel verifies distinct DeviceIDs do not
// block each other.
func TestKeyedLock_DifferentKeysParallel(t *testing.T) {
	kl := NewKeyedLock()

	var running int32
	var maxObserved int32
	var wg sync.WaitGroup

	for i := 0; i < 10; i++ {
		id := domain.DeviceID("device-" + string(rune('a'+i)))
		wg.Add(1)
		go func() {
			defer wg.Done()
			release := kl.Acquire(id)
			defer release()

			n := atomic.AddInt32(&running, 1)
			if n > atomic.LoadInt32(&maxObserved) {
				atomic.StoreInt32(&maxObserved, n)
			}
			time.Sleep(20 * time.Millisecond)
			atomic.AddInt32(&running, -1)
		}()
	}
	wg.Wait()

	if got := atomic.LoadInt32(&maxObserved); got < 2 {
		t.Fatalf("expected parallelism across distinct keys, saw only %d concurrent", got)
	}
}
