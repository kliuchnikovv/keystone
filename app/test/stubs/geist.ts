// The Geist packages require their .ttf files; Node cannot parse those, and a
// test has no use for the real glyphs. The stub hands out the same names with
// dummy asset ids, so the font map keeps its shape.
const asset = (name: string) => ({ name, uri: name });

export const Geist_100Thin = asset('Geist_100Thin');
export const Geist_200ExtraLight = asset('Geist_200ExtraLight');
export const Geist_300Light = asset('Geist_300Light');
export const Geist_400Regular = asset('Geist_400Regular');
export const Geist_500Medium = asset('Geist_500Medium');
export const Geist_600SemiBold = asset('Geist_600SemiBold');
export const Geist_700Bold = asset('Geist_700Bold');
export const Geist_800ExtraBold = asset('Geist_800ExtraBold');
export const Geist_900Black = asset('Geist_900Black');
export const Geist_100Thin_Italic = asset('Geist_100Thin_Italic');
export const Geist_200ExtraLight_Italic = asset('Geist_200ExtraLight_Italic');
export const Geist_300Light_Italic = asset('Geist_300Light_Italic');
export const Geist_400Regular_Italic = asset('Geist_400Regular_Italic');
export const Geist_500Medium_Italic = asset('Geist_500Medium_Italic');
export const Geist_600SemiBold_Italic = asset('Geist_600SemiBold_Italic');
export const Geist_700Bold_Italic = asset('Geist_700Bold_Italic');
export const Geist_800ExtraBold_Italic = asset('Geist_800ExtraBold_Italic');
export const Geist_900Black_Italic = asset('Geist_900Black_Italic');
export const __metadata__ = {};

export function useFonts(_map: Record<string, unknown>) {
  return [true, null];
}
