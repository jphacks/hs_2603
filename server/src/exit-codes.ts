// サーバー（index.ts）の終了コード。start.cmd（tools/supervisor.ts）が、起動し直すかを決めるのに使う。
export const EXIT = {
  /** 設定などを直すまで起動できない（server/.env・データベースのテーブルなど）。直して保存されるのを待つ */
  config: 2,
  /** ポートを、ほかのプログラムが使っている */
  port: 3,
  /** PostgreSQL につながらない。少し待ってから、もう一度試す */
  db: 4,
} as const;
