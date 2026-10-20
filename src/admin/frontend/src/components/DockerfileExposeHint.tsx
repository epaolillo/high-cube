import { Space, Tag, Typography } from 'antd';
import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';

type Props = {
  source?: string;
  githubAppId?: number;
  repo?: string;
  repoUrl?: string;
  branch?: string;
  baseDirectory?: string;
  dockerfilePath?: string;
};

function repoDockerfilePath(baseDirectory?: string, dockerfilePath?: string) {
  const base = (baseDirectory || '/').replace(/^\/+|\/+$/g, '');
  const file = (dockerfilePath || 'Dockerfile').replace(/^\/+/, '');
  if (!base || base === '.') {
    return file || 'Dockerfile';
  }
  return `${base}/${file}`;
}

// Indication only. These ports are never written into the Port expose field.
export function DockerfileExposeHint({
  source,
  githubAppId,
  repo,
  repoUrl,
  branch,
  baseDirectory,
  dockerfilePath,
}: Props) {
  const { t } = useTranslation();
  const [state, setState] = useState<'idle' | 'loading' | 'ready'>('idle');
  const [found, setFound] = useState(false);
  const [reason, setReason] = useState('');
  const [ports, setPorts] = useState<number[]>([]);

  const path = repoDockerfilePath(baseDirectory, dockerfilePath);
  const ready =
    Boolean(branch) &&
    (source === 'github_app' ? Boolean(githubAppId && repo) : source === 'public' ? Boolean(repoUrl) : false);

  useEffect(() => {
    if (!ready) {
      setState('idle');
      setPorts([]);
      return;
    }
    const handle = window.setTimeout(() => {
      setState('loading');
      const params =
        source === 'github_app'
          ? { githubAppId, repo, branch, path }
          : { repoUrl, branch, path };
      api
        .get('/dockerfile-ports', { params })
        .then(({ data }) => {
          setFound(Boolean(data.found));
          setReason(data.reason || '');
          setPorts(Array.isArray(data.ports) ? data.ports : []);
          setState('ready');
        })
        .catch(() => setState('idle'));
    }, 400);
    return () => window.clearTimeout(handle);
  }, [ready, source, githubAppId, repo, repoUrl, branch, path]);

  if (!ready || state === 'idle') {
    return null;
  }
  if (state === 'loading') {
    return <Typography.Text type="secondary">{t('dockerfilePortsLoading')}</Typography.Text>;
  }
  if (!found) {
    if (reason === 'unsupported') {
      return null;
    }
    return <Typography.Text type="secondary">{t('dockerfileNotFound')}</Typography.Text>;
  }

  return (
    <div>
      {ports.length > 0 ? (
        <Space size={[6, 6]} wrap align="center">
          <Typography.Text type="secondary" style={{ fontSize: 12 }}>
            {t('dockerfileExposesLabel')}
          </Typography.Text>
          {ports.map((port) => (
            <Tag key={port} color="blue" style={{ marginInlineEnd: 0 }}>
              {port}
            </Tag>
          ))}
        </Space>
      ) : (
        <Typography.Text type="secondary" style={{ fontSize: 12 }}>
          {t('dockerfileNoExpose')}
        </Typography.Text>
      )}
      <div style={{ marginTop: 4 }}>
        <Typography.Text type="secondary" style={{ fontSize: 12 }}>
          {t('dockerfileExposeReference')}
        </Typography.Text>
      </div>
    </div>
  );
}
