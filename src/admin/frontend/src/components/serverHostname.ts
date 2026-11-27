import { Modal, Typography, message } from 'antd';
import type { TFunction } from 'i18next';
import { createElement } from 'react';
import api from '../api/client';
import { setupCommand } from '../vpn/ServerVpnModal';

// Errors that only go away by reinstalling the root helper on the server.
const HELPER_ERRORS = ['hc-vpn-helper', 'invalid interface name', 'unknown action'];

function showHelperSetup(t: TFunction, error: string) {
  const command = setupCommand();
  Modal.warning({
    title: t('hostnameHelperTitle'),
    width: 640,
    content: createElement(
      'div',
      null,
      createElement(Typography.Paragraph, null, t('hostnameHelperBody')),
      createElement(
        'div',
        { className: 'hc-vpn-command' },
        createElement(Typography.Text, { copyable: { text: command }, style: { color: 'inherit' } }, command),
      ),
      createElement(Typography.Paragraph, { type: 'secondary', style: { marginTop: 12, marginBottom: 0 } }, error),
    ),
  });
}

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
      if (data.status === 'error' && HELPER_ERRORS.some((needle) => data.error.includes(needle))) {
        message.destroy(key);
        showHelperSetup(t, data.error);
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
