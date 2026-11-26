import {
  ApartmentOutlined,
  ArrowDownOutlined,
  ArrowUpOutlined,
  ClockCircleOutlined,
  DatabaseOutlined,
  SafetyOutlined,
} from '@ant-design/icons';
import { Tooltip } from 'antd';
import { useTranslation } from 'react-i18next';
import type { ServerVpnIndicator } from '../vpn/types';

export type ServerCardMetrics = {
  memoryLimitBytes?: number;
  diskUsedBytes?: number;
  diskTotalBytes?: number;
  cpuCores?: number;
  cpuThreads?: number;
  uptimeSeconds?: number;
  rxBytesPerSec?: number;
  txBytesPerSec?: number;
  rxHistory?: number[];
  txHistory?: number[];
};

const SPARK_BARS = 20;
// Below this the sparkline stays flat, so idle noise does not look like traffic.
const SPARK_FLOOR = 4 * 1024;

function sizeShort(bytes?: number) {
  if (!bytes || bytes <= 0) return '—';
  const units = ['B', 'K', 'M', 'G', 'T', 'P'];
  let size = bytes;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size >= 10 || unit === 0 ? Math.round(size) : size.toFixed(1)}${units[unit]}`;
}

function rateText(bytesPerSecond?: number) {
  if (bytesPerSecond === undefined) return '—';
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s'];
  let size = Math.max(0, bytesPerSecond);
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${unit === 0 || size >= 100 ? Math.round(size) : size.toFixed(1)} ${units[unit]}`;
}

function uptimeText(seconds?: number) {
  if (!seconds || seconds <= 0) return '—';
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

// Chip and memory-module glyphs drawn on a 14px grid to match the pixel panel.
function CpuGlyph() {
  return (
    <svg className="hc-card__glyph" viewBox="0 0 14 14" aria-hidden="true">
      <rect x="3" y="3" width="8" height="8" fill="none" stroke="currentColor" strokeWidth="1.5" />
      <rect x="5.5" y="5.5" width="3" height="3" fill="currentColor" />
      <path d="M5 0v3M9 0v3M5 11v3M9 11v3M0 5h3M0 9h3M11 5h3M11 9h3" stroke="currentColor" strokeWidth="1.2" />
    </svg>
  );
}

function RamGlyph() {
  return (
    <svg className="hc-card__glyph" viewBox="0 0 14 14" aria-hidden="true">
      <rect x="0.75" y="3.75" width="12.5" height="6" fill="none" stroke="currentColor" strokeWidth="1.5" />
      <path d="M3.5 6h1.5v1.5H3.5zM6.25 6h1.5v1.5h-1.5zM9 6h1.5v1.5H9z" fill="currentColor" />
      <path d="M2.5 10v2.5M5 10v2.5M7 10v2.5M9 10v2.5M11.5 10v2.5" stroke="currentColor" strokeWidth="1" />
    </svg>
  );
}

export function ServerSpecs({ metrics }: { metrics?: ServerCardMetrics }) {
  const { t } = useTranslation();
  const cores = metrics?.cpuCores;
  const threads = metrics?.cpuThreads;
  const diskTotal = metrics?.diskTotalBytes;
  const diskPercent =
    diskTotal && diskTotal > 0 ? Math.round(((metrics?.diskUsedBytes || 0) / diskTotal) * 100) : undefined;

  return (
    <div className="hc-card__specs rt-inset">
      <Tooltip title={threads ? t('cardCpuSpec', { cores, threads }) : undefined}>
        <span className="hc-card__spec" data-kind="cpu">
          <CpuGlyph />
          <span className="hc-card__spec-label">CPU</span>
          <span className="hc-card__spec-value">{threads ? `${cores}c/${threads}t` : '—'}</span>
        </span>
      </Tooltip>
      <Tooltip title={t('cardRamSpec')}>
        <span className="hc-card__spec" data-kind="mem">
          <RamGlyph />
          <span className="hc-card__spec-label">RAM</span>
          <span className="hc-card__spec-value">{sizeShort(metrics?.memoryLimitBytes)}</span>
        </span>
      </Tooltip>
      <Tooltip
        title={
          diskTotal
            ? t('cardDiskSpec', { used: sizeShort(metrics?.diskUsedBytes), total: sizeShort(diskTotal) })
            : undefined
        }
      >
        <span className="hc-card__spec" data-kind="disk">
          <DatabaseOutlined className="hc-card__glyph" />
          <span className="hc-card__spec-label">DISK</span>
          <span className="hc-card__spec-value">
            {sizeShort(diskTotal)}
            {diskPercent !== undefined && <span className="hc-card__spec-muted"> ({diskPercent}%)</span>}
          </span>
        </span>
      </Tooltip>
    </div>
  );
}

function Sparkline({ values }: { values?: number[] }) {
  const points = (values ?? []).slice(-SPARK_BARS);
  const padded = [...Array<number>(SPARK_BARS - points.length).fill(0), ...points];
  const peak = Math.max(SPARK_FLOOR, ...padded);
  return (
    <span className="hc-card__spark" aria-hidden="true">
      {padded.map((value, index) => (
        <i key={index} style={{ height: `${Math.max(8, Math.round((value / peak) * 100))}%` }} data-idle={value <= 0} />
      ))}
    </span>
  );
}

export function ServerTraffic({ metrics, online }: { metrics?: ServerCardMetrics; online: boolean }) {
  const { t } = useTranslation();
  const known = online && metrics?.rxBytesPerSec !== undefined;
  const boxes = [
    { key: 'rx', label: 'RX', hint: t('cardNetRx'), icon: <ArrowDownOutlined />, rate: metrics?.rxBytesPerSec, history: metrics?.rxHistory },
    { key: 'tx', label: 'TX', hint: t('cardNetTx'), icon: <ArrowUpOutlined />, rate: metrics?.txBytesPerSec, history: metrics?.txHistory },
  ];
  return (
    <div className="hc-card__traffic">
      {boxes.map((box) => (
        <Tooltip key={box.key} title={online && !known ? t('cardNoNetData') : box.hint}>
          <div className="hc-card__net rt-inset" data-dir={box.key} data-off={!known}>
            <span className="hc-card__net-label">
              {box.icon}
              {box.label}
            </span>
            <Sparkline values={known ? box.history : undefined} />
            <span className="hc-card__net-value">{known ? rateText(box.rate) : '—'}</span>
          </div>
        </Tooltip>
      ))}
    </div>
  );
}

export function ServerFooter({
  vpn,
  metrics,
  online,
}: {
  vpn?: ServerVpnIndicator;
  metrics?: ServerCardMetrics;
  online: boolean;
}) {
  const { t } = useTranslation();
  const vpnState = !vpn?.member ? 'none' : vpn.connected ? 'on' : 'off';
  return (
    <div className="hc-card__footer">
      {vpn && (
        <Tooltip
          title={
            vpnState === 'none'
              ? t('vpnNotMember')
              : vpnState === 'on'
                ? t('vpnConnectedAs', { address: vpn.address })
                : t('vpnDisconnected')
          }
        >
          <span className="hc-card__pill rt-pill" data-vpn={vpnState}>
            <SafetyOutlined />
            VPN
            <span className="hc-card__pill-dot" />
          </span>
        </Tooltip>
      )}
      <Tooltip title={t('cardHostUptime')}>
        <span className="hc-card__pill rt-pill">
          <ClockCircleOutlined />
          <span className="hc-card__pill-label">{t('uptime')}</span>
          <b>{online ? uptimeText(metrics?.uptimeSeconds) : '—'}</b>
        </span>
      </Tooltip>
      {vpn?.member && (
        <Tooltip title={t('cardPeersHint', { up: vpn.peersUp ?? 0, total: vpn.peersTotal ?? 0 })}>
          <span className="hc-card__pill rt-pill">
            <ApartmentOutlined />
            <span className="hc-card__pill-label">{t('cardPeers')}</span>
            <b>{vpn.peersTotal ?? 0}</b>
          </span>
        </Tooltip>
      )}
    </div>
  );
}
