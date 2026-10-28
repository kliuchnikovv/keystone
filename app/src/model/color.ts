// Colour maths: the protocol carries CIE 1931 x/y (0..65535 each), people pick colours
// as hue/saturation/brightness. Pure functions, no React — the conversions are exact
// enough for a lamp and easy to test.
export interface Hsv {
  /** 0..360 */
  h: number;
  /** 0..1 */
  s: number;
  /** 0..1 */
  v: number;
}

/** sRGB -> CIE XYZ (D65), the usual matrix. */
function rgbToXyz(r: number, g: number, b: number): [number, number, number] {
  return [
    0.4124564 * r + 0.3575761 * g + 0.1804375 * b,
    0.2126729 * r + 0.7151522 * g + 0.072175 * b,
    0.0193339 * r + 0.119192 * g + 0.9503041 * b,
  ];
}

function xyzToRgb(x: number, y: number, z: number): [number, number, number] {
  return [
    3.2404542 * x - 1.5371385 * y - 0.4985314 * z,
    -0.969266 * x + 1.8760108 * y + 0.041556 * z,
    0.0556434 * x - 0.2040259 * y + 1.0572252 * z,
  ];
}

export function hsvToRgb({ h, s, v }: Hsv): [number, number, number] {
  const c = v * s;
  const hp = ((h % 360) + 360) % 360 / 60;
  const x = c * (1 - Math.abs((hp % 2) - 1));
  let rgb: [number, number, number] = [0, 0, 0];
  if (hp < 1) rgb = [c, x, 0];
  else if (hp < 2) rgb = [x, c, 0];
  else if (hp < 3) rgb = [0, c, x];
  else if (hp < 4) rgb = [0, x, c];
  else if (hp < 5) rgb = [x, 0, c];
  else rgb = [c, 0, x];
  const m = v - c;
  return [rgb[0] + m, rgb[1] + m, rgb[2] + m];
}

export function rgbToHsv(r: number, g: number, b: number): Hsv {
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const d = max - min;
  let h = 0;
  if (d > 0) {
    if (max === r) h = 60 * (((g - b) / d) % 6);
    else if (max === g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
  }
  if (h < 0) h += 360;
  return { h, s: max === 0 ? 0 : d / max, v: max };
}

/** HSV to the wire's colour pair, scaled to 0..65535 like Matter and Zigbee do.
 * The pair is a chromaticity: it carries no brightness (that is the separate
 * `brightness` attribute), so `v` does not change the result. */
export function hsvToXy(colour: Hsv): [number, number] {
  const [r, g, b] = hsvToRgb({ ...colour, v: 1 });
  const [X, Y, Z] = rgbToXyz(r, g, b);
  const sum = X + Y + Z;
  if (sum <= 0) return [0, 0];
  const x = Math.min(1, Math.max(0, X / sum));
  const y = Math.min(1, Math.max(0, Y / sum));
  return [Math.round(x * 65535), Math.round(y * 65535)];
}

/** The wire's colour pair back to HSV, so the picker can show what the lamp has now.
 * The pair says nothing about brightness, so `v` comes back as 1. */
export function xyToHsv(x: number, y: number): Hsv {
  const cx = Math.min(65535, Math.max(0, x)) / 65535;
  const cy = Math.min(65535, Math.max(0, y)) / 65535;
  if (cx + cy <= 0) return { h: 0, s: 0, v: 1 };
  const Y = 1;
  // x = X/(X+Y+Z), y = Y/(X+Y+Z): with Y fixed, X = x/y and Z = (1-x-y)/y.
  const X = (cx / cy) * Y;
  const Z = ((1 - cx - cy) / cy) * Y;
  let [r, g, b] = xyzToRgb(X, Y, Z);
  // The pair fixes the hue and the saturation, not the level: normalise it away.
  const max = Math.max(r, g, b);
  if (max <= 0) return { h: 0, s: 0, v: 1 };
  r = Math.min(1, Math.max(0, r / max));
  g = Math.min(1, Math.max(0, g / max));
  b = Math.min(1, Math.max(0, b / max));
  const hsv = rgbToHsv(r, g, b);
  return { h: hsv.h, s: hsv.s, v: 1 };
}

/** CSS colour for the swatch in the picker. */
export function hsvToCss(colour: Hsv): string {
  const [r, g, b] = hsvToRgb(colour);
  const c = (x: number) => Math.round(x * 255);
  return `rgb(${c(r)}, ${c(g)}, ${c(b)})`;
}
