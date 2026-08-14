# AndroidtoMonitor 転送プロトコル v1

要件 4.4 のバイトレベル仕様。**リトルエンディアン**(送受信ともx86-64/ARM64のため。ネットワークバイトオーダーは採用しない)。

## 接続モデル

- Android 側アプリが `127.0.0.1:5001` で TCP listen
- Windows 側が起動時に `adb forward tcp:5001 tcp:5001` を設定し、`localhost:5001` へ connect
- 1接続のみ。新しい接続が来たら既存接続を破棄して新接続を優先
- 切断検知: heartbeat 途絶(3秒)または TCP エラー。受信側はデコーダをリセットし、再接続後は config+IDR を受信するまでフレームを破棄する

## フレーム構造

すべてのメッセージは 24 バイト固定ヘッダ + 可変ペイロード。

| offset | size | field | 内容 |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII "A2M1" (bytes: 41 32 4D 31) |
| 4 | 1 | `version` | 1 |
| 5 | 1 | `type` | 1=handshake, 2=video, 3=heartbeat |
| 6 | 2 | `flags` | bit0: IDR含む / bit1: SPS+PPS含む / 他は0 |
| 8 | 4 | `seq` | u32 通番(type毎ではなく全メッセージ通し。欠落検知用) |
| 12 | 8 | `pts_us` | u64 送信側 Unix epoch マイクロ秒(遅延計測用。再生スケジューリングには使わない) |
| 20 | 4 | `payload_len` | u32 ペイロード長(バイト) |
| 24 | n | payload | type 依存 |

- ヘッダの `magic`/`version` 不一致は即切断
- `payload_len` 上限 8MB(超過は不正として切断)

## メッセージ種別

### type=1: handshake(接続後、送信側が最初に1回送る)

payload は UTF-8 JSON:

```json
{"width":1080,"height":2400,"fps":30,"codec":"h264"}
```

受信側はこれを受けてデコーダを構成する。video より先に必ず送る。

### type=2: video

payload は H.264 Annex-B(start code `00 00 00 01` 付き)の 1 アクセスユニット。

- IDR フレームには SPS/PPS を先頭に連結し、`flags` の bit0|bit1 を立てる(mid-stream join と再接続復帰を単純化するため)
- 非 IDR は bit なし
- B フレームなし(送信側で無効化)なので表示順=デコード順

### type=3: heartbeat

payload なし(`payload_len=0`)。映像フレームの有無に関わらず 1 秒間隔で送信。受信側は最終受信時刻から 3 秒で切断とみなす。

## 受信側の動作規範(要件 4.3 / M0申し送りの再掲)

1. 到着フレームは即時デコード投入・即時描画。PTS は計測ログ専用
2. 入力キュー滞留が 2 フレームを超えたら古い非 IDR フレームを破棄(次の IDR まで読み捨ててリカバリしてよい)
3. 受信時刻・デコード完了時刻・描画投入時刻を計測ログに記録できること(M1 の遅延実証に使用)
