#include "ui/keymap.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace kv::keymap {

// AI の設定（考える深さ）と同じ settings.ini に書く
static QString settingsPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("settings.ini"));
}

static QString settingsKey(const QString& id) {
  return QStringLiteral("shortcuts/") + id;
}

static QList<QKeySequence> seqs(std::initializer_list<const char*> keys) {
  QList<QKeySequence> out;
  for (const char* k : keys) out << QKeySequence(QString::fromLatin1(k), QKeySequence::PortableText);
  return out;
}

const std::vector<Command>& commands() {
  // 既定のキーは、変えられるようにする前の割り当てと同じ
  static const std::vector<Command> list = [] {
    const QString file = QStringLiteral("ファイル");
    const QString edit = QStringLiteral("編集");
    const QString play = QStringLiteral("再生・移動");
    const QString range = QStringLiteral("書き出す範囲");
    const QString view = QStringLiteral("表示");
    const QString help = QStringLiteral("ヘルプ");
    const QString marker = QStringLiteral("マーカー");
    // Premiere Pro と同じ既定のキーにしたもの: Q / W（リップルトリム）、Shift+Delete（リップル削除）、Ctrl+Shift+K（すべてのトラックで分割）、
    // M / Shift+M / Ctrl+Shift+M（マーカー）、PgUp / PgDn（編集点）、Alt+← / →（1 フレームずらす）、Ctrl+Shift+D（既定のトランジション）、
    // Ctrl+Alt+V（属性をペースト）、Ctrl+Shift+E（フレームを書き出し）、Ctrl+R（速度・デュレーション）、Ctrl+L（ループ）
    return std::vector<Command>{
        {QStringLiteral("file.new"), file, QStringLiteral("新しい編集場所（タブ）を追加"), seqs({"Ctrl+N"})},
        {QStringLiteral("file.open"), file, QStringLiteral("開く（新しいタブ）"), seqs({"Ctrl+O"})},
        {QStringLiteral("file.close"), file, QStringLiteral("編集場所（タブ）を閉じる"), seqs({"Ctrl+W"})},
        {QStringLiteral("file.save"), file, QStringLiteral("保存"), seqs({"Ctrl+S"})},
        {QStringLiteral("file.saveAs"), file, QStringLiteral("名前を付けて保存"), seqs({"Ctrl+Shift+S"})},
        {QStringLiteral("file.import"), file, QStringLiteral("メディアを追加"), seqs({"Ctrl+I"})},
        {QStringLiteral("file.importFolder"), file, QStringLiteral("フォルダからメディアを追加"), QList<QKeySequence>{}},
        {QStringLiteral("file.export"), file, QStringLiteral("書き出し"), seqs({"Ctrl+E"})},
        {QStringLiteral("file.exportFrame"), file, QStringLiteral("フレームを書き出し（再生位置の静止画）"), seqs({"Ctrl+Shift+E"})},
        {QStringLiteral("file.importSubtitles"), file, QStringLiteral("字幕を読み込む（SRT・WebVTT）"), QList<QKeySequence>{}},
        {QStringLiteral("file.exportSubtitles"), file, QStringLiteral("字幕を書き出す（SRT・WebVTT）"), QList<QKeySequence>{}},
        {QStringLiteral("file.quit"), file, QStringLiteral("終了"), seqs({"Alt+F4"})},
        {QStringLiteral("edit.undo"), edit, QStringLiteral("元に戻す"), seqs({"Ctrl+Z"})},
        {QStringLiteral("edit.redo"), edit, QStringLiteral("やり直し"), seqs({"Ctrl+Y"})},
        {QStringLiteral("edit.split"), edit, QStringLiteral("分割"), seqs({"S"})},
        {QStringLiteral("edit.splitAll"), edit, QStringLiteral("すべてのトラックを再生位置で分割"), seqs({"Ctrl+Shift+K"})},
        {QStringLiteral("edit.rippleTrimStart"), edit, QStringLiteral("再生位置より前を削除して詰める（リップルトリム）"), seqs({"Q"})},
        {QStringLiteral("edit.rippleTrimEnd"), edit, QStringLiteral("再生位置より後ろを削除して詰める（リップルトリム）"), seqs({"W"})},
        {QStringLiteral("edit.duplicate"), edit, QStringLiteral("複製"), seqs({"Ctrl+D"})},
        {QStringLiteral("edit.copy"), edit, QStringLiteral("コピー（ほかのタブにも貼り付けられる）"), seqs({"Ctrl+C"})},
        {QStringLiteral("edit.paste"), edit, QStringLiteral("貼り付け（再生位置に）"), seqs({"Ctrl+V"})},
        {QStringLiteral("edit.delete"), edit, QStringLiteral("削除"), seqs({"Del", "Backspace"})},
        {QStringLiteral("edit.rippleDelete"), edit, QStringLiteral("リップル削除（削除して、空いた所を詰める）"), seqs({"Shift+Del"})},
        {QStringLiteral("edit.nudgeLeft"), edit, QStringLiteral("選んだものを 1 フレーム左へ"), seqs({"Alt+Left"})},
        {QStringLiteral("edit.nudgeRight"), edit, QStringLiteral("選んだものを 1 フレーム右へ"), seqs({"Alt+Right"})},
        {QStringLiteral("edit.addTransition"), edit, QStringLiteral("再生位置に近い切れ目に、既定のトランジションを付ける"), seqs({"Ctrl+Shift+D"})},
        {QStringLiteral("edit.copyAttributes"), edit, QStringLiteral("属性をコピー（色・エフェクト・動きなど）"), seqs({"Ctrl+Alt+C"})},
        {QStringLiteral("edit.pasteAttributes"), edit, QStringLiteral("属性をペースト"), seqs({"Ctrl+Alt+V"})},
        {QStringLiteral("edit.speed"), edit, QStringLiteral("速度・デュレーション"), seqs({"Ctrl+R"})},
        {QStringLiteral("edit.detachAudio"), edit, QStringLiteral("映像の音声を切り離す"), QList<QKeySequence>{}},
        {QStringLiteral("edit.frameHold"), edit, QStringLiteral("フレームを保持（再生位置の静止画を差し込む）"), QList<QKeySequence>{}},
        {QStringLiteral("edit.reverse"), edit, QStringLiteral("逆再生にする / やめる"), QList<QKeySequence>{}},
        {QStringLiteral("edit.objectMask"), edit, QStringLiteral("オブジェクトマスク（AI）: 被写体をクリックして選ぶ / やめる"), QList<QKeySequence>{}},
        {QStringLiteral("edit.colorMatte"), edit, QStringLiteral("カラーマット（単色の画像）を追加"), QList<QKeySequence>{}},
        {QStringLiteral("edit.deselect"), edit, QStringLiteral("選択を解除"), seqs({"Esc"})},
        {QStringLiteral("edit.copyKeys"), edit, QStringLiteral("キーフレームをコピー"), seqs({"Ctrl+Shift+C"})},
        {QStringLiteral("edit.pasteKeys"), edit, QStringLiteral("キーフレームを貼り付け"), seqs({"Ctrl+Shift+V"})},
        {QStringLiteral("edit.textUp"), edit, QStringLiteral("選んだテキスト・重ねる映像を上の段へ"), seqs({"Alt+Up"})},
        {QStringLiteral("edit.textDown"), edit, QStringLiteral("選んだテキスト・重ねる映像を下の段へ"), seqs({"Alt+Down"})},
        {QStringLiteral("edit.group"), edit, QStringLiteral("グループ化（一緒に選んだものをまとめる）"), seqs({"Ctrl+G"})},
        {QStringLiteral("edit.ungroup"), edit, QStringLiteral("グループ解除"), seqs({"Ctrl+Shift+G"})},
        {QStringLiteral("edit.addText"), edit, QStringLiteral("テキストを追加"), seqs({"T"})},
        {QStringLiteral("edit.prevObject"), edit, QStringLiteral("前のオブジェクトを選ぶ"), seqs({"Up"})},
        {QStringLiteral("edit.nextObject"), edit, QStringLiteral("次のオブジェクトを選ぶ"), seqs({"Down"})},
        {QStringLiteral("play.toggle"), play, QStringLiteral("再生 / 一時停止"), seqs({"Space"})},
        {QStringLiteral("play.prevFrame"), play, QStringLiteral("1フレーム戻る"), seqs({"Left"})},
        {QStringLiteral("play.nextFrame"), play, QStringLiteral("1フレーム進む"), seqs({"Right"})},
        {QStringLiteral("play.back1s"), play, QStringLiteral("1秒戻る"), seqs({"Shift+Left"})},
        {QStringLiteral("play.forward1s"), play, QStringLiteral("1秒進む"), seqs({"Shift+Right"})},
        {QStringLiteral("play.start"), play, QStringLiteral("先頭へ移動"), seqs({"Home"})},
        {QStringLiteral("play.end"), play, QStringLiteral("末尾へ移動"), seqs({"End"})},
        {QStringLiteral("play.prevEdit"), play, QStringLiteral("前の編集点へ移動"), seqs({"PgUp"})},
        {QStringLiteral("play.nextEdit"), play, QStringLiteral("次の編集点へ移動"), seqs({"PgDown"})},
        {QStringLiteral("play.loop"), play, QStringLiteral("ループ再生のオン / オフ"), seqs({"Ctrl+L"})},
        {QStringLiteral("marker.add"), marker, QStringLiteral("再生位置にマーカーを追加"), seqs({"M"})},
        {QStringLiteral("marker.next"), marker, QStringLiteral("次のマーカーへ移動"), seqs({"Shift+M"})},
        {QStringLiteral("marker.prev"), marker, QStringLiteral("前のマーカーへ移動"), seqs({"Ctrl+Shift+M"})},
        {QStringLiteral("marker.clear"), marker, QStringLiteral("すべてのマーカーを削除"), QList<QKeySequence>{}},
        {QStringLiteral("range.in"), range, QStringLiteral("書き出す範囲の開始を再生位置に"), seqs({"I"})},
        {QStringLiteral("range.out"), range, QStringLiteral("書き出す範囲の終了を再生位置に"), seqs({"O"})},
        {QStringLiteral("range.clear"), range, QStringLiteral("書き出す範囲を解除"), seqs({"Ctrl+Shift+X"})},
        {QStringLiteral("view.zoomIn"), view, QStringLiteral("タイムラインを拡大"), seqs({"Ctrl+="})},
        {QStringLiteral("view.zoomOut"), view, QStringLiteral("タイムラインを縮小"), seqs({"Ctrl+-"})},
        {QStringLiteral("view.fitAll"), view, QStringLiteral("全体を表示"), seqs({"Ctrl+0"})},
        {QStringLiteral("view.tracksTaller"), view, QStringLiteral("タイムラインの段を高く"), seqs({"Ctrl+Shift+Up"})},
        {QStringLiteral("view.tracksShorter"), view, QStringLiteral("タイムラインの段を低く"), seqs({"Ctrl+Shift+Down"})},
        {QStringLiteral("view.tracksReset"), view, QStringLiteral("タイムラインの段の高さを元に戻す"), QList<QKeySequence>{}},
        {QStringLiteral("view.snap"), view, QStringLiteral("タイムラインの吸着（スナップ）のオン / オフ"), QList<QKeySequence>{}},
        {QStringLiteral("view.safeMargins"), view, QStringLiteral("プレビューにセーフマージンを表示"), QList<QKeySequence>{}},
        {QStringLiteral("view.grid"), view, QStringLiteral("プレビューに三分割のグリッドを表示"), QList<QKeySequence>{}},
        {QStringLiteral("view.fullScreen"), view, QStringLiteral("プレビューを全画面表示"), seqs({"F", "F11"})},
        {QStringLiteral("view.leftPanel"), view, QStringLiteral("左の欄を折りたたむ / 開く"), QList<QKeySequence>{}},
        {QStringLiteral("view.ai"), view, QStringLiteral("AI アシスタントを出す / しまう"), seqs({"Ctrl+Shift+A"})},
        {QStringLiteral("view.nextTab"), view, QStringLiteral("次のタブへ切り替え"), seqs({"Ctrl+PgDown"})},
        {QStringLiteral("view.prevTab"), view, QStringLiteral("前のタブへ切り替え"), seqs({"Ctrl+PgUp"})},
        {QStringLiteral("help.shortcuts"), help, QStringLiteral("キーボードショートカットの一覧"), seqs({"?"})},
    };
  }();
  return list;
}

const Command* find(const QString& id) {
  for (const Command& c : commands()) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

QKeySequence normalize(const QKeySequence& key) {
  if (key.isEmpty()) return {};
  const QKeyCombination c = key[0];
  return QKeySequence(QKeyCombination(c.keyboardModifiers() & ~Qt::KeypadModifier, c.key()));
}

bool isPlain(const QKeySequence& key) {
  if (key.isEmpty()) return false;
  // F1〜F35 は文字を入力しないので、Ctrl・Alt と同じく、文字の入力中でも効くようにする（F11 の全画面表示など）
  const Qt::Key k = key[0].key();
  if (k >= Qt::Key_F1 && k <= Qt::Key_F35) return false;
  return !(key[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
}

/** 重なりと空のキーを除いて、そろえた形にする */
static QList<QKeySequence> cleaned(const QList<QKeySequence>& keys) {
  QList<QKeySequence> out;
  for (const QKeySequence& k : keys) {
    const QKeySequence n = normalize(k);
    if (!n.isEmpty() && !out.contains(n)) out << n;
  }
  return out;
}

QList<QKeySequence> keys(const QString& id) {
  const Command* c = find(id);
  if (!c) return {};
  QSettings s(settingsPath(), QSettings::IniFormat);
  if (!s.contains(settingsKey(id))) return c->defaults;
  // 保存してある分（全部外したときは空）
  QList<QKeySequence> out;
  for (const QString& t : s.value(settingsKey(id)).toStringList()) {
    if (!t.isEmpty()) out << QKeySequence(t, QKeySequence::PortableText);
  }
  return cleaned(out);
}

void setKeys(const QString& id, const QList<QKeySequence>& keys) {
  const Command* c = find(id);
  if (!c) return;
  const QList<QKeySequence> k = cleaned(keys);
  QDir().mkpath(QFileInfo(settingsPath()).absolutePath());
  QSettings s(settingsPath(), QSettings::IniFormat);
  if (k == c->defaults) {
    s.remove(settingsKey(id));
    return;
  }
  QStringList list;
  for (const QKeySequence& q : k) list << q.toString(QKeySequence::PortableText);
  // 全部外したときも、空として残す（既定のキーに戻らないように）
  s.setValue(settingsKey(id), list.isEmpty() ? QVariant(QString()) : QVariant(list));
}

void resetAll() {
  QSettings s(settingsPath(), QSettings::IniFormat);
  s.remove(QStringLiteral("shortcuts"));
}

bool isDefault(const QString& id) {
  const Command* c = find(id);
  return c && keys(id) == c->defaults;
}

static QString keyText(const QKeySequence& key) {
  if (key.isEmpty()) return {};
  const QKeyCombination c = key[0];
  const Qt::KeyboardModifiers m = c.keyboardModifiers();
  QString out;
  if (m & Qt::ControlModifier) out += QStringLiteral("Ctrl+");
  if (m & Qt::AltModifier) out += QStringLiteral("Alt+");
  if (m & Qt::ShiftModifier) out += QStringLiteral("Shift+");
  if (m & Qt::MetaModifier) out += QStringLiteral("Win+");
  switch (c.key()) {
    case Qt::Key_Left: out += QStringLiteral("←"); break;
    case Qt::Key_Right: out += QStringLiteral("→"); break;
    case Qt::Key_Up: out += QStringLiteral("↑"); break;
    case Qt::Key_Down: out += QStringLiteral("↓"); break;
    default: out += QKeySequence(QKeyCombination(c.key())).toString(QKeySequence::NativeText); break;
  }
  return out;
}

QString text(const QList<QKeySequence>& keys) {
  QStringList parts;
  for (const QKeySequence& k : keys) parts << keyText(k);
  return parts.join(QStringLiteral(" / "));
}

}  // namespace kv::keymap
