// AI サーバーを動かし続ける（server\start.cmd から起動する）。npm start と違い、更新を自動で取り込む。
// ・ライブラリ（node_modules）が package-lock.json と違えば入れ直す。サーバーを動かしている間は、別の場所に入れてから入れ替える
// ・src・.env・package.json・package-lock.json が変わったら、新しい中身を確かめてから起動し直す。
//   確かめてだめなら、今のサーバーを動かしたまま、直して保存されるのを待つ
// ・サーバーが落ちたら、少し待ってから起動し直す（PostgreSQL より先に起動したときなど）
// ・tools の .ts が変わったら、終了コード 75 で終わる（start.ps1 が、新しい中身でこれを起動し直す）
// public（管理画面）は、サーバーが毎回ファイルを読むので、変えるとすぐに反映される（起動し直さない）。
import type { ChildProcess } from 'node:child_process';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import path from 'node:path';
import { EXIT } from '../src/exit-codes.ts';
import { depsUpToDate, discardPrevious, installInPlace, installStaged, restorePrevious, serverDir, swapIn } from './deps.ts';

/** start.ps1 に、これを起動し直してもらうときの終了コード */
const RELAUNCH = 75;
/** ファイルを見に行く間隔と、変わってから落ち着くまで待つ時間（git pull などで、まとめて変わるため） */
const POLL_MS = 1000;
const SETTLE_MS = 1500;
/** 止めるときに、処理中の依頼を待つ長さの上限（Bedrock の返事を待つのは最長 10 分） */
const STOP_TIMEOUT_MS = 11 * 60_000;

type Group = 'server' | 'deps' | 'self';
const WATCHED: { group: Group; rel: string; dir: boolean; only?: RegExp }[] = [
  { group: 'server', rel: 'src', dir: true },
  { group: 'server', rel: '.env', dir: false },
  { group: 'deps', rel: 'package.json', dir: false },
  { group: 'deps', rel: 'package-lock.json', dir: false },
  { group: 'self', rel: 'tools', dir: true, only: /\.ts$/ },
];

function log(message: string): void {
  const time = new Date().toLocaleTimeString('ja-JP', { hour12: false });
  console.log(`[${time}] ${message}`);
}

// ---- ファイルの見張り（1 秒ごとに大きさと時刻を見て、変わったものだけ中身を比べる） ----------------------

type Entry = { group: Group; sig: string; hash: string };

function listFiles(w: (typeof WATCHED)[number]): string[] {
  if (!w.dir) return [w.rel];
  let names: string[];
  try {
    names = readdirSync(path.join(serverDir, w.rel), { recursive: true }) as string[];
  } catch {
    return [];
  }
  // 編集ソフトの一時ファイルなどは見ない
  return names
    .filter((n) => !n.split(/[\\/]/).some((part) => part.startsWith('.')) && !/(~|\.(tmp|swp|swx|bak|orig))$/i.test(n))
    .filter((n) => !w.only || w.only.test(n))
    .map((n) => path.join(w.rel, n));
}

function scan(prev: Map<string, Entry>): { next: Map<string, Entry>; changed: Map<string, Group> } {
  const next = new Map<string, Entry>();
  const changed = new Map<string, Group>();
  for (const w of WATCHED) {
    for (const rel of listFiles(w)) {
      const full = path.join(serverDir, rel);
      const old = prev.get(rel);
      let sig: string;
      try {
        const st = statSync(full);
        if (!st.isFile()) continue;
        sig = `${st.size}:${st.mtimeMs}`;
      } catch {
        continue;
      }
      let hash = old?.hash ?? '';
      if (!old || old.sig !== sig) {
        try {
          hash = createHash('sha1').update(readFileSync(full)).digest('hex');
        } catch {
          // 書いている途中で読めないときは、次に見たときに比べる
          if (old) next.set(rel, old);
          continue;
        }
      }
      next.set(rel, { group: w.group, sig, hash });
      if (old?.hash !== hash) changed.set(rel, w.group);
    }
  }
  for (const [rel, e] of prev) if (!next.has(rel)) changed.set(rel, e.group);
  return { next, changed };
}

// ---- サーバー（src/index.ts）を動かす ---------------------------------------------------------------

let child: ChildProcess | null = null;
/** 今のサーバーが起動し終わったか */
let ready = false;
/** 一度でも起動できたか（ポートが使われているときに、終わるかを決める） */
let everReady = false;
let readyAt = 0;
const expectedExit = new WeakSet<ChildProcess>();
let retryTimer: NodeJS.Timeout | null = null;
let retryDelay = 2000;
let quitting = false;

const serverArgs = (extra: string[] = []) => [...process.execArgv, path.join('src', 'index.ts'), ...extra];

/** 新しい中身を確かめる（プログラムを読み込めるか・設定・PostgreSQL への接続）。終了コードを返す */
function check(): Promise<number> {
  return new Promise((resolve) => {
    const c = spawn(process.execPath, serverArgs(['--check']), { cwd: serverDir, stdio: 'inherit' });
    c.once('error', () => resolve(1));
    c.once('exit', (code) => resolve(code ?? 1));
  });
}

/** サーバーを起動して、起動し終わるか止まるまで待つ */
function startServer(): Promise<void> {
  return new Promise((resolve) => {
    const c = spawn(process.execPath, serverArgs(), { cwd: serverDir, stdio: ['inherit', 'inherit', 'inherit', 'ipc'] });
    child = c;
    ready = false;
    c.on('message', (m) => {
      if ((m as { type?: string } | null)?.type !== 'ready' || child !== c) return;
      ready = true;
      everReady = true;
      readyAt = Date.now();
      resolve();
    });
    c.once('error', (err) => {
      log(`サーバーを起動できませんでした: ${err.message}`);
      if (child === c) child = null;
      resolve();
      scheduleRetry('サーバーを起動できませんでした');
    });
    c.once('exit', (code, signal) => {
      const wasReady = child === c && ready;
      if (child === c) {
        child = null;
        ready = false;
      }
      resolve();
      if (expectedExit.has(c) || quitting) return;
      onUnexpectedExit(code, signal, wasReady);
    });
  });
}

function onUnexpectedExit(code: number | null, signal: string | null, wasReady: boolean): void {
  // しばらく動いていたなら、待つ時間を最初に戻す（起動してすぐ落ちるのをくり返すときだけ、間を延ばす）
  if (wasReady && Date.now() - readyAt > 10_000) retryDelay = 2000;
  if (code === EXIT.port && !everReady) {
    log('ポートが使われているので、終わります（すぐ上のメッセージを見てください）');
    process.exit(EXIT.port);
  }
  if (code === EXIT.config) {
    log('直して保存すると、もう一度起動します');
    return;
  }
  // タスク マネージャーなどで終了させられたときは、終了コードが 4294967295（-1）になる
  const how = code === null ? `シグナル ${signal}` : code === 4294967295 || code === -1 ? 'ほかから終了させられました' : `終了コード ${code}`;
  scheduleRetry(wasReady ? `サーバーが止まりました（${how}）` : 'サーバーを起動できませんでした');
}

/** サーバーを止める（AI の依頼を処理している途中なら、終わるまで待つ） */
function stopServer(): Promise<void> {
  const c = child;
  if (!c) return Promise.resolve();
  expectedExit.add(c);
  return new Promise((resolve) => {
    const force = setTimeout(() => {
      log('サーバーが止まらないので、強制的に止めます');
      c.kill();
    }, STOP_TIMEOUT_MS);
    c.once('exit', () => {
      clearTimeout(force);
      resolve();
    });
    if (c.exitCode !== null || c.signalCode !== null) {
      clearTimeout(force);
      resolve();
      return;
    }
    try {
      c.send({ type: 'shutdown' });
    } catch {
      c.kill();
    }
  });
}

function scheduleRetry(why: string, delay?: number): void {
  if (quitting) return;
  if (retryTimer) clearTimeout(retryTimer);
  const wait = delay ?? retryDelay;
  if (delay === undefined) retryDelay = Math.min(60_000, retryDelay * 2);
  log(`${why}。${Math.round(wait / 1000)} 秒後に、もう一度試します`);
  retryTimer = setTimeout(() => {
    retryTimer = null;
    void run(refresh);
  }, wait);
}

// ---- 更新を取り込む ----------------------------------------------------------------------------

/** 今のファイルの中身でサーバーを動かす（ライブラリ → 確かめる → 起動し直す）。だめなら、今のサーバーはそのまま */
async function refresh(): Promise<void> {
  if (quitting) return;
  if (retryTimer) {
    clearTimeout(retryTimer);
    retryTimer = null;
  }
  let swapped = false;
  if (!depsUpToDate()) {
    if (!child) {
      log('ライブラリを入れます（package-lock.json のとおり。少しかかります）');
      if (!(await installInPlace())) return scheduleRetry('ライブラリを入れられませんでした（インターネットにつながっているか確かめてください）', 60_000);
    } else {
      log('ライブラリが変わったので、サーバーを動かしたまま、新しいライブラリを別の場所に入れます');
      if (!(await installStaged())) return scheduleRetry('新しいライブラリを入れられませんでした。今のサーバーは、そのまま動かしています', 60_000);
      if (!(await swapIn())) return scheduleRetry('新しいライブラリに入れ替えられませんでした。今のサーバーは、そのまま動かしています', 60_000);
      swapped = true;
    }
    if (quitting) return;
  }
  const result = await check();
  if (quitting) return;
  if (result !== 0) {
    if (swapped) await restorePrevious();
    const keep = child && ready ? '。今のサーバーは、そのまま動かしています' : '';
    if (result === EXIT.db) return scheduleRetry(`PostgreSQL につながりません${keep}`);
    log(`新しい中身に問題があります（すぐ上のメッセージを見てください）${keep}。直して保存すると、もう一度確かめます`);
    return;
  }
  if (child) {
    log('新しい中身で、サーバーを起動し直します（AI の依頼を処理している途中なら、終わるまで待ちます）');
    await stopServer();
  }
  if (swapped) discardPrevious();
  if (quitting) return;
  await startServer();
}

let working = false;
let queue: Promise<void> = Promise.resolve();
/** 更新の処理は 1 つずつ行う */
function run(task: () => Promise<void>): Promise<void> {
  queue = queue.then(async () => {
    working = true;
    try {
      await task();
    } catch (err) {
      log(`思わぬエラーが起きました: ${(err as Error).stack ?? String(err)}`);
    } finally {
      working = false;
    }
  });
  return queue;
}

async function applyChanges(batch: Map<string, Group>): Promise<void> {
  const files = [...batch.keys()];
  log(`ファイルが変わりました: ${files.slice(0, 5).join('、')}${files.length > 5 ? ` ほか ${files.length - 5} 個` : ''}`);
  if ([...batch.values()].includes('self')) {
    log('起動の仕組み（tools）が変わったので、全体を起動し直します');
    if (child) await stopServer();
    process.exit(RELAUNCH);
  }
  await refresh();
}

let snapshot = scan(new Map()).next;
const pending = new Map<string, Group>();
let lastChange = 0;
const poller = setInterval(() => {
  if (quitting) return;
  const { next, changed } = scan(snapshot);
  snapshot = next;
  for (const [rel, g] of changed) pending.set(rel, g);
  if (changed.size) lastChange = Date.now();
  if (pending.size && !working && Date.now() - lastChange >= SETTLE_MS) {
    const batch = new Map(pending);
    pending.clear();
    void run(() => applyChanges(batch));
  }
}, POLL_MS);

// ---- 止める（Ctrl+C・ウィンドウを閉じる） ----------------------------------------------------------

function quit(): void {
  if (quitting) {
    // 2 回目の Ctrl+C: すぐに止める（サーバーも同じ Ctrl+C を受け取って、すぐに止まる）
    child?.kill();
    process.exit(130);
  }
  quitting = true;
  clearInterval(poller);
  if (retryTimer) clearTimeout(retryTimer);
  const c = child;
  if (!c) process.exit(0);
  // サーバーも同じ Ctrl+C を受け取って止まり始めている。念のため、こちらからも伝える
  expectedExit.add(c);
  c.once('exit', () => process.exit(0));
  try {
    c.send({ type: 'shutdown' });
  } catch {
    // もう止まっている
  }
}
process.on('SIGINT', quit);
process.on('SIGBREAK', quit);
process.on('SIGHUP', quit);

log('AI サーバーを起動します。server フォルダーのファイルが変わったら、自動で取り込んで起動し直します（止めるときは Ctrl+C）');
void run(refresh);
