// アプリからの AI の依頼を、上限と考える深さを確かめてから Bedrock に送り、使ったトークンを記録する。
// 送る前に、依頼の形を確かめ、システムプロンプトをサーバーの決まりで挟む（guard.ts）。
import type { FastifyInstance } from 'fastify';
import type { AppContext } from '../app.ts';
import { RateLimiter } from '../auth/ratelimit.ts';
import { BedrockError, bedrockInvoke, limitsFor, readUsage, reasoningFields, supportsEffort } from '../bedrock.ts';
import { checkMessages, checkToolConfig, guardSystem } from '../guard.ts';
import { ApiError, authenticate } from '../http.ts';
import type { Level } from '../policy.ts';
import { clampLevel, effectivePolicy, isChoice, isLevel, monthRange, monthUsage } from '../policy.ts';

/** 1 回の依頼で送れる量（画像も入るので大きめ） */
export const AI_BODY_LIMIT = 24 * 1024 * 1024;
/** 作る画像の比率（Bedrock の Stability AI の画像モデルが受け付けるもの。アプリの画面の比率はすべて入っている） */
export const IMAGE_ASPECTS: readonly string[] = ['16:9', '9:16', '1:1', '4:5', '5:4', '3:2', '2:3', '21:9', '9:21'];
/** 作る画像の説明の長さの上限（文字数） */
export const IMAGE_PROMPT_CHARS = 2000;

export function aiRoutes(app: FastifyInstance, ctx: AppContext): void {
  // 道具を続けて使うと 1 回の依頼で何度も送るので、1 人あたり 1 分に 60 回まで
  const perUser = new RateLimiter(60, 60_000);
  // 画像は、月の上限とは別に、1 人あたり 1 時間に 30 枚まで（短い間に作りすぎないように）
  const imageLimit = new RateLimiter(30, 60 * 60_000);
  const invoke = ctx.invoke ?? bedrockInvoke(ctx.config.bedrockKey);

  // 画像を作る（AI アシスタントの generate_images）。1 枚を、月のトークンの IMAGE_TOKENS トークン分として数える
  app.post('/api/ai/image', async (req) => {
    const { user } = await authenticate(ctx.db, req, 'app');
    const policy = effectivePolicy(user);
    if (!policy.canUseAi) throw new ApiError(403, 'ai_not_allowed', 'このアカウントでは AI を使えません。管理者に連絡してください');
    if (user.must_change_password) throw new ApiError(403, 'must_change_password', '先にパスワードを変えてください');
    const b = (req.body ?? {}) as Record<string, unknown>;
    const prompt = typeof b.prompt === 'string' ? b.prompt.replace(/\s+/g, ' ').trim() : '';
    if (!prompt || prompt.length > IMAGE_PROMPT_CHARS) {
      throw new ApiError(400, 'invalid', `作る画像の説明（prompt）は 1〜${IMAGE_PROMPT_CHARS.toLocaleString('ja-JP')} 文字にしてください`);
    }
    const aspect = b.aspect === undefined ? '1:1' : String(b.aspect);
    if (!IMAGE_ASPECTS.includes(aspect)) throw new ApiError(400, 'invalid', `画像の比率（aspect）は ${IMAGE_ASPECTS.join('・')} のどれかにしてください`);
    if (!imageLimit.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に画像を作りすぎました。少し待ってから、もう一度試してください');
    const cost = ctx.config.imageTokens;
    const range = await monthRange(ctx.db, ctx.config.timezone);
    const before = await monthUsage(ctx.db, user, range);
    if (before.remaining !== null && before.remaining < Math.max(1, cost)) {
      throw new ApiError(
        429,
        'quota',
        `今月の残りのトークンが足りないので、画像を作れません（画像 1 枚で ${cost.toLocaleString('ja-JP')} トークン）。来月まで待つか、管理者に追加を頼んでください`,
      );
    }
    const model = ctx.config.imageModel;
    const started = Date.now();
    // 使用履歴には、チャットと同じ表に「画像を作る」（effort_choice が image）として残す
    const record = (ok: boolean, tokens: number, error = '') =>
      ctx.db.query(
        `insert into usage_events (user_id, user_email, model, effort, effort_choice, output_tokens, duration_ms, ok, error)
         values ($1, $2, $3, '', 'image', $4, $5, $6, $7)`,
        [user.id, user.email, model, tokens, Date.now() - started, ok, error.slice(0, 500)],
      );
    let response: unknown;
    try {
      response = await invoke({ prompt, aspect_ratio: aspect, output_format: 'png' }, { model, region: ctx.config.imageRegion, timeoutMs: 120_000 });
    } catch (err) {
      const message = err instanceof BedrockError ? err.message : '画像を作れませんでした';
      await record(false, 0, message);
      if (!(err instanceof BedrockError)) req.log.error({ err }, 'Bedrock image failed');
      throw new ApiError(err instanceof BedrockError ? err.status : 502, 'ai_failed', message);
    }
    const r = (response ?? {}) as { images?: unknown; finish_reasons?: unknown };
    const image = Array.isArray(r.images) && typeof r.images[0] === 'string' ? r.images[0] : '';
    const reason = Array.isArray(r.finish_reasons) ? r.finish_reasons[0] : null;
    if (!image || reason) {
      // 画像のモデルの決まり（内容の確認）で止められた。トークンは数えない
      await record(false, 0, `画像を作れませんでした（${String(reason ?? 'no image').slice(0, 200)}）`);
      throw new ApiError(422, 'filtered', 'この内容の画像は作れませんでした（画像の AI の決まりで止められました）。説明を変えて、もう一度試してください');
    }
    await record(true, cost);
    const after = await monthUsage(ctx.db, user, range);
    return { image, format: 'png', tokens: cost, usage: after };
  });

  app.post('/api/ai/converse', { bodyLimit: AI_BODY_LIMIT }, async (req) => {
    const { user } = await authenticate(ctx.db, req, 'app');
    const policy = effectivePolicy(user);
    if (!policy.canUseAi) throw new ApiError(403, 'ai_not_allowed', 'このアカウントでは AI を使えません。管理者に連絡してください');
    if (user.must_change_password) throw new ApiError(403, 'must_change_password', '先にパスワードを変えてください');
    if (!perUser.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に送りすぎました。少し待ってから、もう一度送ってください');

    const body = (req.body ?? {}) as Record<string, unknown>;
    const request = body.request as Record<string, unknown> | undefined;
    if (!request || typeof request !== 'object' || Array.isArray(request)) throw new ApiError(400, 'invalid', '送る内容（messages）が正しくありません');
    // アプリが送らない種類の内容は断る。システムプロンプトは、アプリから届いたものをサーバーの決まりで挟む
    const messages = checkMessages(request.messages);
    const system = guardSystem(request.system);
    const toolConfig = checkToolConfig(request.toolConfig);

    // 今月の上限（1 回の依頼が残りを少し超えることはある。超えたら次から受け付けない）
    const range = await monthRange(ctx.db, ctx.config.timezone);
    const before = await monthUsage(ctx.db, user, range);
    if (before.remaining !== null && before.remaining <= 0) {
      throw new ApiError(
        429,
        'quota',
        `今月のトークンを使い切りました（今月使える量 ${before.allowance?.toLocaleString('ja-JP')}）。来月まで待つか、管理者に追加を頼んでください`,
      );
    }

    // 考える深さ: その人の上限まで下げる。深さを変えられないモデルなら、モデルの標準のまま
    const model = ctx.config.model;
    const asked = isLevel(body.effort) ? body.effort : '';
    const level: Level | '' = asked && supportsEffort(model) ? clampLevel(asked, policy.maxEffort) : '';
    const choice = isChoice(body.effortChoice) ? body.effortChoice : '';
    const limits = limitsFor(level);
    const reasoning = reasoningFields(model, level);

    // 送るものは、ここで組み立て直す（モデル・上限・深さ・システムプロンプトの決まりはサーバーが決める）
    const converse: Record<string, unknown> = {
      system,
      messages,
      inferenceConfig: { maxTokens: limits.maxTokens },
    };
    if (toolConfig) converse.toolConfig = toolConfig;
    if (reasoning) converse.additionalModelRequestFields = reasoning;

    const started = Date.now();
    let response: unknown;
    try {
      response = await ctx.converse(converse, { model, timeoutMs: limits.timeoutMs, withEffort: reasoning !== null });
    } catch (err) {
      const message = err instanceof BedrockError ? err.message : 'AI への送信に失敗しました';
      await ctx.db.query(
        `insert into usage_events (user_id, user_email, model, effort, effort_choice, duration_ms, ok, error)
         values ($1, $2, $3, $4, $5, $6, false, $7)`,
        [user.id, user.email, model, level, choice, Date.now() - started, message.slice(0, 500)],
      );
      if (!(err instanceof BedrockError)) req.log.error({ err }, 'Bedrock call failed');
      throw new ApiError(err instanceof BedrockError ? err.status : 502, 'ai_failed', message);
    }
    const used = readUsage(response);
    await ctx.db.query(
      `insert into usage_events (user_id, user_email, model, effort, effort_choice, input_tokens, cached_tokens, output_tokens, duration_ms)
       values ($1, $2, $3, $4, $5, $6, $7, $8, $9)`,
      [user.id, user.email, model, level, choice, used.input, used.cached, used.output, Date.now() - started],
    );
    const after = await monthUsage(ctx.db, user, range);
    return { response, effort: level, usage: after };
  });
}
