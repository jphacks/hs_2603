// ログイン・ログアウト・自分の情報・パスワードの変更。
import type { FastifyInstance } from 'fastify';
import type { AppContext } from '../app.ts';
import { burnTime, hashPassword, passwordProblem, verifyPassword } from '../auth/passwords.ts';
import { RateLimiter } from '../auth/ratelimit.ts';
import type { SessionKind } from '../auth/sessions.ts';
import { createSession, revokeSession, revokeUserSessions } from '../auth/sessions.ts';
import { ADMIN_COOKIE, ApiError, authenticate, check, clientIp, readCookie, requireAdminHeader, setAdminCookie } from '../http.ts';
import type { UserRow } from '../policy.ts';
import { effectivePolicy, findUser, monthRange, monthUsage } from '../policy.ts';
import { readRegistration, registrationRole } from '../settings.ts';

/** アプリや管理画面に返す、自分の情報 */
export async function describeMe(ctx: AppContext, user: UserRow) {
  const policy = effectivePolicy(user);
  const usage = await monthUsage(ctx.db, user, await monthRange(ctx.db, ctx.config.timezone));
  return {
    user: { id: user.id, email: user.email, displayName: user.display_name, role: user.role_name, mustChangePassword: user.must_change_password },
    policy,
    usage,
    server: { model: ctx.config.model },
  };
}

export function authRoutes(app: FastifyInstance, ctx: AppContext): void {
  // 総当たりを防ぐ: 接続元ごとに 15 分で 30 回、メールアドレスごとに 15 分で 10 回まで
  const byIp = new RateLimiter(30, 15 * 60_000);
  const byEmail = new RateLimiter(10, 15 * 60_000);

  app.post('/api/auth/login', async (req, reply) => {
    const body = (req.body ?? {}) as Record<string, unknown>;
    const kind: SessionKind = body.client === 'admin' ? 'admin' : 'app';
    const email = check.email(body.email);
    const password = typeof body.password === 'string' ? body.password : '';
    if (!byIp.hit(clientIp(req)) || !byEmail.hit(email)) {
      throw new ApiError(429, 'too_many', 'ログインの失敗が続いたので、しばらく受け付けません。15 分ほど待ってから、もう一度試してください');
    }
    const user = await findUser(ctx.db, 'u.email = $1', [email]);
    if (!user) {
      await burnTime(password);
      throw new ApiError(401, 'bad_login', 'メールアドレスかパスワードが違います');
    }
    if (!(await verifyPassword(password, user.password_hash))) throw new ApiError(401, 'bad_login', 'メールアドレスかパスワードが違います');
    if (user.disabled) throw new ApiError(403, 'disabled', 'このアカウントは止められています。管理者に連絡してください');
    if (user.pending) throw new ApiError(403, 'pending', 'まだ管理者が登録を承認していません。承認されると、ログインできます');
    if (kind === 'admin' && !user.role_is_admin) throw new ApiError(403, 'not_admin', 'このアカウントでは管理画面を使えません');
    byEmail.reset(email);
    const session = await createSession(ctx.db, user.id, kind, { userAgent: req.headers['user-agent'], ip: clientIp(req) });
    await ctx.db.query('update users set last_login_at = now() where id = $1', [user.id]);
    if (kind === 'admin') {
      setAdminCookie(reply, session.token, session.expiresAt, ctx.config.cookieSecure);
      return describeMe(ctx, user);
    }
    return { token: session.token, expiresAt: session.expiresAt.toISOString(), ...(await describeMe(ctx, user)) };
  });

  // 新規登録（アプリから）。受け付け方は管理画面の「新規登録」で決める。
  // パスワードの変換は重いので、接続元ごとに 1 時間で 10 回まで
  const registerByIp = new RateLimiter(10, 60 * 60_000);
  app.post('/api/auth/register', async (req, reply) => {
    const body = (req.body ?? {}) as Record<string, unknown>;
    if (!registerByIp.hit(clientIp(req))) {
      throw new ApiError(429, 'too_many', '短い間に何度も登録しようとしたので、しばらく受け付けません。1 時間ほど待ってから、もう一度試してください');
    }
    const reg = await readRegistration(ctx.db);
    if (reg.mode === 'off') throw new ApiError(403, 'registration_closed', 'このサーバーでは、新しい登録を受け付けていません。管理者にアカウントを作ってもらってください');
    const roleId = await registrationRole(ctx.db, reg);
    if (roleId === null) throw new ApiError(403, 'registration_closed', '新規登録の設定が終わっていません。管理者に連絡してください');
    const email = check.email(body.email);
    const displayName = body.displayName === undefined ? '' : check.string(body.displayName, '名前', 100);
    const password = typeof body.password === 'string' ? body.password : '';
    const problem = passwordProblem(password, email);
    if (problem) throw new ApiError(400, 'weak_password', problem);
    const pending = reg.mode === 'approval';
    let id: number;
    try {
      const { rows } = await ctx.db.query<{ id: number }>(
        `insert into users (email, display_name, password_hash, role_id, pending, self_registered)
         values ($1, $2, $3, $4, $5, true) returning id`,
        [email, displayName, await hashPassword(password), roleId, pending],
      );
      id = rows[0]!.id;
    } catch (err) {
      if ((err as { code?: string }).code === '23505') throw new ApiError(409, 'duplicate', 'このメールアドレスは、もう登録されています。ログインしてください');
      throw err;
    }
    // 操作の記録にも残す（管理者の操作ではないので、管理者の欄は空）
    await ctx.db.query("insert into audit_log (admin_id, admin_email, action, target, detail) values (null, '', 'user.register', $1, $2)", [
      email,
      JSON.stringify({ pending }),
    ]);
    reply.status(201);
    if (pending) return { pending: true, message: '登録しました。管理者が承認すると、ログインできます' };
    const session = await createSession(ctx.db, id, 'app', { userAgent: req.headers['user-agent'], ip: clientIp(req) });
    await ctx.db.query('update users set last_login_at = now() where id = $1', [id]);
    const user = await findUser(ctx.db, 'u.id = $1', [id]);
    return { token: session.token, expiresAt: session.expiresAt.toISOString(), ...(await describeMe(ctx, user!)) };
  });

  app.post('/api/auth/logout', async (req, reply) => {
    const auth = req.headers.authorization ?? '';
    if (auth.startsWith('Bearer ')) await revokeSession(ctx.db, auth.slice(7).trim());
    const cookie = readCookie(req, ADMIN_COOKIE);
    if (cookie) {
      requireAdminHeader(req);
      await revokeSession(ctx.db, cookie);
      setAdminCookie(reply, '', null, ctx.config.cookieSecure);
    }
    return { ok: true };
  });

  // 自分の情報（アプリは Bearer、管理画面はクッキー）
  app.get('/api/me', async (req) => {
    const kind: SessionKind = req.headers.authorization ? 'app' : 'admin';
    const { user } = await authenticate(ctx.db, req, kind);
    return describeMe(ctx, user);
  });

  app.post('/api/me/password', async (req) => {
    const kind: SessionKind = req.headers.authorization ? 'app' : 'admin';
    if (kind === 'admin') requireAdminHeader(req);
    const session = await authenticate(ctx.db, req, kind);
    const body = (req.body ?? {}) as Record<string, unknown>;
    const current = typeof body.current === 'string' ? body.current : '';
    const next = typeof body.next === 'string' ? body.next : '';
    if (!byEmail.hit(`pw:${session.user.email}`)) throw new ApiError(429, 'too_many', '何度も失敗したので、しばらく受け付けません。15 分ほど待ってください');
    if (!(await verifyPassword(current, session.user.password_hash))) throw new ApiError(400, 'bad_password', '今のパスワードが違います');
    const problem = passwordProblem(next, session.user.email);
    if (problem) throw new ApiError(400, 'weak_password', problem);
    if (next === current) throw new ApiError(400, 'weak_password', '今のパスワードとは違うものにしてください');
    await ctx.db.query('update users set password_hash = $2, must_change_password = false, updated_at = now() where id = $1', [
      session.user.id,
      await hashPassword(next),
    ]);
    // ほかの PC でログインしたままの印は消す（今使っている印は残す）
    await revokeUserSessions(ctx.db, session.user.id, session.sessionId);
    byEmail.reset(`pw:${session.user.email}`);
    const user = await findUser(ctx.db, 'u.id = $1', [session.user.id]);
    return describeMe(ctx, user!);
  });
}
