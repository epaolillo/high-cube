import { Alert, Button, Modal, Result, Spin, Steps, Typography } from 'antd';
import dayjs from 'dayjs';
import { useCallback, useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import './vpn.css';

type Enrollment = { id: number; command: string; expiresAt: string };
type EnrollmentStatus = { status: 'enrolling' | 'active' | 'expired'; name?: string; address?: string };

export function DeviceEnrollModal({ open, onClose }: { open: boolean; onClose: () => void }) {
  const { t } = useTranslation();
  const [enrollment, setEnrollment] = useState<Enrollment>();
  const [status, setStatus] = useState<EnrollmentStatus>();
  const [error, setError] = useState<string>();

  const start = useCallback(async () => {
    setEnrollment(undefined);
    setStatus(undefined);
    setError(undefined);
    try {
      const { data } = await api.post<Enrollment>('/vpn/enrollments', {}, { suppressErrorToast: true });
      setEnrollment(data);
    } catch (failure: unknown) {
      const response = (failure as { response?: { data?: { error?: string } } }).response;
      setError(response?.data?.error || t('operationFailed'));
    }
  }, [t]);

  useEffect(() => {
    if (open) void start();
  }, [open, start]);

  // Waits for the pasted command to register this machine.
  useEffect(() => {
    if (!open || !enrollment || status?.status === 'active') return undefined;
    const timer = window.setInterval(() => {
      api
        .get<EnrollmentStatus>(`/vpn/enrollments/${enrollment.id}`, { suppressErrorToast: true })
        .then(({ data }) => setStatus(data))
        .catch(() => undefined);
    }, 2000);
    return () => window.clearInterval(timer);
  }, [open, enrollment, status?.status]);

  const done = status?.status === 'active';
  const expired = status?.status === 'expired';

  return (
    <Modal
      title={t('vpnAddThisPc')}
      open={open}
      onCancel={onClose}
      width={680}
      destroyOnClose
      footer={
        done ? (
          <Button type="primary" onClick={onClose}>
            {t('vpnDone')}
          </Button>
        ) : (
          <Button onClick={onClose}>{t('cancel')}</Button>
        )
      }
    >
      {error ? (
        <Alert type="warning" showIcon message={error} />
      ) : done ? (
        <Result
          status="success"
          title={t('vpnPcJoined', { name: status?.name })}
          subTitle={t('vpnPcJoinedBody', { address: status?.address })}
        />
      ) : !enrollment ? (
        <div style={{ textAlign: 'center', padding: 32 }}>
          <Spin />
        </div>
      ) : (
        <>
          <Typography.Paragraph type="secondary">{t('vpnEnrollIntro')}</Typography.Paragraph>
          <Steps
            direction="vertical"
            size="small"
            current={1}
            items={[
              { title: t('vpnEnrollStep1'), description: t('vpnEnrollStep1Body') },
              {
                title: t('vpnEnrollStep2'),
                description: (
                  <div className="hc-vpn-command">
                    <Typography.Text copyable={{ text: enrollment.command }} style={{ color: 'inherit' }}>
                      {enrollment.command}
                    </Typography.Text>
                  </div>
                ),
              },
              {
                title: t('vpnEnrollStep3'),
                description: expired ? (
                  <span>
                    {t('vpnEnrollExpired')}{' '}
                    <Button size="small" onClick={() => void start()}>
                      {t('vpnEnrollNew')}
                    </Button>
                  </span>
                ) : (
                  <span>
                    <Spin size="small" /> {t('vpnEnrollWaiting')}
                  </span>
                ),
              },
            ]}
          />
          <Typography.Text type="secondary" style={{ fontSize: 12 }}>
            {t('vpnEnrollExpires', { time: dayjs(enrollment.expiresAt).format('HH:mm') })}
          </Typography.Text>
        </>
      )}
    </Modal>
  );
}
