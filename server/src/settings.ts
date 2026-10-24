// サーバーの設定（settings テーブル）。新規登録の受け付け方と、ワークショップの使い方。
import type { Db, Tx } from './db.ts';

export const REGISTRATION_MODES = ['off', 'approval', 'open'] as const;
export type RegistrationMode = (typeof REGISTRATION_MODES)[number];

export function isRegistrationMode(v: unknown): v is RegistrationMode {
  return typeof v === 'string' && (REGISTRATION_MODES as readonly string[]).includes(v);
}

/** 新規登録: 受け付け方と、登録した人のロール（決まっていなければ null） */
export type Registration = { mode: RegistrationMode; roleId: number | null };

export async function readRegistration(db: Db | Tx): Promise<Registration> {
  const { rows } = await db.query<{ value: Record<string, unknown> }>("select value from settings where key = 'registration'");
  const v = rows[0]?.value ?? {};
  const roleId = typeof v.roleId === 'number' && Number.isSafeInteger(v.roleId) && v.roleId > 0 ? v.roleId : null;
  return { mode: isRegistrationMode(v.mode) ? v.mode : 'off', roleId };
}

export async function writeRegistration(db: Db | Tx, reg: Registration): Promise<void> {
  await db.query(
    `insert into settings (key, value, updated_at) values ('registration', $1, now())
     on conflict (key) do update set value = excluded.value, updated_at = now()`,
    [JSON.stringify(reg)],
  );
}

/**
 * 登録を受け付けられる状態か（受け付けない設定・ロールが無い・管理者のロールになっているときは受け付けない）。
 * 受け付けられるなら、登録した人のロールの ID を返す
 */
export async function registrationRole(db: Db | Tx, reg: Registration): Promise<number | null> {
  if (reg.mode === 'off' || reg.roleId === null) return null;
  const { rows } = await db.query<{ id: number; is_admin: boolean }>('select id, is_admin from roles where id = $1', [reg.roleId]);
  return rows[0] && !rows[0].is_admin ? rows[0].id : null;
}

// ---- ワークショップ（テキストのテンプレートの公開） ----

/** off: 使えない / approval: 管理者が確認してから公開 / open: すぐに公開 */
export const WORKSHOP_MODES = ['off', 'approval', 'open'] as const;
export type WorkshopMode = (typeof WORKSHOP_MODES)[number];

export function isWorkshopMode(v: unknown): v is WorkshopMode {
  return typeof v === 'string' && (WORKSHOP_MODES as readonly string[]).includes(v);
}

export type WorkshopSettings = { mode: WorkshopMode };

export async function readWorkshop(db: Db | Tx): Promise<WorkshopSettings> {
  const { rows } = await db.query<{ value: Record<string, unknown> }>("select value from settings where key = 'workshop'");
  const v = rows[0]?.value ?? {};
  return { mode: isWorkshopMode(v.mode) ? v.mode : 'off' };
}

export async function writeWorkshop(db: Db | Tx, settings: WorkshopSettings): Promise<void> {
  await db.query(
    `insert into settings (key, value, updated_at) values ('workshop', $1, now())
     on conflict (key) do update set value = excluded.value, updated_at = now()`,
    [JSON.stringify(settings)],
  );
}
