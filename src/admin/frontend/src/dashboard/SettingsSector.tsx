import { DeleteOutlined, EditOutlined, PlusOutlined, RocketOutlined } from '@ant-design/icons';
import { Form, Input, InputNumber, List, Modal, Progress, Tag, Tooltip, Typography, message } from 'antd';
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import Swal from 'sweetalert2';
import api from '../api/client';
import { confirmDestructive } from '../components/confirmDestructive';
import { Pager } from './Pager';
import { PAGE_SIZE, usePagedResource } from './usePagedResource';

type DomainRow = { id: number; hostname: string };
type RedeployStatus = 'pending' | 'deploying' | 'success' | 'failed' | 'timeout' | 'error';
type RedeployItem = { id: number; name: string; status: RedeployStatus };

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

const REDEPLOY_COLOR: Record<RedeployStatus, string> = {
  pending: 'default',
  deploying: 'processing',
  success: 'green',
  failed: 'red',
  timeout: 'orange',
  error: 'red',
};

export function SettingsSector() {
  const { t } = useTranslation();
  const [page, setPage] = useState(1);
  const [refreshKey, setRefreshKey] = useState(0);
  const bump = () => setRefreshKey((n) => n + 1);
  const [sel, setSel] = useState<Set<number>>(new Set());
  const [domainOpen, setDomainOpen] = useState(false);
  const [domainForm] = Form.useForm();
  const [ttl, setTtl] = useState<number | null>(null);
  const [metricsModal, setMetricsModal] = useState<{ ttl: number } | null>(null);

  // Redeploy-all progress state.
  const [redeployOpen, setRedeployOpen] = useState(false);
  const [redeploying, setRedeploying] = useState(false);
  const [redeployItems, setRedeployItems] = useState<RedeployItem[]>([]);

  const { rows, total } = usePagedResource<DomainRow>('/settings/base-domains', { page, refreshKey });

  useEffect(() => {
    api
      .get('/settings/metrics', { suppressErrorToast: true })
      .then(({ data }) => setTtl(data.metricsTtlDays))
      .catch(() => undefined);
  }, [refreshKey]);

  useEffect(() => setSel(new Set()), [page]);

  const toggle = (id: number) =>
    setSel((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  const submitDomain = async () => {
    const values = await domainForm.validateFields();
    await api.post('/settings/base-domains', values);
    message.success(t('saved'));
    setDomainOpen(false);
    bump();
  };
  const removeDomain = (d: DomainRow) =>
    confirmDestructive(t, { text: d.hostname }).then((ok) => {
      if (!ok) return;
      return api.delete(`/settings/base-domains/${d.id}`).then(() => {
        message.success(t('deleted'));
        bump();
      });
    });
  const bulkDelete = async () => {
    if (!(await confirmDestructive(t, { text: t('confirmDeleteManyBody', { count: sel.size }) }))) return;
    await Promise.all([...sel].map((id) => api.delete(`/settings/base-domains/${id}`)));
    message.success(t('deleted'));
    setSel(new Set());
    bump();
  };
  const submitMetrics = async () => {
    if (!metricsModal) return;
    await api.patch('/settings/metrics', { metricsTtlDays: metricsModal.ttl });
    message.success(t('saved'));
    setMetricsModal(null);
    bump();
  };

  // Sequential redeploy of every application, ported from the classic settings.
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
      apps = (data.rows || []).map((app: { id: number; name: string }) => ({ id: app.id, name: app.name }));
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

  return (
    <section className="hc-sector hc-sector--settings">
      <div className="hc-sector__head">
        <span className="hc-sector__title">{t('settings')}</span>
        <span className="hc-sector__count">{total}</span>
        <span className="hc-sector__spacer" />
        <button type="button" className="hc-ghostbtn" onClick={startRedeployAll}>
          <RocketOutlined />
          {t('redeployAllShort')}
        </button>
        <button
          type="button"
          className="hc-addbtn"
          onClick={() => {
            domainForm.resetFields();
            setDomainOpen(true);
          }}
        >
          <PlusOutlined />
          {t('baseDomainsShort')}
        </button>
      </div>

      {/* Metrics retention */}
      <div className="hc-row">
        <div className="hc-row__head">
          <div className="hc-readout">
            <b>{ttl ?? '—'}</b>
            <span>{t('metricsTtl')}</span>
          </div>
          <span className="hc-sector__spacer" />
          <div className="hc-acts">
            <Tooltip title={t('edit')}>
              <button
                type="button"
                className="hc-iconbtn"
                onClick={() => setMetricsModal({ ttl: ttl ?? 7 })}
              >
                <EditOutlined />
              </button>
            </Tooltip>
          </div>
        </div>
      </div>

      <div className="hc-bulkbar">
        <span>{t('selectedCount', { count: sel.size })}</span>
        <span className="hc-bulkbar__spacer" />
        <button type="button" className="hc-linkbtn" data-tone="danger" disabled={sel.size === 0} onClick={bulkDelete}>
          {t('delete')}
        </button>
        <button type="button" className="hc-linkbtn" disabled={sel.size === 0} onClick={() => setSel(new Set())}>
          {t('clearSelection')}
        </button>
      </div>

      {rows.map((d) => (
        <div className="hc-row" key={d.id}>
          <div className="hc-row__head">
            <input
              type="checkbox"
              className="hc-row__check"
              checked={sel.has(d.id)}
              onChange={() => toggle(d.id)}
            />
            <span className="hc-row__name" title={d.hostname}>
              {d.hostname}
            </span>
            <span className="hc-row__sub">{t('baseDomain')}</span>
            <div className="hc-acts">
              <Tooltip title={t('delete')}>
                <button
                  type="button"
                  className="hc-iconbtn"
                  data-tone="danger"
                  onClick={() => void removeDomain(d)}
                >
                  <DeleteOutlined />
                </button>
              </Tooltip>
            </div>
          </div>
        </div>
      ))}

      <Pager page={page} pageSize={PAGE_SIZE} total={total} onChange={setPage} />

      <Modal
        title={t('baseDomains')}
        open={domainOpen}
        onCancel={() => setDomainOpen(false)}
        okButtonProps={{ type: 'primary' }}
        okText={t('save')}
        cancelText={t('cancel')}
        onOk={submitDomain}
        destroyOnClose
      >
        <Form form={domainForm} layout="vertical">
          <Form.Item name="hostname" label="Hostname" rules={[{ required: true }]}>
            <Input placeholder="example.com" />
          </Form.Item>
        </Form>
      </Modal>

      <Modal
        title={t('metricsTtl')}
        open={!!metricsModal}
        onCancel={() => setMetricsModal(null)}
        okButtonProps={{ type: 'primary' }}
        okText={t('save')}
        cancelText={t('cancel')}
        onOk={submitMetrics}
      >
        <InputNumber
          min={1}
          style={{ width: '100%' }}
          value={metricsModal?.ttl}
          onChange={(v) => setMetricsModal({ ttl: v || 7 })}
        />
      </Modal>

      <Modal
        open={redeployOpen}
        title={t('redeployAll')}
        maskClosable={false}
        closable={!redeploying}
        onCancel={() => !redeploying && setRedeployOpen(false)}
        footer={null}
      >
        <Typography.Text type="secondary">
          {t('redeployAllProgress', { done: redeployDone, total: redeployItems.length })}
        </Typography.Text>
        <Progress
          percent={redeployItems.length ? Math.round((redeployDone / redeployItems.length) * 100) : 0}
          style={{ marginBottom: 12 }}
        />
        <List
          size="small"
          style={{ maxHeight: 360, overflowY: 'auto' }}
          dataSource={redeployItems}
          renderItem={(item) => (
            <List.Item>
              <span>{item.name}</span>
              <Tag color={REDEPLOY_COLOR[item.status]}>{t(`redeployStatus_${item.status}`)}</Tag>
            </List.Item>
          )}
        />
      </Modal>
    </section>
  );
}
