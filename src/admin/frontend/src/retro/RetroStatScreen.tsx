import type { ReactNode } from 'react';

const SIGNAL_BARS = 10;

/** Rising bar meter, like a signal-strength readout. `level` goes from 0 to 1. */
export function RetroSignal({ level }: { level: number }) {
  const lit = Math.round(Math.min(1, Math.max(0, level)) * SIGNAL_BARS);
  return (
    <span className="rt-signal" aria-hidden>
      {Array.from({ length: SIGNAL_BARS }, (_, index) => (
        <i key={index} data-on={index < lit} style={{ height: `${30 + (index / (SIGNAL_BARS - 1)) * 70}%` }} />
      ))}
    </span>
  );
}

/**
 * Small phosphor readout in a screwed bezel: caption on top, a widget on the left
 * and the value on the right. Pass `level` for the default signal meter.
 */
export function RetroStatScreen({
  label,
  value,
  level,
  widget,
  widgetSide = 'start',
}: {
  label: ReactNode;
  value: ReactNode;
  level?: number;
  widget?: ReactNode;
  widgetSide?: 'start' | 'end';
}) {
  return (
    <div className="rt-stat rt-bezel" data-widget={widgetSide}>
      <div className="rt-stat__label">{label}</div>
      <div className="rt-stat__row">
        {widget ?? (level !== undefined ? <RetroSignal level={level} /> : null)}
        <div className="rt-stat__value">{value}</div>
      </div>
    </div>
  );
}
