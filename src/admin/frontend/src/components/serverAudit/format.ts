import dayjs from 'dayjs';
import type { TFunction } from 'i18next';
import type { AuditMessage, ColumnType } from './types';

export function formatBytes(value: number) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
  let size = Math.abs(value);
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  const text = `${size.toFixed(size >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
  return value < 0 ? `-${text}` : text;
}

export function formatPercent(value: number) {
  return `${value.toFixed(1)}%`;
}

// Timestamps arrive as epoch milliseconds or ISO strings; both render in local time.
export function formatDate(value: unknown) {
  if (value == null || value === '' || value === 0) return '-';
  const parsed = typeof value === 'number' ? dayjs(value) : dayjs(String(value));
  return parsed.isValid() ? parsed.format('YYYY-MM-DD HH:mm:ss') : String(value);
}

export function formatValue(value: unknown, type: ColumnType): string {
  if (value == null) return '-';
  if (typeof value === 'string' && type !== 'date') return value;
  const number = Number(value);
  switch (type) {
    case 'bytes':
      return Number.isFinite(number) ? formatBytes(number) : String(value);
    case 'percent':
      return Number.isFinite(number) ? formatPercent(number) : String(value);
    case 'number':
      if (!Number.isFinite(number)) return String(value);
      return Number.isInteger(number) ? number.toLocaleString() : number.toFixed(2);
    case 'date':
      return formatDate(value);
    case 'duration':
      return Number.isFinite(number) ? `${(number / 1000).toFixed(1)} s` : String(value);
    default:
      return String(value);
  }
}

const PERCENT_KEYS = new Set(['pct', 'share', 'rootPct', 'peak', 'avg', 'baseline', 'cpu', 'io', 'psi']);

// Conclusion params come raw from the backend; the unit is implied by the key name.
export function formatParams(t: TFunction, params: Record<string, unknown> = {}) {
  const out: Record<string, string | number> = {};
  for (const [key, value] of Object.entries(params)) {
    if (typeof value === 'number') {
      if (/bytes$/i.test(key)) out[key] = formatBytes(value);
      else if (PERCENT_KEYS.has(key) || /Pct$/.test(key)) out[key] = formatPercent(value);
      else if (/At$/.test(key)) out[key] = formatDate(value);
      else if (key === 'corr') out[key] = value.toFixed(2);
      else if (key === 'days' || key === 'aboveMinutes' || key === 'load') out[key] = value.toFixed(1);
      else out[key] = Number.isInteger(value) ? value : value.toFixed(1);
    } else if (typeof value === 'string') {
      if (/At$/.test(key)) out[key] = formatDate(value);
      else if (key === 'confidence') out[key] = t(`audit.confidence.${value}`);
      else if (key === 'source') out[key] = t(`audit.source.${value}`);
      else out[key] = value;
    } else if (value != null) {
      out[key] = String(value);
    }
  }
  return out;
}

export function messageText(t: TFunction, message?: AuditMessage) {
  if (!message?.code) return '';
  return t(`audit.c.${message.code}`, { ...formatParams(t, message.params), defaultValue: message.code });
}

export function actionTargetLabel(target?: Record<string, unknown> | null) {
  if (!target) return '';
  const name = target.name ?? target.id;
  if (target.pid != null) return `${String(target.name ?? '')} (PID ${String(target.pid)})`;
  return name == null ? '' : String(name);
}
