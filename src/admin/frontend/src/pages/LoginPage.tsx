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
        background: '#F6F9FC',
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
          borderRadius: 8,
          border: '1px solid #E3E8EE',
          boxShadow:
            '0 0 0 1px rgba(50, 50, 93, 0.06), 0 2px 5px rgba(50, 50, 93, 0.08), 0 1px 1.5px rgba(0, 0, 0, 0.04)',
        }}
        styles={{ body: { padding: 32 } }}
      >
        <div style={{ textAlign: 'center', marginBottom: 24 }}>
          <img src="/logo.png" alt="High Cube" style={{ width: 64, marginBottom: 16 }} />
          <Typography.Title level={3} style={{ margin: 0, color: '#0A2540' }}>
            {t('welcome')}
          </Typography.Title>
          <Typography.Paragraph style={{ color: '#425466', marginTop: 8 }}>
            {t('loginSubtitle')}
          </Typography.Paragraph>
        </div>
        <Form
          layout="vertical"
          onFinish={async (values) => {
            await login(values.email, values.password);
            navigate('/');
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
