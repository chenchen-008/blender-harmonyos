// Dev-only: create the @deepseek-ai/dsh-tools stub used by tests/integration.mjs.
import fs from 'node:fs';
import path from 'node:path';
const dir = path.join(process.cwd(), 'node_modules', '@deepseek-ai', 'dsh-tools');
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, 'package.json'), JSON.stringify({ name: '@deepseek-ai/dsh-tools', version: '0.0.0-test', type: 'module', main: 'index.js' }, null, 2));
fs.writeFileSync(path.join(dir, 'index.js'), "export function defineTool(spec) { if (!spec || typeof spec.name !== 'string') throw new Error('defineTool: name required'); return spec; }\n");
console.log('stub ready:', dir);
