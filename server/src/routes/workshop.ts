// ワークショップ: アプリで作ったテキストのテンプレートと、オーディオ（BGM・効果音など）を公開し、ほかの人が使えるようにする
// （アプリからログインして使う）。使い方（すぐ公開・管理者が確認してから公開・使えない）は、管理画面の「ワークショップ」で決める。
import type { FastifyInstance, FastifyRequest } from 'fastify';
import type { AppContext } from '../app.ts';
import { RateLimiter } from '../auth/ratelimit.ts';
import type { Tx } from '../db.ts';
import { tx } from '../db.ts';
import { ApiError, authenticate, check } from '../http.ts';
import type { UserRow } from '../policy.ts';
import { readWorkshop } from '../settings.ts';
import type { AudioType } from '../workshop.ts';
import { AUDIO_TYPES, detectAudio, fitsFullHd, IMAGE_TYPES, oneLine, readImage, readMp4, sanitizeStyle, VIDEO_TYPES, WORKSHOP_LIMITS } from '../workshop.ts';

type Body = Record<string, unknown>;

/** 一覧の 1 行（作った人は表示名だけ。メールアドレスは出さない） */
export type TemplateRow = {
  id: number;
  user_id: number;
  kind: string;
  name: string;
  description: string;
  style: Record<string, unknown>;
  status: string;
  uses: number;
  file_type: string | null;
  file_size: number | null;
  duration: number | null;
  width: number | null;
  height: number | null;
  thumb: Buffer | null;
  created_at: Date;
  updated_at: Date;
  author: string;
  used?: boolean;
};

/** 一覧などで読む列（t: workshop_templates、u: users） */
export const TEMPLATE_COLUMNS = `t.id, t.user_id, t.kind, t.name, t.description, t.style, t.status, t.uses, t.file_type, t.file_size, t.duration,
  t.width, t.height, t.thumb, t.created_at, t.updated_at`;

/** ファイルの Content-Type（オーディオ・写真・動画） */
export function fileContentType(kind: string, type: string): string {
  const types: Record<string, string> = kind === 'image' ? IMAGE_TYPES : kind === 'video' ? VIDEO_TYPES : AUDIO_TYPES;
  return types[type] ?? 'application/octet-stream';
}

/** ファイルを持つ種類（中身は workshop_files） */
export const FILE_KINDS: readonly string[] = ['audio', 'image', 'video'];

export function templateJson(r: TemplateRow, me: number) {
  return {
    id: String(r.id),
    kind: r.kind,
    name: r.name,
    description: r.description,
    author: r.author,
    mine: r.user_id === me,
    used: Boolean(r.used),
    status: r.status,
    uses: r.uses,
    createdAt: r.created_at,
    updatedAt: r.updated_at,
    // テキストは見た目、オーディオはファイルの形式・大きさ・長さ、写真はファイルの形式・大きさ・幅と高さと、一覧の見本（JPEG の base64）、
    // 動画は写真と同じものに長さを足したもの
    ...(r.kind === 'audio'
      ? { file: { type: r.file_type, size: r.file_size, duration: r.duration } }
      : r.kind === 'image'
        ? { file: { type: r.file_type, size: r.file_size, width: r.width, height: r.height }, thumb: r.thumb ? r.thumb.toString('base64') : '' }
        : r.kind === 'video'
          ? {
              file: { type: r.file_type, size: r.file_size, duration: r.duration, width: r.width, height: r.height },
              thumb: r.thumb ? r.thumb.toString('base64') : '',
            }
          : { style: r.style }),
  };
}

/** 作った人の表示名（無ければ「名前なし」） */
export const AUTHOR_SQL = `coalesce(nullif(btrim(u.display_name), ''), '名前なし')`;

/** LIKE の % と _ を、ただの文字として探せるようにする */
function likePattern(q: string): string {
  return `%${q.replace(/[\\%_]/g, (c) => `\\${c}`)}%`;
}

function int(v: unknown, fallback: number, min: number, max: number): number {
  const n = Math.floor(Number(v));
  return Number.isFinite(n) ? Math.min(max, Math.max(min, n)) : fallback;
}

/** 公開する名前と説明を確かめる（1 行に整える） */
function nameAndDescription(nameValue: unknown, descriptionValue: unknown): { name: string; description: string } {
  const name = oneLine(check.string(nameValue ?? '', '名前', 200));
  if (!name) throw new ApiError(400, 'invalid', '名前を入れてください');
  if (name.length > WORKSHOP_LIMITS.name) throw new ApiError(400, 'invalid', `名前は ${WORKSHOP_LIMITS.name} 文字までにしてください`);
  const description = oneLine(check.string(descriptionValue ?? '', '説明', 1000));
  if (description.length > WORKSHOP_LIMITS.description) throw new ApiError(400, 'invalid', `説明は ${WORKSHOP_LIMITS.description} 文字までにしてください`);
  return { name, description };
}

/** オーディオを受け取るときの本文の上限（ファイルの上限に、少しゆとりを持たせる） */
export const AUDIO_BODY_LIMIT = WORKSHOP_LIMITS.audioBytes + 64 * 1024;
/** 写真を受け取るときの本文の上限（JSON の中の base64 なので、4/3 倍にゆとりを足す） */
export const IMAGE_BODY_LIMIT = Math.ceil(((WORKSHOP_LIMITS.imageBytes + WORKSHOP_LIMITS.thumbBytes) * 4) / 3) + 64 * 1024;
/** 動画を受け取るときの本文の上限（写真と同じ考え方） */
export const VIDEO_BODY_LIMIT = Math.ceil(((WORKSHOP_LIMITS.videoBytes + WORKSHOP_LIMITS.thumbBytes) * 4) / 3) + 64 * 1024;

export function workshopRoutes(app: FastifyInstance, ctx: AppContext): void {
  const { db } = ctx;
  // 公開と報告は、1 人あたり 1 時間に 20 回まで。オーディオの読み込みは、1 人あたり 1 時間に 300 回まで
  const publishLimit = new RateLimiter(20, 60 * 60_000);
  const reportLimit = new RateLimiter(20, 60 * 60_000);
  const downloadLimit = new RateLimiter(300, 60 * 60_000);

  // オーディオのファイルは、そのままのバイトで受け取る（application/octet-stream）
  app.addContentTypeParser('application/octet-stream', { parseAs: 'buffer', bodyLimit: AUDIO_BODY_LIMIT }, (_req, body, done) => done(null, body));

  /** アプリでログインしている人。ワークショップを使えないサーバーなら断る */
  async function member(req: FastifyRequest): Promise<{ user: UserRow; mode: string }> {
    const { user } = await authenticate(db, req, 'app');
    if (user.must_change_password) throw new ApiError(403, 'must_change_password', '先にパスワードを変えてください');
    const { mode } = await readWorkshop(db);
    if (mode === 'off') throw new ApiError(403, 'workshop_closed', 'このサーバーでは、ワークショップを使えません');
    return { user, mode };
  }

  /** 管理者が確認してから公開するサーバーでは、確認待ちにする（管理者が公開したものは、そのまま公開） */
  const statusFor = (mode: string, user: UserRow) => (mode === 'approval' && !user.role_is_admin ? 'pending' : 'published');

  /** 1 人が公開できる数の上限（テキストもオーディオも合わせて） */
  async function checkCount(c: Tx, user: UserRow): Promise<void> {
    const count = await c.query<{ n: number }>('select count(*) as n from workshop_templates where user_id = $1', [user.id]);
    if ((count.rows[0]?.n ?? 0) >= WORKSHOP_LIMITS.perUser) {
      throw new ApiError(409, 'too_many_templates', `公開できるのは 1 人 ${WORKSHOP_LIMITS.perUser} 個までです。使わなくなったものの公開をやめてから、もう一度公開してください`);
    }
  }

  app.get('/api/workshop/templates', async (req) => {
    const { user, mode } = await member(req);
    const q = req.query as Record<string, string | undefined>;
    // kind を送らない古いアプリには、テキストのテンプレートだけを返す
    const kind = q.kind !== undefined && FILE_KINDS.includes(q.kind) ? q.kind : 'text';
    const mine = q.mine === '1' || q.mine === 'true';
    const limit = int(q.limit, 30, 1, 60);
    const offset = int(q.offset, 0, 0, 100_000);
    // 絞り込みの値（数える問い合わせと一覧の問い合わせで同じものを使う。使わない値は渡さない）
    const params: unknown[] = [];
    const arg = (v: unknown) => {
      params.push(v);
      return `$${params.length}`;
    };
    // 自分が公開したもの（確認待ち・非公開も）か、みんなの公開中のもの（止めたアカウントのものは出さない）
    let where = `t.kind = ${arg(kind)} and ` + (mine ? `t.user_id = ${arg(user.id)}` : `t.status = 'published' and not u.disabled`);
    const text = oneLine(String(q.q ?? '')).slice(0, 50);
    if (text) {
      const p = arg(likePattern(text));
      where += ` and (t.name ilike ${p} escape '\\' or t.description ilike ${p} escape '\\' or u.display_name ilike ${p} escape '\\')`;
    }
    const order = q.sort === 'popular' ? 't.uses desc, t.created_at desc, t.id desc' : 't.created_at desc, t.id desc';
    const total = await db.query<{ n: number }>(`select count(*) as n from workshop_templates t join users u on u.id = t.user_id where ${where}`, params);
    const n = params.length;
    const { rows } = await db.query<TemplateRow>(
      `select ${TEMPLATE_COLUMNS}, ${AUTHOR_SQL} as author,
              exists (select 1 from workshop_uses w where w.template_id = t.id and w.user_id = $${n + 1}) as used
         from workshop_templates t join users u on u.id = t.user_id
        where ${where}
        order by ${order}
        limit $${n + 2} offset $${n + 3}`,
      [...params, user.id, limit, offset],
    );
    return { mode, kind, total: total.rows[0]?.n ?? 0, offset, limit, templates: rows.map((r) => templateJson(r, user.id)) };
  });

  // テキストのテンプレートを公開する
  app.post('/api/workshop/templates', async (req, reply) => {
    const { user, mode } = await member(req);
    const b = (req.body ?? {}) as Body;
    const { name, description } = nameAndDescription(b.name, b.description);
    const style = sanitizeStyle(b.style);
    if (!publishLimit.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に公開しすぎました。少し待ってから、もう一度公開してください');
    const status = statusFor(mode, user);
    const row = await tx(db, async (c) => {
      await checkCount(c, user);
      const { rows } = await c.query<TemplateRow>(
        `insert into workshop_templates (user_id, kind, name, description, style, status) values ($1, 'text', $2, $3, $4, $5)
         returning ${TEMPLATE_COLUMNS.replace(/t\./g, '')}, '' as author`,
        [user.id, name, description, JSON.stringify(style), status],
      );
      return rows[0]!;
    });
    row.author = oneLine(user.display_name) || '名前なし';
    reply.status(201);
    return {
      template: templateJson(row, user.id),
      message: status === 'pending' ? '公開を受け付けました。管理者が確認すると、みんなが使えるようになります' : 'ワークショップに公開しました',
    };
  });

  // オーディオを公開する。本文はファイルの中身（application/octet-stream）、名前などは問い合わせの文字（?name=…）
  app.post('/api/workshop/audio', { bodyLimit: AUDIO_BODY_LIMIT }, async (req, reply) => {
    const { user, mode } = await member(req);
    const q = req.query as Record<string, string | undefined>;
    const { name, description } = nameAndDescription(q.name, q.description);
    const body = req.body;
    if (!Buffer.isBuffer(body)) throw new ApiError(400, 'invalid', 'オーディオのファイルの中身を、application/octet-stream で送ってください');
    if (body.length < WORKSHOP_LIMITS.audioMinBytes) throw new ApiError(400, 'invalid', 'オーディオのファイルが小さすぎます');
    if (body.length > WORKSHOP_LIMITS.audioBytes) {
      throw new ApiError(413, 'too_large', `オーディオのファイルは ${WORKSHOP_LIMITS.audioBytes / 1024 / 1024}MB までにしてください`);
    }
    const ext = String(q.fileName ?? '').split('.').pop() ?? '';
    const type: AudioType | null = detectAudio(body, ext);
    if (!type) throw new ApiError(400, 'invalid', 'このファイルは、受け付けているオーディオの形式（MP3・WAV・M4A・AAC・OGG・Opus・FLAC・WMA）ではありません');
    const duration = Number(q.duration);
    if (!Number.isFinite(duration) || duration <= 0 || duration > WORKSHOP_LIMITS.audioSeconds) {
      throw new ApiError(400, 'invalid', `オーディオの長さは ${WORKSHOP_LIMITS.audioSeconds / 60} 分までにしてください`);
    }
    if (!publishLimit.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に公開しすぎました。少し待ってから、もう一度公開してください');
    const status = statusFor(mode, user);
    const row = await tx(db, async (c) => {
      await checkCount(c, user);
      const used = await c.query<{ bytes: number }>("select coalesce(sum(file_size), 0) as bytes from workshop_templates where user_id = $1 and kind = 'audio'", [user.id]);
      if ((used.rows[0]?.bytes ?? 0) + body.length > WORKSHOP_LIMITS.audioBytesPerUser) {
        throw new ApiError(409, 'too_much_audio', `公開できるオーディオは、1 人合わせて ${WORKSHOP_LIMITS.audioBytesPerUser / 1024 / 1024}MB までです。使わなくなったものの公開をやめてから、もう一度公開してください`);
      }
      const { rows } = await c.query<TemplateRow>(
        `insert into workshop_templates (user_id, kind, name, description, style, status, file_type, file_size, duration)
         values ($1, 'audio', $2, $3, '{}'::jsonb, $4, $5, $6, $7)
         returning ${TEMPLATE_COLUMNS.replace(/t\./g, '')}, '' as author`,
        [user.id, name, description, status, type, body.length, Math.round(duration * 1000) / 1000],
      );
      await c.query('insert into workshop_files (template_id, data) values ($1, $2)', [rows[0]!.id, body]);
      return rows[0]!;
    });
    row.author = oneLine(user.display_name) || '名前なし';
    reply.status(201);
    return {
      template: templateJson(row, user.id),
      message: status === 'pending' ? '公開を受け付けました。管理者が確認すると、みんなが使えるようになります' : 'ワークショップに公開しました',
    };
  });

  // 写真を公開する。JSON で、写真（image）と一覧の見本（thumb。長い辺 480px までの JPEG）を base64 で送る。
  // 写真はフル HD（1920×1080、縦長なら 1080×1920）まで。大きさは中身から読む（アプリが縮めてから送る）
  app.post('/api/workshop/image', { bodyLimit: IMAGE_BODY_LIMIT }, async (req, reply) => {
    const { user, mode } = await member(req);
    const b = (req.body ?? {}) as Body;
    const { name, description } = nameAndDescription(b.name, b.description);
    const image = typeof b.image === 'string' ? Buffer.from(b.image, 'base64') : Buffer.alloc(0);
    const thumb = typeof b.thumb === 'string' ? Buffer.from(b.thumb, 'base64') : Buffer.alloc(0);
    if (!image.length) throw new ApiError(400, 'invalid', '写真の中身（image）を base64 で送ってください');
    if (image.length > WORKSHOP_LIMITS.imageBytes) {
      throw new ApiError(413, 'too_large', `写真は ${WORKSHOP_LIMITS.imageBytes / 1024 / 1024}MB までにしてください`);
    }
    const info = readImage(image);
    if (!info) throw new ApiError(400, 'invalid', 'このファイルは、受け付けている写真の形式（JPEG・PNG）ではありません');
    if (!fitsFullHd(info.width, info.height)) {
      throw new ApiError(
        400,
        'invalid',
        `写真の大きさは、フル HD（1920×1080。縦長なら 1080×1920）までにしてください（この写真は ${info.width}×${info.height}）`,
      );
    }
    const small = readImage(thumb);
    if (!small || small.type !== 'jpg' || thumb.length > WORKSHOP_LIMITS.thumbBytes || Math.max(small.width, small.height) > WORKSHOP_LIMITS.thumbLong) {
      throw new ApiError(400, 'invalid', `一覧の見本（thumb）は、長い辺 ${WORKSHOP_LIMITS.thumbLong}px までの JPEG にしてください`);
    }
    if (!publishLimit.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に公開しすぎました。少し待ってから、もう一度公開してください');
    const status = statusFor(mode, user);
    const row = await tx(db, async (c) => {
      await checkCount(c, user);
      const used = await c.query<{ bytes: number }>("select coalesce(sum(file_size), 0) as bytes from workshop_templates where user_id = $1 and kind = 'image'", [user.id]);
      if (Number(used.rows[0]?.bytes ?? 0) + image.length > WORKSHOP_LIMITS.imageBytesPerUser) {
        throw new ApiError(409, 'too_much_images', `公開できる写真は、1 人合わせて ${WORKSHOP_LIMITS.imageBytesPerUser / 1024 / 1024}MB までです。使わなくなったものの公開をやめてから、もう一度公開してください`);
      }
      const { rows } = await c.query<TemplateRow>(
        `insert into workshop_templates (user_id, kind, name, description, style, status, file_type, file_size, width, height, thumb)
         values ($1, 'image', $2, $3, '{}'::jsonb, $4, $5, $6, $7, $8, $9)
         returning ${TEMPLATE_COLUMNS.replace(/t\./g, '')}, '' as author`,
        [user.id, name, description, status, info.type, image.length, info.width, info.height, thumb],
      );
      await c.query('insert into workshop_files (template_id, data) values ($1, $2)', [rows[0]!.id, image]);
      return rows[0]!;
    });
    row.author = oneLine(user.display_name) || '名前なし';
    reply.status(201);
    return {
      template: templateJson(row, user.id),
      message: status === 'pending' ? '公開を受け付けました。管理者が確認すると、みんなが使えるようになります' : 'ワークショップに公開しました',
    };
  });

  // 動画を公開する。JSON で、動画（video。アプリが MP4 にしたもの）と一覧の見本（thumb。長い辺 480px までの JPEG）を base64 で送る。
  // 長さは 15 秒まで、大きさはフル HD まで（どちらも中身から読む）
  app.post('/api/workshop/video', { bodyLimit: VIDEO_BODY_LIMIT }, async (req, reply) => {
    const { user, mode } = await member(req);
    const b = (req.body ?? {}) as Body;
    const { name, description } = nameAndDescription(b.name, b.description);
    const video = typeof b.video === 'string' ? Buffer.from(b.video, 'base64') : Buffer.alloc(0);
    const thumb = typeof b.thumb === 'string' ? Buffer.from(b.thumb, 'base64') : Buffer.alloc(0);
    if (!video.length) throw new ApiError(400, 'invalid', '動画の中身（video）を base64 で送ってください');
    if (video.length > WORKSHOP_LIMITS.videoBytes) {
      throw new ApiError(413, 'too_large', `動画は ${WORKSHOP_LIMITS.videoBytes / 1024 / 1024}MB までにしてください`);
    }
    const info = readMp4(video);
    if (!info) throw new ApiError(400, 'invalid', 'このファイルは、受け付けている動画の形式（MP4）ではありません');
    if (!(info.duration > 0) || info.duration > WORKSHOP_LIMITS.videoSeconds + WORKSHOP_LIMITS.videoSlack) {
      throw new ApiError(400, 'invalid', `動画の長さは ${WORKSHOP_LIMITS.videoSeconds} 秒までにしてください（この動画は ${info.duration.toFixed(1)} 秒）`);
    }
    if (!fitsFullHd(info.width, info.height)) {
      throw new ApiError(
        400,
        'invalid',
        `動画の大きさは、フル HD（1920×1080。縦長なら 1080×1920）までにしてください（この動画は ${info.width}×${info.height}）`,
      );
    }
    const small = readImage(thumb);
    if (!small || small.type !== 'jpg' || thumb.length > WORKSHOP_LIMITS.thumbBytes || Math.max(small.width, small.height) > WORKSHOP_LIMITS.thumbLong) {
      throw new ApiError(400, 'invalid', `一覧の見本（thumb）は、長い辺 ${WORKSHOP_LIMITS.thumbLong}px までの JPEG にしてください`);
    }
    if (!publishLimit.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に公開しすぎました。少し待ってから、もう一度公開してください');
    const status = statusFor(mode, user);
    const row = await tx(db, async (c) => {
      await checkCount(c, user);
      const used = await c.query<{ bytes: number }>("select coalesce(sum(file_size), 0) as bytes from workshop_templates where user_id = $1 and kind = 'video'", [user.id]);
      if (Number(used.rows[0]?.bytes ?? 0) + video.length > WORKSHOP_LIMITS.videoBytesPerUser) {
        throw new ApiError(409, 'too_much_videos', `公開できる動画は、1 人合わせて ${WORKSHOP_LIMITS.videoBytesPerUser / 1024 / 1024}MB までです。使わなくなったものの公開をやめてから、もう一度公開してください`);
      }
      const { rows } = await c.query<TemplateRow>(
        `insert into workshop_templates (user_id, kind, name, description, style, status, file_type, file_size, duration, width, height, thumb)
         values ($1, 'video', $2, $3, '{}'::jsonb, $4, $5, $6, $7, $8, $9, $10)
         returning ${TEMPLATE_COLUMNS.replace(/t\./g, '')}, '' as author`,
        [user.id, name, description, status, info.type, video.length, Math.round(info.duration * 1000) / 1000, info.width, info.height, thumb],
      );
      await c.query('insert into workshop_files (template_id, data) values ($1, $2)', [rows[0]!.id, video]);
      return rows[0]!;
    });
    row.author = oneLine(user.display_name) || '名前なし';
    reply.status(201);
    return {
      template: templateJson(row, user.id),
      message: status === 'pending' ? '公開を受け付けました。管理者が確認すると、みんなが使えるようになります' : 'ワークショップに公開しました',
    };
  });

  // オーディオ・写真・動画のファイルの中身（試聴・追加するとき）。公開中のものか、自分が公開したものだけ
  app.get('/api/workshop/templates/:id/file', async (req, reply) => {
    const { user } = await member(req);
    const id = check.id((req.params as Body).id);
    if (!downloadLimit.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に読み込みすぎました。少し待ってから、もう一度試してください');
    const { rows } = await db.query<{ kind: string; file_type: string; data: Buffer }>(
      `select t.kind, t.file_type, f.data from workshop_templates t join users u on u.id = t.user_id join workshop_files f on f.template_id = t.id
        where t.id = $1 and t.kind in ('audio', 'image', 'video') and ((t.status = 'published' and not u.disabled) or t.user_id = $2)`,
      [id, user.id],
    );
    if (!rows[0]) throw new ApiError(404, 'not_found', '見つかりません（公開をやめたのかもしれません）');
    reply.header('Content-Type', fileContentType(rows[0].kind, rows[0].file_type));
    reply.header('X-Kantan-File-Type', rows[0].file_type);
    return rows[0].data;
  });

  // 公開をやめる（自分が公開したものだけ。管理者は管理画面から消す）
  app.delete('/api/workshop/templates/:id', async (req) => {
    const { user } = await member(req);
    const id = check.id((req.params as Body).id, 'テンプレート');
    const { rows } = await db.query('delete from workshop_templates where id = $1 and user_id = $2 returning id', [id, user.id]);
    if (!rows[0]) throw new ApiError(404, 'not_found', '見つかりません（自分が公開したものだけ、公開をやめられます）');
    return { ok: true };
  });

  // 使ったことを記録する（マイテンプレート・オーディオに追加したとき。同じ人は 1 回だけ数える）
  app.post('/api/workshop/templates/:id/use', async (req) => {
    const { user } = await member(req);
    const id = check.id((req.params as Body).id, 'テンプレート');
    return tx(db, async (c) => {
      const found = await c.query<{ uses: number }>(
        `select t.uses from workshop_templates t join users u on u.id = t.user_id
          where t.id = $1 and ((t.status = 'published' and not u.disabled) or t.user_id = $2)`,
        [id, user.id],
      );
      if (!found.rows[0]) throw new ApiError(404, 'not_found', '見つかりません（公開をやめたのかもしれません）');
      const added = await c.query('insert into workshop_uses (template_id, user_id) values ($1, $2) on conflict do nothing', [id, user.id]);
      if (!added.rowCount) return { uses: found.rows[0].uses };
      const { rows } = await c.query<{ uses: number }>('update workshop_templates set uses = uses + 1 where id = $1 returning uses', [id]);
      return { uses: rows[0]?.uses ?? found.rows[0].uses };
    });
  });

  // 問題のあるもの（名前・説明・音の中身など）を、管理者に知らせる（自分のものは報告できない。同じ人は 1 回だけ）
  app.post('/api/workshop/templates/:id/report', async (req) => {
    const { user } = await member(req);
    const id = check.id((req.params as Body).id, 'テンプレート');
    const b = (req.body ?? {}) as Body;
    const reason = oneLine(check.string(b.reason ?? '', '理由', 1000)).slice(0, WORKSHOP_LIMITS.reason);
    if (!reportLimit.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に報告しすぎました。少し待ってから、もう一度送ってください');
    const found = await db.query<{ user_id: number }>(`select user_id from workshop_templates where id = $1 and status = 'published'`, [id]);
    if (!found.rows[0]) throw new ApiError(404, 'not_found', '見つかりません（公開をやめたのかもしれません）');
    if (found.rows[0].user_id === user.id) throw new ApiError(400, 'invalid', '自分のものは報告できません');
    await db.query(
      `insert into workshop_reports (template_id, user_id, reason) values ($1, $2, $3)
       on conflict (template_id, user_id) do update set reason = excluded.reason, created_at = now()`,
      [id, user.id, reason],
    );
    return { ok: true, message: '管理者に知らせました' };
  });
}
