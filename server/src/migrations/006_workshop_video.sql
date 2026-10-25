-- ワークショップで、短い動画（15 秒まで。GIF はアプリが MP4 にして送る）も公開できるようにする
-- kind: text / audio / image / video（動画。中身は workshop_files。長さ・幅と高さ・一覧の見本は、写真・オーディオと同じ列）
alter table workshop_templates drop constraint workshop_templates_kind_check;
alter table workshop_templates add constraint workshop_templates_kind_check check (kind in ('text', 'audio', 'image', 'video'));
