import { Badge, Button, Form, Input, Select, Table, Tabs, Tag, Typography, message } from 'antd';
import dayjs from 'dayjs';
import relativeTime from 'dayjs/plugin/relativeTime';
import 'dayjs/locale/es';
import { useCallback, useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate, useParams } from 'react-router-dom';
import api from '../api/client';
import { ActivityTimeline, type ActivityBucket } from '../components/ActivityTimeline';

dayjs.extend(relativeTime);

const CHART_HOURS = 12;

type User = {
  id: number;
  email: string;
  name: string;
  role: string;
  serverIds: number[];
  domainIds: number[];
};

type Option = { value: number; label: string };

type ActivityRow = {
  id: number;
  action: string;
  method: string;
  path: string;
  targetType: string;
  targetId: string;
  status: number;
  ip: string;
  createdAt: string;
};

const cardStyle: React.CSSProperties = {
  background: '#fff',
  border: '1px solid #E3E8EE',
  borderRadius: 8,
  padding: 24,
};

function statusColor(status: number) {
  if (status >= 500) return 'error';
  if (status >= 400) return 'warning';
  if (status >= 200 && status < 300) return 'success';
  return 'default';
}

function AuditTrail({ userId }: { userId: string }) {
  const { t, i18n } = useTranslation();
  const [page, setPage] = useState(1);
  const [rows, setRows] = useState<ActivityRow[]>([]);
  const [total, setTotal] = useState(0);
  const [buckets, setBuckets] = useState<ActivityBucket[]>([]);
  const [online, setOnline] = useState(false);

  const loadList = useCallback(async () => {
    try {
      const { data } = await api.get(`/users/${userId}/activity`, {
        params: { page, pageSize: 10 },
        suppressErrorToast: true,
      });
      setRows(data.rows || []);
      setTotal(data.total || 0);
    } catch {
      /* ignore transient polling errors */
    }
  }, [userId, page]);

  const loadChart = useCallback(async () => {
    try {
      const { data } = await api.get(`/users/${userId}/activity-chart`, {
        params: { hours: CHART_HOURS },
        suppressErrorToast: true,
      });
      setBuckets(data.buckets || []);
      setOnline(Boolean(data.online));
    } catch {
      /* ignore transient polling errors */
    }
  }, [userId]);

  useEffect(() => {
    loadList();
  }, [loadList]);

  useEffect(() => {
    loadChart();
    const timer = window.setInterval(loadChart, 15000);
    return () => window.clearInterval(timer);
  }, [loadChart]);

  return (
    <div style={{ display: 'grid', gap: 16 }}>
      <div style={cardStyle}>
        <div
          style={{
            display: 'flex',
            justifyContent: 'space-between',
            alignItems: 'center',
            gap: 12,
            marginBottom: 12,
            flexWrap: 'wrap',
          }}
        >
          <Typography.Title level={5} style={{ margin: 0 }}>
            {t('activityChartTitle', { hours: CHART_HOURS })}
          </Typography.Title>
          <Badge
            status={online ? 'success' : 'default'}
            text={online ? t('presenceOnline') : t('presenceOffline')}
          />
        </div>
        <ActivityTimeline buckets={buckets} hours={CHART_HOURS} />
        <div style={{ display: 'flex', gap: 16, marginTop: 8, flexWrap: 'wrap' }}>
          <Typography.Text type="secondary" style={{ fontSize: 12 }}>
            <span
              style={{
                display: 'inline-block',
                width: 10,
                height: 10,
                borderRadius: 2,
                background: '#635bff',
                marginRight: 6,
              }}
            />
            {t('activityLegendActions')}
          </Typography.Text>
          <Typography.Text type="secondary" style={{ fontSize: 12 }}>
            <span
              style={{
                display: 'inline-block',
                width: 10,
                height: 10,
                borderRadius: 2,
                background: '#09825d',
                marginRight: 6,
              }}
            />
            {t('activityLegendActive')}
          </Typography.Text>
          <Typography.Text type="secondary" style={{ fontSize: 12 }}>
            <span
              style={{
                display: 'inline-block',
                width: 10,
                height: 10,
                borderRadius: 2,
                background: '#e5484d',
                marginRight: 6,
              }}
            />
            {t('activityLegendInactive')}
          </Typography.Text>
        </div>
      </div>

      <div style={cardStyle}>
        <Typography.Title level={5} style={{ marginTop: 0 }}>
          {t('auditTrail')}
        </Typography.Title>
        <Table<ActivityRow>
          rowKey="id"
          size="small"
          dataSource={rows}
          locale={{ emptyText: t('auditEmpty') }}
          pagination={{
            current: page,
            pageSize: 10,
            total,
            onChange: setPage,
            hideOnSinglePage: true,
          }}
          columns={[
            {
              title: t('incidentTime'),
              dataIndex: 'createdAt',
              width: 170,
              render: (value: string) =>
                value ? (
                  <span title={dayjs(value).locale(i18n.language).format('YYYY-MM-DD HH:mm:ss')}>
                    {dayjs(value).locale(i18n.language).fromNow()}
                  </span>
                ) : (
                  '—'
                ),
            },
            {
              title: t('auditAction'),
              dataIndex: 'action',
              render: (action: string) =>
                t(`activityAction_${action.replace(/\./g, '_')}`, { defaultValue: action }),
            },
            {
              title: t('auditTarget'),
              key: 'target',
              render: (_: unknown, row: ActivityRow) =>
                row.targetType
                  ? `${t(`auditTargetType_${row.targetType}`, { defaultValue: row.targetType })}${
                      row.targetId ? ` #${row.targetId}` : ''
                    }`
                  : '—',
            },
            {
              title: t('status'),
              dataIndex: 'status',
              width: 90,
              render: (status: number) => <Tag color={statusColor(status)}>{status || '—'}</Tag>,
            },
            {
              title: t('auditIp'),
              dataIndex: 'ip',
              width: 140,
              render: (ip: string) => ip || '—',
            },
          ]}
        />
      </div>
    </div>
  );
}

export default function UserDetailPage({
  id: propId,
  onSaved,
}: { id?: number; onSaved?: () => void } = {}) {
  const params = useParams();
  const id = propId != null ? String(propId) : params.id;
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

  const settingsTab = (
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
            if (onSaved) onSaved();
            else navigate('/users');
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
        <Form.Item name="serverIds" label={t('assignedServers')} extra={t('assignedServersHint')}>
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
        <Form.Item name="domainIds" label={t('assignedDomains')} extra={t('assignedDomainsHint')}>
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
  );

  return (
    <div>
      <Typography.Title level={3} style={{ color: '#0A2540', marginBottom: 8 }}>
        {user.name || user.email}
      </Typography.Title>
      <Typography.Paragraph type="secondary" style={{ marginBottom: 24 }}>
        {user.email}
      </Typography.Paragraph>
      <Tabs
        items={[
          { key: 'settings', label: t('general'), children: settingsTab },
          { key: 'audit', label: t('auditTrail'), children: id ? <AuditTrail userId={id} /> : null },
        ]}
      />
    </div>
  );
}
