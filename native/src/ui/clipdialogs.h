#pragma once
// 映像まわりの小さな操作と画面: 速度・デュレーション、属性のペースト、カラーマット、フレームの書き出し、フレームの保持

#include <QString>

class QWidget;

namespace kv {

/** 映像 clipId（メイン・重ねるトラック）の速度・デュレーション（長さ）を変える画面。OK で確定（元に戻す 1 回分） */
void execSpeedDialog(QWidget* parent, const QString& clipId);
/** 写しておいた属性のうち、どれを貼り付けるかを選ぶ画面。OK で、選んでいる映像（一緒に選んだものも）に貼り付ける */
void execPasteAttributesDialog(QWidget* parent);
/** 色を選んで、単色の画像（カラーマット）を作り、メディアの一覧と、メインのビデオの再生位置に近い切れ目に入れる */
void execColorMatteDialog(QWidget* parent);
/** 再生位置の画面を、静止画（PNG / JPEG）に書き出す（保存先を聞く。書き出す動画と同じ合成） */
void exportFrameAtPlayhead(QWidget* parent);
/** フレームの保持: 再生位置で、メインのビデオの動画を、その時間の静止画で 2 秒止める（静止画の写真のクリップを間に入れる） */
void frameHoldAtPlayhead(QWidget* parent);
/** 選んでいる映像の音声を、オーディオとして切り離す（できなければ理由を知らせる） */
void detachSelectedAudio();
/**
 * 映像 clipId を逆再生にする / やめる（音も逆になる）。逆向きの動画を作る間は、進み具合を出す（やめられる）。
 * 作った動画はメディアの一覧に入り、同じ素材なら次からは作り直さない
 */
void toggleReverseClip(QWidget* parent, const QString& clipId);
/** 選んでいる映像の設定（色・エフェクト・動きなど）を、属性のペースト用に写しておく（結果を知らせる） */
void copySelectedAttributes();
/** 字幕ファイル（SRT・WebVTT）を選んで、「字幕」のスタイルのテキストとしてタイムラインに並べる */
void importSubtitlesFile(QWidget* parent);
/** タイムラインのテキストを、字幕ファイル（SRT・WebVTT）に書き出す（時間の順） */
void exportSubtitlesFile(QWidget* parent);
/**
 * オブジェクトマスク: プレビューで被写体を選ぶ状態にする / やめる。clipId が空なら、選んでいる映像（無ければ再生位置のメインの映像）。
 * 映像が今の再生位置に出ていなければ、出ている所へ動かす。AI を使えないときは理由を知らせる
 */
void toggleObjectMaskPick(const QString& clipId = QString());

}  // namespace kv
