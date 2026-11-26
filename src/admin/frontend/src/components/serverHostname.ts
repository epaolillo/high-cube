import { message } from 'antd';
import type { TFunction } from 'i18next';
import api from '../api/client';

export type HostnameChange = {
  status: 'sent' | 'offline' | 'invalid';
  hostname: string;
  requestId?: string;
};

type HostnamePoll = {
  status: 'pending' | 'ok' | 'error' | 'timeout' | 'unknown';
  hostname: string;
  error: string;
};

const POLL_INTERVAL_MS = 1500;
const POLL_LIMIT = 40;

// Same rule as the admin: "Sigma Server_2" -> "sigma-server-2".
export function toHostname(name: string) {
  const ascii = name.toLowerCase().replace(/[áéíóúüñ]/g, (char) => 'aeiouun'['áéíóúüñ'.indexOf(char)]);
  let out = '';
  for (const char of ascii) {
    if (/[a-z0-9]/.test(char)) out += char;
    else if (out && !out.endsWith('-')) out += '-';
  }
  return out.slice(0, 63).replace(/-+$/, '');
}

// Reports the host rename started by PATCH /servers/:id until the agent answers.
export function followHostnameChange(t: TFunction, serverId: number | string, change?: HostnameChange) {
  if (!change) return;
  if (change.status === 'invalid') {
    message.warning(t('hostnameInvalid'));
    return;
  }
  if (change.status === 'offline' || !change.requestId) {
    message.warning(t('hostnameOffline', { hostname: change.hostname }));
    return;
  }
  const key = `hostname-${change.requestId}`;
  message.loading({ key, content: t('hostnameChanging', { hostname: change.hostname }), duration: 0 });
  let attempts = 0;
  const poll = async () => {
    attempts += 1;
    try {
      const { data } = await api.get<HostnamePoll>(`/servers/${serverId}/hostname-change/${change.requestId}`, {
        suppressErrorToast: true,
      });
      if (data.status === 'ok') {
        message.success({ key, content: t('hostnameChanged', { hostname: data.hostname }) });
        return;
      }
      if (data.status === 'error' || data.status === 'timeout') {
        message.error({ key, content: t('hostnameChangeFailed', { error: data.error }), duration: 10 });
        return;
      }
      if (data.status === 'unknown') {
        message.destroy(key);
        return;
      }
    } catch {
      // Transient network error: keep polling until the limit.
    }
    if (attempts < POLL_LIMIT) window.setTimeout(poll, POLL_INTERVAL_MS);
    else message.destroy(key);
  };
  window.setTimeout(poll, POLL_INTERVAL_MS);
}
