-- テキストのテンプレートのワークショップ（アプリで作ったテキストの見た目を公開して、みんなで使う）
-- 文字そのものは保存しない（見た目・登場のアニメーション・キーフレームと、名前・説明だけ）
create table workshop_templates (
  id bigint generated always as identity primary key,
  -- 公開した人（アカウントを削除すると、その人が公開したテンプレートも消える）
  user_id bigint not null references users (id) on delete cascade,
  name text not null,
  description text not null default '',
  -- テキストの見た目（サーバーが、知っている項目だけで作り直したもの）
  style jsonb not null,
  -- published: 公開中 / pending: 管理者の確認待ち / hidden: 管理者が非公開にした
  status text not null default 'published' check (status in ('published', 'pending', 'hidden')),
  -- 使った人の数（1 人 1 回まで数える）
  uses integer not null default 0,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);
create index workshop_templates_status_created on workshop_templates (status, created_at desc);
create index workshop_templates_user on workshop_templates (user_id);

-- だれが使ったか（同じ人が何度追加しても、使った人の数は 1 つだけ増やす）
create table workshop_uses (
  template_id bigint not null references workshop_templates (id) on delete cascade,
  user_id bigint not null references users (id) on delete cascade,
  created_at timestamptz not null default now(),
  primary key (template_id, user_id)
);

-- 問題のあるテンプレートの報告（1 人 1 回まで。管理画面に出す）
create table workshop_reports (
  template_id bigint not null references workshop_templates (id) on delete cascade,
  user_id bigint not null references users (id) on delete cascade,
  reason text not null default '',
  created_at timestamptz not null default now(),
  primary key (template_id, user_id)
);

-- ワークショップの使い方: off（使えない）/ approval（管理者が確認してから公開）/ open（すぐ公開）
insert into settings (key, value) values ('workshop', jsonb_build_object('mode', 'open')) on conflict (key) do nothing;
