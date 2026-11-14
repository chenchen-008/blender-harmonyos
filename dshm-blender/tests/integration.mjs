import net from 'node:net';
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { apply } from '../lib/index.js';

const PNG = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lp3G2QAAAABJRU5ErkJggg==';

function startMock() {
  const server = net.createServer((socket) => {
    let buf = '';
    socket.setEncoding('utf8');
    socket.on('data', (chunk) => {
      buf += chunk;
      const nl = buf.indexOf('\n');
      if (nl < 0) return;
      const req = JSON.parse(buf.slice(0, nl));
      const reply = (obj) => socket.write(JSON.stringify(obj) + '\n');
      if (req.type === 'ping') reply({ status: 'success', result: { pong: true, blender: '4.5.12 LTS', python: '3.11.11', addon: 'dshm-blender-bridge', version: '0.1.0' } });
      else if (req.type === 'get_scene_info') reply({ status: 'success', result: { scene: 'Scene', objects_total: 1, objects: [{ name: 'Cube', type: 'MESH', location: [0,0,0], dimensions: [2,2,2] }], collections: ['Collection'], active_object: 'Cube', frame: 1, engine: 'EEVEE' } });
      else if (req.type === 'execute_code') reply({ status: 'success', result: { executed: true, stdout: 'ROTATED\n', stderr: '', ms: 1 } });
      else if (req.type === 'get_viewport_png') reply({ status: 'success', result: { png_base64: PNG, width: 1, height: 1, bytes: 68, ms: 2 } });
      else if (req.type === 'render_view') reply({ status: 'success', result: { png_base64: PNG, width: 1, height: 1, bytes: 68, ms: 3, from: [7, -7, 5], look_at: [0, 0, 0] } });
      else if (req.type === 'loop_run') reply({ status: 'success', result: { ok: true, ran: 3, best: { i: 2, score: 0 }, board: [{ i: 2, score: 0 }], ms: 5 } });
      else if (req.type === 'pick_start') reply({ status: 'success', result: { started: true } });
      else if (req.type === 'pick_poll') reply({ status: 'success', result: { picked: 'Cube', names: ['Cube'], elapsed_ms: 5 } });
      else if (req.type === 'pick_cancel') reply({ status: 'success', result: { cancelled: true } });
      else if (req.type === 'job_start') reply({ status: 'success', result: { id: 'job-1', status: 'queued', name: 't' } });
      else if (req.type === 'job_status') reply({ status: 'success', result: { id: 'job-1', status: 'done', ms: 1, stdout: 'ok', stderr: '', traceback: '' } });
      else if (req.type === 'job_list') reply({ status: 'success', result: { jobs: [{ id: 'job-1', status: 'done' }] } });
      else if (req.type === 'worker_start') reply({ status: 'success', result: { ok: true, pid: 1234, socket: '/tmp/w.sock' } });
      else if (req.type === 'worker_run') reply({ status: 'success', result: { ok: true, stdout: 'worker ok\n', stderr: '', error: '', ms: 3 } });
      else if (req.type === 'worker_stop') reply({ status: 'success', result: { ok: true, stopped: true } });
      else if (req.type === 'list_commands') reply({ status: 'success', result: { commands: [{ name: 'ping', description: 'health' }] } });
      else if (req.type === 'export_scene') reply({ status: 'success', result: { format: 'obj', path: '/tmp/x.obj', bytes: 10, ms: 1, data_base64: Buffer.from('obj').toString('base64') } });
      else reply({ status: 'success', result: { echo: req.type } });
    });
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port })));
}

const { server, port } = await startMock();
const tools = new Map();
const attachments = {
  imageLimits: { mediaTypes: ['image/png'] },
  async saveImage({ data, name }) { return { attachmentId: 'att-1', mediaType: 'image/png', bytes: data.length, width: 1, height: 1, name }; },
};
const ctx = {
  tools: { register(tool) { tools.set(tool.name, tool); return () => tools.delete(tool.name); } },
  effect(fn) { return fn(); },
  get(name) { return name === 'attachments' ? attachments : undefined; },
};
apply(ctx, { port });
assert.equal(tools.size, 20);
console.log('ok  registers 20 tools: ' + Array.from(tools.keys()).join(','));

const exec = { agent: { session: { header: { cwd: fs.mkdtempSync(path.join(os.tmpdir(), 'dshm-blender-')) } } }, signal: new AbortController().signal };
const status = await tools.get('blender_status').execute({}, exec);
assert.match(status.text, /kind=ok/);
console.log('ok  blender_status');

const scene = await tools.get('blender_scene').execute({}, exec);
assert.match(scene.text, /Cube/);
console.log('ok  blender_scene');

const run = await tools.get('blender_run').execute({ code: 'print(1)' }, exec);
assert.match(run.text, /executed=true/);
assert.ok(run.image && run.image.attachmentId === 'att-1');
console.log('ok  blender_run attaches an image');

const see = await tools.get('blender_see').execute({ max_size: 320 }, exec);
assert.ok(see.image);
console.log('ok  blender_see attaches an image');

const exp = await tools.get('blender_export').execute({ format: 'obj', inline: true }, exec);
assert.match(exp.text, /DSHM workspace:/);
console.log('ok  blender_export writes inline bytes to workspace');

const view = await tools.get('blender_view').execute({ max_size: 160 }, exec);
assert.ok(view.image);
console.log('ok  blender_view attaches an image');

const watch = await tools.get('blender_watch').execute({ seconds: 0.2, fps: 4, max_size: 160 }, exec);
assert.match(watch.text, /watch 0.2s/);
assert.ok(Array.isArray(watch.images) && watch.images.length >= 1);
console.log('ok  blender_watch samples and attaches frames');

const txn = await tools.get('blender_txn').execute({ op: 'list' }, exec);
assert.ok(txn.text.length > 0);
console.log('ok  blender_txn');

const preset = await tools.get('blender_preset').execute({ op: 'list' }, exec);
assert.ok(preset.text.length > 0);
console.log('ok  blender_preset');

const opt = await tools.get('blender_opt').execute({ op: 'decimate' }, exec);
assert.ok(opt.text.length > 0);
console.log('ok  blender_opt');

const perf = await tools.get('blender_perf').execute({ op: 'stats' }, exec);
assert.ok(perf.text.length > 0);
console.log('ok  blender_perf');

const viewport = await tools.get('blender_viewport').execute({ op: 'status' }, exec);
assert.match(viewport.text, /kind=ok/);
console.log('ok  blender_viewport');

const loop = await tools.get('blender_loop').execute({ step: 'x = 1', measure: 'score = 0', iterations: 3 }, exec);
assert.match(loop.text, /loop ok/);
console.log('ok  blender_loop');

const pick = await tools.get('blender_pick').execute({ timeout_s: 5, poll_ms: 100 }, exec);
assert.match(pick.text, /picked: Cube/);
console.log('ok  blender_pick');

const plan = await tools.get('blender_plan').execute({ op: 'catalog' }, exec);
assert.match(plan.text, /28 families/);
assert.match(plan.text, /186 ops/);
console.log('ok  blender_plan catalog');

const job = await tools.get('blender_job').execute({ op: 'start', code: 'print(1)' }, exec);
assert.match(job.text, /job started: job-1/);
console.log('ok  blender_job start');

const jobWait = await tools.get('blender_job').execute({ op: 'wait', id: 'job-1', timeout_s: 2, poll_ms: 100 }, exec);
assert.match(jobWait.text, /"status": "done"/);
console.log('ok  blender_job wait');

const hs = await tools.get('blender_headless').execute({ op: 'start' }, exec);
assert.match(hs.text, /headless worker started/);
const hr = await tools.get('blender_headless').execute({ op: 'run', code: 'print(1)' }, exec);
assert.match(hr.text, /worker ok/);
console.log('ok  blender_headless start/run');

const lease = await tools.get('blender_viewport').execute({ op: 'lease', holder: 't1' }, exec);
assert.match(lease.text, /lease acquired/);
const who = await tools.get('blender_viewport').execute({ op: 'who' }, exec);
assert.match(who.text, /holder=t1/);
const release = await tools.get('blender_viewport').execute({ op: 'release', holder: 't1' }, exec);
assert.match(release.text, /lease released/);
console.log('ok  blender_viewport lease');

server.close();
console.log('\nall integration assertions passed');
