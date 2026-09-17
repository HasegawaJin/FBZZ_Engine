import { defineConfig } from 'vite';
import { CreateViteBuildOptions, CreateViteDefine } from './build.config';

// preload境界の調査性をDebug・Developmentで保ち、Releaseでは出力を最小化する。
export default defineConfig({ build: CreateViteBuildOptions(), define: CreateViteDefine() });
