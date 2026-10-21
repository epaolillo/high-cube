import { DeleteOutlined, EditOutlined, KeyOutlined, PlusOutlined, SlidersOutlined } from '@ant-design/icons';
import { Form, Input, Modal, Tooltip, Typography, message } from 'antd';
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { confirmDestructive } from '../components/confirmDestructive';
import { confirmRegenerateToken } from '../components/confirmRegenerateToken';
import { MetricDisplay } from '../components/MetricDisplay';
import { Pager } from './Pager';
import { fetchServerSnapshot, type ServerMetricSnapshot } from './serverMetrics';
import { PAGE_SIZE, usePagedResource } from './usePagedResource';

type ServerRow = { id: number; name: string; ip: string; status: string; lastSeenAt?: string };

function showAgentToken(t: (k: string) => string, token: string) {
  Modal.info({
    title: t('agentTokenTitle'),
    width: 560,
    content: (
      <div>
        <Typography.Paragraph type="secondary">{t('agentTokenHint')}</Typography.Paragraph>
        <Typography.Paragraph copyable code style={{ wordBreak: 'break-all' }}>
          {token}
        </Typography.Paragraph>
      </div>
    ),
  });
}

export function ServersSector({ onOpen }: { onOpen: (id: number) => void }) {
  const { t } = useTranslation();
  const [page, setPage] = useState(1);
  const [q, setQ] = useState('');
  const [refreshKey, setRefreshKey] = useState(0);
  const bump = () => setRefreshKey((n) => n + 1);
  const [sel, setSel] = useState<Set<number>>(new Set());
  const [modal, setModal] = useState<{ id?: number } | null>(null);
  const [form] = Form.useForm();

  const { rows, total } = usePagedResource<ServerRow>('/servers', {
    page,
    params: { q: q || undefined },
    refreshKey,
  });

  // Live resource snapshots for the servers on the current page.
  const [metrics, setMetrics] = useState<Record<number, ServerMetricSnapshot>>({});
  const idsKey = rows.map((r) => r.id).join(',');
  useEffect(() => {
    if (!rows.length) {
      setMetrics({});
      return undefined;
    }
    let stopped = false;
    const load = async () => {
      const entries = await Promise.all(
        rows.map(async (r) => [r.id, await fetchServerSnapshot(r.id)] as const),
      );
      if (!stopped) setMetrics(Object.fromEntries(entries));
    };
    load();
    const timer = window.setInterval(load, 4000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [idsKey]);

  // Selection resets whenever the visible set changes.
  useEffect(() => setSel(new Set()), [page, q]);

  const toggle = (id: number) =>
    setSel((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  const addServer = () => {
    form.resetFields();
    setModal({});
  };
  const editServer = (s: ServerRow) => {
    form.setFieldsValue({ name: s.name, ip: s.ip });
    setModal({ id: s.id });
  };
  const submit = async () => {
    const values = await form.validateFields();
    if (modal?.id) {
      await api.patch(`/servers/${modal.id}`, values);
      message.success(t('saved'));
      setModal(null);
      bump();
    } else {
      const { data } = await api.post('/servers', values);
      setModal(null);
      bump();
      showAgentToken(t, data.agentToken);
    }
  };
  const regenerate = (s: ServerRow) =>
    confirmRegenerateToken(t, s.name, async () => {
      const { data } = await api.post(`/servers/${s.id}/token`);
      message.success(t('agentTokenRotated'));
      showAgentToken(t, data.agentToken);
    });
  const removeOne = (s: ServerRow) =>
    confirmDestructive(t, { text: s.name }).then((ok) => {
      if (!ok) return;
      return api.delete(`/servers/${s.id}`).then(() => {
        message.success(t('serverRemoved'));
        bump();
      });
    });
  const bulkDelete = async () => {
    if (!(await confirmDestructive(t, { text: t('confirmDeleteManyBody', { count: sel.size }) }))) return;
    await Promise.all([...sel].map((id) => api.delete(`/servers/${id}`)));
    message.success(t('serverRemoved'));
    setSel(new Set());
    bump();
  };

  return (
    <section className="hc-sector hc-sector--servers">
      <div className="hc-sector__head">
        <span className="hc-sector__title">{t('servers')}</span>
        <span className="hc-sector__count">{total}</span>
        <span className="hc-sector__spacer" />
        <button type="button" className="hc-addbtn" onClick={addServer}>
          <PlusOutlined />
          {t('create')}
        </button>
      </div>

      <div className="hc-controls">
        <Input
          className="hc-search"
          size="small"
          allowClear
          placeholder={t('name')}
          value={q}
          onChange={(e) => {
            setQ(e.target.value);
            setPage(1);
          }}
        />
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

      {rows.length === 0 ? (
        <div className="hc-sector__empty">—</div>
      ) : (
        rows.map((s) => (
          <div className="hc-row" key={s.id}>
            <div className="hc-row__head">
              <input
                type="checkbox"
                className="hc-row__check"
                checked={sel.has(s.id)}
                onChange={() => toggle(s.id)}
              />
              <span className="hc-dot" data-on={s.status === 'online'} />
              <span className="hc-row__name" title={s.name}>
                {s.name}
              </span>
              <span className="hc-row__sub">{s.ip}</span>
              <div className="hc-acts">
                <Tooltip title={t('edit')}>
                  <button type="button" className="hc-iconbtn" onClick={() => editServer(s)}>
                    <EditOutlined />
                  </button>
                </Tooltip>
                <Tooltip title={t('open')}>
                  <button type="button" className="hc-iconbtn" onClick={() => onOpen(s.id)}>
                    <SlidersOutlined />
                  </button>
                </Tooltip>
                <Tooltip title={t('regenerateToken')}>
                  <button type="button" className="hc-iconbtn" onClick={() => regenerate(s)}>
                    <KeyOutlined />
                  </button>
                </Tooltip>
                <Tooltip title={t('delete')}>
                  <button
                    type="button"
                    className="hc-iconbtn"
                    data-tone="danger"
                    onClick={() => void removeOne(s)}
                  >
                    <DeleteOutlined />
                  </button>
                </Tooltip>
              </div>
            </div>
            <div className="hc-row__metric">
              <MetricDisplay
                cpuPercent={metrics[s.id]?.cpuPercent}
                memoryUsedBytes={metrics[s.id]?.memoryUsedBytes}
                memoryLimitBytes={metrics[s.id]?.memoryLimitBytes}
                diskUsedBytes={metrics[s.id]?.diskUsedBytes}
                diskTotalBytes={metrics[s.id]?.diskTotalBytes}
                gpuPresent={metrics[s.id]?.gpuPresent}
                gpuPercent={metrics[s.id]?.gpuPercent}
                gpuMemoryUsedBytes={metrics[s.id]?.gpuMemoryUsedBytes}
                gpuMemoryTotalBytes={metrics[s.id]?.gpuMemoryTotalBytes}
                status={s.status === 'online' ? 'running' : 'stopped'}
              />
            </div>
          </div>
        ))
      )}

      <Pager page={page} pageSize={PAGE_SIZE} total={total} onChange={setPage} />

      <Modal
        title={modal?.id ? t('edit') : t('create')}
        open={!!modal}
        onCancel={() => setModal(null)}
        okButtonProps={{ type: 'primary' }}
        okText={t('save')}
        cancelText={t('cancel')}
        onOk={submit}
        destroyOnClose
      >
        <Form form={form} layout="vertical">
          <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="ip" label="IP / hostname" rules={[{ required: true }]}>
            <Input />
          </Form.Item>
        </Form>
      </Modal>
    </section>
  );
}
