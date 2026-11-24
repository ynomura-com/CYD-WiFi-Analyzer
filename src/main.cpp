// CYD Wi-Fi Analyzer - ESP32-2432S028 (Cheap Yellow Display)
// 周囲の 2.4GHz Wi-Fi をスキャンして、チャンネルグラフ / 一覧 / おすすめチャンネルを表示します。
//
// VS Code + PlatformIO 用 (platformio.ini でライブラリと TFT_eSPI 設定を管理)
//
// 操作:
//   画面タップ        : 表示切替 (グラフ -> 一覧 -> おすすめ -> グラフ ...)
//   一覧表示で右端タップ: 次ページ / 左端タップ: 前ページ
//   BOOTボタン (GPIO0): 表示切替 (タッチが合わないときの予備)
//   ヘッダー右端タップ : 即時再スキャン
//
// 注意:
//   ESP32 は 2.4GHz のみ対応です。5GHz は表示されません。
//   このアプリは「見るだけ」で、どの AP にも接続しません。
//   配線は不要です。CYD 本体だけで動きます。

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>

// ---------- 設定 ----------
#define FW_VERSION       "v5"   // 画面ヘッダーとシリアルの [scan] 行に出ます
#define SCAN_INTERVAL_MS 5000   // 自動再スキャンの間隔
#define MAX_APS          32     // 保持する最大 AP 数
#define RSSI_TOP         (-30)  // グラフ上端 (dBm)
#define RSSI_BOTTOM      (-100) // グラフ下端 (dBm)
#define BOOT_PIN         0

// ---------- 画面 (横向き 320x240) ----------
#define SCREEN_W 320
#define SCREEN_H 240
#define HEADER_H 26

#define COL_BG     TFT_BLACK
#define COL_GRID   0x4208       // 暗いグレー
#define COL_TEXT   TFT_WHITE
#define COL_SUB    0xAD55       // 薄いグレー
#define COL_ACCENT TFT_CYAN
#define COL_GOOD   TFT_GREEN
#define COL_MID    TFT_YELLOW
#define COL_BAD    TFT_ORANGE
#define COL_WORST  TFT_RED

TFT_eSPI tft = TFT_eSPI();

// AP ごとに使う色 (グラフ用パレット)
static const uint16_t PALETTE[] = {
  TFT_GREEN, TFT_CYAN, TFT_MAGENTA, TFT_YELLOW, TFT_ORANGE,
  TFT_PINK, TFT_SKYBLUE, TFT_GREENYELLOW, TFT_VIOLET, TFT_GOLD,
  0xF81F, 0x07FF, 0xFFE0, 0xFC18, 0x87FF
};
#define PALETTE_N (sizeof(PALETTE) / sizeof(PALETTE[0]))

// ---------- 状態 ----------
struct ApInfo {
  String   ssid;
  String   bssid;
  int32_t  rssi;
  int32_t  channel;
  wifi_auth_mode_t enc;
  int      colorIdx;
};

enum View { VIEW_GRAPH = 0, VIEW_LIST = 1, VIEW_BEST = 2, VIEW_COUNT = 3 };

ApInfo aps[MAX_APS];
int    apCount   = 0;
View   view      = VIEW_GRAPH;
int    listPage  = 0;
unsigned long lastScanMs = 0;
unsigned long lastTouchMs = 0;
bool   scanning  = false;
int    scanCount = 0;

// ---------- タッチ (XPT2046) ----------
// 【重要】CYD のタッチはディスプレイと別の SPI バスに付いています。
//   ディスプレイ: HSPI (MOSI=13 / MISO=12 / SCLK=14)
//   タッチ XPT2046: VSPI (MOSI=32 / MISO=39 / SCLK=25 / CS=33 / IRQ=36)
// TFT_eSPI の getTouch() は「タッチもディスプレイと同じバス」前提のため
// CYD では永遠に反応しません (ライブラリ作者も別ドライバが必要と明言)。
// そのためここでは VSPI を直接叩く最小ドライバを自前で実装しています。
#define TOUCH_SCK  25
#define TOUCH_MISO 39   // 入力専用ピン
#define TOUCH_MOSI 32
#define TOUCH_CS   33
#define TOUCH_IRQ  36   // 入力専用ピン。タッチ中は LOW

// 生値 (0..4095) の校正範囲。個体差があればシリアルの [touch] raw ログを
// 見て、四隅タップ時の実測に合わせてこの4つを直してください。
#define TOUCH_RAW_X_MIN 300
#define TOUCH_RAW_X_MAX 3800
#define TOUCH_RAW_Y_MIN 300
#define TOUCH_RAW_Y_MAX 3800

SPIClass touchSPI(VSPI);

// ---------- 小物 ----------
String encStr(wifi_auth_mode_t e) {
  switch (e) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "?";
  }
}

uint16_t rssiColor(int32_t rssi) {
  if (rssi >= -55) return COL_GOOD;
  if (rssi >= -67) return TFT_GREENYELLOW;
  if (rssi >= -75) return COL_MID;
  if (rssi >= -85) return COL_BAD;
  return COL_WORST;
}

// RSSI から信号バー (0-4)
int rssiBars(int32_t rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

void drawSignalBars(int x, int y, int32_t rssi) {
  int bars = rssiBars(rssi);
  uint16_t c = rssiColor(rssi);
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    if (i < bars) tft.fillRect(x + i * 5, y + (12 - h), 3, h, c);
    else          tft.drawRect(x + i * 5, y + (12 - h), 3, h, COL_GRID);
  }
}

String displaySsid(const ApInfo &ap, size_t maxLen) {
  String s = ap.ssid.length() ? ap.ssid : String("<hidden>");
  if (s.length() > maxLen) s = s.substring(0, maxLen);
  return s;
}

// ---------- ヘッダー ----------
void drawHeader(const char *rightText) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, 0x18E3);
  tft.setTextColor(COL_ACCENT, 0x18E3);
  tft.setTextSize(1);
  tft.setTextFont(2);
  const char *title = (view == VIEW_GRAPH) ? "WiFi Analyzer - Graph 2.4GHz"
                    : (view == VIEW_LIST)  ? "WiFi Analyzer - List"
                                           : "WiFi Analyzer - Best CH";
  tft.drawString(title, 6, 4);
  tft.setTextColor(COL_TEXT, 0x18E3);
  tft.drawRightString(rightText, SCREEN_W - 6, 4, 2);
  // 次回スキャンまでの進捗バー
  if (!scanning && lastScanMs > 0) {
    unsigned long el = millis() - lastScanMs;
    int w = (int)min<unsigned long>(el * SCREEN_W / SCAN_INTERVAL_MS, SCREEN_W);
    tft.fillRect(0, HEADER_H - 2, w, 2, COL_ACCENT);
  }
}

// ---------- スキャン ----------
void doScan() {
  scanning = true;
  drawHeader("Scanning...");
  Serial.println("[scan] start");
  int n = WiFi.scanNetworks(false, true); // hidden も含める
  scanning = false;
  lastScanMs = millis();
  scanCount++;
  if (n < 0) {
    Serial.println("[scan] failed");
    apCount = 0;
    return;
  }
  if (n > MAX_APS) n = MAX_APS;
  apCount = n;
  for (int i = 0; i < n; i++) {
    aps[i].ssid    = WiFi.SSID(i);
    aps[i].bssid   = WiFi.BSSIDstr(i);
    aps[i].rssi    = WiFi.RSSI(i);
    aps[i].channel = WiFi.channel(i);
    aps[i].enc     = WiFi.encryptionType(i);
    aps[i].colorIdx = i % (int)PALETTE_N;
  }
  WiFi.scanDelete();

  // RSSI の強い順にソート (単純なバブルソートで十分)
  for (int i = 0; i < apCount - 1; i++) {
    for (int j = i + 1; j < apCount; j++) {
      if (aps[j].rssi > aps[i].rssi) {
        ApInfo tmp = aps[i]; aps[i] = aps[j]; aps[j] = tmp;
      }
    }
  }
  // 色はグラフで見分けやすいよう、強い順にパレットを割り直す
  for (int i = 0; i < apCount; i++) aps[i].colorIdx = i % (int)PALETTE_N;

  Serial.printf("[scan] fw=%s  %d APs\n", FW_VERSION, apCount);
  for (int i = 0; i < apCount; i++) {
    Serial.printf("  %2d  ch%-2ld  %4ld dBm  %-24s  %s  %s\n",
                  i + 1, (long)aps[i].channel, (long)aps[i].rssi,
                  displaySsid(aps[i], 24).c_str(),
                  encStr(aps[i].enc).c_str(), aps[i].bssid.c_str());
  }
  // 一覧のページが範囲外になっていたら戻す
  int pages = max(1, (apCount + 8) / 9);
  if (listPage >= pages) listPage = 0;
}

// ---------- グラフ表示 ----------
// グラフ領域
#define GX 14
#define GY (HEADER_H + 14)      // グラフ上端
#define GW (SCREEN_W - GX - 6)
#define GH 148                  // グラフ高さ
#define GY_BOT (GY + GH)

int chToX(float ch) { return GX + (int)((ch - 1.0f) * GW / 13.0f); }
int rssiToY(int32_t rssi) {
  if (rssi > RSSI_TOP) rssi = RSSI_TOP;
  if (rssi < RSSI_BOTTOM) rssi = RSSI_BOTTOM;
  return GY_BOT - (int)((rssi - RSSI_BOTTOM) * GH / (RSSI_TOP - RSSI_BOTTOM));
}

void drawGraph() {
  tft.fillScreen(COL_BG);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d APs %s", apCount, FW_VERSION);
  drawHeader(buf);

  // グリッド: -40 / -60 / -80 dBm
  tft.setTextColor(COL_SUB, COL_BG);
  tft.setTextFont(1);
  tft.setTextSize(1);
  for (int db = -40; db >= -80; db -= 20) {
    int y = rssiToY(db);
    tft.drawLine(GX, y, GX + GW, y, COL_GRID);
    char b[8]; snprintf(b, sizeof(b), "%d", db);
    tft.drawString(b, 0, y - 3);
  }
  tft.drawLine(GX, GY_BOT, GX + GW, GY_BOT, COL_SUB);
  // チャンネル目盛り (数字のみ。縦の目盛り線は引かない)
  for (int ch = 1; ch <= 13; ch++) {
    int x = chToX(ch);
    char b[4]; snprintf(b, sizeof(b), "%d", ch);
    tft.drawCentreString(b, x, GY_BOT + 6, 1);
  }
  tft.setTextColor(COL_SUB, COL_BG);
  tft.drawString("2.4 GHz  (ESP32 is 2.4GHz only)", GX, GY_BOT + 18);

  if (apCount == 0) {
    tft.setTextColor(COL_MID, COL_BG);
    tft.drawCentreString("No AP found", SCREEN_W / 2, GY + GH / 2, 2);
    return;
  }

  // 各 AP を滑らかなベル型で描く: 中心 ch ±2 でちょうど底辺に着地する cos^2 カーブ。
  // 高さはピクセル空間で決める (底辺 = グラフ下端、頂点 = RSSI の位置)。
  // 以前は dB 空間の放物線 + 底辺への直線で、強い AP ほど両側が直線になり台形に見えた。
  for (int i = apCount - 1; i >= 0; i--) { // 弱い AP から描いて強い AP を上に
    const ApInfo &ap = aps[i];
    uint16_t col = PALETTE[ap.colorIdx];
    int xc = chToX(ap.channel);
    int x0 = chToX(ap.channel - 2);
    int x1 = chToX(ap.channel + 2);
    int yTop = rssiToY(ap.rssi);
    int h = GY_BOT - yTop; // 山の高さ (px)
    int prevX = x0, prevY = GY_BOT;
    for (int x = x0 + 2; x <= x1; x += 2) {
      float t = (float)(x - xc) / max(1, (x1 - x0) / 2); // -1..1
      if (t < -1.0f) t = -1.0f;
      if (t >  1.0f) t =  1.0f;
      float c = cosf(t * PI / 2.0f);          // 端で 0、中心で 1、両端とも傾き 0
      int y = GY_BOT - (int)(h * c * c + 0.5f);
      tft.drawLine(prevX, prevY, x, y, col);
      prevX = x; prevY = y;
    }
    // ピークに SSID と RSSI (強い AP だけラベルを出すと読みやすい)
    if (i < 8) {
      tft.setTextColor(col, COL_BG);
      tft.setTextFont(1);
      String label = displaySsid(ap, 12);
      tft.drawCentreString(label, chToX(ap.channel), max(GY, yTop - 10), 1);
    }
  }
}

// ---------- 一覧表示 ----------
void drawList() {
  tft.fillScreen(COL_BG);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d APs %s", apCount, FW_VERSION);
  drawHeader(buf);

  if (apCount == 0) {
    tft.setTextColor(COL_MID, COL_BG);
    tft.drawCentreString("No AP found", SCREEN_W / 2, SCREEN_H / 2, 2);
    return;
  }
  const int ROWS = 9;
  const int ROW_H = 21;
  int pages = (apCount + ROWS - 1) / ROWS;
  int start = listPage * ROWS;
  int end = min(apCount, start + ROWS);

  // 見出し
  tft.setTextColor(COL_SUB, COL_BG);
  tft.setTextFont(1);
  tft.drawString("SSID", 6, HEADER_H + 3);
  tft.drawString("CH", 196, HEADER_H + 3);
  tft.drawString("dBm", 224, HEADER_H + 3);
  tft.drawString("ENC", 262, HEADER_H + 3);
  tft.drawLine(0, HEADER_H + 13, SCREEN_W, HEADER_H + 13, COL_GRID);

  tft.setTextFont(2);
  for (int r = 0; r < end - start; r++) {
    const ApInfo &ap = aps[start + r];
    int y = HEADER_H + 16 + r * ROW_H;
    if (r % 2 == 1) tft.fillRect(0, y - 1, SCREEN_W, ROW_H, 0x0841);
    drawSignalBars(6, y + 2, ap.rssi);
    tft.setTextColor(COL_TEXT, (r % 2 == 1) ? 0x0841 : COL_BG);
    tft.drawString(displaySsid(ap, 20), 32, y);
    char b[8];
    snprintf(b, sizeof(b), "%ld", (long)ap.channel);
    tft.setTextColor(COL_ACCENT, (r % 2 == 1) ? 0x0841 : COL_BG);
    tft.drawString(b, 196, y);
    snprintf(b, sizeof(b), "%ld", (long)ap.rssi);
    tft.setTextColor(rssiColor(ap.rssi), (r % 2 == 1) ? 0x0841 : COL_BG);
    tft.drawString(b, 224, y);
    tft.setTextColor(COL_SUB, (r % 2 == 1) ? 0x0841 : COL_BG);
    tft.drawString(encStr(ap.enc), 262, y);
  }
  // ページ表示
  if (pages > 1) {
    char b[20];
    snprintf(b, sizeof(b), "< %d/%d >", listPage + 1, pages);
    tft.setTextColor(COL_SUB, COL_BG);
    tft.drawCentreString(b, SCREEN_W / 2, SCREEN_H - 14, 1);
  }
}

// ---------- おすすめチャンネル ----------
// チャンネル ch の混雑スコア。小さいほど空いている。
// 重なり: 同一 ch = 1.0、1つずれ = 0.8、2つ = 0.5、3つ = 0.25、4つ = 0.1、それ以上 0
// 重み: RSSI を線形電力っぽく近似 (-30dBm=1.0 として dB 差で減衰)
float channelScore(int ch) {
  static const float overlap[5] = {1.0f, 0.8f, 0.5f, 0.25f, 0.1f};
  float score = 0;
  for (int i = 0; i < apCount; i++) {
    int d = abs((int)aps[i].channel - ch);
    if (d > 4) continue;
    float w = powf(10.0f, (float)(aps[i].rssi - RSSI_TOP) / 20.0f); // 振幅比近似
    score += overlap[d] * w;
  }
  return score;
}

void drawBest() {
  tft.fillScreen(COL_BG);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d APs %s", apCount, FW_VERSION);
  drawHeader(buf);

  float scores[14];
  float maxScore = 0.001f;
  for (int ch = 1; ch <= 13; ch++) {
    scores[ch] = channelScore(ch);
    if (scores[ch] > maxScore) maxScore = scores[ch];
  }
  // 空いている順に 1 / 6 / 11 の中での最良と、全体の最良を求める
  int best1611 = 1;
  for (int ch : {6, 11}) if (scores[ch] < scores[best1611]) best1611 = ch;
  int bestAll = 1;
  for (int ch = 2; ch <= 13; ch++) if (scores[ch] < scores[bestAll]) bestAll = ch;

  // 棒グラフ (混雑度)
  const int bx = 28, by = HEADER_H + 22, bw = SCREEN_W - bx - 10, bh = 96;
  tft.setTextColor(COL_SUB, COL_BG);
  tft.setTextFont(1);
  tft.drawString("Congestion per channel (lower = better)", bx, HEADER_H + 8);
  for (int ch = 1; ch <= 13; ch++) {
    int x = bx + (ch - 1) * bw / 13;
    int w = bw / 13 - 3;
    int h = (int)(scores[ch] / maxScore * bh);
    if (h < 1 && apCount > 0) h = 1;
    uint16_t c = (ch == bestAll) ? COL_GOOD : (scores[ch] > maxScore * 0.6f ? COL_WORST : COL_ACCENT);
    tft.fillRect(x, by + bh - h, w, h, c);
    char b[4]; snprintf(b, sizeof(b), "%d", ch);
    tft.setTextColor(ch == bestAll ? COL_GOOD : COL_SUB, COL_BG);
    tft.drawCentreString(b, x + w / 2, by + bh + 4, 1);
  }

  // 結論
  int y = by + bh + 22;
  tft.setTextFont(2);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Recommended:", 10, y);
  tft.setTextColor(COL_GOOD, COL_BG);
  char b[48];
  snprintf(b, sizeof(b), "CH %d  (of 1/6/11)", best1611);
  tft.drawString(b, 130, y);
  y += 22;
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Least busy:", 10, y);
  tft.setTextColor(COL_GOOD, COL_BG);
  snprintf(b, sizeof(b), "CH %d", bestAll);
  tft.drawString(b, 130, y);
  y += 22;
  tft.setTextColor(COL_SUB, COL_BG);
  tft.setTextFont(1);
  tft.drawString("Note: 1/6/11 are non-overlapping. Prefer one of them", 10, y);
  tft.drawString("unless it is heavily congested.", 10, y + 10);
}

void drawCurrent() {
  switch (view) {
    case VIEW_GRAPH: drawGraph(); break;
    case VIEW_LIST:  drawList();  break;
    case VIEW_BEST:  drawBest();  break;
    default: break;
  }
}

void nextView() {
  view = (View)((view + 1) % VIEW_COUNT);
  listPage = 0;
  drawCurrent();
}

// ---------- 入力: XPT2046 自前ドライバ ----------
// 1チャンネル読む。cmd: X=0xD0 / Y=0x90 / Z1=0xB0 / Z2=0xC0 (XPT2046 の制御バイト)
uint16_t touchRead12(uint8_t cmd) {
  touchSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  touchSPI.transfer(cmd);
  uint8_t hi = touchSPI.transfer(0x00);
  uint8_t lo = touchSPI.transfer(0x00);
  digitalWrite(TOUCH_CS, HIGH);
  touchSPI.endTransaction();
  return ((uint16_t)((hi << 8) | lo) >> 3) & 0x0FFF;
}

int median7(int *v) {
  for (int i = 0; i < 6; i++)
    for (int j = i + 1; j < 7; j++)
      if (v[j] < v[i]) { int t = v[i]; v[i] = v[j]; v[j] = t; }
  return v[3];
}

// 生値を読む。押下判定は IRQ (PENIRQ) と圧力 Z の併用:
// IRQ は読み出し状態によっては LOW にならない個体/タイミングがあるため、
// Z (z1 + 4095 - z2。押すほど大きい) でも判定する。どちらかで押下扱い。
#define TOUCH_Z_MIN 2000  // Z だけで押下とみなすしきい値 (IRQ 併用の予備)


bool touchGetPoint(int &sx, int &sy, int &rawX, int &rawY, int &zOut, int &irqOut) {
  irqOut = digitalRead(TOUCH_IRQ);
  int z1 = touchRead12(0xB0);
  int z2 = touchRead12(0xC0);
  int z = z1 + 4095 - z2;
  zOut = z;
  bool pressed = (irqOut == LOW) || (z > TOUCH_Z_MIN);
  if (!pressed) return false;

  int xs[7], ys[7];
  for (int i = 0; i < 7; i++) {
    xs[i] = touchRead12(0xD0);
    ys[i] = touchRead12(0x90);
  }
  rawX = median7(xs);
  rawY = median7(ys);
  if (rawX < 80 || rawX > 4020 || rawY < 80 || rawY > 4020) return false; // 明らかなゴミ

  // 横向き (rotation=1) への変換:
  //   画面X は生Yから (通常方向)、画面Y は生Xから (反転)
  sx = (int)map(rawY, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, SCREEN_W - 1);
  sy = (int)map(rawX, TOUCH_RAW_X_MAX, TOUCH_RAW_X_MIN, 0, SCREEN_H - 1);
  sx = constrain(sx, 0, SCREEN_W - 1);
  sy = constrain(sy, 0, SCREEN_H - 1);
  return true;
}

bool touchWasDown = false;

void handleTouch() {
  int x, y, rawX, rawY, z, irq;
  bool down = touchGetPoint(x, y, rawX, rawY, z, irq);
  if (!down) { touchWasDown = false; return; }
  if (touchWasDown) return; // 押しっぱなしでは繰り返さない。離してから次を受け付ける
  touchWasDown = true;
  unsigned long now = millis();
  if (now - lastTouchMs < 350) return;
  lastTouchMs = now;
  // 押したときは必ず出すログ。何も出ない場合はこの版が動いていません
  Serial.printf("[touch] fw=%s irq=%d z=%d rawX=%d rawY=%d -> x=%d y=%d\n",
                FW_VERSION, irq, z, rawX, rawY, x, y);

  // ヘッダー右端: 即時再スキャン
  if (y < HEADER_H && x > SCREEN_W - 70) {
    doScan();
    drawCurrent();
    return;
  }
  // 一覧表示では左右端でページ送り
  if (view == VIEW_LIST) {
    int pages = max(1, (apCount + 8) / 9);
    if (x > SCREEN_W - 40 && listPage < pages - 1) { listPage++; drawList(); return; }
    if (x < 40 && listPage > 0)                   { listPage--; drawList(); return; }
  }
  nextView();
}

bool bootPrev = HIGH;
void handleBootButton() {
  bool now = digitalRead(BOOT_PIN);
  if (bootPrev == HIGH && now == LOW) nextView();
  bootPrev = now;
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n=== CYD WiFi Analyzer FW %s ===\n", FW_VERSION);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1); // 横向き 320x240
  // タッチは別バス (VSPI) で初期化。tft.setTouch() は使わない (上記コメント参照)
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);
  touchSPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  Serial.println("[touch] XPT2046 on VSPI: SCK=25 MISO=39 MOSI=32 CS=33 IRQ=36");
  Serial.printf("[touch] IRQ idle=%d (触っていなければ 1 が正常)\n", digitalRead(TOUCH_IRQ));
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.drawCentreString("WiFi Analyzer", SCREEN_W / 2, 90, 4);
  tft.setTextColor(COL_SUB, COL_BG);
  tft.drawCentreString("2.4GHz only / no connection is made", SCREEN_W / 2, 130, 2);
  tft.drawCentreString("FW " FW_VERSION, SCREEN_W / 2, 155, 2);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  doScan();
  drawCurrent();
}

void loop() {
  handleTouch();
  handleBootButton();
  if (!scanning && millis() - lastScanMs >= SCAN_INTERVAL_MS) {
    doScan();
    drawCurrent();
  }
  delay(20);
}
