// テスト用: 一時的な PostgreSQL（embedded-postgres）と、Bedrock の代わりの関数でサーバーを動かす。
import { mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import path from 'node:path';
import EmbeddedPostgres from 'embedded-postgres';
import type { FastifyInstance } from 'fastify';
import { buildApp } from '../src/app.ts';
import type { ConverseFn, InvokeFn } from '../src/bedrock.ts';
import { readConfig } from '../src/config.ts';
import type { Config } from '../src/config.ts';
import type { Db } from '../src/db.ts';
import { createPool, migrate } from '../src/db.ts';

export async function freePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const s = createServer();
    s.once('error', reject);
    s.listen(0, '127.0.0.1', () => {
      const port = (s.address() as { port: number }).port;
      s.close(() => resolve(port));
    });
  });
}

export type TestDb = { url: string; stop: () => Promise<void> };

/** 一時フォルダーに PostgreSQL を作って起動する（テストが終わったら消す） */
export async function startPostgres(): Promise<TestDb> {
  const dir = await mkdtemp(path.join(tmpdir(), 'e-editor-pg-'));
  const port = await freePort();
  const pg = new EmbeddedPostgres({
    databaseDir: path.join(dir, 'data'),
    user: 'postgres',
    password: 'test-password',
    port,
    persistent: false,
    onLog: () => {},
    onError: () => {},
  });
  await pg.initialise();
  await pg.start();
  await pg.createDatabase('e_editor_test');
  return {
    url: `postgres://postgres:test-password@127.0.0.1:${port}/e_editor_test`,
    stop: async () => {
      await pg.stop();
      await rm(dir, { recursive: true, force: true }).catch(() => {});
    },
  };
}

export type Calls = { body: Record<string, unknown>; model: string; timeoutMs: number; withEffort: boolean }[];

export type TestServer = {
  app: FastifyInstance;
  db: Db;
  config: Config;
  calls: Calls;
  /** 次の Bedrock の呼び出しの代わりにする関数（無ければ、ふつうの返事） */
  next: { fn: ConverseFn | null };
  /** 画像を作る呼び出し（InvokeModel）と、次の呼び出しの代わりにする関数（無ければ 1 画素の PNG） */
  images: { body: Record<string, unknown>; model: string; region: string }[];
  nextImage: { fn: InvokeFn | null };
  close: () => Promise<void>;
};

/** 1 画素の PNG（画像を作るモデルの代わりの返事） */
export const TINY_PNG = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==';

/** Bedrock の代わり: 入力 100（うちキャッシュ 10）・出力 50 を使ったことにする */
export const fakeResponse = {
  output: { message: { role: 'assistant', content: [{ text: 'はい' }] } },
  stopReason: 'end_turn',
  usage: { inputTokens: 90, cacheReadInputTokens: 10, cacheWriteInputTokens: 0, outputTokens: 50 },
};

export async function startServer(databaseUrl: string, env: Record<string, string> = {}): Promise<TestServer> {
  const config = readConfig({
    DATABASE_URL: databaseUrl,
    AWS_BEARER_TOKEN_BEDROCK: 'test-key',
    AWS_REGION: 'ap-northeast-1',
    BEDROCK_MODEL: 'global.openai.gpt-6.1-sol',
    TIMEZONE: 'Asia/Tokyo',
    ...env,
  });
  const db = createPool(config.databaseUrl);
  await migrate(db);
  const calls: Calls = [];
  const next: { fn: ConverseFn | null } = { fn: null };
  const converse: ConverseFn = async (body, options) => {
    calls.push({ body, ...options });
    if (next.fn) {
      const fn = next.fn;
      next.fn = null;
      return fn(body, options);
    }
    return fakeResponse;
  };
  const images: TestServer['images'] = [];
  const nextImage: TestServer['nextImage'] = { fn: null };
  const invoke: InvokeFn = async (body, options) => {
    images.push({ body, model: options.model, region: options.region });
    if (nextImage.fn) {
      const fn = nextImage.fn;
      nextImage.fn = null;
      return fn(body, options);
    }
    return { seeds: [1], finish_reasons: [null], images: [TINY_PNG] };
  };
  const app = await buildApp({ config, db, converse, invoke }, { logger: false });
  await app.ready();
  return {
    app,
    db,
    config,
    calls,
    next,
    images,
    nextImage,
    close: async () => {
      await app.close();
      await db.end();
    },
  };
}

/** 管理画面のふりをして呼ぶ（クッキーと、管理画面の印のヘッダー） */
export function adminHeaders(cookie: string, change = true): Record<string, string> {
  return { cookie, ...(change ? { 'x-e-editor-admin': '1' } : {}) };
}

export function cookieOf(res: { headers: Record<string, unknown> }): string {
  const set = res.headers['set-cookie'];
  const first = Array.isArray(set) ? set[0] : String(set ?? '');
  return String(first).split(';')[0] ?? '';
}
