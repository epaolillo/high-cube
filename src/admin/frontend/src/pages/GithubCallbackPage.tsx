import { Spin, Typography, message } from 'antd';
import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate, useParams, useSearchParams } from 'react-router-dom';
import api from '../api/client';

export default function GithubCallbackPage() {
  const { id } = useParams();
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const { t } = useTranslation();
  const ran = useRef(false);

  useEffect(() => {
    if (ran.current || !id) return;
    ran.current = true;
    const code = params.get('code');
    const state = sessionStorage.getItem(`hc_github_state_${id}`) || '';
    if (!code) {
      message.error(t('githubCallbackMissingCode'));
      navigate('/');
      return;
    }
    api
      .post(`/github-apps/${id}/manifest/complete`, { code, state })
      .then(({ data }) => {
        sessionStorage.removeItem(`hc_github_state_${id}`);
        message.success(t('githubAppRegistered'));
        if (data.installUrl) {
          window.location.href = data.installUrl;
        } else {
          navigate('/');
        }
      })
      .catch(() => navigate('/'));
  }, [id, params, navigate, t]);

  return (
    <div style={{ padding: 48, textAlign: 'center' }}>
      <Spin size="large" />
      <Typography.Paragraph style={{ marginTop: 16 }}>{t('completingGithubApp')}</Typography.Paragraph>
    </div>
  );
}
