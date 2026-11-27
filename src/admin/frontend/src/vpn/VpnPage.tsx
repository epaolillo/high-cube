import { CloseOutlined, HddOutlined, InfoCircleOutlined, LaptopOutlined, MoreOutlined, WarningOutlined } from '@ant-design/icons';
import { Dropdown, Table, Tooltip, message } from 'antd';
import { useCallback, useEffect, useState, type ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { confirmDestructive } from '../components/confirmDestructive';
import dayjs from 'dayjs';
import { RetroLineChart } from '../retro/RetroLineChart';
import { RetroStatScreen } from '../retro/RetroStatScreen';
import { DeviceEnrollModal } from './DeviceEnrollModal';
import { VpnTopology } from './VpnTopology';
import { formatBytes, formatRate, publicIpOf, type VpnNode, type VpnState } from './types';
import './vpn.css';

const POLL_MS = 2000;
// About five minutes of history at one sample every two seconds.
const HISTORY_POINTS = 150;
const SPARK_BARS = 14;
const METER_SEGMENTS = 5;

type TrafficSample = { time: number; rx: number; tx: number };
type LedColor = 'green' | 'amber' | 'red';

function nodeStatus(node: VpnNode, t: (key: string) => string): { color: LedColor; text: string } {
  if (node.error === 'setup-required') return { color: 'amber', text: t('vpnSetupRequired') };
  if (node.kind === 'server' && !node.agentConnected && node.status !== 'active') {
    return { color: 'amber', text: t('vpnWaitingAgent') };
  }
  if (node.status !== 'active') return { color: 'amber', text: t('vpnStatusPending') };
  if (node.error) return { color: 'red', text: node.error };
  return node.connected ? { color: 'green', text: t('vpnConnected') } : { color: 'red', text: t('vpnDisconnected') };
}

// Address members outside the hub's LAN dial, as the admin computes it.
function hubPublicEndpoint(hub: VpnNode) {
  if (hub.endpoint) return hub.endpoint.includes(':') ? hub.endpoint : `${hub.endpoint}:${hub.listenPort}`;
  if (hub.site && hub.site !== 'local') return `${hub.site}:${hub.listenPort}`;
  return `${window.location.hostname}:${hub.listenPort}`;
}

function SegmentMeter({ value, total }: { value: number; total: number }) {
  const lit = total > 0 ? Math.round((value / total) * METER_SEGMENTS) : 0;
  return (
    <span className="hc-vpn-meter" aria-hidden>
      {Array.from({ length: METER_SEGMENTS }, (_, index) => (
        <i key={index} data-on={METER_SEGMENTS - index <= lit} />
      ))}
    </span>
  );
}

function SparkBars({ values }: { values: number[] }) {
  const recent = values.slice(-SPARK_BARS);
  const padded = [...Array<number>(SPARK_BARS - recent.length).fill(0), ...recent];
  const peak = Math.max(1, ...padded);
  return (
    <span className="hc-vpn-spark" aria-hidden>
      {padded.map((value, index) => (
        <i key={index} style={{ height: `${Math.max(8, (value / peak) * 100)}%` }} />
      ))}
    </span>
  );
}

function ModulePlate({ children }: { children: ReactNode }) {
  return (
    <span className="hc-vpn-plate rt-raised">
      {children}
      <span className="rt-stripes rt-stripes--mini" />
    </span>
  );
}

export default function VpnPage({ onClose }: { onClose?: () => void }) {
  const { t } = useTranslation();
  const [state, setState] = useState<VpnState>();
  const [history, setHistory] = useState<TrafficSample[]>([]);
  const [creating, setCreating] = useState(false);
  const [enrollOpen, setEnrollOpen] = useState(false);

  const load = useCallback(async (silent: boolean) => {
    const { data } = await api.get<VpnState>('/vpn', { suppressErrorToast: silent });
    setState(data);
    const time = data.now || Date.now();
    setHistory((previous) =>
      [...previous, { time, rx: data.totals.rxRate, tx: data.totals.txRate }].slice(-HISTORY_POINTS),
    );
  }, []);

  useEffect(() => {
    let stopped = false;
    load(false).catch(() => undefined);
    const timer = window.setInterval(() => {
      if (!stopped) load(true).catch(() => undefined);
    }, POLL_MS);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [load]);

  const createNetwork = async () => {
    setCreating(true);
    try {
      await api.post('/vpn', {});
      message.success(t('vpnCreated'));
      await load(false);
    } finally {
      setCreating(false);
    }
  };

  const deleteNetwork = async () => {
    if (!(await confirmDestructive(t, { title: t('vpnDeleteTitle'), text: t('vpnDeleteBody') }))) return;
    await api.delete('/vpn');
    message.success(t('vpnDeleted'));
    setHistory([]);
    await load(false);
  };

  const makeHub = async (node: VpnNode) => {
    if (
      !(await confirmDestructive(t, {
        title: t('vpnMakeHubTitle', { name: node.name }),
        text: t('vpnMakeHubBody'),
        confirmText: t('vpnMakeHub'),
        danger: false,
      }))
    )
      return;
    await api.post(`/vpn/nodes/${node.id}/hub`);
    message.success(t('vpnHubChanged'));
    await load(false);
  };

  const removeNode = async (node: VpnNode) => {
    if (!(await confirmDestructive(t, { text: t('vpnRemoveNodeBody', { name: node.name }), confirmText: t('vpnRemove') })))
      return;
    await api.delete(`/vpn/nodes/${node.id}`);
    message.success(t('vpnNodeRemoved'));
    await load(false);
  };

  if (!state) return null;
  const { network, nodes, links, totals } = state;
  const hub = nodes.find((node) => node.isHub);
  const trafficFlowing = totals.rxRate + totals.txRate > 0;
  const lamps = [
    { key: 'hub', on: Boolean(hub?.connected), label: t('vpnLampHub') },
    { key: 'members', on: totals.nodes > 0 && totals.online === totals.nodes, label: t('vpnLampMembers') },
    { key: 'traffic', on: trafficFlowing, label: t('vpnLampTraffic') },
  ];

  return (
    <div className="hc-vpn">
      <header className="hc-vpn__head">
        {onClose && (
          <button type="button" className="hc-vpn__close rt-btn" onClick={onClose} aria-label={t('close')}>
            <CloseOutlined />
          </button>
        )}
        <div className="hc-vpn__heading">
          <div className="hc-vpn__titlerow">
            <h2 className="hc-vpn__title">{t('vpn')}</h2>
            {network && <span className="hc-vpn__cidr rt-lcd rt-lcd-text">{network.cidr}</span>}
            {network && <span className="hc-vpn__iface rt-btn rt-btn--sm rt-btn--static">{network.interface}</span>}
            <span className="hc-vpn__grille" aria-hidden />
          </div>
          <div className="hc-vpn__subtitle">
            <span className="rt-stripes rt-stripes--mini" aria-hidden />
            {t('vpnSubtitle')}
          </div>
        </div>
        {network && (
          <div className="hc-vpn__actions">
            <Tooltip title={t('vpnProfileHint')}>
              <button type="button" className="rt-btn rt-btn--orange" onClick={() => setEnrollOpen(true)}>
                <LaptopOutlined /> {t('vpnAddThisPc')}
              </button>
            </Tooltip>
            <button type="button" className="hc-vpn__delete rt-btn rt-btn--danger" onClick={deleteNetwork}>
              {t('vpnDelete')}
            </button>
          </div>
        )}
      </header>
      <DeviceEnrollModal open={enrollOpen} onClose={() => setEnrollOpen(false)} />

      {!network ? (
        <div className="hc-vpn__empty rt-crt">
          <div className="hc-vpn__empty-title">{t('vpnEmptyTitle')}</div>
          <p>{t('vpnEmptyBody')}</p>
          <button type="button" className="rt-btn rt-btn--orange" disabled={creating} onClick={createNetwork}>
            {t('vpnCreate')}
          </button>
        </div>
      ) : (
        <>
          <div className="hc-vpn-alert rt-bezel" data-tone={hub ? 'info' : 'warn'}>
            <span className="hc-vpn-alert__icon">{hub ? <InfoCircleOutlined /> : <WarningOutlined />}</span>
            <div>
              <div className="hc-vpn-alert__title">
                {hub ? t('vpnHubReachTitle', { name: hub.name }) : t('vpnNoHub')}
              </div>
              {hub && (
                <div className="hc-vpn-alert__body">
                  {t('vpnHubReachBody', {
                    endpoint: hubPublicEndpoint(hub),
                    port: hub.listenPort,
                    lanIp: hub.lanIp || '—',
                  })}
                </div>
              )}
            </div>
          </div>

          <div className="hc-vpn-stats">
            <RetroStatScreen
              widgetSide="end"
              label={t('vpnMembersOnline')}
              value={`${totals.online}/${totals.nodes}`}
              widget={<SegmentMeter value={totals.online} total={totals.nodes} />}
            />
            <RetroStatScreen
              widgetSide="end"
              label={t('vpnLinksUp')}
              value={`${totals.linksUp}/${totals.links}`}
              widget={<SegmentMeter value={totals.linksUp} total={totals.links} />}
            />
            <RetroStatScreen
              widgetSide="end"
              label={t('vpnReceived')}
              value={formatRate(totals.rxRate)}
              widget={<SparkBars values={history.map((sample) => sample.rx)} />}
            />
            <RetroStatScreen
              widgetSide="end"
              label={t('vpnSent')}
              value={formatRate(totals.txRate)}
              widget={<SparkBars values={history.map((sample) => sample.tx)} />}
            />
          </div>

          <section className="hc-vpn-module hc-vpn-module--map rt-panel">
            <aside className="hc-vpn-module__side">
              <span className="hc-vpn-speaker" aria-hidden />
              <div className="hc-vpn-module__lamps">
                {lamps.map((lamp) => (
                  <Tooltip key={lamp.key} title={lamp.label} placement="right">
                    <span className="hc-vpn-lamp" data-on={lamp.on} aria-label={lamp.label} />
                  </Tooltip>
                ))}
              </div>
            </aside>
            <div className="hc-vpn-screen rt-crt">
              <ModulePlate>{t('vpnMap')}</ModulePlate>
              <VpnTopology nodes={nodes} links={links} hubId={network.hubNodeId} />
            </div>
            <footer className="hc-vpn-module__foot">
              <span className="rt-stripes" aria-hidden />
              <span className="hc-vpn-nameplate rt-inset">{t('vpnModuleLabel')}</span>
            </footer>
          </section>

          <section className="hc-vpn-module rt-panel">
            <div className="hc-vpn-screen hc-vpn-screen--table rt-crt">
              <ModulePlate>{t('vpnMembers')}</ModulePlate>
              <Table<VpnNode>
                className="hc-vpn-table"
                rowKey="id"
                dataSource={nodes}
                pagination={false}
                size="middle"
                scroll={{ x: 'max-content' }}
                columns={[
                  {
                    title: t('name'),
                    key: 'name',
                    render: (_, node) => (
                      <span className="hc-vpn-table__name">
                        {node.kind === 'server' ? <HddOutlined /> : <LaptopOutlined />}
                        <b>{node.name}</b>
                        {node.isHub && <span className="hc-vpn-chip">{t('vpnHub')}</span>}
                      </span>
                    ),
                  },
                  {
                    title: t('status'),
                    key: 'status',
                    render: (_, node) => {
                      const status = nodeStatus(node, t);
                      return (
                        <span className="hc-vpn-table__status" data-color={status.color}>
                          <i />
                          {status.text}
                        </span>
                      );
                    },
                  },
                  { title: t('vpnAddress'), dataIndex: 'address' },
                  { title: t('vpnLanIp'), dataIndex: 'lanIp', render: (value: string) => value || '—' },
                  {
                    title: t('vpnPublicIp'),
                    dataIndex: 'observedIp',
                    render: (value: string) => publicIpOf(value) || '—',
                  },
                  { title: t('vpnPort'), dataIndex: 'listenPort' },
                  {
                    title: t('vpnTrafficNow'),
                    key: 'rate',
                    render: (_, node) => `↓${formatRate(node.rxRate)} ↑${formatRate(node.txRate)}`,
                  },
                  {
                    title: t('vpnTrafficTotal'),
                    key: 'total',
                    render: (_, node) => `↓${formatBytes(node.rxBytes)} ↑${formatBytes(node.txBytes)}`,
                  },
                  {
                    title: '',
                    key: 'actions',
                    width: 48,
                    render: (_, node) => (
                      <Dropdown
                        trigger={['click']}
                        menu={{
                          items: [
                            ...(node.kind === 'server' && !node.isHub
                              ? [{ key: 'hub', label: t('vpnMakeHub'), onClick: () => void makeHub(node) }]
                              : []),
                            { key: 'remove', label: t('vpnRemove'), danger: true, onClick: () => void removeNode(node) },
                          ],
                        }}
                      >
                        <button type="button" className="rt-btn rt-btn--terminal rt-btn--icon" aria-label={t('actions')}>
                          <MoreOutlined />
                        </button>
                      </Dropdown>
                    ),
                  },
                ]}
              />
            </div>
          </section>

          <section className="hc-vpn-module rt-panel">
            <div className="hc-vpn-screen hc-vpn-screen--chart rt-crt">
              <ModulePlate>{t('vpnTraffic')}</ModulePlate>
              <div className="hc-vpn-screen__hint">{t('vpnTrafficHint')}</div>
              <RetroLineChart
                labels={history.map((sample) => dayjs(sample.time).format('HH:mm:ss'))}
                series={[
                  { key: 'rx', label: t('vpnReceived'), color: 'green', kind: 'area', values: history.map((sample) => sample.rx) },
                  { key: 'tx', label: t('vpnSent'), color: 'amber', values: history.map((sample) => sample.tx) },
                ]}
                format={formatRate}
                emptyFallback={1024}
                emptyLabel={t('vpnTrafficCollecting')}
                height={220}
              />
            </div>
          </section>
        </>
      )}
    </div>
  );
}
