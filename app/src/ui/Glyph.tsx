import Svg, { Path, Circle, Rect } from 'react-native-svg';
import type { ReactNode } from 'react';

// Real icons: one component per device kind, tab and marker, drawn with the same
// lucide geometry as the design mockups (24×24 grid, round caps, 2 px stroke).
export type GlyphName =
  | 'light' | 'switch' | 'sensor' | 'occupancy' | 'contact' | 'cover' | 'lock' | 'climate' | 'button'
  | 'home' | 'automation' | 'hub' | 'history' | 'plus' | 'pencil' | 'chevron' | 'play' | 'check'
  | 'bolt' | 'chip' | 'signal';

export interface GlyphProps {
  name: GlyphName;
  size?: number;
  color: string;
  /** Fills the main shape at low opacity: reads as "this one is on". */
  filled?: boolean;
}

type Stroke = {
  stroke: string;
  strokeWidth: number;
  strokeLinecap: 'round';
  strokeLinejoin: 'round';
  fill: string;
};
type Solid = Stroke & { fillOpacity: number };

export function Glyph({ name, size = 20, color, filled = false }: GlyphProps) {
  const stroke = Math.max(1.6, Math.round((size / 24) * 2 * 10) / 10);
  const c: Stroke = {
    stroke: color,
    strokeWidth: stroke,
    strokeLinecap: 'round',
    strokeLinejoin: 'round',
    fill: 'none',
  };
  const on: Solid = { ...c, fill: color, fillOpacity: 0.22 };

  return (
    <Svg
      width={size}
      height={size}
      viewBox="0 0 24 24"
      accessible={false}
      importantForAccessibility="no-hide-descendants"
    >
      {icon(name, c, on)}
    </Svg>
  );
}

function icon(name: GlyphName, c: Stroke, on: Solid): ReactNode {
  switch (name) {
    /* ---- devices ---- */
    case 'light':
      return (
        <>
          <Path d="M12 2C8 2 5 5 5 9c0 3 2 5 4 6v5h6v-5c2-1 4-3 4-6 0-4-3-7-7-7z" {...on} />
          <Path d="M9 22h6" {...c} />
        </>
      );
    case 'switch':
      return (
        <>
          <Path d="M9 2v6L4 18a2 2 0 0 0 1.7 3h12.6A2 2 0 0 0 20 18L15 8V2" {...on} />
          <Path d="M9 2h6" {...c} />
        </>
      );
    case 'sensor':
      return (
        <>
          <Path d="M14 4v10.54a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0z" {...on} />
          <Path d="M12 8v6" {...c} />
        </>
      );
    case 'occupancy':
      return (
        <>
          <Path d="M3 7V5a2 2 0 0 1 2-2h2" {...c} />
          <Path d="M17 3h2a2 2 0 0 1 2 2v2" {...c} />
          <Path d="M21 17v2a2 2 0 0 1-2 2h-2" {...c} />
          <Path d="M7 21H5a2 2 0 0 1-2-2v-2" {...c} />
          <Circle cx="12" cy="12" r="2.5" {...on} />
        </>
      );
    case 'contact':
      return (
        <>
          <Path d="M13 4h3a2 2 0 0 1 2 2v14" {...on} />
          <Path d="M2 20h3" {...c} />
          <Path d="M13 20h9" {...c} />
          <Path d="M6 8v12a2 2 0 0 0 2 2h2" {...c} />
        </>
      );
    case 'cover':
      return (
        <>
          <Rect x="3" y="3" width="18" height="18" rx="2" {...on} />
          <Path d="M3 8h18M3 16h18" {...c} />
        </>
      );
    case 'lock':
      return (
        <>
          <Rect x="3" y="11" width="18" height="11" rx="2" {...on} />
          <Path d="M7 11V7a5 5 0 0 1 10 0v4" {...c} />
        </>
      );
    case 'climate':
      return (
        <>
          <Circle cx="12" cy="12" r="4" {...on} />
          <Path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41" {...c} />
        </>
      );
    case 'button':
      return (
        <>
          <Circle cx="12" cy="12" r="10" {...c} />
          <Circle cx="12" cy="12" r="3" {...on} />
        </>
      );

    /* ---- tab bar ---- */
    case 'home':
      return (
        <>
          <Path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" {...on} />
          <Path d="M9 22V12h6v10" {...c} />
        </>
      );
    case 'automation':
      return (
        <>
          <Path d="M21 4h-7M10 4H3M21 12h-9M8 12H3M21 20h-5M12 20H3" {...c} />
          <Circle cx="14" cy="4" r="2" {...on} />
          <Circle cx="12" cy="12" r="2" {...on} />
          <Circle cx="16" cy="20" r="2" {...on} />
        </>
      );
    case 'hub':
      return (
        <>
          <Path d="M21 8v8a2 2 0 0 1-1 1.73l-7 4a2 2 0 0 1-2 0l-7-4A2 2 0 0 1 3 16V8a2 2 0 0 1 1-1.73l7-4a2 2 0 0 1 2 0l7 4A2 2 0 0 1 21 8z" {...on} />
          <Path d="m3.3 7 8.7 5 8.7-5" {...c} />
          <Path d="M12 22V12" {...c} />
        </>
      );
    case 'history':
      return (
        <>
          <Circle cx="12" cy="12" r="10" {...on} />
          <Path d="M12 6v6l4 2" {...c} />
        </>
      );

    /* ---- actions & markers ---- */
    case 'plus':
      return <Path d="M5 12h14M12 5v14" {...c} />;
    case 'pencil':
      return (
        <>
          <Path d="M17 3a2.85 2.83 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5Z" {...on} />
          <Path d="M15 5l4 4" {...c} />
        </>
      );
    case 'chevron':
      return <Path d="m9 18 6-6-6-6" {...c} />;
    case 'play':
      return <Path d="m6 3 14 9-14 9z" {...on} />;
    case 'check':
      return <Path d="M20 6 9 17l-5-5" {...on} />;
    case 'bolt':
      return (
        <Path
          d="M4 14a1 1 0 0 1-.78-1.63l9.9-10.2a.5.5 0 0 1 .86.46l-1.92 6.02A1 1 0 0 0 13 10h7a1 1 0 0 1 .78 1.63l-9.9 10.2a.5.5 0 0 1-.86-.46l1.92-6.02A1 1 0 0 0 11 14z"
          {...on}
        />
      );
    case 'chip':
      return (
        <>
          <Rect x="4" y="4" width="16" height="16" rx="2" {...on} />
          <Rect x="9" y="9" width="6" height="6" {...c} />
          <Path d="M9 2v2M15 2v2M9 20v2M15 20v2M2 9h2M2 15h2M20 9h2M20 15h2" {...c} />
        </>
      );
    case 'signal':
      return (
        <>
          <Path d="M2 8.8a15 15 0 0 1 20 0" {...c} />
          <Path d="M5 12.6a11 11 0 0 1 14 0" {...c} />
          <Path d="M8.5 16.1a6 6 0 0 1 7 0" {...on} />
          <Path d="M12 20h.01" {...c} />
        </>
      );
  }
}