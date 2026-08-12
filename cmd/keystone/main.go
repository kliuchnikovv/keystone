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

	"github.com/kliuchnikovv/keystone/internal/adapters/matter"
	"github.com/kliuchnikovv/keystone/internal/adapters/virtual"
	"github.com/kliuchnikovv/keystone/internal/api/ws"
	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/eventbus"
	"github.com/kliuchnikovv/keystone/internal/ports"
	"github.com/kliuchnikovv/keystone/internal/registry"
	"github.com/kliuchnikovv/keystone/internal/rules"
	"github.com/kliuchnikovv/keystone/internal/service"
	"github.com/kliuchnikovv/keystone/internal/storage/file"
)

func main() {
	addr := flag.String("addr", ":7777", "HTTP admin listen address")
	grpcAddr := flag.String("grpc-addr", "", "gRPC listen address (empty = disabled); requires binary built with -tags=grpc")
	dataDir := flag.String("data", "./keystone-data", "data directory for persistence")
	uiDir := flag.String("ui-dir", "./site", "directory served at /ui/ (dashboard + landing); empty to disable")
	demo := flag.Bool("demo", true, "seed a virtual demo scene on first run")
	matterAddr := flag.String("matter-sidecar", "", "matter.js sidecar URL (e.g. ws://localhost:5580); empty = disabled")
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
	adapters := []ports.Adapter{}

	virt := virtual.New(log.With("component", "virtual"))
	if err := virt.Start(ctx); err != nil {
		log.Error("virtual adapter start", "err", err)
		os.Exit(1)
	}
	defer func() { _ = virt.Stop(context.Background()) }()
	adapters = append(adapters, virt)

	// Matter over a matter.js sidecar. Optional — if -matter-sidecar is empty
	// or the sidecar is unreachable at boot, we log the reason and continue
	// without Matter so keystone still starts on a bare host.
	if *matterAddr != "" {
		cfg, err := parseMatterURL(*matterAddr)
		if err != nil {
			log.Error("matter sidecar url", "value", *matterAddr, "err", err)
			os.Exit(1)
		}
		wsc := matter.NewWSClient(cfg.URL(), log.With("component", "matter-ws"))
		mad := matter.New(log.With("component", "matter"), cfg, wsc)
		if err := mad.Start(ctx); err != nil {
			log.Warn("matter adapter start failed, continuing without matter", "err", err)
		} else {
			defer func() { _ = mad.Stop(context.Background()) }()
			adapters = append(adapters, mad)
		}
	}

	devSvc := service.NewDeviceService(log, reg, bus, adapters)

	// Wrap device commissioning to also persist to disk.
	persistOnAdd := func(d *domain.Device) {
		if err := repo.SaveDevice(context.Background(), d); err != nil {
			log.Error("persist device", "id", d.ID, "err", err)
		}
	}

	// Ingress: transport events -> event bus + registry. One goroutine per adapter.
	for _, ad := range adapters {
		ad := ad
		go func() {
			if err := devSvc.IngressLoop(ctx, ad); err != nil && !errors.Is(err, context.Canceled) {
				log.Error("ingress loop", "transport", ad.Kind(), "err", err)
			}
		}()
	}

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

	// Static UI (dashboard for testing, landing page).
	// Served from a directory on disk so it can be edited without rebuilding.
	if *uiDir != "" {
		if _, statErr := os.Stat(*uiDir); statErr == nil {
			ui := http.StripPrefix("/ui/", http.FileServer(http.Dir(*uiDir)))
			mux.Handle("GET /ui/", ui)
			log.Info("ui served", "dir", *uiDir, "url", "http://"+*addr+"/ui/dashboard.html")
		} else {
			log.Warn("ui dir missing, /ui/ disabled", "dir", *uiDir)
		}
	}

	// Optional gRPC server (compiled in only with -tags=grpc).
	startGRPC(ctx, log, *grpcAddr, devSvc, reg, bus, engine)

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
		{"light:WARMBLIXT (Matter clone)", "WARMBLIXT", domain.DeviceTypeLight},
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

// simulateInspelning generates a sawtooth power draw on the kettle plug so
// the event bus and rules engine have something to react to. It prefers the
// device explicitly named "Kitchen kettle" (which is the target of the demo
// rule); falls back to the first plug found otherwise.
func simulateInspelning(ctx context.Context, svc *service.DeviceService, virt *virtual.Adapter, log *slog.Logger) {
	var target domain.TransportRef
	var firstPlug domain.TransportRef
	for _, d := range svc.List() {
		if d.Transport != domain.TransportVirtual || d.Type != domain.DeviceTypePlug {
			continue
		}
		if firstPlug == "" {
			firstPlug = d.TransportRef
		}
		if strings.HasPrefix(d.Name, "Kitchen") {
			target = d.TransportRef
			break
		}
	}
	if target == "" {
		target = firstPlug
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

// parseMatterURL accepts either a bare host:port or a full ws:// URL and
// returns a matter.Config the adapter can dial.
func parseMatterURL(raw string) (matter.Config, error) {
	s := strings.TrimSpace(raw)
	s = strings.TrimPrefix(s, "ws://")
	s = strings.TrimPrefix(s, "wss://")
	host, portStr, ok := strings.Cut(s, ":")
	if !ok || host == "" || portStr == "" {
		return matter.Config{}, fmt.Errorf("expected host:port or ws://host:port, got %q", raw)
	}
	var port int
	if _, err := fmt.Sscanf(portStr, "%d", &port); err != nil || port <= 0 {
		return matter.Config{}, fmt.Errorf("invalid port in %q", raw)
	}
	return matter.Config{Host: host, Port: port}, nil
}
