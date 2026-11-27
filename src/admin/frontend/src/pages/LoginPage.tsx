import { Button, Form, Input } from 'antd';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { useAuth } from '../auth/AuthContext';
import { PANEL_LABEL } from '../retro/styles';
import './login.css';

export default function LoginPage() {
  const { t } = useTranslation();
  const { login } = useAuth();
  const navigate = useNavigate();

  return (
    <div className="hc-login hc-skin">
      <div className="hc-login__panel rt-panel">
        <span className="hc-login__band" aria-hidden="true" />
        <div className="hc-login__brand">
          <img className="hc-login__monitor" src="/retro/illustrations/logo-monitor.png" alt="" />
          <div>
            <div className="hc-login__wordmark">{t('appName')}</div>
            <small>{t('controlPlane')}</small>
          </div>
        </div>

        <div className="hc-login__screen rt-bezel">
          <div className="hc-login__prompt">
            &gt; {t('welcome')}
            <span className="hc-login__cursor">_</span>
          </div>
          <div className="hc-login__hint">{t('loginSubtitle')}</div>
        </div>

        <Form
          layout="vertical"
          requiredMark={false}
          onFinish={async (values) => {
            await login(values.email, values.password);
            navigate('/');
          }}
        >
          <Form.Item name="email" label={t('email')} rules={[{ required: true, type: 'email' }]}>
            <Input autoComplete="username" />
          </Form.Item>
          <Form.Item name="password" label={t('password')} rules={[{ required: true }]}>
            <Input.Password autoComplete="current-password" />
          </Form.Item>
          <Button type="primary" htmlType="submit" block className="hc-login__submit">
            {t('login')}
          </Button>
        </Form>

        <footer className="hc-login__foot">
          <span className="hc-login__vent" aria-hidden="true" />
          <span className="hc-login__lamps rt-lcd" aria-hidden="true">
            <span className="rt-lamp" data-color="orange" />
            <span className="rt-lamp" data-color="yellow" />
            <span className="rt-lamp" />
          </span>
          <span className="hc-login__plate rt-inset">{PANEL_LABEL}</span>
        </footer>
      </div>
    </div>
  );
}
