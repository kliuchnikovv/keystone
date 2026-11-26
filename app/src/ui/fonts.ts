// The app's type: Geist. Four weights for the text, Geist Mono for the
// technical marks (a token, a hex, a unit).
//
// The fonts are loaded through expo-font; until they are ready the app renders
// with the system font — a missing font must never leave a blank screen.
// The sub-paths, not the package root: the root pulls in every weight and
// every italic — two megabytes of glyphs the app never draws.
import { Geist_400Regular } from '@expo-google-fonts/geist/400Regular';
import { Geist_500Medium } from '@expo-google-fonts/geist/500Medium';
import { Geist_600SemiBold } from '@expo-google-fonts/geist/600SemiBold';
import { Geist_700Bold } from '@expo-google-fonts/geist/700Bold';
import { GeistMono_500Medium } from '@expo-google-fonts/geist-mono/500Medium';

/** The fonts the app loads, in the shape expo-font's useFonts wants. */
export const GEIST_FONTS = {
  Geist_400Regular,
  Geist_500Medium,
  Geist_600SemiBold,
  Geist_700Bold,
  GeistMono_500Medium,
};

/** The base family: the one every piece of text starts from. */
export const BASE_FONT = 'Geist_400Regular';

/** The mono family, for the technical marks. */
export const MONO_FONT = 'GeistMono_500Medium';

/** The family a React Native fontWeight means. */
export function familyForWeight(weight?: string): string {
  switch (weight) {
    case '700':
    case '800':
    case '900':
      return 'Geist_700Bold';
    case '600':
      return 'Geist_600SemiBold';
    case '500':
      return 'Geist_500Medium';
    default:
      return BASE_FONT;
  }
}
