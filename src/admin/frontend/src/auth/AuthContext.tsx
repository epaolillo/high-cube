import { createContext, useContext, useEffect, useMemo, useState, ReactNode } from 'react';
import api from '../api/client';

export type Profile = {
  id: number;
  email: string;
  name: string;
  locale: string;
  avatarPath: string;
  role: 'admin' | 'user';
  roleId: number;
  serverIds?: number[];
  domainIds?: number[];
};

type AuthContextValue = {
  token: string | null;
  profile: Profile | null;
  login: (email: string, password: string) => Promise<void>;
  logout: () => void;
  refreshProfile: () => Promise<void>;
  isAdmin: boolean;
};

const AuthContext = createContext<AuthContextValue | undefined>(undefined);

export function AuthProvider({ children }: { children: ReactNode }) {
  const [token, setToken] = useState<string | null>(localStorage.getItem('hc_token'));
  const [profile, setProfile] = useState<Profile | null>(null);

  const refreshProfile = async () => {
    if (!localStorage.getItem('hc_token')) {
      setProfile(null);
      return;
    }
    const { data } = await api.get<Profile>('/profile');
    setProfile(data);
    if (data.locale) {
      localStorage.setItem('hc_locale', data.locale);
    }
  };

  useEffect(() => {
    if (token) {
      refreshProfile().catch(() => {
        setToken(null);
        localStorage.removeItem('hc_token');
      });
    }
  }, [token]);

  const value = useMemo<AuthContextValue>(
    () => ({
      token,
      profile,
      login: async (email, password) => {
        const { data } = await api.post<{ token: string }>('/auth/login', { email, password });
        localStorage.setItem('hc_token', data.token);
        setToken(data.token);
      },
      logout: () => {
        localStorage.removeItem('hc_token');
        setToken(null);
        setProfile(null);
      },
      refreshProfile,
      isAdmin: profile?.role === 'admin',
    }),
    [token, profile],
  );

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

export function useAuth() {
  const ctx = useContext(AuthContext);
  if (!ctx) throw new Error('useAuth outside provider');
  return ctx;
}
