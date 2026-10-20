import { Popover, Tooltip } from 'antd';

export type HostnameEntry = { hostname: string; tlsStatus?: string };

// Lock icon for secured (active TLS) hostnames.
function LockIcon() {
  return (
    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" aria-hidden>
      <path
        d="M6 10V8a6 6 0 1 1 12 0v2m-9 0h6a3 3 0 0 1 3 3v4a3 3 0 0 1-3 3H9a3 3 0 0 1-3-3v-4a3 3 0 0 1 3-3Z"
        stroke="currentColor"
        strokeWidth="1.8"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  );
}

// Globe icon for plain (non-TLS) hostnames.
function GlobeIcon() {
  return (
    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" aria-hidden>
      <circle cx="12" cy="12" r="9" stroke="currentColor" strokeWidth="1.8" />
      <path
        d="M3 12h18M12 3c2.5 2.5 2.5 15 0 18M12 3C9.5 5.5 9.5 18.5 12 21"
        stroke="currentColor"
        strokeWidth="1.8"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  );
}

// Maps a TLS status to the pill color variant.
function tlsVariant(tlsStatus?: string): 'active' | 'pending' | 'error' | 'none' {
  if (tlsStatus === 'active') return 'active';
  if (tlsStatus === 'pending') return 'pending';
  if (tlsStatus === 'failed' || tlsStatus === 'error') return 'error';
  return 'none';
}

// A single clickable hostname pill.
function Pill({ row, fixed }: { row: HostnameEntry; fixed?: boolean }) {
  const tls = tlsVariant(row.tlsStatus);
  const secure = tls === 'active';
  const href = `${secure ? 'https' : 'http'}://${row.hostname}`;
  return (
    <Tooltip title={href}>
      <a
        href={href}
        target="_blank"
        rel="noreferrer"
        className="hc-url-pill"
        data-tls={tls}
        style={fixed ? { flex: '0 0 auto' } : undefined}
        onClick={(event) => event.stopPropagation()}
      >
        {secure ? <LockIcon /> : <GlobeIcon />}
        <span>{row.hostname}</span>
      </a>
    </Tooltip>
  );
}

type Props = {
  hostnames?: HostnameEntry[];
  // When true, pills stay on a single line and scroll horizontally.
  scroll?: boolean;
  maxWidth?: number;
  // When true, only the first hostname is shown; the rest collapse behind a
  // "+N" trigger that reveals them in a hover popover. Avoids any scrolling.
  collapse?: boolean;
};

export function HostnamePills({ hostnames, scroll = false, maxWidth = 260, collapse = false }: Props) {
  if (!hostnames || hostnames.length === 0) {
    return null;
  }

  if (collapse) {
    const [first, ...rest] = hostnames;
    return (
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, maxWidth }}>
        <Pill row={first} fixed />
        {rest.length > 0 && (
          <Popover
            trigger="hover"
            placement="bottomLeft"
            content={
              <div
                style={{ display: 'flex', flexWrap: 'wrap', gap: 8, maxWidth: 360 }}
                onClick={(event) => event.stopPropagation()}
              >
                {rest.map((row) => (
                  <Pill key={row.hostname} row={row} />
                ))}
              </div>
            }
          >
            <span
              className="hc-url-pill hc-pill-more"
              data-tls="none"
              role="button"
              tabIndex={0}
              onClick={(event) => event.stopPropagation()}
            >
              +{rest.length}
            </span>
          </Popover>
        )}
      </div>
    );
  }

  const containerStyle: React.CSSProperties = scroll
    ? { display: 'flex', gap: 6, flexWrap: 'nowrap', overflowX: 'auto', maxWidth, paddingBottom: 2 }
    : { display: 'flex', gap: 8, flexWrap: 'wrap' };

  return (
    <div style={containerStyle} className={scroll ? 'hc-pill-scroll' : undefined}>
      {hostnames.map((row) => (
        <Pill key={row.hostname} row={row} fixed={scroll} />
      ))}
    </div>
  );
}
