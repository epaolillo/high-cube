// Keeps a persistent WebSocket open to /api/presence while the dashboard is in
// the foreground, so the control plane knows this user is online. The socket is
// re-opened automatically if it drops, and a light keep-alive ping is sent.

const PING_INTERVAL_MS = 25000;
const RECONNECT_DELAY_MS = 3000;

export type PresenceConnection = {
  close: () => void;
};

export function connectPresence(): PresenceConnection {
  let socket: WebSocket | null = null;
  let pingTimer = 0;
  let reconnectTimer = 0;
  let stopped = false;

  const clearTimers = () => {
    window.clearInterval(pingTimer);
    window.clearTimeout(reconnectTimer);
  };

  const scheduleReconnect = () => {
    if (stopped) return;
    window.clearTimeout(reconnectTimer);
    reconnectTimer = window.setTimeout(open, RECONNECT_DELAY_MS);
  };

  const open = () => {
    if (stopped) return;
    const token = localStorage.getItem('hc_token');
    if (!token) {
      scheduleReconnect();
      return;
    }
    const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const params = new URLSearchParams({ token });
    socket = new WebSocket(`${protocol}://${window.location.host}/api/presence?${params.toString()}`);

    socket.onopen = () => {
      window.clearInterval(pingTimer);
      pingTimer = window.setInterval(() => {
        if (socket?.readyState === WebSocket.OPEN) {
          socket.send('ping');
        }
      }, PING_INTERVAL_MS);
    };
    socket.onclose = () => {
      window.clearInterval(pingTimer);
      scheduleReconnect();
    };
    socket.onerror = () => {
      socket?.close();
    };
  };

  open();

  return {
    close: () => {
      stopped = true;
      clearTimers();
      if (socket) {
        socket.onclose = null;
        socket.close();
        socket = null;
      }
    },
  };
}
