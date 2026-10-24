// パスワードの保存（scrypt。Node.js に入っているので、ほかのライブラリは要らない）。
import { randomBytes, randomInt, scrypt, timingSafeEqual } from 'node:crypto';

// OWASP の目安（N=2^17, r=8, p=1。1 回 128 MiB・数百ミリ秒）
const N = 2 ** 17;
const R = 8;
const P = 1;
const KEYLEN = 64;
const MAXMEM = 256 * 1024 * 1024;

function derive(password: string, salt: Buffer, n: number, r: number, p: number): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    scrypt(password.normalize('NFKC'), salt, KEYLEN, { N: n, r, p, maxmem: MAXMEM }, (err, key) => (err ? reject(err) : resolve(key)));
  });
}

/** 保存する形: scrypt$N$r$p$salt$hash（salt と hash は base64） */
export async function hashPassword(password: string): Promise<string> {
  const salt = randomBytes(16);
  const key = await derive(password, salt, N, R, P);
  return ['scrypt', N, R, P, salt.toString('base64'), key.toString('base64')].join('$');
}

export async function verifyPassword(password: string, stored: string): Promise<boolean> {
  const [kind, n, r, p, salt, hash] = stored.split('$');
  if (kind !== 'scrypt' || !n || !r || !p || !salt || !hash) return false;
  const expected = Buffer.from(hash, 'base64');
  const key = await derive(password, Buffer.from(salt, 'base64'), Number(n), Number(r), Number(p));
  return key.length === expected.length && timingSafeEqual(key, expected);
}

// 登録されていないメールアドレスでも同じだけ時間をかける（あるかどうかを、かかる時間で知られないように）
let dummyHash: Promise<string> | null = null;
export async function burnTime(password: string): Promise<void> {
  dummyHash ??= hashPassword('dummy-password-for-timing');
  await verifyPassword(password, await dummyHash);
}

/** パスワードの決まり。だめなら理由（日本語）、よければ null */
export function passwordProblem(password: string, email = ''): string | null {
  if (password.length < 10) return 'パスワードは 10 文字以上にしてください';
  if (password.length > 128) return 'パスワードは 128 文字までにしてください';
  if (email && password.toLowerCase().includes(email.split('@')[0]?.toLowerCase() || '\u0000')) {
    return 'パスワードに、メールアドレスの @ より前の部分を入れないでください';
  }
  if (new Set(password).size < 4) return '同じ文字のくり返しは使えません';
  return null;
}

/** 仮のパスワード（まぎらわしい文字を除いた 16 文字） */
export function generatePassword(): string {
  const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789';
  let out = '';
  for (let i = 0; i < 16; i++) out += chars[randomInt(chars.length)];
  return out;
}
