-- 新規登録（アプリから、使う人が自分でアカウントを作る）

-- サーバーの設定（管理画面で変える）
create table settings (
  key text primary key,
  value jsonb not null,
  updated_at timestamptz not null default now()
);

-- 新規登録の受け付け方: off（受け付けない）/ approval（管理者が承認してから使える）/ open（すぐ使える）と、登録した人のロール
insert into settings (key, value)
values ('registration', jsonb_build_object('mode', 'approval', 'roleId', (select id from roles where not is_admin order by id limit 1)));

-- 承認待ち（自分で登録して、まだ管理者が承認していない）。承認されるまでログインできない
alter table users add column pending boolean not null default false;
-- 自分で登録した人か（管理画面に出す）
alter table users add column self_registered boolean not null default false;
