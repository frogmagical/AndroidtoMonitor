# AndroidtoMonitor

USB 接続した Android 端末を Windows 11 の拡張ディスプレイとして利用する最小構成のソフトウェア。

- 映像片方向ストリーミングのみ(タッチ・音声なし)
- 仮想ディスプレイドライバ(既製 OSS)+ Desktop Duplication + H.264 HW エンコード
- 転送は adb forward (USB) 経由の TCP

詳細は [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) を参照。

## 構成(予定)

```
windows/   送信側 (キャプチャ + エンコード + TCP 送出)
android/   受信側 (TCP 受信 + MediaCodec デコード + 全画面表示)
docs/      要件・設計ドキュメント
```

## Status

要件定義フェーズ。
