import { LeftOutlined, RightOutlined } from '@ant-design/icons';

type Props = {
  page: number;
  pageSize: number;
  total: number;
  onChange: (page: number) => void;
};

// Compact prev/next pager with a "start–end / total" readout. Rendered only
// when there is more than one page worth of data.
export function Pager({ page, pageSize, total, onChange }: Props) {
  if (total <= pageSize) return null;
  const pages = Math.max(1, Math.ceil(total / pageSize));
  const start = (page - 1) * pageSize + 1;
  const end = Math.min(page * pageSize, total);
  return (
    <div className="hc-pager">
      <span className="hc-pager__info">
        {start}–{end} / {total}
      </span>
      <button
        type="button"
        className="hc-iconbtn"
        disabled={page <= 1}
        onClick={() => onChange(page - 1)}
      >
        <LeftOutlined />
      </button>
      <button
        type="button"
        className="hc-iconbtn"
        disabled={page >= pages}
        onClick={() => onChange(page + 1)}
      >
        <RightOutlined />
      </button>
    </div>
  );
}
