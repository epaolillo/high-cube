import { Modal } from 'antd';
import type { TFunction } from 'i18next';

export function confirmRegenerateToken(t: TFunction, serverName: string, onConfirm: () => Promise<void>) {
  Modal.confirm({
    title: t('regenerateTokenConfirmTitle'),
    content: t('regenerateTokenConfirmBody', { name: serverName }),
    okText: t('regenerateTokenConfirmOk'),
    okButtonProps: { danger: true },
    cancelText: t('cancel'),
    autoFocusButton: 'cancel',
    onOk: onConfirm,
  });
}
