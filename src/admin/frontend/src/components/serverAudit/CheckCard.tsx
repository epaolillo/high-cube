import {
  CheckCircleFilled,
  ClockCircleOutlined,
  CloseCircleFilled,
  ExclamationCircleFilled,
  InfoCircleFilled,
  LoadingOutlined,
  MinusCircleOutlined,
  ReloadOutlined,
  WarningFilled,
} from '@ant-design/icons';
import { Button, Collapse, Space, Tag, Tooltip, Typography } from 'antd';
import type { ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import { riskColor } from './ActionConfirmModal';
import { BlockView } from './BlockView';
import { actionTargetLabel, formatBytes, messageText } from './format';
import type { AuditCheck, Severity, SuggestedAction } from './types';

export const severityColor: Record<Severity, string> = {
  ok: '#09825D',
  info: '#635BFF',
  warning: '#D97706',
  critical: '#E5484D',
};

export function severityIcon(severity: Severity | undefined): ReactNode {
  switch (severity) {
    case 'ok':
      return <CheckCircleFilled style={{ color: severityColor.ok }} />;
    case 'warning':
      return <WarningFilled style={{ color: severityColor.warning }} />;
    case 'critical':
      return <ExclamationCircleFilled style={{ color: severityColor.critical }} />;
    default:
      return <InfoCircleFilled style={{ color: severityColor.info }} />;
  }
}

function statusIcon(check: AuditCheck) {
  if (check.status === 'pending') return <ClockCircleOutlined style={{ color: '#8898AA' }} />;
  if (check.status === 'running') return <LoadingOutlined style={{ color: '#635BFF' }} />;
  if (check.status === 'skipped') return <MinusCircleOutlined style={{ color: '#8898AA' }} />;
  if (check.status === 'error') return <CloseCircleFilled style={{ color: '#8898AA' }} />;
  return severityIcon(check.severity);
}

export function ActionButton({
  action,
  applied,
  onAction,
}: {
  action: SuggestedAction;
  applied: boolean;
  onAction: (action: SuggestedAction) => void;
}) {
  const { t } = useTranslation();
  const target = actionTargetLabel(action.target);
  return (
    <div
      style={{
        display: 'flex',
        justifyContent: 'space-between',
        alignItems: 'center',
        gap: 12,
        padding: '8px 12px',
        border: '1px solid #E3E8EE',
        borderRadius: 6,
        background: applied ? '#F6F9FC' : '#fff',
      }}
    >
      <div style={{ minWidth: 0 }}>
        <Space size={6} wrap>
          <Typography.Text strong>{t(`audit.action.${action.id}`, { defaultValue: action.id })}</Typography.Text>
          {target && <Typography.Text code>{target}</Typography.Text>}
          <Tag color={riskColor[action.risk]} style={{ marginInlineEnd: 0 }}>
            {t(`audit.risk.${action.risk}`)}
          </Tag>
        </Space>
        {(action.estimateBytes ?? 0) > 0 && (
          <Typography.Text type="secondary" style={{ display: 'block', fontSize: 12 }}>
            {t('audit.estimateShort', { bytes: formatBytes(action.estimateBytes ?? 0) })}
          </Typography.Text>
        )}
      </div>
      <Button size="small" type={applied ? 'default' : 'primary'} disabled={applied} onClick={() => onAction(action)}>
        {applied ? t('audit.applied') : t('audit.apply')}
      </Button>
    </div>
  );
}

export function actionKey(action: SuggestedAction) {
  return `${action.id}:${JSON.stringify(action.target ?? null)}`;
}

export function CheckCard({
  check,
  index,
  applied,
  rerunning,
  onAction,
  onRerun,
}: {
  check: AuditCheck;
  index: number;
  applied: Set<string>;
  rerunning: boolean;
  onAction: (action: SuggestedAction) => void;
  onRerun?: (checkId: string) => void;
}) {
  const { t } = useTranslation();
  const finished = check.status === 'done' || check.status === 'skipped' || check.status === 'error';
  const blocks = check.blocks ?? [];
  const findings = check.findings ?? [];
  const actions = check.actions ?? [];
  const accent =
    check.status === 'done' && check.severity ? severityColor[check.severity] : check.status === 'running' ? '#635BFF' : '#E3E8EE';

  return (
    <div
      style={{
        border: '1px solid #E3E8EE',
        borderLeft: `3px solid ${accent}`,
        borderRadius: 8,
        padding: '12px 16px',
        background: check.status === 'pending' ? '#FAFBFC' : '#fff',
        opacity: check.status === 'pending' ? 0.7 : 1,
      }}
    >
      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 12, alignItems: 'flex-start' }}>
        <Space align="start" size={10}>
          <span style={{ fontSize: 16, lineHeight: '22px' }}>{statusIcon(check)}</span>
          <div>
            <Typography.Text strong>
              {index + 1}. {t(`audit.check.${check.id}`, { defaultValue: check.id })}
            </Typography.Text>
            <Typography.Text type="secondary" style={{ display: 'block', fontSize: 12 }}>
              {check.status === 'running' ? t('audit.running') : t(`audit.checkHint.${check.id}`, { defaultValue: '' })}
            </Typography.Text>
          </div>
        </Space>
        <Space size={6}>
          {check.status === 'done' && check.severity && (
            <Tag color={severityColor[check.severity]} style={{ marginInlineEnd: 0 }}>
              {t(`audit.severity.${check.severity}`)}
            </Tag>
          )}
          {check.status === 'skipped' && <Tag style={{ marginInlineEnd: 0 }}>{t('audit.skipped')}</Tag>}
          {check.status === 'error' && <Tag style={{ marginInlineEnd: 0 }}>{t('audit.error')}</Tag>}
          {finished && check.durationMs != null && (
            <Typography.Text type="secondary" style={{ fontSize: 12 }}>
              {(check.durationMs / 1000).toFixed(1)} s
            </Typography.Text>
          )}
          {finished && onRerun && (
            <Tooltip title={t('audit.rerun')}>
              <Button size="small" type="text" icon={<ReloadOutlined />} loading={rerunning} onClick={() => onRerun(check.id)} />
            </Tooltip>
          )}
        </Space>
      </div>

      {finished && check.conclusion && (
        <Typography.Paragraph style={{ margin: '10px 0 0 26px' }}>{messageText(t, check.conclusion)}</Typography.Paragraph>
      )}
      {finished && findings.length > 0 && (
        <ul style={{ margin: '4px 0 0 26px', paddingInlineStart: 18, color: '#425466' }}>
          {findings.map((finding, i) => (
            <li key={`${finding.code}-${i}`}>{messageText(t, finding)}</li>
          ))}
        </ul>
      )}
      {finished && actions.length > 0 && (
        <div style={{ margin: '10px 0 0 26px', display: 'grid', gap: 6 }}>
          {actions.map((action) => (
            <ActionButton key={actionKey(action)} action={action} applied={applied.has(actionKey(action))} onAction={onAction} />
          ))}
        </div>
      )}
      {finished && blocks.length > 0 && (
        <Collapse
          ghost
          size="small"
          style={{ marginLeft: 14, marginTop: 4 }}
          defaultActiveKey={
            blocks.some((block) => block.kind === 'line' || block.title === 'orphanVolumes') ? ['details'] : []
          }
          items={[
            {
              key: 'details',
              label: t('audit.details'),
              children: blocks.map((block, i) => <BlockView key={`${block.title}-${i}`} block={block} />),
            },
          ]}
        />
      )}
    </div>
  );
}
