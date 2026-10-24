// Amazon Bedrock の Converse API を呼ぶ（API キーを Authorization: Bearer で送る）。
import type { Level } from './policy.ts';

/** Claude の世代（4.6 なら [4, 6]）。読めなければ null */
function claudeVersion(model: string): [number, number] | null {
  // 例: jp.anthropic.claude-sonnet-4-6 / us.anthropic.claude-opus-4-6-v1 / anthropic.claude-sonnet-5 /
  //     jp.anthropic.claude-sonnet-4-5-20250929-v1:0（日付は世代ではない）
  const m = /claude-[a-z]+-(\d+)(?:-(\d{1,2}))?(?!\d)/.exec(model);
  return m ? [Number(m[1]), m[2] ? Number(m[2]) : 0] : null;
}

/** このモデルで考える深さを変えられるか（Claude Sonnet 4.6 以降・OpenAI GPT） */
export function supportsEffort(model: string): boolean {
  if (model.includes('anthropic.')) {
    const v = claudeVersion(model);
    return v !== null && (v[0] > 4 || (v[0] === 4 && v[1] >= 6));
  }
  return model.includes('openai.gpt-') && !model.includes('gpt-oss');
}

/** additionalModelRequestFields に入れる、考える深さの指定（深さを変えられないモデルなら null） */
export function reasoningFields(model: string, level: Level | ''): Record<string, unknown> | null {
  if (!level || !supportsEffort(model)) return null;
  if (model.includes('anthropic.')) {
    // Claude: adaptive thinking をオンにして深さを指定する（深さは thinking の中ではなく output_config に入れる）
    return { thinking: { type: 'adaptive' }, output_config: { effort: level } };
  }
  // OpenAI の GPT: reasoning.effort（reasoning_effort の書き方は受け付けられない）
  return { reasoning: { effort: level } };
}

/** 返事の長さの上限（考えた分も入る）と、待つ時間 */
export function limitsFor(level: Level | ''): { maxTokens: number; timeoutMs: number } {
  switch (level) {
    case 'low':
      return { maxTokens: 8192, timeoutMs: 180_000 };
    case 'medium':
      return { maxTokens: 16384, timeoutMs: 180_000 };
    case 'high':
      return { maxTokens: 32768, timeoutMs: 300_000 };
    case 'max':
      return { maxTokens: 64000, timeoutMs: 600_000 };
    default:
      return { maxTokens: 4096, timeoutMs: 180_000 };
  }
}

export type BedrockUsage = { input: number; cached: number; output: number };

/** Bedrock の返事の usage を数える（inputTokens にはキャッシュの分が入らないので、足して入力にする） */
export function readUsage(response: unknown): BedrockUsage {
  const u = ((response as { usage?: Record<string, unknown> })?.usage ?? {}) as Record<string, unknown>;
  const n = (k: string) => (typeof u[k] === 'number' && Number.isFinite(u[k]) ? Math.max(0, Math.round(u[k] as number)) : 0);
  const cached = n('cacheReadInputTokens') + n('cacheWriteInputTokens');
  return { input: n('inputTokens') + cached, cached, output: n('outputTokens') };
}

export class BedrockError extends Error {
  readonly status: number;
  constructor(message: string, status: number) {
    super(message);
    this.status = status;
  }
}

/** Bedrock の失敗を、利用者が次にどうすればよいか分かる文にする（キーの問題は管理者向け） */
function describe(status: number, type: string, detail: string, model: string, withEffort: boolean): string {
  const tail = detail ? `\n（${detail.slice(0, 300)}）` : '';
  if (status === 401 || status === 403 || type === 'UnrecognizedClientException') {
    if (/model/i.test(detail) && type === 'AccessDeniedException') return `サーバーのモデル ${model} を使う許可がありません。管理者に連絡してください。${tail}`;
    return `サーバーの Bedrock の API キーが使えません（期限切れなど）。管理者に連絡してください。${tail}`;
  }
  if (status === 404 || type === 'ResourceNotFoundException') return `サーバーのモデル ${model} が見つかりません。管理者に連絡してください。${tail}`;
  if (status === 429 || type === 'ThrottlingException' || type === 'ServiceQuotaExceededException') {
    return `AI が混み合っています。少し待ってから、もう一度送ってください。${tail}`;
  }
  if (status >= 500) return `Bedrock 側でエラーが起きました。少し待ってから、もう一度送ってください。${tail}`;
  if (withEffort && type === 'ValidationException' && (/effort/i.test(detail) || /adaptive thinking/i.test(detail))) {
    return `モデル ${model} は、この考える深さに対応していません。考える深さを変えてから、もう一度送ってください。${tail}`;
  }
  return `AI への送信が受け付けられませんでした（エラー ${status}）。${tail}`;
}

export type ConverseFn = (body: Record<string, unknown>, options: { model: string; timeoutMs: number; withEffort: boolean }) => Promise<unknown>;

/** 本物の Bedrock を呼ぶ関数を作る（テストでは、ここを差し替える） */
export function bedrockConverse(apiKey: string, region: string): ConverseFn {
  return async (body, { model, timeoutMs, withEffort }) => {
    if (!apiKey) throw new BedrockError('サーバーに Bedrock の API キーが設定されていません。管理者に連絡してください。', 503);
    const url = `https://bedrock-runtime.${region}.amazonaws.com/model/${encodeURIComponent(model)}/converse`;
    let res: Response;
    try {
      res = await fetch(url, {
        method: 'POST',
        headers: { Authorization: `Bearer ${apiKey}`, 'Content-Type': 'application/json', Accept: 'application/json' },
        body: JSON.stringify(body),
        signal: AbortSignal.timeout(timeoutMs),
      });
    } catch (err) {
      const timedOut = (err as Error).name === 'TimeoutError';
      throw new BedrockError(timedOut ? 'AI から返事がありませんでした。もう一度送ってください。' : 'サーバーから Bedrock に接続できませんでした。', 502);
    }
    const text = await res.text();
    let json: Record<string, unknown> = {};
    try {
      json = JSON.parse(text) as Record<string, unknown>;
    } catch {
      // 下で、読めなかったことを知らせる
    }
    if (!res.ok) {
      const detail = String(json.message ?? json.Message ?? '');
      const type = (res.headers.get('x-amzn-ErrorType') ?? '').split(':')[0] ?? '';
      throw new BedrockError(describe(res.status, type, detail, model, withEffort), res.status === 429 ? 429 : 502);
    }
    if (!json.output) throw new BedrockError('AI の返事を読めませんでした。もう一度送ってください。', 502);
    return json;
  };
}
