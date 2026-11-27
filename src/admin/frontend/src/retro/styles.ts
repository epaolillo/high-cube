import type { CSSProperties } from 'react';
import pkg from '../../package.json';

/** Screwed cream metal panel, for sections built with inline styles. Same look as `.rt-panel`. */
export const retroCard: CSSProperties = {
  border: '6px solid transparent',
  borderImage: 'url(/retro/frames/frame-panel.png) 32 / 16px stretch',
  borderRadius: 10,
  background: 'url(/retro/textures/tex-grain.png) repeat, var(--rt-cream)',
  backgroundClip: 'padding-box',
  boxShadow: '0 1px 0 rgba(0, 0, 0, 0.25), 0 4px 10px rgba(40, 24, 8, 0.28)',
  padding: '12px 16px',
};

/** Dark phosphor glass in a screwed bezel. Same look as `.rt-bezel`. */
export const retroScreen: CSSProperties = {
  border: '8px solid transparent',
  borderImage: 'url(/retro/frames/frame-screen-bezel.png) 32 / 16px stretch',
  borderRadius: 8,
  background: 'url(/retro/textures/scanlines.png) repeat 0 0 / 2px 2px, var(--rt-lcd-bg)',
  backgroundClip: 'padding-box',
  color: 'var(--rt-lcd-text)',
};

/** xterm.js colors matching the phosphor screens. */
export const retroTerminalTheme = {
  background: '#121a0e',
  foreground: '#a8f57c',
  cursor: '#8ff55a',
  selectionBackground: 'rgba(143, 245, 90, 0.3)',
};

/** G2 charts drawn on a phosphor screen: dark theme over the transparent glass. */
export const retroChartTheme = { type: 'classicDark', view: { viewFill: 'transparent' } };

/** G2 axis styling to spread into each axis channel. */
export const retroAxisStyle = {
  labelFill: '#7fbf5c',
  labelFontFamily: 'VT323, monospace',
  labelFontSize: 14,
  labelFillOpacity: 1,
  titleFill: '#7fbf5c',
  gridStroke: '#8ff55a',
  gridStrokeOpacity: 0.14,
  gridLineDash: [4, 6],
  lineStroke: '#8ff55a',
  lineStrokeOpacity: 0.4,
  tickStroke: '#8ff55a',
  tickStrokeOpacity: 0.4,
};

/** Phosphor and lamp colors for multi-series charts. */
export const RETRO_PALETTE = [
  '#8FF55A',
  '#FFB347',
  '#FF7A5A',
  '#C8FFA8',
  '#F5D76E',
  '#5FD3A0',
  '#E98AD8',
  '#9BC7FF',
  '#D9A066',
  '#B6E35A',
];

export const PANEL_LABEL = `HC-CP v${pkg.version.split('.').slice(0, 2).join('.')}`;
