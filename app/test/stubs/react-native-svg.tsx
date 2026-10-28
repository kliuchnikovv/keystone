// react-native-svg ships Flow-typed sources that no test transform here understands
// (the same reason expo-modules-core is stubbed). The UI tests assert on labels and text,
// never on icon internals, so each icon renders as an empty box of the right size.
import { View } from 'react-native';
import type { ReactNode } from 'react';

function Icon({ size = 20 }: { size?: number }): ReactNode {
  return <View style={{ width: size, height: size }} />;
}

export default Icon;
export const Svg = Icon;
export const Path = Icon;
export const Circle = Icon;
export const Rect = Icon;
export const Line = Icon;
export const G = Icon;
export const Ellipse = Icon;
export const Polyline = Icon;
export const Polygon = Icon;