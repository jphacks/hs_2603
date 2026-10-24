#include "ai/tools.h"

#include <QBuffer>
#include <QImage>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "ai/framegrab.h"
#include "core/editor.h"
#include "core/filters.h"
#include "core/projectio.h"
#include "core/textstyles.h"
#include "core/timeline.h"

namespace kv::ai {

// ---- 道具の説明（AI に渡す） ----------------------------------------------------

static const char* kToolSpecs = R"json([
{"toolSpec": {"name": "get_project",
  "description": "今のプロジェクトの状態（クリップ・テキスト・オーディオ・メディア一覧・再生位置）を JSON で返す。変更したあとの状態を確かめたいときに使う。",
  "inputSchema": {"json": {"type": "object", "properties": {}}}}},
{"toolSpec": {"name": "view_frames",
  "description": "タイムラインの指定した時間の画面を画像で見る（プレビューと同じ見た目）。動画の中身を知りたいとき、タイトルやテロップを考える前に使う。1 回に 6 枚まで。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "times": {"type": "array", "items": {"type": "number"}, "minItems": 1, "maxItems": 6, "description": "タイムラインの時間（秒）"}},
    "required": ["times"]}}}},
{"toolSpec": {"name": "add_texts",
  "description": "テキスト（タイトル・テロップ・字幕）をタイムラインに追加する。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "items": {"type": "array", "minItems": 1, "maxItems": 30, "items": {"type": "object", "properties": {
      "text": {"type": "string", "description": "表示する文字。改行は \\n"},
      "start": {"type": "number", "description": "表示を始める時間（秒）"},
      "duration": {"type": "number", "description": "表示する長さ（秒）。省略すると 3 秒（動画の終わりをまたぐときは終わりまで）"},
      "style": {"type": "string", "enum": ["basic", "bold", "variety", "subtitle", "title", "pop", "note", "impact"],
        "description": "見た目。basic=シンプル（白い文字に細い黒フチ・下部）, bold=太フチ（白い文字に太い黒フチ・下部）, variety=バラエティ（黄色い文字に赤紫の太いフチ・下部）, subtitle=字幕（小さめの白い文字に半透明の黒い帯・下部）, title=タイトル（大きな明朝体の白い文字・中央）, pop=ポップ（ピンクの丸い文字に白い太フチ・中央）, note=メモ（クリーム色の帯に黒い文字・上部）, impact=インパクト（とても大きな白い文字に赤いフチで、遠目には赤く見える・中央）。color は文字の中の色だけを変え、フチや帯の色は変わらない。省略すると basic"},
      "position": {"type": "string", "enum": ["top", "center", "bottom"], "description": "画面の上部・中央・下部。省略するとスタイルの標準の位置"},
      "font": {"type": "string", "enum": ["gothic", "meiryo", "maru", "ud", "mincho", "kyokasho", "impact"], "description": "フォント（省略するとスタイルのまま。名前は update_text と同じ）"},
      "size": {"type": "number", "description": "文字の大きさ。画面の高さに対する %（3〜22）。省略するとスタイルのまま"},
      "color": {"type": "string", "description": "文字の色 #RRGGBB。省略するとスタイルのまま"},
      "animation": {"type": "string", "enum": ["none", "fade", "slide"], "description": "出し方。省略するとスタイルのまま"}},
      "required": ["text", "start"]}}},
    "required": ["items"]}}}},
{"toolSpec": {"name": "update_text",
  "description": "テキストを変更する。指定した項目だけが変わる。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "id": {"type": "string"},
    "text": {"type": "string"},
    "start": {"type": "number", "description": "表示を始める時間（秒）"},
    "duration": {"type": "number", "description": "表示する長さ（秒）"},
    "style": {"type": "string", "enum": ["basic", "bold", "variety", "subtitle", "title", "pop", "note", "impact"], "description": "スタイルを当てはめ直す（文字・時間・位置はそのまま。見た目は add_texts の説明と同じ）"},
    "position": {"type": "string", "enum": ["top", "center", "bottom"]},
    "font": {"type": "string", "enum": ["gothic", "meiryo", "maru", "ud", "mincho", "kyokasho", "impact"],
      "description": "gothic=ゴシック, meiryo=メイリオ, maru=丸ゴシック, ud=UDゴシック, mincho=明朝, kyokasho=教科書体, impact=Impact"},
    "size": {"type": "number", "description": "文字の大きさ。画面の高さに対する %（3〜22。標準は 7.5）"},
    "color": {"type": "string", "description": "文字の色 #RRGGBB"},
    "animation": {"type": "string", "enum": ["none", "fade", "slide"], "description": "出し方。none=なし, fade=フェード, slide=スライド"}},
    "required": ["id"]}}}},
{"toolSpec": {"name": "update_clip",
  "description": "ビデオトラックのクリップ（動画・写真）を変更する。指定した項目だけが変わる。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "id": {"type": "string"},
    "speed": {"type": "number", "description": "再生速度（0.25〜4 倍）。動画だけ。長さは 1/速度 になる"},
    "volume": {"type": "number", "description": "音量 %（0〜100）。音量キーフレームがあるときは外して、全体をこの音量にする"},
    "muted": {"type": "boolean", "description": "true で消音"},
    "fit": {"type": "string", "enum": ["contain", "cover", "blur"], "description": "表示方法。contain=全体表示, cover=拡大, blur=ぼかし背景"},
    "filter": {"type": "string", "enum": ["none", "vivid", "warm", "cool", "soft", "drama", "retro", "sepia", "mono"],
      "description": "色のフィルター（明るさなどの調整は標準に戻る）。none=ノーマル, vivid=ビビッド, warm=ウォーム, cool=クール, soft=ソフト, drama=ドラマ, retro=レトロ, sepia=セピア, mono=モノクロ"},
    "fade_in": {"type": "number", "description": "黒からのフェードイン（秒、0〜3）"},
    "fade_out": {"type": "number", "description": "黒へのフェードアウト（秒、0〜3）"},
    "duration": {"type": "number", "description": "写真の表示時間（秒、0.2〜600）。写真だけ"},
    "source_start": {"type": "number", "description": "元の動画のどこから使うか（秒）。頭を切るときに使う。動画だけ"},
    "source_end": {"type": "number", "description": "元の動画のどこまで使うか（秒）。終わりを切るときに使う。動画だけ"}},
    "required": ["id"]}}}},
{"toolSpec": {"name": "move_clip",
  "description": "ビデオトラックのクリップの順番を変える。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "id": {"type": "string"},
    "to_index": {"type": "integer", "description": "移した後の位置（0 が先頭）"}},
    "required": ["id", "to_index"]}}}},
{"toolSpec": {"name": "split_clip",
  "description": "ビデオトラックのクリップを、タイムラインの指定した時間で 2 つに分ける。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "time": {"type": "number", "description": "分ける時間（秒）"}},
    "required": ["time"]}}}},
{"toolSpec": {"name": "delete_items",
  "description": "クリップ・テキスト・オーディオを ID で削除する。後ろのクリップは前に詰まる。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "ids": {"type": "array", "items": {"type": "string"}, "minItems": 1}},
    "required": ["ids"]}}}},
{"toolSpec": {"name": "add_media",
  "description": "メディア一覧にある素材をタイムラインに追加する。動画・写真はビデオトラックに、音楽・音声はオーディオトラックに入る。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "asset_id": {"type": "string", "description": "media_library の id"},
    "index": {"type": "integer", "description": "動画・写真を入れる位置（0 が先頭）。省略すると最後"},
    "start": {"type": "number", "description": "音楽・音声を始める時間（秒）。省略すると 0"}},
    "required": ["asset_id"]}}}},
{"toolSpec": {"name": "update_audio",
  "description": "オーディオトラックの音楽・音声を変更する。指定した項目だけが変わる。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "id": {"type": "string"},
    "volume": {"type": "number", "description": "音量 %（0〜100）。音量キーフレームがあるときは外して、全体をこの音量にする"},
    "start": {"type": "number", "description": "始める時間（秒）"},
    "fade_in": {"type": "number", "description": "秒（0〜10）"},
    "fade_out": {"type": "number", "description": "秒（0〜10）"},
    "fit_to_video": {"type": "boolean", "description": "true で、動画の終わりで止まるように長さを合わせる（フェードアウトが無ければ 2 秒つける）"}},
    "required": ["id"]}}}},
{"toolSpec": {"name": "set_project",
  "description": "画面の比率やフレームレートを変える。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "aspect": {"type": "string", "enum": ["16:9", "9:16", "1:1", "4:5"], "description": "16:9=横長, 9:16=縦長（ショート動画）, 1:1=正方形, 4:5=縦長の投稿"},
    "fps": {"type": "integer", "enum": [24, 30, 60]}}}}}},
{"toolSpec": {"name": "propose_texts",
  "description": "タイトルやテロップの案を、選べるボタンとしてユーザーに見せる。タイムラインには追加しない（ユーザーが選んだものだけが追加される）。案を頼まれたときはこれを使う。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "kind": {"type": "string", "enum": ["title", "telop"], "description": "title=タイトルの案（どれか 1 つを選ぶ。同じ場面の候補をいくつか出すとき）, telop=場面ごとのテロップ 1 つずつ（「すべて追加」で並べて入るので、時間を重ねない）"},
    "items": {"type": "array", "minItems": 1, "maxItems": 10, "items": {"type": "object", "properties": {
      "text": {"type": "string"},
      "start": {"type": "number", "description": "表示を始める時間（秒）。テロップでは必ず指定する。タイトルは省略すると 0"},
      "duration": {"type": "number", "description": "表示する長さ（秒）"},
      "style": {"type": "string", "enum": ["basic", "bold", "variety", "subtitle", "title", "pop", "note", "impact"]},
      "position": {"type": "string", "enum": ["top", "center", "bottom"]}},
      "required": ["text"]}}},
    "required": ["kind", "items"]}}}},
{"toolSpec": {"name": "propose_choices",
  "description": "編集のやり方の案を 2〜3 個作り、それぞれを当てはめたときのプレビュー画像を並べて、ユーザーに選んでもらう。この時点ではタイムラインは変えない（ユーザーが「これにする」を押した案だけが反映される）。見た目の好みで決まる編集を任されたとき（スタイル・位置・色・フィルター・表示方法などを言われていないとき）に使う。結果には、各案のプレビュー画像が付く。同じ依頼の中でもう一度使うと、前に見せた案と置き換わる。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "question": {"type": "string", "description": "ユーザーへの短い問いかけ（例: どちらの雰囲気にしますか？）"},
    "times": {"type": "array", "items": {"type": "number"}, "minItems": 1, "maxItems": 2, "description": "プレビューを作るタイムラインの時間（秒）。どの案も同じ時間の画面を見せる。省略すると、案が変える場所から決める"},
    "options": {"type": "array", "minItems": 2, "maxItems": 3, "items": {"type": "object", "properties": {
      "label": {"type": "string", "description": "案の短い名前（10 文字くらい。例: ポップに、落ち着いた字幕）。A・B などの記号は付けない（アプリが付ける）"},
      "description": {"type": "string", "description": "どんな案か（1 文）。英語の ID（title・vivid など）や道具の名前は使わず、見た目をふつうの言葉で書く（例: 黄色の丸い文字で上に）"},
      "actions": {"type": "array", "minItems": 1, "maxItems": 20, "description": "この案で行う編集。上から順に行う", "items": {"type": "object", "properties": {
        "tool": {"type": "string", "enum": ["add_texts", "update_text", "update_clip", "move_clip", "split_clip", "delete_items", "add_media", "update_audio", "set_project"]},
        "input": {"type": "object", "description": "その道具の入力（その道具の説明と同じ形）"}},
        "required": ["tool", "input"]}}},
      "required": ["label", "actions"]}}},
    "required": ["options"]}}}},
{"toolSpec": {"name": "seek",
  "description": "再生位置を動かす。変えた場所をユーザーに見せたいときに使う。",
  "inputSchema": {"json": {"type": "object", "properties": {
    "time": {"type": "number", "description": "秒"}},
    "required": ["time"]}}}}
])json";

const QJsonArray& toolSpecs() {
  static const QJsonArray specs = QJsonDocument::fromJson(QByteArray(kToolSpecs)).array();
  return specs;
}

// ---- プロジェクトの説明 ------------------------------------------------------------

static double r2(double v) {
  return std::round(v * 100) / 100;
}

static QString kindName(AssetKind k) {
  return k == AssetKind::Image ? QStringLiteral("image") : k == AssetKind::Audio ? QStringLiteral("audio") : QStringLiteral("video");
}

static QString animName(TextAnimation a) {
  return a == TextAnimation::None ? QStringLiteral("none") : a == TextAnimation::SlideUp ? QStringLiteral("slide") : QStringLiteral("fade");
}

static bool usedOnTimeline(const ProjectData& p, const QString& assetId) {
  for (const auto& c : p.clips) {
    if (c.assetId == assetId) return true;
  }
  for (const auto& a : p.audios) {
    if (a.assetId == assetId) return true;
  }
  return false;
}

static QJsonObject clipJson(const ClipLayout& l) {
  const Editor& ed = editor();
  const VideoClip& c = l.clip;
  const Asset* a = ed.asset(c.assetId);
  QJsonObject o{{QStringLiteral("id"), c.id},
                {QStringLiteral("index"), l.index},
                {QStringLiteral("name"), a ? a->name : QStringLiteral("（見つからないファイル）")},
                {QStringLiteral("kind"), c.isImage ? QStringLiteral("image") : QStringLiteral("video")},
                {QStringLiteral("start"), r2(l.start)},
                {QStringLiteral("end"), r2(l.end)},
                {QStringLiteral("duration"), r2(l.duration)},
                {QStringLiteral("fit"), fitToString(c.fit)},
                {QStringLiteral("filter"), presetToString(c.color.preset)},
                {QStringLiteral("fade_in"), r2(c.fadeIn)},
                {QStringLiteral("fade_out"), r2(c.fadeOut)}};
  if (!c.isImage) {
    o.insert(QStringLiteral("speed"), r2(c.speed));
    o.insert(QStringLiteral("source_start"), r2(c.in));
    o.insert(QStringLiteral("source_end"), r2(c.out));
    if (a) o.insert(QStringLiteral("source_length"), r2(a->duration));
    o.insert(QStringLiteral("has_audio"), a && a->hasAudio);
    o.insert(QStringLiteral("volume"), r2(c.volume * 100));
    o.insert(QStringLiteral("muted"), c.muted);
    if (!c.volumePoints.empty()) o.insert(QStringLiteral("volume_keyframes"), int(c.volumePoints.size()));
  }
  if (!c.anim.empty()) o.insert(QStringLiteral("has_motion_keyframes"), true);
  return o;
}

static QJsonObject textJson(const TextItem& t) {
  return {{QStringLiteral("id"), t.id},
          {QStringLiteral("text"), t.text},
          {QStringLiteral("start"), r2(t.start)},
          {QStringLiteral("end"), r2(t.start + t.duration)},
          {QStringLiteral("duration"), r2(t.duration)},
          {QStringLiteral("x"), r2(t.x)},
          {QStringLiteral("y"), r2(t.y)},
          {QStringLiteral("size"), r2(t.size * 100)},
          {QStringLiteral("font"), fontToString(t.font)},
          {QStringLiteral("color"), t.color.name()},
          {QStringLiteral("animation"), animName(t.animation)},
          {QStringLiteral("track"), t.track}};
}

static QJsonObject audioJson(const AudioItem& au) {
  const Asset* a = editor().asset(au.assetId);
  QJsonObject o{{QStringLiteral("id"), au.id},
                {QStringLiteral("name"), a ? a->name : QStringLiteral("（見つからないファイル）")},
                {QStringLiteral("start"), r2(au.start)},
                {QStringLiteral("end"), r2(au.start + audioDuration(au))},
                {QStringLiteral("duration"), r2(audioDuration(au))},
                {QStringLiteral("volume"), r2(au.volume * 100)},
                {QStringLiteral("fade_in"), r2(au.fadeIn)},
                {QStringLiteral("fade_out"), r2(au.fadeOut)},
                {QStringLiteral("source_start"), r2(au.in)}};
  if (a) o.insert(QStringLiteral("source_length"), r2(a->duration));
  if (!au.volumePoints.empty()) o.insert(QStringLiteral("volume_keyframes"), int(au.volumePoints.size()));
  return o;
}

QString promptJson(const QJsonObject& o) {
  // < > は JSON の文字列の中にしか出てこないので、どこを置き換えても JSON のまま
  QString s = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
  s.replace(QLatin1Char('<'), QLatin1String("\\u003c"));
  s.replace(QLatin1Char('>'), QLatin1String("\\u003e"));
  return s;
}

QJsonObject describeProject() {
  const Editor& ed = editor();
  const ProjectData& p = ed.project();
  QJsonArray clips;
  for (const auto& l : ed.layout().items) clips.append(clipJson(l));
  QJsonArray texts;
  for (const auto& t : p.texts) texts.append(textJson(t));
  QJsonArray audios;
  for (const auto& a : p.audios) audios.append(audioJson(a));
  QJsonArray library;
  for (const QString& id : ed.assetOrder()) {
    const Asset* a = ed.asset(id);
    if (!a) continue;
    QJsonObject o{{QStringLiteral("id"), a->id},
                  {QStringLiteral("name"), a->name},
                  {QStringLiteral("kind"), kindName(a->kind)},
                  {QStringLiteral("used_on_timeline"), usedOnTimeline(p, a->id)}};
    if (a->kind != AssetKind::Image) o.insert(QStringLiteral("duration"), r2(a->duration));
    if (a->kind != AssetKind::Audio) {
      o.insert(QStringLiteral("width"), a->width);
      o.insert(QStringLiteral("height"), a->height);
    }
    if (a->status == AssetStatus::Missing) o.insert(QStringLiteral("missing"), true);
    library.append(o);
  }
  QJsonValue selected;
  const Selection& s = ed.selection();
  // 重ねるトラックの映像は、AI の操作の対象にしていないので、選んでいるものとしては伝えない
  if (s && s.type != Selection::Overlay) {
    const QString type = s.type == Selection::Clip ? QStringLiteral("clip") : s.type == Selection::Text ? QStringLiteral("text") : QStringLiteral("audio");
    selected = QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("id"), s.id}};
  }
  return {{QStringLiteral("aspect"), aspectToString(p.settings.aspect)},
          {QStringLiteral("fps"), p.settings.fps},
          {QStringLiteral("video_length"), r2(ed.layout().total)},
          {QStringLiteral("playhead"), r2(ed.currentTime())},
          {QStringLiteral("selected"), selected},
          {QStringLiteral("clips"), clips},
          {QStringLiteral("texts"), texts},
          {QStringLiteral("audios"), audios},
          {QStringLiteral("media_library"), library}};
}

// ---- 道具の中身 --------------------------------------------------------------------

namespace {

template <typename T>
T* findById(std::vector<T>& list, const QString& id) {
  for (auto& x : list) {
    if (x.id == id) return &x;
  }
  return nullptr;
}

QJsonObject textBlock(const QJsonObject& o) {
  return {{QStringLiteral("text"), promptJson(o)}};
}

ToolResult okResult(const QJsonObject& o = {}) {
  ToolResult r;
  QJsonObject body = o;
  body.insert(QStringLiteral("ok"), true);
  // 速度などで動画の長さが変わったことに、AI が次の判断で気づけるよう、いつも今の長さを付ける
  body.insert(QStringLiteral("video_length"), r2(editor().layout().total));
  r.content.append(textBlock(body));
  return r;
}

/**
 * テキストが動画（長さ total 秒）の外にあるときの注意（書き出しでは動画の長さまでしか出ないので、AI に直してもらう）。
 * 問題なければ空。テキストは id と時間で示す（文字を「」で入れると、文字の中の 」 のあとが、アプリからの注意に見えてしまう。
 * propose_choices の案の中で足したテキストの id は AI に見えないので、時間でも分かるようにする）
 */
QString timingNote(const TextItem& t, double total) {
  if (total <= 0) return {};
  const QString which = QStringLiteral("テキスト（id %1、%2〜%3 秒）").arg(t.id).arg(r2(t.start)).arg(r2(t.start + t.duration));
  if (t.start >= total - 0.05) {
    return QStringLiteral("注意: %1は動画の終わり（%2 秒）より後ろにあるので、書き出しでは表示されません。"
                          "動画の長さに合わせて start を直してください。")
        .arg(which)
        .arg(r2(total));
  }
  if (t.start + t.duration > total + 0.05) {
    return QStringLiteral("注意: %1は動画の終わり（%2 秒）をはみ出しています。はみ出した部分は書き出されません。")
        .arg(which)
        .arg(r2(total));
  }
  return {};
}

QString timingNote(const TextItem& t) {
  return timingNote(t, editor().layout().total);
}

ToolResult errorResult(const QString& message) {
  ToolResult r;
  r.error = true;
  r.message = message;
  r.content.append(textBlock({{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}}));
  return r;
}

QString quote(const QString& text) {
  QString t = text;
  t.replace(QLatin1Char('\n'), QLatin1Char(' '));
  // 中のかぎかっこは『』にする（素材の名前やテキストの 」 で引用が閉じて、そのあとがアプリの文に見えないように。
  // この行は画面に出し、propose_choices の結果で AI にも渡す）。半角 ｢｣ と縦書き用 ﹁﹂ も、AI には同じ 「」 に見えるので変える
  for (const QChar c : {QChar(u'「'), QChar(u'｢'), QChar(u'﹁')}) t.replace(c, QChar(u'『'));
  for (const QChar c : {QChar(u'」'), QChar(u'｣'), QChar(u'﹂')}) t.replace(c, QChar(u'』'));
  if (t.size() > 24) t = t.left(23) + QStringLiteral("…");
  return QStringLiteral("「%1」").arg(t);
}

QString span(double start, double end) {
  return QStringLiteral("%1〜%2").arg(formatTime(start), formatTime(end));
}

const QStringList& styleIds() {
  static const QStringList ids = {QStringLiteral("basic"), QStringLiteral("bold"), QStringLiteral("variety"), QStringLiteral("subtitle"),
                                  QStringLiteral("title"), QStringLiteral("pop"),  QStringLiteral("note"),    QStringLiteral("impact")};
  return ids;
}

QString styleLabel(const QString& id) {
  return textPreset(id).label;
}

std::optional<double> positionY(const QString& pos) {
  // 右側の設定の「上部・中央・下部」ボタンと同じ位置
  if (pos == u"top") return 0.14;
  if (pos == u"center") return 0.5;
  if (pos == u"bottom") return 0.84;
  return std::nullopt;
}

std::optional<double> number(const QJsonObject& o, const char* key) {
  const QJsonValue v = o.value(QLatin1String(key));
  if (!v.isDouble() || !std::isfinite(v.toDouble())) return std::nullopt;
  return v.toDouble();
}

QString string(const QJsonObject& o, const char* key) {
  return o.value(QLatin1String(key)).toString();
}

QString assetName(const QString& assetId) {
  const Asset* a = editor().asset(assetId);
  return a ? a->name : QStringLiteral("クリップ");
}

/** 新しいテキスト（段・長さ・位置まで決める）。p は追加する前の状態 */
TextItem buildText(const ProjectData& p, const QString& text, double start, std::optional<double> duration, const QString& style,
                   const QString& position) {
  const int fps = p.settings.fps;
  const QString styleId = styleIds().contains(style) ? style : QStringLiteral("basic");
  TextItem t = makeText(styleId, frameRound(std::max(0.0, start), fps), text);
  if (duration) {
    t.duration = std::clamp(*duration, kMinTextDuration, 600.0);
  } else {
    // 動画の終わりをまたぐときは、終わりまでにする（テキストを追加するときと同じ）
    const double total = layoutClips(p.clips, fps).total;
    if (total > 1 && t.start < total && t.start + t.duration > total) t.duration = std::max(kMinTextDuration, total - t.start);
  }
  if (const auto y = positionY(position)) {
    t.x = 0.5;
    t.y = *y;
  }
  t.track = freeTextTrack(p.texts, t.start, t.duration);
  return t;
}

/** テキストの見た目（フォント・大きさ・色・出し方）を、AI の入力 o のとおりに変える。変えた項目の名前を changed に足す */
void applyTextLook(TextItem& t, const QJsonObject& o, QStringList& changed) {
  const QString font = string(o, "font");
  if (!font.isEmpty()) {
    t.font = fontFromString(font);
    // 読み込んだフォント・一部だけ変えたフォントがあると、組み込みの種類を変えても見た目が変わらないので、そろえる
    t.fontFamily.clear();
    t.fontSpans.clear();
    changed << QStringLiteral("フォント");
  }
  if (const auto sz = number(o, "size")) {
    t.size = std::clamp(*sz / 100.0, 0.03, 0.22);
    changed << QStringLiteral("大きさ");
  }
  const QColor color = QColor::fromString(string(o, "color"));
  if (o.contains(QStringLiteral("color")) && color.isValid()) {
    t.color = color;
    changed << QStringLiteral("色");
  }
  const QString anim = string(o, "animation");
  if (!anim.isEmpty()) {
    t.animation = anim == u"none" ? TextAnimation::None : anim == u"slide" ? TextAnimation::SlideUp : TextAnimation::Fade;
    changed << QStringLiteral("出し方");
  }
}

ToolResult addTexts(const QJsonObject& in, const QString& merge) {
  const QJsonArray items = in.value(QStringLiteral("items")).toArray();
  if (items.isEmpty()) return errorResult(QStringLiteral("items が空です"));
  QJsonArray added;
  std::vector<TextItem> made;
  QStringList log;
  QStringList problems;
  editor().commit([&](ProjectData& p) {
    for (const QJsonValue& v : items) {
      const QJsonObject o = v.toObject();
      const QString text = string(o, "text").trimmed();
      const auto start = number(o, "start");
      if (text.isEmpty() || !start) {
        problems << QStringLiteral("text と start が必要です");
        continue;
      }
      TextItem t = buildText(p, text, *start, number(o, "duration"), string(o, "style"), string(o, "position"));
      QStringList look;
      applyTextLook(t, o, look);
      p.texts.push_back(t);
      made.push_back(t);
      added.append(textJson(t));
      log << QStringLiteral("テキストを追加: %1（%2）").arg(quote(t.text), span(t.start, t.start + t.duration));
    }
    return !added.isEmpty();
  }, merge);
  if (added.isEmpty()) return errorResult(problems.isEmpty() ? QStringLiteral("追加できませんでした") : problems.join(QLatin1Char(' ')));
  ToolResult r = okResult({{QStringLiteral("added"), added}});
  if (!problems.isEmpty()) r.content.append(QJsonObject{{QStringLiteral("text"), QStringLiteral("一部を追加できませんでした: ") + problems.join(QLatin1Char(' '))}});
  for (const TextItem& t : made) {
    const QString note = timingNote(t);
    if (!note.isEmpty()) r.content.append(QJsonObject{{QStringLiteral("text"), note}});
  }
  r.log = log;
  return r;
}

ToolResult updateText(const QJsonObject& in, const QString& merge) {
  const QString id = string(in, "id");
  if (!editor().text(id)) return errorResult(QStringLiteral("テキスト %1 が見つかりません").arg(id));
  QStringList changed;
  QJsonObject after;
  editor().commit([&](ProjectData& p) {
    TextItem* t = findById(p.texts, id);
    if (!t) return false;
    const int fps = p.settings.fps;
    const QString style = string(in, "style");
    if (styleIds().contains(style)) {
      *t = applyTextPreset(*t, style);
      changed << QStringLiteral("スタイル「%1」").arg(styleLabel(style));
    }
    if (in.contains(QStringLiteral("text"))) {
      t->text = string(in, "text");
      changed << QStringLiteral("文字");
    }
    if (const auto s = number(in, "start")) {
      t->start = frameRound(std::max(0.0, *s), fps);
      changed << QStringLiteral("開始");
    }
    if (const auto d = number(in, "duration")) {
      t->duration = std::clamp(*d, kMinTextDuration, 600.0);
      changed << QStringLiteral("長さ");
    }
    if (const auto y = positionY(string(in, "position"))) {
      t->x = 0.5;
      t->y = *y;
      changed << QStringLiteral("位置");
    }
    applyTextLook(*t, in, changed);
    after = textJson(*t);
    return !changed.isEmpty();
  }, merge);
  if (changed.isEmpty()) return errorResult(QStringLiteral("変える項目がありません"));
  ToolResult r = okResult({{QStringLiteral("text"), after}});
  if (const TextItem* now = editor().text(id)) {
    const QString note = timingNote(*now);
    if (!note.isEmpty()) r.content.append(QJsonObject{{QStringLiteral("text"), note}});
  }
  r.log << QStringLiteral("テキスト%1を変更: %2").arg(quote(after.value(QStringLiteral("text")).toString()), changed.join(QStringLiteral("・")));
  return r;
}

ToolResult updateClip(const QJsonObject& in, const QString& merge) {
  const QString id = string(in, "id");
  const VideoClip* cur = editor().clip(id);
  if (!cur) return errorResult(QStringLiteral("クリップ %1 が見つかりません").arg(id));
  // commit は編集データを丸ごと置き換えるので、cur はその後は使えない。必要な値は先に写しておく
  const QString assetId = cur->assetId;
  const Asset* asset = editor().asset(assetId);
  const double assetDuration = asset ? asset->duration : cur->out;
  cur = nullptr;
  QStringList changed;
  QStringList skipped;
  editor().commit([&](ProjectData& c0) {
    VideoClip* c = findById(c0.clips, id);
    if (!c) return false;
    if (const auto s = number(in, "speed")) {
      if (c->isImage) {
        skipped << QStringLiteral("写真は速度を変えられません");
      } else {
        // 右側の設定で速度を変えるときと同じく、キーフレームは同じ映像の位置に合わせて伸び縮みさせる
        const double ns = std::clamp(*s, 0.25, 4.0);
        const double f = c->speed / ns;
        c->anim = scaleAnimationTime(c->anim, f);
        for (auto& pt : c->volumePoints) pt.t *= f;
        c->speed = ns;
        changed << QStringLiteral("速度 %1 倍").arg(ns);
      }
    }
    if (const auto v = number(in, "volume")) {
      c->volumePoints.clear();
      c->volume = std::clamp(*v / 100.0, 0.0, kMaxVolume);
      changed << QStringLiteral("音量 %1%").arg(std::lround(c->volume * 100));
    }
    if (in.contains(QStringLiteral("muted"))) {
      c->muted = in.value(QStringLiteral("muted")).toBool();
      changed << (c->muted ? QStringLiteral("消音") : QStringLiteral("消音を解除"));
    }
    const QString fit = string(in, "fit");
    if (!fit.isEmpty()) {
      c->fit = fitFromString(fit);
      changed << QStringLiteral("表示方法「%1」").arg(c->fit == FitMode::Cover ? QStringLiteral("拡大")
                                                      : c->fit == FitMode::Blur  ? QStringLiteral("ぼかし背景")
                                                                                 : QStringLiteral("全体表示"));
    }
    const QString filter = string(in, "filter");
    if (!filter.isEmpty()) {
      const FilterPreset fp = presetFromString(filter);
      c->color = colorFromPreset(fp);
      changed << QStringLiteral("フィルター「%1」").arg(presetInfo(fp).label);
    }
    if (const auto f = number(in, "fade_in")) {
      c->fadeIn = std::clamp(*f, 0.0, 3.0);
      changed << QStringLiteral("フェードイン %1 秒").arg(c->fadeIn);
    }
    if (const auto f = number(in, "fade_out")) {
      c->fadeOut = std::clamp(*f, 0.0, 3.0);
      changed << QStringLiteral("フェードアウト %1 秒").arg(c->fadeOut);
    }
    if (const auto d = number(in, "duration")) {
      if (!c->isImage) {
        skipped << QStringLiteral("動画の長さは source_start / source_end か speed で変えてください");
      } else {
        c->out = c->in + std::clamp(*d, kMinClipDuration, 600.0);
        changed << QStringLiteral("表示時間 %1 秒").arg(c->out - c->in);
      }
    }
    if (const auto s = number(in, "source_start")) {
      if (c->isImage) {
        skipped << QStringLiteral("写真には source_start はありません");
      } else {
        const double ni = std::clamp(*s, 0.0, std::max(0.0, c->out - kMinClipDuration * c->speed));
        // タイムラインで左端を詰めたときと同じく、キーフレームの時間をずらして同じ映像の位置に合わせ直す
        const double headCut = (ni - c->in) / c->speed;
        c->anim = trimAnimationStart(c->anim, headCut);
        c->volumePoints = trimVolumePointsStart(c->volumePoints, c->volume, headCut);
        c->in = ni;
        changed << QStringLiteral("使う範囲の始め %1 秒").arg(r2(ni));
      }
    }
    if (const auto e = number(in, "source_end")) {
      if (c->isImage) {
        skipped << QStringLiteral("写真には source_end はありません");
      } else {
        c->out = std::clamp(*e, c->in + kMinClipDuration * c->speed, std::max(c->in + kMinClipDuration * c->speed, assetDuration));
        changed << QStringLiteral("使う範囲の終わり %1 秒").arg(r2(c->out));
      }
    }
    return !changed.isEmpty();
  }, merge);
  if (changed.isEmpty()) return errorResult(skipped.isEmpty() ? QStringLiteral("変える項目がありません") : skipped.join(QLatin1Char(' ')));
  QJsonObject after;
  for (const auto& l : editor().layout().items) {
    if (l.clip.id == id) after = clipJson(l);
  }
  ToolResult r = okResult({{QStringLiteral("clip"), after}});
  if (!skipped.isEmpty()) r.content.append(QJsonObject{{QStringLiteral("text"), QStringLiteral("変えなかった項目: ") + skipped.join(QLatin1Char(' '))}});
  // 長さが変わって、すでにあるテキストが動画の外に出たら知らせる
  for (const TextItem& t : editor().project().texts) {
    const QString note = timingNote(t);
    if (!note.isEmpty()) r.content.append(QJsonObject{{QStringLiteral("text"), note}});
  }
  r.log << QStringLiteral("%1を変更: %2").arg(quote(assetName(assetId)), changed.join(QStringLiteral("・")));
  return r;
}

ToolResult moveClip(const QJsonObject& in, const QString& merge) {
  const QString id = string(in, "id");
  const int to = in.value(QStringLiteral("to_index")).toInt(-1);
  const VideoClip* cur = editor().clip(id);
  if (!cur) return errorResult(QStringLiteral("クリップ %1 が見つかりません").arg(id));
  const QString name = assetName(cur->assetId);
  int placed = -1;
  editor().commit([&](ProjectData& p) {
    auto it = std::find_if(p.clips.begin(), p.clips.end(), [&](const VideoClip& c) { return c.id == id; });
    if (it == p.clips.end()) return false;
    const int from = int(it - p.clips.begin());
    const VideoClip c = *it;
    p.clips.erase(it);
    placed = std::clamp(to, 0, int(p.clips.size()));
    p.clips.insert(p.clips.begin() + placed, c);
    return placed != from;
  }, merge);
  if (placed < 0) return errorResult(QStringLiteral("移せませんでした"));
  ToolResult r = okResult({{QStringLiteral("index"), placed}});
  r.log << QStringLiteral("%1を %2 番目へ移動").arg(quote(name)).arg(placed + 1);
  return r;
}

ToolResult splitClip(const QJsonObject& in, const QString& merge) {
  const auto time = number(in, "time");
  if (!time) return errorResult(QStringLiteral("time が必要です"));
  const Editor& ed = editor();
  const int fps = ed.project().settings.fps;
  const double t = frameRound(*time, fps);
  const ClipLayout* hit = findClipAt(ed.layout(), t);
  if (!hit || t <= hit->start || t >= hit->end) return errorResult(QStringLiteral("%1 にクリップがありません").arg(formatTime(t)));
  const int localFrames = int(std::floor((t - hit->start) * fps + 0.5));
  const int totalFrames = int(std::floor(hit->duration * fps + 0.5));
  if (localFrames < 1 || localFrames > totalFrames - 1) return errorResult(QStringLiteral("クリップの端では分けられません"));
  // 再生ヘッドで分割するときと同じ分け方
  const double local = double(localFrames) / fps;
  // hit はタイムラインの計算結果を指していて、commit のあとで計算し直されると使えなくなる。必要な値は先に写しておく
  const VideoClip c = hit->clip;
  const double hitStart = hit->start;
  hit = nullptr;
  const double cut = c.isImage ? c.in + local : c.in + local * c.speed;
  const QString secondId = newId();
  bool done = false;
  editor().commit([&](ProjectData& p) {
    for (size_t i = 0; i < p.clips.size(); ++i) {
      if (p.clips[i].id != c.id) continue;
      VideoClip second = c;
      second.id = secondId;
      second.in = cut;
      second.fadeIn = 0;
      p.clips[i].out = cut;
      p.clips[i].fadeOut = 0;
      splitAnimation(c.anim, local, p.clips[i].anim, second.anim);
      splitVolumePointList(c.volumePoints, c.volume, local, p.clips[i].volumePoints, second.volumePoints);
      p.clips.insert(p.clips.begin() + long(i) + 1, second);
      done = true;
      return true;
    }
    return false;
  }, merge);
  if (!done) return errorResult(QStringLiteral("分けられませんでした"));
  ToolResult r = okResult({{QStringLiteral("first_id"), c.id}, {QStringLiteral("second_id"), secondId}});
  r.log << QStringLiteral("%1を %2 で分割").arg(quote(assetName(c.assetId)), formatTime(hitStart + local));
  return r;
}

ToolResult deleteItems(const QJsonObject& in, const QString& merge) {
  QStringList ids;
  for (const QJsonValue& v : in.value(QStringLiteral("ids")).toArray()) ids << v.toString();
  QStringList removed;
  QStringList missing;
  editor().commit([&](ProjectData& p) {
    for (const QString& id : ids) {
      bool found = false;
      for (auto it = p.clips.begin(); it != p.clips.end(); ++it) {
        if (it->id != id) continue;
        removed << quote(assetName(it->assetId));
        p.clips.erase(it);
        found = true;
        break;
      }
      for (auto it = p.texts.begin(); !found && it != p.texts.end(); ++it) {
        if (it->id != id) continue;
        removed << quote(it->text);
        p.texts.erase(it);
        found = true;
        break;
      }
      for (auto it = p.audios.begin(); !found && it != p.audios.end(); ++it) {
        if (it->id != id) continue;
        removed << quote(assetName(it->assetId));
        p.audios.erase(it);
        found = true;
        break;
      }
      if (!found) missing << id;
    }
    return !removed.isEmpty();
  }, merge);
  if (removed.isEmpty()) return errorResult(QStringLiteral("見つかりません: ") + missing.join(QStringLiteral(", ")));
  // 再生位置が終わりより後ろになったら戻す
  if (editor().currentTime() > editor().contentEnd()) editor().seek(editor().contentEnd());
  ToolResult r = okResult({{QStringLiteral("deleted"), int(removed.size())}});
  if (!missing.isEmpty()) r.content.append(QJsonObject{{QStringLiteral("text"), QStringLiteral("見つからなかった ID: ") + missing.join(QStringLiteral(", "))}});
  r.log << QStringLiteral("削除: %1").arg(removed.join(QStringLiteral("、")));
  return r;
}

ToolResult addMedia(const QJsonObject& in, const QString& merge) {
  const QString assetId = string(in, "asset_id");
  const Asset* a = editor().asset(assetId);
  if (!a) return errorResult(QStringLiteral("素材 %1 が見つかりません").arg(assetId));
  // 素材は id で示す（名前はプロジェクトのファイルから読んだものなので、文の中に入れるとアプリからの知らせに見えてしまう）
  if (a->status == AssetStatus::Error || a->status == AssetStatus::Missing) {
    return errorResult(QStringLiteral("素材 %1 は読み込めません（ファイルが見つからないか、読み込めない形式です）").arg(assetId));
  }
  const Asset asset = *a;
  QJsonObject added;
  QString log;
  editor().commit([&](ProjectData& p) {
    if (asset.kind == AssetKind::Audio) {
      AudioItem item;
      item.id = newId();
      item.assetId = asset.id;
      item.start = std::max(0.0, number(in, "start").value_or(0.0));
      item.in = 0;
      item.out = asset.duration;
      item.volume = kMaxVolume;
      p.audios.push_back(item);
      added = audioJson(item);
      log = QStringLiteral("オーディオを追加: %1（%2 から）").arg(quote(asset.name), formatTime(item.start));
      return true;
    }
    // メディア一覧からタイムラインへ追加するときと同じクリップを作る
    VideoClip c;
    c.id = newId();
    c.assetId = asset.id;
    c.isImage = asset.kind == AssetKind::Image;
    c.in = 0;
    c.out = c.isImage ? kDefaultImageDuration : std::max(kMinClipDuration, asset.duration);
    c.fit = defaultFit(asset, p.settings.aspect);
    const int index = std::clamp(in.value(QStringLiteral("index")).toInt(int(p.clips.size())), 0, int(p.clips.size()));
    p.clips.insert(p.clips.begin() + index, c);
    added = {{QStringLiteral("id"), c.id}, {QStringLiteral("index"), index}};
    log = QStringLiteral("クリップを追加: %1（%2 番目）").arg(quote(asset.name)).arg(index + 1);
    return true;
  }, merge);
  ToolResult r = okResult({{QStringLiteral("added"), added}});
  r.log << log;
  return r;
}

ToolResult updateAudio(const QJsonObject& in, const QString& merge) {
  const QString id = string(in, "id");
  const AudioItem* cur = editor().audio(id);
  if (!cur) return errorResult(QStringLiteral("オーディオ %1 が見つかりません").arg(id));
  // commit は編集データを丸ごと置き換えるので、cur はその後は使えない。必要な値は先に写しておく
  const QString assetId = cur->assetId;
  const Asset* asset = editor().asset(assetId);
  const double assetDuration = asset ? asset->duration : cur->out;
  cur = nullptr;
  const double total = editor().layout().total;
  QStringList changed;
  editor().commit([&](ProjectData& p) {
    AudioItem* a = findById(p.audios, id);
    if (!a) return false;
    if (const auto v = number(in, "volume")) {
      a->volumePoints.clear();
      a->volume = std::clamp(*v / 100.0, 0.0, kMaxVolume);
      changed << QStringLiteral("音量 %1%").arg(std::lround(a->volume * 100));
    }
    if (const auto s = number(in, "start")) {
      a->start = frameRound(std::max(0.0, *s), p.settings.fps);
      changed << QStringLiteral("開始 %1").arg(formatTime(a->start));
    }
    if (const auto f = number(in, "fade_in")) {
      a->fadeIn = std::clamp(*f, 0.0, 10.0);
      changed << QStringLiteral("フェードイン %1 秒").arg(a->fadeIn);
    }
    if (const auto f = number(in, "fade_out")) {
      a->fadeOut = std::clamp(*f, 0.0, 10.0);
      changed << QStringLiteral("フェードアウト %1 秒").arg(a->fadeOut);
    }
    if (in.value(QStringLiteral("fit_to_video")).toBool()) {
      if (total <= a->start) {
        changed << QStringLiteral("（動画より後ろにあるので長さは変えていません）");
      } else {
        // 右側の設定の「動画の長さに合わせる」と同じ
        a->out = std::min(assetDuration, a->in + (total - a->start));
        if (a->fadeOut <= 0) a->fadeOut = 2;
        changed << QStringLiteral("動画の長さに合わせた");
      }
    }
    return !changed.isEmpty();
  }, merge);
  if (changed.isEmpty()) return errorResult(QStringLiteral("変える項目がありません"));
  const AudioItem* now = editor().audio(id);
  ToolResult r = okResult({{QStringLiteral("audio"), now ? audioJson(*now) : QJsonObject()}});
  r.log << QStringLiteral("%1を変更: %2").arg(quote(assetName(assetId)), changed.join(QStringLiteral("・")));
  return r;
}

ToolResult setProject(const QJsonObject& in, const QString& merge) {
  const QString aspectText = string(in, "aspect");
  const int fps = in.value(QStringLiteral("fps")).toInt(0);
  QStringList changed;
  editor().commit([&](ProjectData& p) {
    if (!aspectText.isEmpty()) {
      const Aspect aspect = aspectFromString(aspectText);
      if (aspect != p.settings.aspect) {
        // 比率を変えるときと同じく、自動で決めた表示方法のままのクリップは選び直す
        for (auto& c : p.clips) {
          const Asset* a = editor().asset(c.assetId);
          if (!a || c.fit != defaultFit(*a, p.settings.aspect)) continue;
          c.fit = defaultFit(*a, aspect);
        }
        p.settings.aspect = aspect;
        changed << QStringLiteral("比率 %1").arg(aspectToString(aspect));
      }
    }
    if ((fps == 24 || fps == 30 || fps == 60) && fps != p.settings.fps) {
      p.settings.fps = fps;
      changed << QStringLiteral("フレームレート %1fps").arg(fps);
    }
    return !changed.isEmpty();
  }, merge);
  if (changed.isEmpty()) return okResult({{QStringLiteral("note"), QStringLiteral("変更はありません")}});
  ToolResult r = okResult();
  r.log << QStringLiteral("プロジェクトを変更: %1").arg(changed.join(QStringLiteral("・")));
  return r;
}

ToolResult proposeTexts(const QJsonObject& in) {
  ToolResult r;
  r.hasIdeas = true;
  r.ideasKind = string(in, "kind") == u"telop" ? QStringLiteral("telop") : QStringLiteral("title");
  for (const QJsonValue& v : in.value(QStringLiteral("items")).toArray()) {
    const QJsonObject o = v.toObject();
    TextIdea idea;
    idea.text = string(o, "text").trimmed();
    if (idea.text.isEmpty()) continue;
    idea.start = number(o, "start");
    idea.duration = number(o, "duration");
    idea.style = string(o, "style");
    idea.position = string(o, "position");
    r.ideas.push_back(idea);
  }
  if (r.ideas.empty()) return errorResult(QStringLiteral("案がありません"));
  if (r.ideasKind == u"telop") {
    // テロップ案は「すべて追加」で並べて入れるので、時間が重なる案（同じ場面の言い換え）は受け付けない
    std::vector<const TextIdea*> sorted;
    for (const TextIdea& i : r.ideas) {
      if (!i.start) return errorResult(QStringLiteral("テロップ案には、すべて start（表示を始める時間）を付けてください"));
      sorted.push_back(&i);
    }
    std::sort(sorted.begin(), sorted.end(), [](const TextIdea* a, const TextIdea* b) { return *a->start < *b->start; });
    for (size_t k = 1; k < sorted.size(); ++k) {
      const TextIdea& prev = *sorted[k - 1];
      const double prevEnd = *prev.start + prev.duration.value_or(kDefaultTextDuration);
      if (*sorted[k]->start < prevEnd - 0.05) {
        return errorResult(QStringLiteral("テロップ案「%1」と「%2」の時間が重なっています。テロップ案は、場面ごとに 1 つずつ、"
                                          "時間が重ならないように出し直してください（前のテロップの start + duration 以降に次を置く）。"
                                          "1 つの場面にいくつか候補を出したいときは、タイトル案（kind: title）として出してください。")
                               .arg(prev.text, sorted[k]->text));
      }
    }
  }
  r.content.append(textBlock({{QStringLiteral("ok"), true},
                              {QStringLiteral("shown"), int(r.ideas.size())},
                              {QStringLiteral("note"), QStringLiteral("案をボタンで表示しました。ユーザーが選ぶまでタイムラインには追加されません。")}}));
  return r;
}

// ---- 見比べて選ぶ案（propose_choices） --------------------------------------------------

/** 編集する道具を使う（案の中で使えるのはこれだけ）。知らない名前なら nullopt */
std::optional<ToolResult> runEdit(const QString& name, const QJsonObject& input, const QString& merge) {
  if (name == u"add_texts") return addTexts(input, merge);
  if (name == u"update_text") return updateText(input, merge);
  if (name == u"update_clip") return updateClip(input, merge);
  if (name == u"move_clip") return moveClip(input, merge);
  if (name == u"split_clip") return splitClip(input, merge);
  if (name == u"delete_items") return deleteItems(input, merge);
  if (name == u"add_media") return addMedia(input, merge);
  if (name == u"update_audio") return updateAudio(input, merge);
  if (name == u"set_project") return setProject(input, merge);
  return std::nullopt;
}

/** 案を、今の編集場所の写しで試した結果 */
struct Trial {
  ProjectData project;
  QStringList log;
  /** 当てはめられなかった理由（空なら当てはめられた） */
  QString error;
  /** この案のせいで、動画の外に出たテキストの注意 */
  QStringList notes;
  double videoLength = 0;
  /** この案で足したり変えたりしたテキスト・クリップの ID（並びは編集データの順） */
  QStringList texts;
  QStringList clips;
};

/** before と比べて、after で足したり変えたりしたものの ID（編集データの JSON の、ID つきの並びどうしで比べる） */
QStringList changedIds(const QJsonArray& before, const QJsonArray& after) {
  std::map<QString, QJsonObject> old;
  for (const QJsonValue& v : before) {
    const QJsonObject o = v.toObject();
    old[o.value(QStringLiteral("id")).toString()] = o;
  }
  QStringList ids;
  for (const QJsonValue& v : after) {
    const QJsonObject o = v.toObject();
    const QString id = o.value(QStringLiteral("id")).toString();
    const auto it = old.find(id);
    if (it == old.end() || it->second != o) ids << id;
  }
  return ids;
}

/** Editor::contentEnd と同じ（テキスト・オーディオが動画より後ろまであれば、そこまで） */
double contentEndOf(const ProjectData& p) {
  double end = layoutClips(p.clips, p.settings.fps).total;
  for (const auto& t : p.texts) end = std::max(end, t.start + t.duration);
  for (const auto& a : p.audios) end = std::max(end, a.start + audioDuration(a));
  return end;
}

/**
 * 案の編集 actions を、写しで上から順に試す。start を渡すと、その状態から試す（別の案に切り替えるとき）。
 * 今の編集場所は何も変わらない
 */
Trial tryActions(const QJsonArray& actions, const ProjectData* start = nullptr) {
  Trial tr;
  const ProjectData base = start ? *start : editor().project();
  tr.project = editor().tryChanges([&] {
    // 試すだけなので、写しの元に戻す履歴は捨てる。merge をそろえて、写しの履歴も増やさない
    const QString merge = QStringLiteral("choice-trial-") + newId();
    if (start) editor().commit([&](ProjectData& p) {
      p = base;
      return true;
    }, merge);
    int step = 0;
    for (const QJsonValue& v : actions) {
      ++step;
      const QJsonObject a = v.toObject();
      // 道具の名前は tool（name と書くモデルもある）。入力を JSON の文字列で書くモデルもある
      const QString tool = a.contains(QStringLiteral("tool")) ? string(a, "tool") : string(a, "name");
      QJsonObject input = a.value(QStringLiteral("input")).toObject();
      if (a.value(QStringLiteral("input")).isString()) input = QJsonDocument::fromJson(string(a, "input").toUtf8()).object();
      const std::optional<ToolResult> r = runEdit(tool, input, merge);
      if (!r) {
        tr.error = QStringLiteral("%1 つ目の編集: 案の中では「%2」は使えません").arg(step).arg(tool);
        return;
      }
      if (r->error) {
        tr.error = QStringLiteral("%1 つ目の編集（%2）: %3").arg(step).arg(tool, r->message);
        return;
      }
      tr.log += r->log;
    }
  });
  if (!tr.error.isEmpty()) return tr;
  const QJsonObject before = projectToJson(base);
  const QJsonObject after = projectToJson(tr.project);
  tr.texts = changedIds(before.value(QStringLiteral("texts")).toArray(), after.value(QStringLiteral("texts")).toArray());
  tr.clips = changedIds(before.value(QStringLiteral("clips")).toArray(), after.value(QStringLiteral("clips")).toArray());
  tr.videoLength = layoutClips(tr.project.clips, tr.project.settings.fps).total;
  const double baseLength = layoutClips(base.clips, base.settings.fps).total;
  for (const TextItem& t : tr.project.texts) {
    const QString note = timingNote(t, tr.videoLength);
    if (note.isEmpty()) continue;
    // もとから動画の外にあって、この案で変えていないテキストは、この案のせいではないので知らせない
    if (!tr.texts.contains(t.id)) {
      const auto old = std::find_if(base.texts.begin(), base.texts.end(), [&](const TextItem& b) { return b.id == t.id; });
      if (old != base.texts.end() && !timingNote(*old, baseLength).isEmpty()) continue;
    }
    tr.notes << note;
  }
  return tr;
}

/** 案が変えたところを見せる時間（テキストは出きったところ、クリップは真ん中）。大事な順 */
std::vector<double> focusTimes(const Trial& tr) {
  std::vector<double> out;
  for (const QString& id : tr.texts) {
    for (const TextItem& t : tr.project.texts) {
      if (t.id == id) out.push_back(t.start + std::min(t.duration / 2, 1.0));
    }
  }
  if (!tr.clips.isEmpty()) {
    const TimelineLayout layout = layoutClips(tr.project.clips, tr.project.settings.fps);
    for (const QString& id : tr.clips) {
      for (const ClipLayout& l : layout.items) {
        if (l.clip.id == id) out.push_back((l.start + l.end) / 2);
      }
    }
  }
  return out;
}

/** プレビューの時間（どの案も同じ時間を見せる。2 つまで）。asked は AI が指定した時間 */
std::vector<double> pickTimes(const std::vector<Trial>& trials, const QJsonArray& asked) {
  double end = 0;
  for (const Trial& tr : trials) end = std::max(end, contentEndOf(tr.project));
  std::vector<double> candidates;
  for (const QJsonValue& v : asked) {
    if (v.isDouble() && std::isfinite(v.toDouble())) candidates.push_back(v.toDouble());
  }
  if (candidates.empty()) {
    // それぞれの案のいちばん大事なところを先に。足りなければ、その次に大事なところ
    std::vector<std::vector<double>> each;
    for (const Trial& tr : trials) each.push_back(focusTimes(tr));
    for (size_t k = 0; k < 3; ++k) {
      for (const auto& f : each) {
        if (k < f.size()) candidates.push_back(f[k]);
      }
    }
  }
  std::vector<double> out;
  for (double t : candidates) {
    t = std::clamp(t, 0.0, std::max(0.0, end - 0.05));
    if (std::none_of(out.begin(), out.end(), [t](double o) { return std::abs(o - t) < 0.5; })) out.push_back(t);
    if (out.size() == 2) break;
  }
  if (out.empty()) {
    const double now = editor().currentTime();
    out.push_back(now > 0 && now < end ? now : std::min(1.0, end / 2));
  }
  std::sort(out.begin(), out.end());
  return out;
}

/** AI に見せる大きさに縮めた JPEG */
QByteArray shrinkJpeg(const QByteArray& jpeg, int longSide) {
  QImage img = QImage::fromData(jpeg, "JPG");
  if (img.isNull() || std::max(img.width(), img.height()) <= longSide) return jpeg;
  img = img.scaled(longSide, longSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QByteArray out;
  QBuffer buf(&out);
  buf.open(QIODevice::WriteOnly);
  img.save(&buf, "JPG", 80);
  return out;
}

/** 案を試し、当てはめた画面を作ってから done を呼ぶ（画面はユーザーに見せる大きさで作り、AI には縮めて渡す） */
void proposeChoices(const QJsonObject& in, QObject* context, std::function<void(ToolResult)> done) {
  const QJsonArray options = in.value(QStringLiteral("options")).toArray();
  if (options.size() < 2) {
    done(errorResult(QStringLiteral("案は 2 つか 3 つ出してください。1 つしかないときは、propose_choices を使わずにそのまま編集してください。")));
    return;
  }
  struct State {
    ChoiceSet set;
    std::vector<Trial> trials;
    int remaining = 0;
    std::function<void(ToolResult)> done;
  };
  auto st = std::make_shared<State>();
  st->done = std::move(done);
  st->set.id = newId();
  st->set.question = string(in, "question").trimmed();
  QStringList problems;
  for (int i = 0; i < options.size() && i < 3; ++i) {
    const QJsonObject o = options[i].toObject();
    Choice c;
    c.label = string(o, "label").trimmed();
    c.description = string(o, "description").trimmed();
    c.actions = o.value(QStringLiteral("actions")).toArray();
    const QString name = QStringLiteral("案 %1「%2」").arg(choiceLetter(i), c.label);
    if (c.label.isEmpty()) problems << QStringLiteral("案 %1 に label がありません。").arg(choiceLetter(i));
    if (c.actions.isEmpty()) {
      problems << QStringLiteral("%1 に actions がありません。").arg(name);
      continue;
    }
    Trial tr = tryActions(c.actions);
    if (!tr.error.isEmpty()) problems << QStringLiteral("%1 を当てはめられません（%2）。").arg(name, tr.error);
    for (const QString& n : tr.notes) problems << QStringLiteral("%1: %2").arg(name, n);
    c.log = tr.log;
    st->set.choices.push_back(c);
    st->trials.push_back(std::move(tr));
  }
  if (!problems.isEmpty()) {
    st->done(errorResult(problems.join(QLatin1Char(' ')) + QStringLiteral(" 直してから、もう一度 propose_choices を使ってください（ユーザーには、まだ何も見せていません）。")));
    return;
  }
  st->set.times = pickTimes(st->trials, in.value(QStringLiteral("times")).toArray());
  st->remaining = int(st->trials.size());
  auto finish = [st] {
    const ChoiceSet& set = st->set;
    // 見た目が同じ案は、見比べられない
    for (size_t a = 0; a < set.choices.size(); ++a) {
      for (size_t b = a + 1; b < set.choices.size(); ++b) {
        if (set.choices[a].previews != set.choices[b].previews) continue;
        st->done(errorResult(QStringLiteral("案 %1 と案 %2 は、プレビューの画面（%3 秒）がまったく同じです。見た目で違いがわかる案にするか、"
                                            "違いが見える時間を times で指定してください。音など見た目に出ない違いは、propose_choices を使わずに文で聞いてください。")
                                 .arg(choiceLetter(int(a)), choiceLetter(int(b)))
                                 .arg(r2(set.times.front()))));
        return;
      }
    }
    ToolResult r;
    r.hasChoices = true;
    QJsonArray opts;
    for (size_t i = 0; i < set.choices.size(); ++i) {
      const Choice& c = set.choices[i];
      opts.append(QJsonObject{{QStringLiteral("option"), choiceLetter(int(i))},
                              {QStringLiteral("label"), c.label},
                              {QStringLiteral("changes"), QJsonArray::fromStringList(c.log)},
                              {QStringLiteral("video_length"), r2(st->trials[i].videoLength)}});
      for (size_t j = 0; j < c.previews.size() && j < set.times.size(); ++j) {
        if (c.previews[j].isEmpty()) continue;
        r.attachments.append(QJsonObject{{QStringLiteral("text"), QStringLiteral("案 %1「%2」の %3 の画面:").arg(choiceLetter(int(i)), c.label, formatTime(set.times[j]))}});
        r.attachments.append(QJsonObject{
            {QStringLiteral("image"),
             QJsonObject{{QStringLiteral("format"), QStringLiteral("jpeg")},
                         {QStringLiteral("source"), QJsonObject{{QStringLiteral("bytes"), QString::fromLatin1(shrinkJpeg(c.previews[j], 768).toBase64())}}}}}});
      }
    }
    QJsonArray times;
    for (double t : set.times) times.append(r2(t));
    r.content.append(textBlock({{QStringLiteral("ok"), true},
                                {QStringLiteral("shown"), int(set.choices.size())},
                                {QStringLiteral("preview_times"), times},
                                {QStringLiteral("options"), opts},
                                {QStringLiteral("note"), QStringLiteral("案をプレビューつきで表示しました。ユーザーが「これにする」を押すまで、タイムラインは変わっていません。"
                                                                        "選ばれた案は、次のメッセージで知らせます。各案のプレビュー画像を、このメッセージの最後に付けました。"
                                                                        "まず画像を確かめ、足した文字が映像にもともとある文字や人の顔に重なっている、読みにくい、はみ出している、"
                                                                        "案の説明が画像と合っていない（色が違う など）といった問題があれば、"
                                                                        "直した案で propose_choices をもう一度使ってください（今の案と置き換わります）。"
                                                                        "問題がなければ、確かめたことは書かずに、どれにするかを 1 文で聞いて終えてください。")}}));
    r.choiceSet = set;
    st->done(r);
  };
  for (size_t i = 0; i < st->trials.size(); ++i) {
    grabFrames(st->trials[i].project, st->set.times, 1280, context, [st, i, finish](std::vector<FrameShot> shots) {
      for (FrameShot& s : shots) st->set.choices[i].previews.push_back(std::move(s.jpeg));
      if (--st->remaining == 0) finish();
    });
  }
}

}  // namespace

QString choiceLetter(int index) {
  return QString(QChar(char16_t(u'A' + std::clamp(index, 0, 25))));
}

ChoiceApplied applyChoice(const Choice& choice, const QString& merge, const ProjectData* start) {
  ChoiceApplied out;
  Trial tr = tryActions(choice.actions, start);
  if (!tr.error.isEmpty()) {
    out.error = tr.error;
    return out;
  }
  const ProjectData result = std::move(tr.project);
  // 試した結果をそのまま入れるので、プレビューと同じになる（1 回の操作として記録する）
  editor().commit([&](ProjectData& p) {
    p = result;
    return true;
  }, merge);
  out.ok = true;
  out.log = tr.log;
  if (!tr.texts.isEmpty()) out.focusText = tr.texts.front();
  if (!tr.clips.isEmpty()) out.focusClip = tr.clips.front();
  return out;
}

void runTool(const QString& name, const QJsonObject& input, const QString& merge, QObject* context, std::function<void(ToolResult)> done) {
  if (name == u"view_frames") {
    std::vector<double> times;
    for (const QJsonValue& v : input.value(QStringLiteral("times")).toArray()) {
      if (v.isDouble() && std::isfinite(v.toDouble())) times.push_back(v.toDouble());
    }
    if (times.empty()) {
      done(errorResult(QStringLiteral("times が必要です")));
      return;
    }
    if (times.size() > 6) times.resize(6);
    grabFrames(times, 768, context, [done](std::vector<FrameShot> shots) {
      ToolResult r;
      QStringList shown;
      for (const FrameShot& s : shots) {
        if (s.jpeg.isEmpty()) continue;
        // 画像は toolResult の中ではなく、メッセージの後ろに付ける（どのモデルでも受け付けられる形）
        r.attachments.append(QJsonObject{{QStringLiteral("text"), QStringLiteral("%1 の画面:").arg(formatTime(s.t))}});
        r.attachments.append(QJsonObject{{QStringLiteral("image"),
                                          QJsonObject{{QStringLiteral("format"), QStringLiteral("jpeg")},
                                                      {QStringLiteral("source"), QJsonObject{{QStringLiteral("bytes"), QString::fromLatin1(s.jpeg.toBase64())}}}}}});
        shown << formatTime(s.t);
      }
      if (shown.isEmpty()) {
        done(errorResult(QStringLiteral("画面を作れませんでした")));
        return;
      }
      r.content.append(textBlock({{QStringLiteral("ok"), true},
                                  {QStringLiteral("note"), QStringLiteral("画面の画像 %1 枚（%2）を、このメッセージの最後に付けました。").arg(shown.size()).arg(shown.join(QStringLiteral("、")))}}));
      done(r);
    });
    return;
  }
  if (name == u"propose_choices") {
    proposeChoices(input, context, std::move(done));
    return;
  }
  ToolResult r;
  if (name == u"get_project") r = okResult({{QStringLiteral("project"), describeProject()}});
  else if (std::optional<ToolResult> edited = runEdit(name, input, merge)) r = std::move(*edited);
  else if (name == u"propose_texts") r = proposeTexts(input);
  else if (name == u"seek") {
    const auto t = number(input, "time");
    if (!t) r = errorResult(QStringLiteral("time が必要です"));
    else {
      editor().pause();
      editor().seek(*t);
      r = okResult({{QStringLiteral("playhead"), r2(editor().currentTime())}});
    }
  } else {
    r = errorResult(QStringLiteral("知らない道具です: ") + name);
  }
  done(r);
}

QStringList addIdeas(const std::vector<TextIdea>& ideas, const QString& kind) {
  QStringList ids;
  if (ideas.empty()) return ids;
  const bool title = kind != u"telop";
  editor().commit([&](ProjectData& p) {
    for (const TextIdea& idea : ideas) {
      // スタイルを決めていない案は、タイトルなら「タイトル」、テロップなら「シンプル」
      const QString style = idea.style.isEmpty() ? (title ? QStringLiteral("title") : QStringLiteral("basic")) : idea.style;
      const TextItem t = buildText(p, idea.text, idea.start.value_or(title ? 0.0 : editor().currentTime()), idea.duration, style, idea.position);
      p.texts.push_back(t);
      ids << t.id;
    }
    return true;
  });
  return ids;
}

}  // namespace kv::ai
