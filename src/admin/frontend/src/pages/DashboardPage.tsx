import { CloseOutlined, DownOutlined } from '@ant-design/icons';
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
import VpnPage from '../vpn/VpnPage';
import { PANEL_LABEL } from '../retro/styles';
import '../dashboard/dashboard.css';
import '../vpn/vpn.css';

// A select, dropdown, date picker, modal or confirm dialog should consume ESC itself.
function overlayWantsEscape() {
  const nodes = document.querySelectorAll(
    '.ant-modal-wrap, .ant-select-dropdown, .ant-dropdown, .ant-picker-dropdown, .swal2-container',
  );
  return Array.from(nodes).some((node) => {
    if (
      node.classList.contains('ant-select-dropdown-hidden') ||
      node.classList.contains('ant-dropdown-hidden') ||
      node.classList.contains('ant-picker-dropdown-hidden')
    ) {
      return false;
    }
    const style = window.getComputedStyle(node);
    return style.display !== 'none' && style.visibility !== 'hidden';
  });
}

type DrawerState =
  | { kind: 'app'; id: number }
  | { kind: 'server'; id: number }
  | { kind: 'user'; id: number }
  | { kind: 'profile' }
  | { kind: 'integrations' }
  | { kind: 'vpn' }
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

  // Close the drawer on ESC no matter which element inside it has focus. Capture runs
  // before xterm and other widgets stop the event. Nested overlays keep ESC for themselves.
  useEffect(() => {
    if (!drawer) return;
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key !== 'Escape' || overlayWantsEscape()) return;
      event.preventDefault();
      event.stopPropagation();
      closeDrawer();
    };
    window.addEventListener('keydown', handleKeyDown, true);
    return () => window.removeEventListener('keydown', handleKeyDown, true);
  }, [drawer, closeDrawer]);

  const userInitial = (profile?.name || profile?.email || 'U').slice(0, 1).toUpperCase();

  return (
    <DashboardStateProvider>
      <ServerFilterProvider>
      <div className="hc-dash">
        <div className="hc-dash__bar">
          <img className="hc-dash__monitor" src="/retro/illustrations/logo-monitor.png" alt="" />
          <div className="hc-dash__brand">
            <span className="hc-dash__wordmark">{t('appName')}</span>
            <small>{t('controlPlane')}</small>
          </div>
          <span className="hc-dash__stripes rt-stripes" aria-hidden="true" />
          <span className="hc-dash__vent" aria-hidden="true" />
          <div className="hc-dash__spacer" />
          <span className="hc-dash__lamps rt-lcd" aria-hidden="true">
            <span className="rt-lamp" data-color="orange" />
            <span className="rt-lamp" data-color="yellow" />
            <span className="rt-lamp" data-color="orange" />
          </span>
          {isAdmin && (
            <button
              type="button"
              className="hc-dash__navbtn rt-btn rt-btn--dark"
              data-active={drawer?.kind === 'vpn'}
              onClick={() => setDrawer({ kind: 'vpn' })}
            >
              {t('vpn')}
            </button>
          )}
          <Dropdown
            trigger={['click']}
            menu={{
              items: [
                { key: 'profile', label: t('profile'), onClick: () => setDrawer({ kind: 'profile' }) },
                { key: 'logout', label: t('logout'), onClick: logout },
              ],
            }}
          >
            <button type="button" className="hc-dash__user rt-btn rt-btn--bezel">
              <span className="hc-dash__avatar rt-key rt-key--purple">{userInitial}</span>
              <span>{profile?.email || profile?.name}</span>
              <DownOutlined />
            </button>
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
          rootClassName="hc-skin"
          width="min(1200px, 94vw)"
          open={!!drawer}
          onClose={closeDrawer}
          destroyOnClose
          title={drawer?.kind === 'vpn' ? null : <span className="hc-drawer__band" aria-hidden="true" />}
          extra={drawer?.kind === 'vpn' ? null : <span className="hc-drawer__plate rt-raised">{PANEL_LABEL}</span>}
          closable={drawer?.kind !== 'vpn'}
          closeIcon={<CloseOutlined />}
        >
          {drawer?.kind === 'app' && <ApplicationDetailPage id={drawer.id} />}
          {drawer?.kind === 'server' && <ServerDetailPage id={drawer.id} />}
          {drawer?.kind === 'user' && <UserDetailPage id={drawer.id} onSaved={closeDrawer} />}
          {drawer?.kind === 'profile' && <ProfilePage />}
          {drawer?.kind === 'integrations' && <IntegrationsPage />}
          {drawer?.kind === 'vpn' && <VpnPage onClose={closeDrawer} />}
        </Drawer>
      </div>
      </ServerFilterProvider>
    </DashboardStateProvider>
  );
}
