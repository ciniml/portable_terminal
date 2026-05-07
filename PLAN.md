* M5Stack Tab5をつかったターミナル・エミュレータ
* 複数接続先管理
* UART、USBシリアル、TELNET、SSH接続
* WireGuard/Tailscale VPN対応

* ESP-IDF 6.0を使用
    * ~/esp-idf/6.0
* ビルド用Makefileを作成して使用
* ビルド手順などは適宜CLAUDE.mdに記載
* ~/serial_wifi_loggerにあるWireGuard/tailscaleコンポーネントを再利用

* C++23を使用。std::expectedを活用したエラーハンドリングを優先

* ESP-IDFのhello worldをベースに開発
* まずはVT100に対応したターミナルエミュレータを作成。
* ソースコードをホストでもビルド可能とし、ハードウェア非依存部分を適切に抽象化
