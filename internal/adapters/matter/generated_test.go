package matter

import (
	"strings"
	"testing"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// Рукописная таблица «менять командой» должна совпадать со спекой. Раньше это
// знание жило в голове: яркость и цвет писались как атрибуты, устройство их
// игнорировало, и никакой ошибки при этом не возникало.
func TestWriteAsCommandMatchesSpec(t *testing.T) {
	for feature, states := range writeAsCommand {
		for state := range states {
			candidates := featureBindings[feature][state]
			if len(candidates) == 0 {
				t.Errorf("%s/%s помечен как командный, но не имеет биндинга", feature, state)
				continue
			}
			for _, b := range candidates {
				key := b.Cluster + "." + b.Attribute
				spec, known := genAttributes[key]
				if !known {
					// Не повод падать: в манифест генератора внесены не все
					// атрибуты. Но и молчать нельзя — иначе таблица снова
					// начнёт расходиться со спекой незаметно.
					t.Logf("%s нет в манифесте генератора — писабельность не проверена", key)
					continue
				}
				if spec.Writable {
					t.Errorf("%s писабелен по спеке, а мы шлём команду — лишний обход", key)
				}
			}
		}
	}
}

// Обратная проверка: если атрибут по спеке read-only, а мы пишем в него
// напрямую, команда не дойдёт до устройства и никто не заметит.
func TestNoDirectWritesToReadOnlyAttributes(t *testing.T) {
	for feature, states := range featureBindings {
		for state, candidates := range states {
			if _, viaCommand := WriteAsCommand(feature, state); viaCommand {
				continue
			}
			// Проверяем только то, что действительно умеем писать.
			if _, err := encodeAttribute(feature, state, 1); err != nil {
				continue
			}
			for _, b := range candidates {
				spec, known := genAttributes[b.Cluster+"."+b.Attribute]
				if known && !spec.Writable {
					t.Errorf("%s.%s: пишем в read-only атрибут %s.%s — устройство это проигнорирует",
						feature, state, b.Cluster, b.Attribute)
				}
			}
		}
	}
}

// Команда без обязательного аргумента отвергается валидацией и до устройства не
// доходит. Проверяем каждую команду, которую keystone умеет отправлять.
func TestCommandsCarryEveryRequiredArgument(t *testing.T) {
	cases := []struct {
		feature domain.FeatureKey
		action  domain.ActionKey
		params  map[string]any
	}{
		{domain.FeatureOnOff, domain.ActionTurnOn, nil},
		{domain.FeatureBrightness, domain.ActionSet, map[string]any{"level": 50}},
		{domain.FeatureColorTemp, domain.ActionSet, map[string]any{"kelvin": 2700}},
		{domain.FeatureColor, domain.ActionSet, map[string]any{"hue": 120}},
		{domain.FeatureColor, domain.ActionSet, map[string]any{"saturation": 50}},
		{domain.FeatureColor, domain.ActionSet, map[string]any{"hue": 120, "saturation": 50}},
		{domain.FeatureColor, domain.ActionSet, map[string]any{"x": 0.3, "y": 0.4}},
		{domain.FeatureLock, domain.ActionLock, nil},
		{domain.FeatureCoverPosition, domain.ActionSet, map[string]any{"level": 40}},
		{domain.FeatureMedia, domain.ActionStart, nil},
		{domain.FeatureSmoke, domain.ActionSelfTest, nil},
		{domain.FeatureTempControl, domain.ActionSet, map[string]any{"celsius": 180.0}},
		{domain.FeatureMode, domain.ActionSet, map[string]any{"mode": 2}},
	}

	for _, tc := range cases {
		inv, err := actionToInvoke("n1", 1, nil, tc.feature, tc.action, tc.params)
		if err != nil {
			t.Errorf("%s/%s: %v", tc.feature, tc.action, err)
			continue
		}
		required, known := genRequiredArgs[inv.Cluster+"."+inv.Command]
		if !known {
			t.Logf("%s.%s нет в манифесте генератора — аргументы не проверены", inv.Cluster, inv.Command)
			continue
		}
		var missing []string
		for _, arg := range required {
			if _, ok := inv.Args[arg]; !ok {
				missing = append(missing, arg)
			}
		}
		if len(missing) > 0 {
			t.Errorf("%s.%s без обязательных аргументов %s — устройство отвергнет команду",
				inv.Cluster, inv.Command, strings.Join(missing, ", "))
		}
	}
}

// Значения енумов должны приходить из модели: рукописные расходились со спекой
// молча — читатель видел «67» вместо состояния пылесоса.
func TestEnumsComeFromTheModel(t *testing.T) {
	for name, table := range map[string]map[int]string{
		"lockStates":        lockStates,
		"hvacModes":         hvacModes,
		"fanModes":          fanModes,
		"airQualityLevels":  airQualityLevels,
		"alarmStates":       alarmStates,
		"operationalStates": operationalStates,
		"playbackStates":    playbackStates,
		"evseStates":        evseStates,
		"evseSupplyStates":  evseSupplyStates,
		"colorModes":        colorModes,
		"valveStates":       valveStates,
	} {
		if len(table) == 0 {
			t.Errorf("%s пуст — генератор не отдал значений, и декодирование молча вернёт число", name)
		}
	}

	// Точечно: состояния, которых не хватало в рукописной версии.
	if operationalStates[67] == "" {
		t.Error("нет состояния 67 (EmptyingDustBin) — пылесос покажет число вместо состояния")
	}
	if evseSupplyStates[5] == "" {
		t.Error("нет состояния 5 (Enabled) у зарядки")
	}
}
