import { Button, Checkbox, Form, Input, Modal, Select, Space, Spin, Table, Tabs, Tag, Typography, message } from 'antd';
import axios from 'axios';
import { useCallback, useEffect, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { confirmDestructive } from './confirmDestructive';
import type { DbEngine } from '../data/databaseFlavors';

export type { DbEngine };

const PRIVILEGES: Record<DbEngine, string[]> = {
  mysql: ['SELECT', 'INSERT', 'UPDATE', 'DELETE', 'CREATE', 'DROP', 'ALTER', 'INDEX', 'REFERENCES'],
  postgres: ['CONNECT', 'CREATE', 'TEMPORARY', 'SELECT', 'INSERT', 'UPDATE', 'DELETE', 'TRUNCATE'],
};

const IDENT = /^[A-Za-z_][A-Za-z0-9_]{0,62}$/;

type DbUser = {
  name: string;
  host?: string;
  protected?: boolean;
  superuser?: boolean;
};

type GrantRow = DbUser & {
  grants: Record<string, boolean>;
};

type UserForm = {
  name: string;
  password: string;
};

function errorText(error: unknown, fallback: string) {
  if (axios.isAxiosError(error)) {
    const data = error.response?.data as { error?: string } | undefined;
    if (data?.error) return data.error;
  }
  if (error instanceof Error && error.message) return error.message;
  return fallback;
}

function aborted(error: unknown, signal?: AbortSignal) {
  return signal?.aborted || axios.isCancel(error);
}

async function runDb(
  appId: string,
  body: Record<string, unknown>,
  fallback: string,
  signal?: AbortSignal,
) {
  const started = await api.post(`/applications/${appId}/db-manage`, body, {
    suppressErrorToast: true,
    signal,
  });
  const requestId = started.data.requestId as string;
  // The server holds each GET until the agent answers (or for a few seconds).
  // A handful of these replaces polling several times a second.
  const deadline = Date.now() + 70000;
  while (Date.now() < deadline) {
    const startedAt = Date.now();
    const polled = await api.get(`/applications/${appId}/db-manage/${requestId}`, {
      suppressErrorToast: true,
      signal,
    });
    const status = polled.data.status as string;
    if (status === 'pending') {
      if (Date.now() - startedAt < 1000) {
        await new Promise((resolve) => setTimeout(resolve, 1000));
      }
      continue;
    }
    if (status !== 'ok') {
      throw new Error(polled.data.error || fallback);
    }
    return (polled.data.result || {}) as Record<string, unknown>;
  }
  throw new Error(fallback);
}

export function DatabaseManagementTab({
  appId,
  engine,
  onChanged,
}: {
  appId: string;
  engine: DbEngine;
  onChanged?: () => void;
}) {
  const { t } = useTranslation();
  const privileges = PRIVILEGES[engine];
  const [section, setSection] = useState('users');
  const [busy, setBusy] = useState(false);
  const [users, setUsers] = useState<DbUser[]>([]);
  const [usersLoading, setUsersLoading] = useState(true);
  const [userModal, setUserModal] = useState<{ mode: 'create' | 'edit'; user?: DbUser } | null>(null);
  const [userForm] = Form.useForm<UserForm>();
  const [databases, setDatabases] = useState<string[]>([]);
  const [databasesLoading, setDatabasesLoading] = useState(true);
  const [database, setDatabase] = useState<string>();
  const [createDbOpen, setCreateDbOpen] = useState(false);
  const [dbForm] = Form.useForm<{ name: string }>();
  const [grantRows, setGrantRows] = useState<GrantRow[]>([]);
  const [grantSnapshot, setGrantSnapshot] = useState('');
  const [grantsLoading, setGrantsLoading] = useState(false);

  const fail = useCallback(
    (error: unknown) => {
      message.error(errorText(error, t('dbManageFailed')));
    },
    [t],
  );

  const loadCatalog = useCallback(async (signal?: AbortSignal) => {
    setUsersLoading(true);
    setDatabasesLoading(true);
    try {
      const result = await runDb(appId, { op: 'catalog' }, t('dbManageTimeout'), signal);
      setUsers((result.users as DbUser[]) || []);
      const names = (result.databases as string[]) || [];
      setDatabases(names);
      setDatabase((current) => current || names[0]);
    } catch (error) {
      if (!aborted(error, signal)) fail(error);
    } finally {
      if (!signal?.aborted) {
        setUsersLoading(false);
        setDatabasesLoading(false);
      }
    }
  }, [appId, fail, t]);

  const loadUsers = useCallback(async () => {
    setUsersLoading(true);
    try {
      const result = await runDb(appId, { op: 'list_users' }, t('dbManageTimeout'));
      setUsers((result.users as DbUser[]) || []);
    } catch (error) {
      fail(error);
    } finally {
      setUsersLoading(false);
    }
  }, [appId, fail, t]);

  const loadDatabases = useCallback(async () => {
    setDatabasesLoading(true);
    try {
      const result = await runDb(appId, { op: 'list_databases' }, t('dbManageTimeout'));
      const names = (result.databases as string[]) || [];
      setDatabases(names);
      setDatabase((current) => current || names[0]);
    } catch (error) {
      fail(error);
    } finally {
      setDatabasesLoading(false);
    }
  }, [appId, fail, t]);

  const loadGrants = useCallback(
    async (name: string, signal?: AbortSignal) => {
      setGrantsLoading(true);
      try {
        const result = await runDb(appId, { op: 'get_grants', database: name }, t('dbManageTimeout'), signal);
        const rows = ((result.rows as GrantRow[]) || []).map((row) => ({
          ...row,
          grants: Object.fromEntries(privileges.map((privilege) => [privilege, Boolean(row.grants?.[privilege])])),
        }));
        setGrantRows(rows);
        setGrantSnapshot(JSON.stringify(rows));
      } catch (error) {
        if (aborted(error, signal)) return;
        setGrantRows([]);
        setGrantSnapshot('');
        fail(error);
      } finally {
        if (!signal?.aborted) setGrantsLoading(false);
      }
    },
    [appId, fail, privileges, t],
  );

  useEffect(() => {
    const controller = new AbortController();
    void loadCatalog(controller.signal);
    return () => controller.abort();
  }, [loadCatalog]);

  useEffect(() => {
    if (section !== 'databases' || !database) return undefined;
    const controller = new AbortController();
    void loadGrants(database, controller.signal);
    return () => controller.abort();
  }, [section, database, loadGrants]);

  const grantsDirty = useMemo(
    () => grantSnapshot !== '' && JSON.stringify(grantRows) !== grantSnapshot,
    [grantRows, grantSnapshot],
  );

  const openCreateUser = () => {
    userForm.setFieldsValue({ name: '', password: '' });
    setUserModal({ mode: 'create' });
  };

  const openEditUser = (user: DbUser) => {
    userForm.setFieldsValue({ name: user.name, password: '' });
    setUserModal({ mode: 'edit', user });
  };

  const submitUser = async () => {
    const values = await userForm.validateFields();
    const editing = userModal?.mode === 'edit' ? userModal.user : undefined;
    const password = values.password || '';
    const rename = Boolean(editing && values.name !== editing.name);
    if (editing && !rename && !password) {
      message.info(t('dbNoChanges'));
      return;
    }
    const confirmed = await confirmDestructive(t, {
      title: editing ? t('dbConfirmUpdateUserTitle') : t('dbConfirmCreateUserTitle'),
      text: editing ? t('dbConfirmUpdateUserBody') : t('dbConfirmCreateUserBody'),
      confirmText: editing ? t('save') : t('create'),
      danger: false,
    });
    if (!confirmed) return;
    setBusy(true);
    try {
      if (editing) {
        await runDb(
          appId,
          {
            op: 'update_user',
            name: editing.name,
            newName: rename ? values.name : undefined,
            password: password || undefined,
          },
          t('dbManageTimeout'),
        );
        message.success(t('dbUserUpdated'));
        if (editing.protected && password) onChanged?.();
      } else {
        await runDb(appId, { op: 'create_user', name: values.name, password }, t('dbManageTimeout'));
        message.success(t('dbUserCreated'));
      }
      setUserModal(null);
      await loadUsers();
    } catch (error) {
      fail(error);
    } finally {
      setBusy(false);
    }
  };

  const deleteUser = async (user: DbUser) => {
    const confirmed = await confirmDestructive(t, {
      title: t('dbConfirmDeleteUserTitle'),
      text: t('dbConfirmDeleteUserBody'),
      confirmText: t('delete'),
    });
    if (!confirmed) return;
    setBusy(true);
    try {
      await runDb(appId, { op: 'delete_user', name: user.name }, t('dbManageTimeout'));
      message.success(t('dbUserDeleted'));
      await loadUsers();
    } catch (error) {
      fail(error);
    } finally {
      setBusy(false);
    }
  };

  const submitDatabase = async () => {
    const values = await dbForm.validateFields();
    const confirmed = await confirmDestructive(t, {
      title: t('dbConfirmCreateDatabaseTitle'),
      text: t('dbConfirmCreateDatabaseBody'),
      confirmText: t('create'),
      danger: false,
    });
    if (!confirmed) return;
    setBusy(true);
    try {
      const result = await runDb(appId, { op: 'create_database', name: values.name }, t('dbManageTimeout'));
      if (result.vectorExtension === false) {
        message.warning(t('dbVectorExtensionWarning'));
      } else {
        message.success(t('dbDatabaseCreated'));
      }
      setCreateDbOpen(false);
      dbForm.resetFields();
      await loadDatabases();
      setDatabase(values.name);
    } catch (error) {
      fail(error);
    } finally {
      setBusy(false);
    }
  };

  const toggleGrant = (userName: string, privilege: string, checked: boolean) => {
    setGrantRows((rows) =>
      rows.map((row) => {
        if (row.name !== userName || row.superuser) return row;
        const grants = { ...row.grants, [privilege]: checked };
        if (engine === 'postgres' && privilege !== 'CONNECT' && checked) {
          grants.CONNECT = true;
        }
        if (engine === 'postgres' && privilege === 'CONNECT' && !checked) {
          for (const key of Object.keys(grants)) grants[key] = false;
        }
        return { ...row, grants };
      }),
    );
  };

  const saveGrants = async () => {
    if (!database) return;
    const confirmed = await confirmDestructive(t, {
      title: t('dbConfirmGrantsTitle'),
      text: t('dbConfirmGrantsBody'),
      confirmText: t('save'),
      danger: false,
    });
    if (!confirmed) return;
    const grants: Record<string, string[]> = {};
    const hosts: Record<string, string> = {};
    for (const row of grantRows) {
      if (row.superuser) continue;
      grants[row.name] = privileges.filter((privilege) => row.grants[privilege]);
      if (row.host) hosts[row.name] = row.host;
    }
    setBusy(true);
    try {
      await runDb(appId, { op: 'set_grants', database, grants, hosts }, t('dbManageTimeout'));
      message.success(t('dbGrantsSaved'));
      await loadGrants(database);
    } catch (error) {
      fail(error);
    } finally {
      setBusy(false);
    }
  };

  const nameRules = [
    { required: true, message: t('nameRequired') },
    { pattern: IDENT, message: t('dbNameInvalid') },
  ];

  const usersPanel = (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 12, marginBottom: 12 }}>
        <Typography.Paragraph type="secondary" style={{ margin: 0 }}>
          {t('dbUsersHint')}
        </Typography.Paragraph>
        <Button type="primary" onClick={openCreateUser}>
          {t('dbUserCreate')}
        </Button>
      </div>
      <Table
        rowKey="name"
        size="small"
        loading={usersLoading}
        dataSource={users}
        pagination={false}
        columns={[
          { title: t('dbUserName'), dataIndex: 'name' },
          {
            title: t('role'),
            render: (_value, row: DbUser) => (
              <Space size={4}>
                {row.protected ? <Tag color="blue">{t('dbUserProtected')}</Tag> : null}
                {row.superuser ? <Tag>{t('dbUserSuperuser')}</Tag> : null}
              </Space>
            ),
          },
          {
            title: t('actions'),
            width: 180,
            render: (_value, row: DbUser) => (
              <Space>
                <Button size="small" onClick={() => openEditUser(row)}>
                  {t('edit')}
                </Button>
                <Button size="small" danger disabled={row.protected || busy} onClick={() => void deleteUser(row)}>
                  {t('delete')}
                </Button>
              </Space>
            ),
          },
        ]}
      />
    </div>
  );

  const databasesPanel = (
    <div>
      <Typography.Paragraph type="secondary">{t('dbGrantsHint')}</Typography.Paragraph>
      <Space wrap style={{ marginBottom: 16 }}>
        <Select
          style={{ minWidth: 240 }}
          placeholder={t('dbDatabaseSelect')}
          value={database}
          loading={databasesLoading}
          options={databases.map((name) => ({ value: name, label: name }))}
          onChange={(value) => {
            if (!grantsDirty) {
              setDatabase(value);
              return;
            }
            void confirmDestructive(t, {
              title: t('dbDiscardGrantsTitle'),
              text: t('dbDiscardGrantsBody'),
              confirmText: t('dbDiscardGrantsConfirm'),
            }).then((confirmed) => {
              if (confirmed) setDatabase(value);
            });
          }}
          notFoundContent={t('dbDatabaseEmpty')}
        />
        <Button
          onClick={() => {
            dbForm.resetFields();
            setCreateDbOpen(true);
          }}
        >
          {t('dbDatabaseCreate')}
        </Button>
        <Button type="primary" disabled={!database || !grantsDirty} loading={busy} onClick={() => void saveGrants()}>
          {t('dbGrantsSave')}
        </Button>
      </Space>
      {!database ? (
        <Typography.Text type="secondary">{t('dbGrantsEmpty')}</Typography.Text>
      ) : (
        <Spin spinning={grantsLoading}>
          <Table
            rowKey="name"
            size="small"
            pagination={false}
            dataSource={grantRows}
            scroll={{ x: true }}
            columns={[
              {
                title: t('dbUserName'),
                dataIndex: 'name',
                fixed: 'left',
                render: (name: string, row: GrantRow) => (
                  <Space size={6}>
                    {name}
                    {row.superuser ? <Tag>{t('dbUserSuperuser')}</Tag> : null}
                  </Space>
                ),
              },
              ...privileges.map((privilege) => ({
                title: privilege,
                width: 110,
                align: 'center' as const,
                render: (_value: unknown, row: GrantRow) => (
                  <Checkbox
                    checked={Boolean(row.grants?.[privilege])}
                    disabled={row.superuser || busy}
                    onChange={(event) => toggleGrant(row.name, privilege, event.target.checked)}
                  />
                ),
              })),
            ]}
          />
        </Spin>
      )}
    </div>
  );

  return (
    <div style={{ background: '#fff', border: '1px solid #E3E8EE', borderRadius: 8, padding: 24 }}>
      <Typography.Paragraph type="secondary" style={{ marginTop: 0 }}>
        {t('dbManagementHint')}
      </Typography.Paragraph>
      <Tabs
        activeKey={section}
        onChange={setSection}
        items={[
          { key: 'users', label: t('dbManagementUsers'), children: usersPanel },
          { key: 'databases', label: t('dbManagementDatabases'), children: databasesPanel },
        ]}
      />
      <Modal
        open={!!userModal}
        title={userModal?.mode === 'edit' ? t('dbUserEdit') : t('dbUserCreate')}
        okText={userModal?.mode === 'edit' ? t('save') : t('create')}
        cancelText={t('cancel')}
        confirmLoading={busy}
        onCancel={() => setUserModal(null)}
        onOk={() => void submitUser()}
        destroyOnClose
      >
        {userModal?.user?.protected ? (
          <Typography.Paragraph type="secondary">{t('dbUserProtectedHint')}</Typography.Paragraph>
        ) : null}
        <Form form={userForm} layout="vertical">
          <Form.Item name="name" label={t('dbUserName')} rules={nameRules}>
            <Input disabled={userModal?.user?.protected} autoComplete="off" />
          </Form.Item>
          <Form.Item
            name="password"
            label={t('dbUserPassword')}
            extra={userModal?.mode === 'edit' ? t('passwordOptional') : undefined}
            rules={userModal?.mode === 'create' ? [{ required: true, message: t('dbPasswordRequired') }] : []}
          >
            <Input.Password autoComplete="new-password" />
          </Form.Item>
        </Form>
      </Modal>
      <Modal
        open={createDbOpen}
        title={t('dbDatabaseCreate')}
        okText={t('create')}
        cancelText={t('cancel')}
        confirmLoading={busy}
        onCancel={() => setCreateDbOpen(false)}
        onOk={() => void submitDatabase()}
        destroyOnClose
      >
        <Form form={dbForm} layout="vertical">
          <Form.Item name="name" label={t('dbDatabaseName')} rules={nameRules}>
            <Input autoComplete="off" />
          </Form.Item>
        </Form>
      </Modal>
    </div>
  );
}
