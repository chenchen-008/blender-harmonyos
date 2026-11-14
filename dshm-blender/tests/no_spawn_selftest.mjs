import fs from 'node:fs';
import path from 'node:path';

const roots = ['lib', 'addon'];
const banned = [
  [/from\s+['\"](?:node:)?child_process['\"]/, 'import child_process'],
  [/require\(\s*['\"](?:node:)?child_process['\"]\s*\)/, 'require child_process'],
  [/\bimport\s+subprocess\b/, 'import subprocess'],
  [/\bos\.system\s*\(/, 'os.system'],
  [/\bpopen\s*\(/, 'popen'],
];
const hits = [];
for (const root of roots) {
  const dir = path.resolve(root);
  if (!fs.existsSync(dir)) continue;
  for (const name of fs.readdirSync(dir)) {
    const file = path.join(dir, name);
    if (!fs.statSync(file).isFile()) continue;
    const text = fs.readFileSync(file, 'utf8');
    for (const [re, label] of banned) {
      if (re.test(text)) hits.push(file + ': ' + label);
    }
  }
}
if (hits.length) {
  console.error('no-spawn selftest FAILED:');
  for (const h of hits) console.error('  ' + h);
  process.exit(1);
}
console.log('no-spawn selftest: PASS（lib/ 与 addon/ 未发现进程创建代码）');