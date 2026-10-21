import { Button, Form, Input, InputNumber, Modal, Select, Space, Tabs, Typography, message } from 'antd';
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { DATABASE_FLAVORS, flavorForImage } from '../data/databaseFlavors';
import { AppImageEntry, AppImagePicker } from './AppImagePicker';
import { APP_CATEGORIES } from './appCategories';
import { DockerfileExposeHint } from './DockerfileExposeHint';
import { BaseDomainOption, HostnameFields, HostnameRow } from './HostnameFields';

type GithubApp = { id: number; name: string; status: string };
type Repo = { fullName: string; defaultBranch: string };
type Branch = { name: string };

type Props = {
  open: boolean;
  isAdmin: boolean;
  onClose: () => void;
  onCreated: () => void;
};

/**
 * Self-contained "create application" modal, lifted out of the old list page so
 * the single-screen dashboard can trigger it without a routed page.
 */
export function ApplicationFormModal({ open, isAdmin, onClose, onCreated }: Props) {
  const { t } = useTranslation();
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

  useEffect(() => {
    if (!open) return;
    api.get('/github-apps', { params: { page: 1, pageSize: 100 } }).then(({ data }) => {
      setGithubApps(
        data.rows
          .filter((app: GithubApp) => app.status === 'installed')
          .map((app: GithubApp) => ({ value: app.id, label: app.name })),
      );
    });
    Promise.all([api.get('/profile/base-domains'), api.get('/profile/hostname-suggestion')]).then(
      ([domainsRes, suggestionRes]) => {
        const rows: BaseDomainOption[] = domainsRes.data.rows || [];
        setDomains(rows);
        setHostnameRows(
          rows.length
            ? [{ subdomain: suggestionRes.data.subdomain || '', baseDomainId: rows[0].id }]
            : [],
        );
      },
    );
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

  const reset = () => {
    form.resetFields();
    setHostnameRows([]);
    setDomains([]);
    setCreateTab('dockerfile');
    setSelectedImageId(undefined);
  };

  const close = () => {
    reset();
    onClose();
  };

  const selectImage = (entry: AppImageEntry) => {
    const bytes = new Uint8Array(18);
    crypto.getRandomValues(bytes);
    const password = Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
    const fill = (value: string) => value.split('{{secret}}').join(password);
    form.setFieldsValue({
      name: form.getFieldValue('name') || entry.name,
      type: entry.group === 'database' ? 'db' : form.getFieldValue('type'),
      databaseFlavor: flavorForImage(entry.id),
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

  const submit = async () => {
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
    if (filledHostnames.some((row) => !row.baseDomainId)) {
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
    values.databaseFlavor = values.databaseFlavor || '';
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
    close();
    onCreated();
  };

  return (
    <Modal
      title={t('create')}
      open={open}
      width={createTab === 'image' ? 880 : 640}
      onCancel={close}
      okButtonProps={{ type: 'primary' }}
      okText={t('save')}
      cancelText={t('cancel')}
      onOk={submit}
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
          databaseFlavor: '',
        }}
      >
        <Form.Item name="name" label={t('name')} rules={[{ required: true }]}>
          <Input />
        </Form.Item>
        <Form.Item name="databaseFlavor" label={t('databaseFlavor')} extra={t('databaseFlavorHint')}>
          <Select
            allowClear
            placeholder={t('databaseFlavorNone')}
            options={DATABASE_FLAVORS.map((flavor) => ({
              value: flavor.id,
              label: t(flavor.labelKey),
            }))}
          />
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
                  <Form.Item
                    name="source"
                    label={t('source')}
                    rules={createTab === 'dockerfile' ? [{ required: true }] : []}
                  >
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
                      <Form.Item
                        name="githubAppId"
                        label={t('githubIntegration')}
                        rules={[{ required: true }]}
                      >
                        <Select options={githubApps} placeholder={t('selectGithubApp')} />
                      </Form.Item>
                      <Form.Item
                        name="repoFullName"
                        label={t('repository')}
                        rules={[{ required: true }]}
                      >
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
                  <Typography.Paragraph style={{ color: '#425466' }}>
                    {t('appImageHint')}
                  </Typography.Paragraph>
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
  );
}
