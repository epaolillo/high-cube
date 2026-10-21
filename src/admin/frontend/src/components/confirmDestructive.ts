import Swal from 'sweetalert2';
import type { TFunction } from 'i18next';

type ConfirmOptions = {
  /** Title shown in the dialog. Defaults to a generic "are you sure?". */
  title?: string;
  /** Body text. Defaults to a generic warning. */
  text?: string;
  /** Label for the confirm button. Defaults to the shared "delete" label. */
  confirmText?: string;
  /** Red confirm button. Set false for non-destructive confirmations. */
  danger?: boolean;
};

// Shared confirmation dialog for any destructive action (delete, remove, stop,
// etc.) across every entity in the system. Returns true when the user confirms.
export async function confirmDestructive(t: TFunction, options: ConfirmOptions = {}): Promise<boolean> {
  const result = await Swal.fire({
    title: options.title ?? t('confirmDeleteTitle'),
    text: options.text ?? t('confirmDeleteBody'),
    icon: options.danger === false ? 'question' : 'warning',
    showCancelButton: true,
    confirmButtonText: options.confirmText ?? t('delete'),
    cancelButtonText: t('cancel'),
    confirmButtonColor: options.danger === false ? '#635BFF' : '#E5484D',
    cancelButtonColor: '#8898AA',
    focusCancel: true,
  });
  return result.isConfirmed;
}
