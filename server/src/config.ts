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
  if (!databaseUrl) problems.push('DATABASE_URL（PostgreSQL への接続先）を書いてください。例: postgres://kantan:パスワード@localhost:5432/kantan');
  const region = str('AWS_REGION', 'ap-northeast-1');
  if (!validRegion(region)) problems.push(`AWS_REGION（${region}）が正しくありません。東京なら ap-northeast-1`);
  const japan = region === 'ap-northeast-1' || region === 'ap-northeast-3';
  const model = str('BEDROCK_MODEL') || (japan ? 'jp.anthropic.claude-sonnet-4-6' : 'global.anthropic.claude-sonnet-4-6');
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
    timezone,
    tlsCert,
    tlsKey,
    // HTTPS で待ち受けるなら、クッキーも HTTPS だけで送る
    cookieSecure: bool('COOKIE_SECURE') || Boolean(tlsCert),
    trustProxy: bool('TRUST_PROXY'),
  };
}
