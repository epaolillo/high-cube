import { Alert, Button, Input, Modal, Result, Space, Spin, Statistic, Tag, Typography } from 'antd';
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { errorText, runAction } from './api';
import { actionTargetLabel, formatBytes } from './format';
import type { ActionJob, SuggestedAction } from './types';

export const riskColor: Record<string, string> = { low: 'green', medium: 'gold', high: 'red' };

export function ActionConfirmModal({
  serverId,
  action,
  onClose,
  onApplied,
}: {
  serverId: string;
  action: SuggestedAction | null;
  onClose: () => void;
  onApplied: (action: SuggestedAction, job: ActionJob) => void;
}) {
  const { t } = useTranslation();
  const [typed, setTyped] = useState('');
  const [running, setRunning] = useState(false);
  const [job, setJob] = useState<ActionJob>();
  const [error, setError] = useState<string>();
  const abortRef = useRef<AbortController>();

  useEffect(() => {
    setTyped('');
    setRunning(false);
    setJob(undefined);
    setError(undefined);
    return () => abortRef.current?.abort();
  }, [action]);

  if (!action) return null;

  const targetLabel = actionTargetLabel(action.target);
  const confirmWord =
    action.id === 'terminate_process' ? String(action.target?.name ?? '') : action.risk === 'high' ? t('audit.confirmWord') : '';
  const canConfirm = !confirmWord || typed.trim() === confirmWord;

  const execute = async () => {
    const controller = new AbortController();
    abortRef.current = controller;
    setRunning(true);
    setError(undefined);
    try {
      const result = await runAction(serverId, action, controller.signal);
      setJob(result);
      if (result.status === 'ok') onApplied(action, result);
      else setError(result.error || t('audit.actionFailed'));
    } catch (err) {
      if (!controller.signal.aborted) setError(errorText(err, t('audit.actionFailed')));
    } finally {
      setRunning(false);
    }
  };

  const result = job?.result;
  const finished = Boolean(job) || Boolean(error);

  return (
    <Modal
      open
      title={t(`audit.action.${action.id}`, { defaultValue: action.id })}
      onCancel={running ? undefined : onClose}
      closable={!running}
      maskClosable={false}
      width={560}
      footer={
        finished ? (
          <Button type="primary" onClick={onClose}>
            {t('audit.close')}
          </Button>
        ) : (
          <Space>
            <Button onClick={onClose} disabled={running}>
              {t('cancel')}
            </Button>
            <Button
              type="primary"
              danger={action.risk !== 'low'}
              loading={running}
              disabled={!canConfirm}
              onClick={execute}
            >
              {t('audit.applyConfirm')}
            </Button>
          </Space>
        )
      }
    >
      {running ? (
        <div style={{ textAlign: 'center', padding: 24 }}>
          <Spin />
          <Typography.Paragraph style={{ marginTop: 12 }}>{t('audit.processing')}</Typography.Paragraph>
        </div>
      ) : finished ? (
        error ? (
          <Result status="error" title={t('audit.actionFailed')} subTitle={error} />
        ) : (
          <div>
            <Result status="success" title={t('audit.actionDone')} style={{ paddingBottom: 8 }} />
            {result?.metric && result.metric !== 'none' && (
              <Space size={32} style={{ width: '100%', justifyContent: 'center' }}>
                <Statistic
                  title={t(result.metric === 'diskAvailable' ? 'audit.diskBefore' : 'audit.memoryBefore')}
                  value={formatBytes(result.before ?? 0)}
                />
                <Statistic
                  title={t(result.metric === 'diskAvailable' ? 'audit.diskAfter' : 'audit.memoryAfter')}
                  value={formatBytes(result.after ?? 0)}
                />
                <Statistic title={t('audit.freed')} value={formatBytes(result.freedBytes ?? 0)} valueStyle={{ color: '#09825D' }} />
              </Space>
            )}
            {result?.output ? (
              <pre
                style={{
                  marginTop: 16,
                  maxHeight: 200,
                  overflow: 'auto',
                  background: '#0A2540',
                  color: '#E6EDF5',
                  padding: 12,
                  borderRadius: 6,
                  fontSize: 12,
                  whiteSpace: 'pre-wrap',
                }}
              >
                {result.output}
              </pre>
            ) : null}
          </div>
        )
      ) : (
        <div>
          <Typography.Paragraph>{t(`audit.actionHint.${action.id}`, { defaultValue: '' })}</Typography.Paragraph>
          <Space direction="vertical" style={{ width: '100%' }}>
            {targetLabel && (
              <div>
                <Typography.Text type="secondary">{t('audit.target')}: </Typography.Text>
                <Typography.Text code>{targetLabel}</Typography.Text>
              </div>
            )}
            {(action.estimateBytes ?? 0) > 0 && (
              <div>
                <Typography.Text type="secondary">{t('audit.estimate')}: </Typography.Text>
                <Typography.Text strong>{formatBytes(action.estimateBytes ?? 0)}</Typography.Text>
              </div>
            )}
            <div>
              <Typography.Text type="secondary">{t('audit.riskLabel')}: </Typography.Text>
              <Tag color={riskColor[action.risk]}>{t(`audit.risk.${action.risk}`)}</Tag>
            </div>
            {action.risk !== 'low' && (
              <Alert type={action.risk === 'high' ? 'error' : 'warning'} showIcon message={t(`audit.riskHint.${action.risk}`)} />
            )}
            {confirmWord && (
              <div>
                <Typography.Paragraph style={{ marginBottom: 6 }}>
                  {t('audit.typeToConfirm', { word: confirmWord })}
                </Typography.Paragraph>
                <Input value={typed} onChange={(event) => setTyped(event.target.value)} placeholder={confirmWord} />
              </div>
            )}
          </Space>
        </div>
      )}
    </Modal>
  );
}
