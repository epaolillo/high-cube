import { Button, Card, Form, Input, Typography } from 'antd';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { useAuth } from '../auth/AuthContext';

export default function LoginPage() {
  const { t } = useTranslation();
  const { login } = useAuth();
  const navigate = useNavigate();

  return (
    <div
      style={{
        minHeight: '100vh',
        background: 'var(--hc-bg)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        padding: 16,
      }}
    >
      <Card
        style={{
          width: '100%',
          maxWidth: 400,
          borderRadius: 0,
          border: '2px solid var(--hc-border)',
          boxShadow: 'var(--hc-shadow)',
        }}
        styles={{ body: { padding: 32 } }}
      >
        <div style={{ textAlign: 'center', marginBottom: 24 }}>
          <img
            src="/logo.png"
            alt="High Cube"
            style={{ width: 64, marginBottom: 16, imageRendering: 'pixelated' }}
          />
          <Typography.Title
            level={3}
            className="hc-pixel-display"
            style={{ margin: 0, color: 'var(--hc-text)' }}
          >
            {t('welcome')}
          </Typography.Title>
          <Typography.Paragraph style={{ color: 'var(--hc-text-dim)', marginTop: 8 }}>
            {t('loginSubtitle')}
          </Typography.Paragraph>
        </div>
        <Form
          layout="vertical"
          onFinish={async (values) => {
            await login(values.email, values.password);
            navigate('/applications');
          }}
        >
          <Form.Item name="email" label={t('email')} rules={[{ required: true, type: 'email' }]}>
            <Input />
          </Form.Item>
          <Form.Item name="password" label={t('password')} rules={[{ required: true }]}>
            <Input.Password />
          </Form.Item>
          <Button type="primary" htmlType="submit" block style={{ height: 40, fontWeight: 500 }}>
            {t('login')}
          </Button>
        </Form>
      </Card>
    </div>
  );
}
