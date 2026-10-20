import { Button, Form, Input, InputNumber, List, Modal, Progress, Tabs, Tag, Typography, message } from 'antd';
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import Swal from 'sweetalert2';
import api from '../api/client';
import { DataTable } from '../components/DataTable';

type RedeployStatus = 'pending' | 'deploying' | 'success' | 'failed' | 'timeout' | 'error';
type RedeployItem = { id: number; name: string; status: RedeployStatus };

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

export default function SettingsPage() {
  const { t } = useTranslation();
  const [ttl, setTtl] = useState(7);
  const [open, setOpen] = useState(false);
  const [refreshKey, setRefreshKey] = useState(0);
  const [form] = Form.useForm();
  const [redeployOpen, setRedeployOpen] = useState(false);
  const [redeploying, setRedeploying] = useState(false);
  const [redeployItems, setRedeployItems] = useState<RedeployItem[]>([]);

  useEffect(() => {
    api.get('/settings/metrics').then(({ data }) => setTtl(data.metricsTtlDays));
  }, []);

  // Redeploys every application one at a time, waiting for each build to reach a
  // terminal state before starting the next so the server is not flooded with builds.
  const startRedeployAll = async () => {
    const confirmed = await Swal.fire({
      title: t('redeployAllConfirmTitle'),
      text: t('redeployAllConfirmBody'),
      icon: 'warning',
      showCancelButton: true,
      confirmButtonText: t('redeployAllConfirmOk'),
      cancelButtonText: t('cancel'),
      confirmButtonColor: '#635BFF',
      cancelButtonColor: '#8898AA',
      focusCancel: true,
    });
    if (!confirmed.isConfirmed) return;

    let apps: { id: number; name: string }[] = [];
    try {
      const { data } = await api.get('/applications', { params: { page: 1, pageSize: 1000 } });
      apps = (data.rows || []).map((app: { id: number; name: string }) => ({
        id: app.id,
        name: app.name,
      }));
    } catch {
      message.error(t('redeployAllLoadError'));
      return;
    }

    const updateStatus = (id: number, status: RedeployStatus) =>
      setRedeployItems((items) => items.map((it) => (it.id === id ? { ...it, status } : it)));

    setRedeployItems(apps.map((app) => ({ ...app, status: 'pending' as RedeployStatus })));
    setRedeployOpen(true);
    setRedeploying(true);

    for (const app of apps) {
      updateStatus(app.id, 'deploying');
      try {
        const { data: deployment } = await api.post(`/applications/${app.id}/deployments`, {});
        let status: string = deployment.status;
        const deploymentId = deployment.id;
        // 25-minute guard so a stuck build never blocks the whole queue.
        const deadline = Date.now() + 25 * 60 * 1000;
        while (status !== 'success' && status !== 'failed') {
          if (Date.now() > deadline) {
            status = 'timeout';
            break;
          }
          await sleep(3000);
          try {
            const { data: current } = await api.get(`/deployments/${deploymentId}`);
            status = current.status;
          } catch {
            /* transient polling error: keep waiting */
          }
        }
        updateStatus(app.id, status as RedeployStatus);
      } catch {
        updateStatus(app.id, 'error');
      }
    }

    setRedeploying(false);
    message.success(t('redeployAllDone'));
  };

  const redeployDone = redeployItems.filter((it) =>
    ['success', 'failed', 'timeout', 'error'].includes(it.status),
  ).length;
  const redeployTotal = redeployItems.length;
  const redeployStatusColor: Record<RedeployStatus, string> = {
    pending: 'default',
    deploying: 'processing',
    success: 'green',
    failed: 'red',
    timeout: 'orange',
    error: 'red',
  };

  return (
    <div>
      <Typography.Title level={3} style={{ color: '#0A2540' }}>
        {t('settings')}
      </Typography.Title>
      <Tabs
        defaultActiveKey="general"
        items={[
          {
            key: 'general',
            label: t('general'),
            children: (
              <>
              <div style={{ background: 'var(--hc-surface)', border: '1px solid var(--hc-border-soft)', borderRadius: 0, padding: 24 }}>
                <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 16 }}>
                  <Typography.Title level={5} style={{ margin: 0 }}>
                    {t('baseDomains')}
                  </Typography.Title>
                  <Button type="primary" onClick={() => setOpen(true)}>
                    {t('create')}
                  </Button>
                </div>
                <DataTable
                  endpoint="/settings/base-domains"
                  refreshKey={refreshKey}
                  columns={[{ title: 'Hostname', dataIndex: 'hostname' }]}
                  actions={[
                    {
                      key: 'delete',
                      label: t('delete'),
                      danger: true,
                      onClick: async (row: any) => {
                        await api.delete(`/settings/base-domains/${row.id}`);
                        setRefreshKey((n) => n + 1);
                      },
                    },
                  ]}
                  bulkActions={[
                    {
                      key: 'delete',
                      label: t('delete'),
                      danger: true,
                      onClick: async (rows: { id: number }[]) => {
                        await Promise.all(rows.map((row) => api.delete(`/settings/base-domains/${row.id}`)));
                        message.success(t('deleted'));
                        setRefreshKey((n) => n + 1);
                      },
                    },
                  ]}
                />
              </div>
              <div
                style={{
                  background: '#fff',
                  border: '1px solid #E3E8EE',
                  borderRadius: 0,
                  padding: 24,
                  marginTop: 24,
                }}
              >
                <Typography.Title level={5} style={{ marginTop: 0 }}>
                  {t('redeployAll')}
                </Typography.Title>
                <Typography.Paragraph type="secondary">{t('redeployAllHint')}</Typography.Paragraph>
                <Button type="primary" onClick={startRedeployAll} loading={redeploying}>
                  {t('redeployAllStart')}
                </Button>
              </div>
              </>
            ),
          },
          {
            key: 'metrics',
            label: t('metrics'),
            children: (
              <div style={{ background: 'var(--hc-surface)', border: '1px solid var(--hc-border-soft)', borderRadius: 0, padding: 24, maxWidth: 420 }}>
                <Form
                  layout="vertical"
                  onFinish={async () => {
                    await api.patch('/settings/metrics', { metricsTtlDays: ttl });
                    message.success(t('save'));
                  }}
                >
                  <Form.Item label={t('metricsTtl')}>
                    <InputNumber min={1} value={ttl} onChange={(v) => setTtl(v || 7)} style={{ width: '100%' }} />
                  </Form.Item>
                  <Button type="primary" htmlType="submit">
                    {t('save')}
                  </Button>
                </Form>
              </div>
            ),
          },
        ]}
      />
      <Modal
        open={open}
        title={t('baseDomains')}
        onCancel={() => setOpen(false)}
        okButtonProps={{ type: 'primary' }}
        onOk={async () => {
          const values = await form.validateFields();
          await api.post('/settings/base-domains', values);
          setOpen(false);
          form.resetFields();
          setRefreshKey((n) => n + 1);
        }}
      >
        <Form form={form} layout="vertical">
          <Form.Item name="hostname" label="Hostname" rules={[{ required: true }]}>
            <Input placeholder="example.com" />
          </Form.Item>
        </Form>
      </Modal>
      <Modal
        open={redeployOpen}
        title={t('redeployAll')}
        maskClosable={false}
        closable={!redeploying}
        onCancel={() => !redeploying && setRedeployOpen(false)}
        footer={
          redeploying ? null : (
            <Button type="primary" onClick={() => setRedeployOpen(false)}>
              {t('close')}
            </Button>
          )
        }
      >
        <Typography.Text type="secondary">
          {t('redeployAllProgress', { done: redeployDone, total: redeployTotal })}
        </Typography.Text>
        <Progress
          percent={redeployTotal ? Math.round((redeployDone / redeployTotal) * 100) : 0}
          style={{ marginBottom: 12 }}
        />
        <List
          size="small"
          style={{ maxHeight: 360, overflowY: 'auto' }}
          dataSource={redeployItems}
          renderItem={(item) => (
            <List.Item>
              <span>{item.name}</span>
              <Tag color={redeployStatusColor[item.status]}>{t(`redeployStatus_${item.status}`)}</Tag>
            </List.Item>
          )}
        />
      </Modal>
    </div>
  );
}
