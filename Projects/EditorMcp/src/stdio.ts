// FBZZ Engine
// stdio.ts | EditorMcp
// Claude Desktop / Claude Code がローカル起動する stdio MCP エントリ
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { EditorBusClient } from './busClient.js';
import { LoadConfig } from './config.js';
import { CreateEditorMcpServer } from './server.js';

// stdout は MCP JSON-RPC 専用とし、診断情報は必ず stderr へ送る。
async function Main(): Promise<void> {
    const config = LoadConfig();
    const bus = new EditorBusClient(config.busEndpoint, config.requestTimeoutMs);
    const server = CreateEditorMcpServer(bus, config.permission);
    const transport = new StdioServerTransport();
    const shutdown = async (): Promise<void> => {
        bus.Close();
        await server.close();
    };
    process.once('SIGINT', () => { void shutdown(); });
    process.once('SIGTERM', () => { void shutdown(); });
    console.error(`[FBZZ Editor MCP] local stdio / permission=${config.permission} / pipe=${config.busEndpoint}`);
    await server.connect(transport);
}

Main().catch((error) => {
    const message = error instanceof Error ? error.message : String(error);
    console.error(`[FBZZ Editor MCP] fatal: ${message}`);
    process.exitCode = 1;
});
