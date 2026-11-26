import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'https://admin.sectc.app',
        changeOrigin: true,
        secure: true,
        ws: true,
      },
    },
  },
  build: {
    outDir: 'dist',
  },
});
