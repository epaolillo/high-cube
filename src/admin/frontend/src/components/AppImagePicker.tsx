import { Input, Typography } from 'antd';
import { useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import catalog from '../data/appImages.json';

export type AppImageEntry = {
  id: string;
  name: string;
  group: 'database' | 'service';
  image: string;
  logo: string;
  type: 'web' | 'private';
  port: number;
  description: string;
  category: string;
  volumes: { name: string; mountPath: string }[];
  healthCheckType: 'none' | 'cmd';
  healthCheckCommand: string;
  env: Record<string, string>;
};

const images = catalog as AppImageEntry[];

export function AppImagePicker({
  selectedId,
  onSelect,
}: {
  selectedId?: string;
  onSelect: (entry: AppImageEntry) => void;
}) {
  const { t } = useTranslation();
  const [query, setQuery] = useState('');
  const needle = query.trim().toLowerCase();
  const filtered = useMemo(
    () =>
      images.filter((entry) => {
        if (!needle) return true;
        return (
          entry.name.toLowerCase().includes(needle) ||
          entry.image.toLowerCase().includes(needle) ||
          entry.category.toLowerCase().includes(needle)
        );
      }),
    [needle],
  );
  const databases = filtered.filter((entry) => entry.group === 'database');
  const services = filtered.filter((entry) => entry.group === 'service');

  return (
    <div>
      <Input
        allowClear
        placeholder={t('appImageSearch')}
        value={query}
        onChange={(event) => setQuery(event.target.value)}
        style={{ marginBottom: 12 }}
      />
      <div style={{ maxHeight: 420, overflow: 'auto', paddingRight: 4 }}>
        {databases.length > 0 && <Section title={t('appImageDatabases')} entries={databases} selectedId={selectedId} onSelect={onSelect} />}
        {services.length > 0 && <Section title={t('appImageServices')} entries={services} selectedId={selectedId} onSelect={onSelect} />}
        {filtered.length === 0 && (
          <Typography.Text type="secondary">{t('appImageEmpty')}</Typography.Text>
        )}
      </div>
    </div>
  );
}

function Section({
  title,
  entries,
  selectedId,
  onSelect,
}: {
  title: string;
  entries: AppImageEntry[];
  selectedId?: string;
  onSelect: (entry: AppImageEntry) => void;
}) {
  return (
    <div style={{ marginBottom: 16 }}>
      <Typography.Text strong style={{ display: 'block', marginBottom: 8 }}>
        {title}
      </Typography.Text>
      <div
        style={{
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fill, minmax(168px, 1fr))',
          gap: 8,
        }}
      >
        {entries.map((entry) => {
          const selected = entry.id === selectedId;
          return (
            <button
              key={entry.id}
              type="button"
              onClick={() => onSelect(entry)}
              style={{
                textAlign: 'left',
                border: selected ? '1px solid #635bff' : '1px solid #E3E8EE',
                background: selected ? '#f6f5ff' : '#fff',
                borderRadius: 0,
                padding: 10,
                cursor: 'pointer',
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                {entry.logo ? (
                  <img
                    src={`/app-images/${entry.logo}`}
                    alt=""
                    width={28}
                    height={28}
                    style={{ objectFit: 'contain', flex: '0 0 28px' }}
                  />
                ) : (
                  <span
                    style={{
                      width: 28,
                      height: 28,
                      borderRadius: 0,
                      background: '#E3E8EE',
                      display: 'inline-flex',
                      alignItems: 'center',
                      justifyContent: 'center',
                      fontSize: 12,
                      flex: '0 0 28px',
                    }}
                  >
                    {entry.name.slice(0, 1)}
                  </span>
                )}
                <span style={{ fontWeight: 600, color: '#0A2540', lineHeight: 1.2 }}>{entry.name}</span>
              </div>
              <div style={{ marginTop: 6, color: '#425466', fontSize: 12, wordBreak: 'break-all' }}>
                {entry.image}
              </div>
            </button>
          );
        })}
      </div>
    </div>
  );
}
