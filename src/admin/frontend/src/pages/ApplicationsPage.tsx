import { Button, Form, Input, InputNumber, Modal, Select, Space, Tabs, Tag, Typography, message } from 'antd';
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import api from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { AppImageEntry, AppImagePicker } from '../components/AppImagePicker';
import { AppIcon, IconEditor } from '../components/IconEditor';
import { APP_CATEGORIES, AppCategory } from '../components/appCategories';
import { DataTable } from '../components/DataTable';
import { DockerfileExposeHint } from '../components/DockerfileExposeHint';
import { BaseDomainOption, HostnameFields, HostnameRow } from '../components/HostnameFields';
import { HostnamePills } from '../components/HostnamePills';
import { MetricDisplay } from '../components/MetricDisplay';
import { ServerPills } from '../components/ServerPills';

type Application = {
  id: number;
  name: string;
  type: AppCategory;
  icon?: string;
  hostname: string;
  hostnames?: { hostname: string; tlsStatus?: string }[];
  desiredReplicas: number;
  tlsStatus: string;
  ownerEmail?: string;
  runtimeStatus?: 'running' | 'deploying' | 'stopped';
  runningReplicas?: number;
  startedAt?: string;
  servers?: { id: number; name: string; ip?: string }[];
  cpuPercent?: number;
  memoryUsedBytes?: number;
  memoryLimitBytes?: number;
  diskUsedBytes?: number;
};

type GithubApp = { id: number; name: string; status: string };
type Repo = { fullName: string; defaultBranch: string };
type Branch = { name: string };

export default function ApplicationsPage() {
  const { t } = useTranslation();
  const { isAdmin } = useAuth();
  const navigate = useNavigate();
  const [open, setOpen] = useState(false);
  const [refreshKey, setRefreshKey] = useState(0);
  const [form] = Form.useForm();
  const source = Form.useWatch('source', form);
  const githubAppId = Form.useWatch('githubAppId', form);
  const repoFullName = Form.useWatch('repoFullName', form);
  const repoUrl = Form.useWatch('repoUrl', form);
  const branch = Form.useWatch('branch', form);
  const baseDirectory = Form.useWatch('baseDirectory', form);
  const dockerfilePath = Form.useWatch('dockerfilePath', form);
  const [githubApps, setGithubApps] = useState<{ value: number; label: string }[]>([]);
  const [repos, setRepos] = useState<{ value: string; label: string; defaultBranch: string }[]>([]);
  const [branches, setBranches] = useState<{ value: string; label: string }[]>([]);
  const [domains, setDomains] = useState<BaseDomainOption[]>([]);
  const [hostnameRows, setHostnameRows] = useState<HostnameRow[]>([]);
  const [createTab, setCreateTab] = useState<'dockerfile' | 'image'>('dockerfile');
  const [selectedImageId, setSelectedImageId] = useState<string>();
  const [iconEdit, setIconEdit] = useState<Application | null>(null);
  const [category, setCategory] = useState<AppCategory>('other');

  // Poll the list so the resource bars and request animation stay near real-time.
  useEffect(() => {
    const timer = window.setInterval(() => setRefreshKey((n) => n + 1), 4000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    if (!open) return;
    api.get('/github-apps', { params: { page: 1, pageSize: 100 } }).then(({ data }) => {
      setGithubApps(
        data.rows
          .filter((app: GithubApp) => app.status === 'installed')
          .map((app: GithubApp) => ({ value: app.id, label: app.name })),
      );
    });
    Promise.all([
      api.get('/profile/base-domains'),
      api.get('/profile/hostname-suggestion'),
    ]).then(([domainsRes, suggestionRes]) => {
      const rows: BaseDomainOption[] = domainsRes.data.rows || [];
      setDomains(rows);
      setHostnameRows(
        rows.length
          ? [
              {
                subdomain: suggestionRes.data.subdomain || '',
                baseDomainId: rows[0].id,
              },
            ]
          : [],
      );
    });
  }, [open]);

  useEffect(() => {
    if (!githubAppId || source !== 'github_app') {
      setRepos([]);
      return;
    }
    api
      .get(`/github-apps/${githubAppId}/repositories`, { params: { page: 1, pageSize: 100 } })
      .then(({ data }) => {
        setRepos(
          data.rows.map((repo: Repo) => ({
            value: repo.fullName,
            label: repo.fullName,
            defaultBranch: repo.defaultBranch || 'main',
          })),
        );
      });
  }, [githubAppId, source]);

  const loadBranches = async (repo: string) => {
    if (!githubAppId || !repo) return;
    const { data } = await api.get(`/github-apps/${githubAppId}/branches`, { params: { repo } });
    setBranches(data.rows.map((b: Branch) => ({ value: b.name, label: b.name })));
  };

  const resetCreate = () => {
    setOpen(false);
    form.resetFields();
    setHostnameRows([]);
    setDomains([]);
    setCreateTab('dockerfile');
    setSelectedImageId(undefined);
  };

  const selectImage = (entry: AppImageEntry) => {
    const bytes = new Uint8Array(18);
    crypto.getRandomValues(bytes);
    const password = Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
    const fill = (value: string) => value.split('{{secret}}').join(password);
    form.setFieldsValue({
      name: form.getFieldValue('name') || entry.name,
      image: entry.image,
      portsExposes: entry.port,
      desiredReplicas: 1,
      volumes: entry.volumes,
      envText: Object.entries(entry.env)
        .map(([key, value]) => `${key}=${fill(value)}`)
        .join('\n'),
      healthCheckType: entry.healthCheckType,
      healthCheckCommand: fill(entry.healthCheckCommand),
    });
    setSelectedImageId(entry.id);
  };

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 16 }}>
        <div>
          <Typography.Title level={3} style={{ margin: 0, color: '#0A2540' }}>
            {t('applications')}
          </Typography.Title>
          <Typography.Paragraph style={{ color: '#425466' }}>
            {t('applicationsSubtitle')}
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
        }}
      >
        <Tabs
          activeKey={category}
          onChange={(key) => setCategory(key as AppCategory)}
          items={APP_CATEGORIES.map((def) => ({
            key: def.value,
            label: (
              <Space size={6}>
                {def.icon}
                {t(def.labelKey)}
              </Space>
            ),
          }))}
        />
        <DataTable<Application>
          endpoint="/applications"
          refreshKey={refreshKey}
          params={{ type: category }}
          rowLink={(row) => `/applications/${row.id}`}
          columns={[
            {
              title: '',
              dataIndex: 'icon',
              width: 64,
              onCell: () => ({ onClick: (e: { stopPropagation: () => void }) => e.stopPropagation() }),
              render: (_: unknown, row: Application) => (
                <button
                  type="button"
                  title={t('iconEditTitle')}
                  onClick={() => setIconEdit(row)}
                  style={{ border: 'none', background: 'transparent', padding: 0, cursor: 'pointer' }}
                >
                  <AppIcon icon={row.icon} name={row.name} size={32} />
                </button>
              ),
            },
            { title: t('name'), dataIndex: 'name' },
            {
              title: t('server'),
              dataIndex: 'servers',
              width: 150,
              onCell: () => ({ onClick: (e: { stopPropagation: () => void }) => e.stopPropagation() }),
              render: (_: unknown, row: Application) => <ServerPills servers={row.servers} />,
            },
            {
              title: t('status'),
              dataIndex: 'runtimeStatus',
              width: 120,
              render: (_: unknown, row: Application) => {
                const status = row.runtimeStatus ?? 'stopped';
                const color = status === 'running' ? 'green' : status === 'deploying' ? 'blue' : 'default';
                return <Tag color={color}>{t(`runtimeStatus_${status}`)}</Tag>;
              },
            },
            {
              title: t('hostname'),
              dataIndex: 'hostname',
              render: (_: unknown, row: Application) => {
                const list =
                  row.hostnames && row.hostnames.length > 0
                    ? row.hostnames
                    : row.hostname
                      ? [{ hostname: row.hostname, tlsStatus: row.tlsStatus }]
                      : [];
                return <HostnamePills hostnames={list} collapse />;
              },
            },
            {
              title: t('resources'),
              dataIndex: 'cpuPercent',
              width: 180,
              onCell: () => ({ onClick: (e: { stopPropagation: () => void }) => e.stopPropagation() }),
              render: (_: unknown, row: Application) => (
                <MetricDisplay
                  cpuPercent={row.cpuPercent}
                  memoryUsedBytes={row.memoryUsedBytes}
                  memoryLimitBytes={row.memoryLimitBytes}
                  diskUsedBytes={row.diskUsedBytes}
                  startedAt={row.startedAt}
                  status={row.runtimeStatus}
                  replicas={row.runningReplicas}
                />
              ),
            },
          ]}
          actions={[
            { key: 'open', label: t('open'), onClick: (row) => navigate(`/applications/${row.id}`) },
            {
              key: 'deploy',
              label: t('deploy'),
              onClick: async (row) => {
                await api.post(`/applications/${row.id}/deployments`);
                message.success(t('deployStarted'));
              },
            },
            {
              key: 'delete',
              label: t('delete'),
              danger: true,
              onClick: async (row) => {
                await api.delete(`/applications/${row.id}`);
                setRefreshKey((n) => n + 1);
              },
            },
          ]}
          bulkActions={[
            {
              key: 'deploy',
              label: t('deploy'),
              onClick: async (rows) => {
                await Promise.all(rows.map((row) => api.post(`/applications/${row.id}/deployments`)));
                message.success(t('deployStarted'));
                setRefreshKey((n) => n + 1);
              },
            },
            {
              key: 'delete',
              label: t('delete'),
              danger: true,
              onClick: async (rows) => {
                await Promise.all(rows.map((row) => api.delete(`/applications/${row.id}`)));
                message.success(t('deleted'));
                setRefreshKey((n) => n + 1);
              },
            },
          ]}
        />
      </div>
      <Modal
        title={t('create')}
        open={open}
        width={createTab === 'image' ? 880 : 640}
        onCancel={resetCreate}
        okButtonProps={{ type: 'primary' }}
        onOk={async () => {
          const values = await form.validateFields();
          if (createTab === 'image') {
            if (!values.image) {
              message.error(t('appImageEmpty'));
              return;
            }
            values.source = 'image';
            values.desiredReplicas = 1;
            delete values.repoUrl;
            delete values.githubAppId;
            delete values.repoFullName;
          } else if (!isAdmin) {
            values.portsExposes = 80;
          }
          const filledHostnames = hostnameRows.filter((row) => row.subdomain || row.baseDomainId);
          if (filledHostnames.some((row) => !row.subdomain || !row.baseDomainId)) {
            message.error(t('hostnameRequired'));
            return;
          }
          if (filledHostnames.length && !domains.length) {
            message.error(t('noAssignedDomains'));
            return;
          }
          if (filledHostnames.length) {
            values.hostnames = filledHostnames.map((row) => ({
              subdomain: row.subdomain,
              baseDomainId: row.baseDomainId,
            }));
          }
          delete values.hostname;
          if (values.envText) {
            values.env = Object.fromEntries(
              String(values.envText)
                .split('\n')
                .filter(Boolean)
                .map((line: string) => {
                  const i = line.indexOf('=');
                  return [line.slice(0, i), line.slice(i + 1)];
                }),
            );
          }
          delete values.envText;
          if (createTab !== 'image') {
            delete values.image;
            delete values.healthCheckType;
            delete values.healthCheckCommand;
          }
          if (Array.isArray(values.volumes)) {
            values.volumes = values.volumes.filter(
              (row: { name?: string; mountPath?: string }) => row?.name && row?.mountPath,
            );
            if (!values.volumes.length) {
              delete values.volumes;
            }
          }
          const { data } = await api.post('/applications', values);
          if (values.source === 'image' && data?.id) {
            await api.post(`/applications/${data.id}/deployments`);
            message.success(t('deployStarted'));
          }
          resetCreate();
          setRefreshKey((n) => n + 1);
        }}
      >
        <Form
          form={form}
          layout="vertical"
          initialValues={{
            source: 'public',
            type: 'other',
            desiredReplicas: 1,
            portsExposes: 80,
            branch: 'main',
            baseDirectory: '/',
            dockerfilePath: 'Dockerfile',
            healthCheckType: 'none',
            healthCheckCommand: '',
            image: '',
          }}
        >
          <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="type" label={t('category')}>
            <Select
              options={APP_CATEGORIES.map((def) => ({
                value: def.value,
                label: (
                  <Space size={6}>
                    {def.icon}
                    {t(def.labelKey)}
                  </Space>
                ),
              }))}
            />
          </Form.Item>
          <Form.Item label={t('hostnames')}>
            <HostnameFields domains={domains} value={hostnameRows} onChange={setHostnameRows} />
          </Form.Item>
          <Form.Item name="image" hidden>
            <Input />
          </Form.Item>
          <Form.Item name="healthCheckType" hidden>
            <Input />
          </Form.Item>
          <Form.Item name="healthCheckCommand" hidden>
            <Input />
          </Form.Item>
          <Tabs
            activeKey={createTab}
            destroyInactiveTabPane
            onChange={(key) => setCreateTab(key as 'dockerfile' | 'image')}
            items={[
              {
                key: 'dockerfile',
                label: t('createTabDockerfile'),
                children: (
                  <>
          <Form.Item name="source" label={t('source')} rules={createTab === 'dockerfile' ? [{ required: true }] : []}>
            <Select
              options={[
                { value: 'public', label: t('sourcePublic') },
                { value: 'github_app', label: t('sourceGithubApp') },
              ]}
            />
          </Form.Item>
          {source === 'public' ? (
            <Form.Item name="repoUrl" label={t('repositoryUrl')} rules={[{ required: true }]}>
              <Input />
            </Form.Item>
          ) : (
            <>
              <Form.Item name="githubAppId" label={t('githubIntegration')} rules={[{ required: true }]}>
                <Select options={githubApps} placeholder={t('selectGithubApp')} />
              </Form.Item>
              <Form.Item name="repoFullName" label={t('repository')} rules={[{ required: true }]}>
                <Select
                  showSearch
                  options={repos}
                  placeholder={t('selectRepository')}
                  onChange={(value) => {
                    const repo = repos.find((r) => r.value === value);
                    form.setFieldValue('branch', repo?.defaultBranch || 'main');
                    void loadBranches(String(value));
                  }}
                />
              </Form.Item>
            </>
          )}
          <Form.Item name="branch" label={t('branch')} rules={[{ required: true }]}>
            {source === 'github_app' ? <Select options={branches} showSearch /> : <Input />}
          </Form.Item>
          <Form.Item name="baseDirectory" label={t('baseDirectory')}>
            <Input />
          </Form.Item>
          <Form.Item name="dockerfilePath" label={t('dockerfilePath')}>
            <Input />
          </Form.Item>
          <Form.Item
            name="portsExposes"
            label={t('portExpose')}
            extra={!isAdmin ? t('portExposeAdminOnly') : undefined}
            rules={isAdmin ? [{ required: true }] : []}
          >
            <InputNumber style={{ width: '100%' }} min={1} max={65535} disabled={!isAdmin} />
          </Form.Item>
          <div style={{ marginBottom: 16 }}>
            <DockerfileExposeHint
              source={source}
              githubAppId={githubAppId}
              repo={repoFullName}
              repoUrl={repoUrl}
              branch={branch}
              baseDirectory={baseDirectory}
              dockerfilePath={dockerfilePath}
            />
          </div>
          {isAdmin && (
            <Form.Item name="desiredReplicas" label={t('replicas')}>
              <InputNumber style={{ width: '100%' }} min={1} />
            </Form.Item>
          )}
                  </>
                ),
              },
              {
                key: 'image',
                label: t('createTabAppImage'),
                children: (
                  <>
                    <Typography.Paragraph style={{ color: '#425466' }}>{t('appImageHint')}</Typography.Paragraph>
                    <AppImagePicker selectedId={selectedImageId} onSelect={selectImage} />
                  </>
                ),
              },
            ]}
          />
          <Form.Item label={t('volumes')} extra={t('volumesCreateHint')}>
            <Form.List name="volumes">
              {(fields, { add, remove }) => (
                <>
                  {fields.map((field) => (
                    <Space key={field.key} align="start" style={{ display: 'flex', marginBottom: 8 }}>
                      <Form.Item
                        {...field}
                        name={[field.name, 'name']}
                        rules={[{ required: true, message: t('volumeName') }]}
                        style={{ marginBottom: 0, width: 140 }}
                      >
                        <Input placeholder={t('volumeName')} />
                      </Form.Item>
                      <Form.Item
                        {...field}
                        name={[field.name, 'mountPath']}
                        rules={[{ required: true, message: t('volumeMountPath') }]}
                        style={{ marginBottom: 0, width: 220 }}
                      >
                        <Input placeholder="/var/lib/data" />
                      </Form.Item>
                      <Button type="link" danger onClick={() => remove(field.name)}>
                        {t('remove')}
                      </Button>
                    </Space>
                  ))}
                  <Button type="dashed" onClick={() => add({ name: 'data', mountPath: '' })} block>
                    {t('addVolume')}
                  </Button>
                </>
              )}
            </Form.List>
          </Form.Item>
          <Form.Item name="envText" label={t('environmentVars')}>
            <Input.TextArea rows={4} />
          </Form.Item>
        </Form>
      </Modal>
      <IconEditor
        open={!!iconEdit}
        value={iconEdit?.icon}
        name={iconEdit?.name}
        onCancel={() => setIconEdit(null)}
        onSave={async (icon) => {
          if (iconEdit) {
            await api.patch(`/applications/${iconEdit.id}`, { icon });
            message.success(t('iconSaved'));
          }
          setIconEdit(null);
          setRefreshKey((k) => k + 1);
        }}
      />
    </div>
  );
}
