// How much of the screen the keyboard takes.
//
// react-native-web's KeyboardAvoidingView does nothing (it never looks at the
// keyboard), so the app measures it itself: on a phone it is the Keyboard
// events, on the web it is the visual viewport shrinking under the soft keyboard.
import { useEffect, useState } from 'react';
import { Keyboard, Platform } from 'react-native';

/** The height of the keyboard, in points; 0 when it is closed. */
export function useKeyboardInset(): number {
  const [inset, setInset] = useState(0);

  useEffect(() => {
    if (Platform.OS === 'web') {
      const view = typeof window !== 'undefined' ? window.visualViewport : null;
      if (!view) return;
      const measure = () => {
        // The layout viewport stays the same; the visual one shrinks under the keyboard.
        const hidden = Math.max(0, window.innerHeight - view.height - view.offsetTop);
        setInset(Math.round(hidden));
      };
      measure();
      view.addEventListener('resize', measure);
      view.addEventListener('scroll', measure);
      return () => {
        view.removeEventListener('resize', measure);
        view.removeEventListener('scroll', measure);
      };
    }

    const shown = Keyboard.addListener('keyboardDidShow', (e) =>
      setInset(Math.round(e.endCoordinates.height)),
    );
    const hidden = Keyboard.addListener('keyboardDidHide', () => setInset(0));
    const will = Platform.OS === 'ios' ? Keyboard.addListener('keyboardWillShow', (e) =>
      setInset(Math.round(e.endCoordinates.height)),
    ) : null;
    return () => {
      shown.remove();
      hidden.remove();
      will?.remove();
    };
  }, []);

  return inset;
}
