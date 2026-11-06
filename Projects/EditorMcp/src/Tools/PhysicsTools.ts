/// @file    PhysicsTools.ts
/// @brief   物理照会の読取専用 MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from '../busClient.js';
import { Vec3Schema } from '../editorContracts.js';
import { TextResult, Safely } from './ToolSupport.js';

export function RegisterPhysicsQueryTools(server: McpServer, bus: EditorBus): void {
    server.registerTool('physics_raycast', {
        description: '物理 World へ Raycast または SphereCast を行い、ヒット位置・法線・距離・NodeId を数値で返します。',
        inputSchema: {
            origin: Vec3Schema,
            direction: Vec3Schema.describe('正規化前でも可。ゼロベクトルは不可'),
            maxDistance: z.number().finite().positive().max(100000),
            sphereRadius: z.number().finite().positive().max(10000).optional(),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ origin, direction, maxDistance, sphereRadius }) => Safely(async () => TextResult(await bus.Query({
        t: 'physics.raycast', origin, direction, maxDistance,
        ...(sphereRadius === undefined ? {} : { sphereRadius }),
    }))));

    server.registerTool('physics_overlap_sphere', {
        description: '指定球と重なる Collider を検索し、対応する NodeId 一覧を返します。',
        inputSchema: { center: Vec3Schema, radius: z.number().finite().positive().max(10000) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ center, radius }) => Safely(async () => TextResult(await bus.Query({
        t: 'physics.overlapSphere', center, radius,
    }))));

    server.registerTool('physics_get_events', {
        description: '直近物理フレームのCollision/Trigger enter・stay・exitをNodeId付きで取得します。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'physics.events' }))));
}
