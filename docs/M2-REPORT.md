# M2 実測レポート

**Status: v1.0 FIXED**(2026-08-15 実測、レビュー済み。**判定: PASS** — E2E p95保守的上限34.9msでgate(100ms)・目標(80ms)とも達成。commit 6dacba9 の実装に対する測定。測定生データは docs/data/m2/)

## 1. 実施内容

M2(Windows送信側 MVP, `windows/`, commit 6dacba9)に対して、M1で作成したE2E遅延実測ツール一式(`tools/m1/clock_offset.py` / `measure_e2e.py`)を新センダー(`a2m-sender.exe`)向けに回し、以下を実施した:

1. E2E実測(クロックオフセット較正込み、送信側cap2enc/enc2sentの区間内訳を追加)
2. CBRビットレート挙動の確認(低motion/高motion)

## 2. 実装サマリ(a2m-sender, C++20/CMake, `windows/`)

```
DXGI Desktop Duplication (仮想ディスプレイ)
  -> D3D11 VideoProcessor BGRA->NV12 (GPUカラー変換、CPUコピーなし)
  -> Media Foundation H.264 エンコーダMFT (HW優先、低遅延、CBR、Bフレームなし)
  -> TCP -> localhost:<port> (adb forwardで端末へ)
```

- **エンコーダ**: `NVIDIA H.264 Encoder MFT`(HARDWARE, async, D3D11-aware)を自動選択。起動ログで `AMDh264Encoder` のactivate失敗(0x8007000E、リソース不足)が出るが、NVENCへのフォールバックで正常継続
- **低遅延設定**: `MF_LOW_LATENCY=TRUE`、`CODECAPI_AVLowLatencyMode=true`、`CODECAPI_AVEncCommonRateControlMode=CBR`、GOP=fps×2(2秒)、非同期MFTのペンディングキュー深度1(新フレーム到着時に未消化の前フレームを破棄)
- **pts_us刻印**: 要件通り送信直前(壁時計、`GetSystemTimePreciseAsFileTime`相当)。capture時刻ではない点に注意(内訳計測は別途cap2enc/enc2sentで行う)
- **adb forward自動設定**: 起動時に `SetupAdbForward()` で実行
- **送信キュー**: 深度2、IDR到着時は滞留を全破棄(受信側のクリーンな再同期を優先)、詰まった非IDRは最古から破棄
- **DDAポーリング設計**(2msタイムアウト): `AcquireNextFrame`はD3D11デバイスロック内でブロックし、エンコーダMFTも同じデバイスを共有するため、長いDDA待ちがエンコーダを飢餓状態にする。実測で「静止画面に~16msタイムアウト」を与えるとエンコーダが約19fpsまで低下し、capture→encode遅延が~63msに悪化したとのコメントがコード中にあり(`main.cpp`)、2msの短いポーリング+ロック外の高精度タイマーでフレームペーシングする設計に変更されている。**意図的な設計判断であり、既知の制限ではなく対策済み事項**として記載

## 3. 検証環境

| 項目 | 内容 |
|---|---|
| PC | Windows 11 Pro、GPU: NVIDIA GeForce RTX 5070 Ti |
| 送信 | `windows\build\Release\a2m-sender.exe --duration 75 --bitrate 12M --fps 30 --serial 1b2f0fc` |
| 端末 | OPPO Reno5 A(シリアル1b2f0fc)、A2Mアプリ(M1のptsToRecv計測入り、dev.frogmagical.a2m) |
| 経路 | `adb forward tcp:5001 tcp:5001`(a2m-sender起動時に自動設定) |
| 画面内容(E2E測定) | `tools/m0/clock_overlay.ps1`(低motion、動きのある時刻表示+バウンド矩形) |
| 画面内容(ビットレート確認) | 低motion(同上)/ 高motion(`tools/m1/high_motion_test.ps1` 40px乱色ブロック、`high_motion_test2.ps1` フル解像度per-pixelノイズ) |

事前準備: `adb shell input keyevent KEYCODE_WAKEUP` → `am start` でアプリをフォアグラウンド化(画面消灯でSurfaceが破棄されlistenが止まる既知の挙動への対処)。

## 4. E2E遅延実測

### 4.1 較正・測定コマンド列

```powershell
# 1. 低motion画面を用意
powershell -File tools\m0\clock_overlay.ps1   # バックグラウンド起動

# 2. 較正(前)
python tools\m1\clock_offset.py --samples 50 --serial 1b2f0fc --json m2_offset_before.json

# 3. logcatクリア→75秒ストリーミング
adb -s 1b2f0fc logcat -c
windows\build\Release\a2m-sender.exe --duration 75 --bitrate 12M --fps 30 --serial 1b2f0fc

# 4. 較正(後)
python tools\m1\clock_offset.py --samples 50 --serial 1b2f0fc --json m2_offset_after.json

# 5. logcat回収・解析
adb -s 1b2f0fc logcat -d -s A2M_PERF:V > m2_e2e_logcat.txt
python tools\m1\measure_e2e.py --logcat m2_e2e_logcat.txt \
    --offset-before m2_offset_before.json --offset-after m2_offset_after.json \
    --json m2_e2e_result.json

# 6. オーバーレイ停止
```

75秒で2250フレーム送信、送信側ドロップ0、エンコーダドロップ0、再接続0。受信側(logcat)も1795→2250フレームともに0ドロップで一致。

### 4.2 クロックオフセット較正

| | 較正前 | 較正後 |
|---|---|---|
| 中央値オフセット | 1154.6ms | 1151.4ms |
| min RTT | 60.9ms | 42.9ms |
| 不確かさ(min RTT/2) | ±30.5ms | ±21.4ms |

ドリフト -3.2ms(75秒間)。M1測定時(前日、約1177〜1181ms)と比べオフセット自体は約25ms小さくなっている(端末クロックのドリフト蓄積とみられ、往復較正の不確かさの範囲内であり測定を妨げるものではない)。

### 4.3 区間別遅延内訳

| 区間 | p50 | p95 | 備考 |
|---|---|---|---|
| capture→encode(送信側、cap2enc) | 12.9ms | 16.1ms | DXGIキャプチャ完了からエンコーダ出力まで |
| encode→sent(送信側、enc2sent) | 0.1ms | 0.1ms | エンコーダ出力からTCP送信完了まで(ほぼ即時) |
| sent→recv(ptsToRecv、オフセット補正後) | -30.6ms | -26.0ms | 生値からクロックオフセット較正値を差引。負値は較正不確かさ(±21〜30ms)に埋もれた結果(M1と同様の限界、§6参照) |
| recv→render(端末内、モノトニッククロック) | 11.3ms | 14.2ms | オフセット問題なし、直接測定 |

### 4.4 E2E(capture→render)推定

| | p50 | p95 |
|---|---|---|
| 点推定(sent→recvを生の補正値のまま加算) | -6.3ms | 4.4ms |
| **保守的上限**(sent→recvを`max(0, 補正値+不確かさ)`で下限0にクランプ) | **24.2ms** | **34.9ms** |

点推定が負値になるのはM1と同じ理由(クロックオフセット較正の不確かさがUSB/adb forward越しの実伝送遅延そのものより大きいため)。**保守的上限(p95≈34.9ms)** を実用的な判断基準とする。

## 5. 合否判定(案)

| 基準 | 結果 |
|---|---|
| E2E p95 ≤ 100ms(gate) | **PASS**(保守的上限 34.9ms、約3倍の余裕) |
| E2E p95 ≤ 80ms(目標値) | **PASS**(保守的上限 34.9msで目標も満たす) |
| 30fps安定(低motion) | **PASS**(75秒間 fps=30 を維持、送信・受信ともドロップ0) |
| 8時間連続動作 | **未検証**(M3送り、要件通り) |

M1・M2ともに条件付きではなく明確にgateを下回っており、**E2E遅延に関してはM2時点でPASSと判断してよい**(M1で保留にした「較正精度の限界」は変わらず残るが、上限見積りの余裕が大きいため実用上の判断は揺るがない)。

### M0/M1/M2比較

| | M0(既製ffmpeg+VLC) | M1(テスト送信ツール、python+ffmpeg) | M2(a2m-sender実装) |
|---|---|---|---|
| 送信側 | ffmpeg CPU encode | ffmpeg libx264 ultrafast | Media Foundation + NVENC HW |
| 測定した遅延 | 下限のみ(ブラケット法、≈1.51s、テストベッド支配) | 受信側のみ(recv→render, p95≈15ms)、E2E上限≈36ms | **全区間**(capture→render, p95保守的上限≈35ms) |
| 30fps安定性 | PASS | PASS(drops=0) | PASS(drops=0, encoder_drops=0) |

## 6. ビットレート挙動の確認(CBR課題)

実装エージェントより「高motion時に12Mbps CBR指定に対し瞬間19.5Mbps」の報告があったため、低motion/高motion 2パターンで1秒毎ビットレート分布を確認した(a2m-senderの標準出力ログの `bytes=... (X Mbps)` を集計)。

| コンテンツ | サンプル数(秒) | min | max | 平均 | stdev | 目標比 |
|---|---|---|---|---|---|---|
| 低motion(clock_overlay) | 75 | 0.7 Mbps | 1.0 Mbps | 0.8 Mbps | 0.07 | 目標の7〜8%(大幅未達) |
| 高motion(40px乱色ブロック) | 20 | 10.2 Mbps | 14.6 Mbps | 11.9 Mbps | 1.16 | -15%〜+22% |
| 高motion(フル解像度per-pixelノイズ) | 15 | 8.8 Mbps | **17.0 Mbps** | 11.2 Mbps | 1.93 | -27%〜**+42%** |

- 17.0Mbpsは開始直後(1秒目、IDR含む区間)に観測。IDRフレームは高エントロピー内容だと通常フレームよりビット消費が大きく、CBRのレート制御が瞬間的に大きくオーバーシュートしている。実装エージェント報告の19.5Mbpsも同様にIDR近傍の瞬間値と推測される(本測定では直接19.5Mbpsは再現しなかったが、17.0Mbpsという近い値・同じ発生パターン(開始直後のIDR)を確認できており、報告内容と整合する)
- 逆に低motionでは目標の1割にも届かず(0.8Mbps)大幅未達。これはNVENCのCBRモードが内容に応じてビット配分を大きく変動させていることを示しており、**「CBR」という設定が名目通りに機能していない**(スキップ可能なフレームでビットを使わず、複雑なフレーム・IDR直後に大きくオーバーシュートする、むしろVBR的な挙動)
- `CODECAPI_AVEncCommonRateControlMode=CBR` は設定自体はエラーなく通っている(`SetCodecUI32`が失敗ログを出していない)ため、**API呼び出しは成功しているにもかかわらず実際の挙動がCBRの定義から外れている** — NVENCドライバ/MFTラッパー側の実装挙動の問題である可能性が高い

### M3での要修正事項(案)
1. IDR直後のビットレートオーバーシュート対策(`AVEncCommonBufferSize`を縮小してVBVバッファを厳格化する、IDR用に別途ビットレート上限を設ける等)
2. 低motion時の極端な低ビットレートは遅延・画質上は問題にならないが、帯域が可変であることを前提にした設計(adb forward越しのUSB帯域を圧迫しないことの確認は良い方向ではある)
3. CBR名目と実挙動の乖離をNVENC固有の問題かMF層の問題か切り分ける(別GPU/別ドライバでの追試が望ましい)

## 7. 既知の制限

1. **マウスカーソル未合成**(要件OQ-3、M2スコープ外として明記済み。M3以降で対応予定)
2. **Bフレーム数を断定できない**: `CODECAPI_AVEncMPVDefaultBPictureCount=0` の設定はログ上 `0x80070057`(パラメータ不正)で**拒否されている**(`AVEncNumWorkerThreads=1` も同様に拒否)。コード側は「Bフレームなし」を前提にログ(`B-frames=0`)を出しているが、これは設定値の記録であって実際のビットストリームでB-sliceが出ていないことを検証したものではない。NAL/スライスレベルでの確認(受信側でスライスタイプを解析する等)は未実施
3. **DDA 2msポーリング**は意図的な設計判断(§2参照)。CPU使用率への影響(短間隔ポーリングによるビジーウェイト気味の挙動)は本測定でプロファイルしておらず、要件§5の「CPU負荷5%以下目安」との整合は未検証
4. **E2Eクロックオフセット較正の精度限界はM1から継続**(adb shellプロセス起動オーバーヘッドが不確かさの支配要因、±21〜30ms)。sent→recv区間の正の実測値は得られておらず、保守的上限での判断に留まる
5. 8時間安定性・CBR挙動の恒久修正はM3送り

## 8. 使用したコマンド列(再現用)

```powershell
# 環境準備
adb -s 1b2f0fc shell input keyevent KEYCODE_WAKEUP
adb -s 1b2f0fc shell am force-stop dev.frogmagical.a2m
adb -s 1b2f0fc shell am start -n dev.frogmagical.a2m/.MainActivity

# E2E測定(低motion, 75秒)
powershell -File tools\m0\clock_overlay.ps1                      # background
python tools\m1\clock_offset.py --samples 50 --json m2_offset_before.json
adb -s 1b2f0fc logcat -c
windows\build\Release\a2m-sender.exe --duration 75 --bitrate 12M --fps 30 --serial 1b2f0fc
python tools\m1\clock_offset.py --samples 50 --json m2_offset_after.json
adb -s 1b2f0fc logcat -d -s A2M_PERF:V > m2_e2e_logcat.txt
python tools\m1\measure_e2e.py --logcat m2_e2e_logcat.txt --offset-before m2_offset_before.json --offset-after m2_offset_after.json --json m2_e2e_result.json
# (clock_overlay停止)

# ビットレート確認(高motion)
powershell -File tools\m1\high_motion_test.ps1                   # 40px乱色ブロック, background
windows\build\Release\a2m-sender.exe --duration 20 --bitrate 12M --fps 30 --serial 1b2f0fc
# (停止 → high_motion_test2.ps1 に切替: フル解像度per-pixelノイズ, background)
windows\build\Release\a2m-sender.exe --duration 15 --bitrate 12M --fps 30 --serial 1b2f0fc
# (停止)
```

すべての一時プロセス(clock_overlay.ps1 / high_motion_test.ps1 / high_motion_test2.ps1)は測定終了後に停止済み。
