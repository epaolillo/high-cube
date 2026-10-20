import { Badge, Button, Form, Input, Modal, Select, Typography, message } from 'antd';
import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { DataTable } from '../components/DataTable';

type GithubApp = {
  id: number;
  name: string;
  ownerLogin: string;
  status: string;
  htmlUrl: string;
  slug: string;
  ownerEmail: string;
};

const statusColor: Record<string, string> = {
  pending: 'default',
  registered: 'processing',
  installed: 'success',
};

export default function IntegrationsPage() {
  const { t } = useTranslation();
  const { isAdmin } = useAuth();
  const [open, setOpen] = useState(false);
  const [refreshKey, setRefreshKey] = useState(0);
  const [accountType, setAccountType] = useState<'personal' | 'organization'>('personal');
  const [form] = Form.useForm();

  const registerApp = async () => {
    const values = await form.validateFields();
    const origin = window.location.origin;
    const body: Record<string, string> = {
      name: values.name,
      origin,
    };
    if (accountType === 'organization') {
      body.organization = values.organization;
    }
    const { data } = await api.post('/github-apps/manifest', body);
    sessionStorage.setItem(`hc_github_state_${data.id}`, data.state);

    // GitHub expects a classic form POST with a JSON string in "manifest".
    const formEl = document.createElement('form');
    formEl.method = 'POST';
    formEl.action = data.postUrl;
    formEl.acceptCharset = 'UTF-8';
    formEl.style.display = 'none';
    const input = document.createElement('input');
    input.type = 'text';
    input.name = 'manifest';
    input.value = JSON.stringify(data.manifest);
    formEl.appendChild(input);
    document.body.appendChild(formEl);
    formEl.submit();
  };

  const columns = [
    { title: t('name'), dataIndex: 'name' },
    { title: t('githubAccount'), dataIndex: 'ownerLogin' },
    {
      title: t('status'),
      dataIndex: 'status',
      render: (value: string) => (
        <Badge
          status={(statusColor[value] as any) || 'default'}
          text={t(`githubStatus.${value}`, { defaultValue: value })}
        />
      ),
    },
    ...(isAdmin
      ? [{ title: t('owner'), dataIndex: 'ownerEmail' }]
      : []),
  ];

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 16 }}>
        <div>
          <Typography.Title level={3} style={{ margin: 0, color: '#0A2540' }}>
            {t('integrations')}
          </Typography.Title>
          <Typography.Paragraph style={{ color: '#425466' }}>
            {t('integrationsSubtitle')}
          </Typography.Paragraph>
        </div>
        <Button type="primary" onClick={() => setOpen(true)}>
          {t('registerGithubApp')}
        </Button>
      </div>
      <div style={{ background: '#fff', border: '1px solid #E3E8EE', borderRadius: 8, padding: 24 }}>
        <DataTable<GithubApp>
          endpoint="/github-apps"
          refreshKey={refreshKey}
          columns={columns as any}
          actions={[
            {
              key: 'install',
              label: t('install'),
              onClick: (row) => {
                if (row.slug) {
                  window.open(`https://github.com/apps/${row.slug}/installations/new`, '_blank');
                } else {
                  message.warning(t('completeRegistrationFirst'));
                }
              },
            },
            {
              key: 'open',
              label: t('openOnGithub'),
              onClick: (row) => {
                if (row.htmlUrl) window.open(row.htmlUrl, '_blank');
              },
            },
            {
              key: 'sync',
              label: t('syncInstallation'),
              onClick: async (row) => {
                await api.post(`/github-apps/${row.id}/installation/sync`);
                message.success(t('installationSynced'));
                setRefreshKey((n) => n + 1);
              },
            },
            {
              key: 'delete',
              label: t('delete'),
              danger: true,
              onClick: async (row) => {
                await api.delete(`/github-apps/${row.id}`);
                message.success(t('deleted'));
                setRefreshKey((n) => n + 1);
              },
            },
          ]}
          bulkActions={[
            {
              key: 'sync',
              label: t('syncInstallation'),
              onClick: async (rows) => {
                await Promise.all(rows.map((row) => api.post(`/github-apps/${row.id}/installation/sync`)));
                message.success(t('installationSynced'));
                setRefreshKey((n) => n + 1);
              },
            },
            {
              key: 'delete',
              label: t('delete'),
              danger: true,
              onClick: async (rows) => {
                await Promise.all(rows.map((row) => api.delete(`/github-apps/${row.id}`)));
                message.success(t('deleted'));
                setRefreshKey((n) => n + 1);
              },
            },
          ]}
        />
      </div>
      <Modal
        open={open}
        title={t('registerGithubApp')}
        onCancel={() => setOpen(false)}
        okText={t('continueOnGithub')}
        okButtonProps={{ type: 'primary' }}
        onOk={() => void registerApp()}
      >
        <Typography.Paragraph type="secondary">{t('registerGithubAppHint')}</Typography.Paragraph>
        <Form form={form} layout="vertical">
          <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
            <Input placeholder="High Cube" />
          </Form.Item>
          <Form.Item label={t('githubAccountType')}>
            <Select
              value={accountType}
              onChange={setAccountType}
              options={[
                { value: 'personal', label: t('personalAccount') },
                { value: 'organization', label: t('organization') },
              ]}
            />
          </Form.Item>
          {accountType === 'organization' && (
            <Form.Item name="organization" label={t('organizationLogin')} rules={[{ required: true }]}>
              <Input placeholder="my-org" />
            </Form.Item>
          )}
        </Form>
      </Modal>
    </div>
  );
}
