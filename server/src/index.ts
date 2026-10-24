// AI サーバーを起動する（start.cmd・npm start）。
//   node --experimental-strip-types src/index.ts           起動する
//   node --experimental-strip-types src/index.ts --check   設定・プログラム・PostgreSQL への接続を確かめるだけ
//                                                          （start.cmd が、起動し直す前に使う。だめなら終了コードで知らせる）
import { readFileSync } from 'node:fs';
import { networkInterfaces } from 'node:os';
import { buildApp } from './app.ts';
import { bedrockConverse } from './bedrock.ts';
import type { Config } from './config.ts';
import { loadEnvFile, readConfig } from './config.ts';
import { createPool, describeDbError, migrate } from './db.ts';
import { EXIT } from './exit-codes.ts';

const checkOnly = process.argv.includes('--check');
/** start.cmd（tools/supervisor.ts）から起動されたか。そのときは、起動できたことと「止めて」を、メッセージでやり取りする */
const supervised = typeof process.send === 'function';

function fail(message: string, code: number): never {
  console.error(message);
  process.exit(code);
}

async function main(): Promise<void> {
  const envFile = loadEnvFile();
  let config: Config;
  try {
    config = readConfig();
  } catch (err) {
    const hint = envFile ? '' : 'server/.env がありません。server/.env.example をコピーして作ってください（start.cmd なら自動で作ります）\n';
    fail(hint + (err as Error).message, EXIT.config);
  }
  const db = createPool(config.databaseUrl);

  if (checkOnly) {
    try {
      await db.query('select 1');
    } catch (err) {
      const d = describeDbError(err, config.databaseUrl);
      fail(d.message, d.retry ? EXIT.db : EXIT.config);
    }
    await db.end();
    process.exit(0);
  }

  if (!envFile) console.warn('server/.env が見つかりません。server/.env.example をコピーして作ってください（今は環境変数だけを使います）');
  try {
    const applied = await migrate(db);
    if (applied.length) console.log(`データベースを更新しました: ${applied.join(', ')}`);
  } catch (err) {
    const d = describeDbError(err, config.databaseUrl);
    fail(d.message, d.retry ? EXIT.db : EXIT.config);
  }
  const admins = await db.query('select count(*) as n from users u join roles r on r.id = u.role_id where r.is_admin and not u.disabled');
  if (!Number(admins.rows[0].n)) {
    console.warn('管理者がまだいません。server フォルダーの create-admin.cmd をダブルクリックして作ってください（npm なら npm run admin:create -- --email あなたのメールアドレス）');
  }
  if (!config.bedrockKey) console.warn('AWS_BEARER_TOKEN_BEDROCK が空です。ログインや管理画面は使えますが、AI は動きません');

  const https = config.tlsCert ? { cert: readFileSync(config.tlsCert), key: readFileSync(config.tlsKey) } : undefined;
  const app = await buildApp({ config, db, converse: bedrockConverse(config.bedrockKey, config.region) }, { https });
  try {
    await app.listen({ host: config.host, port: config.port });
  } catch (err) {
    const code = (err as NodeJS.ErrnoException).code;
    if (code === 'EADDRINUSE') {
      fail(`ポート ${config.port} は、ほかのプログラムが使っています。AI サーバーがもう起動していないか確かめてください（ポートを変えるときは、server/.env の PORT を書き換えます）`, EXIT.port);
    }
    if (code === 'EACCES') fail(`ポート ${config.port} を使えません（Windows が予約しているなど）。server/.env の PORT を別の数にしてください`, EXIT.config);
    if (code === 'EADDRNOTAVAIL') fail(`この PC には ${config.host} というアドレスがありません。server/.env の HOST を確かめてください`, EXIT.config);
    throw err;
  }

  const scheme = https ? 'https' : 'http';
  const hosts = config.host === '0.0.0.0' || config.host === '::'
    ? ['localhost', ...Object.values(networkInterfaces()).flat().filter((n) => n && n.family === 'IPv4' && !n.internal).map((n) => n!.address)]
    : [config.host];
  console.log(`\nかんたん動画エディタの AI サーバーを起動しました（モデル: ${config.model}）`);
  for (const h of hosts) console.log(`  管理画面: ${scheme}://${h}:${config.port}/`);
  if (!https && !config.trustProxy) {
    console.log('  ※ HTTP で動いています。家の外（インターネット）から使うときは、HTTPS にしてください（server/README.md を参照）');
  }
  process.send?.({ type: 'ready', port: config.port });

  // 止めるときは、処理中の依頼（AI の返事待ちなど）が終わるのを待ってから止める（使ったトークンを記録し損ねないように）
  let stopping = false;
  const stop = async (message: string) => {
    if (stopping) return;
    stopping = true;
    if (message) console.log(message);
    try {
      await app.close();
      await db.end();
    } finally {
      process.exit(0);
    }
  };
  // Ctrl+C は 2 回目ですぐに止める
  let interrupts = 0;
  const onInterrupt = () => {
    interrupts += 1;
    if (interrupts >= 2) {
      console.log('すぐに止めます');
      process.exit(0);
    }
    void stop('サーバーを止めています（AI の依頼を処理している途中なら、終わるまで待ちます。すぐに止めるときは、もう一度 Ctrl+C）');
  };
  process.on('SIGINT', onInterrupt);
  process.on('SIGBREAK', onInterrupt);
  process.on('SIGTERM', () => void stop(''));
  process.on('SIGHUP', () => void stop(''));
  if (supervised) {
    // start.cmd からの「止めて」（新しい中身で起動し直すとき）と、start.cmd が先に終わったとき
    process.on('message', (m) => {
      if ((m as { type?: string } | null)?.type === 'shutdown') void stop('');
    });
    process.on('disconnect', () => void stop(''));
  }
}

main().catch((err) => {
  console.error((err as Error).stack ?? String(err));
  process.exit(1);
});
