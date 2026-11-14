import os, json
home = os.environ['DSH_HOME']
cores = os.path.join(os.path.dirname(home), 'cores')
current = sorted(os.listdir(cores))[-1]
core = os.path.join(cores, current)
print('CORE', core)
for rel in ['node_modules/@dsh-external/dshm-blender', 'node_modules/@deepseek-ai/dsh-web-app/package.json']:
    p = os.path.join(core, rel)
    print(rel, os.path.exists(p))
p = os.path.join(home, 'cordis.patch.yml')
print('PATCH_EXISTS', os.path.exists(p))
if os.path.exists(p):
    print(open(p, encoding='utf-8').read())
print('PROFILES', os.listdir(os.path.join(home, 'profiles')) if os.path.isdir(os.path.join(home, 'profiles')) else [])