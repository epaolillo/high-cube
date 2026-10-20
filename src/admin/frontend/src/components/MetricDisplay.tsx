import { Tooltip } from 'antd';
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';

type Props = {
  cpuPercent?: number;
  memoryUsedBytes?: number;
  memoryLimitBytes?: number;
  diskUsedBytes?: number;
  startedAt?: string;
  status?: 'running' | 'deploying' | 'stopped';
  replicas?: number;
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
  ' ': ['000', '000', '000', '000', '000'],
};

// Low-res virtual canvas (in font pixels) upscaled by SCALE with nearest-neighbor.
const VW = 86;
const VH = 34;
const SCALE = 2;

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

// Dark digital panel: three LED meter bars (CPU / MEM / DISK) plus an uptime
// clock in a crisp 1px pixel font, condensed into a tiny high-contrast display.
export function MetricDisplay({
  cpuPercent,
  memoryUsedBytes,
  memoryLimitBytes,
  diskUsedBytes,
  startedAt,
  status = 'stopped',
  replicas,
}: Props) {
  const { t } = useTranslation();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [now, setNow] = useState(() => Date.now());

  // Tick once per second to animate the uptime clock without re-polling.
  useEffect(() => {
    const timer = window.setInterval(() => {
      if (!document.hidden) setNow(Date.now());
    }, 1000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // Backing store at the virtual resolution; CSS upscales with pixelated edges.
    canvas.width = VW;
    canvas.height = VH;
    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, VW, VH);

    // Solid near-black background for maximum contrast.
    ctx.fillStyle = '#060f1b';
    ctx.fillRect(0, 0, VW, VH);

    const running = status === 'running';
    const deploying = status === 'deploying';
    const ledColor = running ? '#2bf59a' : deploying ? '#ffb224' : '#37506a';

    // Status LED (a crisp 3x3 block).
    ctx.fillStyle = ledColor;
    ctx.fillRect(1, 1, 3, 3);

    // Replicas count.
    if (replicas && replicas > 0) {
      drawText(ctx, `x${replicas}`, 6, 0, '#5aa0d0');
    }

    // Uptime clock (bright phosphor green, right-aligned).
    const uptime = running && startedAt ? now - Date.parse(startedAt) : 0;
    const clock = running ? formatUptime(uptime) : '--:--:--';
    drawText(ctx, clock, VW - 1 - measure(clock), 0, running ? '#53ff8f' : '#355a74');

    // Metrics.
    const cpu = clamp(cpuPercent || 0);
    const mem =
      memoryLimitBytes && memoryLimitBytes > 0
        ? clamp(((memoryUsedBytes || 0) / memoryLimitBytes) * 100)
        : 0;
    const diskRef = 2 * 1024 * 1024 * 1024;
    const disk = clamp(((diskUsedBytes || 0) / diskRef) * 100);

    const bars = [
      { label: 'C', value: cpu, text: `${Math.round(cpu)}%`, color: '#a08bff' },
      { label: 'M', value: mem, text: `${Math.round(mem)}%`, color: '#2bf59a' },
      { label: 'D', value: disk, text: diskShort(diskUsedBytes || 0), color: '#ffb224' },
    ];

    const centers = [16, 43, 70];
    const barWidth = 16;
    const segCount = 8;
    const bottomSegY = 27; // top-y of the lowest segment
    const numberColor = running ? '#dcecf9' : '#55707f';

    bars.forEach((bar, index) => {
      const cx = centers[index];
      const left = cx - barWidth / 2;
      // Value readout above the bar.
      drawText(ctx, bar.text, Math.round(cx - measure(bar.text) / 2), 7, numberColor);
      // LED meter: 8 crisp segments, lit from the bottom up.
      const lit = Math.round((bar.value / 100) * segCount);
      for (let i = 0; i < segCount; i += 1) {
        const y = bottomSegY - i * 2; // 1px segment + 1px gap
        const on = running && i < lit;
        ctx.fillStyle = on ? bar.color : '#12304a';
        ctx.fillRect(Math.round(left), y, barWidth, 1);
      }
      // Bar label underneath.
      drawText(ctx, bar.label, Math.round(cx - measure(bar.label) / 2), 29, running ? bar.color : '#55707f');
    });
  }, [cpuPercent, memoryUsedBytes, memoryLimitBytes, diskUsedBytes, startedAt, status, replicas, now]);

  const memText =
    memoryLimitBytes && memoryLimitBytes > 0
      ? `${diskShort(memoryUsedBytes || 0)} / ${diskShort(memoryLimitBytes)}`
      : diskShort(memoryUsedBytes || 0);
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
            {t('disk')}: {diskShort(diskUsedBytes || 0)}
          </div>
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
      <canvas
        ref={canvasRef}
        style={{
          width: VW * SCALE,
          height: VH * SCALE,
          borderRadius: 6,
          display: 'block',
          border: '1px solid #13324d',
          imageRendering: 'pixelated',
        }}
      />
    </Tooltip>
  );
}
