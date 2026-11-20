// The «+» menu: what can be added.
import { Pressable, StyleSheet, Text, View } from './kit';
import { Glyph, type GlyphName } from './Glyph';
import { Sheet } from './Sheet';
import { useTheme } from './theme';

export interface AddMenuProps {
  visible: boolean;
  onClose(): void;
  /** Find a device and pair it with the hub. */
  onDevice(): void;
  /** Save what the devices are doing now as a scene. */
  onScene(): void;
  /** A schedule, a timer, or a rule. */
  onAutomation(): void;
}

/** One row of the menu: an icon tile, a name, a hint, a chevron. */
function AddRow({
  glyph,
  label,
  hint,
  onPress,
}: {
  glyph: GlyphName;
  label: string;
  hint: string;
  onPress(): void;
}) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={label}
      onPress={onPress}
      style={[styles.row, { backgroundColor: t.card, borderColor: t.border }]}
    >
      <View style={[styles.tile, { backgroundColor: t.accentSoft }]}>
        <Glyph name={glyph} size={20} color={t.accentInk} />
      </View>
      <View style={styles.rowText}>
        <Text style={[styles.name, { color: t.text }]}>{label}</Text>
        <Text style={[styles.hint, { color: t.muted }]}>{hint}</Text>
      </View>
      <Glyph name="chevronRight" size={16} color={t.muted} />
    </Pressable>
  );
}

export function AddMenu({ visible, onClose, onDevice, onScene, onAutomation }: AddMenuProps) {
  const t = useTheme();
  return (
    <Sheet visible={visible} title="Add" onCancel={onClose}>
      <View style={styles.list}>
        <AddRow
          glyph="plug"
          label="Device"
          hint="Find a device that is nearby"
          onPress={() => {
            onClose();
            onDevice();
          }}
        />
        <AddRow
          glyph="scene"
          label="Scene"
          hint="Save what the devices are doing now"
          onPress={() => {
            onClose();
            onScene();
          }}
        />
        <AddRow
          glyph="automation"
          label="Automation"
          hint="A schedule, a timer, or a rule"
          onPress={() => {
            onClose();
            onAutomation();
          }}
        />
      </View>
    </Sheet>
  );
}

const styles = StyleSheet.create({
  list: { gap: 8, paddingBottom: 8 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 12, padding: 12, borderRadius: 16, borderWidth: 1 },
  tile: { width: 40, height: 40, borderRadius: 14, alignItems: 'center', justifyContent: 'center' },
  rowText: { flex: 1, gap: 2, minWidth: 0 },
  name: { fontSize: 14.5, fontWeight: '600' },
  hint: { fontSize: 12 },
});
