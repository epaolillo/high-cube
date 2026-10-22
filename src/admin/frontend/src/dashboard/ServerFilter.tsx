import { createContext, useCallback, useContext, useMemo, useState, type ReactNode } from 'react';

// Per-server toggle that filters the Applications column client-side. Every server starts
// checked (its applications are visible). We persist only the UNCHECKED server ids, so a
// brand-new server defaults to checked and the stored set stays small.
const STORAGE_KEY = 'hc_server_filter_off';

function loadOffIds(): Set<number> {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return new Set();
    const parsed = JSON.parse(raw) as unknown;
    return new Set(Array.isArray(parsed) ? parsed.filter((id): id is number => typeof id === 'number') : []);
  } catch {
    return new Set();
  }
}

function persistOffIds(ids: Set<number>) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify([...ids]));
  } catch {
    /* storage may be unavailable (private mode); the filter still works in memory */
  }
}

type ServerFilterValue = {
  // True when this server's applications should be shown.
  isOn: (serverId: number) => boolean;
  toggle: (serverId: number) => void;
};

const ServerFilterContext = createContext<ServerFilterValue | null>(null);

export function ServerFilterProvider({ children }: { children: ReactNode }) {
  const [offIds, setOffIds] = useState<Set<number>>(loadOffIds);

  const isOn = useCallback((serverId: number) => !offIds.has(serverId), [offIds]);

  const toggle = useCallback((serverId: number) => {
    setOffIds((prev) => {
      const next = new Set(prev);
      if (next.has(serverId)) next.delete(serverId);
      else next.add(serverId);
      persistOffIds(next);
      return next;
    });
  }, []);

  const value = useMemo(() => ({ isOn, toggle }), [isOn, toggle]);
  return <ServerFilterContext.Provider value={value}>{children}</ServerFilterContext.Provider>;
}

export function useServerFilter() {
  const value = useContext(ServerFilterContext);
  if (!value) {
    throw new Error('useServerFilter must be used inside ServerFilterProvider');
  }
  return value;
}
