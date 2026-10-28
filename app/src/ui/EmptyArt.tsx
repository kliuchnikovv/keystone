// The empty state needs a picture, not just a sentence: a house with an empty socket.
import Svg, { Circle, Path } from 'react-native-svg';
import { useTheme } from './theme';

export function EmptyArt({ size = 148 }: { size?: number }) {
  const t = useTheme();
  return (
    <Svg width={size} height={(size * 100) / 120} viewBox="0 0 120 100" accessible={false} importantForAccessibility="no-hide-descendants">
      {/* the halo behind the house */}
      <Circle cx="60" cy="52" r="40" fill={t.accentSoft} />
      {/* the house */}
      <Path
        d="M22 50 60 18l38 32"
        fill="none"
        stroke={t.accent}
        strokeWidth={3}
        strokeLinecap="round"
        strokeLinejoin="round"
      />
      <Path
        d="M30 46v40h60V46"
        fill="none"
        stroke={t.accent}
        strokeWidth={3}
        strokeLinecap="round"
        strokeLinejoin="round"
      />
      {/* the socket: nothing plugged in yet */}
      <Circle cx="60" cy="64" r="13" fill={t.card} stroke={t.accent} strokeWidth={2.5} />
      <Circle cx="55" cy="62" r="2" fill={t.accent} />
      <Circle cx="65" cy="62" r="2" fill={t.accent} />
      <Path d="M55 68h10" stroke={t.accent} strokeWidth={2.5} strokeLinecap="round" />
      {/* a little signal above it */}
      <Path d="M46 14a18 18 0 0 1 28 0" fill="none" stroke={t.accent} strokeWidth={2.5} strokeLinecap="round" opacity={0.55} />
    </Svg>
  );
}
