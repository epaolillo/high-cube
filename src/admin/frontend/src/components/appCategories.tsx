import {
  ApiOutlined,
  AppstoreOutlined,
  CloudServerOutlined,
  DashboardOutlined,
  DatabaseOutlined,
  RobotOutlined,
  SafetyCertificateOutlined,
} from '@ant-design/icons';
import type { ReactNode } from 'react';

// Application categories. "other" is the default bucket for unclassified apps.
export type AppCategory = 'other' | 'db' | 'storage' | 'ai_ml' | 'networking' | 'monitoring' | 'security';

export type AppCategoryDef = {
  value: AppCategory;
  /** i18n key for the human-readable label. */
  labelKey: string;
  icon: ReactNode;
};

export const APP_CATEGORIES: AppCategoryDef[] = [
  { value: 'other', labelKey: 'categoryOther', icon: <AppstoreOutlined /> },
  { value: 'db', labelKey: 'categoryDb', icon: <DatabaseOutlined /> },
  { value: 'storage', labelKey: 'categoryStorage', icon: <CloudServerOutlined /> },
  { value: 'ai_ml', labelKey: 'categoryAiMl', icon: <RobotOutlined /> },
  { value: 'networking', labelKey: 'categoryNetworking', icon: <ApiOutlined /> },
  { value: 'monitoring', labelKey: 'categoryMonitoring', icon: <DashboardOutlined /> },
  { value: 'security', labelKey: 'categorySecurity', icon: <SafetyCertificateOutlined /> },
];

const BY_VALUE = new Map(APP_CATEGORIES.map((category) => [category.value, category]));

/** Resolves a category definition, falling back to "other" for unknown values. */
export function getAppCategory(value?: string): AppCategoryDef {
  return (value && BY_VALUE.get(value as AppCategory)) || APP_CATEGORIES[0];
}
