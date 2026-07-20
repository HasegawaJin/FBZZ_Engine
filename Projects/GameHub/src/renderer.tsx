// FBZZ GameHub
// renderer.tsx | renderer
// React UIをDOMへマウントするrendererエントリポイント

import React from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './ui/App';
import './index.css';

const root = document.getElementById('root');
if (!root) throw new Error('React root elementが見つかりません。');

createRoot(root).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
