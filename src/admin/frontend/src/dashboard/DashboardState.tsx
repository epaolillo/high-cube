import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from 'react';
import api from '../api/client';

// Backend list endpoints used by the dashboard only accept this page size.
export const PAGE_SIZE = 10;

const POLL_MS = 4000;

export type Paged<T> = { rows: T[]; total: number };

type ServerParams = { page: number; q: string };
type ApplicationParams = { page: number; type: string; order: 'asc' | 'desc' };
type UserParams = { page: number; order: 'asc' | 'desc' };
type DomainParams = { page: number };

type Params = {
  servers: ServerParams;
  applications: ApplicationParams;
  users: UserParams;
  domains: DomainParams;
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
  setServersParams: (page: number, q: string) => void;
  setApplicationsParams: (page: number, type: string, order: 'asc' | 'desc') => void;
  setUsersParams: (page: number, order: 'asc' | 'desc') => void;
  setDomainsParams: (page: number) => void;
};

const empty = <T,>(): Paged<T> => ({ rows: [], total: 0 });

const defaultParams: Params = {
  servers: { page: 1, q: '' },
  applications: { page: 1, type: 'other', order: 'asc' },
  users: { page: 1, order: 'asc' },
  domains: { page: 1 },
};

const DashboardStateContext = createContext<DashboardStateValue | null>(null);

function sameServers(a: ServerParams, b: ServerParams) {
  return a.page === b.page && a.q === b.q;
}

function sameApplications(a: ApplicationParams, b: ApplicationParams) {
  return a.page === b.page && a.type === b.type && a.order === b.order;
}

function sameUsers(a: UserParams, b: UserParams) {
  return a.page === b.page && a.order === b.order;
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

  const setServersParams = useCallback((page: number, q: string) => {
    const next = { page, q };
    if (sameServers(paramsRef.current.servers, next)) return;
    paramsRef.current = { ...paramsRef.current, servers: next };
    setTick((value) => value + 1);
  }, []);

  const setApplicationsParams = useCallback((page: number, type: string, order: 'asc' | 'desc') => {
    const next = { page, type, order };
    if (sameApplications(paramsRef.current.applications, next)) return;
    paramsRef.current = { ...paramsRef.current, applications: next };
    setTick((value) => value + 1);
  }, []);

  const setUsersParams = useCallback((page: number, order: 'asc' | 'desc') => {
    const next = { page, order };
    if (sameUsers(paramsRef.current.users, next)) return;
    paramsRef.current = { ...paramsRef.current, users: next };
    setTick((value) => value + 1);
  }, []);

  const setDomainsParams = useCallback((page: number) => {
    if (paramsRef.current.domains.page === page) return;
    paramsRef.current = { ...paramsRef.current, domains: { page } };
    setTick((value) => value + 1);
  }, []);

  useEffect(() => {
    let stopped = false;
    const load = async () => {
      const params = paramsRef.current;
      try {
        const { data: body } = await api.get('/state', {
          params: {
            serversPage: params.servers.page,
            serversQ: params.servers.q || undefined,
            appsPage: params.applications.page,
            appsType: params.applications.type || undefined,
            appsOrder: params.applications.order,
            usersPage: params.users.page,
            usersOrder: params.users.order,
            domainsPage: params.domains.page,
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
      value={{ data, refresh, setServersParams, setApplicationsParams, setUsersParams, setDomainsParams }}
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
