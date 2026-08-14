# AndroidtoMonitor 要件定義

**Status: v1.0 FIXED** (2026-08-14 CodeXレビュー反映・ユーザー承認済み)

## 1. 目的

USB接続した Android 端末を Windows 11 の**拡張ディスプレイ**(ミラーではない)として利用する、最小構成の自作ソフトウェア。市販アプリ(SuperDisplay 等)の代替であり、映像表示のみに機能を絞る。

## 2. ターゲット環境

| 項目 | 内容 |
|---|---|
| PC | Windows 11 Pro (x64)、GPU は NVIDIA (HWエンコード利用) |
| Android | OPPO Reno5 A (ColorOS / Android 11+、2400×1080、90Hz対応) |
| 接続 | USBケーブル (adb 経由 TCP フォワード)。無線は対象外 |
| 画面向き | **縦向き固定 (1080×2400)**。Windows には縦長モニタとして認識させる。回転対応は将来拡張 |
| DPI | Windows 側スケーリングは任意(仮想モニタの解像度は 1080×2400 固定) |

## 3. スコープ

### In scope
- 仮想ディスプレイの追加(Windows が「モニタが1枚増えた」と認識する拡張デスクトップ)
- 仮想ディスプレイの映像を H.264 でエンコードし、USB (adb forward) 経由で Android へ片方向ストリーミング
- Android 側での HW デコード・フルスクリーン表示・画面常時ON
- ケーブル抜き差し/アプリ再起動時の自動再接続。**再接続時はストリームをリセットし、SPS/PPS + IDR から再開する**
- マウスカーソルの表示(PC 側で DDA の pointer shape/position を合成。M2 まで省略可)

### Out of scope
- タッチ/ペン入力(Android→PC の入力全般)
- 音声転送
- 無線(Wi-Fi)接続
- 複数端末・iOS 対応
- 画面回転・解像度の動的変更(初期版は縦向き固定)
- 設定 GUI(設定は設定ファイル or コマンドライン引数)

## 4. アーキテクチャ

```
[Windows]
  仮想ディスプレイドライバ (既製OSSを利用、自作しない)
    → Desktop Duplication API でキャプチャ
    → Media Foundation で H.264 HWエンコード
    → TCP client として localhost:PORT へ送出
        ═══ adb forward tcp:PORT tcp:PORT (USB) ═══
[Android]
  127.0.0.1:PORT で TCP listen
    → MediaCodec HWデコード (Surface decode) → SurfaceView 全画面描画
```

### 4.1 方針

- **仮想ディスプレイドライバは自作しない。** 署名済み OSS (MikeTheTech "Virtual Display Driver" = IddSampleDriver 派生) を利用する。ドライバ署名の壁を回避するため。
- **転送は adb に丸投げ。** USB プロトコルを直接扱わない。adb (platform-tools) は導入手順のみ記載し同梱しない。
- **接続方向**: Android 側アプリが `127.0.0.1:PORT` で待受け、Windows 側が `adb forward tcp:PORT tcp:PORT` を起動時に設定した上で `localhost:PORT` へ接続する。

### 4.2 エンコード要件 (Media Foundation)

- `CODECAPI_AVLowLatencyMode` 有効、B フレーム無効
- IDR 間隔 1〜2 秒、IDR 時に SPS/PPS を送出
- CBR / 低 VBV バッファ、NV12 入力、エンコードキュー深度 1
- GPU 上でのコピー回数を最小化(DDA テクスチャ → エンコーダ直結を目指す)

### 4.3 デコード要件 (MediaCodec)

- Surface decode(SurfaceView へ直接出力)
- `KEY_LOW_LATENCY`(対応時)、`KEY_PRIORITY=realtime`、`KEY_OPERATING_RATE` 設定
- 入力キュー滞留 1〜2 フレーム以下。詰まったら古いフレームを破棄して追いつく
- 再接続時はデコーダをリセットし、SPS/PPS + IDR の受信を待って再開

### 4.4 転送プロトコル (v1)

固定ヘッダ付きフレーム列。フィールド:

| フィールド | 内容 |
|---|---|
| `magic` + `version` | プロトコル識別 (v1) |
| `type` | handshake / video / heartbeat |
| `seq` | シーケンス番号(欠落検知) |
| `pts` | 送信側タイムスタンプ(遅延計測に使用) |
| `flags` | IDR / SPS-PPS 含有 |
| `payload_len` + payload | H.264 Annex-B データ |

- handshake で解像度・fps・コーデック情報を通知
- heartbeat を一定間隔で送り、途絶で切断検知
- 詰まり時は送信側で古いフレームを破棄(遅延優先、画質は犠牲にしてよい)

## 5. 非機能要件

| 項目 | 目標 |
|---|---|
| 遅延 (E2E) | 80ms 以下(目標 50ms 台) |
| 転送品質 | 20Mbps/60fps 時に p95 フレーム到着間隔 25ms 以下。詰まり時は古いフレームを破棄して復帰 |
| 解像度/fps | 1080×2400 @ 60fps(まず 30fps で動作確認) |
| ビットレート | 10〜20Mbps 目安、設定可能 |
| PC 負荷 | エンコードは GPU オフロード。CPU 常用 5% 以下目安 |
| 安定性 | 連続 8 時間動作でリーク・切断なきこと |
| 観測性 | E2E 遅延、encode/decode 時間、キュー深度、ドロップ率、再接続回数をログ/統計出力。マイルストーン合否判定に使用 |

## 6. マイルストーン

- **M0 (実測ゲート)**: 既製 VDD 導入 + ffmpeg + adb forward + 既存プレーヤーで実測。
  **合格基準: 1080×2400@30fps、10〜20Mbps、E2E p95 100ms 以下、5 分連続で破綻なし。** 不合格ならアーキテクチャ再検討。
- **M1**: Android 受信アプリ MVP(TCP→MediaCodec→SurfaceView)。低遅延設定、キュー滞留計測、再接続時のデコーダリセットを含む
- **M2**: Windows 送信側 MVP(DDA→MF エンコード→TCP)。低遅延設定、IDR 強制、SPS/PPS 再送、フレームドロップ方針を含む。M0 の ffmpeg を置き換え
- **M3**: 安定化 — 自動再接続の堅牢化、カーソル合成、8 時間連続動作の確認
- **M4**: 配布 UX — トレイ常駐化、セットアップ手順整備(VDD/adb 導入ガイド)

## 7. 決定事項(旧 Open Questions)

| # | 決定 | 理由 |
|---|---|---|
| OQ-1 | Windows 送信側は **C++** | DDA / Media Foundation / COM の制御性と参考実装の多さ。P/Invoke の不確実性を回避 |
| OQ-2 | **H.264 固定** | USB 帯域では HEVC の圧縮効率より互換性・デバッグ容易性が重要 |
| OQ-3 | カーソルは **PC 側合成**、実装は M2 以降 | DDA の pointer shape/position を利用するのが自然 |
| OQ-4 | VDD は **手順書方式**(手動インストール) | 自動化は管理者権限・署名まわりの失敗要因が多い。まず映像パイプライン優先 |
| OQ-5 | adb は **同梱せず導入手順のみ** | 更新・ライセンス・既存 adb server 競合を回避。同梱可否は M0 後に再判断 |
| OQ-6 | **SurfaceView** | TextureView の柔軟性より遅延の少なさを優先 |
| OQ-7 | 自作部分は **MIT**。利用 OSS(VDD / platform-tools / ffmpeg)はライセンス別表で管理 | — |
| 画面向き | **縦向き固定 (1080×2400)**(ユーザー決定) | チャット・ログ監視等の常駐表示用途 |

## 8. 運用・エラー処理要件

以下の失敗状態ごとに、ユーザー向けメッセージと自動リトライ方針を実装する:

- adb 未検出 / adb server 起動失敗
- 端末未接続・複数端末接続(シリアル指定で解決)・unauthorized(USB デバッグ未許可)
- VDD 未導入(仮想モニタが見つからない)
- エンコーダ初期化失敗 / MediaCodec 非対応
- ストリーム中の USB 抜去 → 自動再接続ループへ(指数バックオフ)

## 9. セキュリティ要件

- TCP の bind は Windows/Android とも `127.0.0.1` のみ。外部ネットワークへの listen 禁止
- 接続先端末はシリアル番号で固定可能にする
- VDD / platform-tools は公式取得元を README に固定記載(取得元検証)
- ストリームに認証は設けない(localhost + USB 物理接続前提のため許容)

## 10. リスク

- ColorOS のバックグラウンド kill → フォアグラウンド固定 + 画面常時 ON で回避見込み
- adb 接続の不安定さ(他ツールとの adb server 競合、例: Stream Deck 系ユーティリティ)
- DDA はセキュアデスクトップ(UAC 画面等)をキャプチャできない → 仕様として許容
- 90Hz 端末だが 60fps 超は帯域・遅延とトレードオフ(将来検証)
- adb forward のジッタ(adb server 経由コピーに起因) → p95 計測で監視、破綻時はフレーム破棄で追従
