package supervisor

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"sync"
	"syscall"
	"time"

	"github.com/kliuchnikovv/keystone-api/sidecar"
)

// ProcessConfig configures a bare supervised process — no socket, no
// handshake, just fork+wait+restart+log. Used both for a plugin's
// entrypoint (wrapped by Supervisor with the sidecar handshake) and for
// each declared sidecar in the manifest.
type ProcessConfig struct {
	Label   string // human-facing name for logs, e.g. "matter" or "matter/matter-server"
	Exec    []string
	WorkDir string
	Env     []string
	Restart RestartPolicy
	Logger  *slog.Logger

	OnStdout func(line string)
	OnStderr func(line string)

	// StopTimeout bounds a graceful stop before SIGKILL.
	StopTimeout time.Duration
}

// Process supervises exactly one child. Start returns once the child is
// running; the watcher runs in the background and restarts per policy.
type Process struct {
	cfg ProcessConfig
	log *slog.Logger

	life       context.Context
	lifeCancel context.CancelFunc

	mu      sync.Mutex
	cmd     *exec.Cmd
	proc    *os.Process
	stopped bool
	err     error

	done chan struct{}
}

// NewProcess builds a Process. Start actually launches it.
func NewProcess(cfg ProcessConfig) (*Process, error) {
	if len(cfg.Exec) == 0 {
		return nil, errors.New("supervisor: ProcessConfig.Exec is required")
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
	label := cfg.Label
	if label == "" {
		label = cfg.Exec[0]
	}
	return &Process{
		cfg:  cfg,
		log:  cfg.Logger.With("process", label),
		done: make(chan struct{}),
	}, nil
}

// Start forks the child and returns once it is running.
func (p *Process) Start() error {
	p.life, p.lifeCancel = context.WithCancel(context.Background())
	if err := p.spawn(); err != nil {
		p.lifeCancel()
		return err
	}
	go p.watch()
	return nil
}

// Stop signals the child and waits for the watcher to exit. Idempotent.
func (p *Process) Stop() error {
	p.mu.Lock()
	if p.stopped {
		p.mu.Unlock()
		return nil
	}
	p.stopped = true
	proc := p.proc
	p.mu.Unlock()

	if p.lifeCancel != nil {
		p.lifeCancel()
	}
	if proc != nil {
		p.terminate(proc)
	}
	<-p.done
	return p.err
}

// PID returns the current child's PID, or 0 if none is running.
func (p *Process) PID() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.proc == nil {
		return 0
	}
	return p.proc.Pid
}

func (p *Process) spawn() error {
	cmd := exec.CommandContext(p.life, p.cfg.Exec[0], p.cfg.Exec[1:]...)
	cmd.Dir = p.cfg.WorkDir
	cmd.Env = append(os.Environ(), p.cfg.Env...)
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
		return fmt.Errorf("supervisor: starting %s: %w", p.cfg.Exec[0], err)
	}

	p.mu.Lock()
	p.cmd = cmd
	p.proc = cmd.Process
	p.mu.Unlock()

	go pumpLines(stdout, p.cfg.OnStdout)
	go pumpLines(stderr, p.cfg.OnStderr)

	p.log.Info("process started", "pid", cmd.Process.Pid)
	return nil
}

func (p *Process) watch() {
	defer close(p.done)
	backoff := sidecar.NewBackoff()

	for {
		p.mu.Lock()
		cmd := p.cmd
		p.cmd = nil
		p.mu.Unlock()
		if cmd == nil {
			return
		}
		waitErr := cmd.Wait()

		p.mu.Lock()
		p.proc = nil
		stopped := p.stopped
		p.mu.Unlock()
		if stopped {
			return
		}

		msg := "ok"
		if waitErr != nil {
			msg = waitErr.Error()
		}
		p.log.Warn("process exited", "err", msg, "restart", string(p.cfg.Restart))

		if !shouldRestart(p.cfg.Restart, waitErr) {
			p.err = waitErr
			return
		}
		if !backoff.Wait(p.life) {
			p.err = p.life.Err()
			return
		}
		if err := p.spawn(); err != nil {
			p.log.Error("respawn failed", "err", err)
			p.err = err
			return
		}
	}
}

func (p *Process) terminate(proc *os.Process) {
	_ = proc.Signal(syscall.SIGTERM)
	deadline := time.After(p.cfg.StopTimeout)
	tick := time.NewTicker(50 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-deadline:
			_ = proc.Kill()
			return
		case <-tick.C:
			p.mu.Lock()
			alive := p.proc != nil
			p.mu.Unlock()
			if !alive {
				return
			}
		}
	}
}

func shouldRestart(policy RestartPolicy, err error) bool {
	switch policy {
	case RestartAlways:
		return true
	case RestartOnFailure:
		return err != nil
	default:
		return false
	}
}

func pumpLines(r io.Reader, sink func(string)) {
	if sink == nil {
		_, _ = io.Copy(io.Discard, r)
		return
	}
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 4096), 1<<20)
	for sc.Scan() {
		sink(sc.Text())
	}
	_ = sc.Err()
}
