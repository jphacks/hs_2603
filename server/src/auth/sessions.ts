// ログイン中の印（セッション）。トークンは 32 バイトの乱数で、データベースには SHA-256 だけを保存する。
import { createHash, randomBytes } from 'node:crypto';
import type { Db } from '../db.ts';
import type { UserRow } from '../policy.ts';
import { userSelect } from '../policy.ts';

export type SessionKind = 'app' | 'admin';

/** アプリは 30 日（使うたびに延びる）、管理画面は 12 時間 */
const LIFETIME_MS: Record<SessionKind, number> = { app: 30 * 24 * 3600 * 1000, admin: 12 * 3600 * 1000 };

export function hashToken(token: string): Buffer {
  return createHash('sha256').update(token).digest();
}

export async function createSession(
  db: Db,
  userId: number,
  kind: SessionKind,
  meta: { userAgent?: string; ip?: string } = {},
): Promise<{ token: string; expiresAt: Date }> {
  const token = randomBytes(32).toString('base64url');
  const expiresAt = new Date(Date.now() + LIFETIME_MS[kind]);
  await db.query('insert into sessions (user_id, token_hash, kind, expires_at, user_agent, ip) values ($1, $2, $3, $4, $5, $6)', [
    userId,
    hashToken(token),
    kind,
    expiresAt,
    (meta.userAgent ?? '').slice(0, 300),
    (meta.ip ?? '').slice(0, 100),
  ]);
  // 期限の切れた印は、ついでに片付ける
  await db.query('delete from sessions where expires_at < now()');
  return { token, expiresAt };
}

export type Session = { sessionId: number; token: string; user: UserRow };

/** 印からユーザーを探す。期限切れ・使えないアカウントなら null */
export async function findSession(db: Db, token: string, kind: SessionKind): Promise<Session | null> {
  if (!token || token.length > 200) return null;
  const { rows } = await db.query<UserRow & { session_id: number; last_used_at: Date }>(
    `select s.id as session_id, s.last_used_at, ${userSelect}
       from sessions s join users u on u.id = s.user_id join roles r on r.id = u.role_id
      where s.token_hash = $1 and s.kind = $2 and s.expires_at > now()`,
    [hashToken(token), kind],
  );
  const row = rows[0];
  // 止めたアカウントと、承認待ちのアカウントは使えない
  if (!row || row.disabled || row.pending) return null;
  // 書き込みを減らすため、使った時刻は 1 分に 1 回だけ更新する。アプリの印は使うたびに期限を延ばす
  if (Date.now() - new Date(row.last_used_at).getTime() > 60_000) {
    const extend = kind === 'app' ? new Date(Date.now() + LIFETIME_MS.app) : null;
    await db.query('update sessions set last_used_at = now(), expires_at = coalesce($2, expires_at) where id = $1', [row.session_id, extend]);
  }
  const { session_id, last_used_at, ...user } = row;
  return { sessionId: session_id, token, user };
}

export async function revokeSession(db: Db, token: string): Promise<void> {
  await db.query('delete from sessions where token_hash = $1', [hashToken(token)]);
}

/** そのユーザーの印をすべて消す（パスワードを変えたとき・止めたとき）。exceptId の印だけは残す */
export async function revokeUserSessions(db: Db, userId: number, exceptId?: number): Promise<void> {
  await db.query('delete from sessions where user_id = $1 and id <> coalesce($2, -1)', [userId, exceptId ?? null]);
}
