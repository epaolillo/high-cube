export type VpnNetwork = {
  name: string;
  cidr: string;
  interface: string;
  hubNodeId: number | null;
  createdAt: string;
};

export type VpnNode = {
  id: number;
  kind: 'server' | 'device';
  serverId: number | null;
  name: string;
  icon?: string;
  address: string;
  publicKey: string;
  listenPort: number;
  endpoint: string;
  lanIp: string;
  observedIp: string;
  site: string;
  status: 'pending' | 'active';
  lastSeenAt: string;
  isHub: boolean;
  reporting: boolean;
  interfaceUp: boolean;
  connected: boolean;
  agentConnected: boolean;
  error: string;
  reportedAt: number;
  rxRate: number;
  txRate: number;
  rxBytes: number;
  txBytes: number;
};

// Always between the hub (source) and one member (target).
export type VpnLink = {
  source: number;
  target: number;
  connected: boolean;
  latestHandshake: number;
  sourceToTargetRate: number;
  targetToSourceRate: number;
  sourceEndpoint: string;
  targetEndpoint: string;
};

export type VpnState = {
  now: number;
  suggestedPort: number;
  network: VpnNetwork | null;
  nodes: VpnNode[];
  links: VpnLink[];
  totals: { nodes: number; online: number; links: number; linksUp: number; rxRate: number; txRate: number };
};

export type ServerVpnIndicator = {
  member: boolean;
  connected: boolean;
  address?: string;
  isHub?: boolean;
  status?: string;
  peersUp?: number;
  peersTotal?: number;
  // Address nginx uses for this server right now; empty when it uses the configured IP.
  routedAddress?: string;
};

// The admin sees agents on its own host or LAN through loopback or private addresses,
// which say nothing about how the node is reached from the internet.
export function publicIpOf(ip: string) {
  if (!ip || ip === '::1' || /^(127|10)\./.test(ip) || /^192\.168\./.test(ip) || /^169\.254\./.test(ip)) return '';
  if (/^172\.(1[6-9]|2\d|3[01])\./.test(ip) || /^f[cd]/i.test(ip) || /^fe80/i.test(ip)) return '';
  return ip;
}

export function formatBytes(value: number) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let size = value;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(size >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}

export function formatRate(bytesPerSecond: number) {
  return `${formatBytes(Math.max(0, bytesPerSecond))}/s`;
}
