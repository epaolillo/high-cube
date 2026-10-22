import { Form, Input, Typography } from 'antd';
import { useTranslation } from 'react-i18next';

// Same root as kVolumesRoot on the admin, which refuses any host directory outside it.
export const VOLUMES_ROOT = '/var/lib/data';

const SEGMENT = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
export const VOLUME_NAME_PATTERN = SEGMENT;

// "my-app/data/" or "/var/lib/data/my-app/data" -> "my-app/data"; null when the backend would refuse it.
export function normalizeHostPath(value: string | undefined): string | null {
  let path = (value || '').trim();
  if (path === VOLUMES_ROOT || path.startsWith(`${VOLUMES_ROOT}/`)) {
    path = path.slice(VOLUMES_ROOT.length);
  } else if (path.startsWith('/')) {
    return null;
  }
  const segments = path.split('/').filter(Boolean);
  if (!segments.length || segments.length > 8 || !segments.every((segment) => SEGMENT.test(segment))) {
    return null;
  }
  return segments.join('/');
}

// Same directory, or one nested inside the other.
export function hostPathsOverlap(a: string, b: string): boolean {
  return a === b || a.startsWith(`${b}/`) || b.startsWith(`${a}/`);
}

function slug(value: string): string {
  return (
    value
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '')
      .slice(0, 48) || 'app'
  );
}

// One folder per application, one subfolder per volume.
export function suggestHostPath(appName: string | undefined, volumeName: string | undefined): string {
  return `${slug(appName || '')}/${slug(volumeName || 'data')}`;
}

type Props = {
  // Field name inside a Form.List; omitted when the form edits a single volume.
  field?: number;
  listName?: string;
  // Editing a saved volume: its host directory is shown but cannot change.
  saved?: boolean;
  savedHostDirectory?: string | null;
  // Host paths of the other volumes in the same form, checked for overlaps.
  otherHostPaths?: () => string[];
};

export function VolumeFields({ field, listName, saved, savedHostDirectory, otherHostPaths }: Props) {
  const { t } = useTranslation();
  const name = (key: string) => (field === undefined ? [key] : [field, key]);
  const fullName = (key: string) =>
    field === undefined || !listName ? [key] : [listName, field, key];

  return (
    <>
      <Form.Item
        name={name('name')}
        label={t('volumeName')}
        rules={[
          { required: true, message: t('volumeNameInvalid') },
          { pattern: VOLUME_NAME_PATTERN, message: t('volumeNameInvalid') },
        ]}
      >
        <Input placeholder="data" style={{ maxWidth: 240 }} />
      </Form.Item>
      {saved ? (
        <Form.Item
          label={t('volumeHostDirectory')}
          extra={savedHostDirectory ? t('volumeHostDirectoryLocked') : t('volumeDockerManagedHint')}
        >
          <Input value={savedHostDirectory || t('volumeDockerManaged')} disabled />
        </Form.Item>
      ) : (
        <Form.Item
          name={name('hostPath')}
          label={t('volumeHostDirectory')}
          extra={t('volumeHostDirectoryHelp')}
          rules={[
            { required: true, message: t('volumeHostPathInvalid') },
            {
              validator: async (_, value: string) => {
                if (!value) return;
                const normalized = normalizeHostPath(value);
                if (!normalized) throw new Error(t('volumeHostPathInvalid'));
                if ((otherHostPaths?.() || []).some((other) => hostPathsOverlap(normalized, other))) {
                  throw new Error(t('volumeHostPathOverlap'));
                }
              },
            },
          ]}
        >
          <Input addonBefore={`${VOLUMES_ROOT}/`} placeholder="my-app/data" />
        </Form.Item>
      )}
      <Form.Item
        name={name('mountPath')}
        label={t('volumeContainerDirectory')}
        extra={t('volumeContainerDirectoryHelp')}
        rules={[
          { required: true, message: t('volumeMountPathInvalid') },
          { pattern: /^\/(?!\/*$)[^:,]*$/, message: t('volumeMountPathInvalid') },
        ]}
      >
        <Input placeholder="/var/lib/postgresql/data" />
      </Form.Item>
      <Form.Item noStyle shouldUpdate>
        {({ getFieldValue }) => {
          const hostPath = normalizeHostPath(getFieldValue(fullName('hostPath')));
          const host = saved
            ? savedHostDirectory || t('volumeDockerManaged')
            : hostPath
              ? `${VOLUMES_ROOT}/${hostPath}`
              : '…';
          const container = String(getFieldValue(fullName('mountPath')) || '').trim() || '…';
          return (
            <Typography.Text type="secondary" style={{ display: 'block', marginBottom: 8 }}>
              {t('volumeMountPreview', { host, container })}
            </Typography.Text>
          );
        }}
      </Form.Item>
    </>
  );
}
