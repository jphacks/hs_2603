// サーバーの設定。server/.env（無ければ環境変数）から読む。
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export type Config = {
  host: string;
  port: number;
  databaseUrl: string;
  /** Amazon Bedrock の API キー（AI を動かすのに使う。アプリには渡さない） */
  bedrockKey: string;
  region: string;
  model: string;
  /** 画像を作るモデルと、そのリージョン（Bedrock の Stability AI の画像モデルはオレゴン。チャットとは別のリージョンでよい） */
  imageModel: string;
  imageRegion: string;
  /** 画像 1 枚を、月のトークンの何トークン分として数えるか */
  imageTokens: number;
  /** トークンの上限を区切る月のタイムゾーン */
  timezone: string;
  tlsCert: string;
  tlsKey: string;
  /** 管理画面のクッキーを HTTPS だけで送るか */
  cookieSecure: boolean;
  /** 前にリバースプロキシを置いているか（接続元の IP を X-Forwarded-For から読む） */
  trustProxy: boolean;
};

/** server フォルダー */
export const serverDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

/** server/.env を読み込む（無ければ何もしない） */
export function loadEnvFile(): string | null {
  const file = path.join(serverDir, '.env');
  if (!existsSync(file)) return null;
  process.loadEnvFile(file);
  return file;
}

/** リージョンの書き方（接続先のアドレスに入るので、キーを別のサーバーへ送らないよう必ず確かめる） */
export function validRegion(region: string): boolean {
  return /^[a-z]{2}(-[a-z]+)+-[0-9]{1,2}$/.test(region);
}

function validTimezone(tz: string): boolean {
  try {
    new Intl.DateTimeFormat('ja-JP', { timeZone: tz });
    return true;
  } catch {
    return false;
  }
}

/** 設定を読んで確かめる。間違っていれば、直し方を書いたエラーを投げる */
export function readConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const problems: string[] = [];
  const str = (key: string, fallback = '') => (env[key] ?? fallback).trim();
  const bool = (key: string) => /^(1|true|yes|on)$/i.test(str(key));

  const port = Number(str('PORT', '8787'));
  if (!Number.isInteger(port) || port < 1 || port > 65535) problems.push(`PORT（${str('PORT')}）は 1〜65535 の数にしてください`);
  const databaseUrl = str('DATABASE_URL');
  if (!databaseUrl) problems.push('DATABASE_URL（PostgreSQL への接続先）を書いてください。例: postgres://e_editor:パスワード@localhost:5432/e_editor');
  const region = str('AWS_REGION', 'ap-northeast-1');
  if (!validRegion(region)) problems.push(`AWS_REGION（${region}）が正しくありません。東京なら ap-northeast-1`);
  const japan = region === 'ap-northeast-1' || region === 'ap-northeast-3';
  const model = str('BEDROCK_MODEL') || (japan ? 'jp.anthropic.claude-sonnet-4-6' : 'global.anthropic.claude-sonnet-4-6');
  const imageRegion = str('IMAGE_REGION', 'us-west-2');
  if (!validRegion(imageRegion)) problems.push(`IMAGE_REGION（${imageRegion}）が正しくありません。オレゴンなら us-west-2`);
  // 標準は、Bedrock の画像モデルでいちばんきれいな Stable Image Ultra（速さと安さを取るなら stability.stable-image-core-v1:1）
  const imageModel = str('IMAGE_MODEL', 'stability.stable-image-ultra-v1:1');
  if (!/^[A-Za-z0-9._:-]{1,128}$/.test(imageModel)) problems.push(`IMAGE_MODEL（${imageModel}）が正しくありません。例: stability.stable-image-ultra-v1:1`);
  const imageTokens = Number(str('IMAGE_TOKENS', '25000'));
  if (!Number.isSafeInteger(imageTokens) || imageTokens < 0) problems.push(`IMAGE_TOKENS（${str('IMAGE_TOKENS')}）は 0 以上の整数にしてください`);
  const timezone = str('TIMEZONE', 'Asia/Tokyo');
  if (!validTimezone(timezone)) problems.push(`TIMEZONE（${timezone}）が正しくありません。例: Asia/Tokyo`);
  const tlsCert = str('TLS_CERT');
  const tlsKey = str('TLS_KEY');
  if (Boolean(tlsCert) !== Boolean(tlsKey)) problems.push('HTTPS にするときは TLS_CERT と TLS_KEY の両方を書いてください');
  for (const file of [tlsCert, tlsKey]) {
    if (file && !existsSync(file)) problems.push(`ファイルが見つかりません: ${file}`);
  }
  if (problems.length) throw new Error(`server/.env の設定を直してください:\n- ${problems.join('\n- ')}`);
  return {
    host: str('HOST', '0.0.0.0'),
    port,
    databaseUrl,
    bedrockKey: str('AWS_BEARER_TOKEN_BEDROCK'),
    region,
    model,
    imageModel,
    imageRegion,
    imageTokens,
    timezone,
    tlsCert,
    tlsKey,
    // HTTPS で待ち受けるなら、クッキーも HTTPS だけで送る
    cookieSecure: bool('COOKIE_SECURE') || Boolean(tlsCert),
    trustProxy: bool('TRUST_PROXY'),
  };
}
