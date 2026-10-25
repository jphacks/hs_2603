#pragma once
// プロジェクトファイル (.kvproj) の読み書き（JSON 形式）。

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>
#include <vector>

#include "core/model.h"

namespace kv {

/** 編集データを、プロジェクトファイルと同じ形の JSON にする（2 つの状態が同じかを比べるのにも使う） */
QJsonObject projectToJson(const ProjectData& p);
/** テキスト 1 つ（プロジェクトファイルと同じ形。テキストのテンプレートの保存にも使う） */
QJsonObject textToJson(const TextItem& t);
/** hasTrack には、段（track）が書いてあったかを返す */
TextItem textFromJson(const QJsonObject& o, bool* hasTrack = nullptr);
/** キーフレームアニメーションを o の "anim" に書く（キーフレームが無ければ書かない）/ o の "anim" から読む（範囲・並びをそろえる） */
void animToJson(QJsonObject& o, const Animation& a);
Animation animFromJson(const QJsonObject& o);

/** 今の編集場所 1 つを、プロジェクトファイル (.kvproj) の中身にする */
QByteArray serializeProject();
/** 全部の編集場所（タブ）を、自動保存用の中身にする */
QByteArray serializeSession();
/** プロジェクトファイルも自動保存も読める。編集場所は 1 つ以上になる */
std::optional<ProjectFile> parseProject(const QByteArray& json, QString* error = nullptr);
bool hasMeaningfulContent(const ProjectData& p);
bool hasMeaningfulContent(const ProjectFile& f);

QString autosavePath();
std::optional<ProjectFile> readAutosave();
void writeAutosave();
void clearAutosave();

}  // namespace kv
