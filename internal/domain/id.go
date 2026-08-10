package domain

import (
	"crypto/rand"
	"encoding/hex"
	"time"
)

// NewID returns a time-ordered opaque identifier suitable for DeviceID, RoomID,
// and rule IDs. Format: 8 hex chars of UNIX-ms + 16 hex chars of randomness.
// Not a UUIDv7 but has the same "sortable + collision-resistant" properties
// with zero external dependencies.
func NewID() string {
	ms := uint64(time.Now().UnixMilli())
	tsHex := make([]byte, 8)
	for i := 7; i >= 0; i-- {
		tsHex[i] = "0123456789abcdef"[ms&0xf]
		ms >>= 4
	}
	var rnd [8]byte
	_, _ = rand.Read(rnd[:])
	return string(tsHex) + "-" + hex.EncodeToString(rnd[:])
}
