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
	pluginsapi "github.com/kliuchnikovv/keystone/internal/api/plugins"
	"github.com/kliuchnikovv/keystone/internal/api/ws"
	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/eventbus"
	"github.com/kliuchnikovv/keystone/internal/plugin/manager"
	pluginregistry "github.com/kliuchnikovv/keystone/internal/plugin/registry"
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
	pluginsDir := flag.String("plugins-dir", "./keystone-data/plugins", "directory scanned for installed plugins; empty = disabled")
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

	// Sync devices already present in transports (e.g. Matter peers persisted
	// in the matter.js fabric across restarts) into the domain registry so
	// they show up in GET /devices without user intervention. Persist newly
	// synced devices to disk so subsequent boots see them from storage.
	go func() {
		syncCtx, syncCancel := context.WithTimeout(ctx, 30*time.Second)
		defer syncCancel()
		added, updated, err := devSvc.SyncFromAdapters(syncCtx)
		if err != nil {
			log.Warn("adapter sync had errors", "err", err)
		}
		for _, d := range added {
			persistOnAdd(d)
		}
		for _, d := range updated {
			persistOnAdd(d)
		}
	}()

	// --- Plugin manager ---
	// A disabled plugins dir leaves the manager nil; the HTTP routes stay
	// off and the core behaves exactly as before. Enabling it costs one
	// directory scan at startup — installed plugins are Discovered, not
	// Running, until someone POSTs enable.
	var pluginMgr *manager.Manager
	if *pluginsDir != "" {
		reg := pluginregistry.New(*pluginsDir, log.With("component", "plugin-registry"))
		m, err := manager.New(manager.Options{
			Registry: reg,
			Logger:   log.With("component", "plugin-manager"),
			OnPluginLog: func(name, stream, line string) {
				log.Info("plugin log", "plugin", name, "stream", stream, "line", line)
			},
		})
		if err != nil {
			log.Error("plugin manager", "err", err)
			os.Exit(1)
		}
		if err := m.Discover(); err != nil {
			log.Warn("plugin discover", "err", err)
		}
		pluginMgr = m
		defer m.Shutdown(context.Background())
	}

	// --- HTTP API ---
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok"})
	})
	mux.HandleFunc("GET /devices", handleListDevices(devSvc))
	mux.HandleFunc("POST /devices/commission", handleCommission(devSvc, persistOnAdd))
	mux.HandleFunc("POST /devices/sync", handleSyncFromAdapters(devSvc, persistOnAdd))
	mux.HandleFunc("GET /discover", handleDiscoverCommissionable(devSvc))
	mux.HandleFunc("POST /devices/{id}/camera/session", handleCameraSession(devSvc))
	mux.HandleFunc("POST /devices/{id}/camera/ice", handleCameraICE(devSvc))
	mux.HandleFunc("POST /devices/{id}/camera/stop", handleCameraStop(devSvc))
	mux.HandleFunc("GET /devices/{id}", handleGetDevice(devSvc))
	mux.HandleFunc("DELETE /devices/{id}", handleDeleteDevice(devSvc, repo))
	mux.HandleFunc("POST /devices/{id}/actions", handleInvokeAction(devSvc))
	mux.HandleFunc("POST /devices/{id}/state", handleWriteState(devSvc))
	mux.HandleFunc("GET /rules", handleListRules(engine))
	mux.HandleFunc("POST /rules", handleUpsertRule(engine))
	mux.HandleFunc("DELETE /rules/{id}", handleDeleteRule(engine))
	mux.HandleFunc("GET /rules/runs", handleListRuns(engine))
	mux.HandleFunc("GET /stream", ws.Handler(bus, log.With("component", "ws")))

	if pluginMgr != nil {
		pluginsapi.Register(mux, pluginMgr)
	}

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
	// Drop pending confirmation timers first: a device that never answers
	// should not raise an alarm about a system that is already going away.
	devSvc.StopConfirmations()
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

type commissionRequest struct {
	Transport string            `json:"transport"`      // e.g. "matter", "virtual"
	Payload   string            `json:"payload"`        // Matter setup code, virtual spec, …
	Name      string            `json:"name"`           // user-visible label
	Type      string            `json:"type,omitempty"` // domain.DeviceType override; falls back to what the adapter reports
	WifiSSID  string            `json:"wifi_ssid,omitempty"`
	WifiCred  string            `json:"wifi_cred,omitempty"`
	Extra     map[string]string `json:"extra,omitempty"`
}

func handleCommission(svc *service.DeviceService, persist func(*domain.Device)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req commissionRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		if req.Transport == "" || req.Payload == "" {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": "transport and payload are required"})
			return
		}
		if req.Name == "" {
			req.Name = "New device"
		}

		// NDJSON progress stream. Frontend expects one JSON object per line
		// with shape {stage, message?, device?} where stage is one of
		// discovering|pairing|verifying|done|error.
		w.Header().Set("Content-Type", "application/x-ndjson")
		w.Header().Set("Cache-Control", "no-store")
		flusher, _ := w.(http.Flusher)
		enc := json.NewEncoder(w)
		emit := func(ev map[string]any) {
			_ = enc.Encode(ev)
			if flusher != nil {
				flusher.Flush()
			}
		}

		// Give the adapter generous time — matter commissioning through Thread
		// Border Router can take 30-90 seconds under normal conditions.
		ctx, cancel := context.WithTimeout(r.Context(), 3*time.Minute)
		defer cancel()

		emit(map[string]any{"stage": "discovering", "message": "поиск устройства"})

		// Real progress from the transport. The adapter calls Progress from its
		// event goroutine, so stages land on a buffered channel and only this
		// goroutine ever writes to the ResponseWriter. Buffered generously: a
		// full Matter commissioning emits a dozen or so stages, and dropping one
		// is better than blocking the adapter's event pump.
		type stage struct{ name, message string }
		stages := make(chan stage, 32)

		type result struct {
			d   *domain.Device
			err error
		}
		done := make(chan result, 1)
		go func() {
			d, err := svc.Commission(ctx,
				domain.TransportKind(req.Transport),
				ports.CommissionRequest{
					Payload:  req.Payload,
					WifiSSID: req.WifiSSID,
					WifiCred: req.WifiCred,
					Extra:    req.Extra,
					Progress: func(name, message string) {
						select {
						case stages <- stage{name, message}:
						default: // stream consumer is behind; progress is cosmetic
						}
					},
				},
				req.Name,
				domain.DeviceType(req.Type),
			)
			done <- result{d, err}
		}()

		for {
			select {
			case s := <-stages:
				// "done" is reserved for the frame carrying the device — the
				// frontend treats it as terminal.
				if s.name == "done" {
					continue
				}
				emit(map[string]any{"stage": s.name, "message": s.message})
			case res := <-done:
				if res.err != nil {
					// Attach the transport's error category so the UI can show a
					// sentence a person can act on instead of a wrapped Go error.
					frame := map[string]any{"stage": "error", "message": res.err.Error()}
					if kind := matter.KindOf(res.err); kind != "" {
						frame["kind"] = string(kind)
						frame["retryable"] = matter.IsRetryable(res.err)
					}
					emit(frame)
					return
				}
				persist(res.d)
				emit(map[string]any{"stage": "done", "device": res.d})
				return
			case <-ctx.Done():
				emit(map[string]any{"stage": "error", "message": "timeout"})
				return
			}
		}
	}
}

// handleDiscoverCommissionable streams devices that are advertising themselves
// as ready to pair, as NDJSON, one JSON object per line — the shape the web app
// already consumes.
//
// A found device still cannot be added without its setup code: the passcode is
// never advertised. The list exists so the user can see what is pairable and
// tell two identical lamps apart, then pair the one they picked by passing its
// ref back as extra["matter.target"].
func handleDiscoverCommissionable(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		transport := r.URL.Query().Get("transport")
		if transport == "" {
			transport = string(domain.TransportMatter)
		}
		window := 10 * time.Second
		if raw := r.URL.Query().Get("timeout"); raw != "" {
			parsed, err := time.ParseDuration(raw)
			if err != nil || parsed <= 0 {
				writeJSON(w, http.StatusBadRequest, map[string]any{"error": "invalid timeout"})
				return
			}
			window = min(parsed, time.Minute)
		}

		// Outlive the scan itself so the sidecar's reply isn't cut short.
		ctx, cancel := context.WithTimeout(r.Context(), window+30*time.Second)
		defer cancel()

		found, err := svc.DiscoverCommissionable(ctx, domain.TransportKind(transport), window)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}

		w.Header().Set("Content-Type", "application/x-ndjson")
		w.Header().Set("Cache-Control", "no-store")
		flusher, _ := w.(http.Flusher)
		enc := json.NewEncoder(w)

		for d := range found {
			line := map[string]any{
				"ref":       d.Ref,
				"name":      d.Name,
				"transport": transport,
			}
			if d.Type != "" {
				line["type"] = d.Type
			}
			if d.Discriminator != 0 {
				line["discriminator"] = d.Discriminator
			}
			if d.VendorID != 0 {
				line["vendor_id"] = d.VendorID
			}
			if d.ProductID != 0 {
				line["product_id"] = d.ProductID
			}
			if err := enc.Encode(line); err != nil {
				return // client hung up; the scan drains on its own
			}
			if flusher != nil {
				flusher.Flush()
			}
		}
	}
}

// handleCameraSession opens a WebRTC session with a camera and then streams the
// camera's side of the handshake back as NDJSON.
//
// Only signalling passes through keystone. The video itself flows directly from
// the camera to the browser's RTCPeerConnection — both are on the same LAN,
// which is the case ICE is best at, and it keeps a media stack out of the
// engine entirely.
func handleCameraSession(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.DeviceID(r.PathValue("id"))
		var req struct {
			SDP string `json:"sdp"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.SDP == "" {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": "sdp offer is required"})
			return
		}

		streamer, _, err := svc.CameraStreamer(id)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}

		// Subscribe before offering: the camera may answer immediately, and a
		// late subscription would miss it.
		ctx := r.Context()
		signals, err := streamer.Signals(ctx)
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, map[string]any{"error": err.Error()})
			return
		}

		session, err := streamer.StartStream(ctx, deviceRef(svc, id), req.SDP)
		if err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]any{"error": err.Error()})
			return
		}

		w.Header().Set("Content-Type", "application/x-ndjson")
		w.Header().Set("Cache-Control", "no-store")
		flusher, _ := w.(http.Flusher)
		enc := json.NewEncoder(w)
		emit := func(v any) bool {
			if err := enc.Encode(v); err != nil {
				return false
			}
			if flusher != nil {
				flusher.Flush()
			}
			return true
		}

		if !emit(map[string]any{"kind": "session", "session_id": session}) {
			return
		}

		for {
			select {
			case <-ctx.Done():
				// The viewer navigated away; tell the camera so it stops
				// encoding for nobody.
				stopCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
				_ = streamer.StopStream(stopCtx, session)
				cancel()
				return
			case sig, ok := <-signals:
				if !ok {
					return
				}
				if sig.SessionID != session {
					continue // another viewer's session
				}
				frame := map[string]any{"kind": sig.Kind, "session_id": sig.SessionID}
				if sig.SDP != "" {
					frame["sdp"] = sig.SDP
				}
				if len(sig.Candidates) > 0 {
					frame["candidates"] = sig.Candidates
				}
				if sig.Reason != "" {
					frame["reason"] = sig.Reason
				}
				if !emit(frame) || sig.Kind == "end" {
					return
				}
			}
		}
	}
}

func handleCameraICE(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.DeviceID(r.PathValue("id"))
		var req struct {
			SessionID  int      `json:"session_id"`
			Candidates []string `json:"candidates"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		streamer, _, err := svc.CameraStreamer(id)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
		defer cancel()
		if err := streamer.AddCandidates(ctx, req.SessionID, req.Candidates); err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
	}
}

func handleCameraStop(svc *service.DeviceService) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.DeviceID(r.PathValue("id"))
		var req struct {
			SessionID int `json:"session_id"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		streamer, _, err := svc.CameraStreamer(id)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
		defer cancel()
		if err := streamer.StopStream(ctx, req.SessionID); err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]any{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
	}
}

// deviceRef resolves a device id to its transport ref; the camera handlers have
// already validated the device, so a miss here is impossible in practice.
func deviceRef(svc *service.DeviceService, id domain.DeviceID) domain.TransportRef {
	if d, err := svc.Get(id); err == nil {
		return d.TransportRef
	}
	return ""
}

func handleSyncFromAdapters(svc *service.DeviceService, persist func(*domain.Device)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
		defer cancel()
		added, updated, err := svc.SyncFromAdapters(ctx)
		for _, d := range added {
			persist(d)
		}
		for _, d := range updated {
			persist(d)
		}
		if err != nil {
			writeJSON(w, http.StatusBadGateway, map[string]any{"added": added, "updated": updated, "error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"added": added, "updated": updated})
	}
}

// deviceDeleter is the narrow slice of the storage repo the delete handler
// needs. Declared here (not in ports) so the handler avoids a broader
// dependency, and so tests can inject a stub.
type deviceDeleter interface {
	DeleteDevice(ctx context.Context, id domain.DeviceID) error
}

func handleDeleteDevice(svc *service.DeviceService, repo deviceDeleter) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		id := domain.DeviceID(r.PathValue("id"))
		ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
		defer cancel()

		// Delete is best-effort on both sides: the user asked for the device
		// to be gone from the UI. If the transport peer has already vanished
		// (fabric reset, device unplugged) or the disk write fails, we still
		// tell the UI "ok" and surface warnings — otherwise an orphan can
		// resurrect from devices.json on next boot.
		adapterErr := svc.Decommission(ctx, id)
		persistErr := repo.DeleteDevice(r.Context(), id)

		resp := map[string]any{"ok": true}
		if adapterErr != nil {
			resp["adapter_warning"] = adapterErr.Error()
		}
		if persistErr != nil {
			resp["persist_warning"] = persistErr.Error()
		}
		writeJSON(w, http.StatusOK, resp)
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
