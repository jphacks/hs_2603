// PostgreSQL への接続と、テーブルを作る・更新する処理（migrations フォルダーの SQL を順に当てる）。
import { readdir, readFile } from 'node:fs/promises';
import path from 'node:path';
import pg from 'pg';
import type { Pool, PoolClient } from 'pg';
import { serverDir } from './config.ts';

// bigint（ID・トークン数）と numeric（sum の結果）を数として受け取る（2^53 までは正確）
pg.types.setTypeParser(20, (v) => Number(v));
pg.types.setTypeParser(1700, (v) => Number(v));

export type Db = Pool;
export type Tx = PoolClient;

export function createPool(databaseUrl: string): Db {
  // つながらないときに待ち続けないよう、10 秒で諦める
  const pool = new pg.Pool({ connectionString: databaseUrl, max: 10, connectionTimeoutMillis: 10_000 });
  // 使っていない接続が切れたときに、サーバーごと落ちないようにする
  pool.on('error', (err) => console.error('PostgreSQL の接続でエラーが起きました:', err.message));
  return pool;
}

/**
 * PostgreSQL のエラーを、直し方が分かる文にする（パスワードは出さない）。
 * retry は、待てば直りそうか（PostgreSQL がまだ起動していないなど）。false なら、設定などを直す必要がある
 */
export function describeDbError(err: unknown, databaseUrl: string): { message: string; retry: boolean } {
  const e = (err ?? {}) as { code?: string; message?: string };
  let where = 'PostgreSQL';
  let database = '';
  try {
    const u = new URL(databaseUrl);
    where = `PostgreSQL（${u.hostname || 'localhost'}:${u.port || '5432'}）`;
    database = decodeURIComponent(u.pathname.replace(/^\//, ''));
  } catch {
    // 読めないアドレスなら、場所は出さない
  }
  const detail = String(e.message ?? err);
  switch (e.code) {
    case 'ECONNREFUSED':
      return { retry: true, message: `${where} に接続できません。PostgreSQL が起動しているか確かめてください` };
    case 'ENOTFOUND':
    case 'EAI_AGAIN':
      return { retry: true, message: `${where} のサーバー名が見つかりません。server/.env の DATABASE_URL を確かめてください` };
    case 'ETIMEDOUT':
    case 'ECONNRESET':
    case 'EHOSTUNREACH':
    case 'ENETUNREACH':
      return { retry: true, message: `${where} から返事がありません（${e.code}）` };
    case '57P03':
      return { retry: true, message: `${where} は起動している途中です` };
    case '28P01':
    case '28000':
      return { retry: false, message: `${where} のユーザー名かパスワードが違います。server/.env の DATABASE_URL を確かめてください` };
    case '3D000':
      return { retry: false, message: `データベース「${database}」がありません。server/README.md の手順で作ってください` };
  }
  if (/timeout/i.test(detail)) return { retry: true, message: `${where} から返事がありません` };
  return { retry: false, message: `PostgreSQL でエラーが起きました: ${detail}` };
}

/** fn をトランザクションの中で行う（途中で失敗したら、変更はすべて取り消す） */
export async function tx<T>(db: Db, fn: (c: Tx) => Promise<T>): Promise<T> {
  const c = await db.connect();
  try {
    await c.query('begin');
    const result = await fn(c);
    await c.query('commit');
    return result;
  } catch (err) {
    await c.query('rollback').catch(() => {});
    throw err;
  } finally {
    c.release();
  }
}

const migrationsDir = path.join(serverDir, 'src', 'migrations');

/** まだ当てていない migrations/*.sql を、名前の順に当てる。当てたファイル名を返す */
export async function migrate(db: Db): Promise<string[]> {
  const files = (await readdir(migrationsDir)).filter((f) => f.endsWith('.sql')).sort();
  const c = await db.connect();
  const applied: string[] = [];
  try {
    // 2 つのサーバーが同時に起動しても、二重に当てないようにする
    await c.query('select pg_advisory_lock(727401)');
    await c.query('create table if not exists schema_migrations (version text primary key, applied_at timestamptz not null default now())');
    const done = new Set((await c.query<{ version: string }>('select version from schema_migrations')).rows.map((r) => r.version));
    for (const file of files) {
      if (done.has(file)) continue;
      const sql = await readFile(path.join(migrationsDir, file), 'utf8');
      try {
        await c.query('begin');
        await c.query(sql);
        await c.query('insert into schema_migrations (version) values ($1)', [file]);
        await c.query('commit');
        applied.push(file);
      } catch (err) {
        await c.query('rollback').catch(() => {});
        throw new Error(`${file} を当てられませんでした: ${(err as Error).message}`);
      }
    }
  } finally {
    await c.query('select pg_advisory_unlock(727401)').catch(() => {});
    c.release();
  }
  return applied;
}
