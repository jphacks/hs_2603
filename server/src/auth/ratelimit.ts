// 回数の制限（ログインの総当たりを防ぐ）。サーバーのメモリに持つので、起動し直すと 0 に戻る。
export class RateLimiter {
  readonly max: number;
  readonly windowMs: number;
  private hits = new Map<string, number[]>();

  constructor(max: number, windowMs: number) {
    this.max = max;
    this.windowMs = windowMs;
  }

  /** 1 回数える。上限を超えていれば false（数えない） */
  hit(key: string): boolean {
    const now = Date.now();
    const list = (this.hits.get(key) ?? []).filter((t) => now - t < this.windowMs);
    if (list.length >= this.max) {
      this.hits.set(key, list);
      return false;
    }
    list.push(now);
    this.hits.set(key, list);
    // 使われなくなったキーを、たまに片付ける
    if (this.hits.size > 10_000) {
      for (const [k, v] of this.hits) {
        if (!v.some((t) => now - t < this.windowMs)) this.hits.delete(k);
      }
    }
    return true;
  }

  reset(key: string): void {
    this.hits.delete(key);
  }
}
