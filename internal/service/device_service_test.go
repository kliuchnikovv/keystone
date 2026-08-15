package service

import (
	"context"
	"io"
	"log/slog"
	"testing"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
	"github.com/kliuchnikovv/keystone/internal/registry"
)

// fakeAdapter reports one device the way a real transport would after an
// interview: it knows the type and the name, because it asked the device.
type fakeAdapter struct {
	discovered ports.DiscoveredDevice
}

func (f *fakeAdapter) Kind() domain.TransportKind  { return domain.TransportMatter }
func (f *fakeAdapter) Start(context.Context) error { return nil }
func (f *fakeAdapter) Stop(context.Context) error  { return nil }
func (f *fakeAdapter) Commission(context.Context, ports.CommissionRequest) (domain.TransportRef, error) {
	return f.discovered.TransportRef, nil
}
func (f *fakeAdapter) Discover(context.Context) (<-chan ports.DiscoveredDevice, error) {
	ch := make(chan ports.DiscoveredDevice, 1)
	ch <- f.discovered
	close(ch)
	return ch, nil
}
func (f *fakeAdapter) ReadState(context.Context, domain.TransportRef, domain.FeatureKey, domain.StateKey) (any, error) {
	return nil, nil
}
func (f *fakeAdapter) WriteState(context.Context, domain.TransportRef, domain.FeatureKey, domain.StateKey, any) error {
	return nil
}
func (f *fakeAdapter) InvokeAction(context.Context, domain.TransportRef, domain.FeatureKey, domain.ActionKey, map[string]any) error {
	return nil
}
func (f *fakeAdapter) Subscribe(context.Context) (<-chan ports.TransportEvent, error) {
	return make(chan ports.TransportEvent), nil
}
func (f *fakeAdapter) Decommission(context.Context, domain.TransportRef) error { return nil }

func newTestService(disc ports.DiscoveredDevice) *DeviceService {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	return NewDeviceService(log, registry.New(), nil, []ports.Adapter{&fakeAdapter{discovered: disc}})
}

// A caller that does not know the device type must not end up storing a blank
// one. An empty type leaves the UI with no controls at all — a fully working
// lamp showed up as "управление не реализовано" because of exactly this.
func TestCommissionFallsBackToWhatTheDeviceReports(t *testing.T) {
	disc := ports.DiscoveredDevice{
		TransportRef: "n1",
		Type:         domain.DeviceTypeLight,
		Name:         "VARMBLIXT table/wall lamp",
		Manufacturer: "IKEA of Sweden",
		Features:     []domain.Feature{{Key: domain.FeatureOnOff}, {Key: domain.FeatureBrightness}},
	}

	t.Run("пустые имя и тип берутся у устройства", func(t *testing.T) {
		svc := newTestService(disc)
		d, err := svc.Commission(context.Background(), domain.TransportMatter,
			ports.CommissionRequest{Payload: "code"}, "", "")
		if err != nil {
			t.Fatalf("commission: %v", err)
		}
		if d.Type != domain.DeviceTypeLight {
			t.Errorf("type = %q, а без него UI не покажет ни одного управления", d.Type)
		}
		if d.Name != disc.Name {
			t.Errorf("name = %q want %q", d.Name, disc.Name)
		}
	})

	t.Run("явно заданные значения не перетираются", func(t *testing.T) {
		svc := newTestService(disc)
		d, err := svc.Commission(context.Background(), domain.TransportMatter,
			ports.CommissionRequest{Payload: "code"}, "Лампа у дивана", domain.DeviceTypePlug)
		if err != nil {
			t.Fatalf("commission: %v", err)
		}
		if d.Name != "Лампа у дивана" || d.Type != domain.DeviceTypePlug {
			t.Errorf("выбор пользователя перезаписан: %q / %q", d.Name, d.Type)
		}
	})
}
