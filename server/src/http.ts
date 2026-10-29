// 認証・エラー・クッキーなど、どの API でも使う部品。
import type { FastifyReply, FastifyRequest } from 'fastify';
import type { Db } from './db.ts';
import type { Session, SessionKind } from './auth/sessions.ts';
import { findSession } from './auth/sessions.ts';

/** 利用者に見せてよい失敗（status と、日本語の説明。code はアプリが見分けるため） */
export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  constructor(status: number, code: string, message: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

export const ADMIN_COOKIE = 'e_editor_admin';

export function readCookie(req: FastifyRequest, name: string): string {
  const header = req.headers.cookie ?? '';
  for (const part of header.split(';')) {
    const i = part.indexOf('=');
    if (i < 0) continue;
    if (part.slice(0, i).trim() === name) {
      try {
        return decodeURIComponent(part.slice(i + 1).trim());
      } catch {
        return '';
      }
    }
  }
  return '';
}

export function setAdminCookie(reply: FastifyReply, token: string, expires: Date | null, secure: boolean): void {
  const parts = [
    `${ADMIN_COOKIE}=${token ? encodeURIComponent(token) : ''}`,
    'Path=/',
    'HttpOnly',
    'SameSite=Strict',
    expires ? `Expires=${expires.toUTCString()}` : 'Max-Age=0',
  ];
  if (secure) parts.push('Secure');
  reply.header('Set-Cookie', parts.join('; '));
}

function bearer(req: FastifyRequest): string {
  const h = req.headers.authorization ?? '';
  return h.startsWith('Bearer ') ? h.slice(7).trim() : '';
}

/** アプリ（Authorization: Bearer）か管理画面（クッキー）のログイン中の印を確かめる */
export async function authenticate(db: Db, req: FastifyRequest, kind: SessionKind): Promise<Session> {
  const token = kind === 'app' ? bearer(req) : readCookie(req, ADMIN_COOKIE);
  const session = token ? await findSession(db, token, kind) : null;
  if (!session) throw new ApiError(401, 'login_required', 'ログインしてください（ログインの期限が切れたか、アカウントが止められています）');
  return session;
}

/**
 * 管理画面からの変更か確かめる（ほかのサイトから送らせる攻撃を防ぐ）。
 * ブラウザーは、ほかのサイトからこの独自のヘッダーを付けて送れない（CORS を許していないため）
 */
export function requireAdminHeader(req: FastifyRequest): void {
  if (req.headers['x-e-editor-admin'] !== '1') throw new ApiError(403, 'bad_request', '管理画面から操作してください');
  const origin = req.headers.origin;
  // req.host は、TRUST_PROXY のときだけ X-Forwarded-Host を見る（Caddy などの裏で動かしても、元のアドレスと比べられる）
  const host = req.host;
  if (!origin || !host) return;
  let originHost = '';
  try {
    originHost = new URL(origin).host;
  } catch {
    // 読めない Origin は、ほかのサイトとして扱う
  }
  if (originHost !== host) throw new ApiError(403, 'bad_request', '管理画面から操作してください');
}

export function clientIp(req: FastifyRequest): string {
  return req.ip ?? '';
}

/** 値の確かめ方の部品（だめなら 400） */
export const check = {
  string(v: unknown, name: string, max = 200): string {
    if (typeof v !== 'string') throw new ApiError(400, 'invalid', `${name} を文字で指定してください`);
    const s = v.trim();
    if (s.length > max) throw new ApiError(400, 'invalid', `${name} は ${max} 文字までにしてください`);
    return s;
  },
  email(v: unknown): string {
    const s = check.string(v, 'メールアドレス', 254).toLowerCase();
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(s)) throw new ApiError(400, 'invalid', 'メールアドレスの書き方が正しくありません');
    return s;
  },
  id(v: unknown, name = 'ID'): number {
    const n = typeof v === 'string' ? Number(v) : v;
    if (typeof n !== 'number' || !Number.isSafeInteger(n) || n <= 0) throw new ApiError(400, 'invalid', `${name} が正しくありません`);
    return n;
  },
  /** トークン数（null は無制限） */
  tokens(v: unknown, name: string, allowNegative = false): number | null {
    if (v === null) return null;
    if (typeof v !== 'number' || !Number.isSafeInteger(v) || (!allowNegative && v < 0)) {
      throw new ApiError(400, 'invalid', `${name} は 0 以上の整数にしてください`);
    }
    if (Math.abs(v) > 1e15) throw new ApiError(400, 'invalid', `${name} が大きすぎます`);
    return v;
  },
  bool(v: unknown, name: string): boolean {
    if (typeof v !== 'boolean') throw new ApiError(400, 'invalid', `${name} は true か false にしてください`);
    return v;
  },
};
