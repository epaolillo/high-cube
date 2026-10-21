import { CaretRightOutlined, PauseOutlined, VerticalAlignBottomOutlined } from '@ant-design/icons';
import { Button, Space, Tag, Tooltip, Typography } from 'antd';
import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { connectTerminalTarget, type TerminalMode, type TerminalTransport } from '../terminal/transport';

// Output kept for scrolling back; older lines are dropped from the top.
const MAX_BUFFER_CHARS = 1024 * 1024;
const FLUSH_INTERVAL_MS = 150;
const BOTTOM_TOLERANCE_PX = 24;
// Color, cursor and title escape sequences: the viewer renders plain text.
const ANSI_SOURCE = String.raw`\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\)|[@-Z\\-_])`;
const ANSI_SEQUENCES = new RegExp(ANSI_SOURCE, 'g');
const ANSI_AT_START = new RegExp(`^${ANSI_SOURCE}`);

type StreamState = 'connecting' | 'live' | 'closed';

// Holds back an escape sequence or "\r\n" cut by a chunk boundary until the next chunk.
function splitIncomplete(text: string): [string, string] {
  const esc = text.lastIndexOf('\x1b');
  if (esc !== -1 && text.length - esc <= 64 && !ANSI_AT_START.test(text.slice(esc))) {
    return [text.slice(0, esc), text.slice(esc)];
  }
  if (text.endsWith('\r')) {
    return [text.slice(0, -1), '\r'];
  }
  return [text, ''];
}

function toPlainText(text: string) {
  return text.replace(ANSI_SEQUENCES, '').replace(/\r\n/g, '\n').replace(/\r/g, '\n');
}

// Keeps the last MAX_BUFFER_CHARS characters, cutting at a line start.
function keepTail(text: string) {
  if (text.length <= MAX_BUFFER_CHARS) return text;
  const lineEnd = text.indexOf('\n', text.length - MAX_BUFFER_CHARS - 1);
  return text.slice(lineEnd === -1 ? text.length - MAX_BUFFER_CHARS : lineEnd + 1);
}

// Drops whole lines from the top of the output until it fits the buffer. Returns the new length.
function trimTop(output: HTMLElement, length: number) {
  let remaining = length;
  while (remaining > MAX_BUFFER_CHARS && output.firstChild) {
    const node = output.firstChild as Text;
    const size = node.data.length;
    const excess = remaining - MAX_BUFFER_CHARS;
    const lineEnd = size > excess ? node.data.indexOf('\n', excess - 1) : -1;
    if (lineEnd === -1 || lineEnd + 1 >= size) {
      output.removeChild(node);
      remaining -= size;
    } else {
      node.deleteData(0, lineEnd + 1);
      remaining -= lineEnd + 1;
    }
  }
  return remaining;
}

export function ContainerLogs({
  replicaId,
  enabled,
  active,
}: {
  replicaId: number | undefined;
  enabled: boolean;
  active: boolean;
}) {
  const { t } = useTranslation();
  const viewportRef = useRef<HTMLDivElement>(null);
  const outputRef = useRef<HTMLPreElement>(null);
  // Text received but not yet rendered: everything that arrives while paused waits here.
  const pendingRef = useRef('');
  const renderedRef = useRef(0);
  const pausedRef = useRef(false);
  const followRef = useRef(true);
  const [paused, setPaused] = useState(false);
  const [follow, setFollow] = useState(true);
  const [state, setState] = useState<StreamState>('connecting');
  const [mode, setMode] = useState<TerminalMode>();
  const [hasOutput, setHasOutput] = useState(false);

  const scrollToBottom = useCallback(() => {
    const viewport = viewportRef.current;
    if (viewport) viewport.scrollTop = viewport.scrollHeight;
  }, []);

  const append = useCallback((text: string) => {
    if (text) pendingRef.current = keepTail(pendingRef.current + text);
  }, []);

  const flush = useCallback(() => {
    const output = outputRef.current;
    const text = pendingRef.current;
    if (!output || !text || pausedRef.current) return;
    pendingRef.current = '';
    output.appendChild(document.createTextNode(text));
    renderedRef.current = trimTop(output, renderedRef.current + text.length);
    setHasOutput(true);
    if (followRef.current) scrollToBottom();
  }, [scrollToBottom]);

  const setFollowing = (value: boolean) => {
    followRef.current = value;
    setFollow(value);
    if (value) scrollToBottom();
  };

  const setPausedState = (value: boolean) => {
    pausedRef.current = value;
    setPaused(value);
    if (!value) flush();
  };

  useEffect(() => {
    if (!enabled || !replicaId) return undefined;
    if (outputRef.current) outputRef.current.textContent = '';
    renderedRef.current = 0;
    pendingRef.current = '';
    setHasOutput(false);
    setState('connecting');

    let disposed = false;
    let transport: TerminalTransport | undefined;
    let carry = '';
    const decoder = new TextDecoder();
    const connecting = new AbortController();
    const receive = (raw: string) => {
      const [complete, rest] = splitIncomplete(carry + raw);
      carry = rest;
      append(toPlainText(complete));
    };

    connectTerminalTarget(
      { kind: 'replica-logs', id: String(replicaId) },
      200,
      50,
      {
        onData: (chunk) => {
          if (disposed) return;
          receive(typeof chunk === 'string' ? chunk : decoder.decode(chunk, { stream: true }));
        },
        onClose: () => {
          if (disposed) return;
          setState('closed');
          append(`\n[${t('logsStreamClosed')}]\n`);
        },
      },
      connecting.signal,
    )
      .then((connected) => {
        if (disposed) {
          connected.close();
          return;
        }
        transport = connected;
        setMode(connected.mode);
        setState('live');
      })
      .catch((error: unknown) => {
        if (disposed) return;
        const reason = error instanceof Error ? error.message : String(error);
        setState('closed');
        append(`[${t('logsUnavailable')}: ${reason}]\n`);
      });

    const timer = window.setInterval(flush, FLUSH_INTERVAL_MS);
    return () => {
      disposed = true;
      connecting.abort();
      transport?.close();
      window.clearInterval(timer);
    };
  }, [replicaId, enabled, t, append, flush]);

  // Fills the space down to the bottom of the window, like the shell next to it.
  useEffect(() => {
    const viewport = viewportRef.current;
    if (!viewport || !active) return undefined;
    const place = () => {
      const top = viewport.getBoundingClientRect().top;
      viewport.style.height = `${Math.max(280, Math.floor(window.innerHeight - top - 24))}px`;
      if (followRef.current) scrollToBottom();
    };
    place();
    const content = document.querySelector('.hc-content');
    const observer = new ResizeObserver(place);
    if (content) observer.observe(content);
    window.addEventListener('resize', place);
    return () => {
      observer.disconnect();
      window.removeEventListener('resize', place);
    };
  }, [active, enabled, scrollToBottom]);

  const onScroll = () => {
    const viewport = viewportRef.current;
    if (!viewport || !followRef.current) return;
    // Scrolling up to read stops pinning; "Fix bottom" turns it back on.
    if (viewport.scrollHeight - viewport.scrollTop - viewport.clientHeight > BOTTOM_TOLERANCE_PX) {
      setFollowing(false);
    }
  };

  if (!enabled || !replicaId) {
    return (
      <div
        style={{
          minHeight: 280,
          background: '#F6F9FC',
          color: '#697386',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          padding: 24,
          textAlign: 'center',
        }}
      >
        <div>
          <Typography.Title level={5} style={{ color: '#697386', marginTop: 0 }}>
            {t('logsNoContainer')}
          </Typography.Title>
          <Typography.Text type="secondary">{t('logsNoContainerHint')}</Typography.Text>
        </div>
      </div>
    );
  }

  const status = paused
    ? { color: 'gold', label: t('logsPaused') }
    : state === 'live'
      ? { color: 'green', label: t('logsLive') }
      : state === 'connecting'
        ? { color: 'default', label: t('logsConnecting') }
        : { color: 'red', label: t('logsDisconnected') };

  return (
    <div>
      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          gap: 12,
          padding: '8px 16px',
          borderBottom: '1px solid #E3E8EE',
          flexWrap: 'wrap',
        }}
      >
        <Space size={8}>
          <Tag color={status.color} style={{ marginInlineEnd: 0 }}>
            {status.label}
          </Tag>
          {mode === 'polling' && (
            <Tooltip title={t('terminalPollingHint')}>
              <Tag color="gold" style={{ marginInlineEnd: 0 }}>
                {t('terminalPolling')}
              </Tag>
            </Tooltip>
          )}
        </Space>
        <Space size={8} wrap>
          <Button
            size="small"
            icon={<CaretRightOutlined />}
            disabled={!paused}
            onClick={() => setPausedState(false)}
          >
            {t('logsPlay')}
          </Button>
          <Button
            size="small"
            icon={<PauseOutlined />}
            disabled={paused}
            onClick={() => setPausedState(true)}
          >
            {t('logsPause')}
          </Button>
          <Tooltip title={t('logsFixBottomHint')}>
            <Button
              size="small"
              type={follow ? 'primary' : 'default'}
              icon={<VerticalAlignBottomOutlined />}
              aria-pressed={follow}
              onClick={() => setFollowing(!follow)}
            >
              {t('logsFixBottom')}
            </Button>
          </Tooltip>
        </Space>
      </div>
      <div
        ref={viewportRef}
        onScroll={onScroll}
        style={{ minHeight: 280, overflow: 'auto', background: '#0A2540', position: 'relative' }}
      >
        {!hasOutput && (
          <div style={{ position: 'absolute', padding: '12px 16px', color: '#8792A2', fontSize: 13 }}>
            {t('logsWaiting')}
          </div>
        )}
        {/* Filled imperatively: re-rendering up to 1 MB of text through React on every chunk is too slow. */}
        <pre
          ref={outputRef}
          style={{
            margin: 0,
            padding: '12px 16px',
            color: '#E6EDF5',
            fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
            fontSize: 12.5,
            lineHeight: 1.5,
            whiteSpace: 'pre-wrap',
            wordBreak: 'break-word',
          }}
        />
      </div>
    </div>
  );
}
