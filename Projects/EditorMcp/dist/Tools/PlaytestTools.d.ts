import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
export declare function RegisterPlaytestQueryTools(server: McpServer, bus: EditorBus): void;
export declare function RegisterPlaytestCommandTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void;
