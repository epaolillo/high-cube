import { Spin, Typography, message } from 'antd';
import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate, useParams, useSearchParams } from 'react-router-dom';
import api from '../api/client';

export default function GithubSetupPage() {
  const { id } = useParams();
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const { t } = useTranslation();
  const ran = useRef(false);

  useEffect(() => {
    if (ran.current || !id) return;
    ran.current = true;
    const installationId = params.get('installation_id');
    const finish = async () => {
      try {
        if (installationId) {
          await api.post(`/github-apps/${id}/installation`, {
            installationId: Number(installationId),
          });
        } else {
          await api.post(`/github-apps/${id}/installation/sync`);
        }
        message.success(t('installationSynced'));
      } finally {
        navigate('/integrations');
      }
    };
    void finish();
  }, [id, params, navigate, t]);

  return (
    <div style={{ padding: 48, textAlign: 'center' }}>
      <Spin size="large" />
      <Typography.Paragraph style={{ marginTop: 16 }}>{t('savingInstallation')}</Typography.Paragraph>
    </div>
  );
}
