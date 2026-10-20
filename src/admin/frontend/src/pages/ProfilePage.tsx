import { Button, Form, Input, Select, Typography, message } from 'antd';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../auth/AuthContext';
import api from '../api/client';
import i18n from '../i18n';

export default function ProfilePage() {
  const { t } = useTranslation();
  const { profile, refreshProfile } = useAuth();

  if (!profile) return null;

  return (
    <div style={{ maxWidth: 480 }}>
      <Typography.Title level={3} style={{ color: '#0A2540' }}>
        {t('profile')}
      </Typography.Title>
      <div style={{ background: '#fff', border: '1px solid #E3E8EE', borderRadius: 8, padding: 24 }}>
        <Form
          layout="vertical"
          initialValues={{ name: profile.name, locale: profile.locale || 'en' }}
          onFinish={async (values) => {
            await api.patch('/profile', values);
            localStorage.setItem('hc_locale', values.locale);
            await i18n.changeLanguage(values.locale);
            await refreshProfile();
            message.success(t('save'));
          }}
        >
          <Form.Item label={t('email')}>
            <Input value={profile.email} disabled />
          </Form.Item>
          <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="locale" label="Language">
            <Select options={[{ value: 'en', label: 'English' }, { value: 'es', label: 'Español' }]} />
          </Form.Item>
          <Button type="primary" htmlType="submit">
            {t('save')}
          </Button>
        </Form>
      </div>
    </div>
  );
}
