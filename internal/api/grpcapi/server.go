//go:build grpc

// Package grpcapi implements the gRPC transport for Keystone. It is compiled
// only when the `grpc` build tag is set — this keeps the base binary free of
// heavy dependencies (google.golang.org/grpc, google.golang.org/protobuf) for
// air-gapped and quick-start scenarios.
//
// Build with gRPC:
//
//	make proto        # runs `buf generate`, writing gen/go/keystone/v1/*.pb.go
//	go mod tidy       # pulls grpc, protobuf, googleapis
//	make build-grpc   # go build -tags=grpc ./cmd/keystone
package grpcapi

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"

	"google.golang.org/grpc"
	"google.golang.org/protobuf/types/known/durationpb"
	"google.golang.org/protobuf/types/known/emptypb"
	"google.golang.org/protobuf/types/known/structpb"
	"google.golang.org/protobuf/types/known/timestamppb"

	pb "github.com/keystone/keystone/gen/go/keystone/v1"
	"github.com/keystone/keystone/internal/domain"
	"github.com/keystone/keystone/internal/ports"
	"github.com/keystone/keystone/internal/registry"
	"github.com/keystone/keystone/internal/rules"
	"github.com/keystone/keystone/internal/service"
)

// Server is the composite gRPC service. It multiplexes DeviceService,
// StateService, and RuleService over one net.Listener.
type Server struct {
	pb.UnimplementedDeviceServiceServer
	pb.UnimplementedStateServiceServer
	pb.UnimplementedRuleServiceServer

	log   *slog.Logger
	svc   *service.DeviceService
	reg   *registry.Registry
	bus   ports.EventBus
	rules *rules.Engine
}

// New wires all three services onto one Server.
func New(log *slog.Logger, svc *service.DeviceService, reg *registry.Registry, bus ports.EventBus, eng *rules.Engine) *Server {
	return &Server{log: log, svc: svc, reg: reg, bus: bus, rules: eng}
}

// Serve blocks until ctx is cancelled, running the gRPC server on lis.
func Serve(ctx context.Context, log *slog.Logger, addr string, s *Server) error {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", addr, err)
	}
	gs := grpc.NewServer()
	pb.RegisterDeviceServiceServer(gs, s)
	pb.RegisterStateServiceServer(gs, s)
	pb.RegisterRuleServiceServer(gs, s)

	log.Info("grpc listening", "addr", addr)

	errCh := make(chan error, 1)
	go func() { errCh <- gs.Serve(lis) }()

	select {
	case <-ctx.Done():
		gs.GracefulStop()
		return nil
	case err := <-errCh:
		return err
	}
}

// --- DeviceService ---

func (s *Server) List(ctx context.Context, req *pb.ListDevicesRequest) (*pb.ListDevicesResponse, error) {
	all := s.svc.List()
	out := make([]*pb.Device, 0, len(all))
	for _, d := range all {
		if req.GetTransport() != "" && string(d.Transport) != req.GetTransport() {
			continue
		}
		out = append(out, deviceToProto(d))
	}
	return &pb.ListDevicesResponse{Devices: out}, nil
}

func (s *Server) Get(ctx context.Context, req *pb.GetDeviceRequest) (*pb.Device, error) {
	d, err := s.svc.Get(domain.DeviceID(req.GetId()))
	if err != nil {
		return nil, err
	}
	return deviceToProto(d), nil
}

func (s *Server) Remove(ctx context.Context, req *pb.RemoveDeviceRequest) (*emptypb.Empty, error) {
	if err := s.reg.Remove(domain.DeviceID(req.GetId())); err != nil {
		return nil, err
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) Commission(req *pb.CommissionRequest, stream pb.DeviceService_CommissionServer) error {
	_ = stream.Send(&pb.CommissionEvent{Stage: "pairing", Message: "starting"})

	d, err := s.svc.Commission(stream.Context(), domain.TransportKind(req.GetTransport()),
		ports.CommissionRequest{
			Payload:  req.GetPayload(),
			WifiSSID: req.GetWifiSsid(),
			WifiCred: req.GetWifiCredential(),
			Extra:    req.GetExtra(),
		},
		req.GetName(), domain.DeviceType(req.GetType()))
	if err != nil {
		_ = stream.Send(&pb.CommissionEvent{Stage: "error", Error: err.Error()})
		return err
	}
	_ = stream.Send(&pb.CommissionEvent{Stage: "done", Device: deviceToProto(d)})
	return nil
}

func (s *Server) InvokeAction(ctx context.Context, req *pb.InvokeActionRequest) (*emptypb.Empty, error) {
	params := map[string]any{}
	if req.GetParams() != nil {
		params = req.GetParams().AsMap()
	}
	if err := s.svc.InvokeAction(ctx, domain.DeviceID(req.GetDeviceId()),
		domain.FeatureKey(req.GetFeature()), domain.ActionKey(req.GetAction()), params); err != nil {
		return nil, err
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) WriteState(ctx context.Context, req *pb.WriteStateRequest) (*emptypb.Empty, error) {
	val := req.GetValue().AsInterface()
	if err := s.svc.WriteState(ctx, domain.DeviceID(req.GetDeviceId()),
		domain.FeatureKey(req.GetFeature()), domain.StateKey(req.GetKey()), val); err != nil {
		return nil, err
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) ReadState(ctx context.Context, req *pb.ReadStateRequest) (*pb.ReadStateResponse, error) {
	v, err := s.svc.ReadState(ctx, domain.DeviceID(req.GetDeviceId()),
		domain.FeatureKey(req.GetFeature()), domain.StateKey(req.GetKey()))
	if err != nil {
		return nil, err
	}
	pv, err := structpb.NewValue(v)
	if err != nil {
		return nil, fmt.Errorf("encode value: %w", err)
	}
	return &pb.ReadStateResponse{Value: pv}, nil
}

// --- StateService ---

func (s *Server) Subscribe(req *pb.SubscribeRequest, stream pb.StateService_SubscribeServer) error {
	ctx := stream.Context()

	filter := map[string]struct{}{}
	for _, id := range req.GetDeviceIds() {
		filter[id] = struct{}{}
	}
	match := func(deviceID string) bool {
		if len(filter) == 0 {
			return true
		}
		_, ok := filter[deviceID]
		return ok
	}

	if req.GetIncludeInitial() {
		// Emit current cached state for every subscribed device.
		for _, d := range s.svc.List() {
			if !match(string(d.ID)) {
				continue
			}
			for _, f := range d.Features {
				for _, k := range f.States {
					snap, ok := s.reg.GetState(d.ID, f.Key, k)
					if !ok {
						continue
					}
					_ = stream.Send(&pb.StateStreamMessage{
						Payload: &pb.StateStreamMessage_State{State: stateToProto(snap)},
					})
				}
			}
		}
		_ = stream.Send(&pb.StateStreamMessage{Payload: &pb.StateStreamMessage_Ready{Ready: &pb.Ready{}}})
	}

	stateCh := s.bus.SubscribeStates(ctx)
	eventCh := s.bus.SubscribeEvents(ctx)

	for {
		select {
		case <-ctx.Done():
			return nil
		case s, ok := <-stateCh:
			if !ok {
				return nil
			}
			if !match(string(s.DeviceID)) {
				continue
			}
			if err := stream.Send(&pb.StateStreamMessage{
				Payload: &pb.StateStreamMessage_State{State: stateToProto(s)},
			}); err != nil {
				return err
			}
		case ev, ok := <-eventCh:
			if !ok {
				return nil
			}
			if !match(string(ev.DeviceID)) {
				continue
			}
			if err := stream.Send(&pb.StateStreamMessage{
				Payload: &pb.StateStreamMessage_Event{Event: eventToProto(ev)},
			}); err != nil {
				return err
			}
		}
	}
}

// --- RuleService ---

func (s *Server) ListRules(ctx context.Context, req *pb.ListRulesRequest) (*pb.ListRulesResponse, error) {
	all := s.rules.List()
	out := make([]*pb.Rule, 0, len(all))
	for _, r := range all {
		p, err := ruleToProto(r)
		if err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return &pb.ListRulesResponse{Rules: out}, nil
}

func (s *Server) GetRule(ctx context.Context, req *pb.GetRuleRequest) (*pb.Rule, error) {
	for _, r := range s.rules.List() {
		if string(r.ID) == req.GetId() {
			return ruleToProto(r)
		}
	}
	return nil, errors.New("rule not found")
}

func (s *Server) UpsertRule(ctx context.Context, req *pb.UpsertRuleRequest) (*pb.Rule, error) {
	if req.GetRule() == nil {
		return nil, errors.New("rule required")
	}
	var r domain.Rule
	if err := json.Unmarshal([]byte(req.GetRule().GetSpecJson()), &r); err != nil {
		return nil, fmt.Errorf("decode rule spec: %w", err)
	}
	// Overlay top-level fields from the proto (they win over spec_json).
	if req.GetRule().GetId() != "" {
		r.ID = domain.RuleID(req.GetRule().GetId())
	}
	if req.GetRule().GetName() != "" {
		r.Name = req.GetRule().GetName()
	}
	if req.GetRule().GetHumanReadable() != "" {
		r.HumanReadable = req.GetRule().GetHumanReadable()
	}
	r.Enabled = req.GetRule().GetEnabled()
	if req.GetRule().GetMode() != "" {
		r.Mode = domain.RunMode(req.GetRule().GetMode())
	}
	if err := s.rules.Upsert(ctx, &r); err != nil {
		return nil, err
	}
	return ruleToProto(&r)
}

func (s *Server) RemoveRule(ctx context.Context, req *pb.RemoveRuleRequest) (*emptypb.Empty, error) {
	if err := s.rules.Remove(ctx, domain.RuleID(req.GetId())); err != nil {
		return nil, err
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) EnableRule(ctx context.Context, req *pb.EnableRuleRequest) (*emptypb.Empty, error) {
	return s.toggleRule(ctx, req.GetId(), true)
}

func (s *Server) DisableRule(ctx context.Context, req *pb.DisableRuleRequest) (*emptypb.Empty, error) {
	return s.toggleRule(ctx, req.GetId(), false)
}

func (s *Server) toggleRule(ctx context.Context, id string, enabled bool) (*emptypb.Empty, error) {
	for _, r := range s.rules.List() {
		if string(r.ID) != id {
			continue
		}
		r.Enabled = enabled
		if err := s.rules.Upsert(ctx, r); err != nil {
			return nil, err
		}
		return &emptypb.Empty{}, nil
	}
	return nil, errors.New("rule not found")
}

func (s *Server) ListRuns(ctx context.Context, req *pb.ListRunsRequest) (*pb.ListRunsResponse, error) {
	limit := int(req.GetLimit())
	if limit <= 0 {
		limit = 50
	}
	all := s.rules.Runs(limit)
	out := make([]*pb.RuleRun, 0, len(all))
	for _, r := range all {
		if req.GetRuleId() != "" && string(r.RuleID) != req.GetRuleId() {
			continue
		}
		out = append(out, ruleRunToProto(r))
	}
	return &pb.ListRunsResponse{Runs: out}, nil
}

func (s *Server) TestFire(ctx context.Context, req *pb.TestFireRequest) (*pb.TestFireResponse, error) {
	// v0: describe the actions without executing.
	for _, r := range s.rules.List() {
		if string(r.ID) != req.GetId() {
			continue
		}
		descs := make([]string, 0, len(r.Actions))
		for _, a := range r.Actions {
			descs = append(descs, describeAction(a))
		}
		return &pb.TestFireResponse{SimulatedActions: descs}, nil
	}
	return nil, errors.New("rule not found")
}

// --- Converters ---

func deviceToProto(d *domain.Device) *pb.Device {
	if d == nil {
		return nil
	}
	features := make([]*pb.Feature, 0, len(d.Features))
	for _, f := range d.Features {
		fp := &pb.Feature{Key: string(f.Key)}
		for _, k := range f.States {
			fp.States = append(fp.States, string(k))
		}
		for _, k := range f.Actions {
			fp.Actions = append(fp.Actions, string(k))
		}
		for _, k := range f.Events {
			fp.Events = append(fp.Events, string(k))
		}
		features = append(features, fp)
	}
	return &pb.Device{
		Id:           string(d.ID),
		Type:         string(d.Type),
		Name:         d.Name,
		Manufacturer: d.Manufacturer,
		Model:        d.Model,
		RoomId:       string(d.Room),
		Transport:    string(d.Transport),
		TransportRef: string(d.TransportRef),
		Features:     features,
		Metadata:     d.Metadata,
		CreatedAt:    timestamppb.New(d.CreatedAt),
		UpdatedAt:    timestamppb.New(d.UpdatedAt),
	}
}

func stateToProto(s domain.StateSnapshot) *pb.StateSnapshot {
	pv, _ := structpb.NewValue(s.Value)
	return &pb.StateSnapshot{
		DeviceId:  string(s.DeviceID),
		Feature:   string(s.Feature),
		Key:       string(s.Key),
		Value:     pv,
		UpdatedAt: timestamppb.New(s.UpdatedAt),
		Origin:    string(s.Origin),
	}
}

func eventToProto(e domain.Event) *pb.Event {
	ps, _ := structpb.NewStruct(e.Data)
	return &pb.Event{
		DeviceId: string(e.DeviceID),
		Feature:  string(e.Feature),
		Name:     string(e.Name),
		Data:     ps,
		At:       timestamppb.New(e.At),
	}
}

func ruleToProto(r *domain.Rule) (*pb.Rule, error) {
	b, err := json.Marshal(r)
	if err != nil {
		return nil, err
	}
	return &pb.Rule{
		Id:            string(r.ID),
		Name:          r.Name,
		HumanReadable: r.HumanReadable,
		Enabled:       r.Enabled,
		Mode:          string(r.Mode),
		SpecJson:      string(b),
		CreatedAt:     timestamppb.New(r.CreatedAt),
		UpdatedAt:     timestamppb.New(r.UpdatedAt),
	}, nil
}

func ruleRunToProto(r domain.RuleRun) *pb.RuleRun {
	return &pb.RuleRun{
		Id:          r.ID,
		RuleId:      string(r.RuleID),
		TriggeredAt: timestamppb.New(r.TriggeredAt),
		Status:      string(r.Status),
		Error:       r.Error,
		Duration:    durationpb.New(r.Duration),
	}
}

func describeAction(a domain.RuleAction) string {
	switch v := a.(type) {
	case *rules.InvokeAction:
		return fmt.Sprintf("invoke %s.%s on device %s", v.Feature, v.Action, v.DeviceID)
	case *rules.SetState:
		return fmt.Sprintf("set %s.%s = %v on device %s", v.Feature, v.Key, v.Value, v.DeviceID)
	case *rules.Delay:
		return fmt.Sprintf("delay %s", v.Duration)
	}
	return "unknown action"
}
