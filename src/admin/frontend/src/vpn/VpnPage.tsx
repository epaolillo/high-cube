import { Line } from '@ant-design/charts';
import { CloudServerOutlined, LaptopOutlined } from '@ant-design/icons';
import { Alert, Badge, Button, Dropdown, Table, Tag, Tooltip, Typography, message } from 'antd';
import dayjs from 'dayjs';
import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { confirmDestructive } from '../components/confirmDestructive';
import { VpnTopology } from './VpnTopology';
import { formatBytes, formatRate, publicIpOf, type VpnNode, type VpnState } from './types';
import './vpn.css';

const POLL_MS = 2000;
// About five minutes of history at one sample every two seconds.
const HISTORY_POINTS = 150;

type ThroughputPoint = { time: number; series: string; value: number };

function nodeBadge(node: VpnNode, t: (key: string) => string) {
  if (node.error === 'setup-required') return <Badge status="warning" text={t('vpnSetupRequired')} />;
  if (node.kind === 'server' && !node.agentConnected && node.status !== 'active') {
    return <Badge status="warning" text={t('vpnWaitingAgent')} />;
  }
  if (node.status !== 'active') return <Badge status="warning" text={t('vpnStatusPending')} />;
  if (node.error) return <Badge status="error" text={node.error} />;
  return node.connected ? (
    <Badge status="success" text={t('vpnConnected')} />
  ) : (
    <Badge status="error" text={t('vpnDisconnected')} />
  );
}

// Address members outside the hub's LAN dial, as the admin computes it.
function hubPublicEndpoint(hub: VpnNode) {
  if (hub.endpoint) return hub.endpoint.includes(':') ? hub.endpoint : `${hub.endpoint}:${hub.listenPort}`;
  if (hub.site && hub.site !== 'local') return `${hub.site}:${hub.listenPort}`;
  return `${window.location.hostname}:${hub.listenPort}`;
}

export default function VpnPage() {
  const { t } = useTranslation();
  const [state, setState] = useState<VpnState>();
  const [history, setHistory] = useState<ThroughputPoint[]>([]);
  const [creating, setCreating] = useState(false);
  const labels = useRef({ received: '', sent: '' });
  labels.current = { received: t('vpnReceived'), sent: t('vpnSent') };

  const load = useCallback(async (silent: boolean) => {
    const { data } = await api.get<VpnState>('/vpn', { suppressErrorToast: silent });
    setState(data);
    const time = data.now || Date.now();
    setHistory((previous) =>
      [
        ...previous,
        { time, series: labels.current.received, value: data.totals.rxRate },
        { time, series: labels.current.sent, value: data.totals.txRate },
      ].slice(-HISTORY_POINTS * 2),
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

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: 24 }}>
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
            <Typography.Title level={3} style={{ color: '#0A2540', margin: 0 }}>
              {t('vpn')}
            </Typography.Title>
            {network && <Tag style={{ fontFamily: 'monospace' }}>{network.cidr}</Tag>}
            {network && <Tag>{network.interface}</Tag>}
          </div>
          <Typography.Text type="secondary">{t('vpnSubtitle')}</Typography.Text>
        </div>
        {network && (
          <Button type="primary" danger onClick={deleteNetwork}>
            {t('vpnDelete')}
          </Button>
        )}
      </div>

      {!network ? (
        <div className="hc-vpn-card" style={{ textAlign: 'center', padding: 48 }}>
          <Typography.Title level={4} style={{ color: '#0A2540' }}>
            {t('vpnEmptyTitle')}
          </Typography.Title>
          <Typography.Paragraph type="secondary" style={{ maxWidth: 520, margin: '0 auto 20px' }}>
            {t('vpnEmptyBody')}
          </Typography.Paragraph>
          <Button type="primary" loading={creating} onClick={createNetwork}>
            {t('vpnCreate')}
          </Button>
        </div>
      ) : (
        <>
          {!hub && <Alert type="warning" showIcon message={t('vpnNoHub')} style={{ marginBottom: 16 }} />}
          {hub && (
            <Alert
              type="info"
              showIcon
              style={{ marginBottom: 16 }}
              message={t('vpnHubReachTitle', { name: hub.name })}
              description={t('vpnHubReachBody', {
                endpoint: hubPublicEndpoint(hub),
                port: hub.listenPort,
                lanIp: hub.lanIp || '—',
              })}
            />
          )}

          <div className="hc-vpn-stats">
            <div className="hc-vpn-card">
              <div className="hc-vpn-stat__label">{t('vpnMembersOnline')}</div>
              <div className="hc-vpn-stat__value">
                {totals.online} / {totals.nodes}
              </div>
            </div>
            <div className="hc-vpn-card">
              <div className="hc-vpn-stat__label">{t('vpnLinksUp')}</div>
              <div className="hc-vpn-stat__value">
                {totals.linksUp} / {totals.links}
              </div>
            </div>
            <div className="hc-vpn-card">
              <div className="hc-vpn-stat__label">{t('vpnReceived')}</div>
              <div className="hc-vpn-stat__value">{formatRate(totals.rxRate)}</div>
            </div>
            <div className="hc-vpn-card">
              <div className="hc-vpn-stat__label">{t('vpnSent')}</div>
              <div className="hc-vpn-stat__value">{formatRate(totals.txRate)}</div>
            </div>
          </div>

          <div className="hc-vpn-card" style={{ marginBottom: 16 }}>
            <Typography.Text strong>{t('vpnMap')}</Typography.Text>
            <VpnTopology nodes={nodes} links={links} hubId={network.hubNodeId} />
          </div>

          <div className="hc-vpn-card" style={{ marginBottom: 16 }}>
            <Typography.Text strong>{t('vpnTraffic')}</Typography.Text>
            <Typography.Text type="secondary" style={{ display: 'block', fontSize: 12 }}>
              {t('vpnTrafficHint')}
            </Typography.Text>
            {history.length < 4 ? (
              <Typography.Text type="secondary">{t('vpnTrafficCollecting')}</Typography.Text>
            ) : (
              <Line
                height={220}
                autoFit
                data={history}
                xField="time"
                yField="value"
                seriesField="series"
                animate={false}
                scale={{ x: { type: 'time' }, y: { domainMin: 0, nice: true }, color: { range: ['#635BFF', '#09825D'] } }}
                axis={{
                  x: { tickCount: 6, labelFormatter: (value: number) => dayjs(Number(value)).format('HH:mm:ss') },
                  y: { labelFormatter: (value: number) => formatRate(Number(value)) },
                }}
                tooltip={{
                  title: (datum: ThroughputPoint) => dayjs(datum.time).format('HH:mm:ss'),
                  items: [(datum: ThroughputPoint) => ({ name: datum.series, value: formatRate(datum.value) })],
                }}
                legend={{ color: { position: 'top' } }}
              />
            )}
          </div>

          <div className="hc-vpn-card" style={{ padding: 0 }}>
            <Table<VpnNode>
              rowKey="id"
              dataSource={nodes}
              pagination={false}
              size="middle"
              columns={[
                {
                  title: t('name'),
                  key: 'name',
                  render: (_, node) => (
                    <span style={{ display: 'inline-flex', alignItems: 'center', gap: 8 }}>
                      {node.kind === 'server' ? <CloudServerOutlined /> : <LaptopOutlined />}
                      <span style={{ color: '#0A2540', fontWeight: 500 }}>{node.name}</span>
                      {node.isHub && <Tag color="purple">{t('vpnHub')}</Tag>}
                    </span>
                  ),
                },
                { title: t('status'), key: 'status', render: (_, node) => nodeBadge(node, t) },
                {
                  title: t('vpnAddress'),
                  dataIndex: 'address',
                  render: (value: string) => <Typography.Text code>{value}</Typography.Text>,
                },
                { title: t('vpnLanIp'), dataIndex: 'lanIp', render: (value: string) => value || '—' },
                { title: t('vpnPublicIp'), dataIndex: 'observedIp', render: (value: string) => publicIpOf(value) || '—' },
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
                      <Tooltip title={t('actions')}>
                        <Button type="text">⋮</Button>
                      </Tooltip>
                    </Dropdown>
                  ),
                },
              ]}
            />
          </div>
        </>
      )}
    </div>
  );
}
