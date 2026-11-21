import { PlayCircleOutlined } from '@ant-design/icons';
import { Alert, Button, DatePicker, Progress, Select, Space, Spin, Typography, message } from 'antd';
import dayjs, { type Dayjs } from 'dayjs';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { ActionConfirmModal } from './ActionConfirmModal';
import { CheckCard, actionKey } from './CheckCard';
import { ExecutiveSummary } from './ExecutiveSummary';
import { errorText, runAudit } from './api';
import { actionTargetLabel, formatBytes, formatDate, messageText } from './format';
import type { AuditJob, AuditKind, SuggestedAction } from './types';

const AUDITS: AuditKind[] = ['disk', 'cpu', 'ram'];

function isFinished(status: string) {
  return status === 'done' || status === 'skipped' || status === 'error';
}

export function QuickCommandsPanel({ serverId }: { serverId: string }) {
  const { t } = useTranslation();
  const [audit, setAudit] = useState<AuditKind>('disk');
  const [range, setRange] = useState<[Dayjs, Dayjs] | null>(() => [dayjs().subtract(1, 'hour'), dayjs()]);
  const [job, setJob] = useState<AuditJob>();
  const [running, setRunning] = useState(false);
  const [error, setError] = useState<string>();
  const [applied, setApplied] = useState<Set<string>>(new Set());
  const [rerunning, setRerunning] = useState<Set<string>>(new Set());
  const [pendingAction, setPendingAction] = useState<SuggestedAction | null>(null);
  const auditAbort = useRef<AbortController>();
  const rerunAborts = useRef<AbortController[]>([]);

  useEffect(
    () => () => {
      auditAbort.current?.abort();
      rerunAborts.current.forEach((controller) => controller.abort());
    },
    [],
  );

  const needsWindow = audit !== 'disk';
  const windowValid = !needsWindow || (range != null && range[1].isAfter(range[0]));

  const execute = async () => {
    auditAbort.current?.abort();
    rerunAborts.current.forEach((controller) => controller.abort());
    rerunAborts.current = [];
    const controller = new AbortController();
    auditAbort.current = controller;
    setJob(undefined);
    setError(undefined);
    setApplied(new Set());
    setRerunning(new Set());
    setRunning(true);
    try {
      await runAudit(
        serverId,
        needsWindow && range ? { audit, from: range[0].valueOf(), to: range[1].valueOf() } : { audit },
        (update) => {
          if (!controller.signal.aborted) setJob(update);
        },
        controller.signal,
      );
    } catch (err) {
      if (!controller.signal.aborted) setError(errorText(err, t('audit.failed')));
    } finally {
      if (!controller.signal.aborted) setRunning(false);
    }
  };

  const rerun = useCallback(
    async (checkId: string) => {
      if (!job) return;
      const controller = new AbortController();
      rerunAborts.current.push(controller);
      setRerunning((current) => new Set(current).add(checkId));
      try {
        await runAudit(
          serverId,
          { audit: job.audit, only: checkId, from: job.window?.from, to: job.window?.to },
          (update) => {
            const fresh = update.checks.find((check) => check.id === checkId);
            if (!fresh || controller.signal.aborted) return;
            setJob((current) =>
              current
                ? { ...current, checks: current.checks.map((check) => (check.id === checkId ? fresh : check)) }
                : current,
            );
          },
          controller.signal,
        );
      } catch (err) {
        if (!controller.signal.aborted) message.error(errorText(err, t('audit.failed')));
      } finally {
        setRerunning((current) => {
          const next = new Set(current);
          next.delete(checkId);
          return next;
        });
      }
    },
    [job, serverId, t],
  );

  const onApplied = (action: SuggestedAction) => {
    setApplied((current) => new Set(current).add(actionKey(action)));
    const owner =
      action.checkId ??
      job?.checks.find((check) => check.actions?.some((item) => actionKey(item) === actionKey(action)))?.id;
    // Verdict actions act on live processes; refreshing the live state shows the effect.
    const target = owner === 'verdict' ? 'live_state' : owner;
    if (target) {
      window.setTimeout(() => {
        rerun(target).catch(() => undefined);
      }, 1500);
    }
  };

  const progress = useMemo(() => {
    if (!job) return { done: 0, total: 0, current: undefined as string | undefined };
    const done = job.checks.filter((check) => isFinished(check.status)).length;
    const current = job.checks.find((check) => check.status === 'running')?.id;
    return { done, total: job.plan.length, current };
  }, [job]);

  const exportMarkdown = async () => {
    if (!job) return;
    const lines: string[] = [];
    lines.push(`# ${t(`audit.kind.${job.audit}`)}`);
    lines.push('');
    lines.push(`- ${t('audit.generatedAt')}: ${formatDate(Date.now())}`);
    if (job.window) lines.push(`- ${t('audit.window')}: ${formatDate(job.window.from)} → ${formatDate(job.window.to)}`);
    if (job.summary) lines.push(`- ${t('audit.score')}: ${job.summary.score}/100`);
    if (job.summary?.verdict) {
      lines.push('', `## ${t('audit.verdictTitle')}`, '', messageText(t, job.summary.verdict));
    }
    if (job.summary?.keyFindings.length) {
      lines.push('', `## ${t('audit.keyFindings')}`, '');
      job.summary.keyFindings.forEach((finding) =>
        lines.push(`- **${t(`audit.check.${finding.checkId}`)}**: ${messageText(t, finding.conclusion)}`),
      );
    }
    lines.push('', `## ${t('audit.checksTitle')}`);
    job.checks.forEach((check, index) => {
      lines.push('', `### ${index + 1}. ${t(`audit.check.${check.id}`)} (${t(`audit.severity.${check.severity ?? 'info'}`)})`);
      if (check.conclusion) lines.push('', messageText(t, check.conclusion));
      check.findings?.forEach((finding) => lines.push(`- ${messageText(t, finding)}`));
    });
    if (job.summary?.actions.length) {
      lines.push('', `## ${t('audit.recommendedActions')}`, '');
      job.summary.actions.forEach((action) => {
        const target = actionTargetLabel(action.target);
        const estimate = (action.estimateBytes ?? 0) > 0 ? ` (~${formatBytes(action.estimateBytes ?? 0)})` : '';
        const done = applied.has(actionKey(action)) ? ` [${t('audit.applied')}]` : '';
        lines.push(`- ${t(`audit.action.${action.id}`)}${target ? ` \`${target}\`` : ''}${estimate}${done}`);
      });
    }
    try {
      await navigator.clipboard.writeText(lines.join('\n'));
      message.success(t('audit.copied'));
    } catch {
      message.error(t('audit.copyFailed'));
    }
  };

  return (
    <div style={{ padding: 24 }}>
      <Space wrap size={12} align="start">
        <Select
          style={{ width: 260 }}
          value={audit}
          disabled={running}
          onChange={(value) => {
            setAudit(value);
            setJob(undefined);
            setError(undefined);
          }}
          options={AUDITS.map((value) => ({ value, label: t(`audit.kind.${value}`) }))}
        />
        {needsWindow && (
          <DatePicker.RangePicker
            showTime={{ format: 'HH:mm' }}
            format="YYYY-MM-DD HH:mm"
            value={range}
            disabled={running}
            allowClear={false}
            disabledDate={(current) => current && current.isAfter(dayjs().endOf('day'))}
            presets={[
              { label: t('audit.preset.15m'), value: [dayjs().subtract(15, 'minute'), dayjs()] },
              { label: t('audit.preset.1h'), value: [dayjs().subtract(1, 'hour'), dayjs()] },
              { label: t('audit.preset.6h'), value: [dayjs().subtract(6, 'hour'), dayjs()] },
              { label: t('audit.preset.24h'), value: [dayjs().subtract(24, 'hour'), dayjs()] },
            ]}
            onChange={(value) => {
              if (value && value[0] && value[1]) setRange([value[0], value[1]]);
            }}
          />
        )}
        <Button type="primary" icon={<PlayCircleOutlined />} loading={running} disabled={!windowValid} onClick={execute}>
          {t('audit.execute')}
        </Button>
      </Space>
      <Typography.Paragraph type="secondary" style={{ marginTop: 8, marginBottom: 0 }}>
        {t(`audit.kindHint.${audit}`)}
        {needsWindow ? ` ${t('audit.localTimeHint')}` : ''}
      </Typography.Paragraph>

      {error && <Alert style={{ marginTop: 16 }} type="error" showIcon message={error} />}

      {running && (
        <div
          style={{
            marginTop: 16,
            padding: '12px 16px',
            border: '1px solid #E3E8EE',
            borderRadius: 8,
            background: '#F6F9FC',
          }}
        >
          <Space align="center">
            <Spin size="small" />
            <Typography.Text strong>{t('audit.processing')}</Typography.Text>
            {progress.current && (
              <Typography.Text type="secondary">{t(`audit.check.${progress.current}`, { defaultValue: progress.current })}</Typography.Text>
            )}
          </Space>
          {progress.total > 0 && (
            <Progress
              percent={Math.round((progress.done / progress.total) * 100)}
              format={() => `${progress.done}/${progress.total}`}
              size="small"
              style={{ marginTop: 8, marginBottom: 0 }}
            />
          )}
        </div>
      )}

      {job && (
        <div style={{ display: 'grid', gap: 12, marginTop: 16 }}>
          {job.status === 'done' && (
            <ExecutiveSummary job={job} applied={applied} onAction={setPendingAction} onExport={exportMarkdown} />
          )}
          {job.checks.map((check, index) => (
            <CheckCard
              key={check.id}
              check={check}
              index={index}
              applied={applied}
              rerunning={rerunning.has(check.id)}
              onAction={setPendingAction}
              onRerun={job.status === 'done' && check.id !== 'verdict' ? rerun : undefined}
            />
          ))}
        </div>
      )}

      <ActionConfirmModal
        serverId={serverId}
        action={pendingAction}
        onClose={() => setPendingAction(null)}
        onApplied={onApplied}
      />
    </div>
  );
}
