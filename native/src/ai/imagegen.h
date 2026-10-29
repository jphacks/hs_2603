#pragma once
// AI で画像を作る（AI サーバーが Bedrock の画像モデルで作る。キーはサーバーだけが持つ）。
// 作った画像は %LOCALAPPDATA%\JPHACKS\e-editor\generated\ に JPEG で置く（プロジェクトがそのファイルを使うので、キャッシュとは別）。

#include <QObject>
#include <QString>
#include <functional>

namespace kv::ai {

/** 作った画像を置くフォルダー */
QString generatedImageDir();
/** path が、AI で作った画像（generatedImageDir の中）か */
bool isGeneratedImage(const QString& path);
/**
 * prompt（作る画像の説明）の画像を、aspect（16:9 など）で作る。成功したら file に、保存した JPEG の場所が入る（error は空）。
 * 1 枚ごとに、月のトークンを使う（量は AI サーバーが決める）。ctx が消えたら done は呼ばない
 */
void generateImage(const QString& prompt, const QString& aspect, QObject* ctx, std::function<void(const QString& error, const QString& file)> done);

}  // namespace kv::ai
