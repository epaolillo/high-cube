import { Avatar, Dropdown, Layout, Menu, Typography } from 'antd';
import { Outlet, useLocation, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../auth/AuthContext';
import './AdminLayout.css';

const { Header, Sider, Content } = Layout;

export default function AdminLayout() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const location = useLocation();
  const { profile, logout, isAdmin } = useAuth();

  const selected =
    ['/servers', '/applications', '/integrations', '/users', '/settings', '/profile'].find(
      (path) => location.pathname === path || location.pathname.startsWith(`${path}/`),
    ) || location.pathname;

  const upper = [
    isAdmin && { key: '/servers', label: t('servers') },
    { key: '/applications', label: t('applications') },
    { key: '/integrations', label: t('integrations') },
  ].filter(Boolean) as { key: string; label: string }[];

  const lower = isAdmin
    ? [
        { key: '/users', label: t('users') },
        { key: '/settings', label: t('settings') },
      ]
    : [];

  return (
    <Layout className="hc-shell">
      <Sider width={240} className="hc-sider" theme="light">
        <button type="button" className="hc-brand" onClick={() => navigate('/applications')}>
          <img src="/logo.png" alt="" className="hc-brand-logo" />
          <div className="hc-brand-text">
            <span className="hc-brand-name">{t('appName')}</span>
            <span className="hc-brand-tag">{t('controlPlane')}</span>
          </div>
        </button>

        <div className="hc-sider-body">
          <div className="hc-sider-group">
            <Typography.Text className="hc-sider-label">{t('workspace')}</Typography.Text>
            <Menu
              mode="inline"
              selectedKeys={[selected]}
              items={upper}
              onClick={({ key }) => navigate(key)}
              className="hc-menu"
            />
          </div>
          {lower.length > 0 && (
            <div className="hc-sider-group hc-sider-group-bottom">
              <Typography.Text className="hc-sider-label">{t('adminNav')}</Typography.Text>
              <Menu
                mode="inline"
                selectedKeys={[selected]}
                items={lower}
                onClick={({ key }) => navigate(key)}
                className="hc-menu"
              />
            </div>
          )}
        </div>
      </Sider>

      <Layout className="hc-main">
        <Header className="hc-header">
          <Typography.Text className="hc-header-hint">
            {profile?.email || profile?.name}
          </Typography.Text>
          <Dropdown
            menu={{
              items: [
                { key: 'profile', label: t('profile'), onClick: () => navigate('/profile') },
                { key: 'logout', label: t('logout'), onClick: logout },
              ],
            }}
          >
            <button type="button" className="hc-avatar-btn">
              <Avatar style={{ background: '#635BFF' }}>
                {(profile?.name || profile?.email || 'U').slice(0, 1).toUpperCase()}
              </Avatar>
            </button>
          </Dropdown>
        </Header>
        <Content className="hc-content">
          <Outlet />
        </Content>
      </Layout>
    </Layout>
  );
}
