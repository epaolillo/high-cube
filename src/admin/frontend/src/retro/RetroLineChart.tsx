import type { ReactNode } from 'react';

export type RetroSeries = {
  key: string;
  label: string;
  color: 'green' | 'amber' | 'red';
  values: number[];
  kind?: 'line' | 'area' | 'bars';
};

const VIEW_WIDTH = 1000;
const VIEW_HEIGHT = 200;
const Y_TICKS = 4;

// Round the axis ceiling up to 1, 2 or 5 times a power of ten.
function niceCeiling(value: number, fallback: number) {
  if (value <= 0) return fallback;
  const power = 10 ** Math.floor(Math.log10(value));
  const step = [1, 2, 5, 10].find((factor) => factor * power >= value) ?? 10;
  return step * power;
}

function pointsOf(values: number[], ceiling: number) {
  const step = VIEW_WIDTH / Math.max(1, values.length - 1);
  return values
    .map((value, index) => `${(index * step).toFixed(1)},${(VIEW_HEIGHT - (value / ceiling) * VIEW_HEIGHT).toFixed(1)}`)
    .join(' ');
}

/**
 * Phosphor plot for a CRT/LCD screen. Samples are evenly spaced; `labels` holds one
 * x-axis caption per sample and a handful of them are printed.
 */
export function RetroLineChart({
  title,
  series,
  labels,
  format,
  emptyLabel,
  height = 200,
  xTicks = 5,
  emptyFallback = 1,
  showLegend = true,
}: {
  title?: ReactNode;
  series: RetroSeries[];
  labels: string[];
  format: (value: number) => string;
  emptyLabel: string;
  height?: number;
  xTicks?: number;
  emptyFallback?: number;
  showLegend?: boolean;
}) {
  const head = (title || showLegend) && (
    <div className="rt-chart__head">
      {title && <span className="rt-chart__title">{title}</span>}
      {showLegend &&
        series.map((item) => (
          <span key={item.key} className="rt-chart__series" data-color={item.color}>
            <i />
            {item.label} <b>{format(item.values[item.values.length - 1] ?? 0)}</b>
          </span>
        ))}
    </div>
  );

  if (labels.length < 2) {
    return (
      <div className="rt-chart">
        {head}
        <div className="rt-chart__empty" style={{ minHeight: height }}>
          {emptyLabel}
        </div>
      </div>
    );
  }

  const peak = Math.max(0, ...series.flatMap((item) => item.values));
  const ceiling = niceCeiling(peak, emptyFallback);
  const tickCount = Math.min(xTicks, labels.length);
  const tickLabels = Array.from({ length: tickCount }, (_, index) =>
    labels[Math.round((index * (labels.length - 1)) / Math.max(1, tickCount - 1))],
  );

  return (
    <div className="rt-chart">
      {head}
      <div className="rt-chart__body" style={{ gridTemplateRows: `${height}px auto` }}>
        <div className="rt-chart__y">
          {Array.from({ length: Y_TICKS + 1 }, (_, index) => (
            <span key={index}>{format((ceiling * (Y_TICKS - index)) / Y_TICKS)}</span>
          ))}
        </div>
        <div className="rt-chart__plot">
          <svg viewBox={`0 0 ${VIEW_WIDTH} ${VIEW_HEIGHT}`} preserveAspectRatio="none">
            {Array.from({ length: Y_TICKS - 1 }, (_, index) => {
              const y = ((index + 1) * VIEW_HEIGHT) / Y_TICKS;
              return <line key={index} x1={0} x2={VIEW_WIDTH} y1={y} y2={y} className="rt-chart__grid" />;
            })}
            {series.map((item) => {
              if (item.kind === 'bars') {
                const slot = VIEW_WIDTH / item.values.length;
                return (
                  <g key={item.key} className="rt-chart__bars" data-color={item.color}>
                    {item.values.map((value, index) => {
                      const barHeight = (value / ceiling) * VIEW_HEIGHT;
                      return (
                        <rect
                          key={index}
                          x={index * slot + slot * 0.2}
                          width={slot * 0.6}
                          y={VIEW_HEIGHT - barHeight}
                          height={barHeight}
                        />
                      );
                    })}
                  </g>
                );
              }
              const points = pointsOf(item.values, ceiling);
              return (
                <g key={item.key}>
                  {item.kind === 'area' && (
                    <polygon
                      className="rt-chart__area"
                      data-color={item.color}
                      points={`0,${VIEW_HEIGHT} ${points} ${VIEW_WIDTH},${VIEW_HEIGHT}`}
                    />
                  )}
                  <polyline points={points} className="rt-chart__line" data-color={item.color} />
                </g>
              );
            })}
          </svg>
        </div>
        <div className="rt-chart__x">
          {tickLabels.map((label, index) => (
            <span key={index}>{label}</span>
          ))}
        </div>
      </div>
    </div>
  );
}
