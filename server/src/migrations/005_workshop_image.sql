-- ワークショップで、写真（フル HD まで）も公開できるようにする
-- kind: text（テキストのテンプレート）/ audio（オーディオ）/ image（写真。中身は workshop_files）
alter table workshop_templates drop constraint workshop_templates_kind_check;
alter table workshop_templates add constraint workshop_templates_kind_check check (kind in ('text', 'audio', 'image'));
-- 写真の大きさ（ピクセル）と、一覧に出す小さな見本（JPEG。アプリが作って一緒に送る）
alter table workshop_templates add column width integer;
alter table workshop_templates add column height integer;
alter table workshop_templates add column thumb bytea;
