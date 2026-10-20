import { createContext, useContext, useEffect, useMemo, useState } from 'react';
import type { ReactNode } from 'react';
import type { ThemeMode } from './theme';

type ThemeModeContextValue = {
  mode: ThemeMode;
  toggle: () => void;
  setMode: (mode: ThemeMode) => void;
};

const STORAGE_KEY = 'hc-theme';

const ThemeModeContext = createContext<ThemeModeContextValue | null>(null);

function initialMode(): ThemeMode {
  // Light is the default skin; honor a saved preference when present.
  if (typeof window === 'undefined') return 'light';
  const saved = window.localStorage.getItem(STORAGE_KEY);
  return saved === 'dark' ? 'dark' : 'light';
}

export function ThemeModeProvider({ children }: { children: ReactNode }) {
  const [mode, setMode] = useState<ThemeMode>(initialMode);

  // Expose the active mode to CSS via a root attribute so variables can switch.
  useEffect(() => {
    document.documentElement.dataset.theme = mode;
    window.localStorage.setItem(STORAGE_KEY, mode);
  }, [mode]);

  const value = useMemo<ThemeModeContextValue>(
    () => ({
      mode,
      setMode,
      toggle: () => setMode((prev) => (prev === 'light' ? 'dark' : 'light')),
    }),
    [mode],
  );

  return <ThemeModeContext.Provider value={value}>{children}</ThemeModeContext.Provider>;
}

export function useThemeMode(): ThemeModeContextValue {
  const ctx = useContext(ThemeModeContext);
  if (!ctx) throw new Error('useThemeMode must be used within ThemeModeProvider');
  return ctx;
}
