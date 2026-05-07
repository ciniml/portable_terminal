# FTDI USB-to-Serial Host Driver for ESP32-S3

FTDIのUSBシリアル変換チップ用のESP32-S3 USB OTGホストドライバです。

## 特徴

- **FT232R対応**: 最も一般的なFTDIチップに対応
- **モジュラー設計**: プロトコル処理とUSBホスト処理を分離
- **テスト可能**: プロトコル層はLinux上でテスト可能
- **完全な制御**: DTR/RTS、ボーレート、ライン設定など完全制御
- **モデムステータス**: CTS/DSR/CD/RIの監視

## サポートするチップ

- FT232R (優先実装)
- FT232H (基本サポート)
- FT2232D/H (基本サポート)
- FT4232H (基本サポート)
- FT230X (基本サポート)

## APIの使用方法

### 1. ドライバのインストール

```c
#include "usb/ftdi_sio_host.h"

ftdi_sio_host_driver_config_t driver_config = FTDI_SIO_HOST_DRIVER_CONFIG_DEFAULT();
esp_err_t ret = ftdi_sio_host_install(&driver_config);
```

### 2. デバイスのオープン

```c
#include "usb/ftdi_sio_host.h"
#include "usb/ftdi_sio_host_ops.h"

// データ受信コールバック
void data_rx_callback(const uint8_t *data, size_t data_len, void *user_arg)
{
    printf("Received %d bytes\n", data_len);
}

// デバイスイベントコールバック
void device_event_callback(ftdi_sio_host_dev_event_t event, void *user_arg)
{
    if (event == FTDI_SIO_HOST_MODEM_STATUS) {
        printf("Modem status changed\n");
    }
}

// デバイスを開く
ftdi_sio_host_device_config_t dev_config = FTDI_SIO_HOST_DEVICE_CONFIG_DEFAULT();
dev_config.data_cb = data_rx_callback;
dev_config.event_cb = device_event_callback;

ftdi_sio_dev_hdl_t ftdi_hdl;
ret = ftdi_sio_host_open(FTDI_VID, FTDI_PID_FT232R, 0, &dev_config, &ftdi_hdl);
```

### 3. 通信設定

```c
// ボーレート設定
ftdi_sio_host_set_baudrate(ftdi_hdl, 115200);

// ライン設定 (8N1)
ftdi_sio_host_set_line_property(ftdi_hdl,
                                 FTDI_DATA_BITS_8,
                                 FTDI_STOP_BITS_1,
                                 FTDI_PARITY_NONE);

// DTR/RTS制御
ftdi_sio_host_set_modem_control(ftdi_hdl, true, true);  // DTR=ON, RTS=ON
```

### 4. データ送受信

```c
// データ送信
const uint8_t data[] = "Hello FTDI\n";
ftdi_sio_host_data_tx_blocking(ftdi_hdl, data, sizeof(data) - 1, 1000);

// データ受信は自動的にコールバックで通知される
```

### 5. モデムステータスの取得

```c
ftdi_modem_status_t status;
ftdi_sio_host_get_modem_status(ftdi_hdl, &status);

printf("CTS: %d, DSR: %d, CD: %d, RI: %d\n",
       status.cts, status.dsr, status.rlsd, status.ri);
```

### 6. デバイスのクローズ

```c
ftdi_sio_host_close(ftdi_hdl);
ftdi_sio_host_uninstall();
```

## アーキテクチャ

### ディレクトリ構造

```
components/usb_host_ftdi_sio/
├── include/usb/
│   ├── ftdi_sio_host.h          # 公開API
│   ├── ftdi_sio_host_ops.h      # 操作関数
│   └── ftdi_host_types.h        # 公開型定義
├── include/esp_private/
│   └── ftdi_host_common.h       # 内部デバイス構造体
├── private_include/
│   ├── ftdi_host_protocol.h     # プロトコル層(ポータブル)
│   └── ftdi_host_descriptor_parsing.h
├── src/
│   ├── ftdi_host_protocol.c     # プロトコル実装(Linux/ESP32共通)
│   ├── ftdi_host_descriptor_parsing.c
│   ├── ftdi_sio_host.c          # ESP32統合
│   └── ftdi_sio_host_ops.c      # 操作関数ラッパー
├── host_test/                   # Linuxテスト
│   └── protocol_tests/
│       ├── CMakeLists.txt
│       └── test_ftdi_protocol.cpp
├── CMakeLists.txt
└── idf_component.yml
```

### レイヤー構造

```
Application
    ↓
ftdi_sio_host_ops.h (操作関数API)
    ↓
ftdi_sio_host.h (メインAPI)
    ↓
ftdi_host_protocol.h (プロトコル層 - ポータブル)
    ↓
ESP32 USB Host Library
```

## FTDIプロトコルの特徴

### Bulk INパケット構造

FTDIデバイスは**全てのBulk INパケット**の先頭2バイトにモデムステータスを含みます:

```
[Byte 0] [Byte 1] [Data 0] [Data 1] ... [Data N]
  ↑        ↑         ↑
  B0       B1      実データ
```

- **Byte 0 (B0)**: データバッファステータス、エラー情報
- **Byte 1 (B1)**: CTS/DSR/RI/CD信号状態

ドライバは自動的にこれらのステータスバイトを除去し、実データのみをアプリケーションに渡します。

### ボーレート計算

FT232Rは3MHzの基準クロックと分数divisorを使用:

```
baudrate = 3000000 / divisor
divisor = (integer_part << 3) | fractional_index
```

分数値: 0 (.000), 1 (.125), 2 (.25), 3 (.375), 4 (.5), 5 (.625), 6 (.75), 7 (.875)

### DTR/RTS制御

DTR/RTSは16ビット値で制御:
- 上位バイト: 制御マスク (1=このピンを制御)
- 下位バイト: ピン値 (1=HIGH, 0=LOW)

```c
// DTR=HIGH, RTS=HIGH
value = 0x0303  // (mask=0x0300, value=0x0003)

// DTR=LOW, RTS=LOW
value = 0x0300  // (mask=0x0300, value=0x0000)
```

## Linuxでのテスト

プロトコル層の単体テストをLinux上で実行できます:

```bash
cd components/usb_host_ftdi_sio/host_test/protocol_tests
mkdir build && cd build
cmake ..
make
./ftdi_protocol_tests
```

**期待される出力:**
```
===============================================================================
All tests passed (85 assertions in 7 test cases)
```

**テスト内容:**
- ✅ コントロールリクエストパケット生成 (reset, DTR/RTS, baudrate等)
- ✅ ボーレート計算の精度 (9600, 115200, 921600等)
- ✅ モデムステータス解析 (CTS/DSR/RI/CD)
- ✅ ライン設定 (8N1, 7E1, 8N2等)
- ✅ 境界値テスト (不正な引数、NULLポインタ等)

**注意:** テストはCatch2フレームワークを使用し、ESP-IDFヘッダ(`esp_err.h`)は最小限のLinux用モックを提供しています ([esp_mock/esp_err.h](host_test/protocol_tests/esp_mock/esp_err.h) - `ESP_OK`と`ESP_ERR_INVALID_ARG`のみ定義)。

## 制限事項

- 現在の実装はFT232Rを最優先
- Interrupt INエンドポイントは未サポート(Bulk INで十分)
- フロー制御機能(RTS/CTS自動制御)は未実装
- マルチポートチップ(FT2232H等)は基本サポートのみ

## 今後の拡張

- FT232H/FT2232H/FT4232Hの完全サポート
- フロー制御機能の実装
- Interrupt INエンドポイント対応
- libusbを使った実機テスト環境

## ライセンス

Apache License 2.0

## 参考資料

- [FTDI FT232R Datasheet](https://ftdichip.com/wp-content/uploads/2020/08/DS_FT232R.pdf)
- [FTDI Baud Rates Application Note](https://ftdichip.com/Documents/AppNotes/AN232B-05_BaudRates.pdf)
- ESP-IDF USB Host Library Documentation
