/**
 * Приближение цветовой температуры к RGB для градиента слайдера.
 * Диапазон осмысленный: 1500K..8000K.
 */
export function kelvinToRgb(k: number): { r: number; g: number; b: number } {
  const temp = Math.max(1500, Math.min(8000, k)) / 100;

  let r: number;
  let g: number;
  let b: number;

  if (temp <= 66) {
    r = 255;
    g = 99.4708025861 * Math.log(temp) - 161.1195681661;
    b = temp <= 19 ? 0 : 138.5177312231 * Math.log(temp - 10) - 305.0447927307;
  } else {
    r = 329.698727446 * Math.pow(temp - 60, -0.1332047592);
    g = 288.1221695283 * Math.pow(temp - 60, -0.0755148492);
    b = 255;
  }

  return {
    r: Math.max(0, Math.min(255, Math.round(r))),
    g: Math.max(0, Math.min(255, Math.round(g))),
    b: Math.max(0, Math.min(255, Math.round(b))),
  };
}

export function kelvinToCss(k: number): string {
  const { r, g, b } = kelvinToRgb(k);
  return `rgb(${r}, ${g}, ${b})`;
}
