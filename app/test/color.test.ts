import { describe, expect, it } from 'vitest';
import { hsvToCss, hsvToRgb, hsvToXy, rgbToHsv, xyToHsv } from '../src/model/color';

describe('colour maths', () => {
  it('hues come out as the colours people expect', () => {
    expect(hsvToRgb({ h: 0, s: 1, v: 1 })).toEqual([1, 0, 0]);
    expect(hsvToRgb({ h: 120, s: 1, v: 1 })[1]).toBeCloseTo(1, 5);
    expect(hsvToRgb({ h: 240, s: 1, v: 1 })[2]).toBeCloseTo(1, 5);
    expect(hsvToCss({ h: 0, s: 1, v: 1 })).toBe('rgb(255, 0, 0)');
  });

  it('the wire pair is the CIE x/y of the colour', () => {
    // Pure red sits at the edge of the CIE diagram: x ≈ 0.64, y ≈ 0.33.
    const [x, y] = hsvToXy({ h: 0, s: 1, v: 1 });
    expect(x / 65535).toBeCloseTo(0.64, 2);
    expect(y / 65535).toBeCloseTo(0.33, 2);
    // Green: x ≈ 0.30, y ≈ 0.60.
    const [gx, gy] = hsvToXy({ h: 120, s: 1, v: 1 });
    expect(gx / 65535).toBeCloseTo(0.30, 2);
    expect(gy / 65535).toBeCloseTo(0.60, 2);
  });

  it('a colour survives the round trip', () => {
    for (const hsv of [
      { h: 0, s: 1, v: 1 },
      { h: 45, s: 0.8, v: 1 },
      { h: 200, s: 0.6, v: 1 },
      { h: 300, s: 0.4, v: 1 },
    ]) {
      const back = xyToHsv(...hsvToXy(hsv));
      expect(back.h).toBeCloseTo(hsv.h, 0);
      expect(back.s).toBeCloseTo(hsv.s, 1);
    }
  });

  it('the pair says nothing about brightness, so it comes back as full', () => {
    expect(hsvToXy({ h: 30, s: 1, v: 0.2 })).toEqual(hsvToXy({ h: 30, s: 1, v: 1 }));
    expect(xyToHsv(...hsvToXy({ h: 30, s: 1, v: 1 })).v).toBe(1);
  });

  it('black and white are the corners', () => {
    expect(rgbToHsv(0, 0, 0)).toEqual({ h: 0, s: 0, v: 0 });
    const white = xyToHsv(...hsvToXy({ h: 0, s: 0, v: 1 }));
    expect(white.s).toBeCloseTo(0, 2);
    expect(white.v).toBeCloseTo(1, 2);
  });

  it('a pair from the wire is read as a colour', () => {
    const [x, y] = hsvToXy({ h: 210, s: 0.9, v: 1 });
    const back = xyToHsv(x, y);
    expect(back.h).toBeCloseTo(210, 0);
  });
});
