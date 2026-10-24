// ライブラリ（node_modules）を package-lock.json のとおりに入れる。tools/supervisor.ts と create-admin.cmd が使う。
//   node --experimental-strip-types tools/deps.ts   入っていない・古いときだけ入れる
// 入れたときの package.json と package-lock.json の印を node_modules/.kantan-deps.json に残して、変わったかを見分ける。
// 開発用のライブラリ（TypeScript など）が入っていれば、入れ直すときも入れる（入っていなければ、サーバーに要るものだけ）。
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { copyFileSync, existsSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const serverDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const modules = path.join(serverDir, 'node_modules');
const runtime = path.join(serverDir, '.runtime');
/** サーバーを動かしたまま入れるときの作業場所と、入れ替えた前のもの */
const NEXT = path.join(runtime, 'next');
const PREVIOUS = path.join(runtime, 'previous');
const STAMP = '.kantan-deps.json';
const FILES = ['package.json', 'package-lock.json'];

function signature(dir: string): string {
  const h = createHash('sha256');
  for (const f of FILES) h.update(readFileSync(path.join(dir, f))).update('\0');
  return h.digest('hex');
}

/** 今の node_modules が、package.json と package-lock.json のとおりか */
export function depsUpToDate(): boolean {
  try {
    const stamp = JSON.parse(readFileSync(path.join(modules, STAMP), 'utf8')) as { signature?: string };
    return stamp.signature === signature(serverDir);
  } catch {
    return false;
  }
}

const withDev = (): boolean => existsSync(path.join(modules, 'typescript', 'package.json'));

/** 子のプロセスでも、この Node.js（start.cmd が用意したもの）を使うように */
export function withNodeOnPath(env: NodeJS.ProcessEnv): NodeJS.ProcessEnv {
  const out = { ...env };
  const key = Object.keys(out).find((k) => k.toUpperCase() === 'PATH') ?? 'PATH';
  out[key] = `${path.dirname(process.execPath)}${path.delimiter}${out[key] ?? ''}`;
  return out;
}

function npmCi(cwd: string, dev: boolean): Promise<boolean> {
  // npm は Node.js に付いてくるものを使う（PATH に npm が無くてもよい）
  const cli = path.join(path.dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js');
  if (!existsSync(cli)) {
    console.error(`npm が見つかりません: ${cli}`);
    return Promise.resolve(false);
  }
  // 入れるときのスクリプトは動かさない（使っているライブラリには要らない。知らないプログラムを動かさないように）
  const args = [cli, 'ci', '--ignore-scripts', '--no-audit', '--no-fund', '--no-update-notifier', ...(dev ? [] : ['--omit=dev'])];
  return new Promise((resolve) => {
    const c = spawn(process.execPath, args, { cwd, stdio: 'inherit', env: withNodeOnPath(process.env) });
    c.once('error', (err) => {
      console.error(`npm を動かせませんでした: ${err.message}`);
      resolve(false);
    });
    c.once('exit', (code) => resolve(code === 0));
  });
}

/** server/node_modules に入れる（今あるものは消して入れ直す）。サーバーが止まっているときに使う */
export async function installInPlace(): Promise<boolean> {
  const dev = withDev();
  const sig = signature(serverDir);
  if (!(await npmCi(serverDir, dev))) return false;
  writeFileSync(path.join(modules, STAMP), JSON.stringify({ signature: sig, dev }));
  return true;
}

/** サーバーを動かしたまま、別の場所（.runtime/next）に入れる。swapIn で入れ替える */
export async function installStaged(): Promise<boolean> {
  const dev = withDev();
  rmSync(NEXT, { recursive: true, force: true });
  mkdirSync(NEXT, { recursive: true });
  for (const f of [...FILES, '.npmrc']) {
    if (existsSync(path.join(serverDir, f))) copyFileSync(path.join(serverDir, f), path.join(NEXT, f));
  }
  const sig = signature(NEXT);
  if (!(await npmCi(NEXT, dev))) {
    rmSync(NEXT, { recursive: true, force: true });
    return false;
  }
  writeFileSync(path.join(NEXT, 'node_modules', STAMP), JSON.stringify({ signature: sig, dev }));
  return true;
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** 名前を変える。ウイルス対策ソフトがファイルを読んでいる間などは失敗するので、少し待って何度か試す */
async function renameRetry(from: string, to: string): Promise<void> {
  for (let i = 0; ; i++) {
    try {
      renameSync(from, to);
      return;
    } catch (err) {
      const code = (err as NodeJS.ErrnoException).code;
      if (i >= 20 || (code !== 'EPERM' && code !== 'EBUSY' && code !== 'EACCES')) throw err;
      await sleep(250);
    }
  }
}

/**
 * installStaged で入れたものを node_modules にする（前のものは .runtime/previous に置いておく）。
 * 動いているサーバーは、ライブラリを読み込み終わっているので、そのまま動く
 */
export async function swapIn(): Promise<boolean> {
  rmSync(PREVIOUS, { recursive: true, force: true });
  try {
    if (existsSync(modules)) await renameRetry(modules, PREVIOUS);
  } catch (err) {
    console.error(`node_modules を入れ替えられませんでした: ${(err as Error).message}`);
    rmSync(NEXT, { recursive: true, force: true });
    return false;
  }
  try {
    await renameRetry(path.join(NEXT, 'node_modules'), modules);
  } catch (err) {
    console.error(`node_modules を入れ替えられませんでした: ${(err as Error).message}`);
    await restorePrevious();
    rmSync(NEXT, { recursive: true, force: true });
    return false;
  }
  rmSync(NEXT, { recursive: true, force: true });
  return true;
}

/** swapIn の前に戻す（新しいライブラリでは動かなかったとき） */
export async function restorePrevious(): Promise<void> {
  if (!existsSync(PREVIOUS)) return;
  rmSync(modules, { recursive: true, force: true });
  await renameRetry(PREVIOUS, modules);
}

/** 前のものを消す（新しいライブラリで起動できたとき） */
export function discardPrevious(): void {
  rmSync(PREVIOUS, { recursive: true, force: true });
}

// 直接動かしたとき（create-admin.cmd）: 入っていない・古いときだけ入れる
if (path.resolve(process.argv[1] ?? '').toLowerCase() === fileURLToPath(import.meta.url).toLowerCase()) {
  if (!depsUpToDate()) {
    console.log('ライブラリを入れます（package-lock.json のとおり）');
    if (!(await installInPlace())) {
      console.error('ライブラリを入れられませんでした。インターネットにつながっているか確かめてください');
      process.exit(1);
    }
  }
}
