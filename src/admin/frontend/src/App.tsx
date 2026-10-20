import { ConfigProvider } from 'antd';
import { BrowserRouter, Navigate, Route, Routes } from 'react-router-dom';
import { AuthProvider, useAuth } from './auth/AuthContext';
import AdminLayout from './layout/AdminLayout';
import LoginPage from './pages/LoginPage';
import ServersPage from './pages/ServersPage';
import ServerDetailPage from './pages/ServerDetailPage';
import ApplicationsPage from './pages/ApplicationsPage';
import ApplicationDetailPage from './pages/ApplicationDetailPage';
import UsersPage from './pages/UsersPage';
import UserDetailPage from './pages/UserDetailPage';
import IntegrationsPage from './pages/IntegrationsPage';
import GithubCallbackPage from './pages/GithubCallbackPage';
import GithubSetupPage from './pages/GithubSetupPage';
import SettingsPage from './pages/SettingsPage';
import ProfilePage from './pages/ProfilePage';
import { makeTheme } from './theme';
import { ThemeModeProvider, useThemeMode } from './theme-mode';
import './i18n';

function PrivateRoute({ children }: { children: JSX.Element }) {
  const { token } = useAuth();
  if (!token) return <Navigate to="/login" replace />;
  return children;
}

function AdminRoute({ children }: { children: JSX.Element }) {
  const { isAdmin, profile } = useAuth();
  if (!profile) return null;
  if (!isAdmin) return <Navigate to="/applications" replace />;
  return children;
}

function ThemedApp() {
  const { mode } = useThemeMode();
  return (
    <ConfigProvider theme={makeTheme(mode)}>
      <AuthProvider>
        <BrowserRouter>
          <Routes>
            <Route path="/login" element={<LoginPage />} />
            <Route
              path="/"
              element={
                <PrivateRoute>
                  <AdminLayout />
                </PrivateRoute>
              }
            >
              <Route index element={<Navigate to="/applications" replace />} />
              <Route
                path="servers"
                element={
                  <AdminRoute>
                    <ServersPage />
                  </AdminRoute>
                }
              />
              <Route
                path="servers/:id"
                element={
                  <AdminRoute>
                    <ServerDetailPage />
                  </AdminRoute>
                }
              />
              <Route path="applications" element={<ApplicationsPage />} />
              <Route path="applications/:id" element={<ApplicationDetailPage />} />
              <Route path="integrations" element={<IntegrationsPage />} />
              <Route path="integrations/github/:id/callback" element={<GithubCallbackPage />} />
              <Route path="integrations/github/:id/setup" element={<GithubSetupPage />} />
              <Route
                path="users"
                element={
                  <AdminRoute>
                    <UsersPage />
                  </AdminRoute>
                }
              />
              <Route
                path="users/:id"
                element={
                  <AdminRoute>
                    <UserDetailPage />
                  </AdminRoute>
                }
              />
              <Route
                path="settings"
                element={
                  <AdminRoute>
                    <SettingsPage />
                  </AdminRoute>
                }
              />
              <Route path="profile" element={<ProfilePage />} />
            </Route>
          </Routes>
        </BrowserRouter>
      </AuthProvider>
    </ConfigProvider>
  );
}

export default function App() {
  return (
    <ThemeModeProvider>
      <ThemedApp />
    </ThemeModeProvider>
  );
}
