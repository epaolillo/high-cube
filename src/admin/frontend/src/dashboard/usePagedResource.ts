import { useEffect, useState } from 'react';
import api from '../api/client';

// Backend only accepts these page sizes.
export const PAGE_SIZE = 10;

export type PagedParams = Record<string, string | number | undefined>;

type Options = {
  page: number;
  pageSize?: number;
  params?: PagedParams;
  pollMs?: number;
  enabled?: boolean;
  /** Bump to force an immediate reload (after a create/edit/delete). */
  refreshKey?: number;
};

export type Paged<T> = { rows: T[]; total: number; loaded: boolean };

/**
 * Fetches a `{ rows, total }` list endpoint with pagination support and keeps it
 * near real-time by polling. Polling requests silence the global error toast so
 * one bad poll never spams the UI.
 */
export function usePagedResource<T>(endpoint: string, options: Options): Paged<T> {
  const { page, pageSize = PAGE_SIZE, params, pollMs = 4000, enabled = true, refreshKey = 0 } = options;
  const [state, setState] = useState<Paged<T>>({ rows: [], total: 0, loaded: false });
  const paramsKey = JSON.stringify(params ?? {});

  useEffect(() => {
    if (!enabled) {
      setState({ rows: [], total: 0, loaded: true });
      return undefined;
    }
    let stopped = false;

    const load = async () => {
      try {
        const { data } = await api.get(endpoint, {
          params: { page, pageSize, ...params },
          suppressErrorToast: true,
        });
        if (!stopped) setState({ rows: data.rows || [], total: data.total || 0, loaded: true });
      } catch {
        if (!stopped) setState((prev) => ({ ...prev, loaded: true }));
      }
    };

    load();
    const timer = window.setInterval(load, pollMs);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [endpoint, page, pageSize, paramsKey, enabled, pollMs, refreshKey]);

  return state;
}
