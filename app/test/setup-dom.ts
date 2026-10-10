// What react-native-web expects from a browser and jsdom does not provide.
const w = globalThis as unknown as { matchMedia?: unknown; IS_REACT_ACT_ENVIRONMENT?: boolean };
if (!w.matchMedia) {
  w.matchMedia = () => ({
    matches: false,
    media: '',
    addEventListener() {},
    removeEventListener() {},
    addListener() {},
    removeListener() {},
    dispatchEvent: () => false,
  });
}
w.IS_REACT_ACT_ENVIRONMENT = true;
