export type AuditKind = 'disk' | 'cpu' | 'ram';

export type Severity = 'ok' | 'info' | 'warning' | 'critical';

export type CheckStatus = 'pending' | 'running' | 'done' | 'skipped' | 'error';

export type AuditMessage = {
  code: string;
  params?: Record<string, unknown>;
};

export type ColumnType = 'text' | 'bytes' | 'percent' | 'number' | 'date' | 'duration';

export type TableBlock = {
  kind: 'table';
  title: string;
  columns: { key: string; type: ColumnType }[];
  rows: Record<string, unknown>[];
};

export type BarsBlock = {
  kind: 'bars';
  title: string;
  unit: 'bytes' | 'percent' | 'number';
  items: { label: string; value: number }[];
};

export type LineBlock = {
  kind: 'line';
  title: string;
  unit: 'bytes' | 'percent' | 'number';
  points: { at: string; value: number }[];
  highlight?: { from: string; to: string };
};

export type KvBlock = {
  kind: 'kv';
  title: string;
  items: { key: string; value: unknown; type: ColumnType }[];
};

export type AuditBlock = TableBlock | BarsBlock | LineBlock | KvBlock;

export type SuggestedAction = {
  id: string;
  target?: Record<string, unknown> | null;
  estimateBytes?: number;
  risk: 'low' | 'medium' | 'high';
  checkId?: string;
};

export type AuditCheck = {
  id: string;
  status: CheckStatus;
  severity?: Severity;
  durationMs?: number;
  conclusion?: AuditMessage;
  findings?: AuditMessage[];
  blocks?: AuditBlock[];
  actions?: SuggestedAction[];
};

export type AuditSummary = {
  score: number;
  keyFindings: { checkId: string; severity: Severity; conclusion: AuditMessage }[];
  actions: SuggestedAction[];
  reclaimableBytes: number;
  coverage?: number;
  verdict?: AuditMessage;
};

export type AuditJob = {
  kind: 'audit';
  audit: AuditKind;
  status: 'running' | 'done';
  plan: string[];
  checks: AuditCheck[];
  window?: { from: number; to: number };
  summary?: AuditSummary;
};

export type ActionResult = {
  ok?: boolean;
  metric?: 'diskAvailable' | 'memoryAvailable' | 'none';
  before?: number;
  after?: number;
  freedBytes?: number;
  output?: string;
};

export type ActionJob = {
  kind: 'action';
  status: 'running' | 'ok' | 'error';
  result?: ActionResult;
  error?: string;
};
