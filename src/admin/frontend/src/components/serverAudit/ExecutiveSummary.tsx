import { CopyOutlined } from '@ant-design/icons';
import { Alert, Button, Progress, Space, Typography } from 'antd';
import { useTranslation } from 'react-i18next';
import { ActionButton, actionKey, severityIcon } from './CheckCard';
import { formatBytes, messageText } from './format';
import type { AuditJob, SuggestedAction } from './types';

function scoreColor(score: number) {
  if (score >= 80) return '#09825D';
  if (score >= 50) return '#D97706';
  return '#E5484D';
}

export function ExecutiveSummary({
  job,
  applied,
  onAction,
  onExport,
}: {
  job: AuditJob;
  applied: Set<string>;
  onAction: (action: SuggestedAction) => void;
  onExport: () => void;
}) {
  const { t } = useTranslation();
  const summary = job.summary;
  if (!summary) return null;
  const pendingActions = summary.actions.filter((action) => !applied.has(actionKey(action)));

  return (
    <div style={{ border: '1px solid #C9AC88', borderRadius: 8, padding: 20, background: 'var(--rt-plate)' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 16, flexWrap: 'wrap', alignItems: 'center' }}>
        <Space size={20} align="center">
          <Progress
            type="circle"
            size={76}
            percent={summary.score}
            strokeColor={scoreColor(summary.score)}
            format={(value) => <span style={{ fontSize: 20, fontWeight: 600 }}>{value}</span>}
          />
          <div>
            <Typography.Title level={5} style={{ margin: 0 }}>
              {t('audit.executiveSummary')}
            </Typography.Title>
            <Typography.Text type="secondary">{t(`audit.scoreLabel.${summary.score >= 80 ? 'good' : summary.score >= 50 ? 'fair' : 'poor'}`)}</Typography.Text>
            {job.audit === 'disk' && summary.reclaimableBytes > 0 && (
              <Typography.Text style={{ display: 'block' }}>
                {t('audit.reclaimable', { bytes: formatBytes(summary.reclaimableBytes) })}
              </Typography.Text>
            )}
          </div>
        </Space>
        <Button icon={<CopyOutlined />} onClick={onExport}>
          {t('audit.exportMarkdown')}
        </Button>
      </div>

      {summary.verdict && (
        <Alert
          style={{ marginTop: 16 }}
          type={summary.verdict.code === 'res_verdict_calm' ? 'success' : 'warning'}
          showIcon
          message={t('audit.verdictTitle')}
          description={messageText(t, summary.verdict)}
        />
      )}
      {summary.coverage != null && summary.coverage < 0.8 && (
        <Alert
          style={{ marginTop: 12 }}
          type="info"
          showIcon
          message={t('audit.partialCoverage', { pct: Math.round(summary.coverage * 100) })}
        />
      )}

      <Typography.Text strong style={{ display: 'block', marginTop: 16 }}>
        {t('audit.keyFindings')}
      </Typography.Text>
      {summary.keyFindings.length === 0 ? (
        <Typography.Paragraph type="secondary" style={{ marginTop: 6 }}>
          {t('audit.noIssues')}
        </Typography.Paragraph>
      ) : (
        <div style={{ display: 'grid', gap: 6, marginTop: 6 }}>
          {summary.keyFindings.map((finding) => (
            <Space key={finding.checkId} align="start">
              {severityIcon(finding.severity)}
              <Typography.Text>
                <Typography.Text strong>{t(`audit.check.${finding.checkId}`)}: </Typography.Text>
                {messageText(t, finding.conclusion)}
              </Typography.Text>
            </Space>
          ))}
        </div>
      )}

      <Typography.Text strong style={{ display: 'block', marginTop: 16 }}>
        {t('audit.recommendedActions')}
      </Typography.Text>
      {summary.actions.length === 0 ? (
        <Typography.Paragraph type="secondary" style={{ marginTop: 6 }}>
          {t('audit.noActions')}
        </Typography.Paragraph>
      ) : (
        <div style={{ display: 'grid', gap: 6, marginTop: 6 }}>
          {[...pendingActions, ...summary.actions.filter((action) => applied.has(actionKey(action)))].map((action) => (
            <ActionButton key={actionKey(action)} action={action} applied={applied.has(actionKey(action))} onAction={onAction} />
          ))}
        </div>
      )}
    </div>
  );
}
