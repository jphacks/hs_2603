-- ワークショップで、テキストのテンプレートのほかに、オーディオ（BGM・効果音など）も公開できるようにする
-- kind: text（テキストのテンプレート。見た目は style）/ audio（オーディオ。中身は workshop_files）
alter table workshop_templates add column kind text not null default 'text' check (kind in ('text', 'audio'));
-- オーディオのファイルの形式（mp3 など。サーバーがファイルの中身から決めたもの）・大きさ（バイト）・長さ（秒）
alter table workshop_templates add column file_type text;
alter table workshop_templates add column file_size integer;
alter table workshop_templates add column duration double precision;
create index workshop_templates_kind_status_created on workshop_templates (kind, status, created_at desc);

-- オーディオのファイルの中身（一覧を読むときに一緒に読まないよう、別のテーブルにする）
create table workshop_files (
  template_id bigint primary key references workshop_templates (id) on delete cascade,
  data bytea not null
);
