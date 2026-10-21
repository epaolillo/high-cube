import { Tooltip } from 'antd';
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';

type Variant = 'light' | 'dark';

type Props = {
  cpuPercent?: number;
  memoryUsedBytes?: number;
  memoryLimitBytes?: number;
  diskUsedBytes?: number;
  /** Total disk capacity. When provided, the disk bar reflects used vs available. */
  diskTotalBytes?: number;
  /** Host GPU. Rows are drawn only when the server actually has one. */
  gpuPresent?: boolean;
  gpuPercent?: number;
  gpuMemoryUsedBytes?: number;
  gpuMemoryTotalBytes?: number;
  startedAt?: string;
  status?: 'running' | 'deploying' | 'stopped';
  replicas?: number;
  /** Visual theme of the panel. Defaults to the light UI theme. */
  variant?: Variant;
};

// Color palettes for the pixel panel. Light matches the Stripe UI theme; dark
// keeps the original high-contrast digital look.
type Palette = {
  bg: string;
  border: string;
  ledRunning: string;
  ledDeploying: string;
  ledStopped: string;
  replicas: string;
  clockRunning: string;
  clockStopped: string;
  numberRunning: string;
  numberStopped: string;
  segOff: string;
  cpu: string;
  mem: string;
  disk: string;
  gpu: string;
  vram: string;
  labelStopped: string;
};

const PALETTES: Record<Variant, Palette> = {
  dark: {
    bg: '#060f1b',
    border: '#13324d',
    ledRunning: '#2bf59a',
    ledDeploying: '#ffb224',
    ledStopped: '#37506a',
    replicas: '#5aa0d0',
    clockRunning: '#53ff8f',
    clockStopped: '#355a74',
    numberRunning: '#dcecf9',
    numberStopped: '#55707f',
    segOff: '#12304a',
    cpu: '#a08bff',
    mem: '#2bf59a',
    disk: '#ffb224',
    gpu: '#ff6b3d',
    vram: '#22d3ee',
    labelStopped: '#55707f',
  },
  light: {
    bg: '#F5F8FC',
    border: '#E3E8EE',
    ledRunning: '#09825D',
    ledDeploying: '#B76E00',
    ledStopped: '#C3CEDA',
    replicas: '#6B7C93',
    clockRunning: '#09825D',
    clockStopped: '#AEBECD',
    numberRunning: '#0A2540',
    numberStopped: '#AEBECD',
    segOff: '#DCE3EC',
    cpu: '#635BFF',
    mem: '#09825D',
    disk: '#B76E00',
    gpu: '#C2410C',
    vram: '#0E7490',
    labelStopped: '#8898AA',
  },
};

// Minimal 3x5 pixel font. Each glyph is five rows of three columns; a '1' lights
// up one pixel. Drawn 1:1 into a low-res buffer so every pixel stays crisp.
const FONT: Record<string, string[]> = {
  '0': ['111', '101', '101', '101', '111'],
  '1': ['010', '110', '010', '010', '111'],
  '2': ['111', '001', '111', '100', '111'],
  '3': ['111', '001', '111', '001', '111'],
  '4': ['101', '101', '111', '001', '001'],
  '5': ['111', '100', '111', '001', '111'],
  '6': ['111', '100', '111', '101', '111'],
  '7': ['111', '001', '010', '010', '010'],
  '8': ['111', '101', '111', '101', '111'],
  '9': ['111', '101', '111', '001', '111'],
  ':': ['000', '010', '000', '010', '000'],
  '.': ['000', '000', '000', '000', '010'],
  '%': ['101', '001', '010', '100', '101'],
  '-': ['000', '000', '111', '000', '000'],
  x: ['000', '101', '010', '101', '000'],
  d: ['001', '001', '111', '101', '111'],
  C: ['111', '100', '100', '100', '111'],
  M: ['101', '111', '111', '101', '101'],
  D: ['110', '101', '101', '101', '110'],
  G: ['111', '100', '101', '101', '111'],
  K: ['101', '110', '100', '110', '101'],
  V: ['101', '101', '101', '101', '010'],
  '/': ['001', '001', '010', '100', '100'],
  ' ': ['000', '000', '000', '000', '000'],
};

// Low-res virtual canvas (in font pixels) upscaled by SCALE with nearest-neighbor.
// The width is measured from the container at runtime so the panel fills it.
// Height grows by two rows when a GPU is present. MIN_VW is a safe fallback
// before the first measurement.
const MIN_VW = 120;
const ROW_TOP = 8;
const ROW_PITCH = 8;
const BASE_ROWS = 3;
const SCALE = 2;

function panelHeight(rowCount: number) {
  return ROW_TOP + rowCount * ROW_PITCH - 2;
}

function measure(text: string) {
  return text.length > 0 ? text.length * 4 - 1 : 0;
}

function drawText(ctx: CanvasRenderingContext2D, text: string, x: number, y: number, color: string) {
  ctx.fillStyle = color;
  let cursor = Math.round(x);
  const top = Math.round(y);
  for (const char of text) {
    const glyph = FONT[char] ?? FONT[' '];
    for (let r = 0; r < 5; r += 1) {
      const row = glyph[r];
      for (let c = 0; c < 3; c += 1) {
        if (row[c] === '1') {
          ctx.fillRect(cursor + c, top + r, 1, 1);
        }
      }
    }
    cursor += 4;
  }
}

function diskShort(bytes: number) {
  if (!bytes || bytes <= 0) return '0K';
  const k = bytes / 1024;
  const m = k / 1024;
  const g = m / 1024;
  if (g >= 1) return `${g >= 10 ? Math.round(g) : g.toFixed(1)}G`;
  if (m >= 1) return `${Math.round(m)}M`;
  return `${Math.round(k)}K`;
}

function formatUptime(ms: number) {
  if (ms <= 0) return '--:--:--';
  let s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  s %= 86400;
  const h = Math.floor(s / 3600);
  s %= 3600;
  const m = Math.floor(s / 60);
  s %= 60;
  const p = (n: number) => String(n).padStart(2, '0');
  if (d > 0) return `${d}d ${p(h)}:${p(m)}`;
  return `${p(h)}:${p(m)}:${p(s)}`;
}

function clamp(value: number) {
  return Math.max(0, Math.min(100, value));
}

// Digital panel: a status header (LED, replicas, uptime clock) plus full-width
// horizontal LED meters (CPU / MEM / DISK, and GPU compute + VRAM when the host
// has a GPU) with absolute readouts. The panel stretches to fill its container.
export function MetricDisplay({
  cpuPercent,
  memoryUsedBytes,
  memoryLimitBytes,
  diskUsedBytes,
  diskTotalBytes,
  gpuPresent = false,
  gpuPercent,
  gpuMemoryUsedBytes,
  gpuMemoryTotalBytes,
  startedAt,
  status = 'stopped',
  replicas,
  variant = 'light',
}: Props) {
  const { t } = useTranslation();
  const pal = PALETTES[variant];
  const wrapperRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [now, setNow] = useState(() => Date.now());
  const [vw, setVw] = useState(MIN_VW);
  const hasGpu = gpuPresent;
  const vh = panelHeight(hasGpu ? BASE_ROWS + 2 : BASE_ROWS);

  // Tick once per second to animate the uptime clock without re-polling.
  useEffect(() => {
    const timer = window.setInterval(() => {
      if (!document.hidden) setNow(Date.now());
    }, 1000);
    return () => window.clearInterval(timer);
  }, []);

  // Track the container width so the virtual canvas fills it (square pixels).
  useEffect(() => {
    const wrapper = wrapperRef.current;
    if (!wrapper) return undefined;
    const apply = (width: number) => {
      if (width > 0) setVw(Math.max(MIN_VW, Math.round(width / SCALE)));
    };
    apply(wrapper.clientWidth);
    const observer = new ResizeObserver((entries) => apply(entries[0].contentRect.width));
    observer.observe(wrapper);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // Backing store at the virtual resolution; CSS upscales with pixelated edges.
    canvas.width = vw;
    canvas.height = vh;
    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, vw, vh);

    // Solid background for maximum contrast.
    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, vw, vh);

    const running = status === 'running';
    const deploying = status === 'deploying';
    const ledColor = running ? pal.ledRunning : deploying ? pal.ledDeploying : pal.ledStopped;

    // Status LED (a crisp 3x3 block).
    ctx.fillStyle = ledColor;
    ctx.fillRect(1, 1, 3, 3);

    // Replicas count.
    if (replicas && replicas > 0) {
      drawText(ctx, `x${replicas}`, 6, 0, pal.replicas);
    }

    // Uptime clock (right-aligned).
    const uptime = running && startedAt ? now - Date.parse(startedAt) : 0;
    const clock = running ? formatUptime(uptime) : '--:--:--';
    drawText(ctx, clock, vw - 1 - measure(clock), 0, running ? pal.clockRunning : pal.clockStopped);

    // Metrics.
    const cpu = clamp(cpuPercent || 0);
    const memKnown = Boolean(memoryLimitBytes && memoryLimitBytes > 0);
    const mem = memKnown ? clamp(((memoryUsedBytes || 0) / memoryLimitBytes!) * 100) : 0;
    // When the real total is known (e.g. servers), the disk bar shows used vs
    // available. Otherwise fall back to a fixed reference for a rough gauge.
    const diskKnown = Boolean(diskTotalBytes && diskTotalBytes > 0);
    const diskRef = 2 * 1024 * 1024 * 1024;
    const disk = clamp(((diskUsedBytes || 0) / (diskKnown ? diskTotalBytes! : diskRef)) * 100);

    const meters = [
      { label: 'C', value: cpu, text: `${Math.round(cpu)}%`, color: pal.cpu },
      {
        label: 'M',
        value: mem,
        text: memKnown
          ? `${diskShort(memoryUsedBytes || 0)}/${diskShort(memoryLimitBytes!)}`
          : diskShort(memoryUsedBytes || 0),
        color: pal.mem,
      },
      {
        label: 'D',
        value: disk,
        text: diskKnown
          ? `${diskShort(diskUsedBytes || 0)}/${diskShort(diskTotalBytes!)}`
          : diskShort(diskUsedBytes || 0),
        color: pal.disk,
      },
    ];

    if (hasGpu) {
      const vramKnown = Boolean(gpuMemoryTotalBytes && gpuMemoryTotalBytes > 0);
      const vram = vramKnown ? clamp(((gpuMemoryUsedBytes || 0) / gpuMemoryTotalBytes!) * 100) : 0;
      meters.push(
        { label: 'G', value: clamp(gpuPercent || 0), text: `${Math.round(gpuPercent || 0)}%`, color: pal.gpu },
        {
          label: 'V',
          value: vram,
          text: vramKnown
            ? `${diskShort(gpuMemoryUsedBytes || 0)}/${diskShort(gpuMemoryTotalBytes!)}`
            : diskShort(gpuMemoryUsedBytes || 0),
          color: pal.vram,
        },
      );
    }

    const numberColor = running ? pal.numberRunning : pal.numberStopped;
    // Reserve a fixed right column so every readout aligns and bars share length.
    const readoutZone = Math.max(...meters.map((m) => measure(m.text)));
    const barLeft = 7; // after the 3px label glyph + gap
    const barRight = vw - 1 - readoutZone - 2;
    const barWidth = Math.max(4, barRight - barLeft);
    const pitch = 3; // 2px lit segment + 1px gap
    const segCount = Math.max(1, Math.floor((barWidth + 1) / pitch));

    meters.forEach((meter, index) => {
      const y = ROW_TOP + index * ROW_PITCH;
      // Label glyph.
      drawText(ctx, meter.label, 1, y, running ? meter.color : pal.labelStopped);
      // Readout, right-aligned.
      drawText(ctx, meter.text, vw - 1 - measure(meter.text), y, numberColor);
      // Horizontal LED meter, filled left to right.
      const lit = Math.round((meter.value / 100) * segCount);
      for (let i = 0; i < segCount; i += 1) {
        const on = running && i < lit;
        ctx.fillStyle = on ? meter.color : pal.segOff;
        ctx.fillRect(barLeft + i * pitch, y + 1, 2, 3);
      }
    });
  }, [
    cpuPercent,
    memoryUsedBytes,
    memoryLimitBytes,
    diskUsedBytes,
    diskTotalBytes,
    hasGpu,
    gpuPercent,
    gpuMemoryUsedBytes,
    gpuMemoryTotalBytes,
    startedAt,
    status,
    replicas,
    now,
    pal,
    vw,
    vh,
  ]);

  const memText =
    memoryLimitBytes && memoryLimitBytes > 0
      ? `${diskShort(memoryUsedBytes || 0)} / ${diskShort(memoryLimitBytes)}`
      : diskShort(memoryUsedBytes || 0);
  const diskText =
    diskTotalBytes && diskTotalBytes > 0
      ? `${diskShort(diskUsedBytes || 0)} / ${diskShort(diskTotalBytes)}`
      : diskShort(diskUsedBytes || 0);
  const gpuMemText =
    gpuMemoryTotalBytes && gpuMemoryTotalBytes > 0
      ? `${diskShort(gpuMemoryUsedBytes || 0)} / ${diskShort(gpuMemoryTotalBytes)}`
      : diskShort(gpuMemoryUsedBytes || 0);
  const uptimeText =
    status === 'running' && startedAt ? formatUptime(now - Date.parse(startedAt)) : '—';

  return (
    <Tooltip
      title={
        <div style={{ fontSize: 12, lineHeight: 1.5 }}>
          <div>
            {t('cpu')}: {Math.round(cpuPercent || 0)}%
          </div>
          <div>
            {t('memory')}: {memText}
          </div>
          <div>
            {t('disk')}: {diskText}
          </div>
          {hasGpu ? (
            <>
              <div>
                {t('gpu')}: {Math.round(gpuPercent || 0)}%
              </div>
              <div>
                {t('gpuMemory')}: {gpuMemText}
              </div>
            </>
          ) : null}
          <div>
            {t('uptime')}: {uptimeText}
          </div>
          {replicas && replicas > 0 ? (
            <div>
              {t('replicas')}: {replicas}
            </div>
          ) : null}
        </div>
      }
    >
      <div ref={wrapperRef} style={{ width: '100%' }}>
        <canvas
          ref={canvasRef}
          style={{
            width: '100%',
            height: vh * SCALE,
            borderRadius: 0,
            display: 'block',
            border: `1px solid ${pal.border}`,
            imageRendering: 'pixelated',
          }}
        />
      </div>
    </Tooltip>
  );
}
