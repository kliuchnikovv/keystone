// @vitest-environment jsdom
// The automations tab folds into sections: each with a count, each that can
// be folded away.
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { Text } from 'react-native';
import { CollapsibleSection } from '../src/ui/CollapsibleSection';
import { ThemeProvider } from '../src/ui/theme';

afterEach(cleanup);

describe('a collapsible section', () => {
  it('shows the title and the count', () => {
    render(
      <ThemeProvider mode="light">
        <CollapsibleSection title="Rules" count={4}>
          <Textish />
        </CollapsibleSection>
      </ThemeProvider>,
    );
    expect(screen.getByText('RULES')).toBeTruthy();
    expect(screen.getByText('4')).toBeTruthy();
  });

  it('starts open, and folds away on a tap', () => {
    render(
      <ThemeProvider mode="light">
        <CollapsibleSection title="Rules" count={4}>
          <Textish />
        </CollapsibleSection>
      </ThemeProvider>,
    );
    expect(screen.getByText('inside')).toBeTruthy();
    fireEvent.click(screen.getByLabelText('Collapse Rules'));
    expect(screen.queryByText('inside')).toBeNull();
    // It opens again.
    fireEvent.click(screen.getByLabelText('Expand Rules'));
    expect(screen.getByText('inside')).toBeTruthy();
  });

  it('can start closed', () => {
    render(
      <ThemeProvider mode="light">
        <CollapsibleSection title="Timers" count={1} initiallyOpen={false}>
          <Textish />
        </CollapsibleSection>
      </ThemeProvider>,
    );
    expect(screen.queryByText('inside')).toBeNull();
  });
});

function Textish() {
  return <Text>inside</Text>;
}
