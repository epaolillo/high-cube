import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from 'react';
import api from '../api/client';

const POLL_MS = 4000;

export type Paged<T> = { rows: T[]; total: number };

type ServerParams = { q: string };
type ApplicationParams = { type: string; order: 'asc' | 'desc' };
type UserParams = { order: 'asc' | 'desc' };

type Params = {
  servers: ServerParams;
  applications: ApplicationParams;
  users: UserParams;
};

type DashboardData = {
  applications: Paged<unknown>;
  servers: Paged<unknown>;
  users: Paged<unknown>;
  baseDomains: Paged<unknown>;
  metricsTtlDays: number | null;
  onlineUserIds: number[];
};

type DashboardStateValue = {
  data: DashboardData;
  refresh: () => void;
  setServersParams: (q: string) => void;
  setApplicationsParams: (type: string, order: 'asc' | 'desc') => void;
  setUsersParams: (order: 'asc' | 'desc') => void;
};

const empty = <T,>(): Paged<T> => ({ rows: [], total: 0 });

const defaultParams: Params = {
  servers: { q: '' },
  applications: { type: 'other', order: 'asc' },
  users: { order: 'asc' },
};

const DashboardStateContext = createContext<DashboardStateValue | null>(null);

function sameApplications(a: ApplicationParams, b: ApplicationParams) {
  return a.type === b.type && a.order === b.order;
}

export function DashboardStateProvider({ children }: { children: ReactNode }) {
  const paramsRef = useRef<Params>(defaultParams);
  const [tick, setTick] = useState(0);
  const [data, setData] = useState<DashboardData>({
    applications: empty(),
    servers: empty(),
    users: empty(),
    baseDomains: empty(),
    metricsTtlDays: null,
    onlineUserIds: [],
  });

  const refresh = useCallback(() => setTick((value) => value + 1), []);

  const setServersParams = useCallback((q: string) => {
    if (paramsRef.current.servers.q === q) return;
    paramsRef.current = { ...paramsRef.current, servers: { q } };
    setTick((value) => value + 1);
  }, []);

  const setApplicationsParams = useCallback((type: string, order: 'asc' | 'desc') => {
    const next = { type, order };
    if (sameApplications(paramsRef.current.applications, next)) return;
    paramsRef.current = { ...paramsRef.current, applications: next };
    setTick((value) => value + 1);
  }, []);

  const setUsersParams = useCallback((order: 'asc' | 'desc') => {
    if (paramsRef.current.users.order === order) return;
    paramsRef.current = { ...paramsRef.current, users: { order } };
    setTick((value) => value + 1);
  }, []);

  useEffect(() => {
    let stopped = false;
    const load = async () => {
      const params = paramsRef.current;
      try {
        const { data: body } = await api.get('/state', {
          params: {
            serversQ: params.servers.q || undefined,
            appsType: params.applications.type || undefined,
            appsOrder: params.applications.order,
            usersOrder: params.users.order,
          },
          suppressErrorToast: true,
        });
        if (stopped) return;
        setData({
          applications: {
            rows: body.applications?.rows || [],
            total: body.applications?.total || 0,
          },
          servers: {
            rows: body.servers?.rows || [],
            total: body.servers?.total || 0,
          },
          users: {
            rows: body.users?.rows || [],
            total: body.users?.total || 0,
          },
          baseDomains: {
            rows: body.baseDomains?.rows || [],
            total: body.baseDomains?.total || 0,
          },
          metricsTtlDays: typeof body.metricsTtlDays === 'number' ? body.metricsTtlDays : null,
          onlineUserIds: body.onlineUserIds || [],
        });
      } catch {
        /* one failed poll must not wipe the board */
      }
    };
    void load();
    const timer = window.setInterval(() => {
      void load();
    }, POLL_MS);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [tick]);

  return (
    <DashboardStateContext.Provider
      value={{ data, refresh, setServersParams, setApplicationsParams, setUsersParams }}
    >
      {children}
    </DashboardStateContext.Provider>
  );
}

export function useDashboardState() {
  const value = useContext(DashboardStateContext);
  if (!value) {
    throw new Error('useDashboardState must be used inside DashboardStateProvider');
  }
  return value;
}
