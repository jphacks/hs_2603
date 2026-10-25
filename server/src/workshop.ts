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
  /** 写真 1 枚の大きさの上限（バイト） */
  imageBytes: 10 * 1024 * 1024,
  /** 写真の大きさの上限（フル HD。横長なら 1920×1080、縦長なら 1080×1920 まで）と下限（ピクセル） */
  imageLong: 1920,
  imageShort: 1080,
  imageMin: 16,
  /** 1 人が公開できる写真の大きさの合計（バイト） */
  imageBytesPerUser: 100 * 1024 * 1024,
  /** 一覧に出す見本（JPEG）の長い辺（ピクセル）と大きさ（バイト）の上限 */
  thumbLong: 480,
  thumbBytes: 200 * 1024,
  /** 動画 1 つの大きさの上限（バイト） */
  videoBytes: 30 * 1024 * 1024,
  /** 動画の長さの上限（秒）。書き出しの丸めで少しはみ出すことがあるので、videoSlack 秒までは受け付ける */
  videoSeconds: 15,
  videoSlack: 0.5,
  /** 1 人が公開できる動画の大きさの合計（バイト） */
  videoBytesPerUser: 300 * 1024 * 1024,
} as const;

/** 受け付ける動画の形式と、返すときの Content-Type（アプリが MP4（H.264・AAC）にしてから送る） */
export const VIDEO_TYPES = {
  mp4: 'video/mp4',
} as const;
export type VideoType = keyof typeof VIDEO_TYPES;

/** MP4 の箱（大きさ・種類・中身の範囲）を、start〜end の中で順に読む。壊れていれば、そこまで */
function mp4Boxes(buf: Buffer, start: number, end: number): { type: string; start: number; end: number }[] {
  const out: { type: string; start: number; end: number }[] = [];
  let i = start;
  while (i + 8 <= end && out.length < 4096) {
    let size = buf.readUInt32BE(i);
    const type = buf.toString('latin1', i + 4, i + 8);
    let header = 8;
    if (size === 1) {
      if (i + 16 > end) break;
      const big = buf.readBigUInt64BE(i + 8);
      if (big > BigInt(end - i)) break;
      size = Number(big);
      header = 16;
    } else if (size === 0) {
      size = end - i;
    }
    if (size < header || i + size > end) break;
    out.push({ type, start: i + header, end: i + size });
    i += size;
  }
  return out;
}

/**
 * MP4 の長さ（秒）と映像の大きさ（ピクセル）を、ファイルの中身から読む（拡張子・アプリの知らせは信じない）。
 * 長さは moov の mvhd、大きさは映像のトラックの tkhd から。MP4 でない・読めなければ null
 */
export function readMp4(buf: Buffer): { type: VideoType; duration: number; width: number; height: number } | null {
  if (!startsWith(buf, [0x66, 0x74, 0x79, 0x70], 4)) return null; // ....ftyp
  const moov = mp4Boxes(buf, 0, buf.length).find((b) => b.type === 'moov');
  if (!moov) return null;
  const inner = mp4Boxes(buf, moov.start, moov.end);
  const mvhd = inner.find((b) => b.type === 'mvhd');
  if (!mvhd) return null;
  const v = buf[mvhd.start];
  let timescale = 0;
  let duration = 0;
  if (v === 1 && mvhd.end - mvhd.start >= 32) {
    timescale = buf.readUInt32BE(mvhd.start + 20);
    duration = Number(buf.readBigUInt64BE(mvhd.start + 24));
  } else if (v === 0 && mvhd.end - mvhd.start >= 20) {
    timescale = buf.readUInt32BE(mvhd.start + 12);
    duration = buf.readUInt32BE(mvhd.start + 16);
  }
  if (!timescale) return null;
  // 映像のトラックの大きさ（tkhd の幅・高さ。16.16 の固定小数点。音だけのトラックは 0）
  let width = 0;
  let height = 0;
  for (const trak of inner.filter((b) => b.type === 'trak')) {
    const tkhd = mp4Boxes(buf, trak.start, trak.end).find((b) => b.type === 'tkhd');
    if (!tkhd) continue;
    const at = tkhd.start + (buf[tkhd.start] === 1 ? 88 : 76);
    if (at + 8 > tkhd.end) continue;
    const w = Math.round(buf.readUInt32BE(at) / 65536);
    const h = Math.round(buf.readUInt32BE(at + 4) / 65536);
    if (w > 0 && h > 0) {
      width = w;
      height = h;
      break;
    }
  }
  return { type: 'mp4', duration: duration / timescale, width, height };
}

/** 受け付ける写真の形式と、返すときの Content-Type */
export const IMAGE_TYPES = {
  jpg: 'image/jpeg',
  png: 'image/png',
} as const;
export type ImageType = keyof typeof IMAGE_TYPES;

/** 写真の形式と大きさ（ピクセル）を、ファイルの中身から読む（JPEG と PNG だけ。拡張子は信じない）。読めなければ null */
export function readImage(buf: Buffer): { type: ImageType; width: number; height: number } | null {
  // PNG: 決まった 8 バイトのあとに IHDR（幅・高さ）
  if (startsWith(buf, [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]) && buf.length >= 24 && buf.toString('latin1', 12, 16) === 'IHDR') {
    return { type: 'png', width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
  }
  // JPEG: FF D8 のあとの区切りを順に見て、SOF（大きさの入った区切り）を探す
  if (startsWith(buf, [0xff, 0xd8])) {
    let i = 2;
    while (i + 4 <= buf.length) {
      if (buf[i] !== 0xff) return null;
      const marker = buf[i + 1]!;
      if (marker === 0xff) {
        i += 1;
        continue;
      }
      // 長さの無い区切り
      if (marker === 0x01 || (marker >= 0xd0 && marker <= 0xd8)) {
        i += 2;
        continue;
      }
      // 画像の始まり・終わりより前に大きさが無ければ、読めない
      if (marker === 0xd9 || marker === 0xda) return null;
      const len = buf.readUInt16BE(i + 2);
      if (len < 2) return null;
      if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) {
        if (i + 9 > buf.length) return null;
        return { type: 'jpg', height: buf.readUInt16BE(i + 5), width: buf.readUInt16BE(i + 7) };
      }
      i += 2 + len;
    }
  }
  return null;
}

/** 写真の大きさが、フル HD（向きはどちらでも）に収まるか */
export function fitsFullHd(width: number, height: number): boolean {
  const long = Math.max(width, height);
  const short = Math.min(width, height);
  return short >= WORKSHOP_LIMITS.imageMin && long <= WORKSHOP_LIMITS.imageLong && short <= WORKSHOP_LIMITS.imageShort;
}

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
