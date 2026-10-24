// ワークショップに公開するテキストの見た目（アプリの textToJson の形）を確かめて、知っている項目だけで作り直す。
// 値の範囲はアプリの読み込み（projectio.cpp の textFromJson・timeline.cpp の propLimits）と同じにして、外れた値は範囲に収める。
// 種類の違う値（数のところに文字など）や、知らない選択肢は受け付けない（アプリは、いつも正しい形で送る）。
// 文字そのもの・置いた時間や段・グループ・一部だけのフォントは、受け取っても保存しない。
import { ApiError } from './http.ts';

export const WORKSHOP_LIMITS = {
  /** テンプレートの名前（アプリの TextTemplates::cleanName と同じ） */
  name: 30,
  description: 200,
  fontFamily: 100,
  /** 1 つの項目のキーフレームの数 */
  keyframesPerProp: 100,
  /** 1 人が公開できる数 */
  perUser: 100,
  /** 報告の理由 */
  reason: 200,
  /** オーディオ 1 つの大きさの上限（バイト） */
  audioBytes: 20 * 1024 * 1024,
  /** オーディオの小ささの下限（バイト。音として短すぎるもの・中身の無いものを断る） */
  audioMinBytes: 1024,
  /** 1 人が公開できるオーディオの大きさの合計（バイト） */
  audioBytesPerUser: 200 * 1024 * 1024,
  /** オーディオの長さの上限（秒） */
  audioSeconds: 30 * 60,
} as const;

/** 受け付けるオーディオの形式（ファイルの中身の先頭で見分ける）と、返すときの Content-Type */
export const AUDIO_TYPES = {
  mp3: 'audio/mpeg',
  wav: 'audio/wav',
  ogg: 'audio/ogg',
  opus: 'audio/ogg',
  flac: 'audio/flac',
  m4a: 'audio/mp4',
  aac: 'audio/aac',
  wma: 'audio/x-ms-wma',
} as const;
export type AudioType = keyof typeof AUDIO_TYPES;

function startsWith(buf: Buffer, bytes: number[], at = 0): boolean {
  return buf.length >= at + bytes.length && bytes.every((b, i) => buf[at + i] === b);
}

/**
 * ファイルの中身の先頭から、オーディオの形式を見分ける（拡張子だけでは信じない）。分からなければ null。
 * ext は、元のファイルの拡張子（Ogg の中が Opus か Vorbis か、ID3 の後ろが MP3 か AAC かの手がかりに使う）
 */
export function detectAudio(buf: Buffer, ext: string): AudioType | null {
  const e = ext.toLowerCase();
  if (startsWith(buf, [0x52, 0x49, 0x46, 0x46]) && startsWith(buf, [0x57, 0x41, 0x56, 0x45], 8)) return 'wav'; // RIFF....WAVE
  if (startsWith(buf, [0x4f, 0x67, 0x67, 0x53])) return e === 'opus' ? 'opus' : 'ogg'; // OggS
  if (startsWith(buf, [0x66, 0x4c, 0x61, 0x43])) return 'flac'; // fLaC
  if (startsWith(buf, [0x66, 0x74, 0x79, 0x70], 4)) return 'm4a'; // ....ftyp（MP4 の入れ物）
  if (startsWith(buf, [0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11])) return 'wma'; // ASF
  // ID3 のタグ（MP3 か、まれに AAC）。ADTS（AAC）か MPEG のフレームの始まり
  if (startsWith(buf, [0x49, 0x44, 0x33])) return e === 'aac' ? 'aac' : 'mp3';
  if (buf.length >= 2 && buf[0] === 0xff && (buf[1]! & 0xf6) === 0xf0) return 'aac';
  if (buf.length >= 2 && buf[0] === 0xff && (buf[1]! & 0xe0) === 0xe0) return 'mp3';
  return null;
}

const FONTS = ['gothic', 'maru', 'mincho', 'ud', 'kyokasho', 'impact', 'meiryo'];
const ALIGNS = ['left', 'center', 'right'];
const ANIMATIONS = ['none', 'fade', 'slideUp'];
const EASINGS = ['linear', 'easeIn', 'easeOut', 'easeInOut', 'hold'];
/** キーフレームで動かせる項目と、その範囲（アプリの propLimits と同じ） */
const PROPS: Record<string, [number, number]> = {
  x: [-2, 3],
  y: [-2, 3],
  scale: [0.05, 4],
  rotation: [-3600, 3600],
  opacity: [0, 1],
  letterSpacing: [-50, 100],
};

type Json = Record<string, unknown>;

function invalid(message: string): never {
  throw new ApiError(400, 'invalid', `テンプレートの見た目が正しくありません（${message}）`);
}

function isObject(v: unknown): v is Json {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

/** 数（無ければ fallback）。範囲に収める */
function num(o: Json, key: string, min: number, max: number, fallback: number): number {
  const v = o[key];
  if (v === undefined) return fallback;
  if (typeof v !== 'number' || !Number.isFinite(v)) invalid(`${key} は数にしてください`);
  return Math.min(max, Math.max(min, v));
}

function bool(o: Json, key: string, fallback: boolean): boolean {
  const v = o[key];
  if (v === undefined) return fallback;
  if (typeof v !== 'boolean') invalid(`${key} は true か false にしてください`);
  return v;
}

function oneOf(o: Json, key: string, list: string[], fallback: string): string {
  const v = o[key];
  if (v === undefined) return fallback;
  if (typeof v !== 'string' || !list.includes(v)) invalid(`${key} が正しくありません`);
  return v;
}

function color(o: Json, key: string, fallback: string): string {
  const v = o[key];
  if (v === undefined) return fallback;
  if (typeof v !== 'string' || !/^#[0-9a-fA-F]{6}$/.test(v)) invalid(`${key} は #RRGGBB の色にしてください`);
  return v.toLowerCase();
}

/** 1 行の文字に整える（制御文字を空白にして、空白をまとめる） */
export function oneLine(v: string): string {
  return v.replace(/[\u0000-\u001f\u007f]/g, ' ').replace(/\s+/g, ' ').trim();
}

/** アプリから届いた見た目を確かめ、知っている項目だけの形にして返す（だめなら 400） */
export function sanitizeStyle(v: unknown): Json {
  if (!isObject(v)) invalid('見た目の項目がありません');
  const out: Json = {
    font: oneOf(v, 'font', FONTS, 'gothic'),
    size: num(v, 'size', 0.01, 0.5, 0.075),
    bold: bool(v, 'bold', true),
    color: color(v, 'color', '#ffffff'),
    strokeColor: color(v, 'strokeColor', '#000000'),
    strokeWidth: num(v, 'strokeWidth', 0, 0.5, 0.12),
    boxColor: color(v, 'boxColor', '#000000'),
    boxOpacity: num(v, 'boxOpacity', 0, 1, 0),
    shadow: bool(v, 'shadow', true),
    align: oneOf(v, 'align', ALIGNS, 'center'),
    letterSpacing: num(v, 'letterSpacing', -50, 100, 0),
    animation: oneOf(v, 'animation', ANIMATIONS, 'fade'),
    scale: num(v, 'scale', 0.05, 4, 1),
    rotation: num(v, 'rotation', -3600, 3600, 0),
    opacity: num(v, 'opacity', 0, 1, 1),
    // 追加したときの位置と長さ（テンプレートは、保存したときの位置・長さで入る）
    x: num(v, 'x', -2, 3, 0.5),
    y: num(v, 'y', -2, 3, 0.82),
    duration: num(v, 'duration', 0.3, 600, 3),
  };
  if (v.fontFamily !== undefined) {
    if (typeof v.fontFamily !== 'string') invalid('fontFamily は文字にしてください');
    const family = oneLine(v.fontFamily);
    if (family.length > WORKSHOP_LIMITS.fontFamily) invalid(`fontFamily は ${WORKSHOP_LIMITS.fontFamily} 文字までにしてください`);
    if (family) out.fontFamily = family;
  }
  if (v.anim !== undefined) {
    if (!isObject(v.anim)) invalid('anim が正しくありません');
    const anim: Json = {};
    for (const [prop, keys] of Object.entries(v.anim)) {
      const range = PROPS[prop];
      if (!range) invalid(`anim の ${prop.slice(0, 20)} は動かせない項目です`);
      if (!Array.isArray(keys)) invalid(`anim の ${prop} が正しくありません`);
      if (keys.length > WORKSHOP_LIMITS.keyframesPerProp) invalid(`anim の ${prop} のキーフレームは ${WORKSHOP_LIMITS.keyframesPerProp} 個までにしてください`);
      const list = keys.map((k) => {
        if (!isObject(k) || k.t === undefined || k.value === undefined) invalid(`anim の ${prop} のキーフレームが正しくありません`);
        return {
          t: num(k, 't', 0, 3600, 0),
          value: num(k, 'value', range[0], range[1], range[0]),
          easing: oneOf(k, 'easing', EASINGS, 'linear'),
        };
      });
      if (list.length) anim[prop] = list.sort((a, b) => a.t - b.t);
    }
    if (Object.keys(anim).length) out.anim = anim;
  }
  return out;
}
