import Swal from 'sweetalert2';
import type { TFunction } from 'i18next';

export async function confirmRedeploy(t: TFunction): Promise<boolean> {
  const result = await Swal.fire({
    title: t('redeployAfterSaveTitle'),
    text: t('redeployAfterSaveBody'),
    icon: 'question',
    showCancelButton: true,
    confirmButtonText: t('redeploy'),
    cancelButtonText: t('redeployAfterSaveLater'),
    confirmButtonColor: '#635BFF',
    cancelButtonColor: '#8898AA',
    focusCancel: true,
  });
  return result.isConfirmed;
}
