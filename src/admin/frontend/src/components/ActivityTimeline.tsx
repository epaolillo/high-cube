import dayjs from 'dayjs';
import { useTranslation } from 'react-i18next';

export type ActivityBucket = {
  at: string;
  count: number;
  active: boolean;
};

const GREEN = '#09825d';
const RED = '#e5484d';
const ACCENT = '#635bff';

// Timeline of the last N hours. Action counts rise as bars from a centered
// baseline; the baseline itself is green while the user was present (active) and
// red while they were not.
export function ActivityTimeline({
  buckets,
  hours,
}: {
  buckets: ActivityBucket[];
  hours: number;
}) {
  const { t, i18n } = useTranslation();

  const height = 180;
  const barWidth = 16;
  const gap = 2;
  const paddingX = 8;
  const paddingY = 20;
  const width = Math.max(buckets.length * (barWidth + gap) + paddingX * 2, 320);
  const centerY = height / 2;
  const maxBarHeight = centerY - paddingY;
  const maxCount = Math.max(1, ...buckets.map((b) => b.count));

  // Show a time tick roughly every two hours.
  const bucketsPerTick = Math.max(1, Math.round(buckets.length / Math.max(1, Math.min(hours / 2, 6))));

  if (buckets.length === 0) {
    return null;
  }

  return (
    <div style={{ overflowX: 'auto', width: '100%' }}>
      <svg
        width={width}
        height={height + 24}
        role="img"
        aria-label={t('activityChartTitle', { hours })}
      >
        {/* Centered baseline split into presence segments. */}
        {buckets.map((bucket, index) => {
          const x = paddingX + index * (barWidth + gap);
          const barHeight = (bucket.count / maxCount) * maxBarHeight;
          return (
            <g key={bucket.at}>
              <title>
                {`${dayjs(bucket.at).locale(i18n.language).format('HH:mm')} · ${t('activityActions', {
                  count: bucket.count,
                })} · ${bucket.active ? t('presenceOnline') : t('presenceOffline')}`}
              </title>
              {/* Presence segment. */}
              <rect
                x={x}
                y={centerY - 2}
                width={barWidth}
                height={4}
                rx={2}
                fill={bucket.active ? GREEN : RED}
              />
              {/* Action count bar rising from the baseline. */}
              {bucket.count > 0 && (
                <rect
                  x={x + 2}
                  y={centerY - 4 - barHeight}
                  width={barWidth - 4}
                  height={barHeight}
                  rx={2}
                  fill={ACCENT}
                  opacity={0.9}
                />
              )}
            </g>
          );
        })}
        {/* Time ticks. */}
        {buckets.map((bucket, index) => {
          if (index % bucketsPerTick !== 0) return null;
          const x = paddingX + index * (barWidth + gap) + barWidth / 2;
          return (
            <text
              key={`tick-${bucket.at}`}
              x={x}
              y={height + 16}
              textAnchor="middle"
              fontSize={10}
              fill="#697386"
            >
              {dayjs(bucket.at).locale(i18n.language).format('HH:mm')}
            </text>
          );
        })}
      </svg>
    </div>
  );
}
