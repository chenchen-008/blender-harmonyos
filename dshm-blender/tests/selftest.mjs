import net from 'node:net';
import assert from 'node:assert/strict';
import { call, ping, loadConfig, BlenderError } from '../lib/protocol.js';

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
      if (req.type === 'ping') {
        reply({ status: 'success', result: { pong: true, blender: '4.5.12 LTS', python: '3.11.11', token: req.token || '' } });
      } else if (req.type === 'get_scene_info') {
        reply({ status: 'success', result: { scene: 'Scene', objects_total: 3, objects: [{ name: 'Cube' }] } });
      } else if (req.type === 'fail') {
        reply({ status: 'error', message: 'boom' });
      } else if (req.type === 'slow') {
        // accept and never answer: simulates a frozen Blender main thread
      } else {
        reply({ status: 'success', result: { echo: req.type } });
      }
    });
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port })));
}

const { server, port } = await startMock();
const cfg = { host: '127.0.0.1', port, token: 'secret', timeoutMs: 1500, imageTimeoutMs: 3000 };
let passed = 0;
function ok(name, fn) { fn(); passed += 1; console.log('ok  ' + name); }

const info = await ping(cfg);
ok('ping returns blender version', () => assert.equal(info.blender, '4.5.12 LTS'));
ok('token is forwarded', () => assert.equal(info.token, 'secret'));

const scene = await call(cfg, 'get_scene_info', { limit: 5 });
ok('scene round-trip', () => assert.equal(scene.objects_total, 3));

await assert.rejects(() => call(cfg, 'fail', {}), (error) => {
  ok('addon error is mapped', () => {
    assert.ok(error instanceof BlenderError);
    assert.equal(error.code, 'BLENDER_ADDON_ERROR');
  });
  return true;
});

const cfgShort = Object.assign({}, cfg, { timeoutMs: 300 });
const t0 = Date.now();
await assert.rejects(() => call(cfgShort, 'slow', {}), (error) => {
  ok('timeout is mapped with split-screen hint', () => {
    assert.ok(error instanceof BlenderError);
    assert.equal(error.code, 'BLENDER_TIMEOUT');
    assert.match(error.message, /分屏/);
  });
  return true;
});
ok('timeout respects the budget', () => assert.ok(Date.now() - t0 >= 250));

const closed = net.createServer();
await new Promise((resolve) => closed.listen(0, '127.0.0.1', resolve));
const deadPort = closed.address().port;
await new Promise((resolve) => closed.close(resolve));
await assert.rejects(() => call(Object.assign({}, cfg, { port: deadPort, timeoutMs: 500 }), 'ping', {}), (error) => {
  ok('unreachable is mapped', () => {
    assert.ok(error instanceof BlenderError);
    assert.equal(error.code, 'BLENDER_UNREACHABLE');
  });
  return true;
});

process.env.DSHM_BLENDER_HOST = '127.0.0.1';
process.env.DSHM_BLENDER_PORT = String(port);
process.env.DSHM_BLENDER_TOKEN = 'env-token';
const envCfg = loadConfig({});
ok('env overrides config', () => {
  assert.equal(envCfg.port, port);
  assert.equal(envCfg.token, 'env-token');
});

server.close();
console.log('\n' + String(passed) + ' assertions passed');
