import axios from 'axios';
import api from '../../api/client';
import type { ActionJob, AuditJob, AuditKind, SuggestedAction } from './types';

const POLL_MS = 1500;

export function errorText(error: unknown, fallback: string) {
  if (axios.isAxiosError(error)) {
    const data = error.response?.data as { error?: string } | undefined;
    if (data?.error) return data.error;
  }
  if (error instanceof Error && error.message) return error.message;
  return fallback;
}

function wait(ms: number, signal: AbortSignal) {
  return new Promise<void>((resolve, reject) => {
    const timer = window.setTimeout(resolve, ms);
    signal.addEventListener(
      'abort',
      () => {
        window.clearTimeout(timer);
        reject(new DOMException('Aborted', 'AbortError'));
      },
      { once: true },
    );
  });
}

export type AuditRequest = {
  audit: AuditKind;
  from?: number;
  to?: number;
  only?: string;
};

// Starts an audit and calls onUpdate with every poll until the job finishes.
export async function runAudit(
  serverId: string,
  request: AuditRequest,
  onUpdate: (job: AuditJob) => void,
  signal: AbortSignal,
): Promise<AuditJob> {
  const started = await api.post(`/servers/${serverId}/quick-commands`, request, {
    suppressErrorToast: true,
    signal,
  });
  const jobId = started.data.jobId as string;
  for (;;) {
    const { data } = await api.get<AuditJob>(`/servers/${serverId}/quick-commands/${jobId}`, {
      suppressErrorToast: true,
      signal,
    });
    onUpdate(data);
    if (data.status === 'done') return data;
    await wait(POLL_MS, signal);
  }
}

export async function runAction(serverId: string, action: SuggestedAction, signal: AbortSignal): Promise<ActionJob> {
  const started = await api.post(
    `/servers/${serverId}/quick-actions`,
    { actionId: action.id, target: action.target ?? null },
    { suppressErrorToast: true, signal },
  );
  const jobId = started.data.jobId as string;
  for (;;) {
    const { data } = await api.get<ActionJob>(`/servers/${serverId}/quick-commands/${jobId}`, {
      suppressErrorToast: true,
      signal,
    });
    if (data.status !== 'running') return data;
    await wait(POLL_MS, signal);
  }
}
