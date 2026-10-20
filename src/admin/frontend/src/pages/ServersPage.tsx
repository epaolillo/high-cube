import { Button, Form, Input, Modal, Typography, message } from 'antd';
import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import api from '../api/client';
import { DataTable } from '../components/DataTable';
import { confirmRegenerateToken } from '../components/confirmRegenerateToken';

type Server = { id: number; name: string; ip: string; status: string; lastSeenAt: string };

function showAgentToken(title: string, hint: string, token: string) {
  Modal.info({
    title,
    width: 560,
    content: (
      <div>
        <Typography.Paragraph type="secondary">{hint}</Typography.Paragraph>
        <Typography.Paragraph copyable code style={{ wordBreak: 'break-all' }}>
          {token}
        </Typography.Paragraph>
      </div>
    ),
  });
}

export default function ServersPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const [open, setOpen] = useState(false);
  const [editing, setEditing] = useState<Server | null>(null);
  const [refreshKey, setRefreshKey] = useState(0);
  const [form] = Form.useForm();
  const [editForm] = Form.useForm();

  const openEdit = (row: Server) => {
    setEditing(row);
    editForm.setFieldsValue({ name: row.name, ip: row.ip });
  };

  const regenerateToken = (row: Server) => {
    confirmRegenerateToken(t, row.name, async () => {
      const { data } = await api.post(`/servers/${row.id}/token`);
      message.success(t('agentTokenRotated'));
      showAgentToken(t('agentTokenTitle'), t('agentTokenHint'), data.agentToken);
    });
  };

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 16 }}>
        <div>
          <Typography.Title level={3} style={{ margin: 0, color: '#0A2540' }}>
            {t('servers')}
          </Typography.Title>
          <Typography.Paragraph style={{ color: '#425466' }}>
            Connected agents and host health
          </Typography.Paragraph>
        </div>
        <Button type="primary" onClick={() => setOpen(true)}>
          {t('create')}
        </Button>
      </div>
      <div
        style={{
          background: '#fff',
          border: '1px solid #E3E8EE',
          borderRadius: 8,
          padding: 24,
          boxShadow:
            '0 0 0 1px rgba(50, 50, 93, 0.06), 0 2px 5px rgba(50, 50, 93, 0.08), 0 1px 1.5px rgba(0, 0, 0, 0.04)',
        }}
      >
        <DataTable<Server>
          endpoint="/servers"
          refreshKey={refreshKey}
          rowLink={(row) => `/servers/${row.id}`}
          filters={[{ field: 'q', label: t('name'), type: 'text' }]}
          columns={[
            { title: t('name'), dataIndex: 'name' },
            { title: 'IP', dataIndex: 'ip' },
            { title: t('status'), dataIndex: 'status' },
            { title: t('lastSeen'), dataIndex: 'lastSeenAt' },
          ]}
          actions={[
            { key: 'view', label: t('open'), onClick: (row) => navigate(`/servers/${row.id}`) },
            { key: 'edit', label: t('edit'), onClick: (row) => openEdit(row) },
            {
              key: 'token',
              label: t('regenerateToken'),
              onClick: (row) => regenerateToken(row),
            },
            {
              key: 'delete',
              label: t('delete'),
              danger: true,
              onClick: async (row) => {
                await api.delete(`/servers/${row.id}`);
                message.success(t('serverRemoved'));
                setRefreshKey((n) => n + 1);
              },
            },
          ]}
          bulkActions={[
            {
              key: 'delete',
              label: t('delete'),
              danger: true,
              onClick: async (rows) => {
                await Promise.all(rows.map((row) => api.delete(`/servers/${row.id}`)));
                message.success(t('serverRemoved'));
                setRefreshKey((n) => n + 1);
              },
            },
          ]}
        />
      </div>
      <Modal
        title={t('create')}
        open={open}
        onCancel={() => setOpen(false)}
        okButtonProps={{ type: 'primary' }}
        onOk={async () => {
          const values = await form.validateFields();
          const { data } = await api.post('/servers', values);
          setOpen(false);
          form.resetFields();
          setRefreshKey((n) => n + 1);
          showAgentToken(t('agentTokenTitle'), t('agentTokenHint'), data.agentToken);
        }}
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
      <Modal
        title={t('edit')}
        open={!!editing}
        onCancel={() => setEditing(null)}
        okText={t('save')}
        cancelText={t('cancel')}
        okButtonProps={{ type: 'primary' }}
        onOk={async () => {
          if (!editing) return;
          const values = await editForm.validateFields();
          await api.patch(`/servers/${editing.id}`, values);
          message.success(t('saved'));
          setEditing(null);
          setRefreshKey((n) => n + 1);
        }}
      >
        <Form form={editForm} layout="vertical">
          <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="ip" label="IP / hostname" rules={[{ required: true }]}>
            <Input />
          </Form.Item>
        </Form>
      </Modal>
    </div>
  );
}
