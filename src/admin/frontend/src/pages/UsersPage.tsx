import { Button, Form, Input, Modal, Select, Typography, message } from 'antd';
import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import api from '../api/client';
import { DataTable } from '../components/DataTable';

type User = { id: number; email: string; name: string; role: string };

export default function UsersPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const [open, setOpen] = useState(false);
  const [refreshKey, setRefreshKey] = useState(0);
  const [form] = Form.useForm();

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 16 }}>
        <Typography.Title level={3} style={{ margin: 0, color: '#0A2540' }}>
          {t('users')}
        </Typography.Title>
        <Button type="primary" onClick={() => setOpen(true)}>
          {t('create')}
        </Button>
      </div>
      <div style={{ background: 'var(--hc-surface)', border: '1px solid var(--hc-border-soft)', borderRadius: 0, padding: 24 }}>
        <DataTable<User>
          endpoint="/users"
          refreshKey={refreshKey}
          rowLink={(row) => `/users/${row.id}`}
          columns={[
            { title: t('email'), dataIndex: 'email' },
            { title: t('name'), dataIndex: 'name' },
            {
              title: t('role'),
              dataIndex: 'role',
              render: (value: string) => t(`roles.${value}`, { defaultValue: value }),
            },
          ]}
          actions={[
            { key: 'open', label: t('open'), onClick: (row) => navigate(`/users/${row.id}`) },
            {
              key: 'delete',
              label: t('delete'),
              danger: true,
              onClick: async (row) => {
                await api.delete(`/users/${row.id}`);
                message.success(t('deleted'));
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
                await Promise.all(rows.map((row) => api.delete(`/users/${row.id}`)));
                message.success(t('deleted'));
                setRefreshKey((n) => n + 1);
              },
            },
          ]}
        />
      </div>
      <Modal
        open={open}
        title={t('create')}
        onCancel={() => setOpen(false)}
        okButtonProps={{ type: 'primary' }}
        onOk={async () => {
          const values = await form.validateFields();
          await api.post('/users', values);
          setOpen(false);
          form.resetFields();
          setRefreshKey((n) => n + 1);
        }}
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
    </div>
  );
}
