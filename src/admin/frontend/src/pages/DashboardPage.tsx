import { Drawer, Dropdown } from 'antd';
import { useCallback, useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../auth/AuthContext';
import { connectPresence } from '../presence/presenceClient';
import { ApplicationsSector } from '../dashboard/ApplicationsSector';
import { DashboardStateProvider } from '../dashboard/DashboardState';
import { ServerFilterProvider } from '../dashboard/ServerFilter';
import { ServersSector } from '../dashboard/ServersSector';
import { SettingsSector } from '../dashboard/SettingsSector';
import { UsersSector } from '../dashboard/UsersSector';
import ApplicationDetailPage from './ApplicationDetailPage';
import IntegrationsPage from './IntegrationsPage';
import ProfilePage from './ProfilePage';
import ServerDetailPage from './ServerDetailPage';
import UserDetailPage from './UserDetailPage';
import '../dashboard/dashboard.css';

type DrawerState =
  | { kind: 'app'; id: number }
  | { kind: 'server'; id: number }
  | { kind: 'user'; id: number }
  | { kind: 'profile' }
  | { kind: 'integrations' }
  | null;

export default function DashboardPage() {
  const { t } = useTranslation();
  const { profile, isAdmin, logout } = useAuth();
  const [drawer, setDrawer] = useState<DrawerState>(null);
  const closeDrawer = useCallback(() => setDrawer(null), []);

  // Report this user as online for as long as the dashboard is open.
  useEffect(() => {
    const presence = connectPresence();
    return () => presence.close();
  }, []);

  // Close the drawer when pressing the [ESC] key.
  useEffect(() => {
    if (!drawer) return;
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') closeDrawer();
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [drawer, closeDrawer]);

  const userInitial = (profile?.name || profile?.email || 'U').slice(0, 1).toUpperCase();

  return (
    <DashboardStateProvider>
      <ServerFilterProvider>
      <div className="hc-dash">
        <div className="hc-dash__bar">
          <div className="hc-dash__brand">
            <img src="/logo.png" alt="" />
            <span>{t('appName')}</span>
            <small>{t('controlPlane')}</small>
          </div>
          <div className="hc-dash__spacer" />
          <Dropdown
            trigger={['click']}
            menu={{
              items: [
                { key: 'profile', label: t('profile'), onClick: () => setDrawer({ kind: 'profile' }) },
                { key: 'logout', label: t('logout'), onClick: logout },
              ],
            }}
          >
            <div className="hc-dash__user">
              <span className="hc-dash__avatar">{userInitial}</span>
              <span>{profile?.email || profile?.name}</span>
            </div>
          </Dropdown>
        </div>

        <div className="hc-dash__board hc-scroll">
          <ServersSector readOnly={!isAdmin} onOpen={(id) => setDrawer({ kind: 'server', id })} />
          <ApplicationsSector
            isAdmin={isAdmin}
            onOpen={(id) => setDrawer({ kind: 'app', id })}
            onOpenIntegrations={() => setDrawer({ kind: 'integrations' })}
          />
          <SettingsSector readOnly={!isAdmin} />
          <UsersSector readOnly={!isAdmin} onOpen={(id) => setDrawer({ kind: 'user', id })} />
        </div>

        <Drawer
          className="hc-drawer"
          width="min(1200px, 94vw)"
          open={!!drawer}
          onClose={closeDrawer}
          destroyOnClose
          title={null}
        >
          {drawer?.kind === 'app' && <ApplicationDetailPage id={drawer.id} />}
          {drawer?.kind === 'server' && <ServerDetailPage id={drawer.id} />}
          {drawer?.kind === 'user' && <UserDetailPage id={drawer.id} onSaved={closeDrawer} />}
          {drawer?.kind === 'profile' && <ProfilePage />}
          {drawer?.kind === 'integrations' && <IntegrationsPage />}
        </Drawer>
      </div>
      </ServerFilterProvider>
    </DashboardStateProvider>
  );
}
