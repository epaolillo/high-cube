import type { ThemeConfig } from 'antd';

// Cassette-futurism palette: cream plastics, burnt orange keys, phosphor greens.
export const retroTheme: ThemeConfig = {
  token: {
    colorPrimary: '#E2621F',
    colorInfo: '#E2621F',
    colorSuccess: '#3D9A2C',
    colorWarning: '#D98A12',
    colorError: '#C2391A',
    colorText: '#2B2118',
    colorTextSecondary: '#5E5040',
    colorTextTertiary: '#8A7C66',
    colorBorder: '#CDBFA4',
    colorBgLayout: '#F1E0C9',
    colorBgContainer: '#FBF6EC',
    colorBgElevated: '#FBF6EC',
    borderRadius: 6,
    borderRadiusLG: 8,
    fontFamily: 'Archivo, "Helvetica Neue", Arial, sans-serif',
    fontSize: 14,
    controlHeight: 40,
    boxShadow: '0 2px 5px rgba(40, 24, 8, 0.18), 0 1px 1px rgba(0, 0, 0, 0.08)',
    boxShadowSecondary: '0 7px 14px rgba(40, 24, 8, 0.2), 0 3px 6px rgba(0, 0, 0, 0.1)',
  },
  components: {
    Button: { primaryShadow: '0 2px 0 rgba(94, 34, 8, 0.45)', fontWeight: 600, borderRadius: 6 },
    Layout: { siderBg: '#FBF6EC', headerBg: '#FBF6EC', bodyBg: '#F1E0C9' },
    Menu: {
      itemBg: '#FBF6EC',
      itemColor: '#5E5040',
      itemSelectedBg: 'rgba(226, 98, 31, 0.1)',
      itemSelectedColor: '#E2621F',
      itemHoverBg: '#F3EADB',
      itemHoverColor: '#2B2118',
      itemBorderRadius: 6,
    },
    Table: {
      headerBg: '#F3EADB',
      headerColor: '#6C5D49',
      headerSplitColor: 'transparent',
      rowHoverBg: '#F3EADB',
      borderColor: '#DCCFB5',
    },
    Input: {
      activeBorderColor: '#E2621F',
      hoverBorderColor: '#B8A684',
      activeShadow: '0 0 0 3px rgba(226, 98, 31, 0.2)',
    },
  },
};
