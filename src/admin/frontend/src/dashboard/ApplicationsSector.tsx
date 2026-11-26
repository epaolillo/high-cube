import {
  ArrowDownOutlined,
  ArrowUpOutlined,
  DeleteOutlined,
  GithubOutlined,
  LoadingOutlined,
  PauseOutlined,
  PlayCircleOutlined,
  PlusOutlined,
  RocketOutlined,
  SlidersOutlined,
} from '@ant-design/icons';
import { Select, Space, Tooltip, message } from 'antd';
import { cloneElement, useEffect, useState, type ReactElement } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { APP_CATEGORIES, getAppCategory, type AppCategory } from '../components/appCategories';
import { ApplicationFormModal } from '../components/ApplicationFormModal';
import { confirmDestructive } from '../components/confirmDestructive';
import { HostnamePills } from '../components/HostnamePills';
import { AppIcon, IconEditor } from '../components/IconEditor';
import { MetricDisplay } from '../components/MetricDisplay';
import { useDashboardState } from './DashboardState';
import { useServerFilter } from './ServerFilter';

type AppRow = {
  id: number;
  name: string;
  type: string;
  icon?: string;
  hostname?: string;
  hostnames?: { hostname: string; tlsStatus?: string }[];
  tlsStatus?: string;
  servers?: { id: number; name: string }[];
  runtimeStatus?: 'running' | 'deploying' | 'stopped';
  runningReplicas?: number;
  startedAt?: string;
  cpuPercent?: number;
  memoryUsedBytes?: number;
  memoryLimitBytes?: number;
  diskUsedBytes?: number;
};

const STATUS_ICON = {
  running: <PlayCircleOutlined />,
  deploying: <LoadingOutlined spin />,
  stopped: <PauseOutlined />,
};

export function ApplicationsSector({
  isAdmin,
  onOpen,
  onOpenIntegrations,
}: {
  isAdmin: boolean;
  onOpen: (id: number) => void;
  onOpenIntegrations: () => void;
}) {
  const { t } = useTranslation();
  const { data, refresh, setApplicationsParams } = useDashboardState();
  const { isOn } = useServerFilter();
  const rows = data.applications.rows as AppRow[];
  // Client-side server filter: show an application when it has no server, or when at least one
  // of its servers is checked in the Servers column.
  const visibleRows = rows.filter((a) => {
    const servers = a.servers ?? [];
    return servers.length === 0 || servers.some((s) => isOn(s.id));
  });
  const total = data.applications.total;
  const [category, setCategory] = useState<AppCategory>('other');
  const [order, setOrder] = useState<'asc' | 'desc'>('asc');
  const [createOpen, setCreateOpen] = useState(false);
  const [iconEdit, setIconEdit] = useState<AppRow | null>(null);

  useEffect(() => {
    setApplicationsParams(category, order);
  }, [category, order, setApplicationsParams]);

  const deployOne = async (id: number) => {
    await api.post(`/applications/${id}/deployments`);
    message.success(t('deployStarted'));
  };
  const removeOne = (a: AppRow) =>
    confirmDestructive(t, { text: a.name }).then((ok) => {
      if (!ok) return;
      return api.delete(`/applications/${a.id}`).then(() => {
        message.success(t('deleted'));
        refresh();
      });
    });
  return (
    <section className="hc-sector hc-sector--applications rt-panel">
      <div className="hc-sector__head">
        <span className="hc-sector__title">{t('applications')}</span>
        <span className="hc-sector__count rt-key rt-key--badge">{total}</span>
        <span className="hc-sector__spacer" />
        <button type="button" className="hc-ghostbtn rt-btn" onClick={onOpenIntegrations}>
          <GithubOutlined />
          {t('integrations')}
        </button>
        <button type="button" className="hc-addbtn rt-btn rt-btn--orange" onClick={() => setCreateOpen(true)}>
          <PlusOutlined />
          {t('create')}
        </button>
      </div>

      <div className="hc-controls">
        <Select
          size="small"
          className="hc-search rt-inset"
          value={category}
          onChange={setCategory}
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
        <Tooltip title={order === 'asc' ? 'ASC' : 'DESC'}>
          <button
            type="button"
            className="hc-iconbtn rt-btn rt-btn--icon"
            onClick={() => setOrder((o) => (o === 'asc' ? 'desc' : 'asc'))}
          >
            {order === 'asc' ? <ArrowUpOutlined /> : <ArrowDownOutlined />}
          </button>
        </Tooltip>
      </div>

      <div className="hc-sector__body hc-scroll">
        {visibleRows.length === 0 ? (
          <div className="hc-sector__empty">—</div>
        ) : (
          visibleRows.map((a) => {
            const status = a.runtimeStatus ?? 'stopped';
            const hostnames =
              a.hostnames && a.hostnames.length > 0
                ? a.hostnames
                : a.hostname
                  ? [{ hostname: a.hostname, tlsStatus: a.tlsStatus }]
                  : [];
            const categoryDef = getAppCategory(a.type);
            return (
              <div className="hc-row hc-app rt-raised" key={a.id}>
                <div className="hc-row__head">
                  <Tooltip title={t('iconEditTitle')}>
                    <button
                      type="button"
                      className="hc-row-icon"
                      aria-label={t('iconEditTitle')}
                      onClick={() => setIconEdit(a)}
                    >
                      <AppIcon icon={a.icon} name={a.name} size={30} />
                    </button>
                  </Tooltip>
                  <span className="hc-row__name" title={a.name}>
                    {a.name}
                  </span>
                  <div className="hc-acts">
                    <Tooltip title={t(categoryDef.labelKey)}>
                      <span className="hc-iconbtn rt-btn rt-btn--icon rt-btn--static" data-status="meta" aria-label={t(categoryDef.labelKey)}>
                        {cloneElement(categoryDef.icon as ReactElement)}
                      </span>
                    </Tooltip>
                    <Tooltip title={t(`runtimeStatus_${status}`)}>
                      <span
                        className="hc-iconbtn rt-btn rt-btn--icon rt-btn--static"
                        data-status={status}
                        aria-label={t(`runtimeStatus_${status}`)}
                      >
                        {STATUS_ICON[status]}
                      </span>
                    </Tooltip>
                    <Tooltip title={t('deploy')}>
                      <button type="button" className="hc-iconbtn rt-btn rt-btn--icon" onClick={() => void deployOne(a.id)}>
                        <RocketOutlined />
                      </button>
                    </Tooltip>
                    <Tooltip title={t('open')}>
                      <button type="button" className="hc-iconbtn rt-btn rt-btn--icon" onClick={() => onOpen(a.id)}>
                        <SlidersOutlined />
                      </button>
                    </Tooltip>
                    <Tooltip title={t('delete')}>
                      <button
                        type="button"
                        className="hc-iconbtn rt-btn rt-btn--icon"
                        data-tone="danger"
                        onClick={() => void removeOne(a)}
                      >
                        <DeleteOutlined />
                      </button>
                    </Tooltip>
                  </div>
                </div>
                <div className="hc-row__metric rt-lcd">
                  <MetricDisplay
                    variant="phosphor"
                    cpuPercent={a.cpuPercent}
                    memoryUsedBytes={a.memoryUsedBytes}
                    memoryLimitBytes={a.memoryLimitBytes}
                    diskUsedBytes={a.diskUsedBytes}
                    startedAt={a.startedAt}
                    status={status}
                    replicas={a.runningReplicas}
                  />
                </div>
                {(hostnames.length > 0 || (a.servers && a.servers.length > 0)) && (
                  <div className="hc-row__head" style={{ gap: 8 }}>
                    {hostnames.length > 0 && <HostnamePills hostnames={hostnames} collapse />}
                    {a.servers && a.servers.length > 0 && (
                      <span className="hc-row__sub">{a.servers.map((s) => s.name).join(', ')}</span>
                    )}
                  </div>
                )}
              </div>
            );
          })
        )}
      </div>

      <ApplicationFormModal
        open={createOpen}
        isAdmin={isAdmin}
        onClose={() => setCreateOpen(false)}
        onCreated={refresh}
      />

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
          refresh();
        }}
      />
    </section>
  );
}
