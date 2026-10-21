import api from '../api/client';

export type ServerMetricSnapshot = {
  cpuPercent?: number;
  memoryUsedBytes?: number;
  memoryLimitBytes?: number;
  diskUsedBytes?: number;
  diskTotalBytes?: number;
  gpuPresent?: boolean;
  gpuPercent?: number;
  gpuMemoryUsedBytes?: number;
  gpuMemoryTotalBytes?: number;
};

type CpuCounters = { user?: number; nice?: number; system?: number; idle?: number };
type GpuInfo = { utilizationPercent?: number; memoryUsedMb?: number; memoryTotalMb?: number };
type MetricPayload = {
  memory?: { totalKb?: number; availableKb?: number };
  cpu?: CpuCounters;
  disk?: { totalBytes?: number; availableBytes?: number };
  gpu?: { present?: boolean; gpus?: GpuInfo[] };
};
type MetricRow = { payload: MetricPayload };

function utcStamp(date: Date) {
  return date.toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function cpuFromSamples(previous: CpuCounters, current: CpuCounters) {
  const busy = (s: CpuCounters) => (s.user || 0) + (s.nice || 0) + (s.system || 0);
  const total = (s: CpuCounters) => busy(s) + (s.idle || 0);
  const deltaTotal = total(current) - total(previous);
  const deltaBusy = busy(current) - busy(previous);
  if (deltaTotal <= 0) return 0;
  return Math.max(0, Math.min(100, (deltaBusy / deltaTotal) * 100));
}

function snapshotFromRows(rows: MetricRow[]): ServerMetricSnapshot {
  if (!rows.length) return {};
  const latest = rows[rows.length - 1];
  const previous = rows.length > 1 ? rows[rows.length - 2] : undefined;
  const cpuPercent =
    previous?.payload.cpu && latest.payload.cpu
      ? cpuFromSamples(previous.payload.cpu, latest.payload.cpu)
      : undefined;
  const totalKb = latest.payload.memory?.totalKb || 0;
  const availableKb = latest.payload.memory?.availableKb || 0;
  const diskTotal = latest.payload.disk?.totalBytes || 0;
  const diskAvailable = latest.payload.disk?.availableBytes || 0;
  const gpus = latest.payload.gpu?.present ? latest.payload.gpu.gpus ?? [] : [];
  const gpuCount = gpus.length;
  const gpuMemoryUsedMb = gpus.reduce((sum, gpu) => sum + (gpu.memoryUsedMb || 0), 0);
  const gpuMemoryTotalMb = gpus.reduce((sum, gpu) => sum + (gpu.memoryTotalMb || 0), 0);
  return {
    cpuPercent,
    memoryLimitBytes: totalKb * 1024,
    memoryUsedBytes: Math.max(0, (totalKb - availableKb) * 1024),
    diskUsedBytes: Math.max(0, diskTotal - diskAvailable),
    diskTotalBytes: diskTotal,
    gpuPresent: gpuCount > 0,
    gpuPercent:
      gpuCount > 0 ? gpus.reduce((sum, gpu) => sum + (gpu.utilizationPercent || 0), 0) / gpuCount : undefined,
    gpuMemoryUsedBytes: gpuMemoryUsedMb * 1024 * 1024,
    gpuMemoryTotalBytes: gpuMemoryTotalMb * 1024 * 1024,
  };
}

// Fetches the latest resource snapshot for one server from its recent metrics.
export async function fetchServerSnapshot(id: number): Promise<ServerMetricSnapshot> {
  const to = new Date();
  const from = new Date(to.getTime() - 5 * 60 * 1000);
  try {
    const { data } = await api.get(`/servers/${id}/metrics`, {
      params: { from: utcStamp(from), to: utcStamp(to) },
      suppressErrorToast: true,
    });
    return snapshotFromRows((data.rows as MetricRow[]) ?? []);
  } catch {
    return {};
  }
}
