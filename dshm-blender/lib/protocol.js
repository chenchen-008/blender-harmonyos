/**
 * DSHM <-> Blender transport (pure Node, no undici / no WASM / no child process).
 *
 * One request per TCP connection, newline-delimited JSON:
 *   request : {"type":"ping","params":{},"token":"..."} + "\n"
 *   success : {"status":"success","result":{...}} + "\n"
 *   failure : {"status":"error","message":"..."} + "\n"
 */
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export class BlenderError extends Error {
  constructor(message, code, detail) {
    super(message);
    this.name = 'BlenderError';
    this.code = code || 'BLENDER_ERROR';
    this.detail = detail || '';
  }
}

export function defaultConfig() {
  return {
    host: '127.0.0.1',
    port: 9876,
    token: '',
    timeoutMs: 30000,
    imageTimeoutMs: 90000,
  };
}

function asNumber(value) {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? n : null;
}

/**
 * Config precedence: explicit cordis config < config file < environment.
 * File: $DSHM_BLENDER_CONFIG or $DSH_HOME/dshm-blender.json.
 */
export function loadConfig(overrides) {
  const cfg = defaultConfig();
  const home = process.env.DSH_HOME || path.join(os.homedir(), '.dsh');
  const file = process.env.DSHM_BLENDER_CONFIG || path.join(home, 'dshm-blender.json');
  let fromFile = null;
  try {
    fromFile = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    fromFile = null;
  }
  const layers = [overrides || {}, fromFile || {}];
  for (const layer of layers) {
    if (!layer || typeof layer !== 'object') continue;
    if (typeof layer.host === 'string' && layer.host.length > 0) cfg.host = layer.host;
    const port = asNumber(layer.port);
    if (port !== null) cfg.port = Math.floor(port);
    if (typeof layer.token === 'string') cfg.token = layer.token;
    const timeout = asNumber(layer.timeoutMs);
    if (timeout !== null) cfg.timeoutMs = timeout;
    const imageTimeout = asNumber(layer.imageTimeoutMs);
    if (imageTimeout !== null) cfg.imageTimeoutMs = imageTimeout;
  }
  if (process.env.DSHM_BLENDER_HOST) cfg.host = process.env.DSHM_BLENDER_HOST;
  const envPort = asNumber(process.env.DSHM_BLENDER_PORT);
  if (envPort !== null) cfg.port = Math.floor(envPort);
  if (process.env.DSHM_BLENDER_TOKEN !== undefined) cfg.token = process.env.DSHM_BLENDER_TOKEN;
  const envTimeout = asNumber(process.env.DSHM_BLENDER_TIMEOUT_MS);
  if (envTimeout !== null) cfg.timeoutMs = envTimeout;
  return cfg;
}

function timeoutMessage(cfg) {
  return 'Blender 在 ' + cfg.host + ':' + cfg.port + ' 接受了连接，但主线程没有回包（超时）。'
    + '最常见原因：Blender 被切到后台，被鸿蒙冻结了。请把 DSHM 与 Blender 分屏或让 Blender 悬浮可见，'
    + '确认 Blender 画面仍在刷新后重试。';
}

/**
 * Send one command. Returns the addon's result object.
 * @param {object} cfg
 * @param {string} type
 * @param {object} params
 * @param {{timeoutMs?:number, signal?:AbortSignal, timeoutSec?:number}} opts
 */
export function call(cfg, type, params, opts) {
  const o = opts || {};
  const timeoutMs = o.timeoutMs || cfg.timeoutMs || 30000;
  return new Promise((resolve, reject) => {
    let settled = false;
    let buf = '';
    const socket = net.connect({ host: cfg.host, port: cfg.port });
    const finish = (err, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (o.signal) {
        try { o.signal.removeEventListener('abort', onAbort); } catch { /* ignore */ }
      }
      try { socket.destroy(); } catch { /* ignore */ }
      if (err) reject(err); else resolve(value);
    };
    const parse = (text) => {
      let obj;
      try {
        obj = JSON.parse(text);
      } catch (error) {
        finish(new BlenderError('Blender 回包不是合法 JSON：' + text.slice(0, 200), 'BLENDER_BAD_JSON'));
        return;
      }
      if (obj && obj.status === 'error') {
        finish(new BlenderError(String(obj.message || 'addon error'), 'BLENDER_ADDON_ERROR'));
      } else {
        finish(null, obj && obj.result !== undefined ? obj.result : {});
      }
    };
    const onAbort = () => finish(new BlenderError('已取消（宿主中断）', 'BLENDER_ABORTED'));
    const timer = setTimeout(() => finish(new BlenderError(timeoutMessage(cfg), 'BLENDER_TIMEOUT')), timeoutMs);
    if (o.signal) {
      if (o.signal.aborted) { onAbort(); return; }
      o.signal.addEventListener('abort', onAbort, { once: true });
    }
    socket.setEncoding('utf8');
    socket.once('connect', () => {
      const request = { type, params: params || {} };
      if (cfg.token) request.token = cfg.token;
      if (o.timeoutSec) request.timeout = o.timeoutSec;
      try {
        socket.write(JSON.stringify(request) + '\n');
      } catch (error) {
        finish(new BlenderError('发送请求失败：' + String(error && error.message ? error.message : error), 'BLENDER_IO'));
      }
    });
    socket.on('data', (chunk) => {
      buf += chunk;
      const nl = buf.indexOf('\n');
      if (nl >= 0) parse(buf.slice(0, nl));
    });
    socket.once('error', (error) => {
      const code = error && error.code;
      if (code === 'ECONNREFUSED' || code === 'EHOSTUNREACH' || code === 'ENETUNREACH') {
        finish(new BlenderError(
          '连不上 Blender（' + cfg.host + ':' + cfg.port + '）。请确认 Blender 正在运行、'
          + '已启用 DSHM Blender Bridge 插件，并且端口一致。',
          'BLENDER_UNREACHABLE',
          code
        ));
      } else if (code === 'ECONNRESET') {
        finish(new BlenderError('连接被 Blender 重置（addon 可能刚重启）。稍后重试。', 'BLENDER_IO', code));
      } else {
        finish(new BlenderError('与 Blender 通信失败：' + String(error && error.message ? error.message : error), 'BLENDER_IO', code));
      }
    });
    socket.once('close', () => {
      if (settled) return;
      if (buf.trim().length > 0) parse(buf.trim());
      else finish(new BlenderError('Blender 关闭了连接但没有回包。确认分屏下 Blender 仍在刷新。', 'BLENDER_IO'));
    });
  });
}

/** Quick liveness probe used by blender_status. */
export function ping(cfg, opts) {
  return call(cfg, 'ping', {}, Object.assign({ timeoutMs: 4000 }, opts || {}));
}

export function describe(cfg) {
  return cfg.host + ':' + cfg.port + (cfg.token ? ' (token on)' : '');
}
