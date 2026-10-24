// アプリからの AI の依頼を、上限と考える深さを確かめてから Bedrock に送り、使ったトークンを記録する。
import type { FastifyInstance } from 'fastify';
import type { AppContext } from '../app.ts';
import { RateLimiter } from '../auth/ratelimit.ts';
import { BedrockError, limitsFor, readUsage, reasoningFields, supportsEffort } from '../bedrock.ts';
import { ApiError, authenticate } from '../http.ts';
import type { Level } from '../policy.ts';
import { clampLevel, effectivePolicy, isChoice, isLevel, monthRange, monthUsage } from '../policy.ts';

/** 1 回の依頼で送れる量（画像も入るので大きめ） */
export const AI_BODY_LIMIT = 24 * 1024 * 1024;

export function aiRoutes(app: FastifyInstance, ctx: AppContext): void {
  // 道具を続けて使うと 1 回の依頼で何度も送るので、1 人あたり 1 分に 60 回まで
  const perUser = new RateLimiter(60, 60_000);

  app.post('/api/ai/converse', { bodyLimit: AI_BODY_LIMIT }, async (req) => {
    const { user } = await authenticate(ctx.db, req, 'app');
    const policy = effectivePolicy(user);
    if (!policy.canUseAi) throw new ApiError(403, 'ai_not_allowed', 'このアカウントでは AI を使えません。管理者に連絡してください');
    if (user.must_change_password) throw new ApiError(403, 'must_change_password', '先にパスワードを変えてください');
    if (!perUser.hit(String(user.id))) throw new ApiError(429, 'too_many', '短い間に送りすぎました。少し待ってから、もう一度送ってください');

    const body = (req.body ?? {}) as Record<string, unknown>;
    const request = body.request as Record<string, unknown> | undefined;
    if (!request || typeof request !== 'object' || !Array.isArray(request.messages) || request.messages.length === 0 || request.messages.length > 400) {
      throw new ApiError(400, 'invalid', '送る内容（messages）が正しくありません');
    }
    if (request.system !== undefined && !Array.isArray(request.system)) throw new ApiError(400, 'invalid', 'system が正しくありません');
    if (request.toolConfig !== undefined && (typeof request.toolConfig !== 'object' || Array.isArray(request.toolConfig))) {
      throw new ApiError(400, 'invalid', 'toolConfig が正しくありません');
    }

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

    // 送るものは、ここで組み立て直す（モデル・上限・深さはサーバーが決める）
    const converse: Record<string, unknown> = {
      messages: request.messages,
      inferenceConfig: { maxTokens: limits.maxTokens },
    };
    if (request.system !== undefined) converse.system = request.system;
    if (request.toolConfig !== undefined) converse.toolConfig = request.toolConfig;
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
