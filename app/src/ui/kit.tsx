// The app's building blocks, with Geist as the base type.
//
// Everything else comes from react-native unchanged; only Text and TextInput
// change: they put the base family in front of whatever style they are given,
// and they turn a fontWeight into the Geist family that carries it — the
// platform has no way to know that "700" means Geist_700Bold.
import { forwardRef, type ElementRef } from 'react';
import {
  StyleSheet,
  Text as RNText,
  TextInput as RNTextInput,
  type TextInputProps,
  type TextProps,
} from 'react-native';
import { BASE_FONT, MONO_FONT, familyForWeight } from './fonts';

export * from 'react-native';
export type { TextProps, TextInputProps } from 'react-native';

/** The family a piece of text starts from. */
export const BASE_FAMILY = BASE_FONT;

/** The mono family, for the technical marks. */
export const MONO_FAMILY = MONO_FONT;

const base = { fontFamily: BASE_FONT };

/** The style a piece of text is drawn with: the base family, then its own,
 *  and then the family that carries its weight. */
function withBase(style: TextProps['style']): TextProps['style'] {
  const flat = StyleSheet.flatten(style ?? {}) as { fontWeight?: string; fontFamily?: string } | undefined;
  const weight = flat?.fontWeight;
  // A text that asks for a family of its own (the mono ones) keeps it; a text
  // that only asks for a weight gets the Geist family that carries it.
  const family = flat?.fontFamily ?? (weight ? familyForWeight(weight) : undefined);
  return [base, style, family ? { fontFamily: family } : null];
}

/** Text, with the base family under its own style. */
export const Text = forwardRef<ElementRef<typeof RNText>, TextProps>(function Text({ style, ...rest }, ref) {
  return <RNText {...rest} ref={ref} style={withBase(style)} />;
});

/** An input, with the base family under its own style. */
export const TextInput = forwardRef<ElementRef<typeof RNTextInput>, TextInputProps>(function TextInput({ style, ...rest }, ref) {
  return <RNTextInput {...rest} ref={ref} style={withBase(style)} />;
});
