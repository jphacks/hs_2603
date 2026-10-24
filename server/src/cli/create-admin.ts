// 管理者のアカウントを作る（npm run admin:create -- --email you@example.com [--name 名前] [--reset]）。
// パスワードは自動で作って一度だけ表示する（最初にログインしたときに変えてもらう）。
import { parseArgs } from 'node:util';
import { generatePassword, hashPassword } from '../auth/passwords.ts';
import { revokeUserSessions } from '../auth/sessions.ts';
import type { Config } from '../config.ts';
import { loadEnvFile, readConfig } from '../config.ts';
import { createPool, describeDbError, migrate } from '../db.ts';

const { values } = parseArgs({
  options: { email: { type: 'string' }, name: { type: 'string' }, reset: { type: 'boolean' } },
});
const email = (values.email ?? '').trim().toLowerCase();
if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) {
  console.error('使い方: npm run admin:create -- --email you@example.com [--name 名前] [--reset]');
  process.exit(1);
}

loadEnvFile();
let config: Config;
try {
  config = readConfig();
} catch (err) {
  console.error((err as Error).message);
  process.exit(1);
}
const db = createPool(config.databaseUrl);
try {
  try {
    await migrate(db);
  } catch (err) {
    throw new Error(describeDbError(err, config.databaseUrl).message);
  }
  const role = await db.query<{ id: number }>('select id from roles where is_admin order by id limit 1');
  if (!role.rows[0]) throw new Error('管理者のロールがありません');
  const password = generatePassword();
  const hash = await hashPassword(password);
  const found = await db.query<{ id: number }>('select id from users where email = $1', [email]);
  if (found.rows[0]) {
    if (!values.reset) {
      // 終了コード 2: もう登録されている（create-admin.cmd が、作り直すか聞く）
      process.exitCode = 2;
      throw new Error(`${email} はもう登録されています。管理者にして、パスワードを作り直すときは --reset を付けてください`);
    }
    await db.query(
      'update users set password_hash = $2, role_id = $3, disabled = false, must_change_password = true, updated_at = now() where id = $1',
      [found.rows[0].id, hash, role.rows[0].id],
    );
    await revokeUserSessions(db, found.rows[0].id);
  } else {
    await db.query('insert into users (email, display_name, password_hash, role_id, must_change_password) values ($1, $2, $3, $4, true)', [
      email,
      values.name ?? '',
      hash,
      role.rows[0].id,
    ]);
  }
  console.log(`\n管理者 ${email} を${found.rows[0] ? '作り直し' : '作り'}ました。`);
  console.log(`仮のパスワード: ${password}`);
  console.log('管理画面にこのパスワードでログインすると、新しいパスワードを決める画面になります。この表示は一度だけです。\n');
} catch (err) {
  console.error((err as Error).message);
  // もう登録されているとき（2）は、そのまま返す
  if (process.exitCode !== 2) process.exitCode = 1;
} finally {
  await db.end();
}
