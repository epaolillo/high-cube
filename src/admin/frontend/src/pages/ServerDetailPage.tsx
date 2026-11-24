import { Line } from '@ant-design/charts';
import { Badge, Button, Form, Input, Modal, Segmented, Tabs, Tag, Tooltip, Typography, message } from 'antd';
import { FitAddon } from '@xterm/addon-fit';
import { Terminal } from '@xterm/xterm';
import dayjs from 'dayjs';
import relativeTime from 'dayjs/plugin/relativeTime';
import 'dayjs/locale/es';
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useParams } from 'react-router-dom';
import api from '../api/client';
import { confirmRegenerateToken } from '../components/confirmRegenerateToken';
import { AppIcon, IconEditor } from '../components/IconEditor';
import { type AnalysisRequest, QuickCommandsPanel } from '../components/serverAudit/QuickCommandsPanel';
import { ContainerCpuChart } from '../components/serverMetrics/ContainerCpuChart';
import { connectTerminal, type TerminalMode, type TerminalTransport } from '../terminal/transport';
import { useAuth } from '../auth/AuthContext';
import { ServerVpnModal } from '../vpn/ServerVpnModal';
import '@xterm/xterm/css/xterm.css';

dayjs.extend(relativeTime);

const statusBadge: Record<string, 'success' | 'error' | 'warning' | 'default'> = {
  online: 'success',
  offline: 'error',
  pending: 'warning',
  removed: 'default',
};

type ServerSeries = {
  cpu?: number[];
  memory?: number[];
  gpu?: number[];
};

type ServerLatest = {
  cpuPercent?: number;
  memoryUsedBytes?: number;
  memoryLimitBytes?: number;
  diskUsedBytes?: number;
  diskTotalBytes?: number;
  load?: { '1'?: number; '5'?: number; '15'?: number };
  gpuPresent?: boolean;
  gpuPercent?: number;
  gpuMemoryUsedBytes?: number;
  gpuMemoryTotalBytes?: number;
  gpuCount?: number;
  gpuName?: string;
};

type ServerMetrics = {
  series?: ServerSeries;
  latest?: ServerLatest | null;
};

type Server = {
  name: string;
  ip: string;
  status: string;
  icon?: string;
  lastSeenAt?: string;
};

const cardStyle = {
  background: '#fff',
  border: '1px solid #E3E8EE',
  borderRadius: 8,
  padding: 24,
};

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

type RangeId = '15m' | '1h' | '6h' | '24h' | '7d';

const RANGES: { id: RangeId; minutes: number }[] = [
  { id: '15m', minutes: 15 },
  { id: '1h', minutes: 60 },
  { id: '6h', minutes: 360 },
  { id: '24h', minutes: 24 * 60 },
  { id: '7d', minutes: 7 * 24 * 60 },
];

type MetricPoint = {
  at: string;
  cpu: number | null;
  memory: number | null;
  disk: number | null;
  load1: number | null;
  load5: number | null;
  load15: number | null;
  gpu?: number | null;
  gpuMemory?: number | null;
};

function isoUtc(value: dayjs.Dayjs) {
  return value.toDate().toISOString().replace(/\.\d{3}Z$/, 'Z');
}

// Time axis label. The value is epoch milliseconds, as stored in chart rows.
function formatTick(epochMs: number, range: RangeId) {
  const value = dayjs(epochMs);
  return range === '15m' || range === '1h' ? value.format('HH:mm:ss') : value.format('DD/MM HH:mm');
}

// Full timestamp shown in the tooltip title.
function formatTooltipTime(epochMs: number) {
  return dayjs(epochMs).format('YYYY-MM-DD HH:mm:ss');
}

function summarize(values: number[]) {
  if (values.length === 0) return null;
  const min = Math.min(...values);
  const max = Math.max(...values);
  const avg = values.reduce((sum, value) => sum + value, 0) / values.length;
  return { min, avg, max };
}

// Tooltip and summary values always use two decimals.
function formatMeasure(value: number, unit: string) {
  return `${value.toFixed(2)}${unit}`;
}

// Axis labels drop trailing zeros but never show more than two decimals.
function formatAxisValue(value: number, unit: string) {
  return `${Number(value.toFixed(2))}${unit}`;
}

type ChartRow = Record<string, string | number>;

// One chart per metric. Rows carry `time` (epoch ms), `tip` (full timestamp),
// the value under `yField`, and `series` when a chart has several lines.
function MetricChart({
  title,
  note,
  data,
  yField,
  color,
  unit,
  seriesField,
  range,
  onSelectWindow,
}: {
  title: string;
  note?: string;
  data: ChartRow[];
  yField: string;
  color: string | string[];
  unit: string;
  seriesField?: string;
  range: RangeId;
  // Called with epoch milliseconds when the user drags across the plot.
  onSelectWindow?: (from: number, to: number) => void;
}) {
  const { t } = useTranslation();
  const singleColor = typeof color === 'string' ? color : undefined;
  const seriesColors = Array.isArray(color) ? color : undefined;
  const values = data.map((row) => Number(row[yField])).filter((value) => Number.isFinite(value));
  const summary = summarize(values);
  // The chart instance keeps the first listener; the ref always points at the latest callback.
  const selectRef = useRef(onSelectWindow);
  selectRef.current = onSelectWindow;
  return (
    <div style={cardStyle}>
      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 12, flexWrap: 'wrap' }}>
        <Typography.Text strong>{title}</Typography.Text>
        {summary ? (
          <Typography.Text type="secondary" style={{ fontSize: 12 }}>
            {t('metricSummary', {
              min: formatMeasure(summary.min, unit),
              avg: formatMeasure(summary.avg, unit),
              max: formatMeasure(summary.max, unit),
            })}
          </Typography.Text>
        ) : null}
      </div>
      {note ? (
        <Typography.Text type="secondary" style={{ display: 'block', marginTop: 4 }}>
          {note}
        </Typography.Text>
      ) : null}
      {onSelectWindow && data.length >= 2 ? (
        <Typography.Text type="secondary" style={{ display: 'block', marginTop: 4, fontSize: 12 }}>
          {t('metricSelectWindowHint')}
        </Typography.Text>
      ) : null}
      {data.length < 2 ? (
        <Typography.Text type="secondary">{t('noMetricsYet')}</Typography.Text>
      ) : (
        // G2 v5 options: scales, axes, slider and tooltip are configured per channel.
        <Line
          height={240}
          autoFit
          data={data}
          xField="time"
          yField={yField}
          seriesField={seriesField}
          style={singleColor ? { stroke: singleColor, lineWidth: 2 } : undefined}
          animate={false}
          scale={{
            x: { type: 'time' },
            y: { domainMin: 0, nice: true },
            ...(seriesColors ? { color: { range: seriesColors } } : {}),
          }}
          axis={{
            x: {
              tickCount: 6,
              labelAutoHide: true,
              labelFormatter: (value: number) => formatTick(Number(value), range),
            },
            y: {
              title: unit || false,
              labelFormatter: (value: number) => formatAxisValue(Number(value), unit),
            },
          }}
          slider={{ x: {} }}
          {...(onSelectWindow
            ? {
                interaction: {
                  brushXHighlight: { series: true, maskFill: '#635BFF', maskFillOpacity: 0.12 },
                },
                onReady: (chart: { on: (event: string, handler: (event: unknown) => void) => void }) => {
                  chart.on('brush:end', (event) => {
                    const selection = (event as { data?: { selection?: unknown[][] } }).data?.selection;
                    const domainX = selection?.[0];
                    if (!domainX || domainX.length < 2) return;
                    const from = dayjs(domainX[0] as Date | number).valueOf();
                    const to = dayjs(domainX[domainX.length - 1] as Date | number).valueOf();
                    if (!Number.isFinite(from) || !Number.isFinite(to) || to - from < 60_000) return;
                    selectRef.current?.(Math.min(from, to), Math.max(from, to));
                  });
                },
              }
            : {})}
          tooltip={{
            title: (datum: ChartRow) => formatTooltipTime(Number(datum.time)),
            items: [
              (datum: ChartRow) => ({
                name: seriesField ? String(datum[seriesField]) : title,
                value: formatMeasure(Number(datum[yField]), unit),
              }),
            ],
          }}
          legend={seriesField ? { color: { position: 'top' } } : false}
        />
      )}
    </div>
  );
}

function HostTerminal({
  serverId,
  active,
  onModeChange,
}: {
  serverId: string;
  active: boolean;
  onModeChange: (mode: TerminalMode | undefined) => void;
}) {
  const { t } = useTranslation();
  const containerRef = useRef<HTMLDivElement>(null);
  const activeRef = useRef(active);
  const refitRef = useRef<() => void>();

  useEffect(() => {
    activeRef.current = active;
    // A hidden tab has no size; fit again once the console is visible.
    if (active) refitRef.current?.();
  }, [active]);

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
      if (!activeRef.current) return;
      place();
      fit.fit();
    };
    refitRef.current = () => {
      fitTerminal();
      term.focus();
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
      refitRef.current = undefined;
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

export default function ServerDetailPage({ id: propId }: { id?: number } = {}) {
  const params = useParams();
  const id = propId != null ? String(propId) : params.id;
  const { t, i18n } = useTranslation();
  const [server, setServer] = useState<Server>();
  const [metrics, setMetrics] = useState<ServerMetrics>({});
  const [range, setRange] = useState<RangeId>('1h');
  const [points, setPoints] = useState<MetricPoint[]>([]);
  const [chartsLoading, setChartsLoading] = useState(false);
  const [rotating, setRotating] = useState(false);
  const [editing, setEditing] = useState(false);
  const [iconOpen, setIconOpen] = useState(false);
  const [vpnOpen, setVpnOpen] = useState(false);
  const { isAdmin } = useAuth();
  const [terminalMode, setTerminalMode] = useState<TerminalMode>();
  const [consoleTab, setConsoleTab] = useState<'console' | 'quickCommands'>('console');
  const [analysisRequest, setAnalysisRequest] = useState<AnalysisRequest>();
  const consoleCardRef = useRef<HTMLDivElement>(null);

  const requestAnalysis = (audit: 'cpu' | 'ram', from: number, to: number) => {
    setAnalysisRequest({ audit, from, to, nonce: Date.now() });
    setConsoleTab('quickCommands');
    consoleCardRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' });
  };
  const [editForm] = Form.useForm();

  useEffect(() => {
    if (!id) return undefined;
    let stopped = false;

    const load = async (silent: boolean) => {
      const { data } = await api.get(`/servers/${id}/overview`, { suppressErrorToast: silent });
      if (stopped) return;
      setServer(data.server);
      setMetrics(data.metrics || {});
    };

    load(false).catch(() => undefined);
    const timer = window.setInterval(() => {
      load(true).catch(() => undefined);
    }, 3000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [id]);

  useEffect(() => {
    if (!id) return undefined;
    const windowMinutes = RANGES.find((item) => item.id === range)?.minutes ?? 60;
    const to = dayjs();
    const from = to.subtract(windowMinutes, 'minute');
    let stopped = false;
    setChartsLoading(true);
    api
      .get(`/servers/${id}/metrics`, {
        params: { from: isoUtc(from), to: isoUtc(to) },
        suppressErrorToast: true,
      })
      .then(({ data }) => {
        if (stopped) return;
        setPoints(Array.isArray(data.points) ? data.points : []);
      })
      .catch(() => {
        if (!stopped) setPoints([]);
      })
      .finally(() => {
        if (!stopped) setChartsLoading(false);
      });
    return () => {
      stopped = true;
    };
  }, [id, range]);

  const latest = metrics.latest ?? undefined;
  const memoryTotal = latest?.memoryLimitBytes || 0;
  const memoryUsed = latest?.memoryUsedBytes || 0;
  const diskTotal = latest?.diskTotalBytes || 0;
  const diskUsed = latest?.diskUsedBytes || 0;
  const hasGpu = Boolean(latest?.gpuPresent);
  const gpuName = latest?.gpuName;
  const gpuCount = latest?.gpuCount || 0;
  const gpuLabel = gpuCount > 1 ? `${t('gpu')} (${gpuCount})` : t('gpu');
  const chartRows = (field: 'cpu' | 'memory' | 'disk' | 'gpu' | 'gpuMemory'): ChartRow[] =>
    points.flatMap((point) => {
      const value = point[field];
      if (value == null || Number.isNaN(value)) return [];
      return [{ time: dayjs(point.at).valueOf(), value }];
    });
  const loadRows: ChartRow[] = points.flatMap((point) => {
    const time = dayjs(point.at).valueOf();
    return (['load1', 'load5', 'load15'] as const).flatMap((key) => {
      const value = point[key];
      if (value == null || Number.isNaN(value)) return [];
      return [{ time, series: t(key), value }];
    });
  });

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

  const saveIcon = async (icon: string) => {
    const { data } = await api.patch(`/servers/${id}`, { icon });
    setServer((current) => (current ? { ...current, icon: data.icon } : current));
    setIconOpen(false);
    message.success(t('iconSaved'));
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
            <Tooltip title={t('iconEditTitle')}>
              <button
                type="button"
                aria-label={t('iconEditTitle')}
                onClick={() => setIconOpen(true)}
                style={{ border: 'none', background: 'transparent', padding: 0, cursor: 'pointer' }}
              >
                <AppIcon icon={server.icon} name={server.name} size={44} />
              </button>
            </Tooltip>
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
          {isAdmin && <Button onClick={() => setVpnOpen(true)}>{t('vpn')}</Button>}
        </div>
      </div>
      {isAdmin && (
        <ServerVpnModal
          serverId={Number(id)}
          serverName={server.name}
          open={vpnOpen}
          onClose={() => setVpnOpen(false)}
        />
      )}

      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 12, flexWrap: 'wrap', marginBottom: 8 }}>
        <Segmented
          value={range}
          onChange={(value) => setRange(value as RangeId)}
          options={RANGES.map((item) => ({ value: item.id, label: t(`metricWindow_${item.id}`) }))}
        />
      </div>
      <Typography.Paragraph type="secondary" style={{ marginTop: 0 }}>
        {t('metricZoomHint')}
      </Typography.Paragraph>
      <div style={{ display: 'grid', gap: 16, marginBottom: 16, opacity: chartsLoading ? 0.6 : 1 }}>
        <MetricChart
          title={t('cpu')}
          data={chartRows('cpu')}
          yField="value"
          range={range}
          color="#635BFF"
          unit="%"
          onSelectWindow={server.status === 'online' ? (from, to) => requestAnalysis('cpu', from, to) : undefined}
        />
        <ContainerCpuChart
          serverId={id}
          range={range}
          rangeMinutes={RANGES.find((item) => item.id === range)?.minutes ?? 60}
        />
        <MetricChart
          title={t('memory')}
          note={memoryTotal > 0 ? `${formatBytes(memoryUsed)} / ${formatBytes(memoryTotal)}` : undefined}
          data={chartRows('memory')}
          yField="value"
          range={range}
          color="#09825D"
          unit="%"
          onSelectWindow={server.status === 'online' ? (from, to) => requestAnalysis('ram', from, to) : undefined}
        />
        <MetricChart
          title={t('load')}
          data={loadRows}
          yField="value"
          range={range}
          color={['#635BFF', '#09825D', '#F5A623']}
          unit=""
          seriesField="series"
        />
        <MetricChart
          title={t('disk')}
          note={diskTotal > 0 ? `${formatBytes(diskUsed)} / ${formatBytes(diskTotal)}` : undefined}
          data={chartRows('disk')}
          yField="value"
          range={range}
          color="#0A2540"
          unit="%"
        />
        {hasGpu && (
          <MetricChart
            title={gpuLabel}
            note={gpuName}
            data={chartRows('gpu')}
            yField="value"
            range={range}
            color="#F5A623"
            unit="%"
          />
        )}
        {hasGpu && (
          <MetricChart
            title={t('gpuMemory')}
            note={
              (latest?.gpuMemoryTotalBytes || 0) > 0
                ? `${formatBytes(latest?.gpuMemoryUsedBytes || 0)} / ${formatBytes(latest?.gpuMemoryTotalBytes || 0)}`
                : undefined
            }
            data={chartRows('gpuMemory')}
            yField="value"
            range={range}
            color="#D97706"
            unit="%"
          />
        )}
      </div>

      <div ref={consoleCardRef} style={{ ...cardStyle, padding: 0, overflow: 'hidden' }}>
        <Tabs
          activeKey={consoleTab}
          onChange={(key) => setConsoleTab(key as 'console' | 'quickCommands')}
          tabBarStyle={{ margin: 0, padding: '0 24px' }}
          items={[
            { key: 'console', label: t('hostConsoleTab') },
            { key: 'quickCommands', label: t('quickCommandsTab') },
          ]}
          tabBarExtraContent={
            consoleTab === 'console' && terminalMode === 'polling' ? (
              <Tooltip title={t('terminalPollingHint')}>
                <Tag color="gold" style={{ marginInlineEnd: 0 }}>
                  {t('terminalPolling')}
                </Tag>
              </Tooltip>
            ) : null
          }
        />
        {server.status === 'online' ? (
          <>
            {/* Both panes stay mounted so an open shell and a running analysis survive tab switches. */}
            <div style={{ display: consoleTab === 'console' ? 'block' : 'none' }}>
              <HostTerminal key={id} serverId={id} active={consoleTab === 'console'} onModeChange={setTerminalMode} />
            </div>
            <div style={{ display: consoleTab === 'quickCommands' ? 'block' : 'none' }}>
              <QuickCommandsPanel key={id} serverId={id} request={analysisRequest} />
            </div>
          </>
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
      <IconEditor
        open={iconOpen}
        value={server.icon}
        name={server.name}
        onCancel={() => setIconOpen(false)}
        onSave={saveIcon}
      />
    </div>
  );
}
