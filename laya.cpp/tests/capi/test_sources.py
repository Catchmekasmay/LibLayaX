#!/usr/bin/env python3
"""Runs test-sources over many forms of one model.

    python3 tests/capi/test_sources.py BIN_DIR MODEL_DIR [--wine] [--work DIR]

BIN_DIR holds test-sources (or test-sources.exe) and the library; MODEL_DIR is a model folder.
Every form of the model that should load must give the answer of the plain folder, and every
broken form must fail with the expected message. With --wine the Windows program is run
through Wine. The forms are written to --work (default: a folder in the system's temporary
directory), one at a time, each as large as the model; the folder is removed at the end.
"""
import argparse, io, os, shutil, subprocess, sys, tarfile, tempfile

FILES = ['rl_agent_config.json', 'encoder/config.json', 'model.safetensors',
         'tokenizer/tokenizer.json', 'tokenizer/tokenizer_config.json']
WEIGHTS = 'model.safetensors'

parser = argparse.ArgumentParser()
parser.add_argument('bin_dir')
parser.add_argument('model_dir')
parser.add_argument('--wine', action='store_true')
parser.add_argument('--work')
args = parser.parse_args()

BIN = os.path.abspath(args.bin_dir)
MODEL = os.path.abspath(args.model_dir)
ROOT = os.path.abspath(args.work) if args.work else tempfile.mkdtemp(prefix='laya-sources-')
WORK = os.path.join(ROOT, 'work')


def shown(path):
    """The path as the program under test has to be given it."""
    return 'Z:' + path.replace('/', '\\') if args.wine else path


def run(*arguments):
    env = dict(os.environ)
    if args.wine:
        env['WINEDEBUG'] = '-all'
        command = ['wine', 'test-sources.exe', *arguments]
    else:
        env['LD_LIBRARY_PATH'] = BIN + os.pathsep + env.get('LD_LIBRARY_PATH', '')
        env['DYLD_LIBRARY_PATH'] = BIN + os.pathsep + env.get('DYLD_LIBRARY_PATH', '')
        command = [os.path.join(BIN, 'test-sources'), *arguments]
    p = subprocess.run(command, cwd=BIN, env=env, capture_output=True, text=True, timeout=1800)
    lines = [l.strip() for l in p.stdout.replace('\r', '').split('\n') if l.strip()]
    return lines[-1] if lines else 'NO OUTPUT (exit %d) %s' % (p.returncode, p.stderr[-300:])


results = []


def check(name, got, want_ok=None, want_error=None):
    good = got == want_ok if want_ok is not None else got.startswith('ERROR') and want_error in got
    results.append(good)
    print(('PASS ' if good else 'FAIL ') + name + ('' if good else '\n     got: ' + got[:300]))
    sys.stdout.flush()


def fresh():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)


def make_tar(path, fmt, prefix='', extra=None, skip=(), weights=None):
    with tarfile.open(path, 'w', format=fmt) as tar:
        for name, data in (extra or []):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        for name in FILES:
            if name in skip:
                continue
            source = weights if (name == WEIGHTS and weights) else os.path.join(MODEL, name)
            info = tarfile.TarInfo(prefix + name)
            info.size = os.path.getsize(source)
            with open(source, 'rb') as f:
                tar.addfile(info, f)


fresh()
baseline = run(shown(MODEL))
print('plain folder:', baseline[:160])
sys.stdout.flush()
if not baseline.startswith('OK'):
    sys.exit(1)

# the stored form of the weights for the test codec of test-sources
coded = os.path.join(ROOT, 'coded.safetensors')
print('stored form of the weights:', run('--encode', shown(os.path.join(MODEL, WEIGHTS)), shown(coded)))

T = os.path.join(WORK, 'model.tar')
long_folder = 'a-folder-with-a-very-long-name-' + 'x' * 120 + '/'

# archives
for name, options in [
    ('tar, ustar, files at the top', dict(fmt=tarfile.USTAR_FORMAT)),
    ('tar, gnu, inside one folder', dict(fmt=tarfile.GNU_FORMAT, prefix='laya-model/')),
    ('tar, pax, inside one folder', dict(fmt=tarfile.PAX_FORMAT, prefix='laya-model/')),
    ('tar, gnu, long names', dict(fmt=tarfile.GNU_FORMAT, prefix=long_folder)),
    ('tar, pax, long names', dict(fmt=tarfile.PAX_FORMAT, prefix=long_folder)),
    ('tar, names with ./', dict(fmt=tarfile.GNU_FORMAT, prefix='./')),
    ('tar, with resource-fork entries', dict(fmt=tarfile.PAX_FORMAT,
                                             extra=[('._rl_agent_config.json', b'junk'),
                                                    ('tokenizer/._tokenizer.json', b'junk')])),
]:
    fresh()
    make_tar(T, **options)
    check(name, run(shown(T)), want_ok=baseline)

fresh()
make_tar(T, tarfile.GNU_FORMAT, prefix='typed-decisions/')
check('tar, variant folder only, variant option', run(shown(T), '{"variant":"typed-decisions"}'), want_ok=baseline)
fresh()
make_tar(T, tarfile.GNU_FORMAT, prefix='store/typed-decisions/')
check('tar, store with a variant folder, variant option', run(shown(T), '{"variant":"typed-decisions"}'), want_ok=baseline)
check('tar, store with a variant folder, no variant option', run(shown(T)), want_error='Cannot open')

# codec
fresh()
make_tar(T, tarfile.GNU_FORMAT, weights=coded)
check('coded tar, with the codec', run(shown(T), '--codec', 'shift'), want_ok=baseline)
check('coded tar, without the codec', run(shown(T)), want_error='Invalid safetensors header')
fresh()
make_tar(T, tarfile.PAX_FORMAT, prefix='laya-model/', weights=coded)
check('coded tar inside one folder, with the codec', run(shown(T), '--codec', 'shift'), want_ok=baseline)
fresh()
folder = os.path.join(WORK, 'coded-model')
os.makedirs(os.path.join(folder, 'encoder'))
os.makedirs(os.path.join(folder, 'tokenizer'))
for name in FILES:
    source = coded if name == WEIGHTS else os.path.join(MODEL, name)
    os.symlink(os.path.realpath(source), os.path.join(folder, name))
check('coded folder, with the codec', run(shown(folder), '--codec', 'shift'), want_ok=baseline)
check('coded folder, without the codec', run(shown(folder)), want_error='Invalid safetensors header')
check('plain folder, codec that fails', run(shown(MODEL), '--codec', 'fail'),
      want_error='The codec reported error 7 for model.safetensors')
check('plain folder, an option that does not exist', run(shown(MODEL), '{"codec":"x"}'), want_error='Unknown option: codec')

# broken input
fresh()
check('a file that is not a tar', run(shown(os.path.join(MODEL, 'tokenizer', 'tokenizer.json'))),
      want_error='not a folder and not an uncompressed tar archive')
make_tar(T, tarfile.GNU_FORMAT, skip=('tokenizer/tokenizer.json',))
check('tar without the tokenizer', run(shown(T)), want_error='no such file in the archive')
fresh()
make_tar(T, tarfile.GNU_FORMAT)
with open(T, 'r+b') as f:
    f.truncate(os.path.getsize(T) // 2)
check('tar cut in half', run(shown(T)), want_error='Truncated tar archive')
fresh()
with open(T, 'wb') as f:
    f.write(b'\x1f\x8b' + os.urandom(4096))
check('a compressed file', run(shown(T)), want_error='not a folder and not an uncompressed tar archive')
check('a folder that does not exist', run(shown(os.path.join(ROOT, 'nothing-here'))), want_error='Cannot open')
check('plain folder again', run(shown(MODEL)), want_ok=baseline)

shutil.rmtree(WORK, ignore_errors=True)
os.remove(coded)
if not args.work:
    shutil.rmtree(ROOT, ignore_errors=True)
print('%d of %d passed' % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
