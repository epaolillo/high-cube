import { Line } from '@ant-design/charts';
import { Descriptions, Table, Tooltip, Typography } from 'antd';
import dayjs from 'dayjs';
import { useTranslation } from 'react-i18next';
import { formatBytes, formatPercent, formatValue } from './format';
import type { AuditBlock, BarsBlock, KvBlock, LineBlock, TableBlock } from './types';

function unitFormatter(unit: string) {
  if (unit === 'bytes') return (value: number) => formatBytes(value);
  if (unit === 'percent') return (value: number) => formatPercent(value);
  return (value: number) => (Number.isInteger(value) ? value.toLocaleString() : value.toFixed(2));
}

function TableView({ block }: { block: TableBlock }) {
  const { t } = useTranslation();
  const rows = block.rows.map((row, index) => ({ ...row, __key: index }));
  return (
    <Table
      size="small"
      rowKey="__key"
      dataSource={rows}
      pagination={rows.length > 10 ? { pageSize: 10, size: 'small' } : false}
      scroll={{ x: true }}
      columns={block.columns.map((column) => ({
        key: column.key,
        dataIndex: column.key,
        title: t(`audit.col.${column.key}`, { defaultValue: column.key }),
        ellipsis: column.type === 'text',
        sorter:
          column.type === 'text'
            ? undefined
            : (a: Record<string, unknown>, b: Record<string, unknown>) =>
                Number(a[column.key] ?? 0) - Number(b[column.key] ?? 0),
        render: (value: unknown) => {
          if ((column.key === 'effect' || column.key === 'where') && value != null && value !== '') {
            const translated = t(`audit.volume.${column.key}.${String(value)}`, { defaultValue: '' });
            if (translated) return translated;
          }
          const text = formatValue(value, column.type);
          return column.type === 'text' && text.length > 40 ? (
            <Tooltip title={text}>
              <span style={{ fontFamily: 'ui-monospace, Menlo, monospace', fontSize: 12 }}>{text}</span>
            </Tooltip>
          ) : (
            <span style={{ whiteSpace: 'nowrap' }}>{text}</span>
          );
        },
      }))}
    />
  );
}

function BarsView({ block }: { block: BarsBlock }) {
  const { t } = useTranslation();
  const format = unitFormatter(block.unit);
  const max = Math.max(1, ...block.items.map((item) => item.value));
  return (
    <div style={{ display: 'grid', gap: 6 }}>
      {block.items.map((item) => (
        <div
          key={item.label}
          style={{ display: 'grid', gridTemplateColumns: 'minmax(120px, 32%) 1fr 90px', gap: 10, alignItems: 'center' }}
        >
          <Typography.Text ellipsis={{ tooltip: item.label }} style={{ fontSize: 12 }}>
            {t(`audit.label.${item.label}`, { defaultValue: item.label })}
          </Typography.Text>
          <div style={{ background: '#F3E3CC', borderRadius: 4, height: 12, overflow: 'hidden' }}>
            <div
              style={{
                width: `${Math.max(1, (item.value / max) * 100)}%`,
                height: '100%',
                background: '#E2621F',
                borderRadius: 4,
              }}
            />
          </div>
          <Typography.Text style={{ fontSize: 12, textAlign: 'right' }}>{format(item.value)}</Typography.Text>
        </div>
      ))}
    </div>
  );
}

function LineView({ block }: { block: LineBlock }) {
  const { t } = useTranslation();
  const format = unitFormatter(block.unit);
  const from = block.highlight ? dayjs(block.highlight.from).valueOf() : undefined;
  const to = block.highlight ? dayjs(block.highlight.to).valueOf() : undefined;
  const contextLabel = t('audit.series.context');
  const selectedLabel = t('audit.series.selected');
  // The selected interval is drawn as its own series so it stands out from the ±10 min context.
  const rows = block.points.flatMap((point) => {
    const time = dayjs(point.at).valueOf();
    if (from == null || to == null) return [{ time, value: point.value, series: selectedLabel }];
    const inside = time >= from && time <= to;
    return inside
      ? [
          { time, value: point.value, series: selectedLabel },
          { time, value: point.value, series: contextLabel },
        ]
      : [{ time, value: point.value, series: contextLabel }];
  });
  if (rows.length < 2) {
    return <Typography.Text type="secondary">{t('noMetricsYet')}</Typography.Text>;
  }
  return (
    <Line
      height={220}
      autoFit
      data={rows}
      xField="time"
      yField="value"
      seriesField="series"
      colorField="series"
      animate={false}
      scale={{
        x: { type: 'time' },
        y: { domainMin: 0, nice: true },
        color: { domain: [contextLabel, selectedLabel], range: ['#C1C9D2', '#E5484D'] },
      }}
      axis={{
        x: { tickCount: 6, labelAutoHide: true, labelFormatter: (value: number) => dayjs(Number(value)).format('DD/MM HH:mm') },
        y: { labelFormatter: (value: number) => format(Number(value)) },
      }}
      tooltip={{
        title: (datum: { time: number }) => dayjs(datum.time).format('YYYY-MM-DD HH:mm:ss'),
        items: [(datum: { value: number; series: string }) => ({ name: datum.series, value: format(datum.value) })],
      }}
      legend={from != null ? { color: { position: 'top' } } : false}
    />
  );
}

function KvView({ block }: { block: KvBlock }) {
  const { t } = useTranslation();
  return (
    <Descriptions
      size="small"
      column={{ xs: 1, sm: 2, md: 3 }}
      items={block.items.map((item) => ({
        key: item.key,
        label: t(`audit.kv.${item.key}`, { defaultValue: item.key }),
        children: formatValue(item.value, item.type),
      }))}
    />
  );
}

export function BlockView({ block }: { block: AuditBlock }) {
  const { t } = useTranslation();
  return (
    <div style={{ marginTop: 12, minWidth: 0, maxWidth: '100%', overflowX: 'auto' }}>
      <Typography.Text strong style={{ display: 'block', marginBottom: 8, fontSize: 13 }}>
        {t(`audit.block.${block.title}`, { defaultValue: block.title })}
      </Typography.Text>
      {block.kind === 'table' && <TableView block={block} />}
      {block.kind === 'bars' && <BarsView block={block} />}
      {block.kind === 'line' && <LineView block={block} />}
      {block.kind === 'kv' && <KvView block={block} />}
    </div>
  );
}
