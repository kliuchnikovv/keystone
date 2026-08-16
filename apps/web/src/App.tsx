import { Routes, Route, Navigate } from 'react-router-dom';
import { useEffect } from 'react';
import { HomeScreen } from './screens/HomeScreen';
import { AddDeviceScreen } from './screens/AddDeviceScreen';
import { DeviceDetailScreen } from './screens/DeviceDetailScreen';
import { PluginsScreen } from './screens/PluginsScreen';
import { PluginEmbedScreen } from './screens/PluginEmbedScreen';
import { useLiveStream } from './hooks/useLiveStream';

export function App() {
  useLiveStream();
  useEffect(() => {
    document.documentElement.dataset.theme = 'dark';
  }, []);
  return (
    <Routes>
      <Route path="/" element={<HomeScreen />} />
      <Route path="/add-device" element={<AddDeviceScreen />} />
      <Route path="/device/:id" element={<DeviceDetailScreen />} />
      <Route path="/plugins" element={<PluginsScreen />} />
      <Route path="/plugins/:name/embed" element={<PluginEmbedScreen />} />
      <Route path="*" element={<Navigate to="/" replace />} />
    </Routes>
  );
}
