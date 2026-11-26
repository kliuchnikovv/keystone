// The app's type is Geist: four weights for the text and a mono for the
// technical marks. The mapping from a React Native fontWeight to a family is
// what keeps a `fontWeight: '600'` from falling back to the system font.
import { describe, expect, it } from 'vitest';
import { GEIST_FONTS, familyForWeight } from '../src/ui/fonts';


describe('geist fonts', () => {
  it('maps weights to Geist families and falls back', () => {
    expect(familyForWeight('700')).toBe('Geist_700Bold');
    expect(familyForWeight('600')).toBe('Geist_600SemiBold');
    expect(familyForWeight('500')).toBe('Geist_500Medium');
    expect(familyForWeight('400')).toBe('Geist_400Regular');
    expect(familyForWeight(undefined)).toBe('Geist_400Regular');
  });

  it('exposes the four weights + mono to useFonts', () => {
    expect(Object.keys(GEIST_FONTS)).toEqual(
      expect.arrayContaining(['Geist_400Regular', 'Geist_600SemiBold', 'Geist_700Bold', 'GeistMono_500Medium']),
    );
  });
});
