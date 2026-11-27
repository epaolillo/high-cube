import { Button, Form, Input, Select, Typography, message } from 'antd';
import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../auth/AuthContext';
import api from '../api/client';
import i18n from '../i18n';
import { retroCard } from '../retro/styles';
import { DeviceEnrollModal } from '../vpn/DeviceEnrollModal';

export default function ProfilePage() {
  const { t } = useTranslation();
  const { profile, refreshProfile, isAdmin } = useAuth();
  const [enrollOpen, setEnrollOpen] = useState(false);

  if (!profile) return null;

  return (
    <div style={{ maxWidth: 480 }}>
      <Typography.Title level={3} style={{ color: 'var(--rt-ink)' }}>
        {t('profile')}
      </Typography.Title>
      <div style={retroCard}>
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
      {isAdmin && (
        <div style={{ ...retroCard, marginTop: 14 }}>
          <Typography.Title level={5} style={{ color: 'var(--rt-ink)', marginTop: 0 }}>
            {t('vpn')}
          </Typography.Title>
          <Typography.Paragraph type="secondary">{t('vpnProfileHint')}</Typography.Paragraph>
          <Button onClick={() => setEnrollOpen(true)}>{t('vpnAddThisPc')}</Button>
          <DeviceEnrollModal open={enrollOpen} onClose={() => setEnrollOpen(false)} />
        </div>
      )}
    </div>
  );
}
