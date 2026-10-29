#pragma once
// AI 機能の設定。AI は、いつも AI サーバー（リポジトリの server フォルダー）を通して使う（アプリに Bedrock のキーは要らない）。
// AI サーバーのアドレスは、次の順に決める（画面には出さない。どれも無ければ、AI アシスタントは出さない）。
//   1. .env の E_EDITOR_AI_SERVER（管理者が、別のサーバーで試すとき）。.env は、アプリの exe のフォルダー → リポジトリの一番上（開発中だけ）→
//      アプリのデータフォルダー（%LOCALAPPDATA%\JPHACKS\e-editor）の順に探し、値が書いてある最初の 1 つを使う（環境変数は使わない）
//   2. 配る exe に組み込んだアドレス（build.ps1 が native\release.env から、そのままの文字では見えない形で埋め込む）
// .env を書き換えたら、アプリに戻ってくるだけで読み直す（起動し直さなくてよい）。
// 考える深さは AI の欄で選び、データフォルダーの settings.ini に覚えておく（レジストリは使わない）。

#include <QString>
#include <QStringList>

namespace kv::ai {

/** 1 回の依頼で使う設定（送る直前に作る） */
struct Config {
  /** AI サーバーのアドレス（例: https://ai.example.com。末尾の / は無し。決まっていなければ空） */
  QString server;
  /** サーバーで使っているモデル（深さを変えられるかの判断に使う。ログインすると分かる） */
  QString model;
  /** この依頼の考える深さ（low / medium / high / max。空ならモデルの標準のまま）。サーバーが、その人の上限まで下げる */
  QString effort;
  /** ログイン中の印と、AI の欄で選んでいる深さ（auto など） */
  QString sessionToken;
  QString effortChoice;
  bool hasServer() const { return !server.isEmpty(); }
  /** このモデルで考える深さを変えられるか（Claude Sonnet 4.6 以降・OpenAI GPT） */
  bool supportsEffort() const;
  /** 返事を待つ時間（ミリ秒）。深く考えるほど長く待つ */
  int timeoutMs() const;
};

/** 今のアドレスで作る（ログイン中の印などは、送る直前に入れる） */
Config loadConfig();

/** AI サーバーのアドレスをどこで決めたか */
enum class ServerSource {
  /** どこにも無い（AI は使わない） */
  None,
  /** .env の E_EDITOR_AI_SERVER */
  EnvFile,
  /** exe に組み込んだアドレス */
  BuiltIn,
};

/**
 * 使う AI サーバーのアドレス（整えたもの）。.env の E_EDITOR_AI_SERVER があればそれ、無ければ exe に組み込んだもの、
 * どちらも無ければ空。呼ぶたびに .env を読み直す
 */
QString configuredServer();
/** configuredServer() のアドレスを、どこで決めたか（呼ぶたびに .env を読み直す） */
ServerSource serverSource();
/** exe にアドレスを組み込んであるか */
bool hasBuiltInServer();
/** E_EDITOR_AI_SERVER を読んだ .env の場所（どの .env にも書いていなければ空） */
QString envFile();
/** .env を探すフォルダー（探す順） */
QStringList envSearchDirs();
/**
 * E_EDITOR_AI_SERVER を書いてもらう .env の場所（案内用）。すでに .env があれば最初に読むもの、
 * 無ければ、リポジトリの中で動かしていればその一番上、そうでなければデータフォルダー
 */
QString suggestedEnvPath();
/**
 * アドレスを整える: 前後の空白と末尾の / を取り、大文字・小文字をそろえる。
 * http:// か https:// が無ければ、家の中のアドレスには http://、それ以外には https:// を付ける
 * （例: 「192.168.1.10:8787」→「http://192.168.1.10:8787」、「AI.example.com」→「https://ai.example.com」）
 */
QString normalizeServer(const QString& text);
/**
 * アドレスの問題（なければ空。画面に出す文なので、アドレスそのものは入れない）。パスワードを読まれないよう、
 * HTTP にできるのは家の中のアドレス（この PC・家のネットワーク・.local・Tailscale の 100.64.0.0/10）だけ。
 * 組み込んだアドレスの問題なら、.env ではなく、アプリを配った人に知らせてもらう文にする
 */
QString serverProblem(const QString& url);
/**
 * AI サーバーではないところにつながったときの文（直してもらう場所は、アドレスをどこで決めたかで変える）
 */
QString wrongServerText();
/** 画面に出す文（Qt の通信の失敗の説明など）から、サーバーのアドレスを伏せる（アドレスは画面に出さない） */
QString hideServer(QString text, const QString& server);

/** AI の欄で選べる考える深さ（auto / low / medium / high / max）。auto は依頼ごとにアプリが決める */
const QStringList& effortChoices();
/** 深さ（low〜max）を、上限 max まで下げる */
QString clampEffort(const QString& level, const QString& max);
/** 前に選んだ考える深さ（覚えていなければ空） */
QString savedEffort();
/** 選んだ考える深さを覚えておく */
void saveEffort(const QString& choice);

}  // namespace kv::ai
