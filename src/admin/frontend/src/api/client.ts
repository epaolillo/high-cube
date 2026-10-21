import axios from 'axios';
import { message } from 'antd';
import i18n from '../i18n';

declare module 'axios' {
  // Lets background pollers silence the global error toast per request.
  export interface AxiosRequestConfig {
    suppressErrorToast?: boolean;
  }
}

const api = axios.create({
  baseURL: '/api',
});

api.interceptors.request.use((config) => {
  const token = localStorage.getItem('hc_token');
  if (token) {
    config.headers.Authorization = `Bearer ${token}`;
  }
  return config;
});

const TRANSIENT_TOAST_MS = 8000;
let lastTransientToastAt = 0;

function isTransientFailure(error: { response?: { status?: number } }) {
  const status = error.response?.status;
  return status === undefined || status === 502 || status === 503 || status === 504;
}

api.interceptors.response.use(
  (response) => response,
  (error) => {
    const text = error.response?.data?.error || i18n.t('operationFailed');
    // Background pollers can opt out of the global error toast to avoid spam.
    const suppressToast = Boolean(error.config?.suppressErrorToast);
    if (error.response?.status === 401) {
      localStorage.removeItem('hc_token');
      if (!window.location.pathname.includes('/login')) {
        window.location.href = '/login';
      }
    } else if (suppressToast) {
      /* silent: handled by the caller */
    } else if (isTransientFailure(error)) {
      const now = Date.now();
      if (now - lastTransientToastAt >= TRANSIENT_TOAST_MS) {
        lastTransientToastAt = now;
        message.error(i18n.t('controlPlaneUnavailable'));
      }
    } else {
      message.error(text);
    }
    return Promise.reject(error);
  },
);

export default api;

export type ListParams = {
  page?: number;
  pageSize?: number;
  sort?: string;
  order?: 'asc' | 'desc';
  [key: string]: string | number | undefined;
};

export type ListResponse<T> = {
  rows: T[];
  total: number;
};
