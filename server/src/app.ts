// サーバーの組み立て（API・管理画面のファイル・エラーの返し方）。起動は index.ts、テストはここを直接使う。
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import Fastify from 'fastify';
import type { FastifyInstance, FastifyServerOptions } from 'fastify';
import type { ConverseFn, InvokeFn } from './bedrock.ts';
import type { Config } from './config.ts';
import { serverDir } from './config.ts';
import type { Db } from './db.ts';
import { ApiError } from './http.ts';
import { adminRoutes } from './routes/admin.ts';
import { aiRoutes } from './routes/ai.ts';
import { authRoutes } from './routes/auth.ts';
import { workshopRoutes } from './routes/workshop.ts';
import type { RegistrationMode, WorkshopMode } from './settings.ts';
import { readRegistration, readWorkshop, registrationRole } from './settings.ts';

export type AppContext = {
  config: Config;
  db: Db;
  /** Bedrock を呼ぶ関数（テストでは差し替える） */
  converse: ConverseFn;
  /** Bedrock の InvokeModel を呼ぶ関数（画像を作る。無ければ本物を使う） */
  invoke?: InvokeFn;
};

const publicDir = path.join(serverDir, 'public');
const staticFiles: Record<string, { file: string; type: string }> = {
  '/': { file: 'index.html', type: 'text/html; charset=utf-8' },
  '/admin.js': { file: 'admin.js', type: 'text/javascript; charset=utf-8' },
  '/admin.css': { file: 'admin.css', type: 'text/css; charset=utf-8' },
  '/favicon.svg': { file: 'favicon.svg', type: 'image/svg+xml' },
};

const CSP = [
  "default-src 'self'",
  "script-src 'self'",
  "style-src 'self'",
  "img-src 'self' data:",
  "connect-src 'self'",
  "base-uri 'none'",
  "form-action 'self'",
  "frame-ancestors 'none'",
].join('; ');

export async function buildApp(ctx: AppContext, options: { logger?: FastifyServerOptions['logger']; https?: { key: Buffer; cert: Buffer } } = {}): Promise<FastifyInstance> {
  const app = Fastify({
    logger: options.logger ?? { level: 'info' },
    trustProxy: ctx.config.trustProxy,
    bodyLimit: 1024 * 1024,
    ...(options.https ? { https: options.https } : {}),
  } as FastifyServerOptions) as unknown as FastifyInstance;

  // どの返事にも付ける、ブラウザー向けの守り
  app.addHook('onSend', async (req, reply, payload) => {
    reply.header('X-Content-Type-Options', 'nosniff');
    reply.header('Referrer-Policy', 'no-referrer');
    reply.header('X-Frame-Options', 'DENY');
    reply.header('Content-Security-Policy', CSP);
    if (req.url.startsWith('/api/')) reply.header('Cache-Control', 'no-store');
    return payload;
  });

  app.setErrorHandler((err, req, reply) => {
    if (err instanceof ApiError) return reply.status(err.status).send({ error: err.message, code: err.code });
    const e = err as { code?: string; statusCode?: number };
    if (e.code === 'FST_ERR_CTP_BODY_TOO_LARGE') return reply.status(413).send({ error: '送る内容が大きすぎます', code: 'too_large' });
    if (e.code === 'FST_ERR_CTP_INVALID_MEDIA_TYPE' || e.code === 'FST_ERR_CTP_EMPTY_JSON_BODY' || (e.statusCode === 400 && e.code?.startsWith('FST_'))) {
      return reply.status(400).send({ error: '送る内容の形が正しくありません', code: 'bad_request' });
    }
    if ((err as SyntaxError).name === 'SyntaxError') return reply.status(400).send({ error: '送る内容の形が正しくありません', code: 'bad_request' });
    req.log.error({ err }, 'unexpected error');
    return reply.status(500).send({ error: 'サーバーでエラーが起きました。少し待ってから、もう一度試してください', code: 'server_error' });
  });

  app.setNotFoundHandler((req, reply) => reply.status(404).send({ error: '見つかりません', code: 'not_found' }));

  // アプリが、つながっているかを確かめる（ログインしていなくても使える）。
  // service でこのサーバーだと分かり、registration で新規登録を受け付けているか、workshop でワークショップを使えるかが分かる
  app.get('/api/health', async () => {
    let db = true;
    let registration: RegistrationMode = 'off';
    let workshop: WorkshopMode = 'off';
    try {
      const reg = await readRegistration(ctx.db);
      if ((await registrationRole(ctx.db, reg)) !== null) registration = reg.mode;
      workshop = (await readWorkshop(ctx.db)).mode;
    } catch {
      db = false;
    }
    return {
      ok: db,
      db,
      service: 'kantan-ai-server',
      api: 3,
      registration,
      workshop,
      bedrockConfigured: Boolean(ctx.config.bedrockKey),
      model: ctx.config.model,
    };
  });

  authRoutes(app, ctx);
  aiRoutes(app, ctx);
  workshopRoutes(app, ctx);
  adminRoutes(app, ctx);

  // 管理画面（ファイルは 3 つだけなので、決まった名前だけを返す）
  for (const [url, { file, type }] of Object.entries(staticFiles)) {
    app.get(url, async (_req, reply) => {
      reply.header('Content-Type', type);
      reply.header('Cache-Control', 'no-cache');
      return readFile(path.join(publicDir, file));
    });
  }

  return app;
}
