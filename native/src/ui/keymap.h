#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

#include <vector>

/**
 * キーボードショートカットの一覧と、ユーザーが変えた割り当ての保存。
 * 既定のキーから変えた操作だけを settings.ini の [shortcuts] に残す（既定のままの操作は書かない）
 */
namespace kv::keymap {

struct Command {
  QString id;        // 保存に使う名前（"edit.split" など）。変えない
  QString category;  // 一覧での分類（"編集" など）
  QString label;     // 一覧に出す操作の名前
  QList<QKeySequence> defaults;
};

/** 変えられる操作の一覧（一覧に出す順） */
const std::vector<Command>& commands();
const Command* find(const QString& id);

/** いまの割り当て（変えていなければ既定のキー）。どれも1つのキー（Ctrl+K, Ctrl+S のような続けて押すキーは使わない） */
QList<QKeySequence> keys(const QString& id);
/** 割り当てを変えて保存する。既定と同じになったら、保存してある分を消す */
void setKeys(const QString& id, const QList<QKeySequence>& keys);
/** すべて既定のキーに戻す */
void resetAll();
bool isDefault(const QString& id);

/** 比べる・保存するための形にそろえる（1つ目のキーだけにし、テンキーの印を外す） */
QKeySequence normalize(const QKeySequence& key);
/** Ctrl・Alt・Win を使わないキー（Space・S・Shift+← など。F1〜F35 は除く）。文字の入力中には使わない */
bool isPlain(const QKeySequence& key);
/** 一覧に出す文字（"Ctrl+D / Ctrl+Shift+D"。割り当てが無ければ空） */
QString text(const QList<QKeySequence>& keys);

}  // namespace kv::keymap
