#pragma once
// ワークショップ: 自分で作ったテキストのテンプレート（見た目・登場のアニメーション・キーフレーム。文字そのものは送らない）と、
// オーディオ（BGM・効果音など）を AI サーバーで公開し、ほかの人が公開したものを探して、マイテンプレート・オーディオの一覧に追加して使う。
// 追加したオーディオのファイルは %LOCALAPPDATA%\JPHACKS\kantan-video-editor\workshop\audio\<ID>\ に置く（プロジェクトがそのファイルを使う）。
// AI サーバーのアカウントでログインして使う（AI を使えないアカウントでも使える）。使い方（すぐ公開・管理者が確認してから公開・使えない）は、
// AI サーバーの管理画面で決める。通信は ServerCall を通す。

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

#include "core/model.h"

namespace kv::ai {

/** ワークショップに公開できるものの種類 */
enum class WorkshopKind { Text, Audio };

/** ワークショップに公開されたもの 1 つ（テキストのテンプレートか、オーディオ） */
struct WorkshopTemplate {
  QString id;
  WorkshopKind kind = WorkshopKind::Text;
  QString name;
  QString description;
  /** 作った人の名前（アカウントの表示名。メールアドレスは来ない） */
  QString author;
  /** published（公開中）/ pending（管理者の確認待ち）/ hidden（管理者が非公開にした）。自分のもの以外は published だけ */
  QString status;
  /** 使った人の数 */
  int uses = 0;
  /** 自分が公開したもの */
  bool mine = false;
  /** 自分が使ったことがある */
  bool used = false;
  QDateTime createdAt;
  /** テキストのテンプレートの見た目（値の範囲は確かめてある。文字は空） */
  TextItem style;
  /** オーディオのファイルの形式（mp3 など。サーバーが中身から決めたもの）・大きさ（バイト）・長さ（秒） */
  QString fileType;
  qint64 fileSize = 0;
  double duration = 0;
};

/** オーディオとして公開できるファイルの拡張子（サーバーは中身でも確かめる） */
const QStringList& workshopAudioExtensions();
/** 公開できるオーディオ 1 つの大きさの上限（バイト。サーバーと同じ） */
constexpr qint64 kWorkshopAudioMaxBytes = 20LL * 1024 * 1024;
/** 公開できるオーディオの長さの上限（秒。サーバーと同じ） */
constexpr double kWorkshopAudioMaxSeconds = 30 * 60;
/** ワークショップから追加したオーディオを置くフォルダー（%LOCALAPPDATA%\JPHACKS\kantan-video-editor\workshop\audio） */
QString workshopAudioDir();
/** ワークショップのオーディオ id を、もう手元に置いてあれば、そのファイル（無ければ空） */
QString workshopAudioFile(const QString& id);
/** path が、ワークショップから追加したオーディオか（ほかの人が作ったものなので、公開し直せない） */
bool isWorkshopAudio(const QString& path);

/** 一覧の 1 ページ分 */
struct WorkshopPage {
  std::vector<WorkshopTemplate> items;
  /** 条件に合うものの数（全部のページ） */
  int total = 0;
  int offset = 0;
  /** サーバーの使い方（open / approval） */
  QString mode;
};

/** ワークショップの入り口（ボタンやメニュー）を出すか: AI サーバーが決まっていて、そのサーバーで使えなくしていないとき */
bool workshopVisible();
/** 今ワークショップを使えない理由（使えるなら空。画面にそのまま出せる文で、サーバーのアドレスは入れない） */
QString workshopProblem();

class Workshop : public QObject {
  Q_OBJECT
 public:
  static Workshop& instance();

  /** error が空なら成功 */
  using Done = std::function<void(const QString& error)>;

  /**
   * 一覧を読む。kind はテキストのテンプレートかオーディオか、query は名前・説明・作った人で探す文字（空ならすべて）、
   * popular は使った人の多い順（false なら新しい順）、mine は自分が公開したもの（確認待ち・非公開も）。ctx が消えたら done は呼ばない
   */
  void list(WorkshopKind kind, const QString& query, bool popular, bool mine, int offset, QObject* ctx,
            std::function<void(const QString& error, const WorkshopPage& page)> done);
  /** テキストのテンプレートを公開する。message は画面に出す文（管理者の確認待ちになったときは、その説明） */
  void publish(const QString& name, const QString& description, const TextItem& style, QObject* ctx,
               std::function<void(const QString& error, const WorkshopTemplate& published, const QString& message)> done);
  /** オーディオのファイル path を公開する（duration はその長さ。秒） */
  void publishAudio(const QString& name, const QString& description, const QString& path, double duration, QObject* ctx,
                    std::function<void(const QString& error, const WorkshopTemplate& published, const QString& message)> done);
  /**
   * オーディオを手元に置く（もう置いてあれば、読み込まずにそのファイルを返す）。file は workshopAudioDir() の中のファイル
   */
  void fetchAudio(const WorkshopTemplate& t, QObject* ctx, std::function<void(const QString& error, const QString& file)> done);
  /** 自分が公開したものの公開をやめる */
  void remove(const QString& id, QObject* ctx, Done done);
  /** 使ったことを知らせる（使った人の数を増やす。失敗しても知らせない） */
  void markUsed(const QString& id);
  /** 問題のあるテンプレートを、管理者に知らせる */
  void report(const QString& id, const QString& reason, QObject* ctx, Done done);

 signals:
  /** 公開した・公開をやめた（開いている一覧を直すため） */
  void published(const kv::ai::WorkshopTemplate& t);
  void removed(const QString& id);

 private:
  Workshop() = default;
  /**
   * status: HTTP の状態（つながらなければ 0、送れなければ -1）。json の error に、画面に出せる文が入る。
   * raw は、返事が JSON でないとき（オーディオのファイル）の中身
   */
  void call(const QByteArray& method, const QString& path, const QByteArray& body, const QByteArray& contentType, int timeoutMs, QObject* ctx,
            std::function<void(int status, const QJsonObject& json, const QByteArray& raw)> done);
  /** JSON を送って JSON を受け取る */
  void callJson(const QByteArray& method, const QString& path, const QJsonObject& body, QObject* ctx,
                std::function<void(int status, const QJsonObject& json)> done);
};

inline Workshop& workshop() {
  return Workshop::instance();
}

/** サーバーから届いたテンプレートを読む */
WorkshopTemplate parseWorkshopTemplate(const QJsonObject& o);

}  // namespace kv::ai
