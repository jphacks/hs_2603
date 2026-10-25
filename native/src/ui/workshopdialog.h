#pragma once
// ワークショップの画面: みんなが公開したテキストのテンプレート・オーディオを探して追加する窓（メニューの「ワークショップ」で開く）と、
// 自分のテンプレート・オーディオを公開する画面。通信は ai/workshop.h。

#include <QString>

#include "ai/workshop.h"

class QWidget;

namespace kv {

/** ワークショップの窓を開いて（開いていれば前に出して）、kind の一覧にする。窓は 1 つだけで、編集しながら使える */
void openWorkshop(QWidget* parent, ai::WorkshopKind kind = ai::WorkshopKind::Text);
/** マイテンプレート templateId を、ワークショップに公開する画面を出す（ログインしていなければ、先にログインの画面） */
void publishTemplate(QWidget* parent, const QString& templateId);
/** マイテンプレート templateId の、ワークショップへの公開をやめる（確かめてから） */
void unpublishTemplate(QWidget* parent, const QString& templateId);
/** オーディオの一覧の素材 assetId を、ワークショップに公開する画面を出す */
void publishAudioAsset(QWidget* parent, const QString& assetId);
/** メディアの一覧の写真 assetId を、ワークショップに公開する画面を出す（フル HD より大きければ縮めて公開する） */
void publishImageAsset(QWidget* parent, const QString& assetId);

}  // namespace kv
