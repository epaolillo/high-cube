import { CloudServerOutlined, LaptopOutlined } from '@ant-design/icons';
import { useTranslation } from 'react-i18next';
import { formatRate, type VpnLink, type VpnNode } from './types';

const WIDTH = 860;
const HEIGHT = 440;
const NODE_RADIUS = 26;
const HUB_RADIUS = 34;

type Point = { x: number; y: number };

function nodeColor(node: VpnNode) {
  if (node.status !== 'active') return '#B76E00';
  return node.connected ? '#09825D' : '#DF1B41';
}

// Thicker lines for busier links, on a log scale so 1 KB/s and 100 MB/s both read well.
function strokeWidth(rate: number) {
  if (rate <= 0) return 2;
  return Math.min(9, 2 + Math.log10(1 + rate / 1024) * 1.6);
}

function layout(nodes: VpnNode[], hubId: number | null): Map<number, Point> {
  const positions = new Map<number, Point>();
  const center = { x: WIDTH / 2, y: HEIGHT / 2 };
  const hub = nodes.find((node) => node.id === hubId);
  const members = nodes.filter((node) => node.id !== hubId);
  if (hub) positions.set(hub.id, center);
  const radiusX = hub ? WIDTH / 2 - 110 : WIDTH / 2 - 120;
  const radiusY = HEIGHT / 2 - 70;
  members.forEach((node, index) => {
    if (!hub && members.length === 1) {
      positions.set(node.id, center);
      return;
    }
    const angle = -Math.PI / 2 + (index / members.length) * Math.PI * 2;
    positions.set(node.id, {
      x: center.x + radiusX * Math.cos(angle),
      y: center.y + radiusY * Math.sin(angle),
    });
  });
  return positions;
}

export function VpnTopology({
  nodes,
  links,
  hubId,
}: {
  nodes: VpnNode[];
  links: VpnLink[];
  hubId: number | null;
}) {
  const { t } = useTranslation();
  const positions = layout(nodes, hubId);

  if (nodes.length === 0) {
    return <div className="hc-vpn-map__empty">{t('vpnMapEmpty')}</div>;
  }

  return (
    <svg className="hc-vpn-map" viewBox={`0 0 ${WIDTH} ${HEIGHT}`} role="img" aria-label={t('vpnMap')}>
      {links.map((link) => {
        const from = positions.get(link.source);
        const to = positions.get(link.target);
        if (!from || !to) return null;
        const memberToHub = link.targetToSourceRate;
        const hubToMember = link.sourceToTargetRate;
        const total = memberToHub + hubToMember;
        const mid = { x: (from.x + to.x) / 2, y: (from.y + to.y) / 2 };
        const member = nodes.find((node) => node.id === link.target);
        return (
          <g key={`${link.source}-${link.target}`}>
            <line
              x1={from.x}
              y1={from.y}
              x2={to.x}
              y2={to.y}
              className="hc-vpn-map__link"
              data-state={link.connected ? 'up' : 'down'}
              data-busy={link.connected && total > 1024}
              strokeWidth={strokeWidth(total)}
            >
              <title>
                {`${member?.name ?? ''}: ${link.connected ? t('vpnLinkUp') : t('vpnLinkDown')}` +
                  (link.targetEndpoint ? ` · ${t('vpnVia')} ${link.targetEndpoint}` : '')}
              </title>
            </line>
            {link.connected && (
              <g transform={`translate(${mid.x}, ${mid.y})`}>
                <rect x={-58} y={-11} width={116} height={22} rx={11} className="hc-vpn-map__rate-bg" />
                <text className="hc-vpn-map__rate" textAnchor="middle" dy={4}>
                  {`↑${formatRate(memberToHub)} ↓${formatRate(hubToMember)}`}
                </text>
              </g>
            )}
          </g>
        );
      })}
      {nodes.map((node) => {
        const point = positions.get(node.id);
        if (!point) return null;
        const radius = node.isHub ? HUB_RADIUS : NODE_RADIUS;
        const color = nodeColor(node);
        const Icon = node.kind === 'server' ? CloudServerOutlined : LaptopOutlined;
        return (
          <g key={node.id} transform={`translate(${point.x}, ${point.y})`}>
            <title>
              {`${node.name} · ${node.address}` +
                (node.status !== 'active' ? ` · ${t('vpnStatusPending')}` : '') +
                (node.error ? ` · ${node.error}` : '')}
            </title>
            <circle r={radius + 6} fill={color} opacity={0.12} />
            <circle r={radius} fill="#FFFFFF" stroke={color} strokeWidth={node.isHub ? 3 : 2} />
            <foreignObject x={-radius} y={-radius} width={radius * 2} height={radius * 2}>
              <div className="hc-vpn-map__icon" style={{ color, fontSize: node.isHub ? 26 : 20 }}>
                <Icon />
              </div>
            </foreignObject>
            {node.isHub && (
              <text className="hc-vpn-map__hub" textAnchor="middle" y={-radius - 12}>
                {t('vpnHub')}
              </text>
            )}
            <text className="hc-vpn-map__name" textAnchor="middle" y={radius + 18}>
              {node.name}
            </text>
            <text className="hc-vpn-map__address" textAnchor="middle" y={radius + 33}>
              {node.address}
            </text>
          </g>
        );
      })}
    </svg>
  );
}
