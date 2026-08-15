import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from './busClient.js';
import type { PermissionMode } from './config.js';
export declare function CreateEditorMcpServer(bus: EditorBus, permission: PermissionMode): McpServer;
