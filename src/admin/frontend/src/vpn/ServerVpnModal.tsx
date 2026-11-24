import { Alert, Badge, Button, Checkbox, Descriptions, Form, Input, InputNumber, Modal, Space, Typography, message } from 'antd';
import { useCallback, useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import api from '../api/client';
import { confirmDestructive } from '../components/confirmDestructive';
import { publicIpOf, type VpnState } from './types';
import './vpn.css';

type JoinValues = { listenPort?: number; endpoint?: string; hub?: boolean };

const SETUP_REQUIRED = 'setup-required';

// Run once on the server: installs the root helper hc-agent drives WireGuard through.
function setupCommand() {
  return `curl -fsSL '${window.location.origin}/api/vpn/agent-setup' | sudo bash`;
}

export function ServerVpnModal({
  serverId,
  serverName,
  open,
  onClose,
}: {
  serverId: number;
  serverName: string;
  open: boolean;
  onClose: () => void;
}) {
  const { t } = useTranslation();
  const [state, setState] = useState<VpnState>();
  const [busy, setBusy] = useState(false);
  const [form] = Form.useForm<JoinValues>();

  const load = useCallback(async (silent: boolean) => {
    const { data } = await api.get<VpnState>('/vpn', { suppressErrorToast: silent });
    setState(data);
    return data;
  }, []);

  useEffect(() => {
    if (!open) return undefined;
    let stopped = false;
    load(false)
      .then((data) => {
        if (stopped) return;
        const node = data.nodes.find((item) => item.serverId === serverId);
        form.setFieldsValue({
          listenPort: node?.listenPort ?? data.suggestedPort,
          endpoint: node?.endpoint ?? '',
          hub: !data.network?.hubNodeId,
        });
      })
      .catch(() => undefined);
    // Shows the server going from pending to connected while the agent sets up wg.
    const timer = window.setInterval(() => {
      if (!stopped) load(true).catch(() => undefined);
    }, 3000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [open, serverId, load, form]);

  const node = state?.nodes.find((item) => item.serverId === serverId);
  const hasHub = Boolean(state?.network?.hubNodeId);

  const run = async (action: () => Promise<void>) => {
    setBusy(true);
    try {
      await action();
      await load(true);
    } finally {
      setBusy(false);
    }
  };

  const join = () =>
    run(async () => {
      const values = await form.validateFields();
      await api.post(`/vpn/servers/${serverId}`, {
        listenPort: values.listenPort,
        endpoint: values.endpoint || '',
        hub: !hasHub || Boolean(values.hub),
      });
      message.success(t('vpnJoinStarted'));
    });

  const save = () =>
    run(async () => {
      if (!node) return;
      const values = await form.validateFields();
      await api.patch(`/vpn/nodes/${node.id}`, { listenPort: values.listenPort, endpoint: values.endpoint || '' });
      message.success(t('saved'));
    });

  const makeHub = () =>
    run(async () => {
      if (!node) return;
      await api.post(`/vpn/nodes/${node.id}/hub`);
      message.success(t('vpnHubChanged'));
    });

  const leave = async () => {
    if (!node) return;
    if (!(await confirmDestructive(t, { text: t('vpnRemoveNodeBody', { name: serverName }), confirmText: t('vpnLeave') })))
      return;
    await run(async () => {
      await api.delete(`/vpn/nodes/${node.id}`);
      message.success(t('vpnNodeRemoved'));
    });
  };

  const statusBadge = () => {
    if (!node) return null;
    if (node.error === SETUP_REQUIRED) return <Badge status="warning" text={t('vpnSetupRequired')} />;
    if (node.status !== 'active') {
      return node.agentConnected ? (
        <Badge status="processing" text={t('vpnSettingUp')} />
      ) : (
        <Badge status="warning" text={t('vpnWaitingAgent')} />
      );
    }
    if (node.error) return <Badge status="error" text={node.error} />;
    return node.connected ? (
      <Badge status="success" text={t('vpnConnected')} />
    ) : (
      <Badge status="error" text={t('vpnDisconnected')} />
    );
  };

  const fields = (
    <Form form={form} layout="vertical">
      <Form.Item
        name="listenPort"
        label={t('vpnListenPort')}
        extra={t('vpnListenPortHint')}
        rules={[{ required: true }]}
      >
        <InputNumber min={1024} max={65535} style={{ width: '100%' }} />
      </Form.Item>
      <Form.Item name="endpoint" label={t('vpnEndpoint')} extra={t('vpnEndpointHint')}>
        <Input placeholder={t('vpnEndpointAuto')} />
      </Form.Item>
      {!node && hasHub && (
        <Form.Item name="hub" valuePropName="checked">
          <Checkbox>{t('vpnUseAsHub')}</Checkbox>
        </Form.Item>
      )}
    </Form>
  );

  return (
    <Modal
      title={`${t('vpn')} · ${serverName}`}
      open={open}
      onCancel={onClose}
      width={600}
      destroyOnClose
      footer={
        node ? (
          <Space>
            <Button type="primary" danger onClick={leave} loading={busy}>
              {t('vpnLeave')}
            </Button>
            {!node.isHub && (
              <Button onClick={makeHub} loading={busy}>
                {t('vpnMakeHub')}
              </Button>
            )}
            <Button type="primary" onClick={save} loading={busy}>
              {t('save')}
            </Button>
          </Space>
        ) : (
          <Space>
            <Button onClick={onClose}>{t('cancel')}</Button>
            <Button type="primary" onClick={join} loading={busy} disabled={!state}>
              {t('vpnJoin')}
            </Button>
          </Space>
        )
      }
    >
      {!state ? null : node ? (
        <>
          {node.error === SETUP_REQUIRED && (
            <Alert
              type="warning"
              showIcon
              style={{ marginBottom: 16 }}
              message={t('vpnSetupTitle')}
              description={
                <>
                  <Typography.Paragraph style={{ marginBottom: 8 }}>{t('vpnSetupBody')}</Typography.Paragraph>
                  <div className="hc-vpn-command">
                    <Typography.Text copyable={{ text: setupCommand() }} style={{ color: 'inherit' }}>
                      {setupCommand()}
                    </Typography.Text>
                  </div>
                </>
              }
            />
          )}
          <Descriptions column={1} size="small" bordered style={{ marginBottom: 16 }}>
            <Descriptions.Item label={t('status')}>{statusBadge()}</Descriptions.Item>
            <Descriptions.Item label={t('vpnRole')}>{node.isHub ? t('vpnHub') : t('vpnMember')}</Descriptions.Item>
            <Descriptions.Item label={t('vpnAddress')}>
              <Typography.Text code copyable>
                {node.address}
              </Typography.Text>
            </Descriptions.Item>
            <Descriptions.Item label={t('vpnLanIp')}>{node.lanIp || '—'}</Descriptions.Item>
            <Descriptions.Item label={t('vpnPublicIp')}>{publicIpOf(node.observedIp) || '—'}</Descriptions.Item>
            <Descriptions.Item label={t('vpnPublicKey')}>
              {node.publicKey ? (
                <Typography.Text code copyable style={{ wordBreak: 'break-all' }}>
                  {node.publicKey}
                </Typography.Text>
              ) : (
                '—'
              )}
            </Descriptions.Item>
          </Descriptions>
          {fields}
        </>
      ) : (
        <>
          <Typography.Paragraph type="secondary">{t('vpnJoinIntro')}</Typography.Paragraph>
          {!hasHub && (
            <Alert type="info" showIcon style={{ marginBottom: 16 }} message={t('vpnFirstIsHub')} />
          )}
          {fields}
        </>
      )}
    </Modal>
  );
}
