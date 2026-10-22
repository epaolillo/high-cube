import { Select, Tag, Typography, message } from 'antd';
import { useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { BaseDomainOption, HostnameRow } from './HostnameFields';

type ServerOption = { id: number; name: string; ip?: string };

type Props = {
  domains: BaseDomainOption[];
  hostnames: HostnameRow[];
  servers: ServerOption[];
  isAdmin: boolean;
  onUpdated: (domain: BaseDomainOption) => void;
  // Fired after a server is chosen, so the app can fill a free host port.
  onSelected?: (accessServerId: number) => void;
};

// One global access point per base domain used by this application.
export function DomainAccessPoints({ domains, hostnames, servers, isAdmin, onUpdated, onSelected }: Props) {
  const { t } = useTranslation();
  const used = useMemo(() => {
    const ids = [...new Set(hostnames.map((row) => row.baseDomainId).filter((id) => id > 0))];
    return ids
      .map((id) => domains.find((domain) => domain.id === id))
      .filter((domain): domain is BaseDomainOption => Boolean(domain));
  }, [domains, hostnames]);

  if (used.length === 0) {
    return null;
  }

  const save = async (domain: BaseDomainOption, accessServerId: number | null) => {
    const { data } = await api.patch(`/settings/base-domains/${domain.id}`, { accessServerId });
    onUpdated({
      ...domain,
      accessServerId: data.accessServerId ?? null,
      accessServerName: data.accessServerName ?? null,
      accessServerIp: data.accessServerIp ?? null,
    });
    message.success(t('accessPointSaved'));
    if (accessServerId) {
      onSelected?.(accessServerId);
    }
  };

  return (
    <div style={{ marginTop: 16 }}>
      <Typography.Text strong>{t('accessPoint')}</Typography.Text>
      <Typography.Paragraph type="secondary" style={{ fontSize: 12, margin: '4px 0 12px' }}>
        {t('accessPointHint')}
      </Typography.Paragraph>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        {used.map((domain) => (
          <div
            key={domain.id}
            style={{
              display: 'flex',
              gap: 12,
              alignItems: 'center',
              flexWrap: 'wrap',
              padding: '8px 12px',
              border: '1px solid #E3E8EE',
              borderRadius: 8,
              background: '#FAFBFC',
            }}
          >
            <Typography.Text style={{ minWidth: 140 }}>{domain.hostname}</Typography.Text>
            {domain.accessServerName ? (
              <Tag color="blue">
                {t('accessPoint')}: {domain.accessServerName}
              </Tag>
            ) : (
              <Tag>{t('accessPointNone')}</Tag>
            )}
            {isAdmin ? (
              <Select
                allowClear
                placeholder={t('accessPointLabel')}
                style={{ minWidth: 240, flex: 1 }}
                value={domain.accessServerId ?? undefined}
                options={servers.map((server) => ({
                  value: server.id,
                  label: server.ip ? `${server.name} (${server.ip})` : server.name,
                }))}
                onChange={(value) => {
                  void save(domain, typeof value === 'number' ? value : null);
                }}
              />
            ) : null}
          </div>
        ))}
      </div>
    </div>
  );
}
