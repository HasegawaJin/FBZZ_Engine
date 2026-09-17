import { defineConfig } from 'vite';
import { CreateViteBuildOptions, CreateViteDefine } from './build.config';

// mainプロセスもrendererと同じ構成名でソースマップと圧縮方針を統一する。
export default defineConfig({ build: CreateViteBuildOptions(), define: CreateViteDefine() });
