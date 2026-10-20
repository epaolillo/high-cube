import type { ThemeConfig } from 'antd';

export const stripeTheme: ThemeConfig = {
  token: {
    colorPrimary: '#635BFF',
    colorInfo: '#635BFF',
    colorSuccess: '#09825D',
    colorWarning: '#B76E00',
    colorError: '#DF1B41',
    colorText: '#0A2540',
    colorTextSecondary: '#425466',
    colorTextTertiary: '#6B7C93',
    colorBorder: '#E3E8EE',
    colorBgLayout: '#F6F9FC',
    colorBgContainer: '#FFFFFF',
    borderRadius: 6,
    borderRadiusLG: 8,
    fontFamily: 'Inter, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif',
    fontSize: 14,
    controlHeight: 40,
    boxShadow: '0 2px 5px rgba(50, 50, 93, 0.1), 0 1px 1px rgba(0, 0, 0, 0.07)',
    boxShadowSecondary: '0 7px 14px rgba(50, 50, 93, 0.1), 0 3px 6px rgba(0, 0, 0, 0.08)',
  },
  components: {
    Button: { primaryShadow: 'none', fontWeight: 500, borderRadius: 6 },
    Layout: { siderBg: '#FFFFFF', headerBg: '#FFFFFF', bodyBg: '#F6F9FC' },
    Menu: {
      itemBg: '#FFFFFF',
      itemColor: '#425466',
      itemSelectedBg: 'rgba(99, 91, 255, 0.08)',
      itemSelectedColor: '#635BFF',
      itemHoverBg: '#F6F9FC',
      itemHoverColor: '#0A2540',
      itemBorderRadius: 6,
    },
    Table: {
      headerBg: '#FFFFFF',
      headerColor: '#6B7C93',
      headerSplitColor: 'transparent',
      rowHoverBg: '#F6F9FC',
      borderColor: '#E3E8EE',
    },
    Input: {
      activeBorderColor: '#635BFF',
      hoverBorderColor: '#A0AEC0',
      activeShadow: '0 0 0 3px rgba(99, 91, 255, 0.25)',
    },
  },
};
