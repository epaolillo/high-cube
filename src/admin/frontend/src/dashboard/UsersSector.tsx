import {
  ArrowDownOutlined,
  ArrowUpOutlined,
  DeleteOutlined,
  EditOutlined,
  PlusOutlined,
} from '@ant-design/icons';
import { Form, Input, Modal, Select, Tag, Tooltip, message } from 'antd';
// Note: sort field is intentionally not sent; the list endpoint only supports `order`.
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { confirmDestructive } from '../components/confirmDestructive';
import { Pager } from './Pager';
import { PAGE_SIZE, usePagedResource } from './usePagedResource';

type UserRow = { id: number; email: string; name: string; role: string };

export function UsersSector({ onOpen }: { onOpen: (id: number) => void }) {
  const { t } = useTranslation();
  const [page, setPage] = useState(1);
  const [order, setOrder] = useState<'asc' | 'desc'>('asc');
  const [refreshKey, setRefreshKey] = useState(0);
  const bump = () => setRefreshKey((n) => n + 1);
  const [sel, setSel] = useState<Set<number>>(new Set());
  const [createOpen, setCreateOpen] = useState(false);
  const [form] = Form.useForm();

  const { rows, total } = usePagedResource<UserRow>('/users', {
    page,
    params: { order },
    refreshKey,
  });

  // Live online presence: poll the set of users currently viewing the dashboard.
  const [onlineIds, setOnlineIds] = useState<Set<number>>(new Set());
  useEffect(() => {
    let stopped = false;
    const load = async () => {
      try {
        const { data } = await api.get('/presence/online', { suppressErrorToast: true });
        if (!stopped) setOnlineIds(new Set<number>(data.userIds || []));
      } catch {
        /* ignore transient polling errors */
      }
    };
    load();
    const timer = window.setInterval(load, 5000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, []);

  useEffect(() => setSel(new Set()), [page, order]);

  const toggle = (id: number) =>
    setSel((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  const submit = async () => {
    const values = await form.validateFields();
    await api.post('/users', values);
    message.success(t('saved'));
    setCreateOpen(false);
    bump();
  };
  const removeOne = (u: UserRow) =>
    confirmDestructive(t, { text: u.email }).then((ok) => {
      if (!ok) return;
      return api.delete(`/users/${u.id}`).then(() => {
        message.success(t('deleted'));
        bump();
      });
    });
  const bulkDelete = async () => {
    if (!(await confirmDestructive(t, { text: t('confirmDeleteManyBody', { count: sel.size }) }))) return;
    await Promise.all([...sel].map((id) => api.delete(`/users/${id}`)));
    message.success(t('deleted'));
    setSel(new Set());
    bump();
  };

  return (
    <section className="hc-sector hc-sector--users">
      <div className="hc-sector__head">
        <span className="hc-sector__title">{t('users')}</span>
        <span className="hc-sector__count">{total}</span>
        <span className="hc-sector__spacer" />
        <button
          type="button"
          className="hc-addbtn"
          onClick={() => {
            form.resetFields();
            setCreateOpen(true);
          }}
        >
          <PlusOutlined />
          {t('create')}
        </button>
      </div>

      <div className="hc-controls">
        <Tooltip title={order === 'asc' ? 'ASC' : 'DESC'}>
          <button
            type="button"
            className="hc-iconbtn"
            onClick={() => setOrder((o) => (o === 'asc' ? 'desc' : 'asc'))}
          >
            {order === 'asc' ? <ArrowUpOutlined /> : <ArrowDownOutlined />}
          </button>
        </Tooltip>
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
        rows.map((u) => (
          <div className="hc-row" key={u.id}>
            <div className="hc-row__head">
              <input
                type="checkbox"
                className="hc-row__check"
                checked={sel.has(u.id)}
                onChange={() => toggle(u.id)}
              />
              <span className="hc-avatar-sq">{(u.name || u.email || 'U').slice(0, 1).toUpperCase()}</span>
              <span className="hc-row__name" title={u.name || u.email}>
                {u.name || u.email}
              </span>
              <Tooltip title={onlineIds.has(u.id) ? t('presenceOnline') : t('presenceOffline')}>
                <span
                  className="hc-presence-dot"
                  data-online={onlineIds.has(u.id) ? 'true' : 'false'}
                  aria-label={onlineIds.has(u.id) ? t('presenceOnline') : t('presenceOffline')}
                />
              </Tooltip>
              <Tag style={{ margin: 0 }}>{t(`roles.${u.role}`, { defaultValue: u.role })}</Tag>
              <div className="hc-acts">
                <Tooltip title={t('edit')}>
                  <button type="button" className="hc-iconbtn" onClick={() => onOpen(u.id)}>
                    <EditOutlined />
                  </button>
                </Tooltip>
                <Tooltip title={t('delete')}>
                  <button
                    type="button"
                    className="hc-iconbtn"
                    data-tone="danger"
                    onClick={() => void removeOne(u)}
                  >
                    <DeleteOutlined />
                  </button>
                </Tooltip>
              </div>
            </div>
          </div>
        ))
      )}

      <Pager page={page} pageSize={PAGE_SIZE} total={total} onChange={setPage} />

      <Modal
        title={t('create')}
        open={createOpen}
        onCancel={() => setCreateOpen(false)}
        okButtonProps={{ type: 'primary' }}
        okText={t('save')}
        cancelText={t('cancel')}
        onOk={submit}
        destroyOnClose
      >
        <Form form={form} layout="vertical" initialValues={{ role: 'user' }}>
          <Form.Item name="email" label={t('email')} rules={[{ required: true, type: 'email' }]}>
            <Input />
          </Form.Item>
          <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="password" label={t('password')} rules={[{ required: true }]}>
            <Input.Password />
          </Form.Item>
          <Form.Item name="role" label={t('role')} rules={[{ required: true }]}>
            <Select
              options={[
                { value: 'admin', label: t('roles.admin') },
                { value: 'user', label: t('roles.user') },
              ]}
            />
          </Form.Item>
        </Form>
      </Modal>
    </section>
  );
}
