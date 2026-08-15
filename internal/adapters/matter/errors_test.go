package matter

import (
	"errors"
	"fmt"
	"testing"
)

// Callers must be able to branch on the category without reading messages.
func TestRPCErrorKindAndSentinels(t *testing.T) {
	cases := []struct {
		name      string
		err       *RPCError
		wantKind  ErrorKind
		sentinel  error
		retryable bool
	}{
		{
			name:     "explicit kind from a taxonomy-aware sidecar",
			err:      &RPCError{Code: RPCCodeNotFound, Message: "node 7 not found", ErrKind: ErrKindNotFound},
			wantKind: ErrKindNotFound, sentinel: ErrNotFound, retryable: false,
		},
		{
			name:     "kind derived from code when the sidecar predates the field",
			err:      &RPCError{Code: RPCCodeNotReady, Message: "controller not started"},
			wantKind: ErrKindNotReady, sentinel: ErrNotReady, retryable: true,
		},
		{
			name:     "unreachable peer is worth retrying",
			err:      &RPCError{Code: RPCCodeUnreachable, Message: "no channel", ErrKind: ErrKindUnreachable},
			wantKind: ErrKindUnreachable, sentinel: ErrUnreachable, retryable: true,
		},
		{
			name:     "unsupported command is not",
			err:      &RPCError{Code: RPCCodeUnsupported, Message: "no such command", ErrKind: ErrKindUnsupported},
			wantKind: ErrKindUnsupported, sentinel: ErrUnsupported, retryable: false,
		},
		{
			name:     "explicit retryable flag overrides the kind default",
			err:      &RPCError{Code: RPCCodeInternal, Message: "transient", ErrKind: ErrKindInternal, Retryable: true},
			wantKind: ErrKindInternal, sentinel: ErrInternal, retryable: true,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := tc.err.Kind(); got != tc.wantKind {
				t.Errorf("kind = %q want %q", got, tc.wantKind)
			}
			if got := tc.err.IsRetryable(); got != tc.retryable {
				t.Errorf("retryable = %v want %v", got, tc.retryable)
			}
			// Must survive the wrapping every adapter method applies.
			wrapped := fmt.Errorf("matter: readAttribute OnOff.OnOff: %w", error(tc.err))
			if !errors.Is(wrapped, tc.sentinel) {
				t.Errorf("errors.Is(wrapped, %v) = false", tc.sentinel)
			}
			if got := KindOf(wrapped); got != tc.wantKind {
				t.Errorf("KindOf(wrapped) = %q want %q", got, tc.wantKind)
			}
			if got := IsRetryable(wrapped); got != tc.retryable {
				t.Errorf("IsRetryable(wrapped) = %v want %v", got, tc.retryable)
			}
		})
	}
}

func TestRPCErrorDoesNotMatchForeignSentinel(t *testing.T) {
	err := error(&RPCError{Code: RPCCodeNotFound, ErrKind: ErrKindNotFound, Message: "gone"})
	if errors.Is(err, ErrTimeout) {
		t.Error("a not_found error must not match ErrTimeout")
	}
	if KindOf(errors.New("plain error")) != "" {
		t.Error("a non-RPC error has no kind")
	}
}

// A call that never reached the sidecar is retryable once it reconnects.
func TestIsRetryableTransportErrors(t *testing.T) {
	if !IsRetryable(fmt.Errorf("commission: %w", ErrSidecarUnavailable)) {
		t.Error("ErrSidecarUnavailable should be retryable")
	}
	if !IsRetryable(fmt.Errorf("call: %w", ErrNotConnected)) {
		t.Error("ErrNotConnected should be retryable")
	}
	if IsRetryable(errors.New("something else")) {
		t.Error("unknown errors should not be reported as retryable")
	}
}
