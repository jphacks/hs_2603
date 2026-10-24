// 管理画面の API（ユーザー・ロール・トークン・使用履歴・操作の記録）。管理者だけが使える。
import type { FastifyInstance, FastifyRequest } from 'fastify';
import type { AppContext } from '../app.ts';
import { generatePassword, hashPassword, passwordProblem } from '../auth/passwords.ts';
import { revokeUserSessions } from '../auth/sessions.ts';
import { supportsEffort } from '../bedrock.ts';
import type { Tx } from '../db.ts';
import { tx } from '../db.ts';
import { ApiError, authenticate, check, requireAdminHeader } from '../http.ts';
import type { Choice, Level, PolicyFields, UserRow } from '../policy.ts';
import { allowanceOf, effectivePolicy, isChoice, isLevel, monthRange } from '../policy.ts';
import type { Registration } from '../settings.ts';
import { isRegistrationMode, readRegistration, writeRegistration } from '../settings.ts';

type Body = Record<string, unknown>;

function level(v: unknown, name: string): Level {
  if (!isLevel(v)) throw new ApiError(400, 'invalid', `${name} は low・medium・high・max のどれかにしてください`);
  return v;
}
function choice(v: unknown, name: string): Choice {
  if (!isChoice(v)) throw new ApiError(400, 'invalid', `${name} は auto・low・medium・high・max のどれかにしてください`);
  return v;
}

type ListedUser = PolicyFields & {
  id: number;
  email: string;
  display_name: string;
  disabled: boolean;
  pending: boolean;
  self_registered: boolean;
  must_change_password: boolean;
  created_at: Date;
  last_login_at: Date | null;
  role_id: number;
  role_name: string;
  used: number;
  requests: number;
  granted: number;
};

function userJson(u: ListedUser, month: string) {
  const policy = effectivePolicy(u);
  return {
    id: u.id,
    email: u.email,
    displayName: u.display_name,
    roleId: u.role_id,
    roleName: u.role_name,
    isAdmin: u.role_is_admin,
    disabled: u.disabled,
    pending: u.pending,
    selfRegistered: u.self_registered,
    mustChangePassword: u.must_change_password,
    tokensCustom: u.tokens_custom,
    monthlyTokens: u.monthly_tokens,
    maxEffort: u.max_effort,
    defaultEffort: u.default_effort,
    policy,
    usage: { month, used: u.used, granted: u.granted, requests: u.requests, ...allowanceOf(policy.monthlyTokens, u.used, u.granted) },
    createdAt: u.created_at,
    lastLoginAt: u.last_login_at,
  };
}

function roleJson(r: Record<string, unknown>) {
  return {
    id: r.id,
    name: r.name,
    isAdmin: r.is_admin,
    canUseAi: r.can_use_ai,
    monthlyTokens: r.monthly_tokens,
    maxEffort: r.max_effort,
    defaultEffort: r.default_effort,
    users: r.users ?? 0,
  };
}

/** 変更のあとも、管理者が 1 人以上いて、操作した本人も管理者のままか確かめる（だめならトランザクションごと取り消す） */
async function keepAdmins(c: Tx, actingId: number): Promise<void> {
  const { rows } = await c.query<{ admins: number; me: boolean }>(
    `select count(*) filter (where r.is_admin and not u.disabled) as admins,
            coalesce(bool_or(u.id = $1 and r.is_admin and not u.disabled), false) as me
       from users u join roles r on r.id = u.role_id`,
    [actingId],
  );
  if (!rows[0]?.me) throw new ApiError(409, 'self_lockout', '自分の管理者の権限は外せません（外すときは、ほかの管理者に頼んでください）');
  if (!rows[0].admins) throw new ApiError(409, 'last_admin', '管理者がいなくなってしまうので、この変更はできません');
}

export function adminRoutes(app: FastifyInstance, ctx: AppContext): void {
  const { db, config } = ctx;

  /** 管理者のログインを確かめる。変更する API では、管理画面からの操作かも確かめる */
  async function admin(req: FastifyRequest, change = false): Promise<UserRow> {
    if (change) requireAdminHeader(req);
    const { user } = await authenticate(db, req, 'admin');
    if (!user.role_is_admin) throw new ApiError(403, 'not_admin', '管理者だけが使えます');
    if (user.must_change_password) throw new ApiError(403, 'must_change_password', '先にパスワードを変えてください');
    return user;
  }

  async function audit(c: Tx | typeof db, by: UserRow, action: string, target: string, detail: Record<string, unknown> = {}): Promise<void> {
    await c.query('insert into audit_log (admin_id, admin_email, action, target, detail) values ($1, $2, $3, $4, $5)', [
      by.id,
      by.email,
      action,
      target,
      JSON.stringify(detail),
    ]);
  }

  async function listUsers(month: string | undefined, where = 'true', params: unknown[] = []) {
    const range = await monthRange(db, config.timezone, month);
    const { rows } = await db.query<ListedUser>(
      `select u.id, u.email, u.display_name, u.disabled, u.pending, u.self_registered, u.must_change_password, u.tokens_custom, u.monthly_tokens,
              u.max_effort, u.default_effort, u.created_at, u.last_login_at,
              r.id as role_id, r.name as role_name, r.is_admin as role_is_admin, r.can_use_ai as role_can_use_ai,
              r.monthly_tokens as role_monthly_tokens, r.max_effort as role_max_effort, r.default_effort as role_default_effort,
              coalesce(e.used, 0) as used, coalesce(e.requests, 0) as requests, coalesce(g.granted, 0) as granted
         from users u join roles r on r.id = u.role_id
         left join (select user_id, sum(input_tokens + output_tokens) as used, count(*) filter (where ok) as requests
                      from usage_events where created_at >= $1 and created_at < $2 group by user_id) e on e.user_id = u.id
         left join (select user_id, sum(tokens) as granted from token_grants where month = $3 group by user_id) g on g.user_id = u.id
        where ${where}
        order by r.is_admin desc, u.email`,
      [range.start, range.end, range.month, ...params],
    );
    return { month: range.month, users: rows.map((u) => userJson(u, range.month)) };
  }

  async function oneUser(id: number) {
    const { users } = await listUsers(undefined, 'u.id = $4', [id]);
    if (!users[0]) throw new ApiError(404, 'not_found', 'ユーザーが見つかりません');
    return users[0];
  }

  /** ロールの名前（無ければ 400） */
  async function roleName(c: Tx, id: number): Promise<string> {
    const { rows } = await c.query<{ name: string }>('select name from roles where id = $1', [id]);
    if (!rows[0]) throw new ApiError(400, 'invalid', 'ロールが見つかりません');
    return rows[0].name;
  }

  /** 操作の記録に残す、変わった項目だけ（ロールは ID ではなく名前で残す） */
  async function changes(c: Tx, before: Record<string, unknown>, fields: Record<string, unknown>): Promise<Record<string, unknown>> {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(fields)) {
      if (before[k] === v) continue;
      if (k === 'role_id') out.role = await roleName(c, Number(v));
      else out[k] = v;
    }
    return out;
  }

  /** ユーザーの、人ごとの設定（作るとき・変えるとき共通） */
  function userFields(b: Body) {
    const out: Record<string, unknown> = {};
    if ('displayName' in b) out.display_name = check.string(b.displayName, '名前', 100);
    if ('email' in b) out.email = check.email(b.email);
    if ('roleId' in b) out.role_id = check.id(b.roleId, 'ロール');
    if ('disabled' in b) out.disabled = check.bool(b.disabled, 'disabled');
    if ('tokensCustom' in b) out.tokens_custom = check.bool(b.tokensCustom, 'tokensCustom');
    if ('monthlyTokens' in b) out.monthly_tokens = check.tokens(b.monthlyTokens, '月のトークン');
    if ('maxEffort' in b) out.max_effort = b.maxEffort === null ? null : level(b.maxEffort, '深さの上限');
    if ('defaultEffort' in b) out.default_effort = b.defaultEffort === null ? null : choice(b.defaultEffort, '最初の深さ');
    return out;
  }

  async function updateRow(c: Tx, table: 'users' | 'roles', id: number, fields: Record<string, unknown>): Promise<void> {
    const keys = Object.keys(fields);
    if (!keys.length) return;
    // 列の名前はこのファイルで決めたものだけ（利用者の入力は値にしか入らない）
    const sets = keys.map((k, i) => `${k} = $${i + 2}`).join(', ');
    const { rowCount } = await c.query(`update ${table} set ${sets}, updated_at = now() where id = $1`, [id, ...keys.map((k) => fields[k])]);
    if (!rowCount) throw new ApiError(404, 'not_found', table === 'users' ? 'ユーザーが見つかりません' : 'ロールが見つかりません');
  }

  function uniqueViolation(err: unknown, message: string): never {
    if ((err as { code?: string }).code === '23505') throw new ApiError(409, 'duplicate', message);
    throw err;
  }

  // ---- 概要 ----------------------------------------------------------------------
  app.get('/api/admin/overview', async (req) => {
    await admin(req);
    const q = req.query as Record<string, string | undefined>;
    const range = await monthRange(db, config.timezone, q.month || undefined);
    const totals = await db.query(
      `select coalesce(sum(input_tokens), 0) as input, coalesce(sum(cached_tokens), 0) as cached, coalesce(sum(output_tokens), 0) as output,
              count(*) as requests, count(*) filter (where not ok) as errors, count(distinct user_id) as users
         from usage_events where created_at >= $1 and created_at < $2`,
      [range.start, range.end],
    );
    const daily = await db.query(
      `select to_char(created_at at time zone $3, 'YYYY-MM-DD') as day, sum(input_tokens + output_tokens) as tokens, count(*) as requests
         from usage_events where created_at >= $1 and created_at < $2 group by 1 order by 1`,
      [range.start, range.end, config.timezone],
    );
    const top = await db.query(
      `select coalesce(u.email, e.user_email) as email, coalesce(u.display_name, '') as display_name,
              sum(e.input_tokens + e.output_tokens) as tokens, count(*) as requests
         from usage_events e left join users u on u.id = e.user_id
        where e.created_at >= $1 and e.created_at < $2 group by 1, 2 order by 3 desc limit 10`,
      [range.start, range.end],
    );
    const counts = await db.query(
      `select count(*) as users, count(*) filter (where u.disabled) as disabled, count(*) filter (where u.pending) as pending,
              count(*) filter (where r.is_admin and not u.disabled) as admins
         from users u join roles r on r.id = u.role_id`,
    );
    const t = totals.rows[0];
    return {
      month: range.month,
      server: {
        model: config.model,
        region: config.region,
        timezone: config.timezone,
        bedrockConfigured: Boolean(config.bedrockKey),
        modelSupportsEffort: supportsEffort(config.model),
      },
      totals: { ...t, total: Number(t.input) + Number(t.output) },
      accounts: counts.rows[0],
      daily: daily.rows,
      topUsers: top.rows.map((r) => ({ email: r.email, displayName: r.display_name, tokens: r.tokens, requests: r.requests })),
    };
  });

  // ---- ユーザー ----------------------------------------------------------------------
  app.get('/api/admin/users', async (req) => {
    await admin(req);
    const q = req.query as Record<string, string | undefined>;
    return listUsers(q.month || undefined);
  });

  app.post('/api/admin/users', async (req) => {
    const me = await admin(req, true);
    const b = (req.body ?? {}) as Body;
    const fields = userFields(b);
    if (!fields.email) throw new ApiError(400, 'invalid', 'メールアドレスを入れてください');
    if (!fields.role_id) throw new ApiError(400, 'invalid', 'ロールを選んでください');
    let password = typeof b.password === 'string' ? b.password : '';
    const generated = !password;
    if (generated) password = generatePassword();
    const problem = passwordProblem(password, String(fields.email));
    if (problem) throw new ApiError(400, 'weak_password', problem);
    const hash = await hashPassword(password);
    const id = await tx(db, async (c) => {
      const role = await roleName(c, Number(fields.role_id));
      const all = { display_name: '', ...fields, password_hash: hash, must_change_password: true };
      const keys = Object.keys(all);
      try {
        const { rows } = await c.query<{ id: number }>(
          `insert into users (${keys.join(', ')}) values (${keys.map((_, i) => `$${i + 1}`).join(', ')}) returning id`,
          keys.map((k) => (all as Record<string, unknown>)[k]),
        );
        // 記録には、ロールの名前と、決めた項目だけを残す（空・ロールのままの項目は省く）
        const set = Object.entries(fields).filter(([k, v]) => k !== 'email' && k !== 'role_id' && v !== null && v !== false && v !== '');
        await audit(c, me, 'user.create', String(fields.email), { role, ...Object.fromEntries(set) });
        return rows[0]!.id;
      } catch (err) {
        uniqueViolation(err, 'このメールアドレスは、もう登録されています');
      }
    });
    // 仮のパスワードは、このときだけ管理者に見せる（データベースには残らない）
    return { user: await oneUser(id), temporaryPassword: generated ? password : null };
  });

  app.patch('/api/admin/users/:id', async (req) => {
    const me = await admin(req, true);
    const id = check.id((req.params as Body).id);
    const fields = userFields((req.body ?? {}) as Body);
    await tx(db, async (c) => {
      const { rows } = await c.query(
        'select email, display_name, role_id, disabled, tokens_custom, monthly_tokens, max_effort, default_effort from users where id = $1 for update',
        [id],
      );
      if (!rows[0]) throw new ApiError(404, 'not_found', 'ユーザーが見つかりません');
      if (fields.role_id) await roleName(c, Number(fields.role_id));
      try {
        await updateRow(c, 'users', id, fields);
      } catch (err) {
        if (err instanceof ApiError) throw err;
        uniqueViolation(err, 'このメールアドレスは、もう登録されています');
      }
      await keepAdmins(c, me.id);
      // 変わった項目だけを記録する（何も変えずに保存したときは残さない）
      const changed = await changes(c, rows[0], fields);
      if (Object.keys(changed).length) await audit(c, me, 'user.update', rows[0].email, changed);
    });
    // 止めたアカウントは、ログイン中の印も消す
    if (fields.disabled === true) await revokeUserSessions(db, id);
    return { user: await oneUser(id) };
  });

  app.post('/api/admin/users/:id/password', async (req) => {
    const me = await admin(req, true);
    const id = check.id((req.params as Body).id);
    if (id === me.id) throw new ApiError(400, 'invalid', '自分のパスワードは「アカウント」の画面で変えてください');
    const target = await oneUser(id);
    const b = (req.body ?? {}) as Body;
    let password = typeof b.password === 'string' ? b.password : '';
    const generated = !password;
    if (generated) password = generatePassword();
    const problem = passwordProblem(password, target.email);
    if (problem) throw new ApiError(400, 'weak_password', problem);
    await db.query('update users set password_hash = $2, must_change_password = true, updated_at = now() where id = $1', [id, await hashPassword(password)]);
    await revokeUserSessions(db, id);
    await audit(db, me, 'user.reset_password', target.email);
    return { user: await oneUser(id), temporaryPassword: generated ? password : null };
  });

  app.delete('/api/admin/users/:id', async (req) => {
    const me = await admin(req, true);
    const id = check.id((req.params as Body).id);
    const target = await oneUser(id);
    await tx(db, async (c) => {
      await c.query('delete from users where id = $1', [id]);
      await keepAdmins(c, me.id);
      await audit(c, me, 'user.delete', target.email);
    });
    return { ok: true };
  });

  app.get('/api/admin/users/:id/grants', async (req) => {
    await admin(req);
    const id = check.id((req.params as Body).id);
    const { rows } = await db.query(
      `select g.id, g.month, g.tokens, g.note, g.created_at as "createdAt", coalesce(a.email, '') as "grantedBy"
         from token_grants g left join users a on a.id = g.granted_by where g.user_id = $1 order by g.created_at desc limit 100`,
      [id],
    );
    return { grants: rows };
  });

  app.post('/api/admin/users/:id/grants', async (req) => {
    const me = await admin(req, true);
    const id = check.id((req.params as Body).id);
    const b = (req.body ?? {}) as Body;
    const tokens = check.tokens(b.tokens, '追加するトークン', true);
    if (!tokens) throw new ApiError(400, 'invalid', '追加するトークンを入れてください（減らすときはマイナス）');
    const range = await monthRange(db, config.timezone, typeof b.month === 'string' && b.month ? b.month : undefined);
    const note = b.note === undefined ? '' : check.string(b.note, 'メモ', 200);
    const target = await oneUser(id);
    await db.query('insert into token_grants (user_id, month, tokens, note, granted_by) values ($1, $2, $3, $4, $5)', [id, range.month, tokens, note, me.id]);
    await audit(db, me, 'user.grant', target.email, { month: range.month, tokens, note });
    return { user: await oneUser(id) };
  });

  // ---- ロール ----------------------------------------------------------------------
  function roleFields(b: Body) {
    const out: Record<string, unknown> = {};
    if ('name' in b) {
      const name = check.string(b.name, 'ロールの名前', 50);
      if (!name) throw new ApiError(400, 'invalid', 'ロールの名前を入れてください');
      out.name = name;
    }
    if ('isAdmin' in b) out.is_admin = check.bool(b.isAdmin, 'isAdmin');
    if ('canUseAi' in b) out.can_use_ai = check.bool(b.canUseAi, 'canUseAi');
    if ('monthlyTokens' in b) out.monthly_tokens = check.tokens(b.monthlyTokens, '月のトークン');
    if ('maxEffort' in b) out.max_effort = level(b.maxEffort, '深さの上限');
    if ('defaultEffort' in b) out.default_effort = choice(b.defaultEffort, '最初の深さ');
    return out;
  }

  async function listRoles() {
    const { rows } = await db.query(
      `select r.*, (select count(*) from users u where u.role_id = r.id) as users from roles r order by r.is_admin desc, r.id`,
    );
    return { roles: rows.map(roleJson) };
  }

  app.get('/api/admin/roles', async (req) => {
    await admin(req);
    return listRoles();
  });

  app.post('/api/admin/roles', async (req) => {
    const me = await admin(req, true);
    const fields = roleFields((req.body ?? {}) as Body);
    if (!fields.name) throw new ApiError(400, 'invalid', 'ロールの名前を入れてください');
    const keys = Object.keys(fields);
    await tx(db, async (c) => {
      try {
        await c.query(`insert into roles (${keys.join(', ')}) values (${keys.map((_, i) => `$${i + 1}`).join(', ')})`, keys.map((k) => fields[k]));
      } catch (err) {
        uniqueViolation(err, '同じ名前のロールがあります');
      }
      const { name, ...rest } = fields;
      await audit(c, me, 'role.create', String(name), rest);
    });
    return listRoles();
  });

  app.patch('/api/admin/roles/:id', async (req) => {
    const me = await admin(req, true);
    const id = check.id((req.params as Body).id);
    const fields = roleFields((req.body ?? {}) as Body);
    await tx(db, async (c) => {
      const { rows } = await c.query(
        'select name, is_admin, can_use_ai, monthly_tokens, max_effort, default_effort from roles where id = $1 for update',
        [id],
      );
      if (!rows[0]) throw new ApiError(404, 'not_found', 'ロールが見つかりません');
      // 新規登録した人が管理者になってしまわないように
      if (fields.is_admin === true && (await readRegistration(c)).roleId === id) {
        throw new ApiError(409, 'registration_role', '新規登録した人のロールなので、管理者のロールにはできません。先に「新規登録」の画面で、別のロールを選んでください');
      }
      try {
        await updateRow(c, 'roles', id, fields);
      } catch (err) {
        if (err instanceof ApiError) throw err;
        uniqueViolation(err, '同じ名前のロールがあります');
      }
      await keepAdmins(c, me.id);
      const changed = await changes(c, rows[0], fields);
      if (Object.keys(changed).length) await audit(c, me, 'role.update', rows[0].name, changed);
    });
    return listRoles();
  });

  app.delete('/api/admin/roles/:id', async (req) => {
    const me = await admin(req, true);
    const id = check.id((req.params as Body).id);
    await tx(db, async (c) => {
      const { rows } = await c.query<{ users: number; name: string }>(
        'select name, (select count(*) from users where role_id = $1) as users from roles where id = $1',
        [id],
      );
      if (!rows[0]) throw new ApiError(404, 'not_found', 'ロールが見つかりません');
      if (rows[0].users > 0) throw new ApiError(409, 'in_use', `このロールの人が ${rows[0].users} 人いるので消せません。先にほかのロールに移してください`);
      if ((await readRegistration(c)).roleId === id) {
        throw new ApiError(409, 'in_use', '新規登録した人のロールに使っているので消せません。先に「新規登録」の画面で、別のロールを選んでください');
      }
      await c.query('delete from roles where id = $1', [id]);
      await audit(c, me, 'role.delete', rows[0].name);
    });
    return listRoles();
  });

  // ---- 新規登録 ----------------------------------------------------------------------
  app.get('/api/admin/settings', async (req) => {
    await admin(req);
    return { registration: await readRegistration(db) };
  });

  app.patch('/api/admin/settings', async (req) => {
    const me = await admin(req, true);
    const b = (req.body ?? {}) as Body;
    const r = (b.registration ?? {}) as Body;
    if (!isRegistrationMode(r.mode)) throw new ApiError(400, 'invalid', '新規登録の受け付け方は off・approval・open のどれかにしてください');
    const roleId = check.id(r.roleId, '登録した人のロール');
    await tx(db, async (c) => {
      const { rows } = await c.query<{ name: string; is_admin: boolean }>('select name, is_admin from roles where id = $1', [roleId]);
      if (!rows[0]) throw new ApiError(400, 'invalid', 'ロールが見つかりません');
      // 自分で登録した人が、管理画面を使えるようにはしない
      if (rows[0].is_admin) throw new ApiError(400, 'invalid', '管理者のロールは選べません。管理画面を使えないロールを選んでください');
      const before = await readRegistration(c);
      await writeRegistration(c, { mode: r.mode as Registration['mode'], roleId });
      if (before.mode !== r.mode || before.roleId !== roleId) await audit(c, me, 'settings.update', '新規登録', { registration: r.mode, role: rows[0].name });
    });
    return { registration: await readRegistration(db) };
  });

  // 承認待ちの人を承認する（断るときは、ユーザーを削除する）
  app.post('/api/admin/users/:id/approve', async (req) => {
    const me = await admin(req, true);
    const id = check.id((req.params as Body).id);
    const { rows } = await db.query<{ email: string }>('update users set pending = false, updated_at = now() where id = $1 and pending returning email', [id]);
    if (!rows[0]) throw new ApiError(409, 'not_pending', 'この人は承認待ちではありません');
    await audit(db, me, 'user.approve', rows[0].email);
    return { user: await oneUser(id) };
  });

  // ---- 使用履歴 ----------------------------------------------------------------------
  async function usageQuery(q: Record<string, string | undefined>) {
    const range = await monthRange(db, config.timezone, q.month || undefined);
    const params: unknown[] = [range.start, range.end];
    let where = 'e.created_at >= $1 and e.created_at < $2';
    if (q.userId) {
      params.push(check.id(q.userId, 'ユーザー'));
      where += ` and e.user_id = $${params.length}`;
    }
    return { range, where, params };
  }

  app.get('/api/admin/usage', async (req) => {
    await admin(req);
    const q = req.query as Record<string, string | undefined>;
    const { range, where, params } = await usageQuery(q);
    const int = (v: string | undefined, fallback: number, min: number, max: number) => {
      const n = Math.floor(Number(v));
      return Number.isFinite(n) ? Math.min(max, Math.max(min, n)) : fallback;
    };
    const limit = int(q.limit, 50, 1, 200);
    const offset = int(q.offset, 0, 0, 1_000_000_000);
    const sums = await db.query(
      `select count(*) as count, coalesce(sum(e.input_tokens), 0) as input, coalesce(sum(e.cached_tokens), 0) as cached,
              coalesce(sum(e.output_tokens), 0) as output from usage_events e where ${where}`,
      params,
    );
    const { rows } = await db.query(
      `select e.id, e.created_at as "createdAt", e.user_email as email, e.model, e.effort, e.effort_choice as "effortChoice",
              e.input_tokens as input, e.cached_tokens as cached, e.output_tokens as output, e.duration_ms as "durationMs", e.ok, e.error
         from usage_events e where ${where} order by e.created_at desc, e.id desc
        limit $${params.length + 1} offset $${params.length + 2}`,
      [...params, limit, offset],
    );
    return { month: range.month, limit, offset, sums: sums.rows[0], rows };
  });

  app.get('/api/admin/usage.csv', async (req, reply) => {
    await admin(req);
    const q = req.query as Record<string, string | undefined>;
    const { range, where, params } = await usageQuery(q);
    const { rows } = await db.query(
      `select to_char(e.created_at at time zone $${params.length + 1}, 'YYYY-MM-DD HH24:MI:SS') as at, e.user_email, e.model,
              e.effort_choice, e.effort, e.input_tokens, e.cached_tokens, e.output_tokens, e.duration_ms, e.ok, e.error
         from usage_events e where ${where} order by e.created_at, e.id`,
      [...params, config.timezone],
    );
    const cell = (v: unknown) => {
      const s = String(v ?? '');
      // 表計算ソフトで式として動かないように（= + - @ で始まる値）、引用符で囲んで先頭に ' を付ける
      const safe = /^[=+\-@\t\r]/.test(s) ? `'${s}` : s;
      return `"${safe.replace(/"/g, '""')}"`;
    };
    const header = ['日時', 'メールアドレス', 'モデル', '選んだ深さ', '送った深さ', '入力', 'うちキャッシュ', '出力', 'かかった時間(ミリ秒)', '成功', 'エラー'];
    const lines = [header.map(cell).join(','), ...rows.map((r) => Object.values(r).map(cell).join(','))];
    reply.header('Content-Type', 'text/csv; charset=utf-8');
    reply.header('Content-Disposition', `attachment; filename="usage-${range.month}.csv"`);
    // Excel で文字化けしないよう、先頭に BOM を付ける
    return `\uFEFF${lines.join('\r\n')}\r\n`;
  });

  // ---- 操作の記録 ----------------------------------------------------------------------
  app.get('/api/admin/audit', async (req) => {
    await admin(req);
    const { rows } = await db.query(
      `select id, created_at as "createdAt", admin_email as "adminEmail", action, target, detail from audit_log order by id desc limit 200`,
    );
    return { rows };
  });
}
