// ロールと人ごとの設定から決まる、その人が使えるもの（AI・トークンの上限・考える深さ）。
import type { Db, Tx } from './db.ts';

export const LEVELS = ['low', 'medium', 'high', 'max'] as const;
export type Level = (typeof LEVELS)[number];
export const CHOICES = ['auto', ...LEVELS] as const;
export type Choice = (typeof CHOICES)[number];

export function isLevel(v: unknown): v is Level {
  return typeof v === 'string' && (LEVELS as readonly string[]).includes(v);
}
export function isChoice(v: unknown): v is Choice {
  return typeof v === 'string' && (CHOICES as readonly string[]).includes(v);
}

/** 上限を超える深さは、上限まで下げる */
export function clampLevel(level: Level, max: Level): Level {
  return LEVELS.indexOf(level) > LEVELS.indexOf(max) ? max : level;
}

/** 最初の深さ（auto か、上限まで） */
export function clampChoice(choice: Choice, max: Level): Choice {
  return choice === 'auto' ? 'auto' : clampLevel(choice, max);
}

/** users と roles をまとめて読んだ行 */
export type UserRow = {
  id: number;
  email: string;
  display_name: string;
  password_hash: string;
  disabled: boolean;
  /** 自分で登録して、まだ管理者が承認していない（ログインできない） */
  pending: boolean;
  self_registered: boolean;
  must_change_password: boolean;
  tokens_custom: boolean;
  monthly_tokens: number | null;
  max_effort: Level | null;
  default_effort: Choice | null;
  last_login_at: Date | null;
  created_at: Date;
  role_id: number;
  role_name: string;
  role_is_admin: boolean;
  role_can_use_ai: boolean;
  role_monthly_tokens: number | null;
  role_max_effort: Level;
  role_default_effort: Choice;
};

/** UserRow を読むときの select の列（users は u、roles は r） */
export const userSelect = `u.id, u.email, u.display_name, u.password_hash, u.disabled, u.pending, u.self_registered, u.must_change_password,
  u.tokens_custom, u.monthly_tokens, u.max_effort, u.default_effort, u.last_login_at, u.created_at,
  r.id as role_id, r.name as role_name, r.is_admin as role_is_admin, r.can_use_ai as role_can_use_ai,
  r.monthly_tokens as role_monthly_tokens, r.max_effort as role_max_effort, r.default_effort as role_default_effort`;

export async function findUser(db: Db | Tx, where: string, params: unknown[]): Promise<UserRow | null> {
  const { rows } = await db.query<UserRow>(`select ${userSelect} from users u join roles r on r.id = u.role_id where ${where}`, params);
  return rows[0] ?? null;
}

export type Policy = {
  isAdmin: boolean;
  canUseAi: boolean;
  /** 月のトークンの上限（その月の追加を足す前）。null は無制限 */
  monthlyTokens: number | null;
  maxEffort: Level;
  defaultEffort: Choice;
};

/** 決まりを出すのに要る列だけ */
export type PolicyFields = Pick<
  UserRow,
  | 'tokens_custom'
  | 'monthly_tokens'
  | 'max_effort'
  | 'default_effort'
  | 'role_is_admin'
  | 'role_can_use_ai'
  | 'role_monthly_tokens'
  | 'role_max_effort'
  | 'role_default_effort'
>;

export function effectivePolicy(u: PolicyFields): Policy {
  const maxEffort = u.max_effort ?? u.role_max_effort;
  return {
    isAdmin: u.role_is_admin,
    canUseAi: u.role_can_use_ai,
    monthlyTokens: u.tokens_custom ? u.monthly_tokens : u.role_monthly_tokens,
    maxEffort,
    defaultEffort: clampChoice(u.default_effort ?? u.role_default_effort, maxEffort),
  };
}

export type MonthRange = { month: string; start: Date; end: Date };

/** サーバーのタイムゾーンでの月（2026-10 など）と、その始まり・終わりの時刻。month を省くと今月 */
export async function monthRange(db: Db | Tx, timezone: string, month?: string): Promise<MonthRange> {
  if (month !== undefined && !/^[0-9]{4}-(0[1-9]|1[0-2])$/.test(month)) throw new Error('month は 2026-10 の形にしてください');
  const { rows } = await db.query<{ month: string; start: Date; end: Date }>(
    `with m as (select coalesce(($2 || '-01')::timestamp, date_trunc('month', now() at time zone $1)) as local_start)
     select to_char(local_start, 'YYYY-MM') as month,
            local_start at time zone $1 as start,
            (local_start + interval '1 month') at time zone $1 as "end"
       from m`,
    [timezone, month ?? null],
  );
  return rows[0]!;
}

export type MonthUsage = {
  month: string;
  /** 使ったトークン（入力と出力の合計） */
  used: number;
  /** その月に使える量（上限 + 追加）。null は無制限 */
  allowance: number | null;
  /** 残り（0 より少なくはしない）。null は無制限 */
  remaining: number | null;
  /** その月の追加の合計 */
  granted: number;
};

/** 使える量と残り（上限 + その月の追加） */
export function allowanceOf(limit: number | null, used: number, granted: number): Pick<MonthUsage, 'allowance' | 'remaining'> {
  const allowance = limit === null ? null : Math.max(0, limit + granted);
  return { allowance, remaining: allowance === null ? null : Math.max(0, allowance - used) };
}

export async function monthUsage(db: Db | Tx, user: PolicyFields & { id: number }, range: MonthRange): Promise<MonthUsage> {
  const { rows } = await db.query<{ used: number; granted: number }>(
    `select (select coalesce(sum(input_tokens + output_tokens), 0) from usage_events
              where user_id = $1 and created_at >= $2 and created_at < $3) as used,
            (select coalesce(sum(tokens), 0) from token_grants where user_id = $1 and month = $4) as granted`,
    [user.id, range.start, range.end, range.month],
  );
  const used = rows[0]?.used ?? 0;
  const granted = rows[0]?.granted ?? 0;
  return { month: range.month, used, granted, ...allowanceOf(effectivePolicy(user).monthlyTokens, used, granted) };
}
