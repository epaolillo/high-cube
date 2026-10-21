import { Column, DualAxes, Line } from '@ant-design/charts';
import { DeleteOutlined } from '@ant-design/icons';
import {
  Alert,
  Button,
  Checkbox,
  Form,
  Input,
  InputNumber,
  Modal,
  Select,
  Space,
  Table,
  Tabs,
  Tag,
  Timeline,
  Tooltip,
  Typography,
  message,
} from 'antd';
import { FitAddon } from '@xterm/addon-fit';
import { Terminal } from '@xterm/xterm';
import dayjs from 'dayjs';
import relativeTime from 'dayjs/plugin/relativeTime';
import 'dayjs/locale/es';
import { useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useParams } from 'react-router-dom';
import api from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { confirmRedeploy } from '../components/confirmRedeploy';
import { confirmDestructive } from '../components/confirmDestructive';
import { DockerfileExposeHint } from '../components/DockerfileExposeHint';
import { HostnamePills } from '../components/HostnamePills';
import { AppIcon, IconEditor } from '../components/IconEditor';
import { APP_CATEGORIES } from '../components/appCategories';
import { BaseDomainOption, HostnameFields, HostnameRow } from '../components/HostnameFields';
import { connectTerminalTarget, type TerminalMode, type TerminalTransport } from '../terminal/transport';
import '@xterm/xterm/css/xterm.css';

dayjs.extend(relativeTime);

const cardStyle: React.CSSProperties = {
  background: '#fff',
  border: '1px solid #E3E8EE',
  borderRadius: 8,
  padding: 24,
};

type PortMapping = {
  containerPort: number;
  hostPort: number;
};

type Application = {
  id: number;
  name: string;
  type: string;
  icon?: string;
  repoUrl: string;
  repoFullName?: string;
  source?: string;
  githubAppId?: number;
  baseDirectory?: string;
  dockerfilePath?: string;
  portsExposes?: number;
  portMappings?: PortMapping[];
  pendingRedeploy?: boolean;
  desiredReplicas?: number;
  scaleMode?: string;
  minReplicas?: number;
  maxReplicas?: number;
  scaleTargetCpuPercent?: number;
  scaleTargetMemoryPercent?: number;
  scaleTargetLoad?: number;
  scaleCooldownSeconds?: number;
  scaleWindowSeconds?: number;
  targetServers?: { serverId: number; priority: number }[];
  branch: string;
  hostname?: string;
  tlsStatus?: string;
  hostnames?: {
    id?: number;
    hostname: string;
    subdomain: string;
    baseDomainId: number;
    tlsStatus: string;
    position: number;
  }[];
  availableBaseDomains?: BaseDomainOption[];
  internalEndpoint?: string;
  env?: Record<string, string>;
  volumes?: { id: number; name: string; mountPath: string }[];
  createdBy?: number;
  ownerEmail?: string;
};

type Deployment = {
  id: number;
  gitSha: string;
  gitMessage?: string;
  status: string;
  noCache?: boolean;
  createdAt: string;
  hasBuildLog?: boolean;
  hasDeployLog?: boolean;
};

type Replica = {
  id: number;
  serverId: number;
  serverName?: string;
  serverStatus?: string;
  containerId: string;
  hostPort?: number | null;
  status: string;
  statusChangedAt?: string;
  restartCount?: number;
  shellAvailable?: boolean;
};

type VolumeRow = {
  id: number;
  name: string;
  mountPath: string;
  dockerName?: string;
  hostPath?: string | null;
  sizeBytes?: number | null;
  serverId?: number | null;
};

type MetricPoint = {
  at: string;
  cpuPercent: number;
  memoryUsedBytes: number;
  diskUsedBytes: number;
  restarts: number;
};

type LatestMetrics = {
  cpuPercent?: number;
  memoryUsedBytes?: number;
  diskUsedBytes?: number;
  restartCount?: number;
  collectedAt?: string;
};

function utcStamp(date: Date) {
  return date.toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function formatBytes(value: number) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let size = value;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(size >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}

function shortSha(sha?: string) {
  if (!sha) return '—';
  return sha.slice(0, 12);
}

function repoHref(app: Application) {
  if (app.repoFullName) return `https://github.com/${app.repoFullName}`;
  return app.repoUrl?.replace(/\.git$/, '') || '';
}

function repoLabel(app: Application) {
  return app.repoFullName || app.repoUrl || '—';
}

function isFailureStatus(status: string) {
  return status === 'failed' || status === 'restart_limit_reached' || status === 'unhealthy';
}

function aggregateStatus(replicas: Replica[]) {
  if (replicas.length === 0) return { status: 'pending', since: '' };
  const priority = [
    'failed',
    'restart_limit_reached',
    'unhealthy',
    'building',
    'starting',
    'deploying',
    'pending',
    'stopped',
    'running',
    'healthy',
  ];
  let best = replicas[0];
  for (const replica of replicas) {
    if (priority.indexOf(replica.status) < priority.indexOf(best.status)) {
      best = replica;
    }
  }
  const same = replicas.filter((r) => r.status === best.status);
  const since = same
    .map((r) => r.statusChangedAt)
    .filter(Boolean)
    .sort()
    .at(-1);
  return { status: best.status, since: since || '' };
}

function ContainerTerminal({
  replicaId,
  enabled,
  onModeChange,
}: {
  replicaId: number | undefined;
  enabled: boolean;
  onModeChange: (mode: TerminalMode | undefined) => void;
}) {
  const { t } = useTranslation();
  const containerRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const container = containerRef.current;
    if (!container || !enabled || !replicaId) return undefined;

    const term = new Terminal({
      cursorBlink: true,
      scrollback: 5000,
      fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
      fontSize: 13,
      theme: {
        background: '#0A2540',
        foreground: '#E6EDF5',
        cursor: '#635BFF',
        selectionBackground: 'rgba(99, 91, 255, 0.35)',
      },
    });
    const fit = new FitAddon();
    term.loadAddon(fit);
    term.open(container);

    const place = () => {
      const top = container.getBoundingClientRect().top;
      const next = Math.max(280, Math.floor(window.innerHeight - top - 24));
      if (container.style.height !== `${next}px`) {
        container.style.height = `${next}px`;
      }
    };
    const fitTerminal = () => {
      place();
      fit.fit();
    };
    fitTerminal();

    let disposed = false;
    let transport: TerminalTransport | undefined;
    const connecting = new AbortController();
    const initialCols = term.cols;
    const initialRows = term.rows;
    connectTerminalTarget(
      { kind: 'replica', id: String(replicaId) },
      initialCols,
      initialRows,
      {
        onData: (chunk) => {
          if (!disposed) term.write(chunk);
        },
        onClose: () => {
          if (!disposed) term.write(`\r\n[${t('terminalClosed')}]\r\n`);
        },
      },
      connecting.signal,
    )
      .then((connected) => {
        if (disposed) {
          connected.close();
          return;
        }
        transport = connected;
        onModeChange(connected.mode);
        if (term.cols !== initialCols || term.rows !== initialRows) {
          connected.resize(term.cols, term.rows);
        }
        term.focus();
      })
      .catch((error: unknown) => {
        if (disposed) return;
        const reason = error instanceof Error ? error.message : String(error);
        term.write(`\r\n[${t('terminalUnavailable')}: ${reason}]\r\n`);
      });

    const subscription = term.onData((data) => transport?.send(data));
    const resizeSubscription = term.onResize(({ cols, rows }) => transport?.resize(cols, rows));
    const content = document.querySelector('.hc-content');
    const observer = new ResizeObserver(() => fitTerminal());
    if (content) observer.observe(content);
    window.addEventListener('resize', fitTerminal);

    return () => {
      disposed = true;
      connecting.abort();
      transport?.close();
      onModeChange(undefined);
      subscription.dispose();
      resizeSubscription.dispose();
      observer.disconnect();
      window.removeEventListener('resize', fitTerminal);
      term.dispose();
    };
  }, [replicaId, enabled, t, onModeChange]);

  if (!enabled || !replicaId) {
    return (
      <div
        style={{
          minHeight: 280,
          background: '#F6F9FC',
          color: '#697386',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          padding: 24,
          textAlign: 'center',
        }}
      >
        <div>
          <Typography.Title level={5} style={{ color: '#697386', marginTop: 0 }}>
            {t('noShell')}
          </Typography.Title>
          <Typography.Text type="secondary">{t('noShellHint')}</Typography.Text>
        </div>
      </div>
    );
  }

  return (
    <div
      ref={containerRef}
      style={{ width: '100%', minHeight: 280, overflow: 'hidden', boxSizing: 'border-box' }}
    />
  );
}

function DeploymentsTab({
  appId,
  branch,
  deployments,
  onRefresh,
}: {
  appId: string;
  branch: string;
  deployments: Deployment[];
  onRefresh: () => void;
}) {
  const { t, i18n } = useTranslation();
  const [logModal, setLogModal] = useState<{
    id: number;
    phase: 'build' | 'deploy';
    title: string;
  }>();
  const [logText, setLogText] = useState('');
  const [logOffset, setLogOffset] = useState(0);
  const [logStatus, setLogStatus] = useState('');

  useEffect(() => {
    if (!logModal) return undefined;
    let stopped = false;
    let offset = 0;
    const load = async () => {
      const { data } = await api.get(`/deployments/${logModal.id}/logs`, {
        params: { since: offset, phase: logModal.phase },
      });
      if (stopped) return;
      if (data.chunk) {
        setLogText((prev) => (offset === 0 ? data.chunk : prev + data.chunk));
      } else if (offset === 0) {
        setLogText('');
      }
      offset = data.offset;
      setLogOffset(data.offset);
      setLogStatus(data.status || '');
    };
    load().catch(() => undefined);
    const timer = window.setInterval(() => {
      if (logStatus === 'deploying' || logStatus === 'pending') {
        load().catch(() => undefined);
      }
    }, 2000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [logModal, logStatus]);

  const openLogs = (deployment: Deployment, phase: 'build' | 'deploy') => {
    setLogText('');
    setLogOffset(0);
    setLogStatus(deployment.status);
    setLogModal({
      id: deployment.id,
      phase,
      title: phase === 'build' ? t('buildLogs') : t('deployLogs'),
    });
  };

  if (deployments.length === 0) {
    return (
      <div style={cardStyle}>
        <Typography.Text type="secondary">{t('noDeploymentsYet')}</Typography.Text>
      </div>
    );
  }

  return (
    <>
      <div style={cardStyle}>
        <Timeline
          items={deployments.slice(0, 10).map((deployment) => {
            const failed = isFailureStatus(deployment.status);
            return {
              color: failed ? 'red' : deployment.status === 'success' ? 'green' : 'blue',
              children: (
                <div
                  style={{
                    border: failed ? '1px solid #F5222D' : '1px solid #E3E8EE',
                    background: failed ? '#FFF1F0' : '#fff',
                    borderRadius: 8,
                    padding: 16,
                    marginBottom: 8,
                  }}
                >
                  <div
                    style={{
                      display: 'flex',
                      justifyContent: 'space-between',
                      gap: 12,
                      flexWrap: 'wrap',
                      marginBottom: 8,
                    }}
                  >
                    <Space wrap>
                      <Tag color={failed ? 'error' : deployment.status === 'success' ? 'success' : 'processing'}>
                        {deployment.status}
                      </Tag>
                      {deployment.noCache && <Tag color="orange">{t('noCacheBadge')}</Tag>}
                      <Typography.Text type="secondary">
                        {dayjs(deployment.createdAt).locale(i18n.language).format('YYYY-MM-DD HH:mm:ss')}
                        {' · '}
                        {dayjs(deployment.createdAt).locale(i18n.language).fromNow()}
                      </Typography.Text>
                    </Space>
                    <Space>
                      <Button size="small" onClick={() => openLogs(deployment, 'build')}>
                        {t('buildLogs')}
                      </Button>
                      <Button size="small" onClick={() => openLogs(deployment, 'deploy')}>
                        {t('deployLogs')}
                      </Button>
                    </Space>
                  </div>
                  <Typography.Text strong style={{ display: 'block', color: failed ? '#CF1322' : '#0A2540' }}>
                    {shortSha(deployment.gitSha)}
                    {deployment.gitMessage ? ` — ${deployment.gitMessage}` : ''}
                  </Typography.Text>
                  <Typography.Text type="secondary">
                    {t('branch')}: {branch}
                  </Typography.Text>
                  {failed && (
                    <Typography.Paragraph style={{ color: '#CF1322', marginBottom: 0, marginTop: 8 }}>
                      {t('deploymentFailed')}
                    </Typography.Paragraph>
                  )}
                </div>
              ),
            };
          })}
        />
      </div>
      <Modal
        open={!!logModal}
        title={logModal?.title}
        onCancel={() => setLogModal(undefined)}
        footer={null}
        width={840}
        destroyOnClose
      >
        <pre
          style={{
            background: isFailureStatus(logStatus) ? '#FFF1F0' : '#0A2540',
            color: isFailureStatus(logStatus) ? '#CF1322' : '#E6EDF5',
            border: isFailureStatus(logStatus) ? '1px solid #F5222D' : 'none',
            borderRadius: 8,
            padding: 16,
            maxHeight: '60vh',
            overflow: 'auto',
            fontSize: 12,
            whiteSpace: 'pre-wrap',
          }}
        >
          {logText || '…'}
        </pre>
        <Typography.Text type="secondary" style={{ fontSize: 12 }}>
          offset {logOffset}
        </Typography.Text>
        <Button size="small" style={{ marginLeft: 12 }} onClick={onRefresh}>
          {t('open')}
        </Button>
      </Modal>
      <span style={{ display: 'none' }}>{appId}</span>
    </>
  );
}

export default function ApplicationDetailPage({ id: propId }: { id?: number } = {}) {
  const params = useParams();
  const id = propId != null ? String(propId) : params.id;
  const { t, i18n } = useTranslation();
  const { isAdmin } = useAuth();
  const [app, setApp] = useState<Application>();
  const [owners, setOwners] = useState<{ id: number; email: string; name: string }[]>([]);
  const [deployments, setDeployments] = useState<Deployment[]>([]);
  const [replicas, setReplicas] = useState<Replica[]>([]);
  const [incidents, setIncidents] = useState<
    { id: number; kind: string; detail: string; restartCount: number; createdAt: string }[]
  >([]);
  const [iconOpen, setIconOpen] = useState(false);
  const [editingName, setEditingName] = useState(false);
  const [nameDraft, setNameDraft] = useState('');
  const [savingName, setSavingName] = useState(false);
  const [volumes, setVolumes] = useState<VolumeRow[]>([]);
  const [series, setSeries] = useState<MetricPoint[]>([]);
  const [latest, setLatest] = useState<LatestMetrics | null>(null);
  const [envOpen, setEnvOpen] = useState(false);
  const [envText, setEnvText] = useState('');
  const [hostnameRows, setHostnameRows] = useState<HostnameRow[]>([]);
  const [certificateProgress, setCertificateProgress] = useState<{
    jobId: string;
    hostname: string;
    status: string;
    lines: string[];
  } | null>(null);
  const certificateLogRef = useRef<HTMLPreElement>(null);
  const [domains, setDomains] = useState<BaseDomainOption[]>([]);
  const [portMappings, setPortMappings] = useState<PortMapping[]>([]);
  const serverMappings = useRef<string>('[]');
  // Live host-port availability: port number -> check state.
  type PortCheckStatus = 'checking' | 'available' | 'unavailable' | 'self' | 'unknown';
  const [portStatus, setPortStatus] = useState<Record<number, PortCheckStatus>>({});
  const hostnameDirty = useRef(false);
  const [terminalMode, setTerminalMode] = useState<TerminalMode>();
  const [selectedReplicaId, setSelectedReplicaId] = useState<number>();
  const [form] = Form.useForm();
  const [volumeForm] = Form.useForm();
  const [volumeModal, setVolumeModal] = useState<null | { mode: 'create' | 'edit'; volume?: VolumeRow }>(
    null,
  );

  const load = async () => {
    if (!id) return;
    const to = new Date();
    const from = new Date(to.getTime() - 7 * 24 * 60 * 60 * 1000);
    const [appRes, depRes, repRes, volRes, metricsRes] = await Promise.all([
      api.get(`/applications/${id}`),
      api.get(`/applications/${id}/deployments`, { params: { page: 1, pageSize: 10 } }),
      api.get(`/applications/${id}/replicas`, { params: { page: 1, pageSize: 100 } }),
      api.get(`/applications/${id}/volumes`),
      api.get(`/applications/${id}/metrics`, {
        params: { from: utcStamp(from), to: utcStamp(to) },
      }),
    ]);
    setApp(appRes.data);
    setDomains(appRes.data.availableBaseDomains || []);
    if (!hostnameDirty.current) {
      const rows: HostnameRow[] = Array.isArray(appRes.data.hostnames)
        ? appRes.data.hostnames.map(
            (row: {
              subdomain: string;
              baseDomainId: number;
              tlsStatus: string;
              hostname: string;
            }) => ({
              subdomain: row.subdomain,
              baseDomainId: row.baseDomainId,
              tlsStatus: row.tlsStatus,
              hostname: row.hostname,
            }),
          )
        : appRes.data.hostname
          ? [
              {
                subdomain: String(appRes.data.hostname).split('.')[0] || '',
                baseDomainId: (appRes.data.availableBaseDomains || [])[0]?.id || 0,
                tlsStatus: appRes.data.tlsStatus,
                hostname: appRes.data.hostname,
              },
            ]
          : [];
      setHostnameRows(rows);
    }
    const nextMappings: PortMapping[] = Array.isArray(appRes.data.portMappings)
      ? appRes.data.portMappings.map((m: PortMapping) => ({
          containerPort: Number(m.containerPort) || 0,
          hostPort: Number(m.hostPort) || 0,
        }))
      : [];
    const nextKey = JSON.stringify(nextMappings);
    setPortMappings((current) => {
      const dirty = JSON.stringify(current) !== serverMappings.current;
      // Keep local edits while the operator is typing; only sync the baseline from the server
      // when the form is clean.
      if (dirty) {
        return current;
      }
      serverMappings.current = nextKey;
      return nextMappings;
    });
    setDeployments(depRes.data.rows || []);
    setReplicas(repRes.data.rows || []);
    setVolumes(volRes.data.rows || []);
    setSeries(metricsRes.data.series || []);
    setLatest(metricsRes.data.latest || null);
    // Crash traceability is best-effort so the page still loads on older servers.
    try {
      const incRes = await api.get(`/applications/${id}/incidents`, {
        params: { page: 1, pageSize: 100 },
      });
      setIncidents(incRes.data.rows || []);
    } catch {
      setIncidents([]);
    }
    const rows: Replica[] = repRes.data.rows || [];
    const preferred =
      rows.find((r) => r.status === 'running' || r.status === 'healthy') || rows[0];
    setSelectedReplicaId((current) => {
      if (current && rows.some((r) => r.id === current)) return current;
      return preferred?.id;
    });
  };

  const [saving, setSaving] = useState(false);
  const [availableServers, setAvailableServers] = useState<
    { id: number; name: string; status: string }[]
  >([]);
  const [serversLoaded, setServersLoaded] = useState(false);
  const [targetRows, setTargetRows] = useState<
    { id: number; name: string; status: string; enabled: boolean }[]
  >([]);
  const [dragIndex, setDragIndex] = useState<number | null>(null);
  const targetsDirty = useRef(false);
  const scaleDirty = useRef(false);
  const applyingScale = useRef(false);
  const [scaleForm] = Form.useForm();
  const hasVolumes = Boolean(app?.volumes && app.volumes.length > 0);
  const scaleModeWatch = Form.useWatch('scaleMode', scaleForm);

  useEffect(() => {
    load().catch(() => undefined);
    const timer = window.setInterval(() => {
      load().catch(() => undefined);
    }, 5000);
    return () => window.clearInterval(timer);
  }, [id]);

  useEffect(() => {
    const node = certificateLogRef.current;
    if (!node) return;
    node.scrollTop = node.scrollHeight;
  }, [certificateProgress?.lines]);

  useEffect(() => {
    if (!id || !certificateProgress?.jobId || certificateProgress.status !== 'running') {
      return;
    }
    let cancelled = false;
    const timer = window.setInterval(() => {
      api
        .get(`/applications/${id}/certificate-jobs/${certificateProgress.jobId}`)
        .then(({ data }) => {
          if (cancelled) return;
          setCertificateProgress(data);
          if (data.status !== 'running') {
            load().catch(() => undefined);
          }
        })
        .catch(() => undefined);
    }, 700);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [id, certificateProgress?.jobId, certificateProgress?.status]);

  useEffect(() => {
    if (!isAdmin) return;
    api
      .get('/servers', { params: { page: 1, pageSize: 1000 } })
      .then(({ data }) => {
        setAvailableServers(data.rows || []);
        setServersLoaded(true);
      })
      .catch(() => undefined);
  }, [isAdmin]);

  useEffect(() => {
    if (!app || targetsDirty.current) return;
    const priority = new Map((app.targetServers || []).map((target) => [target.serverId, target.priority]));
    const enabled = availableServers
      .filter((server) => priority.has(server.id))
      .sort((a, b) => (priority.get(a.id) ?? 0) - (priority.get(b.id) ?? 0));
    const disabled = availableServers
      .filter((server) => !priority.has(server.id))
      .sort((a, b) => a.name.localeCompare(b.name));
    setTargetRows([
      ...enabled.map((server) => ({ ...server, enabled: true })),
      ...disabled.map((server) => ({ ...server, enabled: false })),
    ]);
  }, [app, availableServers]);

  const statusInfo = useMemo(() => aggregateStatus(replicas), [replicas]);
  const uniqueServers = useMemo(
    () => new Set(replicas.map((r) => r.serverId)).size,
    [replicas],
  );
  const latestDeploy = deployments[0];
  const selectedReplica = replicas.find((r) => r.id === selectedReplicaId);
  const shellEnabled = Boolean(
    selectedReplica &&
      selectedReplica.containerId &&
      (selectedReplica.status === 'running' || selectedReplica.status === 'healthy') &&
      selectedReplica.serverStatus === 'online',
  );

  const redeploy = async (noCache = false) => {
    if (!id) return;
    const conflict = portMappings.find(
      (mapping) => mapping.hostPort > 0 && portStatus[mapping.hostPort] === 'unavailable',
    );
    if (conflict) {
      message.error(t('hostPortUnavailable', { port: conflict.hostPort }));
      return;
    }
    await api.post(`/applications/${id}/deployments`, noCache ? { noCache: true } : {});
    message.success(t('deployStarted'));
    await load();
  };

  const saveName = async () => {
    if (!id) return;
    const next = nameDraft.trim();
    if (!next) {
      message.error(t('nameRequired'));
      return;
    }
    setSavingName(true);
    try {
      await api.patch(`/applications/${id}`, { name: next });
      setEditingName(false);
      await load();
      message.success(t('nameSaved'));
    } finally {
      setSavingName(false);
    }
  };

  const saveIcon = async (icon: string) => {
    if (!id) return;
    await api.patch(`/applications/${id}`, { icon });
    setIconOpen(false);
    await load();
    message.success(t('iconSaved'));
  };

  const saveType = async (type: string) => {
    if (!id) return;
    await api.patch(`/applications/${id}`, { type });
    await load();
    message.success(t('saved'));
  };

  const saveOwner = async (createdBy: number) => {
    if (!id) return;
    await api.patch(`/applications/${id}`, { createdBy });
    await load();
    message.success(t('ownerSaved'));
  };

  useEffect(() => {
    if (!isAdmin) return;
    api
      .get<{ rows: { id: number; email: string; name: string }[] }>('/users', {
        params: { page: 1, pageSize: 1000 },
      })
      .then(({ data }) => setOwners(data.rows || []))
      .catch(() => undefined);
  }, [isAdmin]);

  useEffect(() => {
    if (!app || scaleDirty.current) return;
    applyingScale.current = true;
    scaleForm.setFieldsValue({
      desiredReplicas: app.desiredReplicas ?? 1,
      scaleMode: app.scaleMode || 'manual',
      minReplicas: app.minReplicas ?? 1,
      maxReplicas: app.maxReplicas ?? app.desiredReplicas ?? 1,
      scaleTargetCpuPercent: app.scaleTargetCpuPercent ?? 70,
      scaleTargetMemoryPercent: app.scaleTargetMemoryPercent ?? 80,
      scaleTargetLoad: app.scaleTargetLoad ?? 1,
      scaleCooldownSeconds: app.scaleCooldownSeconds ?? 120,
      scaleWindowSeconds: app.scaleWindowSeconds ?? 180,
    });
    applyingScale.current = false;
  }, [app, scaleForm]);

  // Asks the agent whether a host port is free on the target servers, then polls
  // for the outcome. Runs quickly so the UI can block save/redeploy on a conflict.
  const checkHostPort = async (port: number) => {
    if (!id || !port || port <= 0) return;
    setPortStatus((current) => ({ ...current, [port]: 'checking' }));
    try {
      const { data } = await api.post(`/applications/${id}/port-check`, { port });
      const checkId = data.checkId as string;
      for (let i = 0; i < 20; i += 1) {
        await new Promise((resolve) => setTimeout(resolve, 400));
        const res = await api.get(`/applications/${id}/port-check/${checkId}`);
        if (res.data.status !== 'pending') {
          setPortStatus((current) => ({ ...current, [port]: res.data.status as PortCheckStatus }));
          return;
        }
      }
      setPortStatus((current) => ({ ...current, [port]: 'unknown' }));
    } catch {
      setPortStatus((current) => ({ ...current, [port]: 'unknown' }));
    }
  };

  const hostPortsKey = portMappings.map((mapping) => mapping.hostPort).join(',');
  useEffect(() => {
    const ports = Array.from(
      new Set(portMappings.map((mapping) => mapping.hostPort).filter((value) => value > 0)),
    );
    if (ports.length === 0) {
      return;
    }
    // Ports already published by this same application are "in use by itself": they are
    // reported busy by the agent, so mark them as 'self' (allowed) instead of checking.
    let savedHostPorts: number[] = [];
    try {
      savedHostPorts = (JSON.parse(serverMappings.current) as PortMapping[])
        .map((mapping) => Number(mapping.hostPort) || 0)
        .filter((value) => value > 0);
    } catch {
      savedHostPorts = [];
    }
    const handle = window.setTimeout(() => {
      ports.forEach((port) => {
        if (savedHostPorts.includes(port)) {
          setPortStatus((current) => ({ ...current, [port]: 'self' }));
        } else {
          checkHostPort(port);
        }
      });
    }, 500);
    return () => window.clearTimeout(handle);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [hostPortsKey, id]);

  // A conflicting host port (busy on a target server) blocks save and redeploy.
  const conflictingHostPort = portMappings.find(
    (mapping) => mapping.hostPort > 0 && portStatus[mapping.hostPort] === 'unavailable',
  );

  const saveApplication = async () => {
    if (!id || saving || !app) return;
    if (isAdmin) {
      for (const mapping of portMappings) {
        if (!mapping.containerPort || mapping.containerPort < 1 || mapping.containerPort > 65535) {
          message.error(t('containerPort'));
          return;
        }
        if (mapping.hostPort < 0 || mapping.hostPort > 65535) {
          message.error(t('hostPort'));
          return;
        }
      }
      if (conflictingHostPort) {
        message.error(t('hostPortUnavailable', { port: conflictingHostPort.hostPort }));
        return;
      }
    }
    setSaving(true);
    let saved = false;
    try {
      const patch: Record<string, unknown> = {};
      if (hostnameRows.some((row) => !row.subdomain || !row.baseDomainId)) {
        message.error(t('hostnameRequired'));
        setSaving(false);
        return;
      }
      patch.hostnames = hostnameRows.map((row) => ({
        subdomain: row.subdomain,
        baseDomainId: row.baseDomainId,
      }));
      if (isAdmin) {
        if (JSON.stringify(portMappings) !== serverMappings.current) {
          patch.portMappings = portMappings;
        }
        if (serversLoaded) {
          patch.targetServers = targetRows
            .filter((row) => row.enabled)
            .map((row, priority) => ({ serverId: row.id, priority }));
        }
        const values = await scaleForm.validateFields();
        const scaleBody =
          hasVolumes || values.scaleMode === 'manual'
            ? {
                desiredReplicas: hasVolumes ? 1 : values.desiredReplicas,
                scaleMode: 'manual',
                minReplicas: hasVolumes ? 1 : values.desiredReplicas,
                maxReplicas: hasVolumes ? 1 : values.desiredReplicas,
              }
            : {
                scaleMode: values.scaleMode,
                minReplicas: values.minReplicas,
                maxReplicas: values.maxReplicas,
                desiredReplicas: values.desiredReplicas,
                scaleTargetCpuPercent: values.scaleTargetCpuPercent,
                scaleTargetMemoryPercent: values.scaleTargetMemoryPercent,
                scaleTargetLoad: values.scaleTargetLoad,
                scaleCooldownSeconds: values.scaleCooldownSeconds,
                scaleWindowSeconds: values.scaleWindowSeconds,
              };
        await api.post(`/applications/${id}/scale`, scaleBody);
      }
      const { data } = await api.patch(`/applications/${id}`, patch);
      const savedMappings: PortMapping[] = Array.isArray(data.portMappings)
        ? data.portMappings.map((mapping: PortMapping) => ({
            containerPort: Number(mapping.containerPort) || 0,
            hostPort: Number(mapping.hostPort) || 0,
          }))
        : [];
      serverMappings.current = JSON.stringify(savedMappings);
      setPortMappings(savedMappings);
      hostnameDirty.current = false;
      targetsDirty.current = false;
      scaleDirty.current = false;
      setApp(data);
      saved = true;
      await load();
    } catch {
      /* validation or request error */
    } finally {
      setSaving(false);
    }
    if (!saved) return;
    if (await confirmRedeploy(t)) {
      await redeploy(false);
    }
  };

  const confirmForceRedeploy = async () => {
    const ok = await confirmDestructive(t, {
      title: t('forceRedeployConfirmTitle'),
      text: t('forceRedeployConfirmBody'),
      confirmText: t('forceRedeployConfirmOk'),
    });
    if (ok) await redeploy(true);
  };

  // Stop the application and keep it down so the container does not come back up.
  // This lets the operator delete the associated volume and then the application.
  const confirmStopApplication = async () => {
    if (!id) return;
    const ok = await confirmDestructive(t, {
      title: t('stopApplicationConfirmTitle'),
      text: t('stopApplicationConfirmBody'),
      confirmText: t('stopApplication'),
    });
    if (!ok) return;
    await api.post(`/applications/${id}/stop`);
    message.success(t('applicationStopped'));
    await load();
  };

  if (!app || !id) return null;

  const chartData = series.map((point) => ({
    at: dayjs(point.at).format('MM-DD HH:mm'),
    cpu: Number(point.cpuPercent.toFixed(2)),
    memoryMb: Number((point.memoryUsedBytes / (1024 * 1024)).toFixed(1)),
    diskMb: Number((point.diskUsedBytes / (1024 * 1024)).toFixed(1)),
    restarts: point.restarts,
  }));

  const overview = (
    <>
      <div
        style={{
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))',
          gap: 16,
          marginBottom: 16,
        }}
      >
        <div style={cardStyle}>
          <Typography.Text type="secondary">{t('consumption')} · {t('cpu')}</Typography.Text>
          <Typography.Title level={3} style={{ margin: '8px 0' }}>
            {latest?.cpuPercent === undefined ? '—' : `${latest.cpuPercent.toFixed(1)}%`}
          </Typography.Title>
        </div>
        <div style={cardStyle}>
          <Typography.Text type="secondary">{t('memory')}</Typography.Text>
          <Typography.Title level={3} style={{ margin: '8px 0' }}>
            {latest?.memoryUsedBytes === undefined ? '—' : formatBytes(latest.memoryUsedBytes)}
          </Typography.Title>
        </div>
        <div style={cardStyle}>
          <Typography.Text type="secondary">{t('disk')}</Typography.Text>
          <Typography.Title level={3} style={{ margin: '8px 0' }}>
            {latest?.diskUsedBytes === undefined ? '—' : formatBytes(latest.diskUsedBytes)}
          </Typography.Title>
        </div>
      </div>

      <div style={{ ...cardStyle, marginBottom: 16 }}>
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
            {t('volumes')}
          </Typography.Title>
          <Button
            onClick={() => {
              volumeForm.setFieldsValue({ name: 'data', mountPath: '' });
              setVolumeModal({ mode: 'create' });
            }}
          >
            {t('addVolume')}
          </Button>
        </div>
        <Typography.Paragraph type="secondary" style={{ marginTop: 0 }}>
          {t('volumeEditStoppedHint')}
        </Typography.Paragraph>
        {volumes.length === 0 ? (
          <Typography.Text type="secondary">{t('noVolumes')}</Typography.Text>
        ) : (
          <Table
            rowKey="id"
            pagination={false}
            dataSource={volumes}
            columns={[
              { title: t('name'), dataIndex: 'name' },
              { title: t('volumeHostPath'), dataIndex: 'hostPath', render: (v) => v || '—' },
              { title: t('volumeMountPath'), dataIndex: 'mountPath' },
              {
                title: t('volumeSize'),
                dataIndex: 'sizeBytes',
                render: (v) => (typeof v === 'number' ? formatBytes(v) : '—'),
              },
              {
                title: '',
                key: 'actions',
                render: (_, row) => (
                  <Space>
                    <Button
                      type="link"
                      onClick={() => {
                        volumeForm.setFieldsValue({ name: row.name, mountPath: row.mountPath });
                        setVolumeModal({ mode: 'edit', volume: row });
                      }}
                    >
                      {t('editVolume')}
                    </Button>
                    <Button
                      danger
                      type="link"
                      onClick={async () => {
                        const ok = await confirmDestructive(t, {
                          title: t('deleteVolume'),
                          text: t('confirmDeleteVolume'),
                          confirmText: t('deleteVolume'),
                        });
                        if (!ok) return;
                        await api.delete(`/applications/${id}/volumes/${row.id}`);
                        message.success(t('volumeDeleted'));
                        await load();
                      }}
                    >
                      {t('deleteVolume')}
                    </Button>
                  </Space>
                ),
              },
            ]}
          />
        )}
        <Modal
          open={Boolean(volumeModal)}
          title={volumeModal?.mode === 'edit' ? t('editVolume') : t('addVolume')}
          okButtonProps={{ type: 'primary' }}
          onCancel={() => setVolumeModal(null)}
          onOk={async () => {
            const values = await volumeForm.validateFields();
            if (volumeModal?.mode === 'edit' && volumeModal.volume) {
              await api.patch(`/applications/${id}/volumes/${volumeModal.volume.id}`, values);
              message.success(t('volumeUpdated'));
            } else {
              await api.post(`/applications/${id}/volumes`, values);
              message.success(t('volumeAdded'));
            }
            setVolumeModal(null);
            volumeForm.resetFields();
            await load();
          }}
        >
          <Form form={volumeForm} layout="vertical" style={{ marginTop: 12 }}>
            <Form.Item
              name="name"
              label={t('volumeName')}
              rules={[{ required: true }, { pattern: /^[A-Za-z0-9._-]+$/, message: t('volumeName') }]}
            >
              <Input placeholder="data" />
            </Form.Item>
            <Form.Item name="mountPath" label={t('volumeMountPath')} rules={[{ required: true }]}>
              <Input placeholder="/var/lib/data" />
            </Form.Item>
          </Form>
        </Modal>
      </div>

        <div style={{ ...cardStyle, marginBottom: 16 }}>
          <Typography.Title level={5} style={{ marginTop: 0 }}>
            {t('domainSettings')}
          </Typography.Title>
          {app.internalEndpoint ? (
            <Typography.Paragraph style={{ marginBottom: 12 }}>
              <Typography.Text type="secondary">{t('internalEndpoint')}: </Typography.Text>
              <Typography.Text code>{app.internalEndpoint}</Typography.Text>
            </Typography.Paragraph>
          ) : null}
          <HostnameFields
            domains={domains}
            value={hostnameRows}
            showTls
            certificateBusy={certificateProgress?.status === 'running'}
            onRequestCertificate={(row) => {
              if (!id || !row.hostname) return;
              setCertificateProgress({
                jobId: '',
                hostname: row.hostname,
                status: 'running',
                lines: [t('certificateStarting', { hostname: row.hostname })],
              });
              api
                .post(`/applications/${id}/certificate`, { hostname: row.hostname })
                .then(({ data }) => {
                  setCertificateProgress(data);
                  if (data.status !== 'running') {
                    return load();
                  }
                  return undefined;
                })
                .catch((error: { response?: { data?: { error?: string } } }) => {
                  const text = error.response?.data?.error || t('operationFailed');
                  setCertificateProgress((current) =>
                    current
                      ? { ...current, status: 'failed', lines: [...current.lines, text] }
                      : current,
                  );
                });
            }}
            onChange={(rows) => {
              hostnameDirty.current = true;
              setHostnameRows(rows);
            }}
          />
          <Modal
            open={Boolean(certificateProgress)}
            title={t('certificateProgressTitle', { hostname: certificateProgress?.hostname || '' })}
            width={760}
            closable={certificateProgress?.status !== 'running'}
            maskClosable={false}
            onCancel={() => {
              if (certificateProgress?.status === 'running') return;
              setCertificateProgress(null);
            }}
            footer={[
              <Button
                key="close"
                type="primary"
                disabled={certificateProgress?.status === 'running'}
                onClick={() => setCertificateProgress(null)}
              >
                {t('close')}
              </Button>,
            ]}
          >
            {certificateProgress ? (
              <>
                <Tag
                  color={
                    certificateProgress.status === 'active'
                      ? 'success'
                      : certificateProgress.status === 'failed'
                        ? 'error'
                        : 'processing'
                  }
                >
                  {certificateProgress.status === 'active'
                    ? t('certificateActive')
                    : certificateProgress.status === 'failed'
                      ? t('certificateFailed')
                      : t('certificateRunning')}
                </Tag>
                <pre
                  ref={certificateLogRef}
                  style={{
                    marginTop: 12,
                    padding: 12,
                    background: '#0A2540',
                    color: '#E8EEF4',
                    borderRadius: 8,
                    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
                    fontSize: 12,
                    lineHeight: 1.55,
                    maxHeight: 420,
                    overflow: 'auto',
                    whiteSpace: 'pre-wrap',
                    wordBreak: 'break-word',
                  }}
                >
                  {certificateProgress.lines.join('\n')}
                </pre>
              </>
            ) : null}
          </Modal>
        </div>

      <div style={{ ...cardStyle, marginBottom: 16 }}>
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
            {t('portExpose')}
          </Typography.Title>
          {isAdmin && (
            <Button
              onClick={() =>
                setPortMappings((rows) => [...rows, { containerPort: 80, hostPort: 0 }])
              }
            >
              {t('addExposePort')}
            </Button>
          )}
        </div>
        <Space direction="vertical" size={12} style={{ width: '100%' }}>
          {portMappings.length === 0 ? (
            <Typography.Text type="secondary">{t('noPortMappings')}</Typography.Text>
          ) : (
            portMappings.map((mapping, index) => {
              const status = mapping.hostPort > 0 ? portStatus[mapping.hostPort] : undefined;
              const statusColor: Record<PortCheckStatus, string> = {
                checking: 'default',
                available: 'green',
                unavailable: 'red',
                self: 'blue',
                unknown: 'default',
              };
              return (
                <div
                  key={`port-${index}`}
                  style={{ display: 'flex', gap: 12, alignItems: 'flex-end', flexWrap: 'wrap' }}
                >
                  <div>
                    <Typography.Text type="secondary" style={{ fontSize: 12, display: 'block' }}>
                      {t('containerPort')}
                    </Typography.Text>
                    <InputNumber
                      min={1}
                      max={65535}
                      value={mapping.containerPort || undefined}
                      disabled={!isAdmin}
                      onChange={(value) => {
                        const next = [...portMappings];
                        next[index] = { ...next[index], containerPort: value || 0 };
                        setPortMappings(next);
                      }}
                      style={{ width: 160 }}
                    />
                  </div>
                  <div>
                    <Typography.Text type="secondary" style={{ fontSize: 12, display: 'block' }}>
                      {t('hostPort')}
                    </Typography.Text>
                    <InputNumber
                      min={0}
                      max={65535}
                      value={mapping.hostPort}
                      disabled={!isAdmin}
                      status={status === 'unavailable' ? 'error' : undefined}
                      onChange={(value) => {
                        const next = [...portMappings];
                        next[index] = { ...next[index], hostPort: value ?? 0 };
                        setPortMappings(next);
                      }}
                      style={{ width: 160 }}
                    />
                  </div>
                  {status && (
                    <Tooltip
                      title={status === 'self' ? t('portStatusSelfHint') : undefined}
                    >
                      <Tag
                        color={statusColor[status]}
                        style={{
                          height: 32,
                          display: 'inline-flex',
                          alignItems: 'center',
                          margin: 0,
                        }}
                      >
                        {t(`portStatus_${status}`)}
                      </Tag>
                    </Tooltip>
                  )}
                  {isAdmin && (
                    <Tooltip title={t('remove')}>
                      <Button
                        danger
                        aria-label={t('remove')}
                        icon={<DeleteOutlined />}
                        onClick={() =>
                          setPortMappings((rows) => rows.filter((_, i) => i !== index))
                        }
                      />
                    </Tooltip>
                  )}
                </div>
              );
            })
          )}
          <Typography.Text type="secondary" style={{ fontSize: 12 }}>
            {t('hostPortExplicitHint')}
          </Typography.Text>
          {!isAdmin && <Typography.Text type="secondary">{t('portExposeAdminOnly')}</Typography.Text>}
          <DockerfileExposeHint
            source={app.source}
            githubAppId={app.githubAppId}
            repo={app.repoFullName}
            repoUrl={app.repoUrl}
            branch={app.branch}
            baseDirectory={app.baseDirectory}
            dockerfilePath={app.dockerfilePath}
          />
        </Space>
      </div>

      <div style={{ marginBottom: 16 }}>
        <Button
          onClick={() => {
            const env = app.env || {};
            setEnvText(
              Object.entries(env)
                .map(([k, v]) => `${k}=${v}`)
                .join('\n'),
            );
            setEnvOpen(true);
          }}
        >
          {t('envVariables')}
        </Button>
      </div>

      {isAdmin && (
        <div style={{ ...cardStyle, marginBottom: 16 }}>
          <Typography.Title level={5} style={{ marginTop: 0 }}>
            {t('deployServers')}
          </Typography.Title>
          <Typography.Paragraph type="secondary" style={{ marginTop: 0 }}>
            {t('deployServersHint')}
          </Typography.Paragraph>
          {targetRows.length === 0 ? (
            <Typography.Text type="secondary">{t('deployServersEmpty')}</Typography.Text>
          ) : (
            targetRows.map((row, index) => (
              <div
                key={row.id}
                draggable
                onDragStart={() => setDragIndex(index)}
                onDragOver={(event) => event.preventDefault()}
                onDrop={() => {
                  if (dragIndex === null || dragIndex === index) return;
                  targetsDirty.current = true;
                  setTargetRows((rows) => {
                    const next = [...rows];
                    const [moved] = next.splice(dragIndex, 1);
                    next.splice(index, 0, moved);
                    return next;
                  });
                  setDragIndex(null);
                }}
                style={{
                  display: 'flex',
                  alignItems: 'center',
                  gap: 12,
                  padding: '8px 4px',
                  borderTop: '1px solid #E3E8EE',
                  cursor: 'grab',
                  background: dragIndex === index ? '#F6F9FC' : 'transparent',
                }}
              >
                <span aria-hidden style={{ color: '#8898AA', width: 16 }}>
                  ::
                </span>
                <Checkbox
                  checked={row.enabled}
                  onChange={(event) => {
                    targetsDirty.current = true;
                    const checked = event.target.checked;
                    setTargetRows((rows) =>
                      rows.map((item) => (item.id === row.id ? { ...item, enabled: checked } : item)),
                    );
                  }}
                />
                <span style={{ flex: 1 }}>{row.name}</span>
                <Tag>{row.status}</Tag>
                <input type="hidden" name={`targetPriority-${row.id}`} value={index} readOnly />
              </div>
            ))
          )}
        </div>
      )}

      {isAdmin && (
        <div style={{ ...cardStyle, marginBottom: 16 }}>
          <Typography.Title level={5} style={{ marginTop: 0 }}>
            {t('scale')}
          </Typography.Title>
          {hasVolumes && (
            <Alert
              type="info"
              showIcon
              style={{ marginBottom: 12 }}
              message={t('scaleVolumesLocked')}
            />
          )}
          <Form
            form={scaleForm}
            layout="vertical"
            disabled={hasVolumes}
            onValuesChange={() => {
              if (!applyingScale.current) scaleDirty.current = true;
            }}
          >
            <Form.Item name="scaleMode" label={t('scaleMode')} rules={[{ required: true }]}>
              <Select
                options={[
                  { value: 'manual', label: t('scaleModeManual') },
                  { value: 'cpu', label: t('scaleModeCpu') },
                  { value: 'memory', label: t('scaleModeMemory') },
                  { value: 'load', label: t('scaleModeLoad') },
                  { value: 'composite', label: t('scaleModeComposite') },
                ]}
              />
            </Form.Item>
            {(scaleModeWatch === 'manual' || hasVolumes) && (
              <Form.Item name="desiredReplicas" label={t('replicas')} rules={[{ required: true }]}>
                <InputNumber min={1} max={100} style={{ width: '100%' }} />
              </Form.Item>
            )}
            {scaleModeWatch !== 'manual' && !hasVolumes && (
              <>
                <Space wrap style={{ width: '100%' }} size={16}>
                  <Form.Item name="minReplicas" label={t('minReplicas')} rules={[{ required: true }]}>
                    <InputNumber min={1} max={100} />
                  </Form.Item>
                  <Form.Item name="maxReplicas" label={t('maxReplicas')} rules={[{ required: true }]}>
                    <InputNumber min={1} max={100} />
                  </Form.Item>
                  <Form.Item name="desiredReplicas" label={t('currentReplicas')}>
                    <InputNumber min={1} max={100} />
                  </Form.Item>
                </Space>
                {(scaleModeWatch === 'cpu' || scaleModeWatch === 'composite') && (
                  <Form.Item name="scaleTargetCpuPercent" label={t('scaleTargetCpu')}>
                    <InputNumber min={1} max={100} style={{ width: '100%' }} addonAfter="%" />
                  </Form.Item>
                )}
                {(scaleModeWatch === 'memory' || scaleModeWatch === 'composite') && (
                  <Form.Item name="scaleTargetMemoryPercent" label={t('scaleTargetMemory')}>
                    <InputNumber min={1} max={100} style={{ width: '100%' }} addonAfter="%" />
                  </Form.Item>
                )}
                {(scaleModeWatch === 'load' || scaleModeWatch === 'composite') && (
                  <Form.Item name="scaleTargetLoad" label={t('scaleTargetLoad')}>
                    <InputNumber min={0.1} max={64} step={0.1} style={{ width: '100%' }} />
                  </Form.Item>
                )}
                <Space wrap size={16}>
                  <Form.Item name="scaleCooldownSeconds" label={t('scaleCooldown')}>
                    <InputNumber min={30} max={3600} addonAfter="s" />
                  </Form.Item>
                  <Form.Item name="scaleWindowSeconds" label={t('scaleWindow')}>
                    <InputNumber min={30} max={3600} addonAfter="s" />
                  </Form.Item>
                </Space>
              </>
            )}
          </Form>
        </div>
      )}

      <div style={{ ...cardStyle, marginBottom: 16 }}>
        <Typography.Title level={5} style={{ marginTop: 0 }}>
          {t('serversRunning')}
        </Typography.Title>
        <Typography.Paragraph type="secondary">
          {t('containersCount', { containers: replicas.length, servers: uniqueServers })}
        </Typography.Paragraph>
        <Table
          rowKey="id"
          pagination={false}
          dataSource={replicas}
          columns={[
            { title: t('servers'), dataIndex: 'serverName' },
            { title: 'Container', dataIndex: 'containerId', render: (v) => (v ? shortSha(v) : '—') },
            { title: 'Port', dataIndex: 'hostPort', render: (v) => v ?? '—' },
            {
              title: t('status'),
              dataIndex: 'status',
              render: (status: string) => (
                <Tag color={isFailureStatus(status) ? 'error' : status === 'healthy' || status === 'running' ? 'success' : 'default'}>
                  {status}
                </Tag>
              ),
            },
            {
              title: t('actions'),
              render: (_: unknown, row: Replica) => (
                <Space>
                  <Button size="small" onClick={() => api.post(`/replicas/${row.id}/restart`).then(load)}>
                    {t('restart')}
                  </Button>
                  <Button
                    size="small"
                    danger
                    type="primary"
                    onClick={async () => {
                      if (!(await confirmDestructive(t, { confirmText: t('stop') }))) return;
                      await api.post(`/replicas/${row.id}/stop`);
                      await load();
                    }}
                  >
                    {t('stop')}
                  </Button>
                </Space>
              ),
            },
          ]}
        />
      </div>

      <div style={{ ...cardStyle, marginBottom: 16 }}>
        <Typography.Title level={5} style={{ marginTop: 0 }}>
          {t('historicalMetrics')}
        </Typography.Title>
        {chartData.length < 2 ? (
          <Typography.Text type="secondary">{t('noMetricsYet')}</Typography.Text>
        ) : (
          <div style={{ display: 'grid', gap: 24 }}>
            <div>
              <Typography.Text type="secondary">
                {t('restarts')} / {t('cpu')}
              </Typography.Text>
              <DualAxes
                height={240}
                data={[chartData, chartData]}
                xField="at"
                yField={['restarts', 'cpu']}
                geometryOptions={[
                  { geometry: 'column', color: '#F5222D' },
                  { geometry: 'line', color: '#635BFF', smooth: true },
                ]}
              />
            </div>
            <div>
              <Typography.Text type="secondary">{t('memory')} (MB)</Typography.Text>
              <Line height={200} data={chartData} xField="at" yField="memoryMb" color="#09825D" smooth />
            </div>
            <div>
              <Typography.Text type="secondary">{t('disk')} (MB)</Typography.Text>
              <Column height={200} data={chartData} xField="at" yField="diskMb" color="#0A2540" />
            </div>
          </div>
        )}
      </div>

      <div style={{ ...cardStyle, padding: 0, overflow: 'hidden' }}>
        <div
          style={{
            padding: '16px 24px',
            borderBottom: '1px solid #E3E8EE',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            gap: 12,
            flexWrap: 'wrap',
          }}
        >
          <Typography.Text strong>{t('containerTerminal')}</Typography.Text>
          <Space>
            {replicas.length > 1 && (
              <Select
                style={{ minWidth: 180 }}
                value={selectedReplicaId}
                onChange={setSelectedReplicaId}
                options={replicas.map((r) => ({
                  value: r.id,
                  label: `${r.serverName || r.serverId} · ${r.status}`,
                }))}
              />
            )}
            {terminalMode === 'polling' && (
              <Tooltip title={t('terminalPollingHint')}>
                <Tag color="gold" style={{ marginInlineEnd: 0 }}>
                  {t('terminalPolling')}
                </Tag>
              </Tooltip>
            )}
          </Space>
        </div>
        <ContainerTerminal
          key={selectedReplicaId}
          replicaId={selectedReplicaId}
          enabled={shellEnabled}
          onModeChange={setTerminalMode}
        />
      </div>
    </>
  );

  return (
    <div>
      <div
        style={{
          display: 'flex',
          justifyContent: 'space-between',
          alignItems: 'flex-start',
          gap: 16,
          marginBottom: 16,
          flexWrap: 'wrap',
        }}
      >
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
            <Tooltip title={t('iconEditTitle')}>
              <button
                type="button"
                aria-label={t('iconEditTitle')}
                onClick={() => setIconOpen(true)}
                style={{ border: 'none', background: 'transparent', padding: 0, cursor: 'pointer' }}
              >
                <AppIcon icon={app.icon} name={app.name} size={44} />
              </button>
            </Tooltip>
            {editingName ? (
              <Space.Compact>
                <Input
                  autoFocus
                  value={nameDraft}
                  onChange={(e) => setNameDraft(e.target.value)}
                  onPressEnter={saveName}
                  style={{ width: 280 }}
                />
                <Button type="primary" loading={savingName} onClick={saveName}>
                  {t('save')}
                </Button>
                <Button onClick={() => setEditingName(false)}>{t('cancel')}</Button>
              </Space.Compact>
            ) : (
              <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <Typography.Title level={3} style={{ color: '#0A2540', margin: 0 }}>
                  {app.name}
                </Typography.Title>
                <Tooltip title={t('editName')}>
                  <Button
                    type="text"
                    size="small"
                    aria-label={t('editName')}
                    onClick={() => {
                      setNameDraft(app.name);
                      setEditingName(true);
                    }}
                    icon={
                      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" aria-hidden>
                        <path
                          d="M4 20h4L18 10l-4-4L4 16v4Z"
                          stroke="currentColor"
                          strokeWidth="1.8"
                          strokeLinejoin="round"
                        />
                        <path
                          d="M13.5 6.5l4 4"
                          stroke="currentColor"
                          strokeWidth="1.8"
                          strokeLinecap="round"
                        />
                      </svg>
                    }
                  />
                </Tooltip>
              </div>
            )}
            {isAdmin && (
              <Tooltip title={t('owner')}>
                <Select
                  value={app.createdBy || undefined}
                  onChange={(value) => void saveOwner(value)}
                  style={{ minWidth: 200 }}
                  options={[
                    ...(app.createdBy && !owners.some((user) => user.id === app.createdBy)
                      ? [{ value: app.createdBy, label: app.ownerEmail || t('owner') }]
                      : []),
                    ...owners.map((user) => ({
                      value: user.id,
                      label: user.name ? `${user.name} (${user.email})` : user.email,
                    })),
                  ]}
                  placeholder={app.ownerEmail || t('owner')}
                />
              </Tooltip>
            )}
            <Tooltip title={t('category')}>
              <Select
                value={APP_CATEGORIES.some((def) => def.value === app.type) ? app.type : 'other'}
                onChange={(value) => void saveType(value)}
                style={{ minWidth: 170 }}
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
            </Tooltip>
          </div>
          {app.hostnames && app.hostnames.length > 0 ? (
            <div style={{ marginTop: 10 }}>
              <HostnamePills hostnames={app.hostnames} />
            </div>
          ) : null}
          <div style={{ marginTop: 8, color: '#425466' }}>
            <div>
              <Typography.Text type="secondary">{t('repository')}: </Typography.Text>
              {repoHref(app) ? (
                <Typography.Link href={repoHref(app)} target="_blank" rel="noreferrer">
                  {repoLabel(app)}
                </Typography.Link>
              ) : (
                repoLabel(app)
              )}
              <Typography.Text type="secondary"> · {app.branch}</Typography.Text>
            </div>
            <div>
              <Typography.Text type="secondary">{t('lastDeploy')}: </Typography.Text>
              <Typography.Text code>{shortSha(latestDeploy?.gitSha)}</Typography.Text>
              {latestDeploy?.gitMessage ? (
                <Typography.Text> — {latestDeploy.gitMessage}</Typography.Text>
              ) : null}
            </div>
            <div style={{ marginTop: 6 }}>
              <Tag
                color={
                  isFailureStatus(statusInfo.status)
                    ? 'error'
                    : statusInfo.status === 'healthy' || statusInfo.status === 'running'
                      ? 'success'
                      : 'processing'
                }
              >
                {statusInfo.status}
              </Tag>
              {statusInfo.since && (
                <Typography.Text type="secondary">
                  {dayjs(statusInfo.since).locale(i18n.language).fromNow()} {t('statusFor')}
                </Typography.Text>
              )}
            </div>
          </div>
        </div>
        <Space wrap>
          <Button onClick={() => redeploy(false)}>
            {t('redeploy')}
          </Button>
          <Button danger onClick={confirmForceRedeploy}>
            {t('forceRedeploy')}
          </Button>
          <Button danger onClick={confirmStopApplication}>
            {t('stopApplication')}
          </Button>
          <Button type="primary" loading={saving} onClick={saveApplication}>
            {t('save')}
          </Button>
        </Space>
      </div>

      {app.pendingRedeploy && (
        <Alert
          type="warning"
          showIcon
          style={{ marginBottom: 16 }}
          message={t('pendingRedeployWarning')}
          action={
            <Button type="primary" size="small" onClick={() => redeploy(false)}>
              {t('redeploy')}
            </Button>
          }
        />
      )}

      <Tabs
        items={[
          { key: 'overview', label: t('overview'), children: overview },
          {
            key: 'deployments',
            label: t('deployments'),
            children: (
              <DeploymentsTab
                appId={id}
                branch={app.branch}
                deployments={deployments}
                onRefresh={load}
              />
            ),
          },
          {
            key: 'incidents',
            label: t('incidents'),
            children: (
              <div style={cardStyle}>
                <Typography.Paragraph type="secondary" style={{ marginTop: 0 }}>
                  {t('incidentsHint')}
                </Typography.Paragraph>
                <Table
                  rowKey="id"
                  size="small"
                  dataSource={incidents}
                  locale={{ emptyText: t('incidentsEmpty') }}
                  pagination={{ pageSize: 15, hideOnSinglePage: true }}
                  columns={[
                    {
                      title: t('incidentTime'),
                      dataIndex: 'createdAt',
                      width: 170,
                      render: (value: string) =>
                        value ? dayjs(value).locale(i18n.language).fromNow() : '—',
                    },
                    {
                      title: t('incidentKind'),
                      dataIndex: 'kind',
                      width: 120,
                      render: (kind: string) => {
                        const color =
                          kind === 'healed'
                            ? 'success'
                            : kind === 'failed' || kind === 'down'
                              ? 'error'
                              : 'warning';
                        return <Tag color={color}>{t(`incidentKind_${kind}` as never, kind)}</Tag>;
                      },
                    },
                    {
                      title: t('incidentRestarts'),
                      dataIndex: 'restartCount',
                      width: 110,
                    },
                    {
                      title: t('incidentDetail'),
                      dataIndex: 'detail',
                    },
                  ]}
                />
              </div>
            ),
          },
        ]}
      />

      <Modal
        open={envOpen}
        title={t('envVariables')}
        onCancel={() => setEnvOpen(false)}
        okText={t('save')}
        onOk={async () => {
          const env = Object.fromEntries(
            envText
              .split('\n')
              .filter(Boolean)
              .map((line) => {
                const i = line.indexOf('=');
                return [line.slice(0, i), line.slice(i + 1)];
              }),
          );
          await api.patch(`/applications/${id}`, { env });
          message.success(t('envSaved'));
          setEnvOpen(false);
          await load();
          if (await confirmRedeploy(t)) {
            await redeploy(false);
          }
        }}
      >
        <Typography.Paragraph type="secondary">{t('envVariablesHint')}</Typography.Paragraph>
        <Input.TextArea rows={12} value={envText} onChange={(e) => setEnvText(e.target.value)} />
        <Form form={form} style={{ display: 'none' }} />
      </Modal>
      <IconEditor
        open={iconOpen}
        value={app.icon}
        name={app.name}
        onCancel={() => setIconOpen(false)}
        onSave={saveIcon}
      />
    </div>
  );
}
