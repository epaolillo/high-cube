import { Badge, Button, Form, Input, Modal, Tag, Tooltip, Typography, message } from 'antd';
import { FitAddon } from '@xterm/addon-fit';
import { Terminal } from '@xterm/xterm';
import dayjs from 'dayjs';
import relativeTime from 'dayjs/plugin/relativeTime';
import 'dayjs/locale/es';
import { useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useParams } from 'react-router-dom';
import api from '../api/client';
import { confirmRegenerateToken } from '../components/confirmRegenerateToken';
import { connectTerminal, type TerminalMode, type TerminalTransport } from '../terminal/transport';
import '@xterm/xterm/css/xterm.css';

dayjs.extend(relativeTime);

const statusBadge: Record<string, 'success' | 'error' | 'warning' | 'default'> = {
  online: 'success',
  offline: 'error',
  pending: 'warning',
  removed: 'default',
};

type CpuCounters = { user?: number; nice?: number; system?: number; idle?: number };

type GpuInfo = {
  name?: string;
  utilizationPercent?: number;
  memoryUsedMb?: number;
  memoryTotalMb?: number;
};

type MetricPayload = {
  load?: { '1'?: number; '5'?: number; '15'?: number };
  memory?: { totalKb?: number; availableKb?: number };
  cpu?: CpuCounters;
  disk?: { totalBytes?: number; availableBytes?: number };
  gpu?: { present?: boolean; gpus?: GpuInfo[] };
};

// Averages GPU utilization and sums memory across all GPUs in a sample.
function aggregateGpu(gpus: GpuInfo[]) {
  const count = gpus.length;
  const utilization = count > 0 ? gpus.reduce((sum, g) => sum + (g.utilizationPercent || 0), 0) / count : 0;
  const memoryUsedMb = gpus.reduce((sum, g) => sum + (g.memoryUsedMb || 0), 0);
  const memoryTotalMb = gpus.reduce((sum, g) => sum + (g.memoryTotalMb || 0), 0);
  return { utilization, memoryUsedMb, memoryTotalMb };
}

type MetricRow = {
  id: number;
  collectedAt: string;
  payload: MetricPayload;
};

type Server = {
  name: string;
  ip: string;
  status: string;
  lastSeenAt?: string;
};

const cardStyle = {
  background: '#fff',
  border: '1px solid #E3E8EE',
  borderRadius: 8,
  padding: 24,
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

function percent(used: number, total: number) {
  if (total <= 0) return 0;
  return Math.max(0, Math.min(100, (used / total) * 100));
}

function cpuPercent(previous: CpuCounters, current: CpuCounters) {
  const busy = (sample: CpuCounters) => (sample.user || 0) + (sample.nice || 0) + (sample.system || 0);
  const total = (sample: CpuCounters) => busy(sample) + (sample.idle || 0);
  const deltaTotal = total(current) - total(previous);
  const deltaBusy = busy(current) - busy(previous);
  if (deltaTotal <= 0) return 0;
  return Math.max(0, Math.min(100, (deltaBusy / deltaTotal) * 100));
}

function Sparkline({ points, color }: { points: number[]; color: string }) {
  if (points.length < 2) {
    return <div style={{ height: 72 }} />;
  }
  const width = 320;
  const height = 72;
  const step = width / (points.length - 1);
  const path = points
    .map((value, index) => {
      const x = index * step;
      const y = height - 4 - (Math.max(0, Math.min(100, value)) / 100) * (height - 8);
      return `${index === 0 ? 'M' : 'L'}${x.toFixed(1)},${y.toFixed(1)}`;
    })
    .join(' ');
  return (
    <svg viewBox={`0 0 ${width} ${height}`} width="100%" height="72" aria-hidden="true">
      <path d={path} fill="none" stroke={color} strokeWidth="2" />
    </svg>
  );
}

function HostTerminal({
  serverId,
  onModeChange,
}: {
  serverId: string;
  onModeChange: (mode: TerminalMode | undefined) => void;
}) {
  const { t } = useTranslation();
  const containerRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return undefined;

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
    connectTerminal(
      serverId,
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
  }, [serverId, t, onModeChange]);

  return (
    <div
      ref={containerRef}
      style={{ width: '100%', minHeight: 280, overflow: 'hidden', boxSizing: 'border-box' }}
    />
  );
}

export default function ServerDetailPage() {
  const { id } = useParams();
  const { t, i18n } = useTranslation();
  const [server, setServer] = useState<Server>();
  const [rows, setRows] = useState<MetricRow[]>([]);
  const [rotating, setRotating] = useState(false);
  const [editing, setEditing] = useState(false);
  const [terminalMode, setTerminalMode] = useState<TerminalMode>();
  const [editForm] = Form.useForm();

  useEffect(() => {
    if (!id) return undefined;
    let stopped = false;

    const load = async () => {
      const to = new Date();
      const from = new Date(to.getTime() - 5 * 60 * 1000);
      const [serverRes, metricsRes] = await Promise.all([
        api.get(`/servers/${id}`),
        api.get(`/servers/${id}/metrics`, {
          params: { from: utcStamp(from), to: utcStamp(to) },
        }),
      ]);
      if (stopped) return;
      setServer(serverRes.data);
      setRows(metricsRes.data.rows || []);
    };

    load().catch(() => undefined);
    const timer = window.setInterval(() => {
      load().catch(() => undefined);
    }, 2000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [id]);

  const series = useMemo(() => {
    const cpu: number[] = [];
    const memory: number[] = [];
    const gpu: number[] = [];
    rows.forEach((row, index) => {
      const previous = index > 0 ? rows[index - 1].payload.cpu : undefined;
      if (previous && row.payload.cpu) {
        cpu.push(cpuPercent(previous, row.payload.cpu));
      }
      const totalKb = row.payload.memory?.totalKb || 0;
      const availableKb = row.payload.memory?.availableKb || 0;
      if (totalKb > 0) {
        memory.push(percent(totalKb - availableKb, totalKb));
      }
      const gpus = row.payload.gpu?.present ? row.payload.gpu.gpus ?? [] : [];
      if (gpus.length > 0) {
        gpu.push(aggregateGpu(gpus).utilization);
      }
    });
    return { cpu, memory, gpu };
  }, [rows]);

  const latest = rows.length > 0 ? rows[rows.length - 1] : undefined;
  const latestCpu = series.cpu.length > 0 ? series.cpu[series.cpu.length - 1] : undefined;
  const memoryTotal = latest?.payload.memory?.totalKb || 0;
  const memoryAvailable = latest?.payload.memory?.availableKb || 0;
  const memoryUsed = Math.max(0, memoryTotal - memoryAvailable);
  const diskTotal = latest?.payload.disk?.totalBytes || 0;
  const diskAvailable = latest?.payload.disk?.availableBytes || 0;
  const diskUsed = Math.max(0, diskTotal - diskAvailable);
  const latestGpus = latest?.payload.gpu?.present ? latest.payload.gpu.gpus ?? [] : [];
  const hasGpu = latestGpus.length > 0;
  const gpuAggregate = aggregateGpu(latestGpus);
  const gpuName = latestGpus.length === 1 ? latestGpus[0].name : undefined;
  const gpuLabel = latestGpus.length > 1 ? `${t('gpu')} (${latestGpus.length})` : t('gpu');

  const regenerateToken = async () => {
    setRotating(true);
    try {
      const { data } = await api.post(`/servers/${id}/token`);
      message.success(t('agentTokenRotated'));
      Modal.info({
        title: t('agentTokenTitle'),
        width: 560,
        content: (
          <div>
            <Typography.Paragraph type="secondary">{t('agentTokenHint')}</Typography.Paragraph>
            <Typography.Paragraph copyable code style={{ wordBreak: 'break-all' }}>
              {data.agentToken}
            </Typography.Paragraph>
          </div>
        ),
      });
    } finally {
      setRotating(false);
    }
  };

  if (!server || !id) return null;

  return (
    <div>
      <div
        style={{
          display: 'flex',
          justifyContent: 'space-between',
          alignItems: 'flex-start',
          marginBottom: 24,
        }}
      >
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 10 }}>
            <Typography.Title level={3} style={{ color: '#0A2540', margin: 0 }}>
              {server.name}
            </Typography.Title>
            <span
              style={{
                display: 'inline-flex',
                alignItems: 'center',
                padding: '2px 10px',
                borderRadius: 999,
                background: server.status === 'online' ? 'rgba(9, 130, 93, 0.1)' : '#F6F9FC',
                border: '1px solid #E3E8EE',
              }}
            >
              <Badge
                status={statusBadge[server.status] || 'default'}
                text={t(`serverStatus.${server.status}`, { defaultValue: server.status })}
              />
            </span>
          </div>
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: 24, color: '#425466' }}>
            <div>
              <Typography.Text type="secondary" style={{ fontSize: 12, display: 'block' }}>
                {t('ipAddress')}
              </Typography.Text>
              <Typography.Text code copyable style={{ fontSize: 13 }}>
                {server.ip}
              </Typography.Text>
            </div>
            <div>
              <Typography.Text type="secondary" style={{ fontSize: 12, display: 'block' }}>
                {t('lastSeen')}
              </Typography.Text>
              {server.lastSeenAt ? (
                <Tooltip title={dayjs(server.lastSeenAt).format('YYYY-MM-DD HH:mm:ss')}>
                  <Typography.Text>
                    {dayjs(server.lastSeenAt).locale(i18n.language).fromNow()}
                  </Typography.Text>
                </Tooltip>
              ) : (
                <Typography.Text>{t('never')}</Typography.Text>
              )}
            </div>
          </div>
        </div>
        <div style={{ display: 'flex', gap: 8 }}>
          <Button
            onClick={() => {
              editForm.setFieldsValue({ name: server.name, ip: server.ip });
              setEditing(true);
            }}
          >
            {t('edit')}
          </Button>
          <Button onClick={() => confirmRegenerateToken(t, server.name, regenerateToken)} loading={rotating}>
            {t('regenerateToken')}
          </Button>
        </div>
      </div>

      {rows.length === 0 ? (
        <div style={{ ...cardStyle, marginBottom: 16 }}>
          <Typography.Text type="secondary">{t('noMetricsYet')}</Typography.Text>
        </div>
      ) : (
        <div
          style={{
            display: 'grid',
            gridTemplateColumns: 'repeat(auto-fit, minmax(220px, 1fr))',
            gap: 16,
            marginBottom: 16,
          }}
        >
          <div style={cardStyle}>
            <Typography.Text type="secondary">{t('cpu')}</Typography.Text>
            <Typography.Title level={3} style={{ margin: '8px 0' }}>
              {latestCpu === undefined ? '—' : `${latestCpu.toFixed(1)}%`}
            </Typography.Title>
            <Sparkline points={series.cpu} color="#635BFF" />
          </div>
          <div style={cardStyle}>
            <Typography.Text type="secondary">{t('memory')}</Typography.Text>
            <Typography.Title level={3} style={{ margin: '8px 0' }}>
              {memoryTotal > 0 ? `${percent(memoryUsed, memoryTotal).toFixed(1)}%` : '—'}
            </Typography.Title>
            <Typography.Text style={{ color: '#425466' }}>
              {memoryTotal > 0
                ? `${formatBytes(memoryUsed * 1024)} / ${formatBytes(memoryTotal * 1024)}`
                : '—'}
            </Typography.Text>
            <Sparkline points={series.memory} color="#09825D" />
          </div>
          <div style={cardStyle}>
            <Typography.Text type="secondary">{t('load')}</Typography.Text>
            <Typography.Title level={3} style={{ margin: '8px 0' }}>
              {(latest?.payload.load?.['1'] ?? 0).toFixed(2)}
            </Typography.Title>
            <Typography.Text style={{ color: '#425466' }}>
              {(latest?.payload.load?.['1'] ?? 0).toFixed(2)} · {(latest?.payload.load?.['5'] ?? 0).toFixed(2)} ·{' '}
              {(latest?.payload.load?.['15'] ?? 0).toFixed(2)}
            </Typography.Text>
          </div>
          <div style={cardStyle}>
            <Typography.Text type="secondary">{t('disk')}</Typography.Text>
            <Typography.Title level={3} style={{ margin: '8px 0' }}>
              {diskTotal > 0 ? `${percent(diskUsed, diskTotal).toFixed(1)}%` : '—'}
            </Typography.Title>
            <Typography.Text style={{ color: '#425466' }}>
              {diskTotal > 0 ? `${formatBytes(diskUsed)} / ${formatBytes(diskTotal)}` : '—'}
            </Typography.Text>
          </div>
          {hasGpu && (
            <div style={cardStyle}>
              <Tooltip title={gpuName}>
                <Typography.Text type="secondary">{gpuLabel}</Typography.Text>
              </Tooltip>
              <Typography.Title level={3} style={{ margin: '8px 0' }}>
                {`${gpuAggregate.utilization.toFixed(1)}%`}
              </Typography.Title>
              <Typography.Text style={{ color: '#425466' }}>
                {gpuAggregate.memoryTotalMb > 0
                  ? `${formatBytes(gpuAggregate.memoryUsedMb * 1024 * 1024)} / ${formatBytes(
                      gpuAggregate.memoryTotalMb * 1024 * 1024,
                    )}`
                  : '—'}
              </Typography.Text>
              <Sparkline points={series.gpu} color="#F5A623" />
            </div>
          )}
        </div>
      )}

      <div style={{ ...cardStyle, padding: 0, overflow: 'hidden' }}>
        <div
          style={{
            padding: '16px 24px',
            borderBottom: '1px solid #E3E8EE',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            gap: 12,
          }}
        >
          <Typography.Text strong>{t('hostTerminal')}</Typography.Text>
          {terminalMode === 'polling' && (
            <Tooltip title={t('terminalPollingHint')}>
              <Tag color="gold" style={{ marginInlineEnd: 0 }}>
                {t('terminalPolling')}
              </Tag>
            </Tooltip>
          )}
        </div>
        {server.status === 'online' ? (
          <HostTerminal key={id} serverId={id} onModeChange={setTerminalMode} />
        ) : (
          <div style={{ padding: 24 }}>
            <Typography.Text type="secondary">{t('waitingForAgent')}</Typography.Text>
          </div>
        )}
      </div>
      <Modal
        title={t('edit')}
        open={editing}
        onCancel={() => setEditing(false)}
        okText={t('save')}
        cancelText={t('cancel')}
        okButtonProps={{ type: 'primary' }}
        onOk={async () => {
          const values = await editForm.validateFields();
          const { data } = await api.patch(`/servers/${id}`, values);
          setServer((current) => (current ? { ...current, name: data.name, ip: data.ip } : current));
          message.success(t('saved'));
          setEditing(false);
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
