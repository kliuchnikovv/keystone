// @vitest-environment jsdom
// Geist is the app's base type: a piece of text keeps its own colour and size
// and still wears the family.
import './setup-dom';
import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { Text, TextInput } from '../src/ui/kit';
import { BASE_FONT } from '../src/ui/fonts';

afterEach(cleanup);

describe('the base type', () => {
  it('is on a text that has a style of its own', () => {
    render(<Text testID="t" style={{ color: 'rgb(1, 2, 3)' }}>hi</Text>);
    const style = screen.getByTestId('t').getAttribute('style') ?? '';
    expect(style).toContain(`font-family: ${BASE_FONT}`);
    expect(style).toContain('rgb(1, 2, 3)');
  });

  it('is on an input too', () => {
    render(<TextInput testID="i" accessibilityLabel="Name" />);
    const style = screen.getByTestId('i').getAttribute('style') ?? '';
    expect(style).toContain(`font-family: ${BASE_FONT}`);
  });

  it('a weight brings its own family', () => {
    render(
      <Text testID="b" style={{ fontWeight: '700' }}>
        hi
      </Text>,
    );
    const style = screen.getByTestId('b').getAttribute('style') ?? '';
    expect(style).toContain('font-family: Geist_700Bold');
  });

  it('a text that asks for a family keeps it', () => {
    render(
      <Text testID="m" style={{ fontFamily: 'GeistMono_500Medium' }}>
        hi
      </Text>,
    );
    const style = screen.getByTestId('m').getAttribute('style') ?? '';
    expect(style).toContain('font-family: GeistMono_500Medium');
    expect(style).not.toContain('font-family: Geist_400Regular');
  });
});
