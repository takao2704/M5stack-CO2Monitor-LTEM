# M5Stack CO2 Monitor システムフロー

このファイルは src/main.cpp の処理フローを抽象化して1ページで表現しています。

## システム全体フロー

```mermaid
%%{init: {'theme':'base', 'themeVariables': {
  'primaryColor':'#1565C0', 'primaryTextColor':'#ffffff', 'primaryBorderColor':'#0D47A1',
  'secondaryColor':'#2E7D32', 'secondaryTextColor':'#ffffff',
  'tertiaryColor':'#F5F5F5', 'lineColor':'#37474F', 'textColor':'#212121',
  'fontSize':'14px', 'fontFamily':'Inter, Roboto, Helvetica, Arial',
  'clusterBkg':'#ECEFF1', 'clusterBorder':'#607D8B',
  'edgeLabelBackground':'#ffffff',
  'error':'#C62828', 'errorTextColor':'#ffffff',
  'warning':'#EF6C00', 'warningTextColor':'#ffffff',
  'success':'#2E7D32', 'successTextColor':'#ffffff'
}}}%%
flowchart TD
  classDef start fill:#1565C0,stroke:#0D47A1,stroke-width:2px,color:#fff;
  classDef config fill:#2E7D32,stroke:#1B5E20,stroke-width:2px,color:#fff;
  classDef mqtt fill:#00838F,stroke:#006064,stroke-width:2px,color:#fff;
  classDef udp fill:#6A1B9A,stroke:#4A148C,stroke-width:2px,color:#fff;
  classDef action fill:#F5F5F5,stroke:#455A64,stroke-width:2px,color:#212121;
  classDef danger fill:#C62828,stroke:#8E0000,stroke-width:2px,color:#fff;

  A[システム起動]:::start --> B[メタデータ取得<br/>(interval_s, mqtt, topic, qos)]:::config
  B --> C{mqtt == true?}:::action

  C -->|はい| M1[MQTT設定<br/>SMCONF(URL/CLIENTID/.../QOS)]:::mqtt
  M1 --> M2[ensurePdp0Active<br/>(+CNACT=0,1 / GPRS再接続)]:::mqtt
  M2 --> M3[SMCONN]:::mqtt
  M3 --> M4{SMSTATE 1/2?}:::action
  M4 -->|はい| M5[測定→JSON生成→SMPUB]:::mqtt
  M4 -->|いいえ| M6[指数バックオフ再試行]:::action
  M6 --> M7{既定回数失敗?}:::action
  M7 -->|はい| M8[SMDISC→SMCONF再適用→最終SMCONN]:::mqtt
  M7 -->|いいえ| M3
  M8 --> M9{オンライン?}:::action
  M9 -->|はい| M5
  M9 -->|いいえ| R[モデムリセット/再起動]:::danger

  C -->|いいえ| U1[UDPソケットオープン]:::udp
  U1 --> U2[測定→16Bバイナリ送信]:::udp
  U2 --> U3{成功?}:::action
  U3 -->|はい| L1[LCD/ログ更新]:::action --> G[待機/次周期]:::action
  U3 -->|いいえ| U4[指数バックオフ再試行]:::action
  U4 --> U5{既定回数失敗?}:::action
  U5 -->|はい| R
  U5 -->|いいえ| U1

  M5 --> L1
  G --> T{5分無成功?}:::action
  T -->|はい| R
  T -->|いいえ| B
```

### 補足
- MQTT 経路: JSONペイロード（例: {"co2": 612.3, "temp": 26.1, "humi": 54.2, "wind": 0.72}）
- UDP 経路: 16バイト固定バイナリ（4つのfloat: CO2, 温度, 湿度, 風速・いずれもLE）