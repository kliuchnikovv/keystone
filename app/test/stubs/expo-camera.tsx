// The camera is faked: a computer has no camera, and the point of the test is what the
// app does with the code it read. The test queues a code, and the fake camera reads it
// once — a real camera keeps reading the same code until the app says stop.
import React, { useEffect } from 'react';
import { View } from 'react-native';

let queued: string | null = null;

/** The code the fake camera will read next. */
export function queueQr(data: string | null): void {
  queued = data;
}

export interface FakeBarcodeResult {
  type: string;
  data: string;
}

export function CameraView({
  onBarcodeScanned,
  ...rest
}: {
  onBarcodeScanned?: (result: FakeBarcodeResult) => void;
} & Record<string, unknown>) {
  useEffect(() => {
    if (queued === null) return;
    const data = queued;
    queued = null;
    onBarcodeScanned?.({ type: 'qr', data });
  }, [onBarcodeScanned]);
  return <View {...rest} testID="camera" />;
}

const GRANTED = { granted: true, canAskAgain: true, status: 'granted', expires: 'never' };

export function useCameraPermissions(): [{ granted: boolean; canAskAgain: boolean; status: string; expires: string }, () => Promise<typeof GRANTED>] {
  return [GRANTED, async () => GRANTED];
}
