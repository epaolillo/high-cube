import { Button, Form, Input, Select, Typography, message } from 'antd';
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate, useParams } from 'react-router-dom';
import api from '../api/client';

type User = {
  id: number;
  email: string;
  name: string;
  role: string;
  serverIds: number[];
  domainIds: number[];
};

type Option = { value: number; label: string };

export default function UserDetailPage() {
  const { id } = useParams();
  const { t } = useTranslation();
  const navigate = useNavigate();
  const [form] = Form.useForm();
  const [user, setUser] = useState<User>();
  const [serverOptions, setServerOptions] = useState<Option[]>([]);
  const [domainOptions, setDomainOptions] = useState<Option[]>([]);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    if (!id) return;
    api.get(`/users/${id}`).then(({ data }) => {
      setUser(data);
      form.setFieldsValue({
        name: data.name,
        role: data.role,
        serverIds: data.serverIds || [],
        domainIds: data.domainIds || [],
        password: '',
      });
      if ((data.serverIds || []).length) {
        Promise.all(
          data.serverIds.map((serverId: number) =>
            api.get(`/servers/${serverId}`).then(({ data: s }) => ({
              value: s.id,
              label: `${s.name} (${s.ip})`,
            })),
          ),
        ).then(setServerOptions);
      }
      if ((data.domainIds || []).length) {
        api.get('/settings/base-domains', { params: { page: 1, pageSize: 100 } }).then(({ data: page }) => {
          const selected = new Set(data.domainIds as number[]);
          setDomainOptions(
            (page.rows || [])
              .filter((row: { id: number }) => selected.has(row.id))
              .map((row: { id: number; hostname: string }) => ({
                value: row.id,
                label: row.hostname,
              })),
          );
        });
      }
    });
  }, [id, form]);

  const searchServers = async (q: string) => {
    const { data } = await api.get('/servers/autocomplete', { params: { q } });
    setServerOptions(data.rows.map((row: { id: number; label: string }) => ({
      value: row.id,
      label: row.label,
    })));
  };

  const searchDomains = async (q: string) => {
    const { data } = await api.get('/settings/base-domains/autocomplete', { params: { q } });
    setDomainOptions(
      data.rows.map((row: { id: number; label: string }) => ({
        value: row.id,
        label: row.label,
      })),
    );
  };

  if (!user) return null;

  return (
    <div>
      <Typography.Title level={3} style={{ color: '#0A2540', marginBottom: 8 }}>
        {user.name || user.email}
      </Typography.Title>
      <Typography.Paragraph type="secondary" style={{ marginBottom: 24 }}>
        {user.email}
      </Typography.Paragraph>
      <div
        style={{
          background: '#fff',
          border: '1px solid #E3E8EE',
          borderRadius: 8,
          padding: 24,
          maxWidth: 560,
        }}
      >
        <Form
          form={form}
          layout="vertical"
          onFinish={async (values) => {
            setSaving(true);
            try {
              const body: Record<string, unknown> = {
                name: values.name,
                role: values.role,
              };
              if (values.password) {
                body.password = values.password;
              }
              await api.patch(`/users/${id}`, body);
              await api.put(`/users/${id}/servers`, { serverIds: values.serverIds || [] });
              await api.put(`/users/${id}/domains`, { domainIds: values.domainIds || [] });
              message.success(t('saved'));
              navigate('/users');
            } finally {
              setSaving(false);
            }
          }}
        >
          <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="role" label={t('role')} rules={[{ required: true }]}>
            <Select
              options={[
                { value: 'admin', label: t('roles.admin') },
                { value: 'user', label: t('roles.user') },
              ]}
            />
          </Form.Item>
          <Form.Item name="password" label={t('password')} extra={t('passwordOptional')}>
            <Input.Password />
          </Form.Item>
          <Form.Item
            name="serverIds"
            label={t('assignedServers')}
            extra={t('assignedServersHint')}
          >
            <Select
              mode="multiple"
              allowClear
              showSearch
              filterOption={false}
              options={serverOptions}
              onSearch={(q) => {
                void searchServers(q);
              }}
              onFocus={() => {
                void searchServers('');
              }}
              placeholder={t('assignedServers')}
            />
          </Form.Item>
          <Form.Item
            name="domainIds"
            label={t('assignedDomains')}
            extra={t('assignedDomainsHint')}
          >
            <Select
              mode="multiple"
              allowClear
              showSearch
              filterOption={false}
              options={domainOptions}
              onSearch={(q) => {
                void searchDomains(q);
              }}
              onFocus={() => {
                void searchDomains('');
              }}
              placeholder={t('assignedDomains')}
            />
          </Form.Item>
          <Button type="primary" htmlType="submit" loading={saving}>
            {t('save')}
          </Button>
        </Form>
      </div>
    </div>
  );
}
