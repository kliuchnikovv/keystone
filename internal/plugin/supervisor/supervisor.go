// Package supervisor manages the lifecycle of a plugin child process.
//
// The core listens on a Unix socket, then forks the plugin binary with
// KEYSTONE_PLUGIN_SOCKET pointing at that socket. sidecar.Client owns the
// wire — it dials, handshakes, and reconnects on its own. Supervisor owns
// the child: it captures its logs, waits on it, and restarts it per policy
// while the sidecar client transparently reconnects.
package supervisor

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"time"

	"github.com/kliuchnikovv/keystone-api/sidecar"
)

// RestartPolicy mirrors manifest.spec.entrypoint.restart.
type RestartPolicy string

const (
	RestartNever     RestartPolicy = "never"
	RestartOnFailure RestartPolicy = "on-failure"
	RestartAlways    RestartPolicy = "always"
)

// Config configures a Supervisor.
type Config struct {
	// Name and Version identify the plugin. Used for logging and
	// sidecar handshake feedback only — the manifest is the source of truth.
	Name    string
	Version string

	// Exec is manifest.spec.entrypoint.exec: argv[0] is the binary,
	// resolved relative to WorkDir if not absolute.
	Exec []string

	// WorkDir is the plugin's install directory. The child cd's here.
	WorkDir string

	// Env is added to os.Environ() for the child. KEYSTONE_PLUGIN_SOCKET
	// is set by the supervisor and always overrides.
	Env []string

	// Restart selects the policy on child exit.
	Restart RestartPolicy

	// StopTimeout bounds a graceful shutdown before SIGKILL. Defaults to
	// 5s. sidecar-protocol shutdown negotiation is a plugin-manager concern;
	// the supervisor is one layer down and just kills.
	StopTimeout time.Duration

	// SocketDir is where the UDS is created. Defaults to os.TempDir(). The
	// supervisor creates a unique subdirectory here.
	SocketDir string

	// OnStdout and OnStderr receive each line of child output. Nil discards.
	// Called from the log-pump goroutine.
	OnStdout func(line string)
	OnStderr func(line string)

	// Client wires the core's sidecar options — Handler, Heartbeat, Backoff,
	// etc. The supervisor sets no defaults beyond sidecar's own.
	Client sidecar.CoreOptions

	Logger *slog.Logger
}

// Supervisor supervises one plugin process.
type Supervisor struct {
	cfg    Config
	log    *slog.Logger
	socket string
	dir    string

	// client is stable across restarts — sidecar.Client reconnects itself.
	client *sidecar.Client

	// life is the supervisor's own context, cancelled by Stop. It backs
	// exec.CommandContext and the watch-loop backoff so a plugin's life is
	// tied to the supervisor itself, not to the caller of Start (whose
	// context — often a request context — cancels the moment the HTTP
	// handler returns).
	life       context.Context
	lifeCancel context.CancelFunc

	mu        sync.Mutex
	proc      *os.Process
	stopped   bool
	lastError error

	done chan struct{}
}

// New builds a Supervisor. Start must be called to actually launch anything.
func New(cfg Config) (*Supervisor, error) {
	if len(cfg.Exec) == 0 {
		return nil, errors.New("supervisor: Exec is required")
	}
	if cfg.StopTimeout <= 0 {
		cfg.StopTimeout = 5 * time.Second
	}
	if cfg.Restart == "" {
		cfg.Restart = RestartOnFailure
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	return &Supervisor{
		cfg:  cfg,
		log:  cfg.Logger.With("plugin", cfg.Name),
		done: make(chan struct{}),
	}, nil
}

// Start allocates the socket, spawns the child, and returns a connected
// sidecar client. The returned client stays alive across restarts.
//
// ctx is used only for the first dial to the child — once the handshake
// completes, the plugin lives under the supervisor's own context, which
// only Stop cancels. This decouples plugin life from the request that
// asked to enable it.
func (s *Supervisor) Start(ctx context.Context) (*sidecar.Client, error) {
	s.life, s.lifeCancel = context.WithCancel(context.Background())

	dir, err := os.MkdirTemp(s.cfg.SocketDir, "keystone-plugin-"+sanitize(s.cfg.Name)+"-*")
	if err != nil {
		s.lifeCancel()
		return nil, fmt.Errorf("supervisor: creating socket dir: %w", err)
	}
	s.dir = dir
	s.socket = filepath.Join(dir, "socket")

	if err := s.spawn(); err != nil {
		s.lifeCancel()
		s.cleanupDir()
		return nil, err
	}

	client, err := s.dialWithRetry(ctx)
	if err != nil {
		s.stopChild()
		s.lifeCancel()
		s.cleanupDir()
		return nil, err
	}
	s.client = client

	go s.watch()
	return client, nil
}

// Client returns the shared sidecar client. Nil before Start.
func (s *Supervisor) Client() *sidecar.Client { return s.client }

// Stop terminates the child and closes the sidecar client. Idempotent.
func (s *Supervisor) Stop(_ context.Context) error {
	s.mu.Lock()
	if s.stopped {
		s.mu.Unlock()
		return nil
	}
	s.stopped = true
	s.mu.Unlock()

	if s.lifeCancel != nil {
		s.lifeCancel()
	}
	s.stopChild()
	if s.client != nil {
		_ = s.client.Close()
	}
	<-s.done
	s.cleanupDir()
	return s.lastError
}

// Done closes once the supervisor has fully stopped.
func (s *Supervisor) Done() <-chan struct{} { return s.done }

// spawn launches one child process. Requires s.socket set.
func (s *Supervisor) spawn() error {
	cmd := exec.CommandContext(s.life, s.cfg.Exec[0], s.cfg.Exec[1:]...)
	cmd.Dir = s.cfg.WorkDir
	env := append(os.Environ(), s.cfg.Env...)
	env = append(env, sidecar.SocketEnvVar+"="+s.socket)
	cmd.Env = env
	// Give the child its own process group so a stray Ctrl+C on the parent
	// does not tear down plugins the manager wanted to keep alive.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("supervisor: stdout pipe: %w", err)
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return fmt.Errorf("supervisor: stderr pipe: %w", err)
	}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("supervisor: starting %s: %w", s.cfg.Exec[0], err)
	}

	s.mu.Lock()
	s.proc = cmd.Process
	s.mu.Unlock()

	go pumpLines(stdout, s.cfg.OnStdout)
	go pumpLines(stderr, s.cfg.OnStderr)

	s.log.Info("plugin started", "pid", cmd.Process.Pid, "socket", s.socket)

	// We don't call cmd.Wait here — the child watcher owns it via
	// findProcess/wait. Storing the *exec.Cmd would let us Wait() cleanly.
	// Keep it on the supervisor for the watcher.
	s.setCmd(cmd)
	return nil
}

// watch waits on the child, restarts per policy, and closes s.done when it
// gives up.
func (s *Supervisor) watch() {
	ctx := s.life
	defer close(s.done)
	backoff := sidecar.NewBackoff()

	for {
		cmd := s.takeCmd()
		if cmd == nil {
			return
		}
		err := cmd.Wait()

		s.mu.Lock()
		s.proc = nil
		stopped := s.stopped
		s.mu.Unlock()
		if stopped {
			// A signalled exit after Stop is the expected outcome and must
			// not be reported as an error — callers wait on Stop returning
			// nil to distinguish clean shutdown from an unrelated failure.
			return
		}

		exited := "ok"
		if err != nil {
			exited = err.Error()
		}
		s.log.Warn("plugin exited", "err", exited, "restart", string(s.cfg.Restart))

		if !shouldRestart(s.cfg.Restart, err) {
			s.lastError = err
			return
		}
		if !backoff.Wait(ctx) {
			s.lastError = ctx.Err()
			return
		}
		if err := s.spawn(); err != nil {
			s.log.Error("plugin respawn failed", "err", err)
			s.lastError = err
			return
		}
	}
}

func (s *Supervisor) dialWithRetry(ctx context.Context) (*sidecar.Client, error) {
	deadline := time.Now().Add(5 * time.Second)
	var lastErr error
	for time.Now().Before(deadline) {
		c, err := sidecar.Connect(ctx, s.socket, s.cfg.Client)
		if err == nil {
			return c, nil
		}
		lastErr = err
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(50 * time.Millisecond):
		}
	}
	return nil, fmt.Errorf("supervisor: plugin did not accept within timeout: %w", lastErr)
}

func (s *Supervisor) stopChild() {
	s.mu.Lock()
	proc := s.proc
	s.mu.Unlock()
	if proc == nil {
		return
	}
	_ = proc.Signal(syscall.SIGTERM)

	deadline := time.After(s.cfg.StopTimeout)
	tick := time.NewTicker(50 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-deadline:
			_ = proc.Kill()
			return
		case <-tick.C:
			s.mu.Lock()
			alive := s.proc != nil
			s.mu.Unlock()
			if !alive {
				return
			}
		}
	}
}

func (s *Supervisor) cleanupDir() {
	if s.dir != "" {
		_ = os.RemoveAll(s.dir)
	}
}

// setCmd/takeCmd hand the current *exec.Cmd from spawn to watch. Kept behind
// helpers so the watcher can nil it out atomically once Wait returns.
type cmdSlot struct {
	mu  sync.Mutex
	cmd *exec.Cmd
}

var cmdSlots sync.Map // *Supervisor -> *cmdSlot

func (s *Supervisor) setCmd(c *exec.Cmd) {
	slot, _ := cmdSlots.LoadOrStore(s, &cmdSlot{})
	cs := slot.(*cmdSlot)
	cs.mu.Lock()
	cs.cmd = c
	cs.mu.Unlock()
}

func (s *Supervisor) takeCmd() *exec.Cmd {
	slot, _ := cmdSlots.LoadOrStore(s, &cmdSlot{})
	cs := slot.(*cmdSlot)
	cs.mu.Lock()
	c := cs.cmd
	cs.cmd = nil
	cs.mu.Unlock()
	return c
}

func sanitize(s string) string {
	if s == "" {
		return "unnamed"
	}
	out := make([]byte, 0, len(s))
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '-', c == '_':
			out = append(out, c)
		default:
			out = append(out, '_')
		}
	}
	return string(out)
}
