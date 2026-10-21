import { ConfigProvider } from 'antd';
import { BrowserRouter, Navigate, Route, Routes } from 'react-router-dom';
import { AuthProvider, useAuth } from './auth/AuthContext';
import LoginPage from './pages/LoginPage';
import DashboardPage from './pages/DashboardPage';
import GithubCallbackPage from './pages/GithubCallbackPage';
import GithubSetupPage from './pages/GithubSetupPage';
import { stripeTheme } from './theme';
import './i18n';

function PrivateRoute({ children }: { children: JSX.Element }) {
  const { token } = useAuth();
  if (!token) return <Navigate to="/login" replace />;
  return children;
}

export default function App() {
  return (
    <ConfigProvider theme={stripeTheme}>
      <AuthProvider>
        <BrowserRouter>
          <Routes>
            <Route path="/login" element={<LoginPage />} />
            {/* GitHub OAuth redirect targets must stay routable. */}
            <Route
              path="/integrations/github/:id/callback"
              element={
                <PrivateRoute>
                  <GithubCallbackPage />
                </PrivateRoute>
              }
            />
            <Route
              path="/integrations/github/:id/setup"
              element={
                <PrivateRoute>
                  <GithubSetupPage />
                </PrivateRoute>
              }
            />
            {/* Everything else is the single administration screen. */}
            <Route
              path="/"
              element={
                <PrivateRoute>
                  <DashboardPage />
                </PrivateRoute>
              }
            />
            <Route path="*" element={<Navigate to="/" replace />} />
          </Routes>
        </BrowserRouter>
      </AuthProvider>
    </ConfigProvider>
  );
}
