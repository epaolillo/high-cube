import { Pie } from '@ant-design/charts';
import { Modal, Spin, Table, Typography } from 'antd';
import dayjs from 'dayjs';
import { useEffect, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../../api/client';

export type AttributionGroup = {
  key: string;
  kind: string;
  label: string;
  applicationId?: number;
  replicaId?: number;
  cpu: number;
  otherCpu?: number;
  procs: { name: string; cpu: number; pids: number; cmd?: string }[];
};

type Props = {
  open: boolean;
  serverId: string;
  from: string;
  to: string;
  initialGroupKey?: string;
  onClose: () => void;
};

function groupLabel(
  group: AttributionGroup,
  t: (key: string) => string,
): string {
  if (group.kind === 'host') return t('containerCpuHost');
  if (group.kind === 'docker' && group.key === 'docker') return t('containerCpuOtherDocker');
  return group.label || group.key;
}

function processLabel(name: string, t: (key: string) => string) {
  if (name === 'other') return t('containerCpuShortLived');
  return name;
}

export function ContainerProcessesModal({ open, serverId, from, to, initialGroupKey, onClose }: Props) {
  const { t } = useTranslation();
  const [groups, setGroups] = useState<AttributionGroup[]>([]);
  const [loading, setLoading] = useState(false);
  const [selectedKey, setSelectedKey] = useState<string>();

  useEffect(() => {
    if (!open) return undefined;
    let stopped = false;
    setLoading(true);
    api
      .get(`/servers/${serverId}/cpu-attribution/processes`, {
        params: { from, to },
        suppressErrorToast: true,
      })
      .then(({ data }) => {
        if (stopped) return;
        const next = Array.isArray(data.groups) ? (data.groups as AttributionGroup[]) : [];
        setGroups(next);
        const preferred =
          (initialGroupKey && next.find((g) => g.key === initialGroupKey)?.key) ||
          (initialGroupKey?.startsWith('docker') && next.find((g) => g.kind === 'docker')?.key) ||
          next[0]?.key;
        setSelectedKey(preferred);
      })
      .catch(() => {
        if (!stopped) {
          setGroups([]);
          setSelectedKey(undefined);
        }
      })
      .finally(() => {
        if (!stopped) setLoading(false);
      });
    return () => {
      stopped = true;
    };
  }, [open, serverId, from, to, initialGroupKey]);

  const selected = groups.find((g) => g.key === selectedKey) ?? groups[0];

  const containerPie = useMemo(
    () =>
      groups
        .filter((g) => g.cpu > 0.005)
        .map((g) => ({
          type: groupLabel(g, t),
          key: g.key,
          value: Number(g.cpu.toFixed(2)),
        })),
    [groups, t],
  );

  const processPie = useMemo(() => {
    if (!selected) return [];
    return selected.procs
      .filter((p) => p.cpu > 0.005)
      .map((p) => ({
        type: processLabel(p.name, t),
        value: Number(p.cpu.toFixed(2)),
      }));
  }, [selected, t]);

  const title = t('containerCpuModalTitle', {
    from: dayjs(from).format('HH:mm:ss'),
    to: dayjs(to).format('HH:mm:ss'),
  });

  return (
    <Modal open={open} onCancel={onClose} footer={null} width={760} title={title} destroyOnClose>
      {loading ? (
        <div style={{ textAlign: 'center', padding: 24 }}>
          <Spin />
        </div>
      ) : groups.length === 0 ? (
        <Typography.Text type="secondary">{t('containerCpuEmpty')}</Typography.Text>
      ) : (
        <div style={{ display: 'grid', gap: 24 }}>
          <div>
            <Typography.Text strong style={{ display: 'block', marginBottom: 8 }}>
              {t('containerCpuContainersPie')}
            </Typography.Text>
            <Pie
              height={220}
              autoFit
              data={containerPie}
              angleField="value"
              colorField="type"
              animate={false}
              legend={{ color: { position: 'right' } }}
              tooltip={{
                items: [
                  (datum: { type: string; value: number }) => ({
                    name: datum.type,
                    value: `${datum.value.toFixed(2)}%`,
                  }),
                ],
              }}
              interaction={{ elementHighlight: true }}
              onReady={(chart: { on: (event: string, handler: (event: unknown) => void) => void }) => {
                chart.on('element:click', (event) => {
                  const datum = (event as { data?: { data?: { key?: string } } }).data?.data;
                  if (datum?.key) setSelectedKey(datum.key);
                });
              }}
            />
          </div>

          {selected ? (
            <div>
              <Typography.Text strong style={{ display: 'block', marginBottom: 8 }}>
                {t('containerCpuProcessesPie', { label: groupLabel(selected, t) })}
              </Typography.Text>
              {processPie.length === 0 ? (
                <Typography.Text type="secondary">{t('containerCpuNoProcesses')}</Typography.Text>
              ) : (
                <Pie
                  height={220}
                  autoFit
                  data={processPie}
                  angleField="value"
                  colorField="type"
                  animate={false}
                  legend={{ color: { position: 'right' } }}
                  tooltip={{
                    items: [
                      (datum: { type: string; value: number }) => ({
                        name: datum.type,
                        value: `${datum.value.toFixed(2)}%`,
                      }),
                    ],
                  }}
                />
              )}
              <Table
                style={{ marginTop: 16 }}
                size="small"
                pagination={false}
                rowKey={(row) => row.name}
                dataSource={selected.procs}
                columns={[
                  {
                    title: t('containerCpuProcess'),
                    dataIndex: 'name',
                    render: (name: string) => processLabel(name, t),
                  },
                  {
                    title: t('containerCpuPercent'),
                    dataIndex: 'cpu',
                    width: 100,
                    render: (cpu: number) => `${cpu.toFixed(2)}%`,
                  },
                  {
                    title: t('containerCpuPids'),
                    dataIndex: 'pids',
                    width: 80,
                  },
                  {
                    title: t('containerCpuCmd'),
                    dataIndex: 'cmd',
                    ellipsis: true,
                    render: (cmd: string) => cmd || '—',
                  },
                ]}
              />
            </div>
          ) : null}
        </div>
      )}
    </Modal>
  );
}
