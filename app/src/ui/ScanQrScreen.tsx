// "Scan a QR code": the code printed on a Matter device.
//
// The app does not commission anything — it has no Matter stack and cannot hold one.
// What it does is read the code and hand the bytes to the hub, which does have the
// stack. The code itself is the payload the Matter specification calls "onboarding":
// the discriminator of the device and the setup PIN code, in eleven bytes.
//
// What the app cannot do is commission from this code alone: the setup hash — the
// proof that the commissioner is allowed to touch the device — is not in the code, it
// comes from the device over Bluetooth. The hub's side of that is not written yet.
import { useCallback, useState } from 'react';
import { CameraView, useCameraPermissions } from 'expo-camera';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { matterFromQr, type MatterIdentity } from '../model/matter';
import { Button, Note } from './controls';
import { Glyph } from './Glyph';
import { ScreenHeader } from './ScreenHeader';
import { useTheme } from './theme';

export function ScanQrScreen({
  onScanned,
  onClose,
}: {
  /** The code was read, and it is a Matter code. */
  onScanned(matter: MatterIdentity): void;
  onClose(): void;
}) {
  const t = useTheme();
  const [permission, requestPermission] = useCameraPermissions();
  const [wrong, setWrong] = useState(false);

  const onBarcodeScanned = useCallback(
    (result: { type: string; data: string }) => {
      const matter = matterFromQr(result.data);
      if (!matter) {
        // Not a Matter code — say so and let the person point the camera elsewhere.
        setWrong(true);
        return;
      }
      setWrong(false);
      onScanned(matter);
    },
    [onScanned],
  );

  return (
    <View style={[styles.screen, { backgroundColor: t.bg }]}>
      <ScreenHeader
        title="Scan a QR code"
        left={
          <Pressable accessibilityRole="button" accessibilityLabel="Back" onPress={onClose} style={styles.back} hitSlop={8}>
            <Glyph name="back" size={22} color={t.accent} />
          </Pressable>
        }
      />

      {permission && !permission.granted ? (
        <View style={styles.page}>
          <Note
            tone="quiet"
            title="The camera is not allowed."
            text="Keystone needs the camera to read the code on the device. You can also turn it on in the settings of the phone."
          />
          <Button label="Allow the camera" onPress={() => void requestPermission()} />
        </View>
      ) : (
        <View style={styles.camera}>
          <CameraView
            style={StyleSheet.absoluteFill}
            facing="back"
            barcodeScannerSettings={{ barcodeTypes: ['qr'] }}
            onBarcodeScanned={onBarcodeScanned}
          />
          <View style={styles.hint}>
            <Text style={styles.hintText}>Point the camera at the QR code on the device.</Text>
          </View>
          {wrong ? (
            <View style={styles.wrong}>
              <Note
                tone="quiet"
                title="That is not a Matter code."
                text="The code on a Matter device starts with MT:. Point the camera at it and try again."
              />
            </View>
          ) : null}
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  page: { padding: 16, gap: 12 },
  camera: { flex: 1, overflow: 'hidden' },
  hint: {
    position: 'absolute',
    top: 14,
    left: 16,
    right: 16,
    padding: 10,
    borderRadius: 12,
    backgroundColor: 'rgba(0,0,0,0.55)',
  },
  hintText: { color: '#fff', fontSize: 13, textAlign: 'center' },
  wrong: { position: 'absolute', left: 16, right: 16, bottom: 24 },
  back: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center' },
});
