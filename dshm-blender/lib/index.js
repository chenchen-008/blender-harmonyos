/**
 * DSHM Blender plugin (host half). Pure JS: node:net + node:fs only.
 * No child_process, no undici, no WASM, no native modules (DSHM jitless host).
 *
 * Same-device requirement: keep Blender visible (split screen / floating
 * window). HarmonyOS freezes a backgrounded app's main thread, so Blender
 * would accept TCP but never execute bpy commands.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import PLAN_CATALOG from './plan-catalog.js';
import { defineTool } from '@deepseek-ai/dsh-tools';
import { call, ping, loadConfig, describe, BlenderError } from './protocol.js';

export const name = '@dsh-external/dshm-blender';
export const inject = ['tools'];

const VERSION = '0.1.0';
const HINT = '同机使用时请把 DSHM 与 Blender 分屏（或让 Blender 悬浮可见）；Blender 在后台会被鸿蒙冻结，命令会超时。';

function txt(value) {
  return String(value === undefined || value === null ? '' : value);
}

function clip(value, head, tail) {
  const s = txt(value);
  const h = head || 4000;
  const t = tail || 1200;
  if (s.length <= h + t) return s;
  return s.slice(0, h) + '\n...[' + String(s.length - h - t) + ' chars omitted]...\n' + s.slice(-t);
}

function renderTool(_args, value) {
  const blocks = [{ type: 'text', text: txt(value && value.text) }];
  if (value && value.image) blocks.push({ type: 'image', attachment: value.image });
  if (value && Array.isArray(value.images)) {
    for (const image of value.images) blocks.push({ type: 'image', attachment: image });
  }
  return blocks;
}

function fmtScene(info) {
  const lines = [];
  lines.push('Scene ' + txt(info.scene) + ' | ' + String(info.objects_total) + ' objects | frame ' + String(info.frame) + ' | ' + txt(info.engine));
  lines.push('file: ' + (info.filepath ? txt(info.filepath) : '(unsaved)'));
  lines.push('active: ' + txt(info.active_object || '(none)'));
  lines.push('collections: ' + (Array.isArray(info.collections) ? info.collections.join(', ') : ''));
  const objects = Array.isArray(info.objects) ? info.objects : [];
  for (const obj of objects) {
    lines.push('- ' + txt(obj.name) + ' [' + txt(obj.type) + '] loc=(' + (obj.location || []).join(',') + ') dim=(' + (obj.dimensions || []).join(',') + ')'
      + (obj.parent ? ' parent=' + txt(obj.parent) : '') + (obj.selected ? ' selected' : ''));
  }
  if (info.objects_total > objects.length) lines.push('... showing first ' + String(objects.length) + ' of ' + String(info.objects_total));
  return lines.join('\n');
}

function errText(error) {
  if (error instanceof BlenderError) {
    if (error.code === 'BLENDER_TIMEOUT' || error.code === 'BLENDER_UNREACHABLE') return error.message + '\n\n' + HINT;
    return error.message;
  }
  return String(error && error.message ? error.message : error);
}

export function apply(ctx, config) {
  const cfg = loadConfig(config || {});
  const leaseState = { holder: '', expiresAt: 0 };
  console.log('[dshm-blender] v' + VERSION + ' target ' + describe(cfg));

  async function attachPng(base64, fileBase) {
    const data = Buffer.from(base64, 'base64');
    const attachments = ctx.get('attachments');
    if (!attachments) return { why: 'host has no attachments service' };
    const mediaTypes = attachments.imageLimits && Array.isArray(attachments.imageLimits.mediaTypes)
      ? attachments.imageLimits.mediaTypes : [];
    if (mediaTypes.length > 0 && mediaTypes.indexOf('image/png') < 0) return { why: 'imageLimits excludes image/png' };
    try {
      const ref = await attachments.saveImage({ data, mediaType: 'image/png', name: fileBase + '.png' });
      const out = { attachmentId: ref.attachmentId, mediaType: ref.mediaType, bytes: ref.bytes, width: ref.width, height: ref.height };
      if (ref.name !== undefined) out.name = ref.name;
      return { ref: out };
    } catch (error) {
      return { why: 'saveImage failed: ' + txt(error && error.message ? error.message : error) };
    }
  }

  function saveWorkspace(exec, data, filename) {
    const cwd = exec && exec.agent && exec.agent.session && exec.agent.session.header
      ? exec.agent.session.header.cwd : undefined;
    const dir = cwd || os.tmpdir();
    try {
      fs.mkdirSync(dir, { recursive: true });
      const target = path.join(dir, filename);
      fs.writeFileSync(target, data);
      return target;
    } catch {
      return null;
    }
  }

  function register(id, spec) {
    const tool = defineTool(Object.assign({}, spec, {
      description: '[dshm-blender] ' + spec.description,
      output: { schema: { type: 'json' }, render: renderTool },
      isConcurrencySafe: () => false,
    }));
    ctx.effect(() => ctx.tools.register(tool), '@dsh-external/dshm-blender: ' + id);
  }

  register('status', {
    name: 'blender_status',
    description: '连接诊断（先调这个）。返回 kind=ok、Blender/Python 版本、端口与耗时；失败时给出可操作原因。',
    parameters: {},
    timeoutMs: 30000,
    async execute(_args, exec) {
      const t0 = Date.now();
      try {
        const info = await ping(cfg, { signal: exec && exec.signal });
        return { text: 'kind=ok | Blender ' + txt(info.blender) + ' | Python ' + txt(info.python)
          + ' | addon ' + txt(info.addon) + ' ' + txt(info.version) + ' | ' + describe(cfg)
          + ' | ' + String(Date.now() - t0) + 'ms\n' + HINT };
      } catch (error) {
        return { text: 'kind=error\n' + errText(error) };
      }
    },
  });

  register('scene', {
    name: 'blender_scene',
    description: '读取场景：对象列表、集合、活动对象、帧与渲染引擎。',
    parameters: { limit: { type: 'integer', description: 'max objects, default 100, max 500' } },
    timeoutMs: 30000,
    async execute(args, exec) {
      try {
        const limit = Math.max(1, Math.min(500, Number(args && args.limit) || 100));
        const info = await call(cfg, 'get_scene_info', { limit }, { signal: exec && exec.signal });
        return { text: fmtScene(info) };
      } catch (error) {
        return { text: 'blender_scene failed:\n' + errText(error) };
      }
    },
  });

  register('object', {
    name: 'blender_object',
    description: '读取单个对象的变换、尺寸、网格统计、材质、修改器与自定义属性。',
    parameters: { name: { type: 'string', description: 'object name, e.g. Cube' } },
    timeoutMs: 30000,
    async execute(args, exec) {
      const objectName = txt(args && args.name);
      if (!objectName) return { text: 'name is required' };
      try {
        const info = await call(cfg, 'get_object_info', { name: objectName }, { signal: exec && exec.signal });
        const mesh = info && info.mesh ? info.mesh : null;
        const lines = [];
        lines.push(txt(info.name) + ' [' + txt(info.type) + ']' + (info.selected ? ' selected' : ''));
        lines.push('loc=(' + (info.location || []).join(',') + ') rot=(' + (info.rotation_euler || []).join(',') + ') scale=(' + (info.scale || []).join(',') + ')');
        lines.push('dim=(' + (info.dimensions || []).join(',') + ') parent=' + txt(info.parent || '(none)'));
        if (mesh) lines.push('mesh: ' + String(mesh.vertices) + ' verts / ' + String(mesh.edges) + ' edges / ' + String(mesh.polygons) + ' polys | materials=' + (mesh.materials || []).join(', '));
        if (Array.isArray(info.modifiers) && info.modifiers.length) lines.push('modifiers: ' + info.modifiers.map(function (m) { return txt(m.name) + '(' + txt(m.type) + ')'; }).join(', '));
        lines.push('json: ' + clip(JSON.stringify(info), 3000, 1500));
        return { text: lines.join('\n') };
      } catch (error) {
        return { text: 'blender_object failed:\n' + errText(error) };
      }
    },
  });

  register('run', {
    name: 'blender_run',
    description: '在 Blender 主线程执行 Python（预置 bpy/math/mathutils/Vector 与持久内核 K），返回 stdout/stderr/traceback；默认同时回一帧视口。',
    parameters: {
      code: { type: 'string', description: 'Python code' },
      see: { type: 'boolean', description: 'capture a viewport frame, default true' },
      max_size: { type: 'integer', description: 'frame max edge, default 640' },
      timeout_ms: { type: 'integer', description: 'execution wait limit, default 120000' },
    },
    timeoutMs: 300000,
    async execute(args, exec) {
      const code = txt(args && args.code);
      if (!code.trim()) return { text: 'code is required' };
      const waitMs = Math.max(1000, Math.min(600000, Number(args && args.timeout_ms) || 120000));
      let result;
      try {
        result = await call(cfg, 'execute_code', { code }, {
          signal: exec && exec.signal,
          timeoutMs: waitMs + 5000,
          timeoutSec: Math.ceil(waitMs / 1000),
        });
      } catch (error) {
        return { text: 'blender_run failed:\n' + errText(error) };
      }
      const parts = [(result.executed ? 'executed=true' : 'executed=false') + ' | ' + String(result.ms || 0) + 'ms'];
      if (result.stdout) parts.push('stdout:\n' + clip(result.stdout, 3000, 1000));
      if (result.stderr) parts.push('stderr:\n' + clip(result.stderr, 2000, 800));
      if (result.traceback) parts.push('traceback:\n' + clip(result.traceback, 3000, 1200));
      const out = { text: parts.join('\n') };
      if (!(args && args.see === false)) {
        try {
          const size = Math.max(160, Math.min(1600, Number(args && args.max_size) || 640));
          const shot = await call(cfg, 'get_viewport_png', { max_size: size }, { signal: exec && exec.signal, timeoutMs: cfg.imageTimeoutMs });
          const attached = await attachPng(shot.png_base64, 'blender-' + String(Date.now()));
          if (attached.ref) {
            out.image = attached.ref;
            out.text += '\nviewport ' + String(shot.width) + 'x' + String(shot.height) + ' | ' + String(Math.round(shot.bytes / 1024)) + 'KB | ' + String(shot.ms) + 'ms';
          } else {
            const saved = saveWorkspace(exec, Buffer.from(shot.png_base64, 'base64'), 'blender-see-' + String(Date.now()) + '.png');
            out.text += '\nviewport captured but not attached: ' + txt(attached.why) + (saved ? ' | saved: ' + saved : '');
          }
        } catch (error) {
          out.text += '\nframe failed: ' + errText(error);
        }
      }
      return out;
    },
  });

  register('see', {
    name: 'blender_see',
    description: '抓取当前 3D 视口的一帧 PNG 并作为图片回传。',
    parameters: { max_size: { type: 'integer', description: 'max edge, default 640, 160-1600' } },
    timeoutMs: 120000,
    async execute(args, exec) {
      try {
        const size = Math.max(160, Math.min(1600, Number(args && args.max_size) || 640));
        const shot = await call(cfg, 'get_viewport_png', { max_size: size }, { signal: exec && exec.signal, timeoutMs: cfg.imageTimeoutMs });
        const attached = await attachPng(shot.png_base64, 'blender-see-' + String(Date.now()));
        const summary = 'viewport ' + String(shot.width) + 'x' + String(shot.height) + ' | ' + String(Math.round(shot.bytes / 1024)) + 'KB | ' + String(shot.ms) + 'ms';
        if (attached.ref) return { text: summary, image: attached.ref };
        const saved = saveWorkspace(exec, Buffer.from(shot.png_base64, 'base64'), 'blender-see-' + String(Date.now()) + '.png');
        return { text: summary + '\nnot attached: ' + txt(attached.why) + (saved ? ' | saved: ' + saved : '') };
      } catch (error) {
        return { text: 'blender_see failed:\n' + errText(error) };
      }
    },
  });

  register('cmd', {
    name: 'blender_cmd',
    description: '透传任意 addon 命令（先用 blender_commands 看清单）。',
    parameters: {
      command: { type: 'string', description: 'addon command name' },
      params: { type: 'object', description: 'command params', additionalProperties: true },
      timeout_ms: { type: 'integer', description: 'wait limit, default 60000' },
    },
    timeoutMs: 300000,
    async execute(args, exec) {
      const command = txt(args && args.command);
      if (!command) return { text: 'command is required' };
      const timeoutMs = Math.max(1000, Math.min(600000, Number(args && args.timeout_ms) || 60000));
      try {
        const result = await call(cfg, command, (args && args.params) || {}, { signal: exec && exec.signal, timeoutMs: timeoutMs + 5000, timeoutSec: Math.ceil(timeoutMs / 1000) });
        return { text: JSON.stringify(result) };
      } catch (error) {
        return { text: 'blender_cmd failed:\n' + errText(error) };
      }
    },
  });

  register('commands', {
    name: 'blender_commands',
    description: '列出 Blender addon 支持的命令与说明。',
    parameters: {},
    timeoutMs: 30000,
    async execute(_args, exec) {
      try {
        const result = await call(cfg, 'list_commands', {}, { signal: exec && exec.signal });
        const commands = Array.isArray(result.commands) ? result.commands : [];
        return { text: commands.map(function (c) { return '- ' + txt(c.name) + ' | ' + txt(c.description); }).join('\n') || '(empty)' };
      } catch (error) {
        return { text: 'blender_commands failed:\n' + errText(error) };
      }
    },
  });

  register('export', {
    name: 'blender_export',
    description: '导出场景：obj/stl/ply 在鸿蒙版可用；glb/gltf 需要 numpy（通常没有）。inline=true 时把字节写回 DSHM 工作区。',
    parameters: {
      format: { type: 'string', description: 'obj / stl / ply / glb / gltf / fbx, default obj' },
      filepath: { type: 'string', description: 'Blender-side path; omit for temp dir' },
      selected_only: { type: 'boolean', description: 'export selected only' },
      apply_modifiers: { type: 'boolean', description: 'apply modifiers, default true' },
      inline: { type: 'boolean', description: 'copy bytes back to the DSHM workspace (<=8MB)' },
    },
    timeoutMs: 180000,
    async execute(args, exec) {
      const format = (txt(args && args.format) || 'obj').toLowerCase();
      try {
        const result = await call(cfg, 'export_scene', {
          format: format,
          filepath: args && args.filepath ? txt(args.filepath) : '',
          selected_only: !!(args && args.selected_only),
          apply_modifiers: args && args.apply_modifiers === false ? false : true,
          inline: !!(args && args.inline),
        }, { signal: exec && exec.signal, timeoutMs: 180000 });
        const lines = ['export ' + txt(result.format) + ' | ' + String(result.bytes) + ' bytes | ' + String(result.ms) + 'ms', 'Blender path: ' + txt(result.path)];
        if (result.data_base64) {
          const target = saveWorkspace(exec, Buffer.from(result.data_base64, 'base64'), 'blender-export-' + String(Date.now()) + '.' + format);
          if (target) lines.push('DSHM workspace: ' + target);
          else lines.push('bytes returned, but no workspace cwd');
        }
        return { text: lines.join('\n') };
      } catch (error) {
        const message = errText(error);
        if (/numpy/i.test(message)) return { text: 'this Blender build has no numpy; glTF/GLB is unavailable. Use format=obj / stl / ply.\n\n' + message };
        return { text: 'blender_export failed:\n' + message };
      }
    },
  });

  function sleep(ms) {
    return new Promise(function (resolve) { setTimeout(resolve, ms); });
  }

  register('watch', {
    name: 'blender_watch',
    description: '时间窗观察：可选先跑一段 Python，然后在 seconds 秒内按 fps 采样视口，返回逐帧 hash 与最多 6 张不同画面。',
    parameters: {
      code: { type: 'string', description: 'optional Python to run before sampling' },
      seconds: { type: 'number', description: 'window seconds, default 2, range 0.2-20' },
      fps: { type: 'number', description: 'samples per second, default 4, range 0.5-8' },
      max_size: { type: 'integer', description: 'frame max edge, default 420' },
    },
    timeoutMs: 240000,
    async execute(args, exec) {
      const seconds = Math.max(0.2, Math.min(20, Number(args && args.seconds) || 2));
      const fps = Math.max(0.5, Math.min(8, Number(args && args.fps) || 4));
      const size = Math.max(160, Math.min(1600, Number(args && args.max_size) || 420));
      const parts = [];
      if (args && args.code) {
        try {
          const ran = await call(cfg, 'execute_code', { code: txt(args.code) }, { signal: exec && exec.signal, timeoutMs: 120000 });
          parts.push((ran.executed ? 'pre-run ok' : 'pre-run failed') + ' | ' + String(ran.ms || 0) + 'ms');
        } catch (error) {
          parts.push('pre-run failed: ' + errText(error));
        }
      }
      const total = Math.max(1, Math.min(32, Math.round(seconds * fps)));
      const interval = (seconds * 1000) / total;
      const frames = [];
      const t0 = Date.now();
      for (let i = 0; i < total; i += 1) {
        try {
          const shot = await call(cfg, 'get_viewport_png', { max_size: size }, { signal: exec && exec.signal, timeoutMs: cfg.imageTimeoutMs });
          const hash = createHash('md5').update(shot.png_base64).digest('hex').slice(0, 8);
          frames.push({ t: Date.now() - t0, ms: shot.ms, hash: hash, png: shot.png_base64 });
        } catch (error) {
          frames.push({ t: Date.now() - t0, ms: -1, hash: 'ERR', png: null });
        }
        const wait = (i + 1) * interval - (Date.now() - t0);
        if (wait > 1) await sleep(wait);
      }
      const hashes = frames.map(function (f) { return f.hash; });
      const distinct = Array.from(new Set(hashes)).length;
      const avg = Math.round(frames.reduce(function (a, f) { return a + Math.max(0, f.ms); }, 0) / Math.max(1, frames.length));
      parts.push('watch ' + String(seconds) + 's @' + String(fps) + 'fps -> ' + String(frames.length) + ' frames | avg ' + String(avg) + 'ms | distinct ' + String(distinct) + '/' + String(frames.length));
      parts.push('timeline: ' + frames.map(function (f, i) { return String(i) + '@' + String(f.t) + 'ms:' + f.hash; }).join(' '));
      const images = [];
      const seen = new Set();
      for (const f of frames) {
        if (!f.png || seen.has(f.hash)) continue;
        seen.add(f.hash);
        if (images.length >= 6) break;
        const att = await attachPng(f.png, 'blender-watch-' + String(Date.now()) + '-' + String(images.length));
        if (att.ref) images.push(att.ref);
        else parts.push('frame not attached: ' + txt(att.why));
      }
      return { text: parts.join('\n'), images: images };
    },
  });

  register('view', {
    name: 'blender_view',
    description: '用临时相机渲染自定义视角（from / look_at），不改变场景相机与用户视口；返回 PNG。默认 Workbench；Maleoon 上 EEVEE 会黑屏，不自动使用 EEVEE。',
    parameters: {
      from: { type: 'string', description: 'camera position x,y,z, default 7,-7,5' },
      look_at: { type: 'string', description: 'target x,y,z, default 0,0,0' },
      lens: { type: 'number', description: 'focal length mm, default 50' },
      max_size: { type: 'integer', description: 'max edge, default 960' },
      width: { type: 'integer', description: 'render width, default 960' },
      height: { type: 'integer', description: 'render height, default 540' },
      engine: { type: 'string', description: 'workbench | eevee | cycles; default workbench' },
    },
    timeoutMs: 300000,
    async execute(args, exec) {
      try {
        const result = await call(cfg, 'render_view', {
          from: args && args.from ? txt(args.from) : '7,-7,5',
          look_at: args && args.look_at ? txt(args.look_at) : '0,0,0',
          lens: Number(args && args.lens) || 50,
          max_size: Math.max(160, Math.min(1600, Number(args && args.max_size) || 960)),
          width: Math.max(160, Math.min(2048, Number(args && args.width) || 960)),
          height: Math.max(120, Math.min(2048, Number(args && args.height) || 540)),
          engine: txt(args && args.engine) || 'workbench',
        }, { signal: exec && exec.signal, timeoutMs: 300000 });
        const att = await attachPng(result.png_base64, 'blender-view-' + String(Date.now()));
        const summary = 'view from (' + (result.from || []).join(',') + ') -> (' + (result.look_at || []).join(',') + ') | '
          + String(result.width) + 'x' + String(result.height) + ' | ' + String(Math.round(result.bytes / 1024)) + 'KB | ' + String(result.ms) + 'ms'
          + ' | engine ' + String(result.engine || 'unknown');
        if (att.ref) return { text: summary, image: att.ref };
        const saved = saveWorkspace(exec, Buffer.from(result.png_base64, 'base64'), 'blender-view-' + String(Date.now()) + '.png');
        return { text: summary + '\nnot attached: ' + txt(att.why) + (saved ? ' | saved: ' + saved : '') };
      } catch (error) {
        return { text: 'blender_view failed:\n' + errText(error) };
      }
    },
  });

  register('txn', {
    name: 'blender_txn',
    description: '事务/回滚：snapshot/restore 文件级 .blend 快照；mark/revert 对象变换级快照；list 列出现有快照。',
    parameters: {
      op: { type: 'string', description: 'snapshot | restore | mark | revert | list' },
      id: { type: 'string', description: 'snapshot/mark id' },
    },
    timeoutMs: 120000,
    async execute(args, exec) {
      const op = (txt(args && args.op) || 'list').toLowerCase();
      if (['snapshot', 'restore', 'mark', 'revert', 'list'].indexOf(op) < 0) return { text: 'op must be snapshot/restore/mark/revert/list' };
      try {
        const result = await call(cfg, 'txn_' + op, { id: args && args.id ? txt(args.id) : '' }, { signal: exec && exec.signal, timeoutMs: 120000 });
        return { text: JSON.stringify(result, null, 2) };
      } catch (error) {
        return { text: 'blender_txn failed:\n' + errText(error) };
      }
    },
  });

  register('preset', {
    name: 'blender_preset',
    description: '参数预设库：把 scene 的 dotted 路径保存成 JSON，之后按名字恢复（渲染分辨率/采样/视图变换等）。',
    parameters: {
      op: { type: 'string', description: 'save | apply | list | delete' },
      name: { type: 'string', description: 'preset name' },
      values: { type: 'object', description: 'dotted scene paths to values', additionalProperties: true },
    },
    timeoutMs: 60000,
    async execute(args, exec) {
      const op = (txt(args && args.op) || 'list').toLowerCase();
      if (['save', 'apply', 'list', 'delete'].indexOf(op) < 0) return { text: 'op must be save/apply/list/delete' };
      try {
        const result = await call(cfg, 'preset_' + op, {
          name: args && args.name ? txt(args.name) : '',
          values: (args && args.values) || {},
        }, { signal: exec && exec.signal, timeoutMs: 60000 });
        return { text: JSON.stringify(result, null, 2) };
      } catch (error) {
        return { text: 'blender_preset failed:\n' + errText(error) };
      }
    },
  });

  register('opt', {
    name: 'blender_opt',
    description: '对象精简：decimate 按比例简化网格；join 合并多个网格对象。names 省略时用当前选中对象。',
    parameters: {
      op: { type: 'string', description: 'decimate | join' },
      names: { type: 'array', items: { type: 'string' }, description: 'mesh object names' },
      ratio: { type: 'number', description: 'decimate ratio 0.01-0.99, default 0.5' },
      apply: { type: 'boolean', description: 'apply the modifier, default true' },
    },
    timeoutMs: 180000,
    async execute(args, exec) {
      const op = (txt(args && args.op) || 'decimate').toLowerCase();
      if (op !== 'decimate' && op !== 'join') return { text: 'op must be decimate/join' };
      try {
        const result = await call(cfg, 'opt_' + op, {
          names: (args && args.names) || [],
          ratio: Number(args && args.ratio) || 0.5,
          apply: args && args.apply === false ? false : true,
        }, { signal: exec && exec.signal, timeoutMs: 180000 });
        return { text: JSON.stringify(result, null, 2) };
      } catch (error) {
        return { text: 'blender_opt failed:\n' + errText(error) };
      }
    },
  });

  register('perf', {
    name: 'blender_perf',
    description: '性能/场景统计：stats 返回对象/网格/材质/分辨率/采样；bench 渲染一帧并计时。',
    parameters: { op: { type: 'string', description: 'stats | bench' } },
    timeoutMs: 300000,
    async execute(args, exec) {
      const op = (txt(args && args.op) || 'stats').toLowerCase();
      if (op !== 'stats' && op !== 'bench') return { text: 'op must be stats/bench' };
      try {
        const result = await call(cfg, 'perf_' + op, {}, { signal: exec && exec.signal, timeoutMs: 300000 });
        return { text: JSON.stringify(result, null, 2) };
      } catch (error) {
        return { text: 'blender_perf failed:\n' + errText(error) };
      }
    },
  });

  register('viewport', {
    name: 'blender_viewport',
    description: '通道运维与写租约：status / doctor / who / lease / release。租约是插件进程内的建议锁，不替代 Blender 侧互斥。',
    parameters: {
      op: { type: 'string', description: 'status | doctor | who | lease | release' },
      holder: { type: 'string', description: 'lease holder id, default "default"' },
      ttl_ms: { type: 'integer', description: 'lease TTL ms, default 600000' },
      force: { type: 'boolean', description: 'take over/release even if held' },
    },
    timeoutMs: 60000,
    async execute(args, exec) {
      const op = (txt(args && args.op) || 'status').toLowerCase();
      const holder = txt(args && args.holder) || 'default';
      const ttl = Math.max(1000, Math.min(86400000, Number(args && args.ttl_ms) || 600000));
      const now = Date.now();
      if (op === 'who') {
        const remaining = leaseState.holder ? Math.max(0, leaseState.expiresAt - now) : 0;
        return { text: 'lease holder=' + (leaseState.holder || '(none)') + ' | remaining=' + String(remaining) + 'ms' };
      }
      if (op === 'lease') {
        if (leaseState.holder && leaseState.holder !== holder && leaseState.expiresAt > now && !(args && args.force)) {
          return { text: 'leased by ' + leaseState.holder + ' | remaining=' + String(leaseState.expiresAt - now) + 'ms (pass force=true to take over)' };
        }
        leaseState.holder = holder;
        leaseState.expiresAt = now + ttl;
        return { text: 'lease acquired: holder=' + holder + ' | ttl=' + String(ttl) + 'ms' };
      }
      if (op === 'release') {
        if (leaseState.holder && leaseState.holder !== holder && !(args && args.force)) {
          return { text: 'lease is held by ' + leaseState.holder + ' (pass force=true to release)' };
        }
        leaseState.holder = '';
        leaseState.expiresAt = 0;
        return { text: 'lease released' };
      }
      try {
        const t0 = Date.now();
        const info = await ping(cfg, { signal: exec && exec.signal });
        const remaining = leaseState.holder ? Math.max(0, leaseState.expiresAt - now) : 0;
        const lines = ['kind=ok | ' + describe(cfg) + ' | ' + String(Date.now() - t0) + 'ms',
          'Blender ' + txt(info.blender) + ' | Python ' + txt(info.python) + ' | addon ' + txt(info.addon) + ' ' + txt(info.version),
          'lease: ' + (leaseState.holder || '(none)') + (remaining ? ' | remaining=' + String(remaining) + 'ms' : '')];
        if (op === 'doctor') {
          const scene = await call(cfg, 'get_scene_info', { limit: 5 }, { signal: exec && exec.signal });
          lines.push('doctor: ' + String(scene.objects_total) + ' objects, active=' + txt(scene.active_object));
        }
        return { text: lines.join('\n') };
      } catch (error) {
        return { text: 'kind=error\n' + errText(error) };
      }
    },
  });

  register('loop', {
    name: 'blender_loop',
    description: '内环搜索：在 Blender 主线程反复执行 setup/step/measure，按 score 取最优。适合尺寸/位置/参数搜索，不占模型往返。',
    parameters: {
      setup: { type: 'string', description: 'run once before the loop' },
      step: { type: 'string', description: 'run every iteration; may use ns["i"]' },
      measure: { type: 'string', description: 'must set ns["score"]; optional ns["params"]' },
      iterations: { type: 'integer', description: 'max iterations, default 100' },
      budget_ms: { type: 'integer', description: 'time budget, default 20000' },
      measure_every: { type: 'integer', description: 'measure every N steps, default 1' },
      minimize: { type: 'boolean', description: 'minimize score, default true' },
      top_k: { type: 'integer', description: 'board size, default 10' },
    },
    timeoutMs: 600000,
    async execute(args, exec) {
      try {
        const result = await call(cfg, 'loop_run', {
          setup: args && args.setup ? txt(args.setup) : '',
          step: args && args.step ? txt(args.step) : '',
          measure: args && args.measure ? txt(args.measure) : '',
          iterations: Math.max(1, Math.min(200000, Number(args && args.iterations) || 100)),
          budget_ms: Math.max(100, Math.min(3600000, Number(args && args.budget_ms) || 20000)),
          measure_every: Math.max(1, Number(args && args.measure_every) || 1),
          minimize: !(args && args.minimize === false),
          top_k: Math.max(1, Math.min(100, Number(args && args.top_k) || 10)),
        }, { signal: exec && exec.signal, timeoutMs: 600000 });
        const lines = [
          (result.ok ? 'loop ok' : 'loop failed') + ' | ran ' + String(result.ran) + ' | ' + String(result.ms) + 'ms',
        ];
        if (result.best) lines.push('best: ' + JSON.stringify(result.best));
        if (result.traceback) lines.push('traceback:\n' + clip(result.traceback, 3000, 1200));
        if (Array.isArray(result.board) && result.board.length) lines.push('board: ' + JSON.stringify(result.board));
        return { text: lines.join('\n') };
      } catch (error) {
        return { text: 'blender_loop failed:\n' + errText(error) };
      }
    },
  });

  register('pick', {
    name: 'blender_pick',
    description: '让用户在 Blender 视口里点选一个对象，阻塞等待并返回确切对象名。适合“是这个吗？”的场景。',
    parameters: {
      timeout_s: { type: 'number', description: 'wait timeout seconds, default 120' },
      poll_ms: { type: 'integer', description: 'poll interval ms, default 500' },
    },
    timeoutMs: 240000,
    async execute(args, exec) {
      const timeoutS = Math.max(5, Math.min(600, Number(args && args.timeout_s) || 120));
      const pollMs = Math.max(100, Math.min(5000, Number(args && args.poll_ms) || 500));
      try {
        await call(cfg, 'pick_start', {}, { signal: exec && exec.signal, timeoutMs: 10000 });
        const deadline = Date.now() + timeoutS * 1000;
        while (Date.now() < deadline) {
          const poll = await call(cfg, 'pick_poll', {}, { signal: exec && exec.signal, timeoutMs: 10000 });
          if (poll.picked) {
            return { text: 'picked: ' + txt(poll.picked) + ' | ' + String(poll.elapsed_ms || 0) + 'ms'
              + (Array.isArray(poll.names) && poll.names.length > 1 ? ' | all: ' + poll.names.join(', ') : '') };
          }
          await sleep(pollMs);
        }
        await call(cfg, 'pick_cancel', {}, { signal: exec && exec.signal, timeoutMs: 10000 });
        return { text: 'pick timed out after ' + String(timeoutS) + 's (user did not click an object)' };
      } catch (error) {
        return { text: 'blender_pick failed:\n' + errText(error) };
      }
    },
  });

  register('plan', {
    name: 'blender_plan',
    description: '建模规划目录（28 family / 186 op）：op=catalog 列全量目录，op=help 看某个 family，op=search 搜 op。执行器按 family 逐步接入；未接入的 op 会明确说明。',
    parameters: {
      op: { type: 'string', description: 'catalog | help | search' },
      family: { type: 'string', description: 'family name for help, e.g. audit' },
      query: { type: 'string', description: 'search keyword' },
      full: { type: 'boolean', description: 'include op names in catalog' },
    },
    timeoutMs: 30000,
    async execute(args) {
      const op = (txt(args && args.op) || 'catalog').toLowerCase();
      const familyName = txt(args && args.family).toLowerCase();
      const query = txt(args && args.query).toLowerCase();
      const families = PLAN_CATALOG;
      const totalOps = families.reduce(function (n, f) { return n + (Array.isArray(f.ops) ? f.ops.length : 0); }, 0);
      if (op === 'help') {
        const f = families.find(function (x) { return x.f === familyName || x.prefix === familyName + '_'; });
        if (!f) return { text: 'family not found: ' + familyName + '\navailable: ' + families.map(function (x) { return x.f; }).join(', ') };
        return { text: 'family ' + f.f + ' (' + f.ops.length + ' ops)\nwhen: ' + f.when + '\nnot: ' + f.not + '\nskeleton: ' + f.sk + '\nkey: ' + f.key + '\nops: ' + f.ops.join(', ') };
      }
      if (op === 'search') {
        const hits = [];
        for (const f of families) {
          const blob = (f.f + ' ' + f.when + ' ' + f.not + ' ' + (f.ops || []).join(' ')).toLowerCase();
          if (!query || blob.indexOf(query) >= 0) {
            const matched = (f.ops || []).filter(function (o) { return !query || o.toLowerCase().indexOf(query) >= 0 || f.f.indexOf(query) >= 0; });
            hits.push('- ' + f.f + ': ' + (matched.length ? matched.join(', ') : f.ops.join(', ')));
          }
        }
        return { text: 'search "' + query + '"\n' + (hits.join('\n') || '(no match)') };
      }
      const lines = ['plan catalog: ' + String(families.length) + ' families | ' + String(totalOps) + ' ops'];
      for (const f of families) {
        lines.push('- ' + f.f + ' (' + f.ops.length + ' ops): ' + f.when);
        if (args && args.full) lines.push('  ops: ' + f.ops.join(', '));
      }
      lines.push('execution is not ported for every family yet; use op=help to inspect, or blender_run/blender_cmd for raw Python.');
      return { text: lines.join('\n') };
    },
  });

  register('job', {
    name: 'blender_job',
    description: '进程内作业队列（降级版）：op=start 提交 Python 立刻拿 id，op=status/list/wait 取结果。执行仍在 Blender 主线程，期间 UI 会卡。',
    parameters: {
      op: { type: 'string', description: 'start | status | list | wait' },
      code: { type: 'string', description: 'Python code for start' },
      name: { type: 'string', description: 'job name' },
      id: { type: 'string', description: 'job id for status/wait' },
      timeout_s: { type: 'number', description: 'wait timeout, default 120' },
      poll_ms: { type: 'integer', description: 'poll interval, default 500' },
    },
    timeoutMs: 600000,
    async execute(args, exec) {
      const op = (txt(args && args.op) || 'list').toLowerCase();
      try {
        if (op === 'start') {
          const job = await call(cfg, 'job_start', { code: txt(args && args.code), name: txt(args && args.name) }, { signal: exec && exec.signal, timeoutMs: 30000 });
          return { text: 'job started: ' + txt(job.id) + ' | status=' + txt(job.status) };
        }
        if (op === 'status') {
          const job = await call(cfg, 'job_status', { id: txt(args && args.id) }, { signal: exec && exec.signal, timeoutMs: 30000 });
          return { text: JSON.stringify(job, null, 2) };
        }
        if (op === 'list') {
          const jobs = await call(cfg, 'job_list', {}, { signal: exec && exec.signal, timeoutMs: 30000 });
          return { text: JSON.stringify(jobs, null, 2) };
        }
        if (op === 'wait') {
          const id = txt(args && args.id);
          const timeoutS = Math.max(1, Math.min(600, Number(args && args.timeout_s) || 120));
          const pollMs = Math.max(100, Math.min(5000, Number(args && args.poll_ms) || 500));
          const deadline = Date.now() + timeoutS * 1000;
          while (Date.now() < deadline) {
            const job = await call(cfg, 'job_status', { id: id }, { signal: exec && exec.signal, timeoutMs: 30000 });
            if (job.status === 'done' || job.status === 'error') return { text: JSON.stringify(job, null, 2) };
            await sleep(pollMs);
          }
          return { text: 'job wait timed out after ' + String(timeoutS) + 's: ' + id };
        }
        return { text: 'op must be start/status/list/wait' };
      } catch (error) {
        return { text: 'blender_job failed:\n' + errText(error) };
      }
    },
  });

  register('headless', {
    name: 'blender_headless',
    description: '鸿蒙原生子进程 headless worker（R&D）：op=start 启动常驻无头 Blender，op=run 执行 Python 并返回 stdout/error，op=stop 退出。不是 child_process，也不保证桌面级后台。',
    parameters: {
      op: { type: 'string', description: 'start | run | stop' },
      code: { type: 'string', description: 'Python code for run' },
      timeout: { type: 'number', description: 'worker run timeout seconds, default 120' },
    },
    timeoutMs: 180000,
    async execute(args, exec) {
      const op = (txt(args && args.op) || 'run').toLowerCase();
      try {
        if (op === 'start') {
          const r = await call(cfg, 'worker_start', {}, { signal: exec && exec.signal, timeoutMs: 120000 });
          return { text: 'headless worker started' + (r && r.pid ? ': pid ' + String(r.pid) : '') };
        }
        if (op === 'stop') {
          await call(cfg, 'worker_stop', {}, { signal: exec && exec.signal, timeoutMs: 30000 });
          return { text: 'headless worker stopped' };
        }
        if (op === 'run') {
          const r = await call(cfg, 'worker_run', {
            code: txt(args && args.code),
            timeout: Math.max(1, Math.min(600, Number(args && args.timeout) || 120)),
          }, { signal: exec && exec.signal, timeoutMs: 180000 });
          if (!r || r.ok !== true) return { text: 'headless worker error:\n' + JSON.stringify(r || {}, null, 2) };
          return { text: 'headless ok | ' + String(r.ms) + 'ms\n' + String(r.stdout || '') + (r.stderr ? '\nstderr:\n' + r.stderr : '') };
        }
        return { text: 'op must be start/run/stop' };
      } catch (error) {
        return { text: 'blender_headless failed:\n' + errText(error) };
      }
    },
  });
}
