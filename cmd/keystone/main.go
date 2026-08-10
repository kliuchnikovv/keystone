// keystone is the smart-home engine daemon.
//
// POC day-5: adds SQLite-alternative JSON persistence, rules engine (time,
// state, threshold, event triggers; time-range + state-equals conditions;
// invoke_action + set_state + delay actions), and a live state-stream
// endpoint (NDJSON over long-lived HTTP; will migrate to WebSocket later).
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/keystone/keystone/internal/adapters/virtual"
	"github.com/keystone/keystone/internal/api/ws"
	"github.com/keystone/keystone/internal/domain"
	"github.com/keystone/keystone/internal/eventbus"
	"github.com/keystone/keystone/internal/ports"
	"github.com/keystone/keystone/internal/registry"
	"github.com/keystone/keystone/internal/rules"
	"github.com/keystone/keystone/internal/service"
	"github.com/keystone/keystone/internal/storage/file"
)

func main() {
	addr := flag.String("addr", ":7777", "HTTP admin listen address")
	dataDir := flag.String("data", "./keystone-data", "data directory for persistence")
	demo := flag.Bool("demo", true, "seed a virtual demo scene on first run")
	flag.Parse()

	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(log)

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	// --- Storage ---
	repo, err := file.Open(*dataDir)
	if err != nil {
		log.Error("open repo", "err", err)
		os.Exit(1)
	}
	log.Info("storage opened", "dir", *dataDir)

	// --- Core primitives ---
	reg := registry.New()
	bus := eventbus.New(log.With("component", "eventbus"))
	defer bus.Close()

	// Rehydrate devices from disk into the in-memory registry.
	stored, err := repo.ListDevices(ctx)
	if err != nil {
		log.Error("list stored devices", "err", err)
		os.Exit(1)
	}
	for _, d := range stored {
		if err := reg.Add(d); err != nil {
			log.Error("rehydrate device", "id", d.ID, "err", err)
		}
	}
	log.Info("devices rehydrated", "count", len(stored))

	// --- Adapters ---
	virt := virtual.New(log.With("component", "virtual"))
	if err := virt.Start(ctx); err != nil {
		log.Error("virtual adapter start", "err", err)
		os.Exit(1)
	}
	defer func() { _ = virt.Stop(context.Background()) }()

	devSvc := service.NewDeviceService(log, reg, bus, []ports.Adapter{virt})

	// Wrap device commissioning to also persist to disk.
	persistOnAdd := func(d *domain.Device) {
		if err := repo.SaveDevice(context.Background(), d); err != nil {
			log.Error("persist device", "id", d.ID, "err", err)
		}
	}

	// Ingress: transport events -> event bus + registry.
	go func() {
		if err := devSvc.IngressLoop(ctx, virt); err != nil && !errors.Is(err, context.Canceled) {
			log.Error("virtual ingress loop", "err", err)
		}
	}()

	// State logger: prints every state change.
	go func() {
		stateCh := bus.SubscribeStates(ctx)
		for s := range stateCh {
			log.Debug("state changed",
				"device_id", s.DeviceID, "feature", s.Feature, "key", s.Key,
				"value", s.Value, "origin", s.Origin)
		}
	}()

	// --- Rules engine ---
	engine := rules.New(rules.Config{
		Log:      log.With("component", "rules"),
		Repo:     repo,
		Reader:   reg,
		Executor: devSvc,
		States:   bus.SubscribeStates(ctx),
		Events:   bus.SubscribeEvents(ctx),
		Workers:  4,
	})
	if err := engine.LoadFromRepo(ctx); err != nil {
		log.Error("load rules", "err", err)
		os.Exit(1)
	}
	go func() {
		if err := engine.Start(ctx); err != nil && !errors.Is(err, context.Canceled) {
			log.Error("rules engine", "err", err)
		}
	}()

	// --- Demo seeding on first run ---
	if *demo && len(stored) == 0 {
		if err := seedDemoScene(ctx, devSvc, engine, persistOnAdd); err != nil {
			log.Error("seed demo scene", "err", err)
			os.Exit(1)
		}
	}
	// Always start the INSPELNING simulator (nice for testing thresholds).
	go simulateInspelning(ctx, devSvc, virt, log)

	// --- HTTP API ---
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok"})
	})
	mux.HandleFunc("GET /devices", handleListDevices(devSvc))
	mux.HandleFunc("GET /devices/{id}", handleGetDevice(devSvc))
	mux.HandleFunc("POST /devices/{id}/actions", handleInvokeAction(devSvc))
	mux.HandleFunc("POST /devices/{id}/state", handleWriteState(devSvc))
	mux.HandleFunc("GET /rules", handleListRules(engine))
	mux.HandleFunc("POST /rules", handleUpsertRule(engine))
	mux.HandleFunc("DELETE /rules/{id}", handleDeleteRule(engine))
	mux.HandleFunc("GET /rules/runs", handleListRuns(engine))
	mux.HandleFunc("GET /stream", ws.Handler(bus, log.With("component", "ws")))

	srv := &http.Server{
		Addr:              *addr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		log.Info("http listening", "addr", *addr)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Error("http", "err", err)
			cancel()
		}
	}()

	<-ctx.Done()
	log.Info("shutting down")
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()
	_ = srv.Shutdown(shutdownCtx)
}

// seedDemoScene commissions two virtual plugs, persists them, and creates one
// demo automation exercising a threshold trigger + delay + turn_off.
func seedDemoScene(ctx context.Context, svc *service.DeviceService, eng *rules.Engine, persist func(*domain.Device)) error {
	specs := []struct {
		payload string
		name    string
		typ     domain.DeviceType
	}{
		{"plug:Kitchen kettle (INSPELNING clone)", "Kitchen kettle", domain.DeviceTypePlug},
		{"plug:Bedroom lamp (TRETAKT clone)", "Bedroom lamp", domain.DeviceTypePlug},
	}
	var kettleID, lampID domain.DeviceID
	for _, s := range specs {
		d, err := svc.Commission(ctx, domain.TransportVirtual,
			ports.CommissionRequest{Payload: s.payload}, s.name, s.typ)
		if err != nil {
			return fmt.Errorf("commission %q: %w", s.name, err)
		}
		persist(d)
		if strings.HasPrefix(s.name, "Kitchen") {
			kettleID = d.ID
		} else {
			lampID = d.ID
		}
	}

	// Demo rule: if the kettle draws more than 1500W (i.e. is actively
	// heating), turn the bedroom lamp on — a whimsical "kettle-triggered
	// ambient light".
	demoRule := &domain.Rule{
		Name:          "Kettle -> Bedroom lamp",
		HumanReadable: "Когда чайник тянет больше 1.5 кВт — включи лампу в спальне",
		Enabled:       true,
		Mode:          domain.RunModeSingle,
		Triggers: []domain.Trigger{
			&rules.ThresholdTrigger{
				DeviceID: kettleID,
				Feature:  domain.FeaturePowerMeter,
				Key:      domain.StatePowerNow,
				Op:       "gt",
				Value:    1500,
			},
		},
		Actions: []domain.RuleAction{
			&rules.InvokeAction{
				DeviceID: lampID,
				Feature:  domain.FeatureOnOff,
				Action:   domain.ActionTurnOn,
			},
		},
	}
	return eng.Upsert(ctx, demoRule)
}

// simulateInspelning generates a sawtooth power draw on the first plug so
// the event bus and rules engine have something to react to.
func simulateInspelning(ctx context.Context, svc *service.DeviceService, virt *virtual.Adapter, log *slog.Logger) {
	var target domain.TransportRef
	for _, d := range svc.List() {
		if d.Transport == domain.TransportVirtual && d.Type == domain.DeviceTypePlug {
			target = d.TransportRef
			break
		}
	}
	if target == "" {
		return
	}

	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()
	var watts float32
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			watts += 200
			if watts > 2000 {
				watts = 0
			}
			virt.Inject(target, domain.FeaturePowerMeter, domain.StatePowerNow, watts)
		}
	}
}

// --- Device handlers ---

func handleListDevices(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"devices": svc.List()})
	}
}

func handleGetDevice(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.DeviceID(r.PathValue("id"))
		d, err := svc.Get(id)
		if err != nil {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, d)
	}
}

type invokeActionRequest struct {
	Feature string         `json:"feature"`
	Action  string         `json:"action"`
	Params  map[string]any `json:"params,omitempty"`
}

func handleInvokeAction(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.DeviceID(r.PathValue("id"))
		var req invokeActionRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		if strings.TrimSpace(req.Feature) == "" || strings.TrimSpace(req.Action) == "" {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": "feature and action required"})
			return
		}
		err := svc.InvokeAction(r.Context(), id,
			domain.FeatureKey(req.Feature), domain.ActionKey(req.Action), req.Params)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
	}
}

type writeStateRequest struct {
	Feature string `json:"feature"`
	Key     string `json:"key"`
	Value   any    `json:"value"`
}

func handleWriteState(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.DeviceID(r.PathValue("id"))
		var req writeStateRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		err := svc.WriteState(r.Context(), id,
			domain.FeatureKey(req.Feature), domain.StateKey(req.Key), req.Value)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
	}
}

// --- Rule handlers ---

func handleListRules(eng *rules.Engine) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"rules": eng.List()})
	}
}

func handleUpsertRule(eng *rules.Engine) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var rule domain.Rule
		if err := json.NewDecoder(r.Body).Decode(&rule); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		if err := eng.Upsert(r.Context(), &rule); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, rule)
	}
}

func handleDeleteRule(eng *rules.Engine) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.RuleID(r.PathValue("id"))
		if err := eng.Remove(r.Context(), id); err != nil {
			writeJSON(w, http.StatusNotFound, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
	}
}

func handleListRuns(eng *rules.Engine) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"runs": eng.Runs(50)})
	}
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("content-type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
