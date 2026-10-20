import { theme as antdTheme } from 'antd';
import type { ThemeConfig } from 'antd';

export type ThemeMode = 'light' | 'dark';

// Pixel UI font stack. Pixelify Sans stays legible at small sizes for dense data,
// Silkscreen is reserved for labels and display text via CSS.
const PIXEL_FONT =
  '"Pixelify Sans", "Silkscreen", ui-monospace, "Cascadia Mono", Menlo, monospace';

// High-contrast palettes. Light is the default skin; dark mirrors the on-screen
// resource widget (near-black panel, phosphor-green accent).
const palettes: Record<ThemeMode, {
  primary: string;
  info: string;
  success: string;
  warning: string;
  error: string;
  text: string;
  textSecondary: string;
  textTertiary: string;
  border: string;
  borderSecondary: string;
  bgLayout: string;
  bgContainer: string;
  bgElevated: string;
  shadow: string;
}> = {
  light: {
    primary: '#00a63e',
    info: '#00a63e',
    success: '#09825d',
    warning: '#b45309',
    error: '#c81e3a',
    text: '#0a0f14',
    textSecondary: '#3f4e5a',
    textTertiary: '#64757f',
    border: '#1b2730',
    borderSecondary: '#c6cfd6',
    bgLayout: '#eef1f4',
    bgContainer: '#ffffff',
    bgElevated: '#ffffff',
    shadow: '3px 3px 0 rgba(10, 15, 20, 0.85)',
  },
  dark: {
    primary: '#2bf59a',
    info: '#2bf59a',
    success: '#2bf59a',
    warning: '#ffb224',
    error: '#ff5c7a',
    text: '#dcecf9',
    textSecondary: '#9fbdd2',
    textTertiary: '#6f8ea6',
    border: '#1f4a66',
    borderSecondary: '#13324d',
    bgLayout: '#060f1b',
    bgContainer: '#0b1826',
    bgElevated: '#102236',
    shadow: '3px 3px 0 rgba(0, 0, 0, 0.6)',
  },
};

export function makeTheme(mode: ThemeMode): ThemeConfig {
  const p = palettes[mode];
  return {
    algorithm: mode === 'dark' ? antdTheme.darkAlgorithm : antdTheme.defaultAlgorithm,
    // Crisp, flat, bordered look suited to the pixel aesthetic.
    token: {
      colorPrimary: p.primary,
      colorInfo: p.info,
      colorSuccess: p.success,
      colorWarning: p.warning,
      colorError: p.error,
      colorText: p.text,
      colorTextSecondary: p.textSecondary,
      colorTextTertiary: p.textTertiary,
      colorBorder: p.border,
      colorBorderSecondary: p.borderSecondary,
      colorBgLayout: p.bgLayout,
      colorBgContainer: p.bgContainer,
      colorBgElevated: p.bgElevated,
      // Zero radius everywhere for hard pixel corners.
      borderRadius: 0,
      borderRadiusLG: 0,
      borderRadiusSM: 0,
      borderRadiusXS: 0,
      borderRadiusOuter: 0,
      fontFamily: PIXEL_FONT,
      fontSize: 13,
      lineHeight: 1.45,
      controlHeight: 36,
      wireframe: true,
      boxShadow: p.shadow,
      boxShadowSecondary: p.shadow,
      boxShadowTertiary: p.shadow,
    },
    components: {
      Button: { primaryShadow: 'none', fontWeight: 600, borderRadius: 0, controlHeight: 36 },
      Layout: {
        siderBg: p.bgContainer,
        headerBg: p.bgContainer,
        bodyBg: p.bgLayout,
      },
      Menu: {
        itemBg: 'transparent',
        itemColor: p.textSecondary,
        itemSelectedBg: mode === 'dark' ? 'rgba(43, 245, 154, 0.14)' : 'rgba(0, 166, 62, 0.12)',
        itemSelectedColor: p.primary,
        itemHoverBg: p.bgLayout,
        itemHoverColor: p.text,
        itemBorderRadius: 0,
        itemHeight: 38,
      },
      Table: {
        headerBg: p.bgContainer,
        headerColor: p.textTertiary,
        headerSplitColor: 'transparent',
        rowHoverBg: p.bgLayout,
        borderColor: p.borderSecondary,
        cellPaddingBlock: 10,
      },
      Card: { borderRadiusLG: 0 },
      Input: { borderRadius: 0, activeBorderColor: p.primary, hoverBorderColor: p.primary },
      Select: { borderRadius: 0 },
      Modal: { borderRadiusLG: 0 },
      Tag: { borderRadiusSM: 0 },
      Tooltip: { borderRadius: 0 },
      Segmented: { borderRadius: 0, borderRadiusSM: 0 },
    },
  };
}

// Backwards-compatible default export used before the light/dark split.
export const stripeTheme = makeTheme('light');
