import { Button, Input, Select, Space, Tag, Typography } from 'antd';
import { useTranslation } from 'react-i18next';

export type BaseDomainOption = { id: number; hostname: string };

export type HostnameRow = {
  subdomain: string;
  baseDomainId: number;
  tlsStatus?: string;
  hostname?: string;
};

type Props = {
  domains: BaseDomainOption[];
  value: HostnameRow[];
  onChange: (rows: HostnameRow[]) => void;
  showTls?: boolean;
  onRequestCertificate?: (row: HostnameRow) => void;
  certificateBusy?: boolean;
};

export function HostnameFields({
  domains,
  value,
  onChange,
  showTls,
  onRequestCertificate,
  certificateBusy,
}: Props) {
  const { t } = useTranslation();
  const showDomainSelect = domains.length > 1;

  const updateRow = (index: number, patch: Partial<HostnameRow>) => {
    onChange(value.map((row, i) => (i === index ? { ...row, ...patch } : row)));
  };

  const addRow = () => {
    onChange([
      ...value,
      {
        subdomain: '',
        baseDomainId: domains[0]?.id || 0,
      },
    ]);
  };

  const removeRow = (index: number) => {
    if (index === 0) return;
    onChange(value.filter((_, i) => i !== index));
  };

  if (domains.length === 0) {
    return (
      <Typography.Text type="danger">{t('noAssignedDomains')}</Typography.Text>
    );
  }

  return (
    <div>
      {value.map((row, index) => {
        const domain = domains.find((d) => d.id === row.baseDomainId) || domains[0];
        return (
          <div
            key={index}
            style={{
              marginBottom: 12,
              padding: 12,
              border: '1px solid #E3E8EE',
              borderRadius: 0,
              background: '#FAFBFC',
            }}
          >
            <Typography.Text type="secondary" style={{ display: 'block', marginBottom: 4 }}>
              {index === 0 ? t('subdomain') : t('additionalSubdomain')}
            </Typography.Text>
            <Space.Compact style={{ width: '100%' }}>
              <Input
                value={row.subdomain}
                onChange={(e) =>
                  updateRow(index, {
                    subdomain: e.target.value.toLowerCase().replace(/[^a-z0-9-]/g, ''),
                  })
                }
                placeholder={t('subdomainPlaceholder')}
              />
              {showDomainSelect ? (
                <Select
                  style={{ width: '45%', minWidth: 140 }}
                  value={row.baseDomainId || domains[0].id}
                  options={domains.map((d) => ({
                    value: d.id,
                    label: `.${d.hostname}`,
                  }))}
                  onChange={(baseDomainId) => updateRow(index, { baseDomainId })}
                />
              ) : (
                <Input
                  style={{ width: '45%' }}
                  value={`.${domain?.hostname || ''}`}
                  disabled
                />
              )}
            </Space.Compact>
            <div
              style={{
                marginTop: 8,
                display: 'flex',
                justifyContent: 'space-between',
                alignItems: 'center',
                gap: 8,
              }}
            >
              <div>
                {showTls && row.tlsStatus ? (
                  <Tag color={row.tlsStatus === 'active' ? 'success' : 'warning'}>
                    {t('tlsStatus')}: {row.tlsStatus}
                  </Tag>
                ) : null}
              </div>
              <Space>
                {showTls && row.hostname && onRequestCertificate ? (
                  <Button
                    size="small"
                    disabled={
                      certificateBusy ||
                      `${row.subdomain}.${domain?.hostname || ''}` !== row.hostname
                    }
                    onClick={() => onRequestCertificate(row)}
                  >
                    {t('retryCertificate')}
                  </Button>
                ) : null}
                {index > 0 && (
                  <Button danger type="link" onClick={() => removeRow(index)}>
                    {t('remove')}
                  </Button>
                )}
              </Space>
            </div>
          </div>
        );
      })}
      <Button type="dashed" onClick={addRow} block>
        {t('addHostname')}
      </Button>
    </div>
  );
}
