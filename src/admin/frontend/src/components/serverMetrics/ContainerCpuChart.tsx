import { Column } from '@ant-design/charts';
import { Typography } from 'antd';
import dayjs from 'dayjs';
import { useEffect, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../../api/client';
import { ContainerProcessesModal } from './ContainerProcessesModal';

type RangeId = '15m' | '1h' | '6h' | '24h' | '7d';

type GroupMeta = {
  key: string;
  kind: string;
  label: string;
  applicationId?: number;
  replicaId?: number;
};

type Bucket = {
  from: string;
  to: string;
  total: number;
  values: Record<string, number>;
};

type Props = {
  serverId: string;
  range: RangeId;
  rangeMinutes: number;
};

const cardStyle = {
  background: '#fff',
  border: '1px solid #E3E8EE',
  borderRadius: 8,
  padding: 24,
};

const COLORS = [
  '#635BFF',
  '#09825D',
  '#F5A623',
  '#0A2540',
  '#D97706',
  '#7C3AED',
  '#0891B2',
  '#DC2626',
  '#4B5563',
  '#059669',
];

function isoUtc(value: dayjs.Dayjs) {
  return value.toDate().toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function formatTick(epochMs: number, range: RangeId) {
  const value = dayjs(epochMs);
  return range === '15m' || range === '1h' ? value.format('HH:mm:ss') : value.format('DD/MM HH:mm');
}

function displayLabel(group: GroupMeta, t: (key: string) => string) {
  if (group.kind === 'host') return t('containerCpuHost');
  if (group.kind === 'docker') return t('containerCpuOtherDocker');
  return group.label || group.key;
}

export function ContainerCpuChart({ serverId, range, rangeMinutes }: Props) {
  const { t } = useTranslation();
  const [groups, setGroups] = useState<GroupMeta[]>([]);
  const [buckets, setBuckets] = useState<Bucket[]>([]);
  const [loading, setLoading] = useState(false);
  const [modal, setModal] = useState<{ from: string; to: string; groupKey?: string }>();
  // Owned here, not by the chart legend: the parent re-renders every few seconds,
  // which makes G2 rebuild the chart and forget its own legend filter.
  const [hiddenKeys, setHiddenKeys] = useState<Set<string>>(() => new Set());

  useEffect(() => {
    const to = dayjs();
    const from = to.subtract(rangeMinutes, 'minute');
    let stopped = false;
    setLoading(true);
    api
      .get(`/servers/${serverId}/cpu-attribution`, {
        params: { from: isoUtc(from), to: isoUtc(to) },
        suppressErrorToast: true,
      })
      .then(({ data }) => {
        if (stopped) return;
        setGroups(Array.isArray(data.groups) ? data.groups : []);
        setBuckets(Array.isArray(data.buckets) ? data.buckets : []);
      })
      .catch(() => {
        if (!stopped) {
          setGroups([]);
          setBuckets([]);
        }
      })
      .finally(() => {
        if (!stopped) setLoading(false);
      });
    return () => {
      stopped = true;
    };
  }, [serverId, range, rangeMinutes]);

  const labelByKey = useMemo(() => {
    const map = new Map<string, string>();
    for (const group of groups) {
      map.set(group.key, displayLabel(group, t));
    }
    return map;
  }, [groups, t]);

  const chartData = useMemo(() => {
    const rows: { time: number; from: string; to: string; group: string; groupKey: string; value: number }[] = [];
    for (const bucket of buckets) {
      const time = dayjs(bucket.from).valueOf();
      for (const group of groups) {
        const value = bucket.values?.[group.key] ?? 0;
        if (value <= 0) continue;
        rows.push({
          time,
          from: bucket.from,
          to: bucket.to,
          group: labelByKey.get(group.key) || group.key,
          groupKey: group.key,
          value: Number(value.toFixed(2)),
        });
      }
    }
    return rows;
  }, [buckets, groups, labelByKey]);

  // Groups with data, each with a fixed color so hiding one never recolors the rest.
  const legendItems = useMemo(() => {
    const present = new Set(chartData.map((row) => row.groupKey));
    return groups
      .filter((group) => present.has(group.key))
      .map((group, index) => ({
        key: group.key,
        label: labelByKey.get(group.key) || group.key,
        color: COLORS[index % COLORS.length],
      }));
  }, [chartData, groups, labelByKey]);

  const visibleData = useMemo(
    () => chartData.filter((row) => !hiddenKeys.has(row.groupKey)),
    [chartData, hiddenKeys],
  );

  const toggleGroup = (key: string) =>
    setHiddenKeys((previous) => {
      const next = new Set(previous);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });

  return (
    <div style={{ ...cardStyle, opacity: loading ? 0.6 : 1 }}>
      <Typography.Text strong>{t('containerCpuTitle')}</Typography.Text>
      <Typography.Text type="secondary" style={{ display: 'block', marginTop: 4, fontSize: 12 }}>
        {t('containerCpuHint')}
      </Typography.Text>
      {chartData.length === 0 ? (
        <Typography.Text type="secondary" style={{ display: 'block', marginTop: 12 }}>
          {t('containerCpuEmpty')}
        </Typography.Text>
      ) : (
        <>
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: '4px 14px', marginTop: 12 }}>
            {legendItems.map((item) => {
              const hidden = hiddenKeys.has(item.key);
              return (
                <button
                  key={item.key}
                  type="button"
                  onClick={() => toggleGroup(item.key)}
                  aria-pressed={!hidden}
                  style={{
                    display: 'inline-flex',
                    alignItems: 'center',
                    gap: 6,
                    padding: 0,
                    border: 'none',
                    background: 'none',
                    cursor: 'pointer',
                    fontSize: 12,
                    color: hidden ? '#AEBECD' : '#425466',
                  }}
                >
                  <span
                    style={{
                      width: 8,
                      height: 8,
                      borderRadius: 2,
                      background: hidden ? '#DCE3EC' : item.color,
                    }}
                  />
                  {item.label}
                </button>
              );
            })}
          </div>
          <Column
            height={260}
            autoFit
            data={visibleData}
            xField="time"
            yField="value"
            colorField="group"
            stack
            animate={false}
            scale={{
              x: { type: 'band' },
              y: { domainMin: 0, nice: true },
              color: {
                domain: legendItems.map((item) => item.label),
                range: legendItems.map((item) => item.color),
              },
            }}
            axis={{
              x: {
                tickCount: 6,
                labelAutoHide: true,
                labelFormatter: (value: number) => formatTick(Number(value), range),
              },
              y: {
                title: '%',
                labelFormatter: (value: number) => `${Number(Number(value).toFixed(2))}%`,
              },
            }}
            legend={false}
            tooltip={{
              title: (datum: { time: number }) => dayjs(datum.time).format('YYYY-MM-DD HH:mm:ss'),
              items: [
                (datum: { group: string; value: number }) => ({
                  name: datum.group,
                  value: `${datum.value.toFixed(2)}%`,
                }),
              ],
            }}
            interaction={{ elementHighlight: true }}
            onReady={(chart: { on: (event: string, handler: (event: unknown) => void) => void }) => {
              chart.on('element:click', (event) => {
                const datum = (event as {
                  data?: { data?: { from?: string; to?: string; groupKey?: string } };
                }).data?.data;
                if (!datum?.from || !datum?.to) return;
                setModal({ from: datum.from, to: datum.to, groupKey: datum.groupKey });
              });
            }}
          />
        </>
      )}
      {modal ? (
        <ContainerProcessesModal
          open
          serverId={serverId}
          from={modal.from}
          to={modal.to}
          initialGroupKey={modal.groupKey}
          onClose={() => setModal(undefined)}
        />
      ) : null}
    </div>
  );
}
