export type TerminalMode = 'websocket' | 'polling';

export type TerminalHandlers = {
  onData: (chunk: Uint8Array | string) => void;
  onClose: () => void;
};

export type TerminalTransport = {
  mode: TerminalMode;
  send: (data: string) => void;
  resize: (cols: number, rows: number) => void;
  close: () => void;
};

export type TerminalTarget =
  | { kind: 'server'; id: string }
  | { kind: 'replica'; id: string }
  // Read-only container output; send() and resize() have no effect on it.
  | { kind: 'replica-logs'; id: string };

type RequestOptions = {
  method?: string;
  body?: string;
  contentType?: string;
  timeoutMs?: number;
};

const MODE_KEY = 'hc_terminal_transport';
const WEBSOCKET_READY_TIMEOUT_MS = 6000;
const POLL_TIMEOUT_MS = 35000;
const REQUEST_TIMEOUT_MS = 15000;
const RESIZE_DEBOUNCE_MS = 150;
const MAX_RETRY_DELAY_MS = 5000;

function authHeaders(): Record<string, string> {
  const token = localStorage.getItem('hc_token');
  return token ? { Authorization: `Bearer ${token}` } : {};
}

function delay(ms: number) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function apiBase(target: TerminalTarget) {
  if (target.kind === 'server') return `/api/servers/${target.id}/terminal`;
  if (target.kind === 'replica-logs') return `/api/replicas/${target.id}/logs`;
  return `/api/replicas/${target.id}/terminal`;
}

async function request(url: string, signal: AbortSignal, options: RequestOptions = {}) {
  const controller = new AbortController();
  const abort = () => controller.abort();
  const timer = window.setTimeout(abort, options.timeoutMs ?? REQUEST_TIMEOUT_MS);
  if (signal.aborted) abort();
  signal.addEventListener('abort', abort);
  try {
    return await fetch(url, {
      method: options.method ?? 'GET',
      body: options.body,
      headers: {
        ...authHeaders(),
        ...(options.contentType ? { 'Content-Type': options.contentType } : {}),
      },
      cache: 'no-store',
      signal: controller.signal,
    });
  } finally {
    window.clearTimeout(timer);
    signal.removeEventListener('abort', abort);
  }
}

async function errorMessage(response: Response) {
  try {
    const body = await response.json();
    if (body && typeof body.error === 'string') return body.error as string;
  } catch {
    // Non-JSON error body.
  }
  return `HTTP ${response.status}`;
}

function nextOffset(header: string | null, fallback: number) {
  const value = header === null ? Number.NaN : Number(header);
  return Number.isFinite(value) ? value : fallback;
}

function openWebSocket(
  target: TerminalTarget,
  cols: number,
  rows: number,
  handlers: TerminalHandlers,
  signal: AbortSignal,
) {
  return new Promise<TerminalTransport>((resolve, reject) => {
    const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const params = new URLSearchParams({
      token: localStorage.getItem('hc_token') || '',
      cols: String(cols),
      rows: String(rows),
    });
    const socket = new WebSocket(
      `${protocol}://${window.location.host}${apiBase(target)}?${params.toString()}`,
    );
    socket.binaryType = 'arraybuffer';
    let ready = false;
    let timer = 0;

    const sendResize = (nextCols: number, nextRows: number) => {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ type: 'resize', cols: nextCols, rows: nextRows }));
      }
    };
    const transport: TerminalTransport = {
      mode: 'websocket',
      send: (data) => {
        if (socket.readyState === WebSocket.OPEN) {
          socket.send(new TextEncoder().encode(data));
        }
      },
      resize: sendResize,
      close: () => {
        socket.onclose = null;
        socket.close();
      },
    };
    const fail = () => {
      if (ready) return;
      window.clearTimeout(timer);
      signal.removeEventListener('abort', fail);
      socket.onopen = null;
      socket.onmessage = null;
      socket.onerror = null;
      socket.onclose = null;
      socket.close();
      reject(new Error('WebSocket unavailable'));
    };

    timer = window.setTimeout(fail, WEBSOCKET_READY_TIMEOUT_MS);
    signal.addEventListener('abort', fail);
    socket.onopen = () => sendResize(cols, rows);
    socket.onmessage = (event) => {
      if (!ready) {
        ready = true;
        window.clearTimeout(timer);
        signal.removeEventListener('abort', fail);
        resolve(transport);
      }
      handlers.onData(event.data instanceof ArrayBuffer ? new Uint8Array(event.data) : String(event.data));
    };
    socket.onerror = fail;
    socket.onclose = () => {
      if (ready) {
        handlers.onClose();
      } else {
        fail();
      }
    };
  });
}

async function openPolling(
  target: TerminalTarget,
  cols: number,
  rows: number,
  handlers: TerminalHandlers,
  signal: AbortSignal,
): Promise<TerminalTransport> {
  const sessions = `${apiBase(target)}/sessions`;
  const created = await request(sessions, signal, {
    method: 'POST',
    body: JSON.stringify({ cols, rows }),
    contentType: 'application/json',
  });
  if (!created.ok) {
    throw new Error(await errorMessage(created));
  }
  const { streamId } = (await created.json()) as { streamId: string };
  const session = `${sessions}/${encodeURIComponent(streamId)}`;

  const closer = new AbortController();
  let closed = false;
  let offset = 0;
  let pendingInput = '';
  let sending = false;
  let resizeTimer = 0;

  const finish = () => {
    if (closed) return;
    closed = true;
    closer.abort();
    window.clearTimeout(resizeTimer);
    handlers.onClose();
  };

  const poll = async () => {
    let failures = 0;
    while (!closed) {
      try {
        const response = await request(`${session}/output?offset=${offset}`, closer.signal, {
          timeoutMs: POLL_TIMEOUT_MS,
        });
        if (response.status >= 500) {
          throw new Error(`HTTP ${response.status}`);
        }
        if (!response.ok) {
          finish();
          return;
        }
        const chunk = new Uint8Array(await response.arrayBuffer());
        failures = 0;
        offset = nextOffset(response.headers.get('X-Next-Offset'), offset + chunk.byteLength);
        if (chunk.byteLength > 0) {
          handlers.onData(chunk);
        }
        if (response.headers.get('X-Terminal-Closed') === '1') {
          finish();
          return;
        }
      } catch {
        if (closed) return;
        failures += 1;
        await delay(Math.min(1000 * failures, MAX_RETRY_DELAY_MS));
      }
    }
  };

  const flushInput = async () => {
    if (sending || closed || !pendingInput) return;
    sending = true;
    const data = pendingInput;
    pendingInput = '';
    try {
      const response = await request(`${session}/input`, closer.signal, {
        method: 'POST',
        body: data,
        contentType: 'application/octet-stream',
      });
      if (!response.ok && response.status < 500) {
        finish();
      }
    } catch {
      // Failed input is dropped, not retried: the server may already have applied it.
    } finally {
      sending = false;
    }
    void flushInput();
  };

  void poll();

  return {
    mode: 'polling',
    send: (data) => {
      if (closed) return;
      pendingInput += data;
      void flushInput();
    },
    resize: (nextCols, nextRows) => {
      window.clearTimeout(resizeTimer);
      resizeTimer = window.setTimeout(() => {
        if (closed) return;
        request(`${session}/resize`, closer.signal, {
          method: 'POST',
          body: JSON.stringify({ cols: nextCols, rows: nextRows }),
          contentType: 'application/json',
        }).catch(() => undefined);
      }, RESIZE_DEBOUNCE_MS);
    },
    close: () => {
      if (closed) return;
      closed = true;
      closer.abort();
      window.clearTimeout(resizeTimer);
      fetch(session, { method: 'DELETE', headers: authHeaders(), keepalive: true }).catch(() => undefined);
    },
  };
}

export async function connectTerminal(
  serverId: string,
  cols: number,
  rows: number,
  handlers: TerminalHandlers,
  signal: AbortSignal,
): Promise<TerminalTransport> {
  return connectTerminalTarget({ kind: 'server', id: serverId }, cols, rows, handlers, signal);
}

export async function connectTerminalTarget(
  target: TerminalTarget,
  cols: number,
  rows: number,
  handlers: TerminalHandlers,
  signal: AbortSignal,
): Promise<TerminalTransport> {
  if (sessionStorage.getItem(MODE_KEY) !== 'polling') {
    try {
      return await openWebSocket(target, cols, rows, handlers, signal);
    } catch (error) {
      if (signal.aborted) throw error;
    }
  }
  const transport = await openPolling(target, cols, rows, handlers, signal);
  sessionStorage.setItem(MODE_KEY, 'polling');
  return transport;
}
