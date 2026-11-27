import { HddOutlined, LaptopOutlined } from '@ant-design/icons';
import { useTranslation } from 'react-i18next';
import { formatRate, type VpnLink, type VpnNode } from './types';

const WIDTH = 960;
const HEIGHT = 470;
const NODE_RADIUS = 22;
const HUB_RADIUS = 30;

type Point = { x: number; y: number };

function nodeState(node: VpnNode) {
  if (node.status !== 'active') return 'pending';
  return node.connected ? 'up' : 'down';
}

// Thicker lines for busier links, on a log scale so 1 KB/s and 100 MB/s both read well.
function strokeWidth(rate: number) {
  if (rate <= 0) return 2;
  return Math.min(7, 2 + Math.log10(1 + rate / 1024) * 1.4);
}

function layout(nodes: VpnNode[], hubId: number | null): Map<number, Point> {
  const positions = new Map<number, Point>();
  const center = { x: WIDTH / 2, y: 240 };
  const hub = nodes.find((node) => node.id === hubId);
  const members = nodes.filter((node) => node.id !== hubId);
  if (hub) positions.set(hub.id, center);
  const radiusX = WIDTH / 2 - 140;
  const radiusY = 160;
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
      <defs>
        <filter id="hc-vpn-glow" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur stdDeviation="3" result="blur" />
          <feMerge>
            <feMergeNode in="blur" />
            <feMergeNode in="SourceGraphic" />
          </feMerge>
        </filter>
        {/* User-space region: vertical or horizontal links have a zero-size bounding box. */}
        <filter id="hc-vpn-glow-link" filterUnits="userSpaceOnUse" x={0} y={0} width={WIDTH} height={HEIGHT}>
          <feGaussianBlur stdDeviation="3" result="blur" />
          <feMerge>
            <feMergeNode in="blur" />
            <feMergeNode in="SourceGraphic" />
          </feMerge>
        </filter>
      </defs>
      {links.map((link) => {
        const from = positions.get(link.source);
        const to = positions.get(link.target);
        if (!from || !to) return null;
        const memberToHub = link.targetToSourceRate;
        const hubToMember = link.sourceToTargetRate;
        const total = memberToHub + hubToMember;
        const mid = { x: (from.x + to.x) / 2, y: (from.y + to.y) / 2 };
        const member = nodes.find((node) => node.id === link.target);
        const rateLabel = `↑${formatRate(memberToHub)} ↓${formatRate(hubToMember)}`;
        // VT323 glyphs are about 0.45em wide.
        const pillWidth = rateLabel.length * 8 + 18;
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
              filter="url(#hc-vpn-glow-link)"
            >
              <title>
                {`${member?.name ?? ''}: ${link.connected ? t('vpnLinkUp') : t('vpnLinkDown')}` +
                  (link.targetEndpoint ? ` · ${t('vpnVia')} ${link.targetEndpoint}` : '')}
              </title>
            </line>
            {link.connected && (
              <g transform={`translate(${mid.x}, ${mid.y})`}>
                <rect x={-pillWidth / 2} y={-11} width={pillWidth} height={22} rx={11} className="hc-vpn-map__rate-bg" />
                <text className="hc-vpn-map__rate" textAnchor="middle" dy={5}>
                  {rateLabel}
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
        const Icon = node.kind === 'server' ? HddOutlined : LaptopOutlined;
        return (
          <g
            key={node.id}
            className="hc-vpn-map__node"
            data-state={nodeState(node)}
            data-hub={node.isHub}
            transform={`translate(${point.x}, ${point.y})`}
          >
            <title>
              {`${node.name} · ${node.address}` +
                (node.status !== 'active' ? ` · ${t('vpnStatusPending')}` : '') +
                (node.error ? ` · ${node.error}` : '')}
            </title>
            {node.isHub && <circle r={radius + 7} className="hc-vpn-map__halo" />}
            <circle r={radius} className="hc-vpn-map__disc" filter="url(#hc-vpn-glow)" />
            <foreignObject x={-radius} y={-radius} width={radius * 2} height={radius * 2}>
              <div className="hc-vpn-map__icon" style={{ fontSize: node.isHub ? 26 : 19 }}>
                <Icon />
              </div>
            </foreignObject>
            {node.isHub && (
              <g transform={`translate(0, ${-radius - 18})`}>
                <rect x={-34} y={-10} width={68} height={20} rx={4} className="hc-vpn-map__hub-bg" />
                <text className="hc-vpn-map__hub" textAnchor="middle" dy={5}>
                  {t('vpnHub')}
                </text>
              </g>
            )}
            <text className="hc-vpn-map__name" textAnchor="middle" y={radius + 24}>
              {node.name}
            </text>
            <text className="hc-vpn-map__address" textAnchor="middle" y={radius + 43}>
              {node.address}
            </text>
          </g>
        );
      })}
    </svg>
  );
}
