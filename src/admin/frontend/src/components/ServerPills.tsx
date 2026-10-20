import { Popover, Tooltip } from 'antd';
import { useNavigate } from 'react-router-dom';

export type ServerEntry = { id: number; name: string; ip?: string };

// Small server/rack icon for the pills.
function ServerIcon() {
  return (
    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" aria-hidden>
      <rect x="3" y="4" width="18" height="7" rx="1.5" stroke="currentColor" strokeWidth="1.8" />
      <rect x="3" y="13" width="18" height="7" rx="1.5" stroke="currentColor" strokeWidth="1.8" />
      <circle cx="7" cy="7.5" r="0.9" fill="currentColor" />
      <circle cx="7" cy="16.5" r="0.9" fill="currentColor" />
    </svg>
  );
}

// A single clickable server pill that navigates to the server detail page.
function Pill({ server, fixed }: { server: ServerEntry; fixed?: boolean }) {
  const navigate = useNavigate();
  return (
    <Tooltip title={server.ip || server.name}>
      <span
        className="hc-url-pill hc-pill-click"
        data-tls="none"
        role="button"
        tabIndex={0}
        style={fixed ? { flex: '0 0 auto' } : undefined}
        onClick={(event) => {
          event.stopPropagation();
          navigate(`/servers/${server.id}`);
        }}
      >
        <ServerIcon />
        <span>{server.name}</span>
      </span>
    </Tooltip>
  );
}

type Props = {
  servers?: ServerEntry[];
  // Show only the first server; collapse the rest behind a "+N" hover popover.
  collapse?: boolean;
};

export function ServerPills({ servers, collapse = true }: Props) {
  if (!servers || servers.length === 0) {
    return <span style={{ color: '#8898AA' }}>—</span>;
  }

  if (!collapse) {
    return (
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
        {servers.map((server) => (
          <Pill key={server.id} server={server} />
        ))}
      </div>
    );
  }

  const [first, ...rest] = servers;
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
      <Pill server={first} fixed />
      {rest.length > 0 && (
        <Popover
          trigger="hover"
          placement="bottomLeft"
          content={
            <div
              style={{ display: 'flex', flexWrap: 'wrap', gap: 8, maxWidth: 320 }}
              onClick={(event) => event.stopPropagation()}
            >
              {rest.map((server) => (
                <Pill key={server.id} server={server} />
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
