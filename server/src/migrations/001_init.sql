-- アカウント・ロール・ログイン・トークンの記録

-- ロール: 管理画面を使えるか、AI を使えるか、月のトークンの上限、使える考える深さ
create table roles (
  id bigint generated always as identity primary key,
  name text not null,
  is_admin boolean not null default false,
  can_use_ai boolean not null default true,
  -- 月のトークンの上限（入力と出力の合計）。null は無制限
  monthly_tokens bigint check (monthly_tokens is null or monthly_tokens >= 0),
  -- 使える深さの上限と、アプリで最初に選ばれる深さ
  max_effort text not null default 'high' check (max_effort in ('low', 'medium', 'high', 'max')),
  default_effort text not null default 'auto' check (default_effort in ('auto', 'low', 'medium', 'high', 'max')),
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);
create unique index roles_name_key on roles (lower(name));

create table users (
  id bigint generated always as identity primary key,
  -- 小文字にそろえて保存する
  email text not null check (email = lower(email)),
  display_name text not null default '',
  password_hash text not null,
  role_id bigint not null references roles (id),
  disabled boolean not null default false,
  -- 管理者が決めた仮のパスワードのまま（次のログインで変えてもらう）
  must_change_password boolean not null default false,
  -- 人ごとの上限。tokens_custom が false ならロールの上限、true なら monthly_tokens（null は無制限）
  tokens_custom boolean not null default false,
  monthly_tokens bigint check (monthly_tokens is null or monthly_tokens >= 0),
  -- 人ごとの深さ（null ならロールのまま）
  max_effort text check (max_effort in ('low', 'medium', 'high', 'max')),
  default_effort text check (default_effort in ('auto', 'low', 'medium', 'high', 'max')),
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  last_login_at timestamptz
);
create unique index users_email_key on users (email);

-- ログイン中の印（トークンそのものは保存せず、SHA-256 だけを持つ）
create table sessions (
  id bigint generated always as identity primary key,
  user_id bigint not null references users (id) on delete cascade,
  token_hash bytea not null,
  kind text not null check (kind in ('app', 'admin')),
  created_at timestamptz not null default now(),
  last_used_at timestamptz not null default now(),
  expires_at timestamptz not null,
  user_agent text not null default '',
  ip text not null default ''
);
create unique index sessions_token_key on sessions (token_hash);
create index sessions_user_idx on sessions (user_id);

-- AI を使った記録（1 回 Bedrock に送るごとに 1 行）
create table usage_events (
  id bigint generated always as identity primary key,
  user_id bigint references users (id) on delete set null,
  -- ユーザーを消しても記録は残す
  user_email text not null,
  created_at timestamptz not null default now(),
  model text not null,
  -- 送った深さ（low など。モデルの標準なら空）と、アプリで選んでいたもの（auto など）
  effort text not null default '',
  effort_choice text not null default '',
  -- 入力はキャッシュの分も含む
  input_tokens bigint not null default 0,
  cached_tokens bigint not null default 0,
  output_tokens bigint not null default 0,
  duration_ms integer not null default 0,
  ok boolean not null default true,
  error text not null default ''
);
create index usage_events_user_time_idx on usage_events (user_id, created_at);
create index usage_events_time_idx on usage_events (created_at);

-- その月だけの追加（マイナスで減らすこともできる）
create table token_grants (
  id bigint generated always as identity primary key,
  user_id bigint not null references users (id) on delete cascade,
  -- サーバーのタイムゾーンでの年月（2026-10 など）
  month text not null check (month ~ '^[0-9]{4}-(0[1-9]|1[0-2])$'),
  tokens bigint not null,
  note text not null default '',
  granted_by bigint references users (id) on delete set null,
  created_at timestamptz not null default now()
);
create index token_grants_user_month_idx on token_grants (user_id, month);

-- 管理画面での操作の記録
create table audit_log (
  id bigint generated always as identity primary key,
  admin_id bigint references users (id) on delete set null,
  admin_email text not null,
  action text not null,
  target text not null default '',
  detail jsonb not null default '{}',
  created_at timestamptz not null default now()
);
create index audit_log_time_idx on audit_log (created_at);

-- はじめのロール
insert into roles (name, is_admin, can_use_ai, monthly_tokens, max_effort, default_effort)
values ('管理者', true, true, null, 'max', 'auto'),
       ('一般', false, true, 1000000, 'high', 'auto');
