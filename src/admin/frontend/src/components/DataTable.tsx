import { AutoComplete, Button, Dropdown, Input, Select, Space, Table } from 'antd';
import type { ColumnsType, TablePaginationConfig } from 'antd/es/table';
import { useEffect, useMemo, useRef, useState } from 'react';
import type { Key, MouseEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import api, { ListParams, ListResponse } from '../api/client';
import { confirmDestructive } from './confirmDestructive';

export type FilterDef = {
  field: string;
  label: string;
  type: 'text' | 'autocomplete';
  autocompleteUrl?: string;
};

export type ActionDef<T> = {
  key: string;
  label: string;
  danger?: boolean;
  onClick: (record: T) => void;
};

export type BulkActionDef<T> = {
  key: string;
  label: string;
  danger?: boolean;
  /** Runs against every selected record, across all visited pages. */
  onClick: (records: T[]) => void | Promise<void>;
};

type Props<T extends object> = {
  endpoint: string;
  columns: ColumnsType<T>;
  filters?: FilterDef[];
  actions?: ActionDef<T>[];
  bulkActions?: BulkActionDef<T>[];
  rowKey?: string;
  refreshKey?: number;
  /** Extra query params merged into every request (e.g. a category filter). */
  params?: Record<string, string | number | undefined>;
  /** Returns the detail route for a row. Rows without a route are not clickable. */
  rowLink?: (record: T) => string | undefined;
};

export function DataTable<T extends object>({
  endpoint,
  columns,
  filters = [],
  actions = [],
  bulkActions = [],
  rowKey = 'id',
  refreshKey = 0,
  params,
  rowLink,
}: Props<T>) {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const [rows, setRows] = useState<T[]>([]);
  const [total, setTotal] = useState(0);
  const [loading, setLoading] = useState(false);
  const [page, setPage] = useState(1);
  const [pageSize, setPageSize] = useState(10);
  const [sort, setSort] = useState<string>();
  const [order, setOrder] = useState<'asc' | 'desc'>('asc');
  const [filterValues, setFilterValues] = useState<Record<string, string>>({});
  const [autoOptions, setAutoOptions] = useState<Record<string, { value: string; label: string }[]>>({});
  const [selectedKeys, setSelectedKeys] = useState<Key[]>([]);
  const [bulkRunning, setBulkRunning] = useState(false);
  // Remember the actual records behind the selected keys so bulk actions can
  // run across pages, even after the current page's dataSource has changed.
  const selectedRecordsRef = useRef<Map<Key, T>>(new Map());

  const keyOf = (record: T) => (record as Record<string, unknown>)[rowKey] as Key;

  const clearSelection = () => {
    selectedRecordsRef.current.clear();
    setSelectedKeys([]);
  };

  const runBulkAction = async (action: BulkActionDef<T>) => {
    const records = selectedKeys
      .map((key) => selectedRecordsRef.current.get(key))
      .filter((record): record is T => Boolean(record));
    if (!records.length) return;
    // Destructive bulk actions must be confirmed first.
    if (action.danger && !(await confirmDestructive(t, { text: t('confirmDeleteManyBody', { count: records.length }) }))) {
      return;
    }
    setBulkRunning(true);
    try {
      await action.onClick(records);
      clearSelection();
    } finally {
      setBulkRunning(false);
    }
  };

  const fetchData = async () => {
    setLoading(true);
    try {
      const requestParams: ListParams = { page, pageSize, sort, order, ...params, ...filterValues };
      const { data } = await api.get<ListResponse<T>>(endpoint, { params: requestParams });
      setRows(data.rows);
      setTotal(data.total);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    fetchData();
  }, [endpoint, page, pageSize, sort, order, JSON.stringify(filterValues), JSON.stringify(params), refreshKey]);

  // Reset to the first page whenever the external params (e.g. category tab) change.
  const paramsKey = JSON.stringify(params);
  useEffect(() => {
    setPage(1);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [paramsKey]);

  const tableColumns = useMemo(() => {
    const cols = [...columns];
    if (actions.length) {
      cols.push({
        title: '',
        key: 'actions',
        width: 56,
        // Clicks inside the actions menu (including its portal) must not trigger row navigation.
        onCell: () => ({ onClick: (e: MouseEvent) => e.stopPropagation() }),
        render: (_: unknown, record: T) => (
          <Dropdown
            menu={{
              items: actions.map((action) => ({
                key: action.key,
                label: action.label,
                danger: action.danger,
                onClick: async () => {
                  // Destructive row actions must be confirmed first.
                  if (action.danger && !(await confirmDestructive(t))) return;
                  action.onClick(record);
                },
              })),
            }}
            trigger={['click']}
          >
            <a onClick={(e) => e.preventDefault()} style={{ color: '#6B7C93', fontSize: 18 }}>
              ⋮
            </a>
          </Dropdown>
        ),
      } as any);
    }
    return cols;
  }, [columns, actions, t]);

  const onTableChange = (pagination: TablePaginationConfig, _f: unknown, sorter: any) => {
    setPage(pagination.current || 1);
    setPageSize(pagination.pageSize || 10);
    if (sorter?.field) {
      setSort(String(sorter.field));
      setOrder(sorter.order === 'descend' ? 'desc' : 'asc');
    }
  };

  const searchAutocomplete = async (filter: FilterDef, q: string) => {
    if (!filter.autocompleteUrl) return;
    const { data } = await api.get<{ rows: { id: number; label: string }[] }>(filter.autocompleteUrl, {
      params: { q },
    });
    setAutoOptions((prev) => ({
      ...prev,
      [filter.field]: data.rows.map((row) => ({ value: String(row.id), label: row.label })),
    }));
  };

  return (
    <div>
      {filters.length > 0 && (
        <Space wrap style={{ marginBottom: 16 }}>
          {filters.map((filter) =>
            filter.type === 'text' ? (
              <Input
                key={filter.field}
                placeholder={filter.label}
                allowClear
                style={{ width: 220 }}
                onChange={(e) => {
                  setPage(1);
                  setFilterValues((prev) => ({ ...prev, [filter.field]: e.target.value }));
                }}
              />
            ) : (
              <AutoComplete
                key={filter.field}
                style={{ width: 220 }}
                options={autoOptions[filter.field] || []}
                placeholder={filter.label}
                onSearch={(q) => searchAutocomplete(filter, q)}
                onSelect={(value) => {
                  setPage(1);
                  setFilterValues((prev) => ({ ...prev, [filter.field]: String(value) }));
                }}
                allowClear
                onClear={() => {
                  setFilterValues((prev) => {
                    const next = { ...prev };
                    delete next[filter.field];
                    return next;
                  });
                }}
              />
            ),
          )}
        </Space>
      )}
      {bulkActions.length > 0 && selectedKeys.length > 0 && (
        <Space
          wrap
          style={{
            marginBottom: 12,
            padding: '8px 12px',
            background: '#F6F9FC',
            border: '1px solid #E3E8EE',
            borderRadius: 6,
          }}
        >
          <span style={{ color: '#425466' }}>{t('selectedCount', { count: selectedKeys.length })}</span>
          {bulkActions.map((action) => (
            <Button
              key={action.key}
              size="small"
              danger={action.danger}
              loading={bulkRunning}
              onClick={() => void runBulkAction(action)}
            >
              {action.label}
            </Button>
          ))}
          <Button size="small" type="text" onClick={clearSelection}>
            {t('clearSelection')}
          </Button>
        </Space>
      )}
      <Table
        rowKey={rowKey as any}
        // Keep background refreshes (e.g. real-time polling) from flashing the spinner.
        loading={loading && rows.length === 0}
        columns={tableColumns}
        dataSource={rows}
        rowSelection={
          bulkActions.length > 0
            ? {
                preserveSelectedRowKeys: true,
                selectedRowKeys: selectedKeys,
                onChange: (keys: Key[], selectedRows: T[]) => {
                  const map = selectedRecordsRef.current;
                  selectedRows.forEach((record) => {
                    if (record) map.set(keyOf(record), record);
                  });
                  for (const key of Array.from(map.keys())) {
                    if (!keys.includes(key)) map.delete(key);
                  }
                  setSelectedKeys(keys);
                },
              }
            : undefined
        }
        onChange={onTableChange}
        onRow={(record) => {
          const link = rowLink?.(record);
          if (!link) return {};
          return {
            style: { cursor: 'pointer' },
            onClick: (e: MouseEvent) => {
              if (e.ctrlKey || e.metaKey) {
                window.open(link, '_blank');
                return;
              }
              navigate(link);
            },
          };
        }}
        pagination={{
          current: page,
          pageSize,
          total,
          showSizeChanger: true,
          pageSizeOptions: ['10', '100', '1000'],
        }}
      />
      <div style={{ marginTop: 8, color: '#6B7C93', fontSize: 12 }}>
        Page size{' '}
        <Select
          size="small"
          value={pageSize}
          options={[10, 100, 1000].map((n) => ({ value: n, label: String(n) }))}
          onChange={(value) => {
            setPage(1);
            setPageSize(value);
          }}
          style={{ width: 90 }}
        />
      </div>
    </div>
  );
}
