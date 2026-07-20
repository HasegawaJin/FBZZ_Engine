import { defineConfig } from 'vite';
import { CreateViteBuildOptions } from './build.config';

// rendererの生成物を構成別に最適化し、Debugではインラインソースマップを利用する。
export default defineConfig({ build: CreateViteBuildOptions() });
