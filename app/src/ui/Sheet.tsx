// Bottom sheets and modal dialogs: the app's way of asking for something that does not
// fit on the screen — a colour, a confirmation, a form.
import { type ReactNode } from 'react';
import {
  Modal,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
  useWindowDimensions,
} from 'react-native';
import { useKeyboardInset } from '../hooks/useKeyboardInset';
import { useTheme } from './theme';

export interface SheetProps {
  visible: boolean;
  title?: string;
  onCancel(): void;
  children: ReactNode;
}

/** A sheet that rises from the bottom over a dimmed screen. */
export function Sheet({ visible, title, onCancel, children }: SheetProps) {
  const t = useTheme();
  // The keyboard must not cover the field being typed into: the sheet rides up
  // with it and gives the space back.
  const inset = useKeyboardInset();
  const { height } = useWindowDimensions();
  const maxHeight = Math.max(200, Math.min(height * 0.78, height - inset - 24));
  // Nothing at all when closed: on the web a hidden modal keeps its content in the
  // document, and a screen reader would still find it.
  if (!visible) return null;
  return (
    <Modal visible transparent animationType="slide" onRequestClose={onCancel}>
      <Pressable style={[StyleSheet.absoluteFill, styles.backdrop]} onPress={onCancel} accessibilityLabel="Close" />
      <View
        style={[
          styles.sheet,
          {
            backgroundColor: t.surface,
            borderColor: t.border,
            maxHeight,
            transform: [{ translateY: -inset }],
          },
        ]}
      >
        <View style={[styles.grabber, { backgroundColor: t.border }]} />
        {title ? <Text style={[styles.title, { color: t.text }]}>{title}</Text> : null}
        <ScrollView
          style={styles.body}
          contentContainerStyle={styles.bodyInner}
          keyboardShouldPersistTaps="handled"
          keyboardDismissMode={Platform.OS === 'ios' ? 'interactive' : 'on-drag'}
          showsVerticalScrollIndicator={false}
        >
          {children}
        </ScrollView>
      </View>
    </Modal>
  );
}

/** A row of a sheet: plain text, a hairline between the rows, the last one without. */
export function SheetRow({ label, tone = 'normal', onPress, a11y, last }: {
  label: string;
  tone?: 'normal' | 'danger';
  onPress(): void;
  a11y?: string;
  last?: boolean;
}) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={a11y ?? label}
      onPress={onPress}
      style={[styles.sheetRow, { borderBottomColor: t.border, borderBottomWidth: last ? 0 : StyleSheet.hairlineWidth }]}
    >
      <Text style={{ color: tone === 'danger' ? t.bad : t.text, fontSize: 15 }}>{label}</Text>
    </Pressable>
  );
}

export interface ConfirmDialogProps {  visible: boolean;
  title: string;
  body: string;
  confirmLabel: string;
  /** The confirm button turns red for destructive acts. */
  danger?: boolean;
  onConfirm(): void;
  onCancel(): void;
}

/** A dialog that says what will happen, in words, before it happens. */
export function ConfirmDialog({ visible, title, body, confirmLabel, danger, onConfirm, onCancel }: ConfirmDialogProps) {
  const t = useTheme();
  if (!visible) return null;
  return (
    <Modal visible={visible} transparent animationType="fade" onRequestClose={onCancel}>
      <Pressable style={[StyleSheet.absoluteFill, styles.backdrop]} onPress={onCancel} accessibilityLabel="Close the dialog" />
      <View style={styles.dialogWrap}>
        <View style={[styles.dialog, { backgroundColor: t.surface, borderColor: t.border }]}>
          <Text style={[styles.dialogTitle, { color: t.text }]}>{title}</Text>
          <Text style={[styles.dialogBody, { color: t.muted }]}>{body}</Text>
          <View style={styles.dialogActions}>
            <Pressable
              accessibilityRole="button"
              accessibilityLabel={confirmLabel}
              onPress={onConfirm}
              style={[styles.dialogButton, danger ? { borderColor: t.bad } : { backgroundColor: t.accent, borderColor: t.accent }]}
            >
              <Text style={{ color: danger ? t.bad : t.onAccent, fontWeight: '600', fontSize: 15 }}>{confirmLabel}</Text>
            </Pressable>
            <Pressable
              accessibilityRole="button"
              accessibilityLabel="Cancel"
              onPress={onCancel}
              style={[styles.dialogButton, { backgroundColor: t.fill, borderColor: t.fill }]}
            >
              <Text style={{ color: t.text, fontWeight: '600', fontSize: 15 }}>Cancel</Text>
            </Pressable>
          </View>
        </View>
      </View>
    </Modal>
  );
}

const styles = StyleSheet.create({
  backdrop: { backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 40 },
  sheet: {
    position: 'absolute',
    left: 0,
    right: 0,
    bottom: 0,
    zIndex: 41,
    borderTopLeftRadius: 24,
    borderTopRightRadius: 24,
    borderWidth: 1,
    padding: 20,
    paddingBottom: 34,
  },
  grabber: { width: 36, height: 4, borderRadius: 2, alignSelf: 'center', marginBottom: 14 },
  body: { flexShrink: 1 },
  bodyInner: { paddingBottom: 8 },
  title: { fontSize: 17, fontWeight: '700' },
  sheetRow: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingVertical: 13 },
  dialogWrap: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24 },
  dialog: { width: '100%', maxWidth: 380, borderRadius: 20, borderWidth: 1, padding: 20 },
  dialogTitle: { fontSize: 17, fontWeight: '700' },
  dialogBody: { fontSize: 13.5, marginTop: 6, lineHeight: 1.45 },
  dialogActions: { marginTop: 18, gap: 8 },
  dialogButton: { alignItems: 'center', paddingVertical: 12, borderRadius: 14, borderWidth: 1 },
});
