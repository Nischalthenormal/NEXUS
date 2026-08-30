// =====================================================================
//  JEE TYPE DEFINITIONS -- deliberately ABOVE the #includes.
// ---------------------------------------------------------------------
//  immediately AFTER the last #include. Any prototype mentioning a JEE
//  type (e.g. "JeeDay &JeeToday();") is therefore emitted before anything
//  declared later in the file -- which produced:
//      error: 'JeeDay' does not name a type
//  Nothing is ever inserted above the FIRST #include, so defining the
//  types here makes the generated prototypes valid in every case.
//  Only <stdint.h> is needed; it is include-guarded and safe to pull in
//  before Arduino.h.
// =====================================================================
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------
//  MASCOT / RAGDOLL stick-figure type. Must live before the #include
//  block: the Arduino IDE injects function prototypes immediately after
//  the last #include, and those prototypes take Stick by reference.
// ---------------------------------------------------------------------
struct Stick {
  float x,y,vx,vy;
  float squash;        // 1 = neutral, <1 squashed, >1 stretched
  float lean;          // body tilt
  int   face;          // -1 left, 1 right
  bool  grounded;
  float anim;          // gait phase
};

// ---------------------------------------------------------------------
//  UI PHYSICS card type. Same reason as Stick above: the Arduino IDE
//  injects prototypes for CardInit/CardUpdate/CardGrab/CardRelease right
//  after the last #include, and it does this for `static` functions too,
//  so the type must already be visible at that point.
// ---------------------------------------------------------------------
struct PhysCard {
  float x,y,vx,vy;
  float hx,hy;                 // home / snap target
  bool  held;
  float grabX,grabY;
  float ang,angV;
};

// ---------------------------------------------------------------------
//  Launcher app descriptor. Must precede the #include block for the
//  same reason as Stick and PhysCard: DrawAppIcon() takes it by const
//  reference and the IDE prototypes that function up there.
// ---------------------------------------------------------------------
struct AppDef { const char *name; uint8_t target; uint8_t icon;
                uint8_t hue; uint8_t page; };

#define JEE_MAGIC   0x4A454531      // 'JEE1'
#define JEE_VER     1

// ---- firmware version: the ONE place the build number lives ----
#define NEXUS_VER      7
#define NEXUS_VER_STR  "V7"
#define JEE_TASKS   32
#define JEE_NOTES   16
#define JEE_GOALS   10
#define JEE_DAYS    120             // rolling history (~4 months)
#define JEE_TITLE   38
#define JEE_BODY    120
#define JEE_SAVE_MIN_MS 4000        // never write flash faster than this

enum { SUB_PHY=0, SUB_CHE, SUB_MAT, SUB_OTH, SUB_N };
static const char *SUB_NAME[SUB_N] = { "PHYSICS","CHEMISTRY","MATHS","OTHER" };
static const char *SUB_SHORT[SUB_N] = { "PHY","CHE","MAT","OTH" };

enum { PRI_LOW=0, PRI_MED, PRI_HIGH };
static const char *PRI_NAME[3] = { "LOW","MED","HIGH" };

struct JeeTask {
  char     title[JEE_TITLE];
  uint8_t  subject;      // SUB_*
  uint8_t  priority;     // PRI_*
  uint16_t estMin;       // estimated minutes
  uint8_t  done;
  uint8_t  used;         // slot occupied
  uint16_t doneDay;      // day index when completed
};
struct JeeNote {
  char     title[28];
  char     body[JEE_BODY];
  uint8_t  subject;
  uint8_t  pinned;
  uint8_t  used;
};
struct JeeGoal {
  char     title[JEE_TITLE];
  uint8_t  subject;
  uint8_t  kind;         // 0 daily 1 weekly 2 long
  uint16_t target;       // target units (min or count)
  uint16_t progress;
  uint8_t  isTime;       // 1 = minutes, 0 = count
  uint8_t  used;
};
struct JeeDay {
  uint16_t studyMin;     // minutes studied
  uint8_t  tasksDone;
  uint8_t  tasksTotal;
  uint16_t subMin[SUB_N];// per-subject minutes
};

// ---- the persistent blob ---------------------------------------------
struct JeeBlob {
  uint32_t magic, ver, crc;
  uint16_t targetMin;              // daily study target, minutes
  uint16_t focusMin, shortMin, longMin, cycles;
  uint16_t streak, bestStreak;
  uint32_t lastDayStamp;           // yyyymmdd of last roll
  uint16_t dayHead;                // ring index into day[]
  uint16_t totalMin;               // lifetime minutes (capped)
  uint8_t  lastSub, lastPri;
  uint8_t  pad0;
  JeeTask  task[JEE_TASKS];
  JeeNote  note[JEE_NOTES];
  JeeGoal  goal[JEE_GOALS];
  JeeDay   day[JEE_DAYS];
};


// =====================================================================
//   N E X U S   O S   v7   --  ESP32-S3 N16R8
// ---------------------------------------------------------------------
//   75 screens: 3D engine (15 solids / 13 render modes), 13 games,
//   JEE command center, 11 simulation modules, gesture recognition,
//   Creator Mode, math visualisers, animation lab, universal search,
//   achievements, hidden dev room, screensaver and easter eggs.
//
//   Display : Waveshare ST7789 320x240, SPI, WRITE-ONLY (no MISO).
//             Pins come from your TFT_eSPI User_Setup -- untouched.
//   Touch   : CST328 @ 0x1A, SDA=4 SCL=5 INT=6 RST=7, 400 kHz
//   Backlight: GPIO 3, hardware PWM
//   Storage : NVS flash -- calibration, theme, brightness, high scores
//
//   PERFORMANCE CONTRACT (target >= 20 FPS, 50 ms/frame)
//     A full-screen alpha blend costs ~76,800 blended pixels (~12 ms) and
//     is BANNED from the per-frame path. Dimming overlays are drawn as
//     coarse scanline bands, glows come from a small additive LUT sprite,
//     and only ONE shaded mesh is drawn per frame. Every screen was
//     budgeted against this before it was written.
//
//   All settings live on the device. There is no web server.
//
//   Requires: Arduino.h, Wire.h, TFT_eSPI.h, Preferences.h, WiFi.h
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#if __has_include(<esp_sleep.h>)
#include <esp_sleep.h>
#include <driver/gpio.h>
#endif


// =====================================================================
//  USER SETTINGS -- WiFi credentials are entered ON SCREEN (WIFI module),
//  but you can seed a default here to auto-connect at boot.
// =====================================================================
// ---------------------------------------------------------------------
//  WIFI CREDENTIALS  --  hardcoded by request.
//  NOTE: these are plaintext in the source. If you ever publish this
//  file or paste it somewhere public, change your network password
//  first. Boot always falls back to these if NVS holds nothing usable.
// ---------------------------------------------------------------------
#define DEF_SSID  "pRaNaG"
#define DEF_PASS  "Stupid$99"

#define TZ_OFFSET_SEC 19800          // IST = UTC +5:30
#define TZ_DST_SEC    0
#define NTP_1 "pool.ntp.org"
#define NTP_2 "time.google.com"

// =====================================================================
//  HARDWARE
// =====================================================================
#define TFT_BL          3
#define BL_PWM_CH       0
#define BL_PWM_FREQ     5000
#define BL_PWM_BITS     8

#define CST328_ADDR     0x1A
#define CST328_SDA_PIN  4
#define CST328_SCL_PIN  5
#define CST328_INT_PIN  6
#define CST328_RST_PIN  7
#define I2C_HZ          400000
#define REG_NUM         0xD005
#define REG_XY          0xD000

// NOTE: never use a bare identifier named BR / SAR / LBEG / LEND / PS / M0..M3
// here. The ESP32-S3 Xtensa header <xtensa/config/specreg.h> #defines those as
// register numbers, so a local variable called BR expands to "const int 4=3;".
#define SCREEN_W 320
#define SCREEN_H 240
#define FB_PIXELS (SCREEN_W * SCREEN_H)
#define FB_BYTES  (FB_PIXELS * 2)

#define TAU 6.28318531f
#define A_FILL 220
#define A_GLOW 120

// geometry pools (12 solids)
#define MAX_VERTS      2900
#define MAX_TRIS       4800
#define MAX_MESH_VERTS 700

#define NUM_PART   200
#define NUM_WARP   200
#define NUM_DUST    36
#define GRAPH_LEN   72
#define GLOW_R      11
#define GLOW_D      (GLOW_R * 2 + 1)

// =====================================================================
//  TYPES
// =====================================================================
struct Vert    { float x, y, z; };
struct Tri     { uint16_t a, b, c; uint8_t cr, cg, cb; };
struct SVert   { float sx, sy, invz, diff, spec, nx, ny, nz; };
struct Mesh    { Vert *pos; Vert *nrm; Tri *tri; uint16_t nv, nt; };
struct Spring  { float v, vel; };
struct Particle{ float x, y, vx, vy, life, inv; uint16_t col; uint8_t kind; };
struct Dust    { float x, y, z, ph; };
struct WarpStar{ float x, y, z; };

struct Theme {
  const char *name;
  uint16_t bg, panel, accent, hilite, hair, data, text, warn, sand;
  uint16_t spec[6];
  // ---- V8 semantic roles ----------------------------------------
  // The legacy fields above stay so no existing screen breaks. These
  // add the named roles the design system actually reasons about.
  uint16_t surface;    // elevated surface (cards sit on this)
  uint16_t surface2;   // pressed / selected surface
  uint16_t line;       // hairline divider, 1px
  uint16_t textDim;    // secondary text
  uint16_t accent2;    // secondary accent, used sparingly
  uint16_t ok;         // success
  uint16_t err;        // error
  uint16_t off;        // disabled
};


// ---- easter-egg identifiers, hoisted so earlier screens can trigger them
enum {
  EGG_NONE=0, EGG_CHAOS, EGG_SINGULARITY, EGG_GLITCH, EGG_MIRROR,
  EGG_RETRO, EGG_RAIN, EGG_MASCOT, EGG_TERMINAL, EGG_DEVMODE,
  EGG_SUPERNOVA, EGG_COUNT
};

// ---- forward declarations -------------------------------------------
bool  I2C_Read(uint8_t a, uint16_t r, uint8_t *d, uint32_t n);
void  CST328_Reset(void);
bool  ReadRawTouch(uint16_t &x, uint16_t &y);
bool  GetTouch(int &x, int &y);
extern bool rawValid;
extern uint16_t rawTX, rawTY;
void  CalibrateTouch(float rx, float ry, int &ox, int &oy);
void  PollTouch(void);
bool  AllocBuffers(void);
void  BuildAll(void);
void  PushFrame(void);
void  DisplaySleep(void);
void  DisplayWake(void);
void  DisplayToggle(void);
extern bool displaySleeping;
bool  DmaBandsAlloc(void);
void  DmaReport(void);
void  PxBlend(int x, int y, uint16_t c, uint8_t a);
void  PxAdd(int x, int y, uint16_t c, uint8_t amt);
void  FillRectFB(int x, int y, int w, int h, uint16_t c);
void  BlendRectFB(int x, int y, int w, int h, uint16_t c, uint8_t a);
void  HLineFB(int x, int y, int w, uint16_t c);
void  VLineFB(int x, int y, int h, uint16_t c);
void  LineFB(int x0, int y0, int x1, int y1, uint16_t c, uint8_t a);
void  LineAdd(int x0, int y0, int x1, int y1, uint16_t c, uint8_t amt);
void  RingFB(int cx, int cy, int r, uint16_t c, uint8_t a);
void  ArcFB(int cx, int cy, int r, float a0, float a1, uint16_t c, uint8_t a);
void  CircleFB(int cx, int cy, int r, uint16_t c, uint8_t a);
void  HexFB(int cx, int cy, int r, uint16_t c, uint8_t a, bool fill);
void  Glow(int cx, int cy, uint16_t c, uint8_t amt, float sc);
void  Scrim(uint8_t strength);
void  BuildGlowLUT(void);
void  DrawChar(int x, int y, char ch, uint16_t c, uint8_t s);
void  DrawText(int x, int y, const char *t, uint16_t c, uint8_t s);
void  DrawTextC(int cx, int y, const char *t, uint16_t c, uint8_t s);
void  GlowText(int x, int y, const char *t, uint16_t c, uint8_t s, uint8_t amt);
void  GlowTextC(int cx, int y, const char *t, uint16_t c, uint8_t s, uint8_t amt);
void  DrawTextDecode(int x, int y, const char *t, uint16_t c, uint8_t s, float p);
int   TextW(const char *t, uint8_t s);
void  Panel(int x, int y, int w, int h, const char *title, uint16_t ac, const char *tag);
void  Bracket(int x, int y, int w, int h, uint16_t c, int len);
bool  Button(int x, int y, int w, int h, const char *label, uint16_t c, bool on);
void  Graph(int x, int y, int w, int h, const uint8_t *hist, int head, uint16_t c);
void  SpawnBurst(float x, float y, int n, uint16_t c, float sp, uint8_t k);
void  UpdateParticles(float dt);
void  DrawParticles(void);
void  Backdrop(void);
void  RasterTriangle(const SVert &a, const SVert &b, const SVert &c,
                     uint8_t r, uint8_t g, uint8_t bl);
void  RasterAdd(const SVert &a, const SVert &b, const SVert &c, uint16_t col, uint8_t amt);
void  RenderMesh(const Mesh &m, float rx, float ry, float rz, float sc,
                 float ox, float oy, float cz, uint8_t mode, uint16_t tint);
void  UpdateOrbit(float dt);
void  TopBar(const char *title, uint16_t ac);
bool  BackHit(void);
void  GoTo(int target, float fx, float fy, uint16_t col, int mode);
void  BootSequence(void);
void  TransitionDraw(float t);
void  SetBrightness(uint8_t v);
void  BacklightAttach(void);
void  ApplyTheme(int i);
void  SaveCalibration(void);
void  LoadSettings(void);
void  SaveSettings(void);
void  ResetCalibration(void);
void  SaveScores(void);
void  NetBegin(const char *ssid, const char *pass);
void  NetLoop(void);
// ---- V8 UI system ----
void  IconV8(uint8_t g,int cx,int cy,uint16_t c,int s);
void  IconPack(uint8_t id,int cx,int cy,int r,uint16_t c,int w);
void  UiRect(int x,int y,int w,int h,uint16_t c,uint8_t a);
void  UiOutline(int x,int y,int w,int h,uint16_t c,uint8_t a);
void  UiDivider(int x,int y,int w);
void  UiScrim(uint8_t strength);
bool  UiHeader(const char *title,const char *context);
void  UiFooter(const char *left,const char *right);
void  UiCard(int x,int y,int w,int h,int state,uint16_t accent);
bool  UiButton(int x,int y,int w,int h,const char *label,int state,int id);
bool  UiToggle(int x,int y,const char *label,bool on);
bool  UiSlider(int x,int y,int w,const char *label,float *val,const char *unit);
bool  UiListItem(int x,int y,int w,int h,uint8_t glyph,const char *title,
                 const char *sub,const char *value,bool selected,uint16_t accent);
void  UiProgress(int x,int y,int w,float frac,uint16_t c);
void  UiStat(int x,int y,const char *label,const char *value,uint16_t vc);
void  UiModalOpen(const char *title,const char *body,int kind);
int   UiModalDraw(float dt);
void  UiToast(const char *msg,uint16_t c);
void  UiToastDraw(float dt);
void  UiPageDots(int cx,int y,int n,int active);
void  UiEmpty(const char *title,const char *hint,uint8_t glyph);
void  UiError(const char *what,const char *detail);
void  UiLabel(int x,int y,const char *t);
void  UiValue(int x,int y,const char *t);
void  UiData(int x,int y,const char *t);
void  UiDataR(int right,int y,const char *t);
void  UiTitle(int x,int y,const char *t);
void  ScreenHomeV8(float dt);
void  DrawAppIcon(int cx,int cy,const AppDef &a,float press,float alpha);
void  LogoMark(uint8_t id,int cx,int cy,int u,uint16_t ink,uint16_t base);
// ---- V8 mobile shell ----
void  IconV8(uint8_t g,int cx,int cy,uint16_t c,int s);
bool  ShellPreFrame(float dt);
void  ShellPostFrame(float dt);
void  ShellGoHome(void);
void  ShellOpenApp(int appIdx,float fx,float fy);
void  RecentsInit(void);
void  SheetOpen(const char *title,const char **items,const uint8_t *icons,int n);
int   SheetDraw(float dt);
void  ContextOpen(int appIdx,float x,float y);
int   ContextDraw(float dt);
void  DrawStatusBar(bool overApp);
void  DrawHomeIndicator(float glow);
void  DrawHomeScreen(float dt);
void  DrawAppSwitcher(float dt);
void  DrawControlCenter(float p,float dt);
void  DrawLockScreen(float dt);
void  DrawAppOpenAnim(float p,bool opening);
void  DrawPageDots(int cx,int y,int n,float pos);
void  DrawDock(float alpha);
void  ShRect(int x,int y,int w,int h,uint16_t c,uint8_t a);
void  ShRectR(int x,int y,int w,int h,int r,uint16_t c,uint8_t a);
void  ShShadow(int x,int y,int w,int h);
void  ShOutline(int x,int y,int w,int h,uint16_t c,uint8_t a);
void  ShScrim(uint8_t s);
bool  ShellGestures(float dt);
void  ShellGestureOverlay(void);
void  SetBrightness(uint8_t v);
void  ApplyTheme(int i);
// ---- v7 forward declarations ----
void  KbCommitEq(const char *t);
void  KbCommitSearch(const char *t);
void  AchGrant(int id);
void  AchVisit(int st);
void  AchTick(float dt);
void  AchToast(void);
void  AchLoad(void);
void  AchSave(void);
void  ImpactTick(float dt);
void  Impact(float mag);
void  BulletReset(void);
void  SaverEnter(void);
void  CreatorLoad(void);
void  GestClear(void);
void  SearchRun2(void);
void  ScreenSearch(float dt);      void  ScreenCreator(float dt);
void  ScreenGesture(float dt);     void  ScreenMorph(float dt);
void  ScreenKaleido(float dt);     void  ScreenExplode(float dt);
void  ScreenVoxel(float dt);       void  ScreenImpossible(float dt);
void  ScreenTunnel(float dt);      void  ScreenGravWell(float dt);
void  ScreenBoids(float dt);       void  ScreenAquarium(float dt);
void  ScreenAnts(float dt);        void  ScreenCharges(float dt);
void  ScreenRagdoll(float dt);     void  ScreenTimeline(float dt);
void  ScreenGrapher(float dt);     void  ScreenParametric(float dt);
void  ScreenSurface(float dt);     void  ScreenVectors(float dt);
void  ScreenMatrixViz(float dt);   void  ScreenFourier(float dt);
void  ScreenClocks(float dt);      void  ScreenAchieve(float dt);
void  ScreenDevRoom(float dt);     void  ScreenSaver(float dt);
void  TimelineReset(void);
extern uint32_t ssIdleMs;
// ---- JEE ----
void  JeeInit(void);
void  JeeSave(void);
void  JeeLoad(void);
void  JeeTick(float dt);
void  JeeRollDay(void);
void  ScreenJee(float dt);
void  ScreenJeeTimer(float dt);
void  ScreenJeeTasks(float dt);
void  ScreenJeeGoals(float dt);
void  ScreenJeeStats(float dt);
void  ScreenJeeHist(float dt);
void  ScreenJeeNotes(float dt);
void  ScreenJeeQuote(float dt);
void  ScreenJeeSet(float dt);
void  ScreenKeyboard(float dt);
void  KbOpen(const char *title,const char *init,int maxLen,int ret,int purpose);
bool  JeeBar(const char *title,uint16_t ac);
// ---- v5 screens ----
void  ScreenPhysics(float dt);
void  ScreenSand(float dt);
void  ScreenSpace(float dt);
void  ScreenPlanetGen(float dt);
void  ScreenFractal(float dt);
void  ScreenMatrix(float dt);
void  ScreenField(float dt);
void  ScreenTouchPlay(float dt);
void  ScreenLife(float dt);
void  ScreenDemo(float dt);
void  ScreenMolecule(float dt);
void  ScreenAnimLab(float dt);
void  EggFire(uint8_t id, float dur, const char *msg);
extern uint16_t eggFound;
void  DrawMolecule(float t,int cx,int cy,float sc,bool dna);
extern JeeBlob *JB;             // full definition is above
uint32_t JeeTodayMin(void);
float    JeeProgress(void);
uint16_t JeeStreak(void);
// ---- JEE ----
void  JeeInit(void);
void  JeeTick(float dt);
void  JeeSave(void);
void  JeeLoad(void);
void  JeeRollDay(void);
void  ScreenJeeHome(float dt);
void  ScreenJeeTimer(float dt);
void  ScreenJeeTasks(float dt);
void  ScreenJeeGoals(float dt);
void  ScreenJeeStats(float dt);
void  ScreenJeeHist(float dt);
void  ScreenJeeNotes(float dt);
void  ScreenJeeQuote(float dt);
void  ScreenJeeSettings(float dt);
void  ScreenKeyboard(float dt);
void  KbOpen(const char *title,const char *initial,int maxLen,int retState,int purpose);
bool  JeeNavBar(const char *title);

// =====================================================================
//  PALETTE -- 4 curated themes, each with a designed 6-hue spectrum.
//  Hues rotate widely while VALUE stays in one band, so colour reads as
//  intentional rather than as confetti.
// =====================================================================
static inline uint16_t RGB565(int r, int g, int b) {
  if (r > 255) r = 255;
  if (r < 0) r = 0;
  if (g > 255) g = 255;
  if (g < 0) g = 0;
  if (b > 255) b = 255;
  if (b < 0) b = 0;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#define NUM_THEMES 5
static const Theme THEMES[NUM_THEMES] = {
  // =================================================================
  //  V8 PALETTE  --  built like a mobile OS tonal system, not a HUD.
  //
  //  Construction rules (why these numbers and not others):
  //   * Surfaces are NEUTRAL and step in even luminance increments:
  //     bg 8% -> surface 12% -> surface2 17% -> line 24%. That even
  //     ladder is what makes elevation read without borders or glow.
  //   * Surfaces carry a faint tint of the accent hue (2-4 points of
  //     one channel). Pure grey looks cheap; tinted grey looks
  //     designed. This is the single biggest "premium" lever.
  //   * Text is never pure white. #F2F3F5-ish at 95% reads as ink;
  //     #FFFFFF glares on an ST7789 and looks like a terminal.
  //   * Exactly ONE accent per theme. Secondary accent is reserved for
  //     data, success/error only for state. No decorative colour.
  //   * The 6-hue spec[] is a muted, desaturated family that shares
  //     one value band -- app-icon tints, never rainbow chrome.
  // =================================================================
  { "NEXUS",                                    // deep slate + azure
    RGB565( 16, 18, 23), RGB565( 24, 27, 34), RGB565( 74,144,246),
    RGB565(150,190,255), RGB565( 52, 58, 70), RGB565(122,182,236),
    RGB565(238,240,244), RGB565(240,111, 92), RGB565(146,154,170),
    { RGB565( 74,144,246), RGB565(122,182,236), RGB565(126,196,178),
      RGB565(232,168, 96), RGB565(226,124,132), RGB565(158,142,224) },
    RGB565( 24, 27, 34), RGB565( 34, 38, 47), RGB565( 52, 58, 70),
    RGB565(146,154,170), RGB565(122,182,236), RGB565(102,196,140),
    RGB565(240,111, 92), RGB565( 78, 84, 96) },
  { "GRAPHITE",                                 // neutral mono, precise
    RGB565( 18, 18, 19), RGB565( 27, 27, 29), RGB565(236,238,242),
    RGB565(255,255,255), RGB565( 56, 57, 60), RGB565(176,180,188),
    RGB565(240,241,244), RGB565(238,120, 96), RGB565(142,145,152),
    { RGB565(196,200,208), RGB565(182,186,196), RGB565(170,174,184),
      RGB565(190,178,158), RGB565(176,180,188), RGB565(162,166,176) },
    RGB565( 27, 27, 29), RGB565( 38, 38, 41), RGB565( 56, 57, 60),
    RGB565(142,145,152), RGB565(176,180,188), RGB565(120,192,144),
    RGB565(238,120, 96), RGB565( 82, 84, 88) },
  { "MIDNIGHT",                                 // indigo, high-end dark
    RGB565( 14, 16, 26), RGB565( 22, 25, 38), RGB565(126,134,248),
    RGB565(182,188,255), RGB565( 48, 54, 78), RGB565(134,168,232),
    RGB565(234,236,246), RGB565(236,110,130), RGB565(140,148,176),
    { RGB565(126,134,248), RGB565(134,168,232), RGB565(150,146,222),
      RGB565(196,150,232), RGB565(120,178,206), RGB565(166,158,238) },
    RGB565( 22, 25, 38), RGB565( 32, 36, 52), RGB565( 48, 54, 78),
    RGB565(140,148,176), RGB565(134,168,232), RGB565(108,198,166),
    RGB565(236,110,130), RGB565( 72, 78, 98) },
  { "AMBER",                                    // warm graphite + gold
    RGB565( 20, 18, 16), RGB565( 29, 26, 23), RGB565(240,168, 78),
    RGB565(255,212,152), RGB565( 60, 54, 47), RGB565(198,174,142),
    RGB565(244,240,234), RGB565(234,108, 78), RGB565(158,148,134),
    { RGB565(240,168, 78), RGB565(224,146, 92), RGB565(206,168,110),
      RGB565(198,174,142), RGB565(178,158,126), RGB565(216,132, 96) },
    RGB565( 29, 26, 23), RGB565( 40, 36, 31), RGB565( 60, 54, 47),
    RGB565(158,148,134), RGB565(198,174,142), RGB565(150,186,124),
    RGB565(234,108, 78), RGB565( 88, 82, 74) },
  { "MINT",                                     // cool teal, calm
    RGB565( 14, 20, 21), RGB565( 21, 30, 32), RGB565( 76,204,182),
    RGB565(154,238,222), RGB565( 46, 62, 66), RGB565(126,186,196),
    RGB565(232,240,240), RGB565(238,116, 98), RGB565(138,158,162),
    { RGB565( 76,204,182), RGB565(126,186,196), RGB565(112,196,158),
      RGB565(176,204,140), RGB565(226,168,110), RGB565(140,170,214) },
    RGB565( 21, 30, 32), RGB565( 30, 42, 45), RGB565( 46, 62, 66),
    RGB565(138,158,162), RGB565(126,186,196), RGB565( 98,200,150),
    RGB565(238,116, 98), RGB565( 70, 86, 90) }
};

int   gTheme = 0;
Theme TH;
#define C_BG     TH.bg
#define C_PANEL  TH.panel
#define C_ACCENT TH.accent
#define C_HILITE TH.hilite
#define C_HAIR   TH.hair
#define C_DATA   TH.data
#define C_TEXT   TH.text
#define C_WARN   TH.warn
#define C_SAND   TH.sand

// ---- V8 semantic colour roles ----------------------------------------
#define C_SURFACE  TH.surface
#define C_SURFACE2 TH.surface2
#define C_LINE     TH.line
#define C_DIM      TH.textDim
#define C_ACCENT2  TH.accent2
#define C_OK       TH.ok
#define C_ERR      TH.err
#define C_OFF      TH.off

// =====================================================================
//  V8 LAYOUT GRID
//  Every screen composes from these. No arbitrary coordinates.
//  320x240, 8 px base unit.
// =====================================================================
#define U            4              // half unit
#define U2           8              // base unit
#define GUTTER       12             // screen side margin
#define SAFE_TOP     16             // shell status-bar band (owned by the shell)
#define SAFE_BOTTOM  14             // home-indicator band
#define HEADER_H     26             // top chrome
#define FOOTER_H     18             // bottom status strip
#define CONTENT_TOP  22               // just below the 18 px app header
#define CONTENT_BOT  (SCREEN_H - FOOTER_H - U)
#define CONTENT_H    (CONTENT_BOT - CONTENT_TOP)
#define CONTENT_W    (SCREEN_W - GUTTER*2)
#define CARD_GAP     6
#define CARD_PAD     8
#define RADIUS       3              // corner inset, in px
#define TAP_MIN      30             // minimum comfortable touch target
// type scale (multiplier passed to DrawText)
#define T_DISPLAY    3
#define T_TITLE      2
#define T_BODY       1
#define T_SMALL      1
// alpha discipline -- only these three, plus A_FILL/A_GLOW for legacy
#define A_SURF       236            // card fill
#define A_HAIR       90             // divider
#define A_PRESS      255
static inline uint16_t Spec(int i) { return TH.spec[((i % 6) + 6) % 6]; }
void ApplyTheme(int i) {
  if (i < 0) i = 0;
  if (i >= NUM_THEMES) i = NUM_THEMES - 1;
  gTheme = i; TH = THEMES[i];
}

// JEE colour helpers live here because they depend on the palette macros
// defined just above (Spec / C_WARN / C_ACCENT / C_SAND).
inline uint16_t SubCol(int s){ return Spec(s==0?4:(s==1?3:(s==2?0:5))); }
inline uint16_t PriCol(int p){ return p==PRI_HIGH?C_WARN:(p==PRI_MED?C_ACCENT:C_SAND); }

// =====================================================================
//  GLOBALS
// =====================================================================
TFT_eSPI tft = TFT_eSPI();

uint16_t *fb[2] = { nullptr, nullptr };
uint16_t *depth = nullptr, *accum = nullptr, *canvas = nullptr, *frame = nullptr;
uint8_t   fbIndex = 0;
bool      gDepthInternal = false;
uint8_t  *glowLUT = nullptr;

Vert  *vPos = nullptr, *vNrm = nullptr;
Tri   *tArr = nullptr;
SVert *gSV  = nullptr;
uint16_t vTop = 0, tTop = 0;

#define NUM_OBJ 15
Mesh gMesh[NUM_OBJ];
Mesh meshOcta;
static const char *OBJ_NAME[NUM_OBJ] = {
  "PLANET","TORUS","CUBE","ICOSA","CRYSTAL","SHIP",
  "KNOT","SPIKE","GEAR","CYLINDER","CONE","HELIX",
  "RINGWORLD","PYRAMID","MOBIUS" };

#define NUM_MODES 13
static const char *MODE_NAME[NUM_MODES] = {
  "SOLID","WIREFRAME","FLAT SHADE","SMOOTH","X-RAY","POINTS","NEON EDGE","NORMALS",
  "EDGE","DEPTH","DITHERED","HOLOGRAM","VOXEL" };
enum { M_SOLID = 0, M_WIRE, M_FLAT, M_SMOOTH, M_XRAY, M_POINTS, M_NEON, M_NORMALS,
       M_EDGE, M_DEPTH, M_DITHER, M_HOLO, M_VOXEL };
// Per-object preferred style, so the gallery never renders everything the
// same way. -1 means "use whatever the user selected".
static const int8_t OBJ_STYLE[NUM_OBJ] = {
  M_SMOOTH,   // planet   : smooth + atmosphere
  M_WIRE,     // torus    : wireframe
  M_SOLID,    // cube     : solid metallic
  M_FLAT,     // icosa    : flat low-poly
  M_XRAY,     // crystal  : faceted translucent
  M_NEON,     // ship     : neon edges
  M_SMOOTH,   // knot
  M_FLAT,     // spike
  M_SOLID,    // gear
  M_DEPTH,    // cylinder
  M_DITHER,   // cone
  M_NEON,     // helix
  M_SMOOTH,   // ringworld: smooth + ring
  M_EDGE,     // pyramid
  M_POINTS    // mobius
};


Particle *parts = nullptr;
Dust     *dust  = nullptr;
WarpStar *warp  = nullptr;
int       partHead = 0;
enum { PK_SPARK = 0, PK_EMBER };

uint8_t histFps[GRAPH_LEN], histFrame[GRAPH_LEN], histLoad[GRAPH_LEN];
int     histHead = 0;

// ---- app states ------------------------------------------------------
enum AppState {
  ST_HOME = 0,
  ST_LAB, ST_OBJECTS, ST_MODES, ST_INSPECT, ST_WARP, ST_MAZE,
  ST_2048, ST_BREAK, ST_FLAPPY, ST_SNAKE, ST_PONG, ST_TETRIS,
  ST_MEMORY, ST_SIMON, ST_MINES, ST_WHACK, ST_DODGE, ST_LIGHTS,
  ST_DRAW, ST_CLOCK, ST_STOPW, ST_TIMER, ST_WIFI, ST_SETTINGS,
  ST_CALIB, ST_SYSTEM,
  // ---- JEE COMMAND CENTER ----
  ST_JEE, ST_JTIMER, ST_JTASKS, ST_JGOALS, ST_JSTATS, ST_JHIST,
  ST_JNOTES, ST_JQUOTE, ST_JSET, ST_KBD,
  // ---- v5 GRAPHICS ENVIRONMENT ----
  ST_PHYS, ST_PSAND, ST_SPACE, ST_PLANETGEN, ST_FRACTAL,
  ST_MATRIX, ST_FIELD, ST_TOUCHPLAY, ST_LIFE, ST_DEMO,
  ST_MOLECULE, ST_ANIMLAB,
  // ---- v7 EXPANSION ----
  ST_SEARCH, ST_CREATOR, ST_GESTURE, ST_MORPH, ST_KALEIDO, ST_EXPLODE,
  ST_VOXEL, ST_IMPOSSIBLE, ST_TUNNEL, ST_GRAVWELL, ST_BOIDS, ST_AQUARIUM,
  ST_ANTS, ST_CHARGES, ST_RAGDOLL, ST_TIMELINE, ST_GRAPHER, ST_PARAMETRIC,
  ST_SURFACE, ST_VECTORS, ST_MATRIXVIZ, ST_FOURIER, ST_CLOCKS, ST_ACHIEVE,
  ST_DEVROOM, ST_SAVER,
  ST_COUNT
};
AppState appState = ST_HOME;

float gTime = 0.0f, enterAnim = 0.0f;
float transT = 0.0f;
int   transTarget = 0, transMode = 0;
float transX = 160, transY = 120;
uint16_t transCol;
enum { TR_SHOCK = 0, TR_HEX, TR_GLITCH, TR_IRIS };

int   gObj = 0, gMode = M_SMOOTH;
float gVoxSize = 3.2f;              // voxel edge scale for M_VOXEL
uint8_t gBright = 200;
uint8_t gGlowLevel = 2;                 // 0 off, 1 soft, 2 full
uint8_t gFxLevel = 2;                   // particle density
bool    gShowFps = true;

float rotX = 0.35f, rotY = 0.0f, rotZ = 0.0f;
float velX = 0.0f, velY = 0.35f, dragVX = 0, dragVY = 0;
float sLight = 0.35f, sCam = 0.42f, sSpin = 0.40f;

bool  touchActive = false, touchDown = false, touchUp = false;
int   touchX = 0, touchY = 0, lastTX = 0, lastTY = 0, pressX = 0, pressY = 0;
float smoothTX = 0, smoothTY = 0;
uint32_t lastTouchMs = 0;

float warpSpeed = 1.2f, warpTarget = 1.2f, warpTilt = 0;

uint32_t frameCount = 0, fpsTimer = 0;
float    fpsValue = 0, frameMs = 0;
char     fpsStr[12] = "20";
bool     gLowDetail = false;

float LX = -0.577f, LY = -0.577f, LZ = -0.577f;

// BACK hitbox. Was 74x34, which overlapped 17 app buttons drawn at
// y=22..29 -- tapping them closed the app instead. Now sized to the
// chevron actually drawn by TopBar (x 6..20, y SAFE_TOP+2..SAFE_TOP+16)
// plus a comfortable margin, while still being a large target.
#define BACK_W 30
#define BACK_H 34

Preferences prefs;
#define NVS_NS "nexusos"
#define CAL_MAGIC 0x43414C34

// calibration coefficients (runtime, persisted)
float CAL_A =  0.973913038f, CAL_B = -0.031239455f, CAL_C =   7.33205278f;
float CAL_D = -0.046093319f, CAL_E = -0.903280217f, CAL_F = 235.615557f;
float EDGE_GX = 1.04f, EDGE_GY = 1.18f;
bool  calLoaded = false;
#define TOUCH_DEBUG 0

// high scores
uint16_t hs2048 = 0, hsBreak = 0, hsFlappy = 0, hsSnake = 0, hsPong = 0;
uint16_t hsTetris = 0, hsMemory = 0, hsSimon = 0, hsDodge = 0, hsWhack = 0;
uint16_t hsMaze = 0;

// network
bool netUp = false, timeOk = false;
char ipStr[20] = "OFFLINE";
char wifiSsid[34] = DEF_SSID;
char wifiPass[66] = DEF_PASS;

// =====================================================================
//  FONT 5x7
// =====================================================================
static const uint8_t FONT5x7[72][5] PROGMEM = {
  {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
  {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
  {0x06,0x49,0x49,0x29,0x1E},
  {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
  {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
  {0x00,0x36,0x36,0x00,0x00},  // 36 :
  {0x00,0x60,0x60,0x00,0x00},  // 37 .
  {0x08,0x08,0x08,0x08,0x08},  // 38 -
  {0x00,0x41,0x22,0x14,0x08},  // 39 >
  {0x08,0x14,0x22,0x41,0x00},  // 40 <
  {0x20,0x10,0x08,0x04,0x02},  // 41 /
  {0x08,0x08,0x3E,0x08,0x08},  // 42 +
  {0x00,0x00,0x5F,0x00,0x00},  // 43 !
  {0x02,0x01,0x51,0x09,0x06},  // 44 ?
  {0x00,0x00,0x00,0x00,0x00},  // 45 space
  // ---- 46..71 : lowercase a-z, 5x7, x-height 5 ----
  {0x20,0x54,0x54,0x54,0x78},  // a
  {0x7F,0x48,0x44,0x44,0x38},  // b
  {0x38,0x44,0x44,0x44,0x20},  // c
  {0x38,0x44,0x44,0x48,0x7F},  // d
  {0x38,0x54,0x54,0x54,0x18},  // e
  {0x08,0x7E,0x09,0x01,0x02},  // f
  {0x0C,0x52,0x52,0x52,0x3E},  // g
  {0x7F,0x08,0x04,0x04,0x78},  // h
  {0x00,0x44,0x7D,0x40,0x00},  // i
  {0x20,0x40,0x44,0x3D,0x00},  // j
  {0x7F,0x10,0x28,0x44,0x00},  // k
  {0x00,0x41,0x7F,0x40,0x00},  // l
  {0x7C,0x04,0x18,0x04,0x78},  // m
  {0x7C,0x08,0x04,0x04,0x78},  // n
  {0x38,0x44,0x44,0x44,0x38},  // o
  {0x7C,0x14,0x14,0x14,0x08},  // p
  {0x08,0x14,0x14,0x18,0x7C},  // q
  {0x7C,0x08,0x04,0x04,0x08},  // r
  {0x48,0x54,0x54,0x54,0x20},  // s
  {0x04,0x3F,0x44,0x40,0x20},  // t
  {0x3C,0x40,0x40,0x20,0x7C},  // u
  {0x1C,0x20,0x40,0x20,0x1C},  // v
  {0x3C,0x40,0x30,0x40,0x3C},  // w
  {0x44,0x28,0x10,0x28,0x44},  // x
  {0x0C,0x50,0x50,0x50,0x3C},  // y
  {0x44,0x64,0x54,0x4C,0x44}   // z
};
static inline int FontIndex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 46 + (c - 'a');   // real lowercase
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  switch (c) {
    case ':': return 36;  case '.': return 37;  case '-': return 38;
    case '>': return 39;  case '<': return 40;  case '/': return 41;
    case '+': return 42;  case '!': return 43;  case '?': return 44;
    default:  return -1;
  }
}

// =====================================================================
//  MATH + MOTION
// =====================================================================
#define TRIG_SIZE 1024
static float sinTab[TRIG_SIZE];
static inline float fsin(float a) {
  int i = (int)(a * (TRIG_SIZE / TAU));
  return sinTab[((i % TRIG_SIZE) + TRIG_SIZE) & (TRIG_SIZE - 1)];
}
static inline float fcos(float a) {
  int i = (int)(a * (TRIG_SIZE / TAU)) + (TRIG_SIZE / 4);
  return sinTab[((i % TRIG_SIZE) + TRIG_SIZE) & (TRIG_SIZE - 1)];
}
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline uint16_t Dim(uint16_t c, uint8_t n, uint8_t d) {
  uint16_t r = ((c >> 11) & 0x1F) * n / d;
  uint16_t g = ((c >> 5) & 0x3F) * n / d;
  uint16_t b = (c & 0x1F) * n / d;
  return (r << 11) | (g << 5) | b;
}
static inline uint16_t Fade(uint16_t c, uint8_t a) {
  uint16_t r = (((c >> 11) & 0x1F) * a) >> 8;
  uint16_t g = (((c >> 5) & 0x3F) * a) >> 8;
  uint16_t b = ((c & 0x1F) * a) >> 8;
  return (r << 11) | (g << 5) | b;
}
static inline float Hash(uint32_t n) {
  n = (n << 13) ^ n;
  n = n * (n * n * 15731u + 789221u) + 1376312589u;
  return (float)(n & 0x7FFFFFFF) / 2147483647.0f;
}
static inline void SpringTo(Spring &s, float t, float stiff, float damp, float dt) {
  if (dt > 0.05f) dt = 0.05f;
  s.vel += (t - s.v) * stiff * dt;
  s.vel *= expf(-damp * dt);
  s.v += s.vel * dt;
}
static inline float Approach(float c, float t, float rate, float dt) {
  return t + (c - t) * expf(-rate * dt);
}
static inline float EaseOutCubic(float t) { float u = 1 - t; return 1 - u*u*u; }
static inline float EaseInCubic(float t)  { return t*t*t; }
static inline float EaseOutQuint(float t) { float u = 1 - t; return 1 - u*u*u*u*u; }
static inline float EaseInOutCubic(float t) {
  return (t < 0.5f) ? (4*t*t*t) : (1 - powf(-2*t + 2, 3) * 0.5f);
}
static inline float EaseOutBack(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1;
  float u = t - 1;
  return 1 + c3*u*u*u + c1*u*u;
}
static inline float EaseAnticipate(float t) {
  if (t < 0.3f) { float u = t / 0.3f; return -0.12f * fsin(u * 3.14159f); }
  return EaseOutBack((t - 0.3f) / 0.7f);
}
static inline float Pulse(float t, float sp) {
  float v = 0.5f + 0.5f * fsin(t * sp);
  return v * v * (3 - 2 * v);
}
static inline float Stagger(float anim, int i, float d, float span) {
  return EaseOutCubic(clampf((anim - i * d) / span, 0, 1));
}

// motion clock: median-filtered dt + fixed-step physics
#define FIXED_DT (1.0f / 120.0f)
static float dtHist[3] = { 0.02f, 0.02f, 0.02f };
static int   dtIdx = 0;
static float dtSmooth = 0.02f, animAccum = 0;
static float FilterDt(float raw) {
  raw = clampf(raw, 0.002f, 0.120f);
  dtHist[dtIdx] = raw; dtIdx = (dtIdx + 1) % 3;
  float a = dtHist[0], b = dtHist[1], c = dtHist[2];
  float med = fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
  dtSmooth = dtSmooth * 0.82f + med * 0.18f;
  return dtSmooth;
}

// =====================================================================
//  NVS  --  calibration, settings, high scores
// =====================================================================
void SaveCalibration(void) {
  prefs.begin(NVS_NS, false);
  prefs.putUInt("magic", CAL_MAGIC);
  prefs.putFloat("A", CAL_A); prefs.putFloat("B", CAL_B); prefs.putFloat("C", CAL_C);
  prefs.putFloat("D", CAL_D); prefs.putFloat("E", CAL_E); prefs.putFloat("F", CAL_F);
  prefs.putFloat("GX", EDGE_GX); prefs.putFloat("GY", EDGE_GY);
  prefs.end();
  calLoaded = true;
  Serial.printf("CAL saved: X=%.6f,%.6f,%.3f  Y=%.6f,%.6f,%.3f\n",
                CAL_A, CAL_B, CAL_C, CAL_D, CAL_E, CAL_F);
}
void SaveSettings(void) {
  prefs.begin(NVS_NS, false);
  prefs.putUChar("bright", gBright);
  prefs.putUChar("theme", (uint8_t)gTheme);
  prefs.putUChar("glow", gGlowLevel);
  prefs.putUChar("fx", gFxLevel);
  prefs.putUChar("fps", gShowFps ? 1 : 0);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
}
void SaveScores(void) {
  prefs.begin(NVS_NS, false);
  prefs.putUShort("s2048", hs2048);   prefs.putUShort("sbrk", hsBreak);
  prefs.putUShort("sfl", hsFlappy);   prefs.putUShort("ssn", hsSnake);
  prefs.putUShort("spo", hsPong);     prefs.putUShort("ste", hsTetris);
  prefs.putUShort("sme", hsMemory);   prefs.putUShort("ssi", hsSimon);
  prefs.putUShort("sdo", hsDodge);    prefs.putUShort("swh", hsWhack);
  prefs.putUShort("sma", hsMaze);
  prefs.end();
}
void LoadSettings(void) {
  prefs.begin(NVS_NS, true);
  if (prefs.getUInt("magic", 0) == CAL_MAGIC) {
    CAL_A = prefs.getFloat("A", CAL_A); CAL_B = prefs.getFloat("B", CAL_B);
    CAL_C = prefs.getFloat("C", CAL_C); CAL_D = prefs.getFloat("D", CAL_D);
    CAL_E = prefs.getFloat("E", CAL_E); CAL_F = prefs.getFloat("F", CAL_F);
    EDGE_GX = prefs.getFloat("GX", EDGE_GX); EDGE_GY = prefs.getFloat("GY", EDGE_GY);
    calLoaded = true;
  }
  gBright    = prefs.getUChar("bright", 200);
  gTheme     = prefs.getUChar("theme", 0);
  gGlowLevel = prefs.getUChar("glow", 2);
  gFxLevel   = prefs.getUChar("fx", 2);
  gShowFps   = prefs.getUChar("fps", 1) != 0;
  String ss  = prefs.getString("ssid", DEF_SSID);
  String pp  = prefs.getString("pass", DEF_PASS);
  snprintf(wifiSsid, sizeof(wifiSsid), "%s", ss.c_str());
  snprintf(wifiPass, sizeof(wifiPass), "%s", pp.c_str());
  hs2048 = prefs.getUShort("s2048", 0); hsBreak  = prefs.getUShort("sbrk", 0);
  hsFlappy= prefs.getUShort("sfl", 0);  hsSnake  = prefs.getUShort("ssn", 0);
  hsPong  = prefs.getUShort("spo", 0);  hsTetris = prefs.getUShort("ste", 0);
  hsMemory= prefs.getUShort("sme", 0);  hsSimon  = prefs.getUShort("ssi", 0);
  hsDodge = prefs.getUShort("sdo", 0);  hsWhack  = prefs.getUShort("swh", 0);
  hsMaze  = prefs.getUShort("sma", 0);
  prefs.end();
  Serial.println(calLoaded ? "CAL: loaded from flash" : "CAL: using defaults");
}
void ResetCalibration(void) {
  CAL_A =  0.973913038f; CAL_B = -0.031239455f; CAL_C =   7.33205278f;
  CAL_D = -0.046093319f; CAL_E = -0.903280217f; CAL_F = 235.615557f;
  EDGE_GX = 1.04f; EDGE_GY = 1.18f;
  prefs.begin(NVS_NS, false); prefs.remove("magic"); prefs.end();
  calLoaded = false;
}

void CalibrateTouch(float rx, float ry, int &ox, int &oy) {
  float x = CAL_A * rx + CAL_B * ry + CAL_C;
  float y = CAL_D * rx + CAL_E * ry + CAL_F;
  x = (x - SCREEN_W * 0.5f) * EDGE_GX + SCREEN_W * 0.5f;
  y = (y - SCREEN_H * 0.5f) * EDGE_GY + SCREEN_H * 0.5f;
  ox = clampi((int)(x + 0.5f), 0, SCREEN_W - 1);
  oy = clampi((int)(y + 0.5f), 0, SCREEN_H - 1);
#if TOUCH_DEBUG
  Serial.printf("raw(%4d,%4d) -> (%3d,%3d)\n", (int)rx, (int)ry, ox, oy);
#endif
}

// =====================================================================
//  CST328  -- known-working protocol, unchanged
// =====================================================================
bool I2C_Read(uint8_t a, uint16_t r, uint8_t *d, uint32_t n) {
  Wire.beginTransmission(a);
  Wire.write((uint8_t)(r >> 8));
  Wire.write((uint8_t)(r & 0xFF));
  if (Wire.endTransmission(true) != 0) return false;
  uint32_t got = Wire.requestFrom((uint8_t)a, (uint8_t)n);
  if (got != n) return false;
  for (uint32_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}
void CST328_Reset(void) {
  pinMode(CST328_RST_PIN, OUTPUT);
  digitalWrite(CST328_RST_PIN, HIGH); delay(50);
  digitalWrite(CST328_RST_PIN, LOW);  delay(5);
  digitalWrite(CST328_RST_PIN, HIGH); delay(50);
}
bool ReadRawTouch(uint16_t &x, uint16_t &y) {
  uint8_t buf[28] = {0};
  if (!I2C_Read(CST328_ADDR, REG_NUM, buf, 1)) return false;
  if ((buf[0] & 0x0F) == 0) return false;
  if (!I2C_Read(CST328_ADDR, REG_XY, &buf[1], 27)) return false;
  uint16_t rx = ((uint16_t)buf[2] << 4) | ((buf[4] & 0xF0) >> 4);
  uint16_t ry = ((uint16_t)buf[3] << 4) | (buf[4] & 0x0F);
  x = ry;   // this panel has X/Y swapped -- do not remove
  y = rx;
  Wire.beginTransmission(CST328_ADDR);
  Wire.write((uint8_t)(REG_NUM >> 8));
  Wire.write((uint8_t)(REG_NUM & 0xFF));
  Wire.write((uint8_t)0);
  Wire.endTransmission(true);
  return true;
}
// ---------------------------------------------------------------------
//  RAW TOUCH CACHE
//  ReadRawTouch() is DESTRUCTIVE: it writes 0 to the CST328 touch-count
//  register (0xD005) to acknowledge the sample. Calling it twice in one
//  frame means the second call always sees count==0 and fails. The
//  calibration wizard used to do exactly that (PollTouch consumed the
//  sample first), so it could never collect a point. Everything now goes
//  through this single cached read.
// ---------------------------------------------------------------------
bool     rawValid = false;      // did we get a raw sample this frame?
uint16_t rawTX = 0, rawTY = 0;  // last raw coords, pre-calibration

bool GetTouch(int &x, int &y) {
  uint16_t rx, ry;
  rawValid = false;
  if (!ReadRawTouch(rx, ry)) return false;
  rawTX = rx; rawTY = ry; rawValid = true;
  CalibrateTouch((float)rx, (float)ry, x, y);
  return true;
}
void PollTouch(void) {
  int tx, ty;
  bool got = GetTouch(tx, ty);
  uint32_t now = millis();
  touchDown = touchUp = false;
  if (got) {
    touchX = tx; touchY = ty;
    if (!touchActive) {
      touchActive = true; touchDown = true;
      lastTX = tx; lastTY = ty; pressX = tx; pressY = ty;
      smoothTX = tx; smoothTY = ty;
    } else {
      smoothTX += (tx - smoothTX) * 0.55f;
      smoothTY += (ty - smoothTY) * 0.55f;
    }
    lastTouchMs = now;
  } else if (touchActive && now - lastTouchMs > 90) {
    touchActive = false; touchUp = true;
  }
}

// Bind the LEDC timer/channel to the backlight pad. Safe to call again.
void BacklightAttach(void) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcAttach(TFT_BL, BL_PWM_FREQ, BL_PWM_BITS);
#else
  ledcSetup(BL_PWM_CH, BL_PWM_FREQ, BL_PWM_BITS);
  ledcAttachPin(TFT_BL, BL_PWM_CH);
#endif
}

// Force the backlight pin fully off.
// SetBrightness() refuses duty 0 (floor of 8 / duty 3) so sleep must not
// go through it. LEDC is zeroed with the correct core API, then detached,
// then the pad is driven as a plain GPIO LOW. No gpio_hold: display sleep
// no longer enters esp_light_sleep, so a hold is unnecessary and has been
// a source of sticky-pin surprises on wake.
static void BacklightForceOff(void) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWrite(TFT_BL, 0);
  ledcDetach(TFT_BL);
#else
  ledcWrite(BL_PWM_CH, 0);
  ledcDetachPin(TFT_BL);
#endif
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);            // active-HIGH backlight assumed
}

// Re-bind PWM after a forced off. Pair with BacklightForceOff.
static void BacklightReleaseHold(void) {
  BacklightAttach();
}

void SetBrightness(uint8_t v) {
  // 8 is the floor: below that the panel reads as off and the user
  // cannot find the slider again. Display sleep bypasses this path
  // via BacklightForceOff() so the panel can go fully dark.
  if (v < 8) v = 8;
  gBright = v;
  // Perceptual ramp. A linear duty cycle feels like nothing happens
  // across the top half of the slider, which is exactly the reported
  // symptom. Gamma 2.2 makes the control feel even end to end.
  float n = (float)v / 255.0f;
  uint32_t duty = (uint32_t)(powf(n, 2.2f) * 255.0f + 0.5f);
  if (duty < 3) duty = 3;
  if (duty > 255) duty = 255;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWrite(TFT_BL, duty);
#else
  ledcWrite(BL_PWM_CH, duty);
#endif
}

// =====================================================================
//  PSRAM
// =====================================================================
bool AllocBuffers(void) {
  fb[0]  = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
  fb[1]  = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
  accum  = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
  canvas = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
  // Depth buffer lives in PSRAM. It used to claim 150 KB of INTERNAL
  // SRAM, which together with the 25 KB DMA bands starved the WiFi
  // driver: it could not allocate its 4 static RX buffers and logged
  //   "wifi: Expected to init 4 rx buffer, actual is 0"
  // leaving the radio permanently unable to associate.
  // DMA bands MUST be internal (the SPI engine cannot read PSRAM), so
  // the depth buffer is the one that moves. It is accessed linearly per
  // scanline inside RasterTriangle, which PSRAM handles well.
  depth  = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
  gDepthInternal = false;
  vPos = (Vert *)heap_caps_malloc(sizeof(Vert) * MAX_VERTS, MALLOC_CAP_SPIRAM);
  vNrm = (Vert *)heap_caps_malloc(sizeof(Vert) * MAX_VERTS, MALLOC_CAP_SPIRAM);
  tArr = (Tri  *)heap_caps_malloc(sizeof(Tri)  * MAX_TRIS,  MALLOC_CAP_SPIRAM);
  warp = (WarpStar *)heap_caps_malloc(sizeof(WarpStar) * NUM_WARP, MALLOC_CAP_SPIRAM);
  gSV = (SVert *)heap_caps_malloc(sizeof(SVert) * MAX_MESH_VERTS,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!gSV) gSV = (SVert *)heap_caps_malloc(sizeof(SVert) * MAX_MESH_VERTS, MALLOC_CAP_SPIRAM);
  parts = (Particle *)heap_caps_malloc(sizeof(Particle) * NUM_PART,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!parts) parts = (Particle *)heap_caps_malloc(sizeof(Particle) * NUM_PART, MALLOC_CAP_SPIRAM);
  dust = (Dust *)heap_caps_malloc(sizeof(Dust) * NUM_DUST,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!dust) dust = (Dust *)heap_caps_malloc(sizeof(Dust) * NUM_DUST, MALLOC_CAP_SPIRAM);
  glowLUT = (uint8_t *)heap_caps_malloc(GLOW_D * GLOW_D,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!glowLUT) glowLUT = (uint8_t *)heap_caps_malloc(GLOW_D * GLOW_D, MALLOC_CAP_SPIRAM);
  return (fb[0] && fb[1] && depth && accum && canvas && vPos && vNrm && tArr &&
          warp && gSV && parts && dust && glowLUT);
}

// =====================================================================
//  GEOMETRY  (12 solids)
// =====================================================================
static bool PoolFit(uint16_t nv, uint16_t nt, const char *who) {
  if (vTop + nv > MAX_VERTS || tTop + nt > MAX_TRIS) {
    Serial.printf("POOL OVERFLOW %s\n", who); return false;
  }
  if (nv > MAX_MESH_VERTS) { Serial.printf("MESH TOO BIG %s\n", who); return false; }
  return true;
}
static void MeshEmpty(Mesh &m) { m.pos = vPos; m.nrm = vNrm; m.tri = tArr; m.nv = 0; m.nt = 0; }
static void MeshBegin(Mesh &m) { m.pos = &vPos[vTop]; m.nrm = &vNrm[vTop]; m.tri = &tArr[tTop]; }
static void MeshEnd(Mesh &m, int v, int t) { m.nv = v; m.nt = t; vTop += v; tTop += t; }
static inline void SetTri(Mesh &m, int t, uint16_t a, uint16_t b, uint16_t c,
                          uint8_t r, uint8_t g, uint8_t bl) {
  m.tri[t].a = a; m.tri[t].b = b; m.tri[t].c = c;
  m.tri[t].cr = r; m.tri[t].cg = g; m.tri[t].cb = bl;
}
static inline void SetVert(Mesh &m, int i, float x, float y, float z,
                           float nx, float ny, float nz) {
  m.pos[i].x = x; m.pos[i].y = y; m.pos[i].z = z;
  float l = sqrtf(nx*nx + ny*ny + nz*nz);
  if (l < 1e-5f) l = 1;
  m.nrm[i].x = nx/l; m.nrm[i].y = ny/l; m.nrm[i].z = nz/l;
}
static void BuildSphere(Mesh &m) {
  const int LAT = 18, LON = 28;
  if (!PoolFit((LAT+1)*(LON+1), LAT*LON*2, "sphere")) { MeshEmpty(m); return; }
  MeshBegin(m);
  int v = 0;
  for (int i = 0; i <= LAT; i++) {
    float th = (float)M_PI * i / LAT, st = sinf(th), ct = cosf(th);
    for (int j = 0; j <= LON; j++) {
      float ph = TAU * j / LON;
      float x = st*cosf(ph), y = ct, z = st*sinf(ph);
      SetVert(m, v++, x, y, z, x, y, z);
    }
  }
  int t = 0;
  for (int i = 0; i < LAT; i++)
    for (int j = 0; j < LON; j++) {
      uint16_t a = i*(LON+1)+j, b = a+1, c = a+(LON+1), d = c+1;
      uint8_t r, g, bl;
      int n = ((i*7 + j*13) ^ (i*j)) & 7;
      if (i < 2 || i >= LAT-2) { r = 226; g = 214; bl = 190; }
      else if (n < 3)          { r = 176; g = 126; bl = 70; }
      else if (n < 5)          { r = 58;  g = 50;  bl = 40; }
      else                     { r = 96;  g = 76;  bl = 52; }
      SetTri(m, t++, a, c, b, r, g, bl);
      SetTri(m, t++, b, c, d, r, g, bl);
    }
  MeshEnd(m, v, t);
}
static void BuildTorus(Mesh &m) {
  const int MAJ = 26, MIN = 12;
  if (!PoolFit((MAJ+1)*(MIN+1), MAJ*MIN*2, "torus")) { MeshEmpty(m); return; }
  MeshBegin(m);
  const float R = 0.76f, rr = 0.30f;
  int v = 0;
  for (int i = 0; i <= MAJ; i++) {
    float u = TAU*i/MAJ, cu = cosf(u), su = sinf(u);
    for (int j = 0; j <= MIN; j++) {
      float w = TAU*j/MIN, cw = cosf(w), sw = sinf(w);
      SetVert(m, v++, (R+rr*cw)*cu, rr*sw, (R+rr*cw)*su, cw*cu, sw, cw*su);
    }
  }
  int t = 0;
  for (int i = 0; i < MAJ; i++)
    for (int j = 0; j < MIN; j++) {
      uint16_t a = i*(MIN+1)+j, b = a+1, c = a+(MIN+1), d = c+1;
      float f = 0.5f + 0.5f*fsin(TAU*i/MAJ);
      uint8_t r = (uint8_t)(210-60*f), g = (uint8_t)(160-20*f), bl = (uint8_t)(100+60*f);
      if (((i+j)&1) == 0) { r = (r*3)>>2; g = (g*3)>>2; bl = (bl*3)>>2; }
      SetTri(m, t++, a, b, c, r, g, bl);
      SetTri(m, t++, b, d, c, r, g, bl);
    }
  MeshEnd(m, v, t);
}
static void BuildKnot(Mesh &m) {
  const int SEG = 40, SIDE = 8;
  if (!PoolFit((SEG+1)*(SIDE+1), SEG*SIDE*2, "knot")) { MeshEmpty(m); return; }
  MeshBegin(m);
  int v = 0;
  for (int i = 0; i <= SEG; i++) {
    float u = TAU*i/SEG;
    float cx = (sinf(u)+2*sinf(2*u))*0.26f;
    float cy = (cosf(u)-2*cosf(2*u))*0.26f;
    float cz = (-sinf(3*u))*0.26f;
    float e = 0.01f;
    float tx = (sinf(u+e)+2*sinf(2*(u+e)))*0.26f - cx;
    float ty = (cosf(u+e)-2*cosf(2*(u+e)))*0.26f - cy;
    float tz = (-sinf(3*(u+e)))*0.26f - cz;
    float tl = sqrtf(tx*tx+ty*ty+tz*tz); if (tl < 1e-6f) tl = 1;
    tx/=tl; ty/=tl; tz/=tl;
    float ax = 0, ay = 0, az = 1;
    if (fabsf(tz) > 0.9f) { ax = 1; az = 0; }
    float nx = ty*az-tz*ay, ny = tz*ax-tx*az, nz = tx*ay-ty*ax;
    float nl = sqrtf(nx*nx+ny*ny+nz*nz); if (nl < 1e-6f) nl = 1;
    nx/=nl; ny/=nl; nz/=nl;
    float bx = ty*nz-tz*ny, by = tz*nx-tx*nz, bz = tx*ny-ty*nx;
    for (int j = 0; j <= SIDE; j++) {
      float w = TAU*j/SIDE, cw = cosf(w), sw = sinf(w);
      float ox = nx*cw+bx*sw, oy = ny*cw+by*sw, oz = nz*cw+bz*sw;
      SetVert(m, v++, cx+ox*0.17f, cy+oy*0.17f, cz+oz*0.17f, ox, oy, oz);
    }
  }
  int t = 0;
  for (int i = 0; i < SEG; i++)
    for (int j = 0; j < SIDE; j++) {
      uint16_t a = i*(SIDE+1)+j, b = a+1, c = a+(SIDE+1), d = c+1;
      float f = (float)i/SEG;
      uint8_t r = (uint8_t)(150+80*fsin(f*TAU));
      uint8_t g = (uint8_t)(130+50*fcos(f*TAU));
      uint8_t bl= (uint8_t)(110+70*fsin(f*TAU+2));
      SetTri(m, t++, a, b, c, r, g, bl);
      SetTri(m, t++, b, d, c, r, g, bl);
    }
  MeshEnd(m, v, t);
}
static void BuildSpike(Mesh &m) {
  const int LAT = 12, LON = 16;
  if (!PoolFit((LAT+1)*(LON+1), LAT*LON*2, "spike")) { MeshEmpty(m); return; }
  MeshBegin(m);
  int v = 0;
  for (int i = 0; i <= LAT; i++) {
    float th = (float)M_PI*i/LAT, st = sinf(th), ct = cosf(th);
    for (int j = 0; j <= LON; j++) {
      float ph = TAU*j/LON;
      float x = st*cosf(ph), y = ct, z = st*sinf(ph);
      float sp = (((i+j)&1) == 0) ? 1.34f : 0.80f;
      SetVert(m, v++, x*sp, y*sp, z*sp, x, y, z);
    }
  }
  int t = 0;
  for (int i = 0; i < LAT; i++)
    for (int j = 0; j < LON; j++) {
      uint16_t a = i*(LON+1)+j, b = a+1, c = a+(LON+1), d = c+1;
      bool hi = (((i+j)&1) == 0);
      uint8_t r = hi?208:96, g = hi?168:80, bl = hi?104:66;
      SetTri(m, t++, a, c, b, r, g, bl);
      SetTri(m, t++, b, c, d, r, g, bl);
    }
  MeshEnd(m, v, t);
}
static void BuildGear(Mesh &m) {
  const int TEETH = 12, PTS = TEETH*2;
  if (!PoolFit(PTS*2+2, PTS*4, "gear")) { MeshEmpty(m); return; }
  MeshBegin(m);
  const float hz = 0.22f;
  int v = 0;
  for (int s = 0; s < 2; s++) {
    float z = s ? hz : -hz;
    for (int i = 0; i < PTS; i++) {
      float a = TAU*i/PTS, r = (i&1) ? 0.60f : 0.92f;
      SetVert(m, v++, fcos(a)*r, fsin(a)*r, z, fcos(a), fsin(a), 0);
    }
  }
  int cb = v; SetVert(m, v++, 0, 0, -hz, 0, 0, -1);
  int cf = v; SetVert(m, v++, 0, 0,  hz, 0, 0,  1);
  int t = 0;
  for (int i = 0; i < PTS; i++) {
    int j = (i+1)%PTS, b0 = i, b1 = j, f0 = PTS+i, f1 = PTS+j;
    uint8_t r = (i&1)?196:150, g = (i&1)?158:122, bl = (i&1)?92:74;
    SetTri(m, t++, b0, b1, f1, r, g, bl);
    SetTri(m, t++, b0, f1, f0, r, g, bl);
    SetTri(m, t++, cb, b1, b0, 120, 100, 70);
    SetTri(m, t++, cf, f0, f1, 210, 176, 110);
  }
  MeshEnd(m, v, t);
}
static void BuildTube(Mesh &m, float rTop, float rBot, const char *who) {
  const int S = 24;
  if (!PoolFit(S*2+2, S*4, who)) { MeshEmpty(m); return; }
  MeshBegin(m);
  const float hz = 0.85f;
  int v = 0;
  for (int i = 0; i < S; i++) { float a = TAU*i/S;
    SetVert(m, v++, fcos(a)*rBot, -hz, fsin(a)*rBot, fcos(a), 0.25f, fsin(a)); }
  for (int i = 0; i < S; i++) { float a = TAU*i/S; float rr = (rTop < 0.02f) ? 0.02f : rTop;
    SetVert(m, v++, fcos(a)*rr, hz, fsin(a)*rr, fcos(a), 0.25f, fsin(a)); }
  int cb = v; SetVert(m, v++, 0, -hz, 0, 0, -1, 0);
  int ct = v; SetVert(m, v++, 0,  hz, 0, 0,  1, 0);
  int t = 0;
  for (int i = 0; i < S; i++) {
    int j = (i+1)%S, b0 = i, b1 = j, t0 = S+i, t1 = S+j;
    float f = (float)i/S;
    uint8_t r = (uint8_t)(150+60*fsin(f*TAU));
    uint8_t g = (uint8_t)(132+40*fsin(f*TAU+1));
    uint8_t bl= (uint8_t)(104+40*fsin(f*TAU+2));
    SetTri(m, t++, b0, t1, b1, r, g, bl);
    SetTri(m, t++, b0, t0, t1, r, g, bl);
    SetTri(m, t++, cb, b1, b0, 96, 84, 66);
    SetTri(m, t++, ct, t0, t1, 200, 176, 130);
  }
  MeshEnd(m, v, t);
}
static void BuildHelix(Mesh &m) {
  const int SEG = 60, SIDE = 6;
  if (!PoolFit((SEG+1)*(SIDE+1), SEG*SIDE*2, "helix")) { MeshEmpty(m); return; }
  MeshBegin(m);
  const float coilR = 0.62f, tubeR = 0.12f, turns = 3, height = 1.5f;
  int v = 0;
  for (int i = 0; i <= SEG; i++) {
    float u = (float)i/SEG, a = TAU*turns*u;
    float cx = fcos(a)*coilR, cy = -height*0.5f + height*u, cz = fsin(a)*coilR;
    float e = 0.02f, a2 = TAU*turns*(u+e);
    float tx = fcos(a2)*coilR-cx, ty = height*e, tz = fsin(a2)*coilR-cz;
    float tl = sqrtf(tx*tx+ty*ty+tz*tz); if (tl < 1e-6f) tl = 1;
    tx/=tl; ty/=tl; tz/=tl;
    float nx = -fcos(a), ny = 0, nz = -fsin(a);
    float bx = ty*nz-tz*ny, by = tz*nx-tx*nz, bz = tx*ny-ty*nx;
    for (int j = 0; j <= SIDE; j++) {
      float w = TAU*j/SIDE, cw = fcos(w), sw = fsin(w);
      float ox = nx*cw+bx*sw, oy = ny*cw+by*sw, oz = nz*cw+bz*sw;
      SetVert(m, v++, cx+ox*tubeR, cy+oy*tubeR, cz+oz*tubeR, ox, oy, oz);
    }
  }
  int t = 0;
  for (int i = 0; i < SEG; i++)
    for (int j = 0; j < SIDE; j++) {
      uint16_t a = i*(SIDE+1)+j, b = a+1, c = a+(SIDE+1), d = c+1;
      float f = (float)i/SEG;
      uint8_t r = (uint8_t)(180+50*fsin(f*TAU*1.5f));
      uint8_t g = (uint8_t)(150+30*fsin(f*TAU*1.5f+1.4f));
      uint8_t bl= (uint8_t)(96+50*fsin(f*TAU*1.5f+2.8f));
      SetTri(m, t++, a, b, c, r, g, bl);
      SetTri(m, t++, b, d, c, r, g, bl);
    }
  MeshEnd(m, v, t);
}
static void BuildPoly(Mesh &m, const float *p, int np, const uint16_t *ix, int nt,
                      uint8_t r, uint8_t g, uint8_t b, const char *who) {
  if (!PoolFit(np, nt, who)) { MeshEmpty(m); return; }
  MeshBegin(m);
  for (int i = 0; i < np; i++)
    SetVert(m, i, p[i*3], p[i*3+1], p[i*3+2], p[i*3], p[i*3+1], p[i*3+2]);
  for (int i = 0; i < nt; i++)
    SetTri(m, i, ix[i*3], ix[i*3+1], ix[i*3+2], r, g, b);
  MeshEnd(m, np, nt);
}

// ---- ringworld: a sphere plus a flat annulus ------------------------
static void BuildRingWorld(Mesh &m){
  const int LAT=12,LON=18,RSEG=28;
  uint16_t nv=(LAT+1)*(LON+1)+RSEG*2, nt=LAT*LON*2+RSEG*2;
  if (!PoolFit(nv,nt,"ringworld")){ MeshEmpty(m); return; }
  MeshBegin(m);
  int v=0;
  for (int i=0;i<=LAT;i++){
    float th=(float)M_PI*i/LAT, st=sinf(th), ct=cosf(th);
    for (int j=0;j<=LON;j++){
      float ph=TAU*j/LON;
      float x=st*cosf(ph)*0.62f, y=ct*0.62f, z=st*sinf(ph)*0.62f;
      SetVert(m,v++,x,y,z,x,y,z); } }
  int ringBase=v;
  for (int i=0;i<RSEG;i++){
    float a=TAU*i/RSEG;
    SetVert(m,v++,fcos(a)*0.86f,0,fsin(a)*0.86f,0,1,0);
    SetVert(m,v++,fcos(a)*1.30f,0,fsin(a)*1.30f,0,1,0); }
  int t=0;
  for (int i=0;i<LAT;i++)
    for (int j=0;j<LON;j++){
      uint16_t a=i*(LON+1)+j,b=a+1,c=a+(LON+1),d=c+1;
      uint8_t r,g,bl;
      int n=((i*5+j*11)^(i*j))&7;
      if (i<2||i>=LAT-2){ r=214; g=206; bl=190; }
      else if (n<3){ r=150; g=112; bl=64; }
      else { r=72; g=90; bl=112; }
      SetTri(m,t++,a,c,b,r,g,bl);
      SetTri(m,t++,b,c,d,r,g,bl); }
  for (int i=0;i<RSEG;i++){
    int j=(i+1)%RSEG;
    uint16_t i0=ringBase+i*2, i1=ringBase+i*2+1;
    uint16_t j0=ringBase+j*2, j1=ringBase+j*2+1;
    uint8_t sh=(i&1)?190:140;
    SetTri(m,t++,i0,i1,j1,sh,(uint8_t)(sh*0.85f),(uint8_t)(sh*0.6f));
    SetTri(m,t++,i0,j1,j0,sh,(uint8_t)(sh*0.85f),(uint8_t)(sh*0.6f)); }
  MeshEnd(m,v,t);
}
// ---- pyramid --------------------------------------------------------
static void BuildPyramid(Mesh &m){
  static const float P[]={ -0.8f,-0.6f,-0.8f, 0.8f,-0.6f,-0.8f,
                            0.8f,-0.6f, 0.8f, -0.8f,-0.6f, 0.8f, 0,1.0f,0 };
  static const uint16_t I[]={0,2,1, 0,3,2, 0,1,4, 1,2,4, 2,3,4, 3,0,4};
  BuildPoly(m,P,5,I,6,206,178,120,"pyramid");
}
// ---- mobius strip ---------------------------------------------------
static void BuildMobius(Mesh &m){
  // Mobius is built LAST, so it only ever gets the pool remainder.
  // Measured: after the other 14 solids the pool holds 2412/2700 verts
  // and 4212/4400 tris, leaving 288 v / 188 t. At SEG=40 this strip
  // wants 164 v / 240 t -- the TRIANGLE budget is what overflows, and
  // the old code then emitted an EMPTY mesh, so object 14 rendered as
  // nothing. Rather than inflating the pool (which costs PSRAM for
  // every object), degrade tessellation until it fits. SEG=30 gives
  // 124 v / 180 t and is visually indistinguishable at this scale.
  // The pool is now sized so SEG=40 fits with room to spare (measured:
  // 2576/2900 verts, 4452/4800 tris after all 15 solids). This loop is
  // retained as a safety net: if a future mesh is added ahead of Mobius
  // it degrades gracefully instead of emitting an empty object.
  const int W=3, MARGIN_V=64, MARGIN_T=96;
  int SEG=40;
  while (SEG>=12){
    uint16_t nv=(uint16_t)((SEG+1)*(W+1)), nt=(uint16_t)(SEG*W*2);
    if (vTop+nv+MARGIN_V<=MAX_VERTS && tTop+nt+MARGIN_T<=MAX_TRIS) break;
    SEG-=2; }
  uint16_t nv=(uint16_t)((SEG+1)*(W+1)), nt=(uint16_t)(SEG*W*2);
  if (!PoolFit(nv,nt,"mobius")){ MeshEmpty(m); return; }
  if (SEG<40) Serial.printf("mobius: tessellation reduced to SEG=%d to fit pool\n",SEG);
  MeshBegin(m);
  int v=0;
  for (int i=0;i<=SEG;i++){
    float u=TAU*i/SEG;
    for (int j=0;j<=W;j++){
      float t2=((float)j/W-0.5f)*0.46f;
      float half=u*0.5f;
      float r=0.78f+t2*fcos(half);
      float x=r*fcos(u), y=t2*fsin(half), z=r*fsin(u);
      SetVert(m,v++,x,y,z,fcos(u)*fcos(half),fsin(half),fsin(u)*fcos(half)); } }
  int t=0;
  for (int i=0;i<SEG;i++)
    for (int j=0;j<W;j++){
      uint16_t a=i*(W+1)+j,b=a+1,c=a+(W+1),d=c+1;
      float f=(float)i/SEG;
      uint8_t r=(uint8_t)(140+80*fsin(f*TAU));
      uint8_t g=(uint8_t)(150+60*fcos(f*TAU));
      uint8_t bl=(uint8_t)(170+60*fsin(f*TAU+2));
      SetTri(m,t++,a,b,c,r,g,bl);
      SetTri(m,t++,b,d,c,r,g,bl); }
  MeshEnd(m,v,t);
}

void BuildAll(void) {
  BuildSphere(gMesh[0]);
  BuildTorus(gMesh[1]);
  static const float cubeP[] = {
    -0.72f,-0.72f,-0.72f, 0.72f,-0.72f,-0.72f, 0.72f,0.72f,-0.72f, -0.72f,0.72f,-0.72f,
    -0.72f,-0.72f, 0.72f, 0.72f,-0.72f, 0.72f, 0.72f,0.72f, 0.72f, -0.72f,0.72f, 0.72f };
  static const uint16_t cubeI[] = {
    0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4, 1,2,6, 1,6,5, 2,3,7, 2,7,6, 3,0,4, 3,4,7 };
  BuildPoly(gMesh[2], cubeP, 8, cubeI, 12, 196, 158, 96, "cube");
  static float icoP[36];
  { const float t0 = 0.618f;
    const float p[36] = { -1,t0,0, 1,t0,0, -1,-t0,0, 1,-t0,0, 0,-1,t0, 0,1,t0,
                          0,-1,-t0, 0,1,-t0, t0,0,-1, t0,0,1, -t0,0,-1, -t0,0,1 };
    for (int i = 0; i < 36; i++) icoP[i] = p[i]*0.62f; }
  static const uint16_t icoI[] = {
    0,11,5, 0,5,1, 0,1,7, 0,7,10, 0,10,11, 1,5,9, 5,11,4, 11,10,2, 10,7,6, 7,1,8,
    3,9,4, 3,4,2, 3,2,6, 3,6,8, 3,8,9, 4,9,5, 2,4,11, 6,2,10, 8,6,7, 9,8,1 };
  BuildPoly(gMesh[3], icoP, 12, icoI, 20, 200, 150, 96, "icosa");
  static const float diaP[] = {
     0.86f,0,0, 0.43f,0,0.74f, -0.43f,0,0.74f, -0.86f,0,0,
    -0.43f,0,-0.74f, 0.43f,0,-0.74f, 0,1.05f,0, 0,-1.05f,0 };
  static const uint16_t diaI[] = {
    0,6,1, 1,6,2, 2,6,3, 3,6,4, 4,6,5, 5,6,0, 1,7,0, 2,7,1, 3,7,2, 4,7,3, 5,7,4, 0,7,5 };
  BuildPoly(gMesh[4], diaP, 8, diaI, 12, 222, 196, 156, "crystal");
  static const float shipP[] = {
     0,0,1.25f, -0.75f,0,-0.75f, 0.75f,0,-0.75f,
     0,0.34f,-0.35f, 0,-0.20f,-0.35f, -0.25f,0,-0.90f, 0.25f,0,-0.90f };
  static const uint16_t shipI[] = {
    0,3,1, 0,2,3, 0,1,4, 0,4,2, 1,3,5, 3,2,6, 1,5,4, 4,6,2, 5,3,6, 5,6,4 };
  BuildPoly(gMesh[5], shipP, 7, shipI, 10, 132, 156, 168, "ship");
  BuildKnot(gMesh[6]);
  BuildSpike(gMesh[7]);
  BuildGear(gMesh[8]);
  BuildTube(gMesh[9],  0.62f, 0.62f, "cyl");
  BuildTube(gMesh[10], 0.00f, 0.86f, "cone");
  BuildHelix(gMesh[11]);
  BuildRingWorld(gMesh[12]);
  BuildPyramid(gMesh[13]);
  BuildMobius(gMesh[14]);
  static const float octP[] = { 0,0.95f,0, 0,-0.95f,0, 0.9f,0,0, -0.9f,0,0, 0,0,0.9f, 0,0,-0.9f };
  static const uint16_t octI[] = { 0,4,2, 0,2,5, 0,5,3, 0,3,4, 1,2,4, 1,5,2, 1,3,5, 1,4,3 };
  BuildPoly(meshOcta, octP, 6, octI, 8, 200, 168, 110, "octa");

  randomSeed(0xC0FFEE);
  for (int i = 0; i < NUM_WARP; i++) {
    warp[i].x = (float)random(-1600, 1600);
    warp[i].y = (float)random(-1200, 1200);
    warp[i].z = (float)random(20, 1600);
  }
  for (int i = 0; i < NUM_DUST; i++) {
    dust[i].x = (float)random(0, SCREEN_W);
    dust[i].y = (float)random(0, SCREEN_H);
    dust[i].z = 0.3f + 0.7f*Hash(i*977u);
    dust[i].ph = Hash(i*31u)*TAU;
  }
  for (int i = 0; i < NUM_PART; i++) parts[i].life = 0;
  for (int i = 0; i < GRAPH_LEN; i++) { histFps[i] = 60; histFrame[i] = 100; histLoad[i] = 25; }
}

// =====================================================================
//  FRAMEBUFFER PRIMITIVES
// =====================================================================
// =====================================================================
//  FRAME PUSH  --  DMA out of internal SRAM, never out of PSRAM.
//
//  ROOT CAUSE of the boot crash:
//  fb[0]/fb[1] live in PSRAM (MALLOC_CAP_SPIRAM). The ESP32-S3 SPI
//  master driver rejects a non-DMA-capable source: spi_device_queue_trans
//  returns ESP_ERR_INVALID_ARG, and TFT_eSPI asserts on it
//  (TFT_eSPI_ESP32_S3.c:666  assert(ret == ESP_OK)).
//  A 153,600 byte framebuffer cannot live in internal RAM (only ~74 KB
//  free), so we stream: copy one horizontal band PSRAM -> internal, then
//  DMA that band out. Two bands alternate, so the CPU fills band N+1
//  while band N is still on the wire.
//
//  Overlap safety: TFT_eSPI::pushPixelsDMA() calls dmaWait() before it
//  queues. Queueing band N+1 therefore blocks until band N has fully
//  completed, which guarantees the buffer we are about to memcpy into is
//  no longer owned by the DMA engine. No transaction can overlap itself.
// =====================================================================
#define DMA_BAND_ROWS  20
#define DMA_BAND_PX    (SCREEN_W * DMA_BAND_ROWS)
#define DMA_BAND_BYTES (DMA_BAND_PX * 2)
static uint16_t *dmaBand[2] = { nullptr, nullptr };
static bool      gDmaReady  = false;

// Diagnostic only: the ESP32-S3 internal DRAM window. PSRAM maps at
// 0x3C000000.. and is NOT a valid SPI DMA source on this path. We report
// this but never gate on it -- MALLOC_CAP_DMA is the authoritative test,
// because the allocator, not a hardcoded constant, knows the real map.
static inline bool PtrIsInternalDram(const void *p){
  uintptr_t a = (uintptr_t)p;
  return (a >= 0x3FC80000UL && a < 0x3FD00000UL);
}
bool DmaBandsAlloc(void){
  for (int i=0;i<2;i++){
    // MALLOC_CAP_DMA is the contract: the heap returns memory the DMA
    // engine can actually read. If it cannot satisfy that, we fall back
    // rather than handing the driver a pointer it will assert on.
    dmaBand[i] = (uint16_t *)heap_caps_malloc(
        DMA_BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dmaBand[i]){
      for (int k=0;k<i;k++){ heap_caps_free(dmaBand[k]); dmaBand[k]=nullptr; }
      gDmaReady=false;
      return false; } }
  gDmaReady=true;
  return true;
}
// One-time boot diagnostic. Deliberately not per-frame.
void DmaReport(void){
  Serial.println("---- DMA path ----");
  Serial.printf("  framebuffer fb[0]   %p  %s\n", (void*)fb[0],
                PtrIsInternalDram(fb[0]) ? "INTERNAL" : "PSRAM (not DMA-safe)");
  for (int i=0;i<2;i++)
    Serial.printf("  dma band %d          %p  %s  %u B\n", i, (void*)dmaBand[i],
                  dmaBand[i] ? (PtrIsInternalDram(dmaBand[i])?"INTERNAL/DMA-capable"
                                                              :"DMA-capable (outside expected window)")
                             : "NULL",
                  (unsigned)DMA_BAND_BYTES);
  Serial.printf("  bands per frame     %d x %d rows\n",
                (SCREEN_H + DMA_BAND_ROWS - 1)/DMA_BAND_ROWS, DMA_BAND_ROWS);
  Serial.printf("  mode                %s\n",
                gDmaReady ? "banded DMA from internal SRAM"
                          : "CPU pushPixels fallback (PSRAM safe)");
  Serial.printf("  free heap %u | free PSRAM %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  Serial.println("------------------");
}
// =====================================================================
//  DISPLAY POWER  (ST7789 SLPIN/SLPOUT + backlight off)
//
//  This is a DISPLAY power control, not a shutdown and not a CPU sleep.
//  Nothing is reset: appState, shMode, the framebuffers, JEE data, WiFi
//  and calibration stay exactly as they were, so waking redraws the
//  very screen the user left -- no boot animation.
//
//  We deliberately do NOT call esp_light_sleep_start() here. On the
//  ESP32-S3 with USB-CDC serial, light sleep kills the USB stack
//  (Serial goes blank) and with WiFi up it is a frequent reboot source.
//  Idle path: stop rendering, poll CST328 for a fresh touch, NetLoop.
// =====================================================================
bool     displaySleeping = false;
static bool gAppHeader = false;   // an app drew its own header this frame
static uint32_t dispSleepMs = 0;
static bool     dispEatTouch = false;   // swallow the tap that woke us
static bool     dispTouchReleased = false;  // finger lifted since sleeping

void DisplaySleep(void){
  if (displaySleeping) return;          // repeated presses are no-ops
  displaySleeping = true;
  dispSleepMs = millis();
  dispTouchReleased = false;
  // Drain any in-flight DMA before we touch the SPI command path.
  // Pushing SLPIN mid-band has been a hard-fault source on this driver.
  if (gDmaReady) tft.dmaWait();
  tft.endWrite();
  // 1. Backlight HARD off (version-correct LEDC + plain GPIO LOW).
  //    Never call SetBrightness(0): it floors duty at 3.
  BacklightForceOff();
  // 2. ST7789 display off + sleep-in. Order and delay matter: some
  //    panels ignore SLPIN if it arrives in the same transaction burst
  //    as DISPOFF without a short settle.
  tft.writecommand(0x28);               // DISPOFF
  delay(20);
  tft.writecommand(0x10);               // SLPIN
  Serial.println("display: sleep");
  Serial.flush();
}
void DisplayWake(void){
  if (!displaySleeping) return;
  // Panel first, backlight last -- never light an uninitialised frame.
  tft.writecommand(0x11);               // SLPOUT
  delay(120);                           // ST7789 requires >=120 ms
  tft.writecommand(0x29);               // DISPON
  delay(10);
  BacklightReleaseHold();
  SetBrightness(gBright);
  displaySleeping = false;
  dispEatTouch = true;                  // the waking tap must not click
  // Keep the frame clock sane: a long idle would otherwise produce a
  // huge dt and fling every animation across the screen on resume.
  animAccum = 0;
  lastTouchMs = millis();
  touchActive = false; touchDown = false; touchUp = false;
  pressX = -1000; pressY = -1000;
  Serial.println("display: wake");
  Serial.flush();
}
void DisplayToggle(void){ displaySleeping ? DisplayWake() : DisplaySleep(); }

void PushFrame(void) {
  tft.startWrite();
  tft.setAddrWindow(0, 0, SCREEN_W, SCREEN_H);
  if (gDmaReady){
    int band = 0;
    for (int y = 0; y < SCREEN_H; y += DMA_BAND_ROWS){
      int rows = SCREEN_H - y;
      if (rows > DMA_BAND_ROWS) rows = DMA_BAND_ROWS;
      uint16_t *dst = dmaBand[band & 1];
      memcpy(dst, &frame[y * SCREEN_W], (size_t)rows * SCREEN_W * 2);
      tft.pushPixelsDMA(dst, (uint32_t)rows * SCREEN_W);
      band++; }
    tft.dmaWait();            // last band must land before we return
  } else {
    // Fallback: CPU-driven push. Slower, but reads PSRAM safely.
    tft.pushPixels(frame, FB_PIXELS); }
  tft.endWrite();
}
static inline void BlendInto(uint16_t *p, uint16_t c, uint8_t a) {
  if (a >= 250) { *p = c; return; }
  uint16_t d = *p;
  uint16_t dr = (d>>11)&0x1F, dg = (d>>5)&0x3F, db = d&0x1F;
  uint16_t sr = (c>>11)&0x1F, sg = (c>>5)&0x3F, sb = c&0x1F;
  uint16_t ia = 255-a;
  *p = (uint16_t)((((sr*a+dr*ia)>>8)<<11) | (((sg*a+dg*ia)>>8)<<5) | ((sb*a+db*ia)>>8));
}
static inline void AddInto(uint16_t *p, uint16_t c, uint8_t amt) {
  uint16_t d = *p;
  int r = ((d>>11)&0x1F) + ((((c>>11)&0x1F)*amt)>>8);
  int g = ((d>>5)&0x3F)  + ((((c>>5)&0x3F)*amt)>>8);
  int b = (d&0x1F)       + (((c&0x1F)*amt)>>8);
  if (r>31) r=31; if (g>63) g=63; if (b>31) b=31;
  *p = (r<<11)|(g<<5)|b;
}
void PxBlend(int x, int y, uint16_t c, uint8_t a) {
  if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H) return;
  BlendInto(&frame[y*SCREEN_W+x], c, a);
}
void PxAdd(int x, int y, uint16_t c, uint8_t amt) {
  if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H) return;
  AddInto(&frame[y*SCREEN_W+x], c, amt);
}
void FillRectFB(int x, int y, int w, int h, uint16_t c) {
  if (x<0){w+=x;x=0;} if (y<0){h+=y;y=0;}
  if (x+w>SCREEN_W) w=SCREEN_W-x;
  if (y+h>SCREEN_H) h=SCREEN_H-y;
  if (w<=0||h<=0) return;
  for (int j=0;j<h;j++){ uint16_t *p=&frame[(y+j)*SCREEN_W+x];
    for (int i=0;i<w;i++) p[i]=c; }
}
void BlendRectFB(int x, int y, int w, int h, uint16_t c, uint8_t a) {
  if (x<0){w+=x;x=0;} if (y<0){h+=y;y=0;}
  if (x+w>SCREEN_W) w=SCREEN_W-x;
  if (y+h>SCREEN_H) h=SCREEN_H-y;
  if (w<=0||h<=0) return;
  for (int j=0;j<h;j++){ uint16_t *p=&frame[(y+j)*SCREEN_W+x];
    for (int i=0;i<w;i++) BlendInto(&p[i], c, a); }
}
void HLineFB(int x,int y,int w,uint16_t c){ FillRectFB(x,y,w,1,c); }
void VLineFB(int x,int y,int h,uint16_t c){ FillRectFB(x,y,1,h,c); }
void LineFB(int x0,int y0,int x1,int y1,uint16_t c,uint8_t a){
  int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy, guard=dx-dy+4;
  while (guard-->0){ PxBlend(x0,y0,c,a);
    if (x0==x1&&y0==y1) break;
    int e2=2*err;
    if (e2>=dy){err+=dy;x0+=sx;}
    if (e2<=dx){err+=dx;y0+=sy;} }
}
void LineAdd(int x0,int y0,int x1,int y1,uint16_t c,uint8_t amt){
  int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy, guard=dx-dy+4;
  while (guard-->0){ PxAdd(x0,y0,c,amt);
    if (x0==x1&&y0==y1) break;
    int e2=2*err;
    if (e2>=dy){err+=dy;x0+=sx;}
    if (e2<=dx){err+=dx;y0+=sy;} }
}
void RingFB(int cx,int cy,int r,uint16_t c,uint8_t a){
  if (r<=0||a==0) return;
  int steps=r*6+10;
  for (int i=0;i<steps;i++){ float g=TAU*i/steps;
    PxBlend(cx+(int)(fcos(g)*r), cy+(int)(fsin(g)*r), c, a); }
}
void ArcFB(int cx,int cy,int r,float a0,float a1,uint16_t c,uint8_t a){
  if (r<=0||a==0) return;
  int steps=(int)(fabsf(a1-a0)*r)+4;
  for (int i=0;i<=steps;i++){ float g=a0+(a1-a0)*i/steps;
    PxBlend(cx+(int)(fcos(g)*r), cy+(int)(fsin(g)*r), c, a); }
}
void CircleFB(int cx,int cy,int r,uint16_t c,uint8_t a){
  if (r<0) return;
  for (int y=-r;y<=r;y++){ int w=(int)(sqrtf((float)(r*r-y*y))+0.5f);
    BlendRectFB(cx-w, cy+y, 2*w+1, 1, c, a); }
}
void HexFB(int cx,int cy,int r,uint16_t c,uint8_t a,bool fill){
  int px[6],py[6];
  for (int i=0;i<6;i++){ float g=(float)M_PI/3.0f*i-(float)M_PI/2.0f;
    px[i]=cx+(int)(fcos(g)*r); py[i]=cy+(int)(fsin(g)*r); }
  if (fill) for (int y=-r;y<=r;y++){
    float fy=fabsf((float)y)/r;
    float hw=r*0.866f*(1.0f-clampf((fy-0.5f)*2.0f,0,1));
    if (hw>0) BlendRectFB(cx-(int)hw, cy+y, (int)(hw*2), 1, c, a); }
  for (int i=0;i<6;i++) LineFB(px[i],py[i],px[(i+1)%6],py[(i+1)%6],c,a);
}
void BuildGlowLUT(void){
  for (int y=0;y<GLOW_D;y++) for (int x=0;x<GLOW_D;x++){
    float dx=x-GLOW_R, dy=y-GLOW_R;
    float d=sqrtf(dx*dx+dy*dy)/GLOW_R;
    float v=clampf(1.0f-d,0,1);
    glowLUT[y*GLOW_D+x]=(uint8_t)(v*v*v*255.0f); }
}
// Additive bloom sprite. Respects the glow setting so it can be dialled
// down for speed without touching any call site.
void Glow(int cx,int cy,uint16_t c,uint8_t amt,float sc){
  if (gGlowLevel==0) return;
  if (gGlowLevel==1) { amt=(uint8_t)(amt>>1); sc*=0.75f; }
  int r=(int)(GLOW_R*sc); if (r<1) r=1;
  int x0=cx-r,x1=cx+r,y0=cy-r,y1=cy+r;
  if (x1<0||y1<0||x0>=SCREEN_W||y0>=SCREEN_H) return;
  if (x0<0) x0=0; if (y0<0) y0=0;
  if (x1>SCREEN_W-1) x1=SCREEN_W-1;
  if (y1>SCREEN_H-1) y1=SCREEN_H-1;
  float inv=(float)GLOW_R/r;
  for (int y=y0;y<=y1;y++){
    uint16_t *row=&frame[y*SCREEN_W];
    int ly=(int)((y-cy)*inv)+GLOW_R;
    if ((unsigned)ly>=GLOW_D) continue;
    const uint8_t *lr=&glowLUT[ly*GLOW_D];
    for (int x=x0;x<=x1;x++){
      int lx=(int)((x-cx)*inv)+GLOW_R;
      if ((unsigned)lx>=GLOW_D) continue;
      uint8_t v=lr[lx];
      if (v) AddInto(&row[x], c, (uint8_t)((v*amt)>>8)); } }
}
// PERFORMANCE: full-screen dimming without a full-screen blend.
// Draws every 2nd scanline at double strength -- half the pixels touched,
// visually equivalent at this size, ~6 ms saved per call.
void Scrim(uint8_t strength){
  for (int y=0;y<SCREEN_H;y+=2)
    BlendRectFB(0,y,SCREEN_W,1,C_BG,strength);
}

// =====================================================================
//  PARTICLES
// =====================================================================
void SpawnBurst(float x,float y,int n,uint16_t c,float sp,uint8_t k){
  if (gFxLevel==0) return;
  if (gFxLevel==1) n=(n+1)/2;
  for (int i=0;i<n;i++){
    Particle &p=parts[partHead];
    partHead=(partHead+1)%NUM_PART;
    float a=Hash((uint32_t)(gTime*1000)+i*7919u+partHead*104729u)*TAU;
    float s=sp*(0.35f+0.65f*Hash(i*6151u+partHead*53u));
    p.x=x; p.y=y; p.vx=fcos(a)*s; p.vy=fsin(a)*s;
    p.life=(k==PK_EMBER)?1.1f:0.55f; p.inv=1.0f/p.life;
    p.col=c; p.kind=k;
  }
}
void UpdateParticles(float dt){
  for (int i=0;i<NUM_PART;i++){
    Particle &p=parts[i];
    if (p.life<=0) continue;
    p.life-=dt; p.x+=p.vx*dt; p.y+=p.vy*dt;
    if (p.kind==PK_EMBER){ p.vy+=42.0f*dt; p.vx*=expf(-1.2f*dt); }
    else { float d=expf(-2.6f*dt); p.vx*=d; p.vy*=d; } }
}
void DrawParticles(void){
  for (int i=0;i<NUM_PART;i++){
    Particle &p=parts[i];
    if (p.life<=0) continue;
    float t=p.life*p.inv;
    uint8_t amt=(uint8_t)(255.0f*t*t);
    int x=(int)p.x,y=(int)p.y;
    if (p.kind==PK_SPARK){
      LineAdd((int)(p.x-p.vx*0.02f),(int)(p.y-p.vy*0.02f),x,y,p.col,amt);
      if (t>0.8f) Glow(x,y,p.col,(uint8_t)(amt>>2),0.4f);
    } else { PxAdd(x,y,p.col,amt); PxAdd(x+1,y,p.col,amt>>1); PxAdd(x,y+1,p.col,amt>>1); } }
}

// =====================================================================
//  TEXT
// =====================================================================
void DrawChar(int x,int y,char ch,uint16_t c,uint8_t s){
  int idx=FontIndex(ch);
  if (idx<0) return;
  for (int cx=0;cx<5;cx++){
    uint8_t bits=pgm_read_byte(&FONT5x7[idx][cx]);
    if (!bits) continue;
    for (int cy=0;cy<7;cy++)
      if (bits&(1<<cy)) FillRectFB(x+cx*s,y+cy*s,s,s,c); }
}
void DrawText(int x,int y,const char *t,uint16_t c,uint8_t s){
  while (*t){ DrawChar(x,y,*t,c,s); x+=6*s; t++; }
}
int TextW(const char *t,uint8_t s){ return (int)strlen(t)*6*s; }
void DrawTextC(int cx,int y,const char *t,uint16_t c,uint8_t s){
  DrawText(cx-TextW(t,s)/2,y,t,c,s);
}
// Text with a soft additive bloom behind it -- the core of the glow look.
void GlowText(int x,int y,const char *t,uint16_t c,uint8_t s,uint8_t amt){
  if (gGlowLevel){
    int w=TextW(t,s), h=7*s;
    int n=(w/(6*s))+1;
    for (int i=0;i<n;i++) Glow(x+i*6*s+3*s, y+h/2, c, amt, 0.42f*s+0.32f);
  }
  DrawText(x,y,t,c,s);
}
void GlowTextC(int cx,int y,const char *t,uint16_t c,uint8_t s,uint8_t amt){
  GlowText(cx-TextW(t,s)/2,y,t,c,s,amt);
}
void DrawTextDecode(int x,int y,const char *t,uint16_t c,uint8_t s,float p){
  int n=(int)strlen(t);
  float head=p*(n+4.0f);
  static const char SOUP[]="01234789ABCDEFXZ/+-<>";
  for (int i=0;i<n;i++){
    char ch=t[i];
    if (ch==' '){ x+=6*s; continue; }
    float d=head-i;
    if (d<=0){ x+=6*s; continue; }
    if (d<1.6f){
      uint32_t h=(uint32_t)(gTime*45.0f)+i*131u;
      ch=SOUP[(uint32_t)(Hash(h)*20.0f)%20];
      int j=(int)((Hash(h*7u)-0.5f)*3.0f);
      DrawChar(x,y+j,ch,Fade(c,(uint8_t)(120+100*Hash(h*3u))),s);
    } else DrawChar(x,y,ch,c,s);
    x+=6*s; }
}

// =====================================================================
//  V8 ICON SYSTEM
//  ONE visual language for every glyph in the OS:
//    * outline only -- no filled shapes, no mixed weights
//    * 1 px stroke, always
//    * drawn on a normalised square of half-extent `s`, centred (cx,cy)
//    * silhouettes built from circle / square / triangle / line only
//    * no animation inside icons -- motion belongs to the UI, not the mark
//  Callers pass a size so the same glyph works at 8 px in a list row and
//  at 14 px on a category card without redrawing it differently.
// =====================================================================
enum {
  IC_STUDY=0, IC_LAB, IC_CREATE, IC_PLAY, IC_SYSTEM, IC_HOME,
  IC_CUBE, IC_SPHERE, IC_GRID, IC_LAYERS, IC_TARGET, IC_WARP,
  IC_MAZE, IC_GAMEPAD, IC_BRICK, IC_BIRD, IC_SNAKE, IC_PADDLE,
  IC_BLOCKS, IC_CARDS, IC_BULB, IC_MINE, IC_HAMMER, IC_SHIELD,
  IC_BRUSH, IC_CLOCK, IC_STOPWATCH, IC_TIMER, IC_WIFI, IC_GEAR,
  IC_CROSSHAIR, IC_CHIP, IC_BOOK, IC_CHECK, IC_FLAG, IC_CHART,
  IC_HISTORY, IC_NOTE, IC_QUOTE, IC_ATOM, IC_PARTICLE, IC_STAR,
  IC_PLANET, IC_FRACTAL, IC_RAIN, IC_WAVE, IC_LIFE, IC_PLAYBTN,
  IC_MOLECULE, IC_TOUCH, IC_SEARCH, IC_COMPOSE, IC_GESTURE, IC_MORPH,
  IC_KALEIDO, IC_EXPLODE, IC_VOXEL, IC_IMPOSSIBLE, IC_TUNNEL, IC_WELL,
  IC_FLOCK, IC_FISH, IC_ANT, IC_MAGNET, IC_FIGURE, IC_TIMELINE,
  IC_FUNCTION, IC_CURVE, IC_SURFACE, IC_VECTOR, IC_MATRIX, IC_FOURIER,
  IC_TROPHY, IC_TERMINAL, IC_DISPLAY, IC_SLIDERS, IC_INFO, IC_TETRIS,
       // ---- names from the NEXUS ICON FAMILY sheet ----
       IC_ABOUT_, IC_POWER, IC_WARNING, IC_ERROR, IC_LOCK, IC_UNLOCK,
       IC_REFRESH, IC_MENU, IC_MORE, IC_BACK, IC_CLOSE, IC_SIGMA,
       IC_FOCUS_, IC_CALC, IC_STREAK, IC_CALENDAR, IC_ADD, IC_EDIT,
       IC_DELETE, IC_SAVE, IC_SIMULATION, IC_GALLERY, IC_CAMERA,
       IC_REMOTE, IC_WIRELESS, IC_PERF, IC_PAUSE, IC_STOP, IC_NEXT,
       IC_PREV, IC_VOLUME, IC_BRIGHT,
       IC_COUNT
};

// stroke helpers -- every icon is built only from these
static inline void IkLine(int x0,int y0,int x1,int y1,uint16_t c){
  LineFB(x0,y0,x1,y1,c,255); }
static inline void IkRect(int x,int y,int w,int h,uint16_t c){
  HLineFB(x,y,w,c); HLineFB(x,y+h-1,w,c);
  VLineFB(x,y,h,c); VLineFB(x+w-1,y,h,c); }
static inline void IkCircle(int cx,int cy,int r,uint16_t c){
  RingFB(cx,cy,r,c,255); }
static inline void IkDot(int x,int y,uint16_t c){ PxBlend(x,y,c,255); }
static inline void IkTri(int cx,int cy,int r,uint16_t c){
  IkLine(cx,cy-r,cx+r,cy+r,c); IkLine(cx+r,cy+r,cx-r,cy+r,c);
  IkLine(cx-r,cy+r,cx,cy-r,c); }

// =====================================================================
//  NEXUS ICON FAMILY
//  One monoline system, drawn from the reference sheet.
//
//  Rules taken from the sheet and enforced here:
//   * OUTLINE ONLY. No filled silhouettes except intentional dots.
//   * ONE stroke weight per render. `w` is the pen width in px; the
//     whole mark scales but the pen stays visually constant, which is
//     what makes 20px and 48px read as the same family.
//   * Marks are authored on a normalised square of half-extent `r`.
//   * Geometry is circle / rect / line / arc only.
//   * Optical sizing: circular marks get the full `r`, square marks are
//     inset ~8% so they do not look heavier than the round ones.
//
//  Both the flat UI icons and the launcher app logos call this, so the
//  two can never drift apart.
// =====================================================================

// ---- pen primitives --------------------------------------------------
// A stroke of width w is drawn as a w*w brush swept along the path.
static inline void PenDot(int x,int y,int w,uint16_t c){
  if (w<=1) PxBlend(x,y,c,255);
  else FillRectFB(x-(w>>1),y-(w>>1),w,w,c);
}
static void PenLine(int x0,int y0,int x1,int y1,int w,uint16_t c){
  int dx=abs(x1-x0), sx=x0<x1?1:-1;
  int dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy, guard=dx-dy+4;
  while (guard-->0){
    PenDot(x0,y0,w,c);
    if (x0==x1&&y0==y1) break;
    int e2=2*err;
    if (e2>=dy){ err+=dy; x0+=sx; }
    if (e2<=dx){ err+=dx; y0+=sy; } }
}
static void PenRect(int x,int y,int ww,int hh,int w,uint16_t c){
  PenLine(x,y,x+ww,y,w,c);
  PenLine(x+ww,y,x+ww,y+hh,w,c);
  PenLine(x+ww,y+hh,x,y+hh,w,c);
  PenLine(x,y+hh,x,y,w,c);
}
// arc from a0..a1 degrees, radius rr
static void PenArc(int cx,int cy,int rr,int a0,int a1,int w,uint16_t c){
  if (rr<1) return;
  int step=(rr>14)?4:(rr>7?6:9);
  int px=0,py=0; bool first=true;
  for (int a=a0;;a+=step){
    if (a>a1) a=a1;
    float u=a*0.017453f;
    int x=cx+(int)(fcos(u)*rr), y=cy+(int)(fsin(u)*rr);
    if (!first) PenLine(px,py,x,y,w,c);
    px=x; py=y; first=false;
    if (a>=a1) break; }
}
static inline void PenCirc(int cx,int cy,int rr,int w,uint16_t c){
  PenArc(cx,cy,rr,0,360,w,c);
}
// small solid dot (the sheet uses these for MORE, PARTICLES, etc.)
static void PenBlob(int cx,int cy,int rr,uint16_t c){
  if (rr<1){ PxBlend(cx,cy,c,255); return; }
  for (int j=-rr;j<=rr;j++){
    int hw=(int)sqrtf((float)(rr*rr-j*j));
    if (hw>0) FillRectFB(cx-hw,cy+j,hw*2,1,c);
    else PxBlend(cx,cy+j,c,255); }
}
// rounded-corner outline rect (screens, cards, calculators)
static void PenRRect(int x,int y,int ww,int hh,int rad,int w,uint16_t c){
  if (rad*2>ww) rad=ww/2;
  if (rad*2>hh) rad=hh/2;
  PenLine(x+rad,y,x+ww-rad,y,w,c);
  PenLine(x+rad,y+hh,x+ww-rad,y+hh,w,c);
  PenLine(x,y+rad,x,y+hh-rad,w,c);
  PenLine(x+ww,y+rad,x+ww,y+hh-rad,w,c);
  if (rad>0){
    PenArc(x+rad,y+rad,rad,180,270,w,c);
    PenArc(x+ww-rad,y+rad,rad,270,360,w,c);
    PenArc(x+ww-rad,y+hh-rad,rad,0,90,w,c);
    PenArc(x+rad,y+hh-rad,rad,90,180,w,c); }
}
// arrowhead used by REFRESH / NEXT / BACK
static void PenChevron(int x,int y,int sz,int dir,int w,uint16_t c){
  // dir 0=left 1=right 2=up 3=down
  switch (dir){
    case 0: PenLine(x+sz,y-sz,x,y,w,c); PenLine(x,y,x+sz,y+sz,w,c); break;
    case 1: PenLine(x-sz,y-sz,x,y,w,c); PenLine(x,y,x-sz,y+sz,w,c); break;
    case 2: PenLine(x-sz,y+sz,x,y,w,c); PenLine(x,y,x+sz,y+sz,w,c); break;
    default:PenLine(x-sz,y-sz,x,y,w,c); PenLine(x,y,x+sz,y-sz,w,c); break; }
}
// solid triangle (PLAY, warning fill is outline so this is play only)
static void PenTriFill(int ax,int ay,int bx,int by,int cx2,int cy2,uint16_t c){
  int mny=ay<by?(ay<cy2?ay:cy2):(by<cy2?by:cy2);
  int mxy=ay>by?(ay>cy2?ay:cy2):(by>cy2?by:cy2);
  for (int y=mny;y<=mxy;y++){
    int lo=32767,hi=-32768;
    int X[3]={ax,bx,cx2},Y[3]={ay,by,cy2};
    for (int e=0;e<3;e++){
      int n=(e+1)%3;
      if ((y>=Y[e]&&y<Y[n])||(y>=Y[n]&&y<Y[e])){
        int xx=X[e]+(X[n]-X[e])*(y-Y[e])/(Y[n]-Y[e]);
        if(xx<lo)lo=xx; if(xx>hi)hi=xx; } }
    if (lo<=hi) FillRectFB(lo,y,hi-lo+1,1,c); }
}

// =====================================================================
//  THE FAMILY
//  r = half-extent, w = pen width.
// =====================================================================
void IconPack(uint8_t id,int cx,int cy,int r,uint16_t c,int w){
  if (r<3) r=3;
  if (w<1) w=1;
  const int q=r/2, t=(r*2)/3, s=(r*7)/8;   // common sub-radii
  switch (id){

  // ------------------------------------------------ SYSTEM
  case IC_HOME:
    PenLine(cx-r,cy,cx,cy-r,w,c);
    PenLine(cx,cy-r,cx+r,cy,w,c);
    PenLine(cx-t,cy-q/2,cx-t,cy+r,w,c);
    PenLine(cx+t,cy-q/2,cx+t,cy+r,w,c);
    PenLine(cx-t,cy+r,cx+t,cy+r,w,c);
    PenRect(cx-q/2,cy+q/2,q,r-q/2,w,c);          // door
    break;
  case IC_SEARCH:
    PenCirc(cx-q/3,cy-q/3,t,w,c);
    PenLine(cx-q/3+(int)(t*0.7f),cy-q/3+(int)(t*0.7f),cx+r,cy+r,w,c);
    break;
  case IC_GEAR: case IC_SYSTEM: case IC_CHIP: {
    // Teeth are SHORT stubs sitting just outside the body ring. Long
    // spokes starting inside the ring read as a sunburst, not a gear.
    PenCirc(cx,cy,t,w,c);
    PenCirc(cx,cy,q-1,w,c);
    for (int i=0;i<6;i++){ float u=TAU*i/6.0f;
      int x0=cx+(int)(fcos(u)*(t+1)), y0=cy+(int)(fsin(u)*(t+1));
      int x1=cx+(int)(fcos(u)*r),     y1=cy+(int)(fsin(u)*r);
      PenLine(x0,y0,x1,y1,w,c); }
  } break;
  case IC_INFO: case IC_ABOUT_:
    PenCirc(cx,cy,s,w,c);
    PenBlob(cx,cy-q,(w>1)?2:1,c);
    PenLine(cx,cy-q/3,cx,cy+q+1,w,c);
    break;
  case IC_TERMINAL:                              // </>  developer
    PenChevron(cx-q-2,cy,q,0,w,c);
    PenChevron(cx+q+2,cy,q,1,w,c);
    PenLine(cx+q/2,cy-t,cx-q/2,cy+t,w,c);
    break;
  case IC_TROPHY:
    PenLine(cx-q,cy-r,cx+q,cy-r,w,c);
    PenLine(cx-q,cy-r,cx-q,cy-q/2,w,c);
    PenLine(cx+q,cy-r,cx+q,cy-q/2,w,c);
    PenArc(cx,cy-q/2,q,0,180,w,c);
    PenArc(cx-q,cy-q,q/2+1,90,270,w,c);          // handles
    PenArc(cx+q,cy-q,q/2+1,270,360,w,c);
    PenArc(cx+q,cy-q,q/2+1,0,90,w,c);
    PenLine(cx,cy+q/2,cx,cy+t,w,c);
    PenLine(cx-q,cy+r,cx+q,cy+r,w,c);
    PenLine(cx-q/2,cy+t,cx+q/2,cy+t,w,c);
    break;
  case IC_POWER:
    PenArc(cx,cy+q/3,t,-60,240,w,c);
    PenLine(cx,cy-r,cx,cy-q/3,w,c);
    break;
  case IC_WARNING:
    PenLine(cx,cy-r,cx+r,cy+t,w,c);
    PenLine(cx+r,cy+t,cx-r,cy+t,w,c);
    PenLine(cx-r,cy+t,cx,cy-r,w,c);
    PenLine(cx,cy-q/2,cx,cy+q/2,w,c);
    PenBlob(cx,cy+t-q/2,(w>1)?2:1,c);
    break;
  case IC_ERROR:
    PenCirc(cx,cy,s,w,c);
    PenLine(cx-q,cy-q,cx+q,cy+q,w,c);
    PenLine(cx+q,cy-q,cx-q,cy+q,w,c);
    break;
  case IC_LOCK: case IC_UNLOCK:
    PenRRect(cx-t,cy-q/3,t*2,r+q/3,2,w,c);
    if (id==IC_LOCK) PenArc(cx,cy-q/3,q+1,180,360,w,c);
    else             PenArc(cx-q,cy-q/3,q+1,180,330,w,c);
    PenBlob(cx,cy+q/2,(w>1)?2:1,c);
    break;
  case IC_REFRESH:
    PenArc(cx,cy,t,40,320,w,c);
    PenLine(cx+(int)(t*0.77f),cy-(int)(t*0.64f),cx+r,cy-t,w,c);
    PenLine(cx+(int)(t*0.77f),cy-(int)(t*0.64f),cx+q,cy-r+1,w,c);
    break;
  case IC_MENU:
    for (int i=-1;i<=1;i++) PenLine(cx-r,cy+i*q,cx+r,cy+i*q,w,c);
    break;
  case IC_MORE:
    for (int i=-1;i<=1;i++) PenBlob(cx+i*q,cy,(w>1)?2:1,c);
    break;
  case IC_BACK:
    PenChevron(cx+q/2,cy,t,0,w,c);
    break;
  case IC_CLOSE:
    PenLine(cx-t,cy-t,cx+t,cy+t,w,c);
    PenLine(cx+t,cy-t,cx-t,cy+t,w,c);
    break;

  // ------------------------------------------------ JEE / PRODUCTIVITY
  case IC_SIGMA:                                  // JEE tracker
    PenLine(cx-t,cy-r,cx+t,cy-r,w,c);
    PenLine(cx-t,cy-r,cx,cy,w,c);
    PenLine(cx,cy,cx-t,cy+r,w,c);
    PenLine(cx-t,cy+r,cx+t,cy+r,w,c);
    break;
  case IC_BOOK: case IC_STUDY:                    // open book
    // Two symmetric pages meeting at a vertical spine. Drawn as plain
    // quads -- the previous arc version collapsed into a wedge.
    PenLine(cx,cy-t,cx,cy+t,w,c);                 // spine
    PenLine(cx,cy-t,cx-r,cy-q,w,c);               // left page top
    PenLine(cx-r,cy-q,cx-r,cy+q,w,c);             // left outer edge
    PenLine(cx-r,cy+q,cx,cy+t,w,c);               // left page bottom
    PenLine(cx,cy-t,cx+r,cy-q,w,c);               // right page top
    PenLine(cx+r,cy-q,cx+r,cy+q,w,c);             // right outer edge
    PenLine(cx+r,cy+q,cx,cy+t,w,c);               // right page bottom
    break;
  case IC_LAB: case IC_FOCUS_:                    // focus diamond + ticks
    PenLine(cx,cy-t,cx+t,cy,w,c);
    PenLine(cx+t,cy,cx,cy+t,w,c);
    PenLine(cx,cy+t,cx-t,cy,w,c);
    PenLine(cx-t,cy,cx,cy-t,w,c);
    PenBlob(cx,cy,(w>1)?2:1,c);                   // the focal point
    PenLine(cx,cy-r,cx,cy-t,w,c); PenLine(cx,cy+t,cx,cy+r,w,c);
    PenLine(cx-r,cy,cx-t,cy,w,c); PenLine(cx+t,cy,cx+r,cy,w,c);
    break;
  case IC_NOTE:
    PenRect(cx-t,cy-r,t*2,r*2,w,c);
    PenLine(cx-q,cy-q-1,cx+q,cy-q-1,w,c);
    PenLine(cx-q,cy+1,cx+q,cy+1,w,c);
    PenLine(cx-q,cy+q+2,cx,cy+q+2,w,c);
    break;
  case IC_CALC:
    PenRRect(cx-t,cy-r,t*2,r*2,2,w,c);
    PenRect(cx-q,cy-t,q*2,q,w,c);
    for (int j=0;j<2;j++) for (int i=0;i<3;i++)
      PenBlob(cx-q+i*q,cy+q/2+j*q,(w>1)?2:1,c);
    break;
  case IC_FLAG:                                   // goals
    PenLine(cx-q,cy-r,cx-q,cy+r,w,c);
    PenLine(cx-q,cy-r,cx+t,cy-t,w,c);
    PenLine(cx+t,cy-t,cx-q,cy,w,c);
    break;
  case IC_CHART:                                  // statistics
    for (int i=0;i<3;i++)
      PenRect(cx-r+i*(t),cy+r-(i+1)*(q+1),q,(i+1)*(q+1),w,c);
    break;
  case IC_STREAK:                                 // flame
    PenArc(cx,cy+q/2,t,0,180,w,c);
    PenLine(cx-t,cy+q/2,cx-q/2,cy-q,w,c);
    PenLine(cx-q/2,cy-q,cx,cy-r,w,c);
    PenLine(cx,cy-r,cx+q/2,cy-q,w,c);
    PenLine(cx+q/2,cy-q,cx+t,cy+q/2,w,c);
    break;
  case IC_CLOCK:
    PenCirc(cx,cy,r,w,c);
    PenLine(cx,cy,cx,cy-t+1,w,c);                 // minute hand
    PenLine(cx,cy,cx+q,cy,w,c);                   // hour hand
    PenBlob(cx,cy,1,c);
    break;
  case IC_TIMER: case IC_STOPWATCH: {
    // Stopwatch per the sheet: body ring sits low, flat crown on top,
    // one hand pointing up-right. The old version drew a stem straight
    // through the dial, which read as a keyhole.
    int by=cy+q/3;                                 // body centre
    PenCirc(cx,by,t,w,c);
    PenLine(cx-q/2,cy-r,cx+q/2,cy-r,w,c);          // crown bar
    PenLine(cx,cy-r,cx,by-t,w,c);                  // stem, stops AT the ring
    PenLine(cx,by,cx+(int)(t*0.55f),by-(int)(t*0.55f),w,c);
    PenBlob(cx,by,1,c);
  } break;
  case IC_CALENDAR:
    PenRect(cx-t,cy-t,t*2,t*2,w,c);
    PenLine(cx-t,cy-q,cx+t,cy-q,w,c);
    PenLine(cx-q,cy-r,cx-q,cy-t,w,c);
    PenLine(cx+q,cy-r,cx+q,cy-t,w,c);
    PenBlob(cx-q/2,cy+q/2,(w>1)?2:1,c);
    PenBlob(cx+q/2,cy+q/2,(w>1)?2:1,c);
    break;
  case IC_CHECK:
    PenCirc(cx,cy,s,w,c);
    PenLine(cx-q,cy,cx-q/3,cy+q,w,c);
    PenLine(cx-q/3,cy+q,cx+q,cy-q/2,w,c);
    break;
  case IC_ADD:
    PenCirc(cx,cy,s,w,c);
    PenLine(cx-q,cy,cx+q,cy,w,c);
    PenLine(cx,cy-q,cx,cy+q,w,c);
    break;
  case IC_EDIT:                                   // pencil
    PenLine(cx-t,cy+t,cx+q,cy-t,w,c);
    PenLine(cx-t,cy+t,cx-q,cy+q,w,c);
    PenLine(cx-q,cy+q,cx+t,cy-q,w,c);
    PenLine(cx+q,cy-t,cx+t,cy-q,w,c);
    break;
  case IC_DELETE:                                 // trash
    PenLine(cx-t,cy-q,cx+t,cy-q,w,c);
    PenRect(cx-q,cy-q,q*2,r+q/2,w,c);
    PenLine(cx-q/2,cy-q,cx-q/2,cy-t,w,c);
    PenLine(cx+q/2,cy-q,cx+q/2,cy-t,w,c);
    PenLine(cx-q/2,cy-t,cx+q/2,cy-t,w,c);
    PenLine(cx,cy,cx,cy+q,w,c);
    break;
  case IC_SAVE:                                   // floppy
    PenRect(cx-t,cy-t,t*2,t*2,w,c);
    PenRect(cx-q/2,cy-t,q,q,w,c);
    PenRect(cx-q,cy+q/3,q*2,t-q/3,w,c);
    break;

  // ------------------------------------------------ SCIENCE / CREATIVE
  case IC_FUNCTION:                               // math lab
    PenCirc(cx,cy,s,w,c);
    PenLine(cx-q,cy+q,cx+q,cy-q,w,c);
    break;
  case IC_ATOM: {                                 // physics lab
    // Nucleus plus a flattened orbit. Two same-radius arcs simply
    // retraced the circle, which is why it read as a plain ring.
    PenBlob(cx,cy,(w>1)?2:1,c);
    for (int a=0;a<360;a+=10){
      float u0=a*0.017453f, u1=(a+10)*0.017453f;
      PenLine(cx+(int)(fcos(u0)*r),   cy+(int)(fsin(u0)*q),
              cx+(int)(fcos(u1)*r),   cy+(int)(fsin(u1)*q), w,c); }
    for (int a=0;a<360;a+=10){
      float u0=a*0.017453f, u1=(a+10)*0.017453f;
      PenLine(cx+(int)(fsin(u0)*q),   cy+(int)(fcos(u0)*r),
              cx+(int)(fsin(u1)*q),   cy+(int)(fcos(u1)*r), w,c); }
  } break;
  case IC_CUBE:                                   // 3D lab
    PenLine(cx,cy-r,cx+r,cy-q,w,c);
    PenLine(cx+r,cy-q,cx+r,cy+q,w,c);
    PenLine(cx+r,cy+q,cx,cy+r,w,c);
    PenLine(cx,cy+r,cx-r,cy+q,w,c);
    PenLine(cx-r,cy+q,cx-r,cy-q,w,c);
    PenLine(cx-r,cy-q,cx,cy-r,w,c);
    PenLine(cx,cy-r,cx,cy,w,c);
    PenLine(cx,cy,cx+r,cy-q,w,c);
    PenLine(cx,cy,cx-r,cy-q,w,c);
    break;
  case IC_SIMULATION:
    PenCirc(cx,cy,s,w,c);
    PenLine(cx-t,cy-t,cx+t,cy+t,w,c);
    PenLine(cx+t,cy-t,cx-t,cy+t,w,c);
    break;
  case IC_FRACTAL:
    PenCirc(cx,cy,r,w,c);
    PenCirc(cx,cy,t,w,c);
    PenBlob(cx,cy,(w>1)?3:2,c);
    break;
  case IC_PARTICLE:
    for (int i=0;i<8;i++){ float u=TAU*i/8.0f;
      PenBlob(cx+(int)(fcos(u)*((i&1)?r:q)),
              cy+(int)(fsin(u)*((i&1)?r:q)),(w>1)?2:1,c); }
    PenBlob(cx,cy,(w>1)?2:1,c);
    break;
  case IC_COMPOSE: case IC_CREATE:                // creator droplet
    PenLine(cx,cy-r,cx+t,cy+q/2,w,c);
    PenArc(cx,cy+q/2,t,0,180,w,c);
    PenLine(cx-t,cy+q/2,cx,cy-r,w,c);
    break;
  case IC_GALLERY:
    PenRect(cx-r,cy-t,r*2,t*2,w,c);
    PenLine(cx-r,cy+q,cx-q,cy-q/2,w,c);
    PenLine(cx-q,cy-q/2,cx+q/2,cy+t,w,c);
    PenBlob(cx+q,cy-q,(w>1)?2:1,c);
    break;

  // ------------------------------------------------ HARDWARE
  case IC_CAMERA:
    PenRRect(cx-r,cy-q,r*2,t+q,2,w,c);
    PenLine(cx-q,cy-q,cx-q/2,cy-t,w,c);
    PenLine(cx-q/2,cy-t,cx+q/2,cy-t,w,c);
    PenLine(cx+q/2,cy-t,cx+q,cy-q,w,c);
    PenCirc(cx,cy+q/3,q,w,c);
    break;
  case IC_WIFI:
    for (int i=1;i<=3;i++) PenArc(cx,cy+t,(r*i)/3,200,340,w,c);
    PenBlob(cx,cy+t,(w>1)?2:1,c);
    break;
  case IC_WIRELESS:
    PenBlob(cx,cy,(w>1)?2:1,c);
    PenArc(cx,cy,q+1,300,420,w,c);  PenArc(cx,cy,q+1,120,240,w,c);
    PenArc(cx,cy,r,310,410,w,c);    PenArc(cx,cy,r,130,230,w,c);
    break;
  case IC_DISPLAY:
    PenRRect(cx-r,cy-t,r*2,t+q,2,w,c);
    PenLine(cx,cy+q,cx,cy+t,w,c);
    PenLine(cx-q,cy+t,cx+q,cy+t,w,c);
    break;
  case IC_TOUCH:
    PenArc(cx,cy,t,180,360,w,c);
    PenLine(cx-t,cy,cx-t,cy+q,w,c);
    PenLine(cx+t,cy,cx+t,cy+q,w,c);
    PenArc(cx,cy+q,t,0,180,w,c);
    PenLine(cx,cy-t,cx,cy+q/2,w,c);
    break;
  case IC_CROSSHAIR:
    PenCirc(cx,cy,t,w,c);
    PenLine(cx-r,cy,cx-t,cy,w,c); PenLine(cx+t,cy,cx+r,cy,w,c);
    PenLine(cx,cy-r,cx,cy-t,w,c); PenLine(cx,cy+t,cx,cy+r,w,c);
    PenBlob(cx,cy,(w>1)?2:1,c);
    break;
  case IC_PERF:                                   // gauge
    PenArc(cx,cy+q/2,r,180,360,w,c);
    PenLine(cx,cy+q/2,cx+(int)(t*0.7f),cy+q/2-(int)(t*0.7f),w,c);
    PenBlob(cx,cy+q/2,(w>1)?2:1,c);
    break;

  // ------------------------------------------------ MEDIA
  case IC_PLAYBTN:
    PenTriFill(cx-q,cy-t,cx+t,cy,cx-q,cy+t,c);
    break;
  case IC_PAUSE:
    PenLine(cx-q,cy-t,cx-q,cy+t,w+1,c);
    PenLine(cx+q,cy-t,cx+q,cy+t,w+1,c);
    break;
  case IC_STOP:
    PenRect(cx-t,cy-t,t*2,t*2,w,c);
    break;
  case IC_NEXT:
    PenTriFill(cx-t,cy-q,cx,cy,cx-t,cy+q,c);
    PenTriFill(cx,cy-q,cx+t,cy,cx,cy+q,c);
    break;
  case IC_PREV:
    PenTriFill(cx+t,cy-q,cx,cy,cx+t,cy+q,c);
    PenTriFill(cx,cy-q,cx-t,cy,cx,cy+q,c);
    break;
  case IC_VOLUME:
    PenLine(cx-r,cy-q/2,cx-q,cy-q/2,w,c);
    PenLine(cx-r,cy-q/2,cx-r,cy+q/2,w,c);
    PenLine(cx-r,cy+q/2,cx-q,cy+q/2,w,c);
    PenLine(cx-q,cy-q/2,cx,cy-t,w,c);
    PenLine(cx,cy-t,cx,cy+t,w,c);
    PenLine(cx,cy+t,cx-q,cy+q/2,w,c);
    PenArc(cx,cy,q+2,300,420,w,c);
    break;
  case IC_BRIGHT:
    PenCirc(cx,cy,q,w,c);
    for (int i=0;i<8;i++){ float u=TAU*i/8.0f;
      PenLine(cx+(int)(fcos(u)*(q+2)),cy+(int)(fsin(u)*(q+2)),
              cx+(int)(fcos(u)*r),cy+(int)(fsin(u)*r),w,c); }
    break;

  // ------------------------------------------------ APP-SPECIFIC
  // Everything below keeps an existing NEXUS app working. Same pen,
  // same construction rules, so they belong to the one family.
  case IC_GRID:
    PenRect(cx-r,cy-r,r,r,w,c);   PenRect(cx+1,cy-r,r,r,w,c);
    PenRect(cx-r,cy+1,r,r,w,c);   PenRect(cx+1,cy+1,r,r,w,c);
    break;
  case IC_LAYERS:
    for (int i=0;i<3;i++){ int oy=cy-q+i*q;
      PenLine(cx-r,oy,cx,oy-q/2,w,c); PenLine(cx,oy-q/2,cx+r,oy,w,c);
      PenLine(cx+r,oy,cx,oy+q/2,w,c); PenLine(cx,oy+q/2,cx-r,oy,w,c); }
    break;
  case IC_TARGET:
    PenCirc(cx,cy,r,w,c); PenCirc(cx,cy,q,w,c); PenBlob(cx,cy,(w>1)?2:1,c);
    break;
  case IC_HISTORY:
    // Clock face with a deliberate gap at the upper-left, and a return
    // arrow in that gap. The arc must NOT exceed 360 or it wraps and
    // closes the gap.
    PenArc(cx,cy,t,300,360,w,c);
    PenArc(cx,cy,t,0,240,w,c);
    PenLine(cx,cy,cx,cy-q,w,c); PenLine(cx,cy,cx+q,cy,w,c);
    PenBlob(cx,cy,1,c);
    { int ax=cx+(int)(fcos(4.19f)*t), ay=cy+(int)(fsin(4.19f)*t);
      PenLine(ax,ay,ax-q/2,ay-q/2,w,c);
      PenLine(ax,ay,ax+q/2,ay-q/2,w,c); }
    break;
  case IC_QUOTE:
    PenArc(cx-q,cy,q/2+1,90,300,w,c);
    PenArc(cx+q,cy,q/2+1,90,300,w,c);
    break;
  case IC_SLIDERS:
    for (int i=0;i<3;i++){ int oy=cy-q+i*q;
      PenLine(cx-r,oy,cx+r,oy,w,c);
      PenBlob(cx-q+i*q,oy,(w>1)?3:2,c); }
    break;
  case IC_STAR: {
    // 4-point sparkle with concave waists. Straight spokes from the
    // centre just make a plus sign.
    int wst=q/2+1;
    PenLine(cx,cy-r,cx+wst,cy-wst,w,c); PenLine(cx+wst,cy-wst,cx+r,cy,w,c);
    PenLine(cx+r,cy,cx+wst,cy+wst,w,c); PenLine(cx+wst,cy+wst,cx,cy+r,w,c);
    PenLine(cx,cy+r,cx-wst,cy+wst,w,c); PenLine(cx-wst,cy+wst,cx-r,cy,w,c);
    PenLine(cx-r,cy,cx-wst,cy-wst,w,c); PenLine(cx-wst,cy-wst,cx,cy-r,w,c);
  } break;
  case IC_PLANET:
    PenCirc(cx,cy,t,w,c);
    PenArc(cx,cy,r,340,380,w,c);
    PenLine(cx-r,cy+q/2,cx+r,cy-q/2,w,c);
    break;
  case IC_MOLECULE:
    PenLine(cx-t,cy+t,cx,cy-t,w,c);
    PenLine(cx+t,cy+t,cx,cy-t,w,c);
    PenLine(cx-t,cy+t,cx+t,cy+t,w,c);
    PenCirc(cx,cy-t,q/2+1,w,c);
    PenCirc(cx-t,cy+t,q/2+1,w,c);
    PenCirc(cx+t,cy+t,q/2+1,w,c);
    break;
  case IC_WAVE:
    for (int i=-r;i<r;i++)
      PenLine(cx+i,cy+(int)(fsin(i*0.42f)*q),
              cx+i+1,cy+(int)(fsin((i+1)*0.42f)*q),w,c);
    break;
  case IC_WELL:
    PenArc(cx,cy+q/2,r,0,360,w,c);
    PenArc(cx,cy-q/2,t,0,360,w,c);
    PenBlob(cx,cy,(w>1)?3:2,c);
    break;
  case IC_MAGNET: {
    // Horseshoe: outer and INNER arc, so the magnet has real thickness
    // and an open gap between the poles. One arc alone reads as an arch.
    int ro=r, ri=q;
    PenArc(cx,cy+q/2,ro,180,360,w,c);
    PenArc(cx,cy+q/2,ri,180,360,w,c);
    PenLine(cx-ro,cy+q/2,cx-ro,cy+r,w,c);
    PenLine(cx-ri,cy+q/2,cx-ri,cy+r,w,c);
    PenLine(cx+ri,cy+q/2,cx+ri,cy+r,w,c);
    PenLine(cx+ro,cy+q/2,cx+ro,cy+r,w,c);
    PenLine(cx-ro,cy+r,cx-ri,cy+r,w,c);           // pole faces
    PenLine(cx+ri,cy+r,cx+ro,cy+r,w,c);
  } break;
  case IC_LIFE:
    PenRect(cx-r,cy-r,r*2,r*2,w,c);
    PenBlob(cx-q,cy-q,(w>1)?2:1,c); PenBlob(cx+q,cy,(w>1)?2:1,c);
    PenBlob(cx,cy+q,(w>1)?2:1,c);
    break;
  case IC_FLOCK:
    for (int i=0;i<3;i++){
      int ox=cx-q+i*q, oy=cy-q+(i&1)*q;
      PenLine(ox,oy,ox-q,oy-q/2,w,c); PenLine(ox,oy,ox-q,oy+q/2,w,c); }
    break;
  case IC_FISH:
    PenArc(cx,cy,t,0,360,w,c);
    PenLine(cx-t,cy,cx-r,cy-q,w,c);
    PenLine(cx-t,cy,cx-r,cy+q,w,c);
    PenBlob(cx+q,cy-q/2,(w>1)?2:1,c);
    break;
  case IC_ANT:
    for (int i=-1;i<=1;i++) PenCirc(cx+i*q,cy,q/2+1,w,c);
    PenLine(cx-r,cy-q,cx-q,cy,w,c); PenLine(cx+r,cy-q,cx+q,cy,w,c);
    break;
  case IC_FIGURE:
    PenCirc(cx,cy-t,q/2+1,w,c);
    PenLine(cx,cy-q,cx,cy+q/2,w,c);
    PenLine(cx,cy-q/2,cx-q,cy,w,c); PenLine(cx,cy-q/2,cx+q,cy,w,c);
    PenLine(cx,cy+q/2,cx-q,cy+r,w,c); PenLine(cx,cy+q/2,cx+q,cy+r,w,c);
    break;
  case IC_TIMELINE:
    PenLine(cx-r,cy,cx+r,cy,w,c);
    for (int i=0;i<3;i++) PenLine(cx-q+i*q,cy-q/2,cx-q+i*q,cy+q/2,w,c);
    break;
  case IC_CURVE:
    for (int i=0;i<28;i++){ float u=TAU*i/28.0f;
      PenBlob(cx+(int)(fcos(u*3)*r),cy+(int)(fsin(u*2)*t),(w>1)?2:1,c); }
    break;
  case IC_SURFACE:
    for (int rr=0;rr<3;rr++)
      for (int i=-r;i<r;i+=2)
        PenBlob(cx+i,cy-q+rr*q-(int)(fsin(i*0.4f)*2),(w>1)?2:1,c);
    break;
  case IC_VECTOR:
    PenLine(cx-t,cy+t,cx+q,cy-q,w,c);
    PenChevron(cx+q,cy-q,q/2+1,1,w,c);
    break;
  case IC_MATRIX:
    PenLine(cx-r,cy-r,cx-t,cy-r,w,c);
    PenLine(cx-r,cy-r,cx-r,cy+r,w,c);
    PenLine(cx-r,cy+r,cx-t,cy+r,w,c);
    PenLine(cx+r,cy-r,cx+t,cy-r,w,c);
    PenLine(cx+r,cy-r,cx+r,cy+r,w,c);
    PenLine(cx+r,cy+r,cx+t,cy+r,w,c);
    PenBlob(cx-q/2,cy-q/2,(w>1)?2:1,c);
    PenBlob(cx+q/2,cy+q/2,(w>1)?2:1,c);
    break;
  case IC_FOURIER:
    PenCirc(cx-q,cy,t,w,c);
    PenLine(cx-q,cy,cx-q+t,cy-q/2,w,c);
    for (int i=0;i<r+q;i++)
      PenBlob(cx+q/2+i-1,cy-(int)(fsin(i*0.5f)*q),(w>1)?2:1,c);
    break;
  case IC_MAZE:
    PenRect(cx-r,cy-r,r*2,r*2,w,c);
    PenLine(cx-q,cy-r,cx-q,cy+q/2,w,c);
    PenLine(cx+q,cy-q/2,cx+q,cy+r,w,c);
    PenLine(cx-q,cy,cx+q,cy,w,c);
    break;
  case IC_BLOCKS: case IC_TETRIS:
    PenRect(cx-r,cy-r,r,r,w,c);
    PenRect(cx+1,cy-r,r,r,w,c);
    PenRect(cx-r,cy+1,r,r,w,c);
    break;
  case IC_BRICK:
    for (int rr=0;rr<2;rr++) for (int i=0;i<2;i++)
      PenRect(cx-r+i*r,cy-r+rr*q,r,q,w,c);
    PenLine(cx-q,cy+r,cx+q,cy+r,w,c);
    PenBlob(cx+q/2,cy+q,(w>1)?2:1,c);
    break;
  case IC_BIRD:
    PenCirc(cx,cy,q,w,c);
    PenLine(cx+q,cy,cx+r,cy-q/2,w,c);
    PenLine(cx-q,cy-q/2,cx-r,cy-q,w,c);
    break;
  case IC_SNAKE:
    PenLine(cx-r,cy-q,cx,cy-q,w,c);
    PenLine(cx,cy-q,cx,cy+q,w,c);
    PenLine(cx,cy+q,cx+r,cy+q,w,c);
    PenBlob(cx+r,cy+q,(w>1)?2:1,c);
    break;
  case IC_PADDLE:
    PenLine(cx-r,cy-t,cx-r,cy,w+1,c);
    PenLine(cx+r,cy,cx+r,cy+t,w+1,c);
    PenBlob(cx,cy,(w>1)?2:1,c);
    break;
  case IC_CARDS:
    PenRect(cx-r,cy-q,r+q,r+q,w,c);
    PenRect(cx-q,cy-r,r+q,r+q,w,c);
    break;
  case IC_BULB:
    PenCirc(cx,cy-q/2,t,w,c);
    PenLine(cx-q/2,cy+q,cx+q/2,cy+q,w,c);
    PenLine(cx-q/2,cy+r,cx+q/2,cy+r,w,c);
    break;
  case IC_MINE:
    PenCirc(cx,cy,t,w,c);
    for (int i=0;i<4;i++){ float u=TAU*i/4.0f+0.785f;
      PenLine(cx+(int)(fcos(u)*t),cy+(int)(fsin(u)*t),
              cx+(int)(fcos(u)*r),cy+(int)(fsin(u)*r),w,c); }
    break;
  case IC_HAMMER:
    PenRect(cx-r,cy-t,r+q,q,w,c);
    PenLine(cx-q/2,cy-q/2,cx+q,cy+r,w,c);
    break;
  case IC_SHIELD:
    PenLine(cx-t,cy-r,cx+t,cy-r,w,c);
    PenLine(cx-t,cy-r,cx-t,cy,w,c);
    PenLine(cx+t,cy-r,cx+t,cy,w,c);
    PenLine(cx-t,cy,cx,cy+r,w,c);
    PenLine(cx,cy+r,cx+t,cy,w,c);
    break;
  case IC_KALEIDO:
    for (int i=0;i<6;i++){ float u=TAU*i/6.0f, v=TAU*(i+1)/6.0f;
      PenLine(cx,cy,cx+(int)(fcos(u)*r),cy+(int)(fsin(u)*r),w,c);
      PenLine(cx+(int)(fcos(u)*r),cy+(int)(fsin(u)*r),
              cx+(int)(fcos(v)*r),cy+(int)(fsin(v)*r),w,c); }
    break;
  case IC_EXPLODE:
    PenRect(cx-q/2,cy-q/2,q,q,w,c);
    for (int i=0;i<4;i++){ float u=TAU*i/4.0f+0.785f;
      PenRect(cx+(int)(fcos(u)*r)-q/2,cy+(int)(fsin(u)*r)-q/2,q,q,w,c); }
    break;
  case IC_VOXEL:
    PenRect(cx-r,cy-q/2,t,t,w,c);
    PenRect(cx-q/2,cy-r,t,t,w,c);
    PenRect(cx-q/2,cy+q/2-1,t,t,w,c);
    break;
  case IC_IMPOSSIBLE:
    PenLine(cx,cy-r,cx+r,cy+t,w,c);
    PenLine(cx+r,cy+t,cx-r,cy+t,w,c);
    PenLine(cx-r,cy+t,cx,cy-r,w,c);
    PenLine(cx,cy-q/2,cx+q,cy+q,w,c);
    PenLine(cx+q,cy+q,cx-q,cy+q,w,c);
    break;
  case IC_TUNNEL:
    PenCirc(cx,cy,r,w,c); PenCirc(cx,cy,t,w,c); PenCirc(cx,cy,q/2+1,w,c);
    break;
  case IC_WARP:
    for (int i=0;i<8;i++){ float u=TAU*i/8.0f;
      PenLine(cx+(int)(fcos(u)*q),cy+(int)(fsin(u)*q),
              cx+(int)(fcos(u)*r),cy+(int)(fsin(u)*r),w,c); }
    PenBlob(cx,cy,(w>1)?2:1,c);
    break;
  case IC_RAIN:
    for (int i=-1;i<=1;i++)
      PenLine(cx+i*q,cy-r+((i+1)%2)*q,cx+i*q,cy+((i&1)?q:r),w,c);
    break;
  case IC_MORPH:
    PenCirc(cx-q,cy,t,w,c);
    PenRect(cx-q/2,cy-t,r,t*2,w,c);
    break;
  case IC_GESTURE: {
    int px=cx,py=cy; bool f=true;
    for (int i=0;i<22;i++){
      float u=TAU*i/22.0f*1.2f;
      int rr=r-(i*r)/30;
      int x=cx+(int)(fcos(u)*rr), y=cy+(int)(fsin(u)*rr);
      if (!f) PenLine(px,py,x,y,w,c);
      px=x; py=y; f=false; }
    PenBlob(px,py,(w>1)?2:1,c);
  } break;
  case IC_SPHERE:
    PenCirc(cx,cy,r,w,c);
    PenArc(cx,cy,r,270,450,w,c);
    for (int i=-r;i<=r;i+=2){
      int hw=(int)(r*0.42f*sqrtf(fmaxf(0.0f,1.0f-(float)(i*i)/(r*r))));
      if (hw>0){ PenBlob(cx-hw,cy+i,1,c); PenBlob(cx+hw,cy+i,1,c); } }
    break;
  case IC_BRUSH:
    PenLine(cx-r,cy+r,cx+q,cy-q,w,c);
    PenRect(cx+q/2,cy-r,q+1,q+1,w,c);
    break;
  case IC_GAMEPAD: case IC_PLAY:
    PenRRect(cx-r,cy-q,r*2,q*2,2,w,c);
    PenLine(cx-q,cy,cx-q/2,cy,w,c);
    PenLine(cx-t+1,cy-q/2,cx-t+1,cy+q/2,w,c);
    PenBlob(cx+q,cy,(w>1)?2:1,c);
    break;
  default:
    PenCirc(cx,cy,t,w,c);
    break; }
}

// Flat UI icons keep their old entry point and signature; they just draw
// from the new family now. Pen width scales with size so a 6px icon in a
// list row stays 1px and a 14px category mark gets 2px.
void IconV8(uint8_t g,int cx,int cy,uint16_t c,int s){
  // Same 0.05 pen ratio as the launcher, so flat UI icons and app tiles
  // are visibly one family.
  IconPack(g,cx,cy,s,c,((s*2)>=34)?2:1);
}

// =====================================================================
//  V8 UI COMPONENT LIBRARY
//  Every screen composes from these. Rules enforced here rather than
//  re-decided per screen:
//    * surfaces read by VALUE STEP, never by border or glow
//    * exactly one accent communicates state; it never decorates
//    * one corner treatment (RADIUS px notch) used everywhere
//    * touch targets are >= TAP_MIN tall, with forgiving hitboxes
//  Cost note: none of these do full-screen work. The most expensive is
//  UiScrim(), which touches every 2nd scanline only.
// =====================================================================

// ---- geometry helpers ------------------------------------------------
// A soft-cornered filled rect. The corner is a small diagonal notch --
// cheaper than a circle and it is the single shape motif of the whole UI.
void UiRect(int x,int y,int w,int h,uint16_t c,uint8_t a){
  if (w<=0||h<=0) return;
  const int r=(w>10&&h>10)?RADIUS:0;
  for (int j=0;j<h;j++){
    int inset=0;
    if (j<r)        inset=r-j;
    else if (j>=h-r) inset=r-(h-1-j);
    if (a>=255) FillRectFB(x+inset,y+j,w-inset*2,1,c);
    else        BlendRectFB(x+inset,y+j,w-inset*2,1,c,a); }
}
// 1 px outline in the same corner language.
void UiOutline(int x,int y,int w,int h,uint16_t c,uint8_t a){
  if (w<=0||h<=0) return;
  const int r=(w>10&&h>10)?RADIUS:0;
  BlendRectFB(x+r,y,w-r*2,1,c,a);
  BlendRectFB(x+r,y+h-1,w-r*2,1,c,a);
  BlendRectFB(x,y+r,1,h-r*2,c,a);
  BlendRectFB(x+w-1,y+r,1,h-r*2,c,a);
  for (int i=0;i<r;i++){
    PxBlend(x+r-i,     y+i,      c,a);
    PxBlend(x+w-1-r+i, y+i,      c,a);
    PxBlend(x+r-i,     y+h-1-i,  c,a);
    PxBlend(x+w-1-r+i, y+h-1-i,  c,a); }
}
// Hairline divider. One job, one appearance, everywhere.
void UiDivider(int x,int y,int w){ BlendRectFB(x,y,w,1,C_LINE,A_HAIR); }

// Background scrim for modals -- every 2nd scanline, so half the cost of
// a full blend. Never call a full-screen blend from UI code.
void UiScrim(uint8_t strength){
  for (int y=0;y<SCREEN_H;y+=2)
    BlendRectFB(0,y,SCREEN_W,1,TH.bg,strength);
}

// ---- typography ------------------------------------------------------
// Small caps label: dim, wide, used above values and as section headers.
void UiLabel(int x,int y,const char *t){ DrawText(x,y,t,C_DIM,T_SMALL); }
void UiValue(int x,int y,const char *t){ DrawText(x,y,t,C_TEXT,T_BODY); }
// Numeric readout. All data in the OS uses this one colour, per the
// original rule: cool steel is reserved strictly for numbers.
void UiData(int x,int y,const char *t){ DrawText(x,y,t,C_DATA,T_BODY); }
void UiDataR(int right,int y,const char *t){
  DrawText(right-TextW(t,T_BODY),y,t,C_DATA,T_BODY); }
void UiTitle(int x,int y,const char *t){ DrawText(x,y,t,C_TEXT,T_TITLE); }

// ---- interaction state ----------------------------------------------
enum { UST_NORMAL=0, UST_PRESSED, UST_SELECTED, UST_DISABLED };
static inline bool UiHit(int x,int y,int w,int h,int pad){
  return touchX>=x-pad&&touchX<x+w+pad&&touchY>=y-pad&&touchY<y+h+pad;
}

// =====================================================================
//  HEADER  -- identical on every screen: title left, context right,
//  one hairline underneath. No brackets, no glow, no decoration.
// =====================================================================
static bool uiBackArmed=false;
bool UiHeader(const char *title,const char *context){
  UiRect(0,0,SCREEN_W,HEADER_H,C_SURFACE,A_SURF);
  UiDivider(0,HEADER_H,SCREEN_W);
  // back chevron -- generous invisible hitbox, minimal visible mark
  bool over=UiHit(0,0,54,HEADER_H,0);
  uint16_t bc=over?C_ACCENT:C_DIM;
  int cy=HEADER_H/2;
  IconPack(IC_BACK,13,cy,5,bc,1);
  DrawText(24,cy-3,title,C_TEXT,T_BODY);
  if (context){
    int w=TextW(context,T_SMALL);
    DrawText(SCREEN_W-GUTTER-w,cy-3,context,C_DIM,T_SMALL); }
  if (touchDown&&over&&transT==0){ uiBackArmed=true; return true; }
  return false;
}
// Footer status strip: left slot, right slot, hairline above.
void UiFooter(const char *left,const char *right){
  int y=SCREEN_H-FOOTER_H;
  UiDivider(0,y,SCREEN_W);
  UiRect(0,y+1,SCREEN_W,FOOTER_H-1,C_SURFACE,A_SURF);
  if (left)  DrawText(GUTTER,y+6,left,C_DIM,T_SMALL);
  if (right) DrawText(SCREEN_W-GUTTER-TextW(right,T_SMALL),y+6,right,C_DATA,T_SMALL);
}

// =====================================================================
//  CARD  -- elevation by value step only. An accent rail on the left
//  edge is the ONLY accent a card gets, and only when selected.
// =====================================================================
void UiCard(int x,int y,int w,int h,int state,uint16_t accent){
  uint16_t fill=(state==UST_PRESSED)?C_SURFACE2:
                (state==UST_SELECTED)?C_SURFACE2:C_SURFACE;
  UiRect(x,y,w,h,fill,A_SURF);
  // top edge catches one step of light -- reads as a raised surface
  BlendRectFB(x+RADIUS,y,w-RADIUS*2,1,C_LINE,120);
  if (state==UST_SELECTED){
    FillRectFB(x,y+RADIUS,2,h-RADIUS*2,accent?accent:C_ACCENT); }
  else if (state==UST_PRESSED){
    BlendRectFB(x,y+RADIUS,2,h-RADIUS*2,accent?accent:C_ACCENT,160); }
}

// ---------------------------------------------------------------------
//  LIST PRIMITIVES  --  the whole settings/list language is these three.
//  Hierarchy comes from type and spacing, not from boxes.
// ---------------------------------------------------------------------
// Quiet section label. Sits above a group, never boxed.
void UiSection(int x,int y,const char *label){
  DrawText(x,y,label,C_OFF,T_SMALL);
}
// One row: label left, value right, optional disclosure chevron.
// Returns true on tap. Hitbox is the full row height regardless of ink.
bool UiRow(int x,int y,int w,int h,const char *label,const char *value,
           bool chevron){
  bool over=touchActive&&UiHit(x,y,w,h,0);
  if (over) UiRect(x-2,y,w+4,h,C_SURFACE,160);
  DrawText(x+2,y+(h-7)/2,label,C_TEXT,T_BODY);
  int rx=x+w-4;
  if (chevron){
    // Right-pointing disclosure, same pen as every other glyph.
    PenChevron(rx-4,y+h/2,4,1,1,over?C_ACCENT:C_OFF);
    rx-=13; }
  if (value) DrawText(rx-TextW(value,T_BODY),y+(h-7)/2,value,C_DIM,T_BODY);
  return touchDown&&UiHit(x,y,w,h,0);
}
// A row whose right side is a segmented choice.
int UiSegRow(int x,int y,int w,int h,const char *label,
             const char *const*opts,int n,int cur){
  DrawText(x+2,y+(h-7)/2,label,C_TEXT,T_BODY);
  // Cell width follows the widest label, so "Full" cannot overflow a
  // fixed 21 px box the way it did.
  int cw=0;
  for (int i=0;i<n;i++){ int lw=TextW(opts[i],T_SMALL); if (lw>cw) cw=lw; }
  cw+=8;
  if (cw<20) cw=20;
  int sw=cw*n, sx=x+w-sw-2;
  int hit=-1;
  for (int i=0;i<n;i++){
    int bx=sx+i*cw;
    bool sel=(i==cur);
    bool over=touchActive&&UiHit(bx,y+3,cw,h-6,2);
    if (sel) UiRect(bx,y+3,cw-2,h-6,C_ACCENT,255);
    else if (over) UiRect(bx,y+3,cw-2,h-6,C_SURFACE2,255);
    int lw=TextW(opts[i],T_SMALL);
    DrawText(bx+(cw-2-lw)/2,y+(h-7)/2,opts[i],sel?TH.bg:C_DIM,T_SMALL);
    if (touchDown&&UiHit(bx,y+3,cw,h-6,2)) hit=i; }
  return hit;
}

// =====================================================================
//  BUTTON  -- 5 states, physical press (1 px sink + value step).
// =====================================================================
static int uiPressId=-1;
static float uiPressT=0;
bool UiButton(int x,int y,int w,int h,const char *label,int state,int id){
  bool dis=(state==UST_DISABLED);
  bool over=!dis&&UiHit(x,y,w,h,3);
  bool sel=(state==UST_SELECTED);
  int sink=(over&&touchActive)?1:0;
  uint16_t fill = dis ? C_SURFACE
                : sel ? C_ACCENT
                : (over&&touchActive) ? C_SURFACE2 : C_SURFACE;
  uint16_t txt  = dis ? C_OFF
                : sel ? TH.bg
                : over ? C_TEXT : C_DIM;
  UiRect(x,y+sink,w,h,fill,A_SURF);
  if (!sel) UiOutline(x,y+sink,w,h,over?C_ACCENT:C_LINE,over?200:A_HAIR);
  DrawText(x+(w-TextW(label,T_BODY))/2, y+sink+(h-7)/2, label, txt, T_BODY);
  bool hit = !dis && touchDown && UiHit(x,y,w,h,3);
  if (hit){ uiPressId=id; uiPressT=0; }
  return hit;
}
// Compact icon-only button, square, for toolbars.
bool UiIconButton(int x,int y,int s,uint8_t glyph,int state,int id){
  bool over=UiHit(x,y,s,s,3);
  bool sel=(state==UST_SELECTED);
  UiRect(x,y,s,s,sel?C_ACCENT:((over&&touchActive)?C_SURFACE2:C_SURFACE),A_SURF);
  if (!sel) UiOutline(x,y,s,s,over?C_ACCENT:C_LINE,over?190:A_HAIR);
  IconV8(glyph,x+s/2,y+s/2,sel?TH.bg:(over?C_TEXT:C_DIM),8);
  return touchDown&&over;
}

// =====================================================================
//  TOGGLE
// =====================================================================
bool UiToggle(int x,int y,const char *label,bool on){
  const int TW=34, THh=16;
  int tx=x+CONTENT_W-TW-CARD_PAD;
  DrawText(x+CARD_PAD,y+5,label,C_TEXT,T_BODY);
  UiRect(tx,y+3,TW,THh,on?C_ACCENT:C_SURFACE2,A_SURF);
  if (!on) UiOutline(tx,y+3,TW,THh,C_LINE,A_HAIR);
  // knob springs across
  static float kp[8]={0,0,0,0,0,0,0,0};
  int slot=(y/12)&7;
  float target=on?1.0f:0.0f;
  kp[slot]+=(target-kp[slot])*0.35f;
  int kx=tx+2+(int)(kp[slot]*(TW-14));
  UiRect(kx,y+5,12,THh-4,on?TH.bg:C_DIM,255);
  return touchDown&&UiHit(x,y,CONTENT_W,THh+6,2);
}

// =====================================================================
//  SLIDER  -- direct manipulation, value always visible.
// =====================================================================
bool UiSlider(int x,int y,int w,const char *label,float *val,const char *unit){
  const int TRACK=3;
  int ty=y+13;
  DrawText(x,y,label,C_DIM,T_SMALL);
  char vb[16];
  if (unit) snprintf(vb,sizeof(vb),"%d%s",(int)(*val*100),unit);
  else      snprintf(vb,sizeof(vb),"%d",(int)(*val*100));
  DrawText(x+w-TextW(vb,T_SMALL),y,vb,C_DATA,T_SMALL);
  // forgiving vertical hitbox -- the visual track stays thin
  bool drag = touchActive && UiHit(x-6,y-4,w+12,26,0);
  if (drag) *val=clampf((float)(touchX-x)/w,0,1);
  UiRect(x,ty,w,TRACK,C_SURFACE2,255);
  int fw=(int)(w*clampf(*val,0,1));
  UiRect(x,ty,fw,TRACK,C_ACCENT,255);
  int kx=x+fw;
  UiRect(kx-3,ty-4,6,TRACK+8,drag?C_HILITE:C_ACCENT,255);
  return drag;
}

// =====================================================================
//  LIST ITEM  -- the workhorse. Icon, title, optional value, chevron.
// =====================================================================
bool UiListItem(int x,int y,int w,int h,uint8_t glyph,
                const char *title,const char *sub,const char *value,
                bool selected,uint16_t accent){
  bool over=UiHit(x,y,w,h,0);
  int st=selected?UST_SELECTED:((over&&touchActive)?UST_PRESSED:UST_NORMAL);
  UiCard(x,y,w,h,st,accent);
  int ix=x+CARD_PAD+9;
  if (glyph!=0xFF) IconV8(glyph,ix,y+h/2,selected?C_ACCENT:C_DIM,9);
  int tx=x+CARD_PAD+(glyph!=0xFF?24:0);
  if (sub){
    DrawText(tx,y+h/2-8,title,C_TEXT,T_BODY);
    DrawText(tx,y+h/2+2,sub,C_DIM,T_SMALL);
  } else {
    DrawText(tx,y+(h-7)/2,title,C_TEXT,T_BODY); }
  if (value) DrawText(x+w-CARD_PAD-TextW(value,T_BODY),y+(h-7)/2,value,C_DATA,T_BODY);
  return touchDown&&over;
}

// =====================================================================
//  PROGRESS  -- one bar style for the whole OS.
// =====================================================================
void UiProgress(int x,int y,int w,float frac,uint16_t c){
  frac=clampf(frac,0,1);
  UiRect(x,y,w,3,C_SURFACE2,255);
  int fw=(int)(w*frac);
  if (fw>0) UiRect(x,y,fw,3,c?c:C_ACCENT,255);
}
// Compact stat block: label above, big value below.
void UiStat(int x,int y,const char *label,const char *value,uint16_t vc){
  DrawText(x,y,label,C_DIM,T_SMALL);
  DrawText(x,y+11,value,vc?vc:C_TEXT,T_TITLE);
}

// =====================================================================
//  MODAL  -- one dialog shape, integrated rather than a floating box.
// =====================================================================
static bool  uiModalOn=false;
static float uiModalT=0;
static char  uiModalTitle[24]="";
static char  uiModalBody[64]="";
static int   uiModalKind=0;      // 0 = confirm, 1 = notice
static int   uiModalResult=0;    // 0 none, 1 confirm, 2 cancel
void UiModalOpen(const char *title,const char *body,int kind){
  snprintf(uiModalTitle,sizeof(uiModalTitle),"%s",title?title:"");
  snprintf(uiModalBody,sizeof(uiModalBody),"%s",body?body:"");
  uiModalOn=true; uiModalT=0; uiModalKind=kind; uiModalResult=0;
}
// returns 1 confirm, 2 cancel/dismiss, 0 nothing
int UiModalDraw(float dt){
  if (!uiModalOn) return 0;
  uiModalT=clampf(uiModalT+dt*6.0f,0,1);
  float e=EaseOutCubic(uiModalT);
  UiScrim((uint8_t)(150*e));
  const int MW=232, MH=88;
  int mx=(SCREEN_W-MW)/2;
  int my=(SCREEN_H-MH)/2 + (int)((1.0f-e)*10.0f);
  UiRect(mx,my,MW,MH,C_SURFACE2,255);
  UiOutline(mx,my,MW,MH,C_LINE,180);
  BlendRectFB(mx+RADIUS,my,MW-RADIUS*2,1,C_ACCENT,200);
  DrawText(mx+CARD_PAD+2,my+10,uiModalTitle,C_TEXT,T_BODY);
  UiDivider(mx+CARD_PAD,my+24,MW-CARD_PAD*2);
  DrawText(mx+CARD_PAD+2,my+34,uiModalBody,C_DIM,T_SMALL);
  int by=my+MH-CARD_PAD-TAP_MIN+8;
  int r=0;
  if (uiModalKind==0){
    if (UiButton(mx+CARD_PAD,by,96,24,"CANCEL",UST_NORMAL,900)) r=2;
    IconPack(IC_CLOSE,mx+CARD_PAD+12,by+12,4,C_DIM,1);
    if (UiButton(mx+MW-CARD_PAD-96,by,96,24,"CONFIRM",UST_SELECTED,901)) r=1;
    IconPack(IC_CHECK,mx+MW-CARD_PAD-84,by+12,4,TH.bg,1);
  } else {
    if (UiButton(mx+MW-CARD_PAD-96,by,96,24,"OK",UST_SELECTED,902)) r=1; }
  if (r){ uiModalOn=false; uiModalResult=r; }
  return r;
}
static inline bool UiModalActive(void){ return uiModalOn; }

// =====================================================================
//  TOAST  -- brief, bottom-anchored, never blocks input.
// =====================================================================
static char  uiToastMsg[32]="";
static float uiToastT=99.0f;
static uint16_t uiToastCol=0;
void UiToast(const char *msg,uint16_t c){
  snprintf(uiToastMsg,sizeof(uiToastMsg),"%s",msg?msg:"");
  uiToastT=0; uiToastCol=c?c:C_ACCENT;
}
void UiToastDraw(float dt){
  if (uiToastT>2.2f) return;
  uiToastT+=dt;
  float in =clampf(uiToastT/0.18f,0,1);
  float out=clampf((uiToastT-1.9f)/0.3f,0,1);
  float e=EaseOutCubic(in)*(1.0f-EaseOutCubic(out));
  if (e<=0.01f) return;
  int w=TextW(uiToastMsg,T_BODY)+28;
  int x=(SCREEN_W-w)/2;
  int y=SCREEN_H-FOOTER_H-26+(int)((1.0f-e)*8.0f);
  UiRect(x,y,w,22,C_SURFACE2,(uint8_t)(240*e));
  UiOutline(x,y,w,22,C_LINE,(uint8_t)(150*e));
  IconPack(IC_CHECK,x+CARD_PAD+4,y+11,4,uiToastCol,1);
  DrawText(x+CARD_PAD+13,y+8,uiToastMsg,C_TEXT,T_BODY);
}

// =====================================================================
//  PAGE INDICATOR
// =====================================================================
void UiPageDots(int cx,int y,int n,int active){
  int w=n*10-4;
  int x=cx-w/2;
  for (int i=0;i<n;i++){
    bool a=(i==active);
    UiRect(x+i*10,y,a?6:4,3,a?C_ACCENT:C_OFF,255); }
}

// =====================================================================
//  EMPTY + ERROR STATES  (spec 25 / 26)
// =====================================================================
void UiEmpty(const char *title,const char *hint,uint8_t glyph){
  int cy=CONTENT_TOP+CONTENT_H/2-10;
  IconV8(glyph,SCREEN_W/2,cy-16,C_OFF,14);
  int w=TextW(title,T_BODY);
  DrawText((SCREEN_W-w)/2,cy+8,title,C_DIM,T_BODY);
  if (hint){ int w2=TextW(hint,T_SMALL);
    DrawText((SCREEN_W-w2)/2,cy+22,hint,C_OFF,T_SMALL); }
}
void UiError(const char *what,const char *detail){
  int cy=CONTENT_TOP+CONTENT_H/2-12;
  int bw=CONTENT_W-40;
  int bx=(SCREEN_W-bw)/2;
  UiRect(bx,cy-8,bw,52,C_SURFACE2,A_SURF);
  FillRectFB(bx,cy-8+RADIUS,2,52-RADIUS*2,C_ERR);
  DrawText(bx+CARD_PAD+4,cy+2,what,C_ERR,T_BODY);
  if (detail) DrawText(bx+CARD_PAD+4,cy+16,detail,C_DIM,T_SMALL);
}

// =====================================================================
//  WIDGETS
// =====================================================================
void Bracket(int x,int y,int w,int h,uint16_t c,int len){
  HLineFB(x,y,len,c);                   VLineFB(x,y,len,c);
  HLineFB(x+w-len,y,len,c);             VLineFB(x+w-1,y,len,c);
  HLineFB(x,y+h-1,len,c);               VLineFB(x,y+h-len,len,c);
  HLineFB(x+w-len,y+h-1,len,c);         VLineFB(x+w-1,y+h-len,len,c);
}
#define CHAMFER 7
void Panel(int x,int y,int w,int h,const char *title,uint16_t ac,const char *tag){
  // Minimal surface. Was: chamfered frame + 4 accent corner ticks + 2
  // corner glows + a filled title bar + a decorative notch. That is the
  // "box around everything" the redesign removes, and it ran on 76 call
  // sites. Now: one raised surface, an optional quiet section label,
  // no border at all. Elevation is carried purely by value step.
  (void)ac;
  UiRect(x,y,w,h,C_SURFACE,A_SURF);
  // top edge catches one step of light -- reads as raised, costs 1 line
  BlendRectFB(x+RADIUS,y,w-RADIUS*2,1,C_LINE,110);
  if (title){
    DrawText(x+CARD_PAD,y+6,title,C_DIM,1);
    if (tag) DrawText(x+w-TextW(tag,1)-CARD_PAD,y+6,tag,C_OFF,1);
  } else if (tag){
    DrawText(x+w-TextW(tag,1)-CARD_PAD,y+6,tag,C_OFF,1); }
}
// Draws a button and returns true if it was pressed this frame.
bool Button(int x,int y,int w,int h,const char *label,uint16_t c,bool on){
  bool hit = touchDown && touchX>=x && touchX<x+w && touchY>=y && touchY<y+h;
  bool hov = touchActive && touchX>=x && touchX<x+w && touchY>=y && touchY<y+h;
  BlendRectFB(x,y,w,h, on?Dim(c,2,5):Dim(c,1,6), A_FILL);
  Bracket(x,y,w,h, on?c:Dim(c,3,5), 5);
  if (on||hov){
    Glow(x+w/2,y+h/2,c,(uint8_t)(on?70:40),1.4f);
    HLineFB(x+2,y,w-4,c); }
  DrawTextC(x+w/2, y+(h-7)/2, label, on?C_TEXT:C_SAND, 1);
  return hit;
}
void Graph(int x,int y,int w,int h,const uint8_t *hist,int head,uint16_t c){
  BlendRectFB(x,y,w,h,C_PANEL,A_FILL);
  for (int i=1;i<4;i++) BlendRectFB(x,y+h*i/4,w,1,Dim(c,1,6),A_GLOW);
  int shown=(int)(GRAPH_LEN*EaseOutCubic(clampf(enterAnim*1.4f,0,1)));
  int px=x, py=y+h-1;
  for (int i=0;i<w&&i<shown;i++){
    int idx=(head+GRAPH_LEN-(GRAPH_LEN-1)+i)%GRAPH_LEN;
    int nx=x+i*w/GRAPH_LEN;
    int ny=y+h-1-(hist[idx]*(h-2)/255);
    if (i){ LineFB(px,py,nx,ny,c,A_FILL);
      BlendRectFB(nx,ny+1,1,(y+h-1)-ny,c,34); }
    px=nx; py=ny; }
  Glow(px,py,c,(uint8_t)(130*Pulse(gTime,5.0f)),0.5f);
  PxBlend(px,py,C_TEXT,255);
}
// Horizontal slider. Returns true while being dragged; writes to *val.
static bool SliderRow(int x,int y,int w,const char *label,float *val,uint16_t c){
  DrawText(x,y-9,label,C_SAND,1);
  int ty=y+3;
  BlendRectFB(x,ty,w,3,C_HAIR,A_FILL);
  bool drag = touchActive && touchX>=x-12 && touchX<=x+w+12 && touchY>=y-12 && touchY<=y+18;
  if (drag) *val = clampf((float)(touchX-x)/w, 0, 1);
  int fw=(int)(w*clampf(*val,0,1));
  BlendRectFB(x,ty,fw,3,c,255);
  int kx=x+fw;
  if (drag){ Glow(kx,ty+1,C_HILITE,(uint8_t)(140*Pulse(gTime,7.0f)),0.9f);
             BlendRectFB(x,ty,fw,3,C_HILITE,A_GLOW); }
  else Glow(kx,ty+1,c,60,0.6f);
  int lift=drag?1:0;
  BlendRectFB(kx-2,ty-4-lift,5,11+lift*2,c,255);
  BlendRectFB(kx-1,ty-3-lift,3,9+lift*2,C_TEXT,A_FILL);
  char v[8]; snprintf(v,sizeof(v),"%d",(int)(*val*100));
  DrawText(x+w+6,y-1,v,C_DATA,1);
  return drag;
}

// =====================================================================
//  BACKDROP
// =====================================================================
void Backdrop(void){
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t *)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  memset(depth,0,FB_BYTES);
  // One soft vertical lift so the screen is not a dead black rectangle.
  // Every 4th scanline -> a quarter of the cost of a full pass, and at
  // these alphas the banding is invisible on the panel.
  if (!gLowDetail)
    for (int y=CONTENT_TOP;y<SCREEN_H;y+=4){
      float f=(float)(y-CONTENT_TOP)/(float)(SCREEN_H-CONTENT_TOP);
      uint8_t a=(uint8_t)(7.0f+9.0f*fsin(f*3.14159f));
      BlendRectFB(0,y,SCREEN_W,1,C_SURFACE,a); }
}

// =====================================================================
//  3D CORE
// =====================================================================
static const float FOV_F = 250.0f;
void RasterTriangle(const SVert &v0,const SVert &v1,const SVert &v2,
                    uint8_t br,uint8_t bg,uint8_t bb){
  float mnx=v0.sx,mxx=v0.sx,mny=v0.sy,mxy=v0.sy;
  if (v1.sx<mnx) mnx=v1.sx; if (v1.sx>mxx) mxx=v1.sx;
  if (v2.sx<mnx) mnx=v2.sx; if (v2.sx>mxx) mxx=v2.sx;
  if (v1.sy<mny) mny=v1.sy; if (v1.sy>mxy) mxy=v1.sy;
  if (v2.sy<mny) mny=v2.sy; if (v2.sy>mxy) mxy=v2.sy;
  int minX=(int)floorf(mnx),maxX=(int)ceilf(mxx);
  int minY=(int)floorf(mny),maxY=(int)ceilf(mxy);
  if (minX<0) minX=0; if (minY<0) minY=0;
  if (maxX>SCREEN_W-1) maxX=SCREEN_W-1;
  if (maxY>SCREEN_H-1) maxY=SCREEN_H-1;
  if (minX>maxX||minY>maxY) return;
  float ax=v0.sx,ay=v0.sy,bx=v1.sx,by=v1.sy,cx=v2.sx,cy=v2.sy;
  float area=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
  if (area>-0.0001f&&area<0.0001f) return;
  float ia=1.0f/area;
  float e0dx=(by-cy),e0dy=(cx-bx);
  float e1dx=(cy-ay),e1dy=(ax-cx);
  float e2dx=(ay-by),e2dy=(bx-ax);
  float px=(float)minX+0.5f,py=(float)minY+0.5f;
  float w0r=(e0dx*(px-bx)+e0dy*(py-by))*ia;
  float w1r=(e1dx*(px-cx)+e1dy*(py-cy))*ia;
  float w2r=(e2dx*(px-ax)+e2dy*(py-ay))*ia;
  e0dx*=ia; e0dy*=ia; e1dx*=ia; e1dy*=ia; e2dx*=ia; e2dy*=ia;
  for (int y=minY;y<=maxY;y++){
    float w0=w0r,w1=w1r,w2=w2r;
    uint16_t *rc=&frame[y*SCREEN_W], *rz=&depth[y*SCREEN_W];
    for (int x=minX;x<=maxX;x++){
      if (w0>=0&&w1>=0&&w2>=0){
        float iz=w0*v0.invz+w1*v1.invz+w2*v2.invz;
        uint16_t z=(uint16_t)clampf(iz*45000.0f,0,65000.0f);
        if (z>rz[x]){
          rz[x]=z;
          float d=w0*v0.diff+w1*v1.diff+w2*v2.diff;
          float s=w0*v0.spec+w1*v1.spec+w2*v2.spec;
          int li=(int)(d*256.0f), sp=(int)(s*255.0f);
          rc[x]=RGB565(((br*li)>>8)+sp,((bg*li)>>8)+sp,((bb*li)>>8)+sp); } }
      w0+=e0dx; w1+=e1dx; w2+=e2dx; }
    w0r+=e0dy; w1r+=e1dy; w2r+=e2dy; }
}
void RasterAdd(const SVert &v0,const SVert &v1,const SVert &v2,uint16_t col,uint8_t amt){
  int minX=(int)floorf(fminf(v0.sx,fminf(v1.sx,v2.sx)));
  int maxX=(int)ceilf (fmaxf(v0.sx,fmaxf(v1.sx,v2.sx)));
  int minY=(int)floorf(fminf(v0.sy,fminf(v1.sy,v2.sy)));
  int maxY=(int)ceilf (fmaxf(v0.sy,fmaxf(v1.sy,v2.sy)));
  if (minX<0) minX=0; if (minY<0) minY=0;
  if (maxX>SCREEN_W-1) maxX=SCREEN_W-1;
  if (maxY>SCREEN_H-1) maxY=SCREEN_H-1;
  if (minX>maxX||minY>maxY) return;
  float ax=v0.sx,ay=v0.sy,bx=v1.sx,by=v1.sy,cx=v2.sx,cy=v2.sy;
  float area=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
  if (fabsf(area)<0.0001f) return;
  float ia=1.0f/area;
  float e0dx=(by-cy),e0dy=(cx-bx);
  float e1dx=(cy-ay),e1dy=(ax-cx);
  float e2dx=(ay-by),e2dy=(bx-ax);
  float px=(float)minX+0.5f,py=(float)minY+0.5f;
  float w0r=(e0dx*(px-bx)+e0dy*(py-by))*ia;
  float w1r=(e1dx*(px-cx)+e1dy*(py-cy))*ia;
  float w2r=(e2dx*(px-ax)+e2dy*(py-ay))*ia;
  e0dx*=ia; e0dy*=ia; e1dx*=ia; e1dy*=ia; e2dx*=ia; e2dy*=ia;
  for (int y=minY;y<=maxY;y++){
    float w0=w0r,w1=w1r,w2=w2r;
    uint16_t *rc=&frame[y*SCREEN_W];
    for (int x=minX;x<=maxX;x++){
      if (w0>=0&&w1>=0&&w2>=0) AddInto(&rc[x],col,amt);
      w0+=e0dx; w1+=e1dx; w2+=e2dx; }
    w0r+=e0dy; w1r+=e1dy; w2r+=e2dy; }
}
void RenderMesh(const Mesh &m,float rx,float ry,float rz,float sc,
                float ox,float oy,float cz,uint8_t mode,uint16_t tint){
  if (m.nt==0||sc<=0.001f||m.nv>MAX_MESH_VERTS) return;
  const float AMB=0.17f,DIF=0.92f;
  float sxr=fsin(rx),cxr=fcos(rx),syr=fsin(ry),cyr=fcos(ry),szr=fsin(rz),czr=fcos(rz);
  float a00=cyr,a01=0,a02=syr;
  float a10=sxr*syr,a11=cxr,a12=-sxr*cyr;
  float a20=-cxr*syr,a21=sxr,a22=cxr*cyr;
  float m00=czr*a00-szr*a10,m01=czr*a01-szr*a11,m02=czr*a02-szr*a12;
  float m10=szr*a00+czr*a10,m11=szr*a01+czr*a11,m12=szr*a02+czr*a12;
  float m20=a20,m21=a21,m22=a22;
  float hx=-LX,hy=-LY,hz=-LZ-1.0f;
  float hl=1.0f/sqrtf(hx*hx+hy*hy+hz*hz);
  hx*=hl; hy*=hl; hz*=hl;
  for (int i=0;i<m.nv;i++){
    float vx=m.pos[i].x,vy=m.pos[i].y,vz=m.pos[i].z;
    float px=(m00*vx+m01*vy+m02*vz)*sc;
    float py=(m10*vx+m11*vy+m12*vz)*sc;
    float pz=(m20*vx+m21*vy+m22*vz)*sc+cz;
    if (pz<0.30f) pz=0.30f;
    float nx=m.nrm[i].x,ny=m.nrm[i].y,nz=m.nrm[i].z;
    float tnx=m00*nx+m01*ny+m02*nz;
    float tny=m10*nx+m11*ny+m12*nz;
    float tnz=m20*nx+m21*ny+m22*nz;
    float iz=1.0f/pz;
    gSV[i].sx=ox+px*FOV_F*iz;
    gSV[i].sy=oy-py*FOV_F*iz;
    gSV[i].invz=iz;
    gSV[i].nx=tnx; gSV[i].ny=tny; gSV[i].nz=tnz;
    float ndl=-(tnx*LX+tny*LY+tnz*LZ);
    if (ndl<0) ndl=0;
    gSV[i].diff=AMB+DIF*ndl;
    if (mode==M_SMOOTH){
      float ndh=tnx*hx+tny*hy+tnz*hz;
      if (ndh<0) ndh=0;
      float s=ndh*ndh; s=s*s; s=s*s; s=s*s;
      gSV[i].spec=s*0.8f;
    } else gSV[i].spec=0; }
  if (mode==M_POINTS){
    uint16_t col=tint?tint:C_ACCENT;
    for (int i=0;i<m.nv;i++){
      if (gSV[i].nz>-0.05f) continue;
      PxAdd((int)gSV[i].sx,(int)gSV[i].sy,col,(uint8_t)(110+145*clampf(gSV[i].diff,0,1))); }
    return; }
  for (int t=0;t<m.nt;t++){
    uint16_t ia2=m.tri[t].a,ib=m.tri[t].b,ic=m.tri[t].c;
    SVert A=gSV[ia2],B=gSV[ib],C=gSV[ic];
    float area=(B.sx-A.sx)*(C.sy-A.sy)-(B.sy-A.sy)*(C.sx-A.sx);
    bool front=(area>0);
    if (mode==M_XRAY){
      if (!front) continue;
      float fn=fabsf(A.nz+B.nz+C.nz)/3.0f;
      uint16_t col=tint?tint:C_DATA;
      RasterAdd(A,B,C,col,(uint8_t)(24+66*(1.0f-fn)));
      LineFB((int)A.sx,(int)A.sy,(int)B.sx,(int)B.sy,col,70);
      LineFB((int)B.sx,(int)B.sy,(int)C.sx,(int)C.sy,col,70);
      continue; }
    if (!front) continue;
    if (mode==M_WIRE){
      uint16_t col=tint?tint:C_ACCENT;
      uint8_t a=(uint8_t)(90+165*clampf((A.diff+B.diff+C.diff)/3.0f,0,1));
      LineFB((int)A.sx,(int)A.sy,(int)B.sx,(int)B.sy,col,a);
      LineFB((int)B.sx,(int)B.sy,(int)C.sx,(int)C.sy,col,a);
      continue; }
    if (mode==M_HOLO){
      // holographic: wireframe restricted to scanlines, depth-tinted, flickering
      uint16_t col=tint?tint:C_DATA;
      float fn=(A.nz+B.nz+C.nz)/3.0f;
      float iz=(A.invz+B.invz+C.invz)/3.0f;
      uint8_t base=(uint8_t)(50+150*clampf(1.0f+fn,0,1)*clampf(iz*2.4f,0.2f,1.0f));
      float flick=0.82f+0.18f*Hash((uint32_t)(gTime*22.0f)+t*7u);
      uint8_t a=(uint8_t)(base*flick);
      // only emit the edge where it crosses an even scanline -> hologram banding
      int ya=(int)A.sy, yb=(int)B.sy, yc=(int)C.sy;
      if (((ya^t)&1)==0) LineFB((int)A.sx,ya,(int)B.sx,yb,col,a);
      if (((yb^t)&1)==0) LineFB((int)B.sx,yb,(int)C.sx,yc,Dim(col,4,5),a);
      PxAdd((int)A.sx,ya,C_HILITE,(uint8_t)(a>>1));
      continue; }
    if (mode==M_VOXEL){
      // stylised voxels: one lit cube per triangle centroid, size by depth
      float iz=(A.invz+B.invz+C.invz)/3.0f;
      int vx=(int)((A.sx+B.sx+C.sx)/3.0f), vy=(int)((A.sy+B.sy+C.sy)/3.0f);
      int vs=clampi((int)(gVoxSize*iz*3.4f),1,7);
      float lit=clampf((A.diff+B.diff+C.diff)/3.0f,0,1);
      uint8_t cr2=m.tri[t].cr,cg2=m.tri[t].cg,cb2=m.tri[t].cb;
      if (tint){ cr2=((tint>>11)&0x1F)<<3; cg2=((tint>>5)&0x3F)<<2; cb2=(tint&0x1F)<<3; }
      uint16_t vc=(uint16_t)((((int)(cr2*lit)>>3)<<11)|(((int)(cg2*lit)>>2)<<5)|((int)(cb2*lit)>>3));
      FillRectFB(vx-vs/2,vy-vs/2,vs,vs,vc);
      HLineFB(vx-vs/2,vy-vs/2,vs,Fade(C_HILITE,(uint8_t)(90*lit)));
      VLineFB(vx+vs/2,vy-vs/2,vs,Dim(vc,2,5));
      continue; }
    if (mode==M_EDGE){
      // silhouette only: draw an edge where the facing flips
      uint16_t col=tint?tint:C_HILITE;
      float fn=(A.nz+B.nz+C.nz)/3.0f;
      if (fn>-0.34f){
        uint8_t a=(uint8_t)(120+135*clampf(1.0f+fn,0,1));
        LineFB((int)A.sx,(int)A.sy,(int)B.sx,(int)B.sy,col,a);
        LineFB((int)B.sx,(int)B.sy,(int)C.sx,(int)C.sy,col,a);
        LineFB((int)C.sx,(int)C.sy,(int)A.sx,(int)A.sy,col,a); }
      continue; }
    uint8_t cr=m.tri[t].cr,cg=m.tri[t].cg,cb=m.tri[t].cb;
    if (tint){ cr=((tint>>11)&0x1F)<<3; cg=((tint>>5)&0x3F)<<2; cb=(tint&0x1F)<<3; }
    if (mode==M_NORMALS){
      float nx=(A.nx+B.nx+C.nx)/3.0f,ny=(A.ny+B.ny+C.ny)/3.0f,nz=(A.nz+B.nz+C.nz)/3.0f;
      cr=(uint8_t)(150+80*nx); cg=(uint8_t)(130+70*ny); cb=(uint8_t)(110-60*nz);
      A.diff=B.diff=C.diff=1.0f;
    } else if (mode==M_FLAT){
      float d=(A.diff+B.diff+C.diff)/3.0f; A.diff=B.diff=C.diff=d;
    } else if (mode==M_NEON) A.diff=B.diff=C.diff=0.13f;
    else if (mode==M_DEPTH){
      // colour purely by distance: near = accent, far = background
      float iz=(A.invz+B.invz+C.invz)/3.0f;
      float f=clampf((iz-0.20f)*2.2f,0,1);
      uint16_t nearC=tint?tint:C_HILITE, farC=C_PANEL;
      cr=(uint8_t)((((nearC>>11)&0x1F)<<3)*f+(((farC>>11)&0x1F)<<3)*(1-f));
      cg=(uint8_t)((((nearC>>5)&0x3F)<<2)*f+(((farC>>5)&0x3F)<<2)*(1-f));
      cb=(uint8_t)(((nearC&0x1F)<<3)*f+((farC&0x1F)<<3)*(1-f));
      A.diff=B.diff=C.diff=1.0f; }
    else if (mode==M_DITHER){
      // quantise the lighting into 4 bands for a print-like look
      float d=(A.diff+B.diff+C.diff)/3.0f;
      d=floorf(clampf(d,0,1)*4.0f)/4.0f+0.12f;
      A.diff=B.diff=C.diff=d; }
    RasterTriangle(A,B,C,cr,cg,cb);
    if (mode==M_NEON){
      uint16_t col=tint?tint:C_ACCENT;
      float lit=clampf((gSV[ia2].diff+gSV[ib].diff+gSV[ic].diff)/3.0f,0,1);
      uint8_t a=(uint8_t)(70+185*lit);
      LineFB((int)A.sx,(int)A.sy,(int)B.sx,(int)B.sy,col,a);
      LineFB((int)B.sx,(int)B.sy,(int)C.sx,(int)C.sy,col,a); } }
}
static void ApplyLight(void){
  float ang=sLight*TAU;
  LX=fcos(ang)*0.75f; LY=-0.45f; LZ=fsin(ang)*0.75f-0.35f;
  float l=1.0f/sqrtf(LX*LX+LY*LY+LZ*LZ);
  LX*=l; LY*=l; LZ*=l;
}
void UpdateOrbit(float dt){
  const float SENS=0.011f,FR=3.4f;
  float autoRate=0.10f+sSpin*0.9f;
  if (touchActive&&pressY>BACK_H){
    float dx=(float)(touchX-lastTX),dy=(float)(touchY-lastTY);
    lastTX=touchX; lastTY=touchY;
    rotY+=dx*SENS; rotX+=dy*SENS;
    if (dt>0.0001f){
      dragVX=Approach(dragVX,(dy*SENS)/dt,18.0f,dt);
      dragVY=Approach(dragVY,(dx*SENS)/dt,18.0f,dt); }
    velX=velY=0;
  } else if (touchUp){
    velX=clampf(dragVX,-9,9); velY=clampf(dragVY,-9,9); dragVX=dragVY=0; }
  if (!touchActive){
    rotX+=velX*dt; rotY+=velY*dt;
    float dc=expf(-FR*dt);
    velX*=dc; velY*=dc;
    if (fabsf(velX)<0.02f) velX=0;
    if (fabsf(velY)<0.02f) velY=0;
    if (velX==0&&velY==0) rotY+=autoRate*dt; }
  if (rotX> 1.35f){ rotX= 1.35f; velX=0; }
  if (rotX<-1.35f){ rotX=-1.35f; velX=0; }
  if (rotY> TAU) rotY-=TAU;
  if (rotY<-TAU) rotY+=TAU;
}

// =====================================================================
//  NAVIGATION
// =====================================================================
void GoTo(int target,float fx,float fy,uint16_t col,int mode){
  if (transT>0) return;
  transT=0.001f; transTarget=target;
  transX=fx; transY=fy; transCol=col; transMode=mode;
  SpawnBurst(fx,fy,20,col,150.0f,PK_SPARK);
}
void TopBar(const char *title,uint16_t ac){
  // Minimal app header: chevron, title, one hairline. Nothing else.
  // The touch target is unchanged (BACK_W x BACK_H) even though the
  // visible mark is far smaller -- large hitbox, small ink.
  (void)ac;
  // The app header occupies y=0..18, the same band the original TopBar
  // used. Every legacy screen lays its first control row at y=22, so the
  // header must not grow past 18 -- when it briefly sat at y=18..32 it
  // covered 17 app buttons and their taps hit BACK instead.
  // While an app is open the shell suppresses its own status bar here
  // (see DrawStatusBar), so the clock never collides with the title.
  gAppHeader = true;
  bool over = touchActive && pressX<BACK_W && pressY<BACK_H;
  IconPack(IC_BACK,11,9,5,over?C_ACCENT:C_DIM,1);
  DrawText(24,6,title,C_TEXT,1);
  BlendRectFB(0,18,SCREEN_W,1,C_LINE,A_HAIR);
  if (gShowFps)
    DrawText(SCREEN_W-TextW(fpsStr,1)-GUTTER,6,fpsStr,C_OFF,1);
}
bool BackHit(void){
  if (touchDown && pressX<BACK_W && pressY<BACK_H && transT==0){
    // Under the mobile shell, BACK is the same action as the home
    // gesture: close the app with its reverse open animation. The
    // button stays as the always-available fallback (spec 6).
    ShellGoHome();
    return true; }
  return false;
}
static void EnterOverlay(void){
  if (enterAnim>=1.0f) return;
  float p=EaseOutCubic(enterAnim);
  Scrim((uint8_t)(A_FILL*(1.0f-p)));
  int wx=(int)(p*(SCREEN_W+60))-30;
  for (int i=0;i<20;i++){
    uint8_t a=(uint8_t)(A_GLOW*(1.0f-i/20.0f)*(1.0f-p));
    if (a) BlendRectFB(wx-i,0,1,SCREEN_H,C_HILITE,a); }
}
void TransitionDraw(float t){
  float p=(t<1.0f)?t:(2.0f-t);
  float e=EaseInOutCubic(clampf(p,0,1));
  switch (transMode){
    case TR_HEX: {
      const int CS=24;
      for (int gy=0;gy<SCREEN_H/CS+2;gy++)
        for (int gx=0;gx<SCREEN_W/CS+2;gx++){
          int cx=gx*CS+((gy&1)?CS/2:0), cy=gy*CS;
          float dx=cx-transX,dy=cy-transY;
          float d=sqrtf(dx*dx+dy*dy)/300.0f;
          float lp=clampf((e-d*0.55f)*2.4f,0,1);
          if (lp<=0) continue;
          int r=(int)(CS*0.62f*EaseOutBack(lp));
          HexFB(cx,cy,r,transCol,(uint8_t)(A_FILL*lp),true);
          if (lp<0.95f) HexFB(cx,cy,r,C_HILITE,(uint8_t)(200*(1.0f-lp)),false); }
    } break;
    case TR_GLITCH: {
      int bands=14;
      for (int i=0;i<bands;i++){
        int by=i*SCREEN_H/bands, bh=SCREEN_H/bands;
        float h=Hash(i*7919u+(uint32_t)(t*12.0f));
        int sh=(int)((h-0.5f)*90.0f*e);
        if (sh==0) continue;
        for (int y=by;y<by+bh&&y<SCREEN_H;y++){
          uint16_t *row=&frame[y*SCREEN_W];
          if (sh>0) memmove(row+sh,row,(SCREEN_W-sh)*2);
          else      memmove(row,row-sh,(SCREEN_W+sh)*2); }
        if (h>0.72f) BlendRectFB(0,by,SCREEN_W,bh,transCol,(uint8_t)(A_FILL*e)); }
      Scrim((uint8_t)(230*EaseInCubic(e)));
    } break;
    case TR_IRIS: {
      int r=(int)(300*(1.0f-e));
      for (int y=0;y<SCREEN_H;y+=2){
        int dy=y-(int)transY;
        float in=(float)(r*r-dy*dy);
        int half=(in>0)?(int)sqrtf(in):-1;
        if (half<0){ BlendRectFB(0,y,SCREEN_W,2,C_BG,A_FILL); continue; }
        int l=(int)transX-half, rr=(int)transX+half;
        if (l>0) BlendRectFB(0,y,l,2,C_BG,A_FILL);
        if (rr<SCREEN_W) BlendRectFB(rr,y,SCREEN_W-rr,2,C_BG,A_FILL); }
      RingFB((int)transX,(int)transY,r,transCol,A_FILL);
      RingFB((int)transX,(int)transY,r-3,C_HILITE,A_GLOW);
    } break;
    default: {
      float g=EaseInCubic(e);
      int rr=(int)(g*470);
      CircleFB((int)transX,(int)transY,rr,transCol,(uint8_t)(A_GLOW*e));
      CircleFB((int)transX,(int)transY,(int)(rr*0.68f),C_TEXT,(uint8_t)(A_FILL*e*e));
      int ring=(int)(EaseOutQuint(e)*320);
      RingFB((int)transX,(int)transY,ring,C_HILITE,(uint8_t)(A_FILL*(1.0f-e)));
      RingFB((int)transX,(int)transY,ring-4,transCol,(uint8_t)(A_GLOW*(1.0f-e)));
    } break; }
}

// =====================================================================
//  HOME  --  3 pages x 9 tiles, swipe or tap the pager
// =====================================================================
enum { GI_LAB=0, GI_GRID, GI_LAYER, GI_TARGET, GI_WARP, GI_MAZE,
       GI_2048, GI_BRICK, GI_BIRD, GI_SNAKE, GI_PONG, GI_TETRIS,
       GI_MEM, GI_SIMON, GI_MINE, GI_WHACK, GI_DODGE, GI_BULB,
       GI_BRUSH, GI_CLOCK, GI_WATCH, GI_TIMER, GI_WIFI, GI_GEAR,
       GI_CROSS, GI_CHIP, GI_JEE,
       GI_PHYS, GI_PART, GI_SPACE, GI_PLANET, GI_FRAC, GI_RAIN,
       GI_WAVE, GI_LIFE, GI_DEMO, GI_MOL, GI_TOUCH,
       GI_SEARCH, GI_CREATE, GI_GEST, GI_MORPH, GI_KAL, GI_EXPL,
       GI_VOX, GI_IMPOSS, GI_TUNNEL, GI_WELL, GI_BOID, GI_FISH,
       GI_ANT, GI_CHARGE, GI_RAG, GI_TLINE, GI_GRAPH, GI_PARAM,
       GI_SURF, GI_VEC, GI_MTX, GI_FOUR, GI_CLOCKS, GI_TROPHY };
struct HubItem { const char *label; uint8_t target; uint8_t icon; uint8_t hue; uint8_t tr; };
static const HubItem HUB[] = {
  { "3D LAB",  ST_LAB,     GI_LAB,    0, TR_HEX    },
  { "OBJECTS", ST_OBJECTS, GI_GRID,   4, TR_SHOCK  },
  { "MODES",   ST_MODES,   GI_LAYER,  5, TR_IRIS   },
  { "INSPECT", ST_INSPECT, GI_TARGET, 3, TR_HEX    },
  { "WARP",    ST_WARP,    GI_WARP,   1, TR_GLITCH },
  { "MAZE 3D", ST_MAZE,    GI_MAZE,   2, TR_HEX    },
  { "2048",    ST_2048,    GI_2048,   0, TR_HEX    },
  { "BREAKOUT",ST_BREAK,   GI_BRICK,  3, TR_SHOCK  },
  { "FLAPPY",  ST_FLAPPY,  GI_BIRD,   2, TR_IRIS   },

  { "SNAKE",   ST_SNAKE,   GI_SNAKE,  3, TR_HEX    },
  { "PONG",    ST_PONG,    GI_PONG,   4, TR_SHOCK  },
  { "TETRIS",  ST_TETRIS,  GI_TETRIS, 5, TR_IRIS   },
  { "MEMORY",  ST_MEMORY,  GI_MEM,    2, TR_HEX    },
  { "SIMON",   ST_SIMON,   GI_SIMON,  1, TR_SHOCK  },
  { "MINES",   ST_MINES,   GI_MINE,   0, TR_IRIS   },
  { "WHACK",   ST_WHACK,   GI_WHACK,  4, TR_HEX    },
  { "DODGE",   ST_DODGE,   GI_DODGE,  1, TR_GLITCH },
  { "LIGHTS",  ST_LIGHTS,  GI_BULB,   5, TR_SHOCK  },

  { "JEE",     ST_JEE,     GI_JEE,    0, TR_HEX    },
  { "DRAW",    ST_DRAW,    GI_BRUSH,  2, TR_IRIS   },
  { "CLOCK",   ST_CLOCK,   GI_CLOCK,  4, TR_SHOCK  },
  { "STOPWTCH",ST_STOPW,   GI_WATCH,  0, TR_HEX    },
  { "TIMER",   ST_TIMER,   GI_TIMER,  1, TR_HEX    },
  { "WIFI",    ST_WIFI,    GI_WIFI,   4, TR_SHOCK  },
  { "SETTINGS",ST_SETTINGS,GI_GEAR,   5, TR_IRIS   },
  { "CALIBRTE",ST_CALIB,   GI_CROSS,  3, TR_HEX    },
  { "SYSTEM",  ST_SYSTEM,  GI_CHIP,   4, TR_SHOCK  },

  // ---- page 4: SIMULATION (new in v5, nothing removed) ----
  { "PHYSICS", ST_PHYS,     GI_PHYS,   1, TR_HEX    },
  { "PARTICLE",ST_PSAND,    GI_PART,   4, TR_SHOCK  },
  { "SPACE",   ST_SPACE,    GI_SPACE,  5, TR_GLITCH },
  { "PLANET",  ST_PLANETGEN,GI_PLANET, 0, TR_IRIS   },
  { "FRACTAL", ST_FRACTAL,  GI_FRAC,   2, TR_HEX    },
  { "MATRIX",  ST_MATRIX,   GI_RAIN,   3, TR_GLITCH },
  { "FIELD",   ST_FIELD,    GI_WAVE,   4, TR_IRIS   },
  { "LIFE",    ST_LIFE,     GI_LIFE,   3, TR_HEX    },
  { "DEMO",    ST_DEMO,     GI_DEMO,   0, TR_SHOCK  },
  // ---- page 5 : WORLDS ----
  { "CREATOR", ST_CREATOR,  GI_CREATE, 1, TR_HEX    },
  { "GESTURE", ST_GESTURE,  GI_GEST,   4, TR_SHOCK  },
  { "MORPH",   ST_MORPH,    GI_MORPH,  5, TR_IRIS   },
  { "KALEIDO", ST_KALEIDO,  GI_KAL,    2, TR_GLITCH },
  { "EXPLODE", ST_EXPLODE,  GI_EXPL,   3, TR_SHOCK  },
  { "VOXEL",   ST_VOXEL,    GI_VOX,    0, TR_HEX    },
  { "ESCHER",  ST_IMPOSSIBLE,GI_IMPOSS,4, TR_IRIS   },
  { "TUNNEL",  ST_TUNNEL,   GI_TUNNEL, 1, TR_GLITCH },
  { "GRAVITY", ST_GRAVWELL, GI_WELL,   5, TR_HEX    },
  // ---- page 6 : LIFE + MATH ----
  { "BOIDS",   ST_BOIDS,    GI_BOID,   3, TR_IRIS   },
  { "AQUARIUM",ST_AQUARIUM, GI_FISH,   4, TR_HEX    },
  { "ANTS",    ST_ANTS,     GI_ANT,    1, TR_SHOCK  },
  { "CHARGES", ST_CHARGES,  GI_CHARGE, 5, TR_GLITCH },
  { "RAGDOLL", ST_RAGDOLL,  GI_RAG,    2, TR_SHOCK  },
  { "GRAPHER", ST_GRAPHER,  GI_GRAPH,  0, TR_HEX    },
  { "CURVES",  ST_PARAMETRIC,GI_PARAM, 4, TR_IRIS   },
  { "SURFACE", ST_SURFACE,  GI_SURF,   3, TR_HEX    },
  { "FOURIER", ST_FOURIER,  GI_FOUR,   1, TR_GLITCH },
  // ---- page 7 : STUDIO ----
  { "VECTORS", ST_VECTORS,  GI_VEC,    5, TR_HEX    },
  { "MATRIX3D",ST_MATRIXVIZ,GI_MTX,    2, TR_IRIS   },
  { "TIMELINE",ST_TIMELINE, GI_TLINE,  0, TR_SHOCK  },
  { "CLOCKS",  ST_CLOCKS,   GI_CLOCKS, 4, TR_HEX    },
  { "AWARDS",  ST_ACHIEVE,  GI_TROPHY, 3, TR_IRIS   },
  { "SEARCH",  ST_SEARCH,   GI_SEARCH, 1, TR_GLITCH },
  { "ANIM LAB",ST_ANIMLAB,  GI_RAG,    5, TR_SHOCK  },
  { "TOUCH",   ST_TOUCHPLAY,GI_TOUCH,  2, TR_HEX    },
  { "MOLECULE",ST_MOLECULE, GI_MOL,    0, TR_IRIS   }
};
#define HUB_N ((int)(sizeof(HUB)/sizeof(HUB[0])))
#define PAGE_N 7
#define PER_PAGE 9
static int   hubPage = 0;
static Spring hubS[PER_PAGE];
static Spring pageS;
static int   swipeX0 = 0;
static bool  swipeArmed = false;

static void HubPos(int slot,int &x,int &y){
  int c = slot % 3, r = slot / 3;
  x = 60 + c * 100;
  y = 74 + r * 52;
}

// ---- icons: crisp 1px vector art, all glow-lit -----------------------
static void Icon(uint8_t g,int cx,int cy,uint16_t c,uint8_t a,float t){
  switch (g){
    case GI_LAB:
      RingFB(cx,cy,9,c,a);
      for (int i=-2;i<=2;i++){ int w=(int)sqrtf(fmaxf(0.0f,(float)(81-i*i*16)));
        if (w>0) LineFB(cx-w,cy+i*4,cx+w,cy+i*4,Dim(c,2,5),a); }
      for (int i=0;i<18;i++){ float u=TAU*i/18.0f;
        PxBlend(cx+(int)(fcos(u)*13),cy+(int)(fsin(u)*4.5f*fcos(t*1.4f)),c,a); }
      break;
    case GI_GRID:
      for (int i=0;i<4;i++){ int ox=cx-9+(i%2)*10, oy=cy-9+(i/2)*10;
        HLineFB(ox,oy,8,c); HLineFB(ox,oy+7,8,c);
        VLineFB(ox,oy,8,c); VLineFB(ox+7,oy,8,c);
        if (i==((int)(t*2)&3)) BlendRectFB(ox+2,oy+2,4,4,c,a); }
      break;
    case GI_LAYER:
      for (int i=2;i>=0;i--){ int oy=cy-6+i*6;
        for (int k=-10;k<=10;k++)
          PxBlend(cx+k,oy+(int)(fsin(k*0.157f+t)*2),i==0?c:Dim(c,3-i,4),a); }
      break;
    case GI_TARGET:
      RingFB(cx,cy,10,c,a);
      RingFB(cx,cy,4+(int)(2*Pulse(t,3.0f)),Dim(c,3,5),a);
      LineFB(cx-14,cy,cx-6,cy,c,a); LineFB(cx+6,cy,cx+14,cy,c,a);
      LineFB(cx,cy-14,cx,cy-6,c,a); LineFB(cx,cy+6,cx,cy+14,c,a);
      break;
    case GI_WARP:
      for (int i=0;i<10;i++){ float u=TAU*i/10.0f+t*0.6f, r0=4+3*fsin(t*3+i);
        LineFB(cx+(int)(fcos(u)*r0),cy+(int)(fsin(u)*r0),
               cx+(int)(fcos(u)*13),cy+(int)(fsin(u)*13),c,a); }
      break;
    case GI_MAZE: {
      for (int i=0;i<4;i++){ int o=i*3;
        HLineFB(cx-11+o,cy-11+o,22-o*2,(i&1)?Dim(c,3,5):c);
        VLineFB(cx-11+o,cy-11+o,22-o*2,(i&1)?Dim(c,3,5):c); }
      int px=cx-9+(int)(fmodf(t*8,18.0f));
      PxAdd(px,cy+9,C_TEXT,220); PxAdd(px+1,cy+9,C_TEXT,140);
    } break;
    case GI_2048:
      for (int i=0;i<4;i++){ int ox=cx-10+(i%2)*11, oy=cy-10+(i/2)*11;
        uint16_t tc=Spec(i+(int)(t*0.7f));
        BlendRectFB(ox,oy,9,9,tc,a); Bracket(ox,oy,9,9,Dim(tc,4,5),3); }
      break;
    case GI_BRICK:
      for (int r=0;r<3;r++) for (int cc=0;cc<4;cc++)
        BlendRectFB(cx-12+cc*6,cy-11+r*4,5,3,Spec(r+1),a);
      { int px=cx+(int)(fsin(t*1.6f)*7);
        BlendRectFB(px-5,cy+9,10,2,c,a);
        CircleFB(px+(int)(fcos(t*2.2f)*3),cy+3,2,C_TEXT,a); }
      break;
    case GI_BIRD: {
      for (int oy=-3;oy<=3;oy++) for (int ox=-5;ox<=5;ox++)
        if (ox*ox*2+oy*oy*5<40) PxBlend(cx+ox,cy+oy,c,a);
      int wy=cy+(int)(fsin(t*6)*3);
      for (int ox=-4;ox<=1;ox++) PxBlend(cx+ox,wy+3,Dim(c,4,5),a);
      PxBlend(cx+3,cy-1,C_BG,a);
      LineFB(cx+5,cy,cx+8,cy+1,Spec(1),a);
    } break;
    case GI_SNAKE: {
      for (int i=0;i<7;i++){
        float u=t*2.0f-i*0.5f;
        int sx=cx-9+i*3, sy=cy+(int)(fsin(u)*6);
        BlendRectFB(sx,sy,3,3,i==6?C_TEXT:Dim(c,5-(i>>1),5),a); }
      CircleFB(cx+11,cy-5,2,Spec(1),a);
    } break;
    case GI_PONG:
      VLineFB(cx-12,cy-6+(int)(fsin(t*2)*4),12,c);
      VLineFB(cx+11,cy-6-(int)(fsin(t*2)*4),12,c);
      for (int i=-10;i<=10;i+=4) PxBlend(cx,cy+i,Dim(c,2,5),a);
      CircleFB(cx+(int)(fsin(t*3)*8),cy+(int)(fcos(t*2.3f)*5),2,C_TEXT,a);
      break;
    case GI_TETRIS: {
      const int8_t sh[4][2]={{0,0},{1,0},{0,1},{1,1}};
      int off=(int)(fmodf(t*6,10.0f));
      for (int i=0;i<4;i++)
        BlendRectFB(cx-10+sh[i][0]*6,cy-12+sh[i][1]*6+off,5,5,Spec(1),a);
      for (int i=0;i<4;i++)
        BlendRectFB(cx-2+sh[i][0]*6,cy+2+sh[i][1]*6,5,5,Spec(4),a);
    } break;
    case GI_MEM:
      for (int i=0;i<4;i++){ int ox=cx-10+(i%2)*11, oy=cy-10+(i/2)*11;
        bool up=(((int)(t*1.5f)+i)&1)==0;
        BlendRectFB(ox,oy,9,9,up?Spec(i):Dim(c,1,5),a);
        Bracket(ox,oy,9,9,Dim(c,4,5),3); }
      break;
    case GI_SIMON:
      for (int i=0;i<4;i++){
        float u=TAU*i/4.0f-0.785f;
        int ox=cx+(int)(fcos(u)*8), oy=cy+(int)(fsin(u)*8);
        bool lit=(((int)(t*2.0f))%4)==i;
        CircleFB(ox,oy,5,lit?Spec(i):Dim(Spec(i),1,4),a);
        if (lit) Glow(ox,oy,Spec(i),110,0.7f); }
      break;
    case GI_MINE:
      for (int i=0;i<3;i++) for (int j=0;j<3;j++){
        int ox=cx-11+i*8, oy=cy-11+j*8;
        HLineFB(ox,oy,7,Dim(c,3,5)); VLineFB(ox,oy,7,Dim(c,3,5)); }
      CircleFB(cx+3,cy+3,3,c,a);
      for (int i=0;i<4;i++){ float u=TAU*i/4.0f+0.4f;
        LineFB(cx+3,cy+3,cx+3+(int)(fcos(u)*6),cy+3+(int)(fsin(u)*6),c,a); }
      break;
    case GI_WHACK: {
      RingFB(cx,cy+6,9,Dim(c,3,5),a);
      int hy=cy+4-(int)(fabsf(fsin(t*2.4f))*9);
      CircleFB(cx,hy,5,c,a);
      PxBlend(cx-2,hy-1,C_BG,a); PxBlend(cx+2,hy-1,C_BG,a);
    } break;
    case GI_DODGE:
      for (int i=0;i<4;i++){
        float ph=fmodf(t*0.8f+i*0.25f,1.0f);
        int oy=cy-12+(int)(ph*22);
        BlendRectFB(cx-9+i*6,oy,4,4,Spec(i+1),(uint8_t)(a*(1.0f-ph*0.4f))); }
      CircleFB(cx+(int)(fsin(t*2)*6),cy+11,3,C_TEXT,a);
      break;
    case GI_BULB:
      for (int i=0;i<9;i++){
        int ox=cx-8+(i%3)*8, oy=cy-8+(i/3)*8;
        bool on=((int)(t*1.5f)+i)%3!=0;
        BlendRectFB(ox,oy,6,6,on?c:Dim(c,1,5),a);
        if (on) Glow(ox+3,oy+3,c,60,0.4f); }
      break;
    case GI_BRUSH:
      LineFB(cx-8,cy+8,cx+4,cy-6,c,a);
      LineFB(cx-6,cy+9,cx+6,cy-5,c,a);
      CircleFB(cx+6,cy-8,3,c,a);
      for (int i=0;i<3;i++) PxBlend(cx-10+i*2,cy+11,Dim(c,3,5),a);
      break;
    case GI_CLOCK: {
      // digital-style icon, matching the new numeric clock module
      Bracket(cx-12,cy-8,24,16,c,4);
      DrawText(cx-9,cy-3,"12",c,1);
      PxBlend(cx+2,cy-2,c,a); PxBlend(cx+2,cy+1,c,a);
      int bl=(int)(fmodf(t*2,2.0f));
      if (bl) DrawText(cx+4,cy-3,"00",Dim(c,3,5),1);
    } break;
    case GI_WATCH:
      RingFB(cx,cy+1,10,c,a);
      VLineFB(cx-2,cy-13,5,c);
      LineFB(cx,cy+1,cx+(int)(fcos(t*4-1.57f)*7),cy+1+(int)(fsin(t*4-1.57f)*7),Spec(1),a);
      PxBlend(cx,cy+1,C_TEXT,a);
      break;
    case GI_TIMER: {
      RingFB(cx,cy+1,10,Dim(c,3,5),a);
      float fr=fmodf(t*0.4f,1.0f);
      ArcFB(cx,cy+1,10,-1.5708f,-1.5708f+TAU*(1.0f-fr),c,a);
      VLineFB(cx-2,cy-13,5,c);
      DrawText(cx-2,cy-2,"3",c,1);
    } break;
    case GI_WIFI:
      for (int i=1;i<=3;i++){
        uint8_t aa=(netUp||((int)(t*2)%4)>=i)?a:(uint8_t)(a/4);
        ArcFB(cx,cy+8,i*5,-2.36f,-0.78f,c,aa); }
      CircleFB(cx,cy+8,2,c,a);
      break;
    case GI_GEAR:
      RingFB(cx,cy,7,c,a);
      for (int i=0;i<8;i++){ float u=TAU*i/8.0f+t*0.4f;
        LineFB(cx+(int)(fcos(u)*8),cy+(int)(fsin(u)*8),
               cx+(int)(fcos(u)*12),cy+(int)(fsin(u)*12),c,a); }
      CircleFB(cx,cy,3,Dim(c,3,5),a);
      break;
    case GI_CROSS: {
      RingFB(cx,cy,10,c,a);
      float ph=fmodf(t*0.8f,1.0f);
      RingFB(cx,cy,(int)(4+ph*12),c,(uint8_t)(a*(1.0f-ph)));
      LineFB(cx-13,cy,cx-4,cy,c,a); LineFB(cx+4,cy,cx+13,cy,c,a);
      LineFB(cx,cy-13,cx,cy-4,c,a); LineFB(cx,cy+4,cx,cy+13,c,a);
      PxBlend(cx,cy,C_TEXT,a);
    } break;
    case GI_JEE: {
      // live progress ring + book
      float f = JB ? JeeProgress() : 0;
      RingFB(cx,cy,11,Dim(c,3,5),a);
      ArcFB(cx,cy,11,-1.5708f,-1.5708f+TAU*f,c,255);
      ArcFB(cx,cy,10,-1.5708f,-1.5708f+TAU*f,c,A_GLOW);
      for (int i=0;i<3;i++){
        int bh=3+i*2+(int)(2*fsin(t*2.0f+i));
        BlendRectFB(cx-5+i*4,cy+4-bh,3,bh,c,a); }
      HLineFB(cx-6,cy+5,12,Dim(c,4,5));
    } break;
    case GI_PHYS: {            // pendulum + arc
      LineFB(cx,cy-11,cx+(int)(fsin(t*1.6f)*9),cy-11+(int)(fcos(t*1.6f)*13),c,a);
      CircleFB(cx+(int)(fsin(t*1.6f)*9),cy-11+(int)(fcos(t*1.6f)*13),4,c,a);
      ArcFB(cx,cy-11,13,0.6f,2.5f,Dim(c,3,5),a);
      PxBlend(cx,cy-11,C_TEXT,a);
    } break;
    case GI_PART: {            // swirling dots
      for (int i=0;i<10;i++){
        float u=TAU*i/10.0f+t*1.2f;
        float r=4+7*fsin(t*0.9f+i);
        PxAdd(cx+(int)(fcos(u)*r),cy+(int)(fsin(u)*r),Spec(i%6),220); }
      Glow(cx,cy,c,90,0.8f);
    } break;
    case GI_SPACE: {           // planet + ring + stars
      CircleFB(cx-2,cy,7,c,a);
      for (int k=0;k<20;k++){ float u=TAU*k/20;
        PxBlend(cx-2+(int)(fcos(u)*13),cy+(int)(fsin(u)*4),Dim(c,4,5),a); }
      for (int i=0;i<4;i++)
        PxAdd(cx+8-i*5,cy-9+((i*7)%5),C_TEXT,200);
    } break;
    case GI_PLANET: {          // banded globe
      for (int y=-9;y<=9;y++){
        int w=(int)sqrtf((float)(81-y*y));
        uint16_t bc=((y+9)/4)&1?c:Dim(c,3,5);
        BlendRectFB(cx-w,cy+y,w*2,1,bc,a); }
      RingFB(cx,cy,10,Dim(c,4,5),a);
      Glow(cx-3,cy-3,C_TEXT,60,0.7f);
    } break;
    case GI_FRAC: {            // recursive triangles
      for (int d=0;d<3;d++){
        int sz=11-d*3;
        int oy=cy-8+d*6;
        LineFB(cx,oy,cx-sz,oy+sz,c,a);
        LineFB(cx,oy,cx+sz,oy+sz,c,a);
        LineFB(cx-sz,oy+sz,cx+sz,oy+sz,c,a); }
    } break;
    case GI_RAIN: {            // falling glyph columns
      for (int i=0;i<5;i++){
        int x=cx-8+i*4;
        int y=cy-10+(int)fmodf(t*22+i*7,22.0f);
        PxAdd(x,y,C_TEXT,240);
        for (int k=1;k<4;k++) PxAdd(x,y-k*3,Spec(3),(uint8_t)(180-k*45)); }
    } break;
    case GI_WAVE: {            // interference rings
      for (int i=0;i<3;i++){
        float ph=fmodf(t*0.7f+i*0.33f,1.0f);
        RingFB(cx-5,cy,(int)(ph*11),c,(uint8_t)(a*(1.0f-ph)));
        RingFB(cx+5,cy,(int)(ph*11),Spec(4),(uint8_t)(a*(1.0f-ph)*0.7f)); }
    } break;
    case GI_LIFE: {            // glider
      const int8_t G[5][2]={{0,-1},{1,0},{-1,1},{0,1},{1,1}};
      int off=(int)fmodf(t*3,4.0f)-2;
      for (int i=0;i<5;i++)
        BlendRectFB(cx+G[i][0]*5-2+off,cy+G[i][1]*5-2,4,4,c,a);
      for (int gx=-2;gx<=2;gx++) for (int gy=-2;gy<=2;gy++)
        PxBlend(cx+gx*5,cy+gy*5,Dim(c,1,6),a);
    } break;
    case GI_DEMO: {            // play triangle in a ring
      RingFB(cx,cy,11,c,a);
      float pp=Pulse(t,2.0f);
      RingFB(cx,cy,(int)(11+pp*3),c,(uint8_t)(a*(1.0f-pp)));
      for (int i=0;i<9;i++)
        LineFB(cx-3,cy-6+i,cx-3+(9-i)/2+3,cy-6+i,c,a);
      Glow(cx,cy,c,(uint8_t)(70+50*pp),1.0f);
    } break;
    case GI_MOL: {
      CircleFB(cx,cy,4,c,a);
      for (int i=0;i<4;i++){ float u=TAU*i/4.0f+t*0.5f;
        int nx=cx+(int)(fcos(u)*10), ny=cy+(int)(fsin(u)*10);
        LineFB(cx,cy,nx,ny,Dim(c,3,5),a);
        CircleFB(nx,ny,3,Spec(i),a); }
    } break;
    case GI_TOUCH: {
      for (int i=0;i<3;i++){
        float ph=fmodf(t*0.9f+i*0.33f,1.0f);
        RingFB(cx,cy,(int)(3+ph*12),c,(uint8_t)(a*(1.0f-ph))); }
      CircleFB(cx,cy,3,C_TEXT,a);
    } break;
    case GI_CHIP:
      HLineFB(cx-7,cy-7,14,c); HLineFB(cx-7,cy+7,14,c);
      VLineFB(cx-7,cy-7,15,c); VLineFB(cx+7,cy-7,15,c);
      BlendRectFB(cx-3,cy-3,7,7,Dim(c,2,5),a);
      for (int i=-1;i<=1;i++){
        LineFB(cx+i*5,cy-11,cx+i*5,cy-8,c,a);
        LineFB(cx+i*5,cy+8,cx+i*5,cy+11,c,a);
        LineFB(cx-11,cy+i*5,cx-8,cy+i*5,c,a);
        LineFB(cx+8,cy+i*5,cx+11,cy+i*5,c,a); }
      break;
    // ---- v7 icons ----
    case GI_SEARCH:
      RingFB(cx-2,cy-2,8,c,a); RingFB(cx-2,cy-2,7,Dim(c,3,5),a);
      LineFB(cx+4,cy+4,cx+11,cy+11,c,a); LineFB(cx+5,cy+3,cx+12,cy+10,Dim(c,3,5),a);
      { int k=(int)(t*9)%8; PxAdd(cx-2+(int)(fcos(k*0.785f)*5),cy-2+(int)(fsin(k*0.785f)*5),C_HILITE,200); }
      break;
    case GI_CREATE:
      for (int i=0;i<3;i++){ int o=i*4-4;
        HexFB(cx+o,cy+o-2,6,Dim(c,3-i,4),(uint8_t)(a*0.8f),i==0); }
      LineFB(cx+4,cy-10,cx+4,cy-2,c,a); LineFB(cx,cy-6,cx+8,cy-6,c,a);
      break;
    case GI_GEST: {
      for (int i=0;i<22;i++){ float u=t*0.6f+i*0.3f;
        PxBlend(cx+(int)(fcos(u)*(10-i*0.3f)),cy+(int)(fsin(u)*(10-i*0.3f)),c,
                (uint8_t)(a*(1.0f-i/26.0f))); }
      CircleFB(cx+10,cy,2,C_HILITE,a);
    } break;
    case GI_MORPH: {
      float m=0.5f+0.5f*fsin(t*1.3f);
      for (int i=0;i<20;i++){ float u=TAU*i/20.0f;
        int sx=(int)(fcos(u)*11), sy=(int)(fsin(u)*11);
        int qx=(fcos(u)>0?10:-10), qy=(fsin(u)>0?10:-10);
        PxBlend(cx+(int)(sx*(1-m)+qx*m),cy+(int)(sy*(1-m)+qy*m),c,a); }
    } break;
    case GI_KAL:
      for (int k=0;k<6;k++){ float u=TAU*k/6.0f+t*0.4f;
        LineFB(cx,cy,cx+(int)(fcos(u)*12),cy+(int)(fsin(u)*12),Dim(c,3,5),a);
        LineFB(cx+(int)(fcos(u)*6),cy+(int)(fsin(u)*6),
               cx+(int)(fcos(u+1.047f)*6),cy+(int)(fsin(u+1.047f)*6),c,a); }
      break;
    case GI_EXPL: {
      float e=0.5f+0.5f*fsin(t*1.1f);
      for (int k=0;k<6;k++){ float u=TAU*k/6.0f;
        int ox=(int)(fcos(u)*e*9), oy=(int)(fsin(u)*e*9);
        BlendRectFB(cx+ox-2,cy+oy-2,5,5,c,a); }
      BlendRectFB(cx-2,cy-2,5,5,C_HILITE,a);
    } break;
    case GI_VOX:
      for (int r=0;r<3;r++) for (int q=0;q<3;q++){
        int ox=cx-11+q*8+r*2, oy=cy-9+r*7;
        BlendRectFB(ox,oy,6,6,Dim(c,4-r,4),a); Bracket(ox,oy,6,6,c,2); }
      break;
    case GI_IMPOSS: {
      const int R=11;
      for (int k=0;k<3;k++){ float u=TAU*k/3.0f-1.57f, v=TAU*(k+1)/3.0f-1.57f;
        LineFB(cx+(int)(fcos(u)*R),cy+(int)(fsin(u)*R),
               cx+(int)(fcos(v)*R),cy+(int)(fsin(v)*R),c,a);
        LineFB(cx+(int)(fcos(u)*(R-5)),cy+(int)(fsin(u)*(R-5)),
               cx+(int)(fcos(v)*(R-5)),cy+(int)(fsin(v)*(R-5)),Dim(c,3,5),a); }
    } break;
    case GI_TUNNEL:
      for (int i=0;i<5;i++){ float ph=fmodf(t*0.8f+i*0.2f,1.0f);
        int r=(int)(1+ph*ph*14);
        HexFB(cx,cy,r,c,(uint8_t)(a*(1.0f-ph*0.7f)),false); }
      break;
    case GI_WELL:
      for (int i=0;i<5;i++){ int rr=3+i*2;
        for (int k=0;k<14;k++){ float u=TAU*k/14.0f+t*(0.9f-i*0.13f);
          PxBlend(cx+(int)(fcos(u)*rr),cy+(int)(fsin(u)*rr*0.45f)+i,c,
                  (uint8_t)(a*(1.0f-i*0.16f))); } }
      Glow(cx,cy,C_HILITE,90,0.7f);
      break;
    case GI_BOID:
      for (int i=0;i<6;i++){ float u=t*0.7f+i*1.05f;
        int bx=cx+(int)(fcos(u)*9), by=cy+(int)(fsin(u*1.3f)*7);
        LineFB(bx,by,bx-3,by-2,c,a); LineFB(bx,by,bx-3,by+2,c,a); }
      break;
    case GI_FISH: {
      for (int oy=-3;oy<=3;oy++) for (int ox=-6;ox<=4;ox++)
        if (ox*ox+oy*oy*4<34) PxBlend(cx+ox,cy+oy,c,a);
      int tw=(int)(fsin(t*5)*3);
      LineFB(cx-6,cy,cx-11,cy+tw,c,a); LineFB(cx-6,cy,cx-11,cy-tw,c,a);
      PxBlend(cx+3,cy-1,C_BG,255);
      for (int i=0;i<3;i++) RingFB(cx+9,cy-4-i*4,1,Dim(c,3,5),a);
    } break;
    case GI_ANT:
      for (int i=0;i<7;i++){ float u=t*1.4f+i*0.7f;
        int ax=cx-10+(int)(fmodf(u*2.2f+i*3,21.0f)), ay=cy+(int)(fsin(u)*7);
        PxBlend(ax,ay,c,a); PxBlend(ax-1,ay,Dim(c,3,5),a); }
      RingFB(cx,cy,11,Dim(c,2,5),a);
      break;
    case GI_CHARGE:
      LineFB(cx-11,cy-4,cx-3,cy-4,c,a); LineFB(cx-7,cy-8,cx-7,cy,c,a);
      LineFB(cx+3,cy+4,cx+11,cy+4,Spec(3),a);
      for (int k=0;k<9;k++){ float u=TAU*k/9.0f;
        PxBlend(cx-7+(int)(fcos(u+t)*9),cy-4+(int)(fsin(u+t)*9),Dim(c,2,5),a); }
      break;
    case GI_RAG: {
      int hy=cy-8+(int)(fsin(t*2)*2);
      CircleFB(cx,hy,3,c,a);
      LineFB(cx,hy+3,cx,cy+3,c,a);
      LineFB(cx,cy-2,cx-6,cy+(int)(fsin(t*3)*4),c,a);
      LineFB(cx,cy-2,cx+6,cy+(int)(fcos(t*3)*4),c,a);
      LineFB(cx,cy+3,cx-4,cy+10,c,a); LineFB(cx,cy+3,cx+4,cy+10,c,a);
    } break;
    case GI_TLINE:
      HLineFB(cx-12,cy,24,c);
      for (int i=0;i<5;i++){ int kx=cx-12+i*6;
        VLineFB(kx,cy-3,7,Dim(c,3,5)); }
      { int px=cx-12+(int)(fmodf(t*7,24.0f));
        VLineFB(px,cy-8,17,C_HILITE); Glow(px,cy,C_HILITE,90,0.5f); }
      break;
    case GI_GRAPH:
      LineFB(cx-12,cy,cx+12,cy,Dim(c,2,5),a);
      LineFB(cx,cy-12,cx,cy+12,Dim(c,2,5),a);
      for (int i=-12;i<=12;i++)
        PxBlend(cx+i,cy-(int)(fsin(i*0.28f+t)*8),c,a);
      break;
    case GI_PARAM:
      for (int i=0;i<44;i++){ float u=TAU*i/44.0f;
        PxBlend(cx+(int)(fcos(u*3+t)*11),cy+(int)(fsin(u*2)*11),c,a); }
      break;
    case GI_SURF:
      for (int r=0;r<5;r++) for (int q=0;q<9;q++){
        int gx=cx-12+q*3, gy=cy+r*3-6-(int)(fsin(q*0.6f+r*0.4f+t)*4);
        PxBlend(gx,gy,Dim(c,5-r,5),a);
        if (q) LineFB(gx-3,gy,gx,gy,Dim(c,4-r/2,5),(uint8_t)(a*0.6f)); }
      break;
    case GI_VEC:
      LineFB(cx-9,cy+8,cx+6,cy-7,c,a);
      LineFB(cx+6,cy-7,cx+1,cy-6,c,a); LineFB(cx+6,cy-7,cx+5,cy-2,c,a);
      LineFB(cx-9,cy+8,cx+9,cy+4,Spec(3),a);
      LineFB(cx+9,cy+4,cx+4,cy+2,Spec(3),a); LineFB(cx+9,cy+4,cx+5,cy+7,Spec(3),a);
      break;
    case GI_MTX: {
      float sh=fsin(t)*0.4f;
      int px[4],py[4];
      for (int k=0;k<4;k++){ int bx=(k==0||k==3)?-9:9, by=(k<2)?-9:9;
        px[k]=cx+bx+(int)(by*sh); py[k]=cy+by; }
      for (int k=0;k<4;k++){ int n=(k+1)&3;
        LineFB(px[k],py[k],px[n],py[n],c,a); }
      Bracket(cx-12,cy-12,25,25,Dim(c,2,5),4);
    } break;
    case GI_FOUR: {
      float u=t*1.6f;
      int ax=cx-4+(int)(fcos(u)*6), ay=cy+(int)(fsin(u)*6);
      LineFB(cx-4,cy,ax,ay,c,a);
      LineFB(ax,ay,ax+(int)(fcos(u*3)*3),ay+(int)(fsin(u*3)*3),Spec(3),a);
      RingFB(cx-4,cy,6,Dim(c,2,5),a);
      for (int i=0;i<10;i++) PxBlend(cx+4+i,cy-(int)(fsin(u-i*0.4f)*5),C_HILITE,a);
    } break;
    case GI_CLOCKS:
      RingFB(cx,cy,11,c,a);
      for (int i=0;i<12;i++){ float u=TAU*i/12.0f;
        PxBlend(cx+(int)(fcos(u)*9),cy+(int)(fsin(u)*9),Dim(c,3,5),a); }
      LineFB(cx,cy,cx+(int)(fcos(t*0.6f-1.57f)*6),cy+(int)(fsin(t*0.6f-1.57f)*6),c,a);
      LineFB(cx,cy,cx+(int)(fcos(t*2.4f-1.57f)*9),cy+(int)(fsin(t*2.4f-1.57f)*9),C_HILITE,a);
      break;
    case GI_TROPHY:
      BlendRectFB(cx-6,cy-9,13,9,c,a);
      RingFB(cx-8,cy-6,3,Dim(c,3,5),a); RingFB(cx+8,cy-6,3,Dim(c,3,5),a);
      VLineFB(cx,cy,6,c); HLineFB(cx-6,cy+6,13,c);
      { uint8_t g=(uint8_t)(60+90*Pulse(t,2.0f)); Glow(cx,cy-5,C_HILITE,g,0.8f); }
      break; }
}

static const char *PAGE_NAME[PAGE_N] = { "GRAPHICS", "ARCADE", "TOOLS", "SIMULATION",
                                         "WORLDS", "LIFE + MATH", "STUDIO" };

void ScreenHome(float dt){
  enterAnim=clampf(enterAnim+dt*1.6f,0,1);
  Backdrop();
  SpringTo(pageS,(float)hubPage,300.0f,22.0f,dt);

  // page swipe
  if (touchDown && touchY>44){ swipeArmed=true; swipeX0=touchX; }
  if (touchUp && swipeArmed){
    swipeArmed=false;
    int dx=touchX-swipeX0;
    if (dx>50 && hubPage>0){ hubPage--; SpawnBurst(40,120,10,C_ACCENT,110.0f,PK_SPARK); }
    else if (dx<-50 && hubPage<PAGE_N-1){ hubPage++; SpawnBurst(280,120,10,C_ACCENT,110.0f,PK_SPARK); }
  }

  int hit=-1;
  int base=hubPage*PER_PAGE;
  for (int i=0;i<PER_PAGE;i++){
    if (base+i>=HUB_N) break;
    int hx,hy; HubPos(i,hx,hy);
    int dx=touchX-hx, dy=touchY-hy;
    if (dx*dx+dy*dy < 25*25) hit=i; }
  if (touchUp && hit>=0 && !swipeArmed && transT==0 && base+hit<HUB_N){
    int hx,hy; HubPos(hit,hx,hy);
    const HubItem &it=HUB[base+hit];
    GoTo(it.target,hx,hy,Spec(it.hue),it.tr); }
  for (int i=0;i<PER_PAGE;i++)
    SpringTo(hubS[i],(touchActive&&hit==i)?1.0f:0.0f,420.0f,26.0f,dt);

  float e=enterAnim;
  GlowText(8,6,"NEXUS OS",C_ACCENT,2,90);
  DrawTextDecode(8,24,PAGE_NAME[hubPage],C_SAND,1,clampf(e*1.8f-0.2f,0,1));
  if (gShowFps){ DrawText(SCREEN_W-TextW(fpsStr,1)-8,6,fpsStr,C_DATA,1);
                 DrawText(SCREEN_W-TextW(fpsStr,1)-20,6,"FPS",C_HAIR,1); }
  DrawText(SCREEN_W-TextW(ipStr,1)-8,20,ipStr,netUp?C_DATA:C_HAIR,1);
  HLineFB(8,34,SCREEN_W-16,C_HAIR);
  { float sw=fmodf(gTime*0.28f,1.4f);
    int sx=(int)(8+sw*(SCREEN_W-16));
    for (int i=-24;i<=24;i++) PxAdd(sx+i,34,C_HILITE,(uint8_t)(190-abs(i)*7)); }

  float pageOff=(pageS.v-hubPage)*-320.0f;
  for (int i=0;i<PER_PAGE;i++){
    int idx=base+i;
    if (idx>=HUB_N) break;
    float d=clampf((enterAnim-i*0.045f)/0.5f,0,1);
    if (d<=0.001f) continue;
    float ee=EaseAnticipate(d), vis=EaseOutCubic(d);
    int hx,hy; HubPos(i,hx,hy);
    hx+=(int)pageOff;
    int sy=hy+(int)((1.0f-ee)*20.0f);
    const HubItem &it=HUB[idx];
    uint16_t c=Spec(it.hue);
    uint8_t a=(uint8_t)(255*vis);
    float press=clampf(hubS[i].v,0,1.4f);
    float br=Pulse(gTime+i*0.6f,1.5f);
    int r=20+(int)(br*1.2f)-(int)(press*3.0f);
    HexFB(hx,sy,r,Dim(c,1,6),(uint8_t)(A_FILL*vis),true);
    HexFB(hx,sy,r,c,a,false);
    Glow(hx,sy,c,(uint8_t)((26+18*br)*vis),1.5f);
    if (press>0.01f){
      HexFB(hx,sy,r-3,c,(uint8_t)(A_FILL*press),false);
      Glow(hx,sy,C_HILITE,(uint8_t)(120*press),1.8f); }
    Icon(it.icon,hx,sy-1,press>0.4f?C_HILITE:c,a,gTime+i);
    DrawTextC(hx,sy+r+3,it.label,press>0.4f?C_HILITE:(vis>0.9f?C_TEXT:Dim(C_TEXT,3,5)),1);
    // JEE tile carries a live mini summary, as requested
    if (it.target==ST_JEE && JB && vis>0.85f){
      char s1[16],s2[16];
      snprintf(s1,sizeof(s1),"%uH%02u",
               (unsigned)(JeeTodayMin()/60),(unsigned)(JeeTodayMin()%60));
      snprintf(s2,sizeof(s2),"%d%%  %u",(int)(JeeProgress()*100),(unsigned)JeeStreak());
      DrawTextC(hx,sy-r-16,s1,C_DATA,1);
      DrawTextC(hx,sy-r-7,s2,C_SAND,1); } }

  // pager dots
  for (int p=0;p<PAGE_N;p++){
    int px=SCREEN_W/2-(PAGE_N*14)/2+p*14+7;
    bool on=(p==hubPage);
    CircleFB(px,SCREEN_H-9,on?4:2,on?C_ACCENT:C_HAIR,255);
    if (on) Glow(px,SCREEN_H-9,C_ACCENT,110,0.8f);
    if (touchDown && abs(touchX-px)<10 && touchY>SCREEN_H-22) hubPage=p; }
  DrawText(6,SCREEN_H-13,"<",hubPage>0?C_SAND:C_HAIR,1);
  DrawText(SCREEN_W-12,SCREEN_H-13,">",hubPage<PAGE_N-1?C_SAND:C_HAIR,1);

  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  CALIBRATION  --  FULL SCREEN, no top bar, no overlapping controls.
//  Solves the affine transform on-chip by least squares, saves to NVS,
//  then returns to HOME automatically.
// =====================================================================
#define CAL_PTS 5
static const int16_t CAL_TX[CAL_PTS] = {  20, 300, 160,  20, 300 };
static const int16_t CAL_TY[CAL_PTS] = {  18,  18, 120, 222, 222 };
static int   calStage=0, calSamples=0;
static float calSumX=0, calSumY=0;
static float calRawX[CAL_PTS], calRawY[CAL_PTS];
static float calResid=0, calDoneT=0;
static uint32_t calHoldStart=0;
static bool  calFinished=false;

void CalibReset(void){
  calStage=0; calSamples=0; calSumX=calSumY=0;
  calResid=0; calFinished=false; calDoneT=0;
}
static bool Solve3(float A[3][3],float b[3],float out[3]){
  float m[3][4];
  for (int i=0;i<3;i++){ for (int j=0;j<3;j++) m[i][j]=A[i][j]; m[i][3]=b[i]; }
  for (int c=0;c<3;c++){
    int piv=c;
    for (int r=c+1;r<3;r++) if (fabsf(m[r][c])>fabsf(m[piv][c])) piv=r;
    if (fabsf(m[piv][c])<1e-9f) return false;
    if (piv!=c) for (int j=0;j<4;j++){ float t=m[c][j]; m[c][j]=m[piv][j]; m[piv][j]=t; }
    float d=m[c][c];
    for (int j=0;j<4;j++) m[c][j]/=d;
    for (int r=0;r<3;r++){ if (r==c) continue;
      float f=m[r][c];
      for (int j=0;j<4;j++) m[r][j]-=f*m[c][j]; } }
  out[0]=m[0][3]; out[1]=m[1][3]; out[2]=m[2][3];
  return true;
}
static void CalibSolve(void){
  float M[3][3]={{0,0,0},{0,0,0},{0,0,0}}, bx[3]={0,0,0}, by[3]={0,0,0};
  for (int i=0;i<CAL_PTS;i++){
    float v[3]={calRawX[i],calRawY[i],1.0f};
    for (int r=0;r<3;r++){
      for (int c=0;c<3;c++) M[r][c]+=v[r]*v[c];
      bx[r]+=v[r]*(float)CAL_TX[i];
      by[r]+=v[r]*(float)CAL_TY[i]; } }
  float sx[3],sy[3];
  // keep the old values so a bad solve can be rolled back
  float oA=CAL_A,oB=CAL_B,oC=CAL_C,oD=CAL_D,oE=CAL_E,oF=CAL_F;
  float oGX=EDGE_GX,oGY=EDGE_GY;
  if (Solve3(M,bx,sx)&&Solve3(M,by,sy)){
    CAL_A=sx[0]; CAL_B=sx[1]; CAL_C=sx[2];
    CAL_D=sy[0]; CAL_E=sy[1]; CAL_F=sy[2];
    EDGE_GX=1.0f; EDGE_GY=1.0f;
    float sum=0;
    for (int i=0;i<CAL_PTS;i++){
      float px=CAL_A*calRawX[i]+CAL_B*calRawY[i]+CAL_C;
      float py=CAL_D*calRawX[i]+CAL_E*calRawY[i]+CAL_F;
      sum+=sqrtf((px-CAL_TX[i])*(px-CAL_TX[i])+(py-CAL_TY[i])*(py-CAL_TY[i])); }
    calResid=sum/CAL_PTS;
    // validate before trusting: NaN, wild scale or huge error = reject
    // NaN check without <cmath> ambiguity: NaN != itself
    bool finite = (CAL_A==CAL_A)&&(CAL_B==CAL_B)&&(CAL_C==CAL_C)&&
                  (CAL_D==CAL_D)&&(CAL_E==CAL_E)&&(CAL_F==CAL_F);
    bool sane = finite &&
                fabsf(CAL_A)<8.0f&&fabsf(CAL_E)<8.0f&&
                fabsf(CAL_C)<2000.0f&&fabsf(CAL_F)<2000.0f&&
                calResid>=0.0f&&calResid<28.0f;
    if (sane){
      SaveCalibration();
      Serial.printf("CAL mean residual %.2f px -- saved\n",calResid);
    } else {
      CAL_A=oA; CAL_B=oB; CAL_C=oC; CAL_D=oD; CAL_E=oE; CAL_F=oF;
      EDGE_GX=oGX; EDGE_GY=oGY;
      calResid=-1;
      Serial.println("CAL rejected (bad fit) -- previous values kept"); }
  } else { calResid=-1; Serial.println("CAL solve failed"); }
  calFinished=true; calDoneT=0;
}
void ScreenCalib(float dt){
  // Deliberately NO TopBar and NO BACK button: a stray press near the
  // corner target would otherwise exit mid-calibration.
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t *)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;

  for (int y=8;y<SCREEN_H;y+=16)
    for (int x=8;x<SCREEN_W;x+=16) PxBlend(x,y,C_HAIR,80);
  Bracket(4,4,SCREEN_W-8,SCREEN_H-8,Dim(C_ACCENT,3,5),16);

  if (!calFinished){
    // consume the cached raw sample -- never issue a second I2C read
    uint16_t rx=rawTX, ry=rawTY;
    bool got=rawValid;
    if (got){
      if (calSamples==0) calHoldStart=millis();
      calSumX+=rx; calSumY+=ry; calSamples++;
      if (calSamples>=14 && millis()-calHoldStart>300){
        calRawX[calStage]=calSumX/calSamples;
        calRawY[calStage]=calSumY/calSamples;
        Serial.printf("CAL %d/%d target(%d,%d) raw(%.1f,%.1f)\n",
          calStage+1,CAL_PTS,CAL_TX[calStage],CAL_TY[calStage],
          calRawX[calStage],calRawY[calStage]);
        SpawnBurst(CAL_TX[calStage],CAL_TY[calStage],24,C_ACCENT,170.0f,PK_SPARK);
        calStage++; calSamples=0; calSumX=calSumY=0;
        if (calStage>=CAL_PTS) CalibSolve(); }
    } else { calSamples=0; calSumX=calSumY=0; }

    if (!calFinished){
      int tx=CAL_TX[calStage], ty=CAL_TY[calStage];
      for (int i=0;i<3;i++){
        float ph=fmodf(gTime*0.9f+i*0.33f,1.0f);
        RingFB(tx,ty,(int)(6+ph*26),C_ACCENT,(uint8_t)(A_FILL*(1.0f-ph))); }
      LineFB(tx-18,ty,tx-5,ty,C_TEXT,255); LineFB(tx+5,ty,tx+18,ty,C_TEXT,255);
      LineFB(tx,ty-18,tx,ty-5,C_TEXT,255); LineFB(tx,ty+5,tx,ty+18,C_TEXT,255);
      CircleFB(tx,ty,3,C_HILITE,255);
      Glow(tx,ty,C_ACCENT,(uint8_t)(110+70*Pulse(gTime,4.0f)),2.0f);
      if (calSamples>0){
        float fr=clampf(calSamples/14.0f,0,1);
        ArcFB(tx,ty,22,-1.5708f,-1.5708f+TAU*fr,C_HILITE,255);
        ArcFB(tx,ty,21,-1.5708f,-1.5708f+TAU*fr,C_HILITE,A_GLOW); }
      GlowTextC(160,96,"CALIBRATION",C_ACCENT,2,90);
      char b[40];
      snprintf(b,sizeof(b),"POINT %d OF %d",calStage+1,CAL_PTS);
      DrawTextC(160,120,b,C_TEXT,1);
      DrawTextC(160,134,"HOLD ON THE CROSSHAIR",C_SAND,1);
      DrawTextC(160,146,"UNTIL THE RING FILLS",C_SAND,1);
      for (int i=0;i<CAL_PTS;i++){
        int dx=160-(CAL_PTS*12)/2+i*12+6;
        CircleFB(dx,168,i<calStage?4:2,i<calStage?C_ACCENT:C_HAIR,255);
        if (i<calStage) Glow(dx,168,C_ACCENT,90,0.6f); } }
  } else {
    // success screen, then auto-return to HOME
    calDoneT+=dt;
    float p=clampf(calDoneT/0.5f,0,1);
    GlowTextC(160,72,"CALIBRATED",C_ACCENT,3,(uint8_t)(110*p));
    char b[48];
    if (calResid>=0){
      snprintf(b,sizeof(b),"MEAN ERROR %d.%d PX",(int)calResid,((int)(calResid*10))%10);
      DrawTextC(160,108,b,C_DATA,1);
      DrawTextC(160,124,calResid<4.0f?"EXCELLENT":(calResid<9.0f?"GOOD":"POOR - RERUN"),
                calResid<9.0f?C_ACCENT:C_WARN,1);
    } else DrawTextC(160,110,"SOLVE FAILED",C_WARN,1);
    DrawTextC(160,146,"SAVED TO FLASH",C_SAND,1);
    int bw=(int)(180*clampf(calDoneT/1.8f,0,1));
    HLineFB(70,170,180,C_HAIR);
    FillRectFB(70,170,bw,3,C_ACCENT);
    Glow(70+bw,171,C_HILITE,150,0.8f);
    DrawTextC(160,182,"RETURNING TO MENU",C_HAIR,1);
    if (calDoneT>1.9f && transT==0) GoTo(ST_HOME,160,120,C_ACCENT,TR_IRIS);
  }
  DrawParticles();
}

// =====================================================================
//  3D LAB / OBJECTS / MODES / INSPECT / WARP
// =====================================================================
void ScreenLab(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  bool onSlider=false;
  UpdateOrbit(dt);
  ApplyLight();
  Backdrop();
  Panel(4,22,166,190,"VIEWPORT",C_ACCENT,"LIVE");
  float camz=2.3f+(1.0f-sCam)*2.6f;
  RenderMesh(gMesh[gObj],rotX,rotY,rotZ,1.0f,87.0f,118.0f,camz,gMode,
             (gMode==M_WIRE||gMode==M_NEON||gMode==M_POINTS)?C_ACCENT:0);
  Glow(87,118,C_ACCENT,30,2.2f);
  for (int i=-6;i<=6;i++){ PxBlend(87+i,118,C_ACCENT,60); PxBlend(87,118+i,C_ACCENT,60); }
  char b[32];
  snprintf(b,sizeof(b),"RX %d",(int)(rotX*57.3f)); DrawText(10,200,b,C_DATA,1);
  snprintf(b,sizeof(b),"RY %d",(int)(rotY*57.3f)); DrawText(66,200,b,C_DATA,1);
  snprintf(b,sizeof(b),"Z %d",(int)(camz*10));     DrawText(122,200,b,C_DATA,1);
  Panel(174,22,142,190,"CONTROL",C_ACCENT,"CFG");
  Graph(180,42,128,24,histFps,histHead,C_DATA);
  onSlider |= SliderRow(180,86,120,"LIGHT",&sLight,C_ACCENT);
  onSlider |= SliderRow(180,116,120,"CAMERA",&sCam,C_ACCENT);
  onSlider |= SliderRow(180,146,120,"SPIN",&sSpin,C_ACCENT);
  if (Button(178,172,64,17,"MODE",C_ACCENT,false)){
    gMode=(gMode+1)%NUM_MODES; SpawnBurst(210,180,12,C_ACCENT,110.0f,PK_SPARK); }
  if (Button(246,172,66,17,"OBJ",C_ACCENT,false)){
    gObj=(gObj+1)%NUM_OBJ; SpawnBurst(279,180,12,C_ACCENT,110.0f,PK_SPARK); }
  DrawTextC(245,196,MODE_NAME[gMode],C_TEXT,1);
  DrawTextC(245,206,OBJ_NAME[gObj],C_SAND,1);
  DrawParticles();
  TopBar("3D LAB",C_ACCENT);
  EnterOverlay();
}

static Spring objS[NUM_OBJ];
void ScreenObjects(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  ApplyLight();
  Backdrop();
  { char cntTag[8]; snprintf(cntTag,sizeof(cntTag),"%d",NUM_OBJ);
    Panel(4,22,312,212,"OBJECT LIBRARY",C_ACCENT,cntTag); }
  // --- hidden: long-press the panel title reveals a secret solid ------
  {
    static uint32_t oh=0;
    if (touchActive && touchY>22 && touchY<38 && touchX>100 && touchX<210){
      if (!oh) oh=millis();
      else if (millis()-oh>1500){ oh=0;
        EggFire(EGG_MIRROR,3.0f,"HIDDEN SOLID"); gObj=14; }
    } else oh=0;
  }
  const int gx=10,gy=42,cw=76,ch=62;
  int hit=-1;
  for (int i=0;i<NUM_OBJ;i++){
    int x=gx+(i%4)*(cw+1), y=gy+(i/4)*(ch+2);
    if (touchX>=x&&touchX<x+cw&&touchY>=y&&touchY<y+ch) hit=i; }
  if (touchDown&&hit>=0){ gObj=hit;
    SpawnBurst(gx+(hit%4)*(cw+1)+cw/2,gy+(hit/4)*(ch+2)+ch/2,14,Spec(hit),120.0f,PK_SPARK); }
  if (touchUp&&hit>=0&&hit==gObj&&transT==0)
    GoTo(ST_INSPECT,gx+(hit%4)*(cw+1)+cw/2,gy+(hit/4)*(ch+2)+ch/2,Spec(hit),TR_IRIS);
  for (int i=0;i<NUM_OBJ;i++){
    float d=Stagger(enterAnim,i,0.03f,0.42f);
    if (d<=0.01f) continue;
    bool sel=(i==gObj);
    SpringTo(objS[i],sel?1.0f:0.0f,300.0f,20.0f,dt);
    int x=gx+(i%4)*(cw+1);
    int y=gy+(i/4)*(ch+2)+(int)((1.0f-d)*16.0f)-(int)(objS[i].v*2.0f);
    uint16_t hue=Spec(i), col=sel?hue:Dim(hue,2,5);
    BlendRectFB(x,y,cw,ch,C_PANEL,(uint8_t)(A_FILL*d));
    Bracket(x,y,cw,ch,col,7);
    if (objS[i].v>0.02f){
      int p=(int)(fmodf(gTime*1.2f,1.0f)*(2*(cw+ch)));
      for (int k=0;k<14;k++){
        int q2=(p+k)%(2*(cw+ch)); int px,py;
        if (q2<cw){ px=x+q2; py=y; }
        else if (q2<cw+ch){ px=x+cw-1; py=y+(q2-cw); }
        else if (q2<2*cw+ch){ px=x+cw-1-(q2-cw-ch); py=y+ch-1; }
        else { px=x; py=y+ch-1-(q2-2*cw-ch); }
        PxAdd(px,py,C_HILITE,(uint8_t)(k*14*objS[i].v)); }
      Glow(x+cw/2,y+ch/2-4,hue,(uint8_t)(42*objS[i].v),2.0f); }
    RenderMesh(gMesh[i],0.35f,gTime*(0.45f+i*0.07f),0,
               0.46f*d*(1.0f+objS[i].v*0.10f),x+cw/2.0f,y+ch/2.0f-4,3.0f,
               sel?gMode:M_SMOOTH,
               (sel&&(gMode==M_WIRE||gMode==M_NEON||gMode==M_POINTS))?hue:0);
    DrawTextC(x+cw/2,y+ch-10,OBJ_NAME[i],sel?C_TEXT:C_SAND,1); }
  char tb[24]; snprintf(tb,sizeof(tb),"%d TRI",gMesh[gObj].nt);
  DrawText(10,224,tb,C_DATA,1);
  DrawTextC(SCREEN_W/2+30,224,"TAP SELECT  -  AGAIN TO INSPECT",C_SAND,1);
  DrawParticles();
  TopBar("OBJECTS",C_ACCENT);
  EnterOverlay();
}

static Spring modeS[NUM_MODES];
void ScreenModes(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  ApplyLight();
  Backdrop();
  Panel(4,22,312,212,"RENDER MODES",C_ACCENT,"8");
  const int gx=12,gy=46,cw=75,ch=84;
  int hit=-1;
  for (int i=0;i<NUM_MODES;i++){
    int x=gx+(i%4)*cw, y=gy+(i/4)*ch;
    if (touchX>=x&&touchX<x+cw-4&&touchY>=y&&touchY<y+ch-6) hit=i; }
  if (touchDown&&hit>=0){ gMode=hit;
    SpawnBurst(gx+(hit%4)*cw+(cw-4)/2,gy+(hit/4)*ch+30,16,Spec(hit),130.0f,PK_SPARK); }
  for (int i=0;i<NUM_MODES;i++){
    float d=Stagger(enterAnim,i,0.04f,0.45f);
    if (d<=0.01f) continue;
    bool sel=(i==gMode);
    SpringTo(modeS[i],sel?1.0f:0.0f,320.0f,20.0f,dt);
    int x=gx+(i%4)*cw, y=gy+(i/4)*ch;
    int ccx=x+(cw-4)/2, ccy=y+30;
    uint16_t hue=Spec(i), col=sel?hue:Dim(hue,2,5);
    CircleFB(ccx,ccy,(int)(26*d),C_PANEL,A_FILL);
    RingFB(ccx,ccy,(int)(26*d),col,(uint8_t)(A_FILL*d));
    if (modeS[i].v>0.02f){
      RingFB(ccx,ccy,(int)((29+modeS[i].v*2)*d),C_HILITE,
             (uint8_t)(A_GLOW+90*Pulse(gTime,4.0f)));
      ArcFB(ccx,ccy,33,gTime*1.6f,gTime*1.6f+1.2f,hue,A_FILL);
      ArcFB(ccx,ccy,33,gTime*1.6f+3.14f,gTime*1.6f+4.34f,hue,A_FILL);
      Glow(ccx,ccy,hue,(uint8_t)(42*modeS[i].v),2.0f); }
    RenderMesh(gMesh[3],0.4f,gTime*0.8f+i*0.5f,0,0.60f*d*(1.0f+modeS[i].v*0.12f),
               (float)ccx,(float)ccy,3.0f,i,
               (i==M_WIRE||i==M_NEON||i==M_POINTS||i==M_XRAY)?hue:0);
    DrawTextC(ccx,y+62,MODE_NAME[i],sel?C_TEXT:C_SAND,1); }
  char b[44]; snprintf(b,sizeof(b),"ACTIVE %s",MODE_NAME[gMode]);
  DrawTextC(SCREEN_W/2,224,b,C_SAND,1);
  DrawParticles();
  TopBar("MODES",C_ACCENT);
  EnterOverlay();
}

void ScreenInspect(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (touchDown&&touchX>=252&&touchX<312&&touchY>=214&&touchY<230){
    gObj=(gObj+1)%NUM_OBJ; SpawnBurst(282,222,14,C_ACCENT,120.0f,PK_SPARK); }
  UpdateOrbit(dt);
  ApplyLight();
  Backdrop();
  Panel(4,22,118,212,"READOUT",C_ACCENT,0);
  char b[32]; int ty=44;
  DrawTextDecode(10,ty,OBJ_NAME[gObj],C_TEXT,2,clampf(enterAnim*2.2f-0.3f,0,1)); ty+=20;
  HLineFB(10,ty,104,Dim(C_ACCENT,2,5)); ty+=6;
  snprintf(b,sizeof(b),"VERTS %d",gMesh[gObj].nv); DrawText(10,ty,b,C_DATA,1); ty+=11;
  snprintf(b,sizeof(b),"TRIS  %d",gMesh[gObj].nt); DrawText(10,ty,b,C_DATA,1); ty+=11;
  DrawText(10,ty,MODE_NAME[gMode],C_ACCENT,1); ty+=14;
  snprintf(b,sizeof(b),"ROT X %d",(int)(rotX*57.3f)); DrawText(10,ty,b,C_DATA,1); ty+=11;
  snprintf(b,sizeof(b),"ROT Y %d",(int)(rotY*57.3f)); DrawText(10,ty,b,C_DATA,1); ty+=14;
  DrawText(10,ty,"Z-BUFFER",C_SAND,1); ty+=11;
  DrawText(10,ty,"GOURAUD",C_SAND,1);
  for (int i=0;i<8;i++){
    int h=3+(int)(9*fabsf(fsin(gTime*3.0f+i*0.6f)));
    FillRectFB(12+i*12,214-h,8,h,Dim(C_ACCENT,3,5));
    PxAdd(12+i*12+4,214-h,C_HILITE,180); }
  Panel(126,22,190,212,"INSPECTION",C_ACCENT,"3D");
  float ccx=210.0f,ccy=128.0f;
  RenderMesh(gMesh[gObj],rotX,rotY,rotZ,1.15f,ccx,ccy,3.0f,gMode,
             (gMode==M_WIRE||gMode==M_NEON||gMode==M_POINTS||gMode==M_XRAY)?C_ACCENT:0);
  { float p=fmodf(gTime*0.45f,1.0f);
    int y=(int)(ccy-66+p*132);
    float in=(float)(66*66-(y-ccy)*(y-ccy));
    if (in>0){ int half=(int)sqrtf(in);
      LineAdd((int)ccx-half,y,(int)ccx+half,y,C_DATA,190);
      Glow((int)ccx-half,y,C_DATA,110,0.5f);
      Glow((int)ccx+half,y,C_DATA,110,0.5f); } }
  float sxr=fsin(rotX),cxr=fcos(rotX),syr=fsin(rotY),cyr=fcos(rotY);
  const float ax[3][3]={{1,0,0},{0,1,0},{0,0,1}};
  const char *an[3]={"X","Y","Z"};
  const uint16_t ac[3]={C_WARN,C_ACCENT,C_DATA};
  int gx0=292,gy0=60;
  for (int i=0;i<3;i++){
    float vx=ax[i][0],vy=ax[i][1],vz=ax[i][2];
    float px=cyr*vx+syr*vz;
    float py=sxr*syr*vx+cxr*vy-sxr*cyr*vz;
    int ex=gx0+(int)(px*15),ey=gy0-(int)(py*15);
    LineFB(gx0,gy0,ex,ey,ac[i],230);
    DrawText(ex-2,ey-3,an[i],ac[i],1); }
  RingFB(gx0,gy0,19,C_HAIR,A_GLOW);
  Button(252,214,60,16,"NEXT OBJ",C_ACCENT,false);
  DrawText(132,218,"DRAG TO ROTATE",C_SAND,1);
  DrawParticles();
  TopBar("INSPECT",C_ACCENT);
  EnterOverlay();
}

void ScreenWarp(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  bool hold=touchActive&&pressY>BACK_H;
  warpTarget=hold?4.4f:1.3f;
  if (hold) warpTilt=Approach(warpTilt,(touchX-160)*0.0025f,5.0f,dt);
  else      warpTilt=Approach(warpTilt,0.0f,1.8f,dt);
  warpSpeed=Approach(warpSpeed,warpTarget,3.2f,dt);
  uint16_t *save=frame;
  frame=accum;
  uint32_t *pp=(uint32_t *)accum;
  for (int i=0;i<FB_PIXELS/2;i++){
    uint32_t two=pp[i];
    if (!two) continue;
    uint32_t out=0;
    for (int k=0;k<2;k++){
      uint16_t c=(uint16_t)(two>>(k*16));
      if (c){ int r=(c>>11)&0x1F,g=(c>>5)&0x3F,b=c&0x1F;
        r=(r*11)>>4; g=(g*11)>>4; b=(b*12)>>4;
        if (r) r--; if (g) g--; if (b) b--;
        c=(uint16_t)((r<<11)|(g<<5)|b); }
      out|=(uint32_t)c<<(k*16); }
    pp[i]=out; }
  memset(depth,0,FB_BYTES);
  float cxo=160+warpTilt*210.0f, cyo=120+fsin(gTime*0.7f)*8.0f;
  for (int i=0;i<NUM_WARP;i++){
    float oz=warp[i].z;
    warp[i].z-=620.0f*warpSpeed*dt;
    if (warp[i].z<18.0f){
      warp[i].x=(float)random(-1600,1600);
      warp[i].y=(float)random(-1200,1200);
      warp[i].z=1600.0f; oz=warp[i].z; }
    float k1=260.0f/oz,k2=260.0f/warp[i].z;
    int x1=(int)(cxo+warp[i].x*k1),y1=(int)(cyo+warp[i].y*k1);
    int x2=(int)(cxo+warp[i].x*k2),y2=(int)(cyo+warp[i].y*k2);
    float br=clampf(k2*1.2f,0.12f,1.0f);
    uint16_t col=Fade(((i&7)==0)?C_HILITE:C_WARN,(uint8_t)(br*255));
    if (abs(x2-x1)<220&&abs(y2-y1)<220) LineFB(x1,y1,x2,y2,col,255);
    if (k2>2.2f) Glow(x2,y2,C_HILITE,80,0.4f); }
  for (int r=0;r<4;r++){
    float ph=fmodf(gTime*0.6f*warpSpeed+r*0.25f,1.0f);
    RingFB((int)cxo,(int)cyo,(int)(ph*ph*300),C_WARN,(uint8_t)(A_GLOW*(1.0f-ph))); }
  ApplyLight();
  RenderMesh(gMesh[5],0.25f+fsin(gTime)*0.06f,(float)M_PI+warpTilt*1.2f,
             fsin(gTime*0.8f)*0.15f,0.75f,70.0f,175.0f,3.0f,M_NEON,C_HILITE);
  frame=save;
  memcpy(frame,accum,FB_BYTES);
  DrawParticles();
  char sp[24]; snprintf(sp,sizeof(sp),"THROTTLE %d",(int)(warpSpeed*24));
  DrawText(6,224,sp,C_DATA,1);
  DrawText(120,224,hold?"ACCELERATING":"HOLD TO ACCELERATE",C_SAND,1);
  int bw=(int)(clampf((warpSpeed-1.0f)/3.4f,0,1)*88);
  Bracket(SCREEN_W-98,220,92,13,C_WARN,4);
  FillRectFB(SCREEN_W-96,222,bw,9,C_WARN);
  if (bw>4) Glow(SCREEN_W-96+bw,226,C_WARN,140,0.7f);
  TopBar("WARP",C_WARN);
  EnterOverlay();
}

// =====================================================================
//  MAZE 3D  --  first-person raycast maze (replaces the old runner)
//    A real raycaster: DDA through a grid, per-column wall heights,
//    distance shading and a live minimap. Find the exit to advance.
// =====================================================================
#define MZ 15
static uint8_t mzMap[MZ][MZ];
static float mzPX, mzPY, mzDir;
static int   mzLevel, mzExitX, mzExitY;
static float mzWin;
static Spring mzTurn, mzWalk;

static void MazeGen(void){
  for (int y=0;y<MZ;y++) for (int x=0;x<MZ;x++) mzMap[y][x]=1;
  // randomized DFS on odd cells
  int stx=1,sty=1;
  mzMap[sty][stx]=0;
  int sx[MZ*MZ],sy[MZ*MZ],sp=0;
  sx[sp]=stx; sy[sp]=sty; sp++;
  uint32_t seed=(uint32_t)(millis()*2654435761u)+mzLevel*7919u;
  while (sp>0){
    int cx=sx[sp-1],cy=sy[sp-1];
    int dirs[4]={0,1,2,3};
    for (int i=3;i>0;i--){ seed=seed*1103515245u+12345u;
      int j=(seed>>16)%(i+1); int t=dirs[i]; dirs[i]=dirs[j]; dirs[j]=t; }
    bool moved=false;
    for (int d=0;d<4;d++){
      int nx=cx,ny=cy;
      if (dirs[d]==0) ny-=2; else if (dirs[d]==1) ny+=2;
      else if (dirs[d]==2) nx-=2; else nx+=2;
      if (nx<1||ny<1||nx>=MZ-1||ny>=MZ-1) continue;
      if (mzMap[ny][nx]==0) continue;
      mzMap[ny][nx]=0;
      mzMap[(cy+ny)/2][(cx+nx)/2]=0;
      sx[sp]=nx; sy[sp]=ny; sp++;
      moved=true; break; }
    if (!moved) sp--; }
  mzPX=1.5f; mzPY=1.5f; mzDir=0;
  mzExitX=MZ-2; mzExitY=MZ-2;
  mzMap[mzExitY][mzExitX]=0;
}
void ResetMaze(void){ mzLevel=1; mzWin=0; MazeGen(); mzTurn.v=0; mzTurn.vel=0; }

void ScreenMaze(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  // controls: left third turns left, right third turns right, middle walks
  float turn=0, walk=0;
  if (touchActive&&touchY>BACK_H){
    if (touchX<100) turn=-1.7f;
    else if (touchX>220) turn=1.7f;
    else walk=1.9f;
    if (touchY>170) walk=-1.3f; }
  SpringTo(mzTurn,turn,200.0f,18.0f,dt);
  SpringTo(mzWalk,walk,180.0f,16.0f,dt);
  mzDir+=mzTurn.v*dt;
  float nx=mzPX+fcos(mzDir)*mzWalk.v*dt;
  float ny=mzPY+fsin(mzDir)*mzWalk.v*dt;
  if (mzMap[(int)mzPY][(int)nx]==0) mzPX=nx;
  if (mzMap[(int)ny][(int)mzPX]==0) mzPY=ny;
  if (mzWin<=0 && (int)mzPX==mzExitX && (int)mzPY==mzExitY){
    mzWin=1.8f; mzLevel++;
    if (mzLevel>hsMaze){ hsMaze=mzLevel; SaveScores(); }
    SpawnBurst(160,120,50,C_ACCENT,220.0f,PK_SPARK); }
  if (mzWin>0){ mzWin-=dt; if (mzWin<=0) MazeGen(); }

  // ---- raycast render ----
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t *)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  // ceiling + floor gradient bands (cheap, 8 bands not 240 rows)
  for (int i=0;i<8;i++){
    uint8_t a=(uint8_t)(70-i*8);
    BlendRectFB(0,20+i*7,SCREEN_W,7,Dim(C_HAIR,3,5),a);
    BlendRectFB(0,SCREEN_H-28-i*7,SCREEN_W,7,Dim(C_HAIR,2,5),a); }

  const int W=SCREEN_W, HH=SCREEN_H;
  const float FOV=1.05f;
  int step=(fpsValue<24.0f)?2:1;          // adaptive column stride
  for (int col=0;col<W;col+=step){
    float camx=2.0f*col/(float)W-1.0f;
    float ra=mzDir+camx*FOV*0.5f;
    float rdx=fcos(ra), rdy=fsin(ra);
    int mx=(int)mzPX,my=(int)mzPY;
    float ddx=(rdx==0)?1e30f:fabsf(1.0f/rdx);
    float ddy=(rdy==0)?1e30f:fabsf(1.0f/rdy);
    float sdx,sdy; int stx,sty;
    if (rdx<0){ stx=-1; sdx=(mzPX-mx)*ddx; } else { stx=1; sdx=(mx+1.0f-mzPX)*ddx; }
    if (rdy<0){ sty=-1; sdy=(mzPY-my)*ddy; } else { sty=1; sdy=(my+1.0f-mzPY)*ddy; }
    int side=0, guard=0;
    bool hitw=false;
    while (guard++<64){
      if (sdx<sdy){ sdx+=ddx; mx+=stx; side=0; }
      else { sdy+=ddy; my+=sty; side=1; }
      if (mx<0||my<0||mx>=MZ||my>=MZ){ hitw=true; break; }
      if (mzMap[my][mx]){ hitw=true; break; } }
    if (!hitw) continue;
    float pd=(side==0)?(sdx-ddx):(sdy-ddy);
    if (pd<0.05f) pd=0.05f;
    int lh=(int)(HH/pd);
    int y0=-lh/2+HH/2, y1=lh/2+HH/2;
    if (y0<20) y0=20;
    if (y1>HH-22) y1=HH-22;
    // colour by wall orientation + distance
    uint16_t base=(side==0)?Spec(1):Spec(4);
    uint8_t sh=(uint8_t)clampf(255.0f/(1.0f+pd*pd*0.22f),26,255);
    uint16_t c=Fade(base,sh);
    bool isExit=(mx==mzExitX&&my==mzExitY);
    if (isExit) c=Fade(C_HILITE,sh);
    FillRectFB(col,y0,step,y1-y0,c);
    if (isExit&&pd<6.0f) Glow(col,(y0+y1)/2,C_HILITE,(uint8_t)(40/(1+pd)),1.2f);
    // edge highlight
    if (y0>20) BlendRectFB(col,y0,step,1,C_HILITE,90);
  }
  // minimap
  int mmx=SCREEN_W-64,mmy=24,cell=3;
  BlendRectFB(mmx-2,mmy-2,MZ*cell+4,MZ*cell+4,C_PANEL,A_FILL);
  Bracket(mmx-2,mmy-2,MZ*cell+4,MZ*cell+4,C_ACCENT,5);
  for (int y=0;y<MZ;y++) for (int x=0;x<MZ;x++)
    if (mzMap[y][x]) FillRectFB(mmx+x*cell,mmy+y*cell,cell,cell,Dim(C_HAIR,4,5));
  FillRectFB(mmx+mzExitX*cell,mmy+mzExitY*cell,cell,cell,C_HILITE);
  { int px=mmx+(int)(mzPX*cell), py=mmy+(int)(mzPY*cell);
    CircleFB(px,py,2,C_ACCENT,255);
    LineFB(px,py,px+(int)(fcos(mzDir)*6),py+(int)(fsin(mzDir)*6),C_ACCENT,255);
    Glow(px,py,C_ACCENT,120,0.6f); }
  // control hints
  BlendRectFB(0,SCREEN_H-20,100,20,C_PANEL,A_GLOW);
  BlendRectFB(220,SCREEN_H-20,100,20,C_PANEL,A_GLOW);
  DrawText(30,SCREEN_H-14,"< TURN",C_SAND,1);
  DrawText(250,SCREEN_H-14,"TURN >",C_SAND,1);
  DrawTextC(160,SCREEN_H-14,"WALK",C_ACCENT,1);
  char b[24]; snprintf(b,sizeof(b),"LEVEL %d",mzLevel);
  GlowText(6,24,b,C_ACCENT,1,70);
  snprintf(b,sizeof(b),"BEST %d",hsMaze); DrawText(6,36,b,C_SAND,1);
  if (mzWin>0){
    Scrim(150);
    GlowTextC(160,100,"EXIT FOUND",C_HILITE,3,110);
    snprintf(b,sizeof(b),"LEVEL %d",mzLevel);
    DrawTextC(160,132,b,C_DATA,1); }
  DrawParticles();
  TopBar("MAZE 3D",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : 2048
// =====================================================================
#define G_N 4
static uint8_t gGrid[G_N][G_N];
static float   gSlX[G_N][G_N], gSlY[G_N][G_N];
static Spring  gPop[G_N][G_N];
static uint32_t g2Score=0;
static bool    g2Over=false, g2Won=false;
static float   g2Shake=0;
static bool    g2Armed=false;
static int     g2X0=0,g2Y0=0;

static uint16_t TileCol(uint8_t e){
  if (e==0) return C_PANEL;
  if (e<=2) return Dim(C_SAND,5-e,5);
  if (e>=11) return C_HILITE;
  return Spec(e-3);
}
static void Add2048(void){
  int fr[G_N*G_N],n=0;
  for (int y=0;y<G_N;y++) for (int x=0;x<G_N;x++) if (!gGrid[y][x]) fr[n++]=y*G_N+x;
  if (!n) return;
  int p=fr[(int)(Hash((uint32_t)(millis()*2654435761u))*n)%n];
  gGrid[p/G_N][p%G_N]=(Hash((uint32_t)millis()*7919u)>0.88f)?2:1;
  gPop[p/G_N][p%G_N].v=0; gPop[p/G_N][p%G_N].vel=0;
}
void Reset2048(void){
  memset(gGrid,0,sizeof(gGrid));
  memset(gSlX,0,sizeof(gSlX)); memset(gSlY,0,sizeof(gSlY));
  for (int y=0;y<G_N;y++) for (int x=0;x<G_N;x++){ gPop[y][x].v=1; gPop[y][x].vel=0; }
  g2Score=0; g2Over=false; g2Won=false; g2Shake=0;
  Add2048(); Add2048();
}
static bool Can2048(void){
  for (int y=0;y<G_N;y++) for (int x=0;x<G_N;x++){
    if (!gGrid[y][x]) return true;
    if (x+1<G_N&&gGrid[y][x]==gGrid[y][x+1]) return true;
    if (y+1<G_N&&gGrid[y][x]==gGrid[y+1][x]) return true; }
  return false;
}
static bool Move2048(int dir){
  bool moved=false;
  for (int i=0;i<G_N;i++){
    uint8_t line[G_N]; int sx[G_N],sy[G_N];
    for (int j=0;j<G_N;j++){
      int x,y;
      switch (dir){ case 0: x=j; y=i; break; case 1: x=G_N-1-j; y=i; break;
                    case 2: x=i; y=j; break; default: x=i; y=G_N-1-j; break; }
      line[j]=gGrid[y][x]; sx[j]=x; sy[j]=y; }
    uint8_t out[G_N]={0,0,0,0};
    int from[G_N]={-1,-1,-1,-1}, mf[G_N]={-1,-1,-1,-1}, w=0;
    for (int j=0;j<G_N;j++){
      if (!line[j]) continue;
      if (w>0&&out[w-1]==line[j]&&mf[w-1]<0){
        out[w-1]++; mf[w-1]=j; g2Score+=(1u<<out[w-1]);
        if (out[w-1]>=11) g2Won=true;
        moved=true;
      } else { out[w]=line[j]; from[w]=j; w++; } }
    for (int j=0;j<G_N;j++){
      int x=sx[j],y=sy[j]; uint8_t nv=out[j];
      if (gGrid[y][x]!=nv) moved=true;
      gGrid[y][x]=nv;
      if (nv){
        int src=(from[j]>=0)?from[j]:j;
        float d=(float)(src-j);
        if (dir==0){ gSlX[y][x]=d; gSlY[y][x]=0; }
        else if (dir==1){ gSlX[y][x]=-d; gSlY[y][x]=0; }
        else if (dir==2){ gSlY[y][x]=d; gSlX[y][x]=0; }
        else { gSlY[y][x]=-d; gSlX[y][x]=0; }
        if (mf[j]>=0){
          gPop[y][x].v=1.42f; gPop[y][x].vel=0;
          SpawnBurst(22+x*46+21,44+y*44+20,10,TileCol(nv),120.0f,PK_SPARK);
          g2Shake=3.0f; }
      } else { gSlX[y][x]=0; gSlY[y][x]=0; } } }
  return moved;
}
void Screen2048(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (touchDown&&touchY>BACK_H){ g2Armed=true; g2X0=touchX; g2Y0=touchY; }
  if (touchDown&&touchX>=236&&touchY>=44&&touchY<76) Reset2048();
  if (touchUp&&g2Armed){
    g2Armed=false;
    int dx=touchX-g2X0, dy=touchY-g2Y0;
    if (abs(dx)>22||abs(dy)>22){
      int dir=(abs(dx)>abs(dy))?(dx>0?1:0):(dy>0?3:2);
      if (!g2Over&&Move2048(dir)){
        Add2048();
        if (!Can2048()){ g2Over=true;
          if (g2Score>hs2048){ hs2048=(uint16_t)min((uint32_t)65535,g2Score); SaveScores(); }
          SpawnBurst(120,130,32,C_WARN,170.0f,PK_EMBER); } } } }
  g2Shake=Approach(g2Shake,0,9.0f,dt);
  for (int y=0;y<G_N;y++) for (int x=0;x<G_N;x++){
    gSlX[y][x]=Approach(gSlX[y][x],0,17.0f,dt);
    gSlY[y][x]=Approach(gSlY[y][x],0,17.0f,dt);
    SpringTo(gPop[y][x],1.0f,300.0f,19.0f,dt); }
  Backdrop();
  int sh=(int)(fsin(gTime*60.0f)*g2Shake);
  const int BX=10,BY=40,CELL=46,GAP=4;
  Panel(BX+sh,BY,200,194,0,C_ACCENT,0);
  for (int y=0;y<G_N;y++) for (int x=0;x<G_N;x++)
    BlendRectFB(BX+6+sh+x*CELL,BY+6+y*CELL,CELL-GAP,CELL-GAP,Dim(C_HAIR,2,5),A_FILL);
  for (int y=0;y<G_N;y++) for (int x=0;x<G_N;x++){
    uint8_t e=gGrid[y][x];
    if (!e) continue;
    float sc=clampf(gPop[y][x].v,0.05f,1.6f);
    int w=(int)((CELL-GAP)*sc),h=w;
    int cx=BX+6+sh+(int)((x+gSlX[y][x])*CELL)+(CELL-GAP)/2;
    int cy=BY+6+(int)((y+gSlY[y][x])*CELL)+(CELL-GAP)/2;
    uint16_t tc=TileCol(e);
    BlendRectFB(cx-w/2,cy-h/2,w,h,tc,A_FILL);
    Bracket(cx-w/2,cy-h/2,w,h,C_TEXT,4);
    if (e>=6) Glow(cx,cy,tc,(uint8_t)(40+30*Pulse(gTime,2.4f)),1.4f);
    char lab[8]; snprintf(lab,sizeof(lab),"%u",1u<<e);
    uint8_t ts=(e>=10)?1:2;
    DrawTextC(cx,cy-(ts*7)/2,lab,(e<=2)?C_BG:C_TEXT,ts); }
  Panel(216,40,100,96,"SCORE",C_ACCENT,0);
  char b[16]; snprintf(b,sizeof(b),"%u",(unsigned)g2Score);
  GlowText(224,62,b,C_DATA,2,70);
  DrawText(224,86,"BEST",C_SAND,1);
  snprintf(b,sizeof(b),"%u",(unsigned)hs2048);
  DrawText(224,98,b,C_DATA,1);
  Button(224,112,84,16,"RESET",C_WARN,false);
  Panel(216,142,100,92,"HOW",C_ACCENT,0);
  DrawText(222,162,"SWIPE TO",C_SAND,1);
  DrawText(222,174,"SLIDE ALL",C_SAND,1);
  DrawText(222,192,"MERGE TO",C_SAND,1);
  DrawText(222,204,"REACH 2048",C_ACCENT,1);
  if (g2Over){
    Scrim(170);
    GlowTextC(160,104,"GAME OVER",C_WARN,3,110);
    DrawTextC(160,134,"TAP RESET",C_SAND,1);
  } else if (g2Won) GlowTextC(112,20,"2048 REACHED",C_HILITE,1,90);
  DrawParticles();
  TopBar("2048",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : BREAKOUT
// =====================================================================
#define BR_COLS 10
#define BR_ROWS 5
static uint8_t brB[BR_ROWS][BR_COLS];
static Spring  brPad;
static float   brBX,brBY,brVX,brVY,brFlash=0;
static int     brFX=160,brFY=120;
static int     brLives,brScore;
static bool    brLaunch,brOver;
void ResetBreak(void){
  for (int r=0;r<BR_ROWS;r++) for (int c=0;c<BR_COLS;c++) brB[r][c]=1;
  brPad.v=160; brPad.vel=0;
  brBX=160; brBY=150; brVX=0; brVY=0;
  brLives=3; brScore=0; brLaunch=false; brOver=false;
}
void ScreenBreak(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int PW=46,PY=214,BALL_R=3;
  if (touchDown&&brOver) ResetBreak();
  if (touchActive&&touchY>BACK_H){
    SpringTo(brPad,clampf((float)touchX,PW/2.0f,SCREEN_W-PW/2.0f),260.0f,20.0f,dt);
    if (!brLaunch&&!brOver){ brLaunch=true; brVX=66.0f; brVY=-132.0f; }
  } else SpringTo(brPad,brPad.v,260.0f,20.0f,dt);
  brFlash=Approach(brFlash,0,11.0f,dt);
  if (!brLaunch&&!brOver){ brBX=brPad.v; brBY=PY-8; }
  if (brLaunch&&!brOver){
    int sub=3; float sdt=dt/sub;
    for (int k=0;k<sub;k++){
      brBX+=brVX*sdt; brBY+=brVY*sdt;
      if (brBX<BALL_R+2){ brBX=BALL_R+2; brVX=-brVX; }
      if (brBX>SCREEN_W-BALL_R-2){ brBX=SCREEN_W-BALL_R-2; brVX=-brVX; }
      if (brBY<26+BALL_R){ brBY=26+BALL_R; brVY=-brVY; }
      if (brVY>0&&brBY>PY-BALL_R&&brBY<PY+6){
        float rel=(brBX-brPad.v)/(PW*0.5f);
        if (rel>-1.35f&&rel<1.35f){
          brBY=PY-BALL_R; brVY=-fabsf(brVY);
          brVX=clampf(brVX+rel*78.0f,-190.0f,190.0f);
          SpawnBurst(brBX,PY,6,C_ACCENT,80.0f,PK_SPARK); } }
      int bw=(SCREEN_W-20)/BR_COLS,bh=12;
      bool hitB=false;
      for (int r=0;r<BR_ROWS&&!hitB;r++)
        for (int c=0;c<BR_COLS;c++){
          if (!brB[r][c]) continue;
          int bx=10+c*bw,by=34+r*(bh+3);
          if (brBX>bx-BALL_R&&brBX<bx+bw+BALL_R&&brBY>by-BALL_R&&brBY<by+bh+BALL_R){
            brB[r][c]=0; brScore+=(BR_ROWS-r)*10; brVY=-brVY;
            brFlash=1.0f; brFX=bx+bw/2; brFY=by+bh/2;
            SpawnBurst(brFX,brFY,10,Spec(r),140.0f,PK_SPARK);
            hitB=true; break; } }
      if (hitB) break;
      if (brBY>SCREEN_H+10){
        brLives--; brLaunch=false;
        SpawnBurst(brBX,SCREEN_H-6,20,C_WARN,140.0f,PK_EMBER);
        if (brLives<=0){ brOver=true;
          if (brScore>hsBreak){ hsBreak=brScore; SaveScores(); } }
        break; } }
    bool any=false;
    for (int r=0;r<BR_ROWS&&!any;r++) for (int c=0;c<BR_COLS;c++) if (brB[r][c]){ any=true; break; }
    if (!any){
      for (int r=0;r<BR_ROWS;r++) for (int c=0;c<BR_COLS;c++) brB[r][c]=1;
      brVX*=1.12f; brVY*=1.12f; brLaunch=false;
      SpawnBurst(160,120,40,C_HILITE,200.0f,PK_SPARK); } }
  Backdrop();
  if (brFlash>0.02f){
    int rr=(int)(10+(1.0f-brFlash)*26);
    RingFB(brFX,brFY,rr,C_HILITE,(uint8_t)(A_FILL*brFlash));
    Glow(brFX,brFY,C_HILITE,(uint8_t)(150*brFlash),1.8f); }
  int bw=(SCREEN_W-20)/BR_COLS,bh=12;
  for (int r=0;r<BR_ROWS;r++) for (int c=0;c<BR_COLS;c++){
    if (!brB[r][c]) continue;
    int bx=10+c*bw,by=34+r*(bh+3);
    float d=Stagger(enterAnim,r*2+(c&1),0.03f,0.4f);
    int h=(int)((bh-2)*d);
    uint16_t bc=Spec(r);
    BlendRectFB(bx,by,bw-2,h,bc,A_FILL);
    HLineFB(bx,by,bw-2,Fade(C_TEXT,90));
    Bracket(bx,by,bw-2,(h>2?h:2),Dim(bc,4,5),3); }
  int px=(int)brPad.v;
  BlendRectFB(px-PW/2,PY,PW,5,C_ACCENT,255);
  Glow(px,PY+2,C_ACCENT,110,1.3f);
  HLineFB(px-PW/2,PY,PW,C_HILITE);
  CircleFB((int)brBX,(int)brBY,BALL_R,C_TEXT,255);
  Glow((int)brBX,(int)brBY,C_HILITE,150,0.9f);
  char b[32];
  snprintf(b,sizeof(b),"SCORE %d",brScore); DrawText(8,226,b,C_DATA,1);
  snprintf(b,sizeof(b),"BEST %d",hsBreak);  DrawText(120,226,b,C_SAND,1);
  for (int i=0;i<brLives;i++) CircleFB(232+i*12,229,3,C_WARN,255);
  if (!brLaunch&&!brOver) DrawTextC(160,190,"TOUCH TO LAUNCH",C_SAND,1);
  if (brOver){
    Scrim(170);
    GlowTextC(160,104,"GAME OVER",C_WARN,3,110);
    snprintf(b,sizeof(b),"SCORE %d",brScore);
    DrawTextC(160,134,b,C_DATA,1);
    DrawTextC(160,150,"TAP TO RESTART",C_SAND,1); }
  DrawParticles();
  TopBar("BREAKOUT",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : FLAPPY
// =====================================================================
#define FL_P 4
static float flPX[FL_P],flGY[FL_P];
static bool  flSc[FL_P];
static float flY,flVY,flRot,flShake=0;
static int   flScore;
static bool  flOver,flStart;
void ResetFlappy(void){
  for (int i=0;i<FL_P;i++){ flPX[i]=330.0f+i*96.0f;
    flGY[i]=90.0f+Hash(i*7919u)*80.0f; flSc[i]=false; }
  flY=120; flVY=0; flRot=0; flScore=0; flOver=false; flStart=false; flShake=0;
}
void ScreenFlappy(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const float GRAV=620.0f,FLAP=-215.0f,GAP=64.0f,FLOOR=214.0f,CEIL=24.0f;
  const int BX=84,PW=30;
  if (touchDown&&touchY>BACK_H){
    if (flOver) ResetFlappy();
    else { flStart=true; flVY=FLAP; SpawnBurst(BX-8,flY+6,6,C_ACCENT,80.0f,PK_SPARK); } }
  flShake=Approach(flShake,0,9.0f,dt);
  if (flStart&&!flOver){
    flVY+=GRAV*dt; flY+=flVY*dt;
    flRot=Approach(flRot,clampf(flVY/420.0f,-0.55f,1.15f),9.0f,dt);
    float sp=92.0f+flScore*1.9f;
    for (int i=0;i<FL_P;i++){
      flPX[i]-=sp*dt;
      if (flPX[i]<-PW){
        float mx=0; for (int k=0;k<FL_P;k++) mx=fmaxf(mx,flPX[k]);
        flPX[i]=mx+96.0f;
        flGY[i]=78.0f+Hash((uint32_t)(gTime*977.0f)+i*104729u)*96.0f;
        flSc[i]=false; }
      if (!flSc[i]&&flPX[i]+PW<BX){ flSc[i]=true; flScore++;
        SpawnBurst(BX,flY,8,Spec(flScore%6),100.0f,PK_SPARK); }
      if (BX+7>flPX[i]&&BX-7<flPX[i]+PW){
        if (flY-6<flGY[i]-GAP*0.5f||flY+6>flGY[i]+GAP*0.5f){
          flOver=true; flShake=6.0f;
          if (flScore>hsFlappy){ hsFlappy=flScore; SaveScores(); }
          SpawnBurst(BX,flY,36,C_WARN,200.0f,PK_EMBER); } } }
    if (flY>FLOOR-6){ flY=FLOOR-6; flOver=true; flShake=6.0f;
      if (flScore>hsFlappy){ hsFlappy=flScore; SaveScores(); }
      SpawnBurst(BX,flY,32,C_WARN,180.0f,PK_EMBER); }
    if (flY<CEIL+6){ flY=CEIL+6; flVY=0; }
  } else if (!flStart){
    flY=120+fsin(gTime*2.4f)*7.0f; flRot=fsin(gTime*2.4f)*0.18f; }
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t *)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  memset(depth,0,FB_BYTES);
  int sy=(int)(fsin(gTime*52.0f)*flShake);
  for (int l=0;l<2;l++){
    float sp=(l?0.30f:0.13f);
    uint16_t hc=Dim(Spec(l?4:5),l?2:1,6);
    int baseY=l?196:186;
    for (int i=0;i<12;i++){
      float bx=fmodf(i*46.0f-gTime*sp*46.0f+400.0f,400.0f)-40.0f;
      int hgt=16+(int)(Hash(i*313u+l*77u)*(l?30:20));
      BlendRectFB((int)bx,baseY-hgt,30,hgt,hc,A_FILL); } }
  BlendRectFB(0,196,SCREEN_W,SCREEN_H-196,Dim(C_HAIR,3,5),A_FILL);
  HLineFB(0,196,SCREEN_W,Spec(3));
  for (int i=0;i<22;i++){
    int gx2=(int)fmodf(i*16.0f-gTime*92.0f+640.0f,352.0f)-16;
    LineFB(gx2,198,gx2+10,208,Dim(C_HAIR,4,5),A_GLOW); }
  for (int i=0;i<FL_P;i++){
    int px=(int)flPX[i];
    if (px>SCREEN_W||px+PW<0) continue;
    int gy=(int)flGY[i]+sy;
    uint16_t pc=Spec(i+1);
    int topH=gy-(int)(GAP*0.5f)-24;
    int botY=gy+(int)(GAP*0.5f);
    if (topH>0){ BlendRectFB(px,24,PW,topH,pc,A_FILL);
      BlendRectFB(px-3,24+topH-9,PW+6,9,Dim(pc,4,5),A_FILL);
      Bracket(px,24,PW,topH,Dim(pc,5,5),5); }
    int botH=196-botY;
    if (botH>0){ BlendRectFB(px,botY,PW,botH,pc,A_FILL);
      BlendRectFB(px-3,botY,PW+6,9,Dim(pc,4,5),A_FILL);
      Bracket(px,botY,PW,botH,Dim(pc,5,5),5); }
    Glow(px+PW/2,gy-(int)(GAP*0.5f),pc,70,1.0f);
    Glow(px+PW/2,gy+(int)(GAP*0.5f),pc,70,1.0f); }
  { int by=(int)flY+sy;
    float cs=fcos(flRot),sn=fsin(flRot);
    for (int oy=-5;oy<=5;oy++) for (int ox=-7;ox<=7;ox++){
      if (ox*ox*2+oy*oy*5>58) continue;
      PxBlend(BX+(int)(ox*cs-oy*sn),by+(int)(ox*sn+oy*cs),(oy<-1)?C_HILITE:C_ACCENT,255); }
    float wf=flStart?fsin(gTime*22.0f):fsin(gTime*5.0f);
    int wy=by+(int)(wf*3.0f);
    for (int ox=-5;ox<=1;ox++) PxBlend(BX+ox-1,wy+2,Dim(C_ACCENT,4,5),255);
    PxBlend(BX+4,by-2,C_BG,255); PxBlend(BX+5,by-2,C_BG,255);
    LineFB(BX+7,by,BX+11,by+1,Spec(1),255);
    Glow(BX,by,C_ACCENT,(uint8_t)(60+40*Pulse(gTime,6.0f)),1.4f); }
  char b[24]; snprintf(b,sizeof(b),"%d",flScore);
  GlowTextC(160,34,b,C_TEXT,4,90);
  snprintf(b,sizeof(b),"BEST %d",hsFlappy);
  DrawText(8,226,b,C_SAND,1);
  if (!flStart) DrawTextC(160,150,"TAP TO FLAP",C_SAND,1);
  if (flOver){
    Scrim(170);
    GlowTextC(160,96,"GAME OVER",C_WARN,3,110);
    snprintf(b,sizeof(b),"SCORE %d   BEST %d",flScore,hsFlappy);
    DrawTextC(160,126,b,C_DATA,1);
    DrawTextC(160,144,"TAP TO RETRY",C_SAND,1); }
  DrawParticles();
  TopBar("FLAPPY",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : SNAKE  (swipe to steer)
// =====================================================================
#define SN_W 20
#define SN_H 13
#define SN_MAX (SN_W*SN_H)
static int8_t snX[SN_MAX],snY[SN_MAX];
static int    snLen,snDir,snNextDir,snFX,snFY,snScore;
static float  snTick;
static bool   snOver;
static bool   snArmed=false;
static int    snX0=0,snY0=0;
void ResetSnake(void){
  snLen=4; snDir=1; snNextDir=1;
  for (int i=0;i<snLen;i++){ snX[i]=6-i; snY[i]=6; }
  snFX=12; snFY=6; snScore=0; snTick=0; snOver=false;
}
static void SnakeFood(void){
  for (int t=0;t<200;t++){
    int fx=(int)(Hash((uint32_t)(millis()*7919u)+t*131u)*SN_W);
    int fy=(int)(Hash((uint32_t)(millis()*104729u)+t*313u)*SN_H);
    bool clash=false;
    for (int i=0;i<snLen;i++) if (snX[i]==fx&&snY[i]==fy){ clash=true; break; }
    if (!clash){ snFX=fx; snFY=fy; return; } }
}
void ScreenSnake(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (touchDown&&touchY>BACK_H){ snArmed=true; snX0=touchX; snY0=touchY;
    if (snOver) ResetSnake(); }
  if (touchUp&&snArmed){
    snArmed=false;
    int dx=touchX-snX0,dy=touchY-snY0;
    if (abs(dx)>18||abs(dy)>18){
      int nd=(abs(dx)>abs(dy))?(dx>0?1:3):(dy>0?2:0);
      if ((nd+2)%4 != snDir) snNextDir=nd; } }
  if (!snOver){
    snTick+=dt;
    float rate=0.16f-clampf(snScore*0.0015f,0,0.08f);
    if (snTick>=rate){
      snTick=0; snDir=snNextDir;
      int nx=snX[0],ny=snY[0];
      if (snDir==0) ny--; else if (snDir==1) nx++;
      else if (snDir==2) ny++; else nx--;
      if (nx<0||ny<0||nx>=SN_W||ny>=SN_H){ snOver=true;
        if (snScore>hsSnake){ hsSnake=snScore; SaveScores(); }
        SpawnBurst(20+snX[0]*15,34+snY[0]*15,26,C_WARN,170.0f,PK_EMBER);
      } else {
        for (int i=0;i<snLen;i++) if (snX[i]==nx&&snY[i]==ny){ snOver=true;
          if (snScore>hsSnake){ hsSnake=snScore; SaveScores(); }
          SpawnBurst(20+nx*15,34+ny*15,26,C_WARN,170.0f,PK_EMBER); break; }
        if (!snOver){
          for (int i=snLen;i>0;i--){ snX[i]=snX[i-1]; snY[i]=snY[i-1]; }
          snX[0]=nx; snY[0]=ny;
          if (nx==snFX&&ny==snFY){
            if (snLen<SN_MAX-1) snLen++;
            snScore+=10;
            SpawnBurst(20+nx*15+7,34+ny*15+7,12,Spec(1),120.0f,PK_SPARK);
            SnakeFood(); } } } } }
  Backdrop();
  const int OX=20,OY=34,CS=15;
  Panel(OX-6,OY-6,SN_W*CS+12,SN_H*CS+12,0,C_ACCENT,0);
  for (int x=0;x<=SN_W;x+=4) VLineFB(OX+x*CS,OY,SN_H*CS,Dim(C_HAIR,3,5));
  for (int y=0;y<=SN_H;y+=4) HLineFB(OX,OY+y*CS,SN_W*CS,Dim(C_HAIR,3,5));
  { float pp=Pulse(gTime,4.0f);
    int fx=OX+snFX*CS,fy=OY+snFY*CS;
    CircleFB(fx+CS/2,fy+CS/2,4+(int)(pp*2),Spec(1),255);
    Glow(fx+CS/2,fy+CS/2,Spec(1),(uint8_t)(110+70*pp),1.1f); }
  for (int i=snLen-1;i>=0;i--){
    int x=OX+snX[i]*CS,y=OY+snY[i]*CS;
    uint16_t c=(i==0)?C_HILITE:Fade(C_ACCENT,(uint8_t)(255-i*(140/(snLen+1))));
    BlendRectFB(x+1,y+1,CS-2,CS-2,c,A_FILL);
    if (i==0){ Glow(x+CS/2,y+CS/2,C_HILITE,120,0.9f);
      Bracket(x+1,y+1,CS-2,CS-2,C_TEXT,4); } }
  char b[28]; snprintf(b,sizeof(b),"SCORE %d",snScore);
  GlowText(8,226,b,C_DATA,1,60);
  snprintf(b,sizeof(b),"BEST %d",hsSnake); DrawText(120,226,b,C_SAND,1);
  snprintf(b,sizeof(b),"LEN %d",snLen);    DrawText(230,226,b,C_SAND,1);
  if (snOver){
    Scrim(170);
    GlowTextC(160,100,"GAME OVER",C_WARN,3,110);
    snprintf(b,sizeof(b),"SCORE %d",snScore);
    DrawTextC(160,132,b,C_DATA,1);
    DrawTextC(160,150,"SWIPE TO RESTART",C_SAND,1); }
  DrawParticles();
  TopBar("SNAKE",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : PONG  (vs CPU)
// =====================================================================
static Spring poP;
static float poAI,poBX,poBY,poVX,poVY;
static int   poS1,poS2;
static bool  poServe;
void ResetPong(void){
  poP.v=120; poP.vel=0; poAI=120;
  poBX=160; poBY=120; poVX=0; poVY=0;
  poS1=0; poS2=0; poServe=true;
}
void ScreenPong(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int PH=42,PX1=16,PX2=SCREEN_W-20,BALL_R=4;
  if (touchActive&&touchY>BACK_H){
    SpringTo(poP,clampf((float)touchY,26+PH/2.0f,SCREEN_H-14-PH/2.0f),240.0f,19.0f,dt);
    if (poServe){ poServe=false;
      poVX=(Hash((uint32_t)millis())>0.5f)?150.0f:-150.0f;
      poVY=(Hash((uint32_t)millis()*7u)-0.5f)*140.0f; }
  } else SpringTo(poP,poP.v,240.0f,19.0f,dt);
  // CPU tracks with a deliberate lag so it is beatable
  poAI=Approach(poAI,poBY,3.4f,dt);
  poAI=clampf(poAI,26+PH/2.0f,SCREEN_H-14-PH/2.0f);
  if (!poServe){
    poBX+=poVX*dt; poBY+=poVY*dt;
    if (poBY<26+BALL_R){ poBY=26+BALL_R; poVY=-poVY; SpawnBurst(poBX,poBY,5,C_ACCENT,70.0f,PK_SPARK); }
    if (poBY>SCREEN_H-14-BALL_R){ poBY=SCREEN_H-14-BALL_R; poVY=-poVY;
      SpawnBurst(poBX,poBY,5,C_ACCENT,70.0f,PK_SPARK); }
    if (poVX<0&&poBX<PX1+6&&poBX>PX1-6&&fabsf(poBY-poP.v)<PH/2.0f+4){
      poBX=PX1+6; poVX=-poVX*1.04f;
      poVY+=(poBY-poP.v)*3.2f;
      SpawnBurst(poBX,poBY,10,C_HILITE,110.0f,PK_SPARK); }
    if (poVX>0&&poBX>PX2-6&&poBX<PX2+6&&fabsf(poBY-poAI)<PH/2.0f+4){
      poBX=PX2-6; poVX=-poVX*1.04f;
      poVY+=(poBY-poAI)*3.0f;
      SpawnBurst(poBX,poBY,10,Spec(4),110.0f,PK_SPARK); }
    poVY=clampf(poVY,-230.0f,230.0f);
    if (poBX<0){ poS2++; poServe=true; poBX=160; poBY=120; poVX=poVY=0;
      SpawnBurst(10,poBY,24,C_WARN,160.0f,PK_EMBER); }
    if (poBX>SCREEN_W){ poS1++; poServe=true; poBX=160; poBY=120; poVX=poVY=0;
      if (poS1>hsPong){ hsPong=poS1; SaveScores(); }
      SpawnBurst(SCREEN_W-10,poBY,24,C_ACCENT,160.0f,PK_SPARK); } }
  Backdrop();
  for (int y=26;y<SCREEN_H-14;y+=12)
    BlendRectFB(SCREEN_W/2-1,y,2,7,Dim(C_HAIR,4,5),A_FILL);
  BlendRectFB(PX1-3,(int)poP.v-PH/2,6,PH,C_ACCENT,255);
  Glow(PX1,(int)poP.v,C_ACCENT,110,1.6f);
  BlendRectFB(PX2-3,(int)poAI-PH/2,6,PH,Spec(4),255);
  Glow(PX2,(int)poAI,Spec(4),110,1.6f);
  CircleFB((int)poBX,(int)poBY,BALL_R,C_TEXT,255);
  Glow((int)poBX,(int)poBY,C_HILITE,150,1.0f);
  char b[16];
  snprintf(b,sizeof(b),"%d",poS1); GlowText(120,30,b,C_ACCENT,3,80);
  snprintf(b,sizeof(b),"%d",poS2); GlowText(180,30,b,Spec(4),3,80);
  snprintf(b,sizeof(b),"BEST %d",hsPong); DrawText(8,226,b,C_SAND,1);
  DrawTextC(160,226,poServe?"TOUCH TO SERVE - DRAG TO MOVE":"DRAG TO MOVE",C_SAND,1);
  DrawParticles();
  TopBar("PONG",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : TETRIS
// =====================================================================
#define TT_W 10
#define TT_H 18
static uint8_t ttB[TT_H][TT_W];
static int  ttPiece,ttRot,ttPX,ttPY,ttScore,ttLines;
static float ttTick;
static bool ttOver;
static int  ttArmX=0,ttArmY=0;
static bool ttArmed=false;
static const uint16_t TT_SHAPES[7][4] = {
  {0x0F00,0x2222,0x00F0,0x4444}, // I
  {0x8E00,0x6440,0x0E20,0x44C0}, // J
  {0x2E00,0x4460,0x0E80,0xC440}, // L
  {0x6600,0x6600,0x6600,0x6600}, // O
  {0x6C00,0x4620,0x06C0,0x8C40}, // S
  {0x4E00,0x4640,0x0E40,0x4C40}, // T
  {0xC600,0x2640,0x0C60,0x4C80}  // Z
};
static bool TtHit(int p,int r,int px,int py){
  uint16_t s=TT_SHAPES[p][r&3];
  for (int i=0;i<16;i++){
    if (!(s&(0x8000>>i))) continue;
    int x=px+(i%4), y=py+(i/4);
    if (x<0||x>=TT_W||y>=TT_H) return true;
    if (y>=0&&ttB[y][x]) return true; }
  return false;
}
static void TtLock(void){
  uint16_t s=TT_SHAPES[ttPiece][ttRot&3];
  for (int i=0;i<16;i++){
    if (!(s&(0x8000>>i))) continue;
    int x=ttPX+(i%4), y=ttPY+(i/4);
    if (y>=0&&y<TT_H&&x>=0&&x<TT_W) ttB[y][x]=ttPiece+1; }
  int cleared=0;
  for (int y=TT_H-1;y>=0;y--){
    bool full=true;
    for (int x=0;x<TT_W;x++) if (!ttB[y][x]){ full=false; break; }
    if (full){
      cleared++;
      SpawnBurst(160,40+y*10,20,C_HILITE,150.0f,PK_SPARK);
      for (int yy=y;yy>0;yy--) memcpy(ttB[yy],ttB[yy-1],TT_W);
      memset(ttB[0],0,TT_W);
      y++; } }
  if (cleared){ ttLines+=cleared;
    ttScore+=(cleared==1)?100:(cleared==2)?300:(cleared==3)?500:800;
    if (ttScore>hsTetris){ hsTetris=(uint16_t)min(65535,ttScore); SaveScores(); } }
  ttPiece=(int)(Hash((uint32_t)(millis()*7919u))*7)%7;
  ttRot=0; ttPX=3; ttPY=-1;
  if (TtHit(ttPiece,ttRot,ttPX,ttPY)) ttOver=true;
}
void ResetTetris(void){
  memset(ttB,0,sizeof(ttB));
  ttPiece=(int)(Hash((uint32_t)millis())*7)%7;
  ttRot=0; ttPX=3; ttPY=-1; ttScore=0; ttLines=0; ttTick=0; ttOver=false;
}
void ScreenTetris(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (touchDown&&touchY>BACK_H){ ttArmed=true; ttArmX=touchX; ttArmY=touchY;
    if (ttOver) ResetTetris(); }
  if (touchUp&&ttArmed&&!ttOver){
    ttArmed=false;
    int dx=touchX-ttArmX, dy=touchY-ttArmY;
    if (abs(dx)<16&&abs(dy)<16){                    // tap = rotate
      if (!TtHit(ttPiece,ttRot+1,ttPX,ttPY)){ ttRot=(ttRot+1)&3;
        SpawnBurst(touchX,touchY,6,C_ACCENT,70.0f,PK_SPARK); }
    } else if (abs(dx)>abs(dy)){                    // swipe = move
      int d=(dx>0)?1:-1;
      if (!TtHit(ttPiece,ttRot,ttPX+d,ttPY)) ttPX+=d;
    } else if (dy>0){                               // swipe down = drop
      while (!TtHit(ttPiece,ttRot,ttPX,ttPY+1)) ttPY++;
      SpawnBurst(160,40+ttPY*10,14,C_HILITE,130.0f,PK_SPARK);
      TtLock(); } }
  if (!ttOver){
    ttTick+=dt;
    float rate=clampf(0.55f-ttLines*0.012f,0.10f,0.55f);
    if (touchActive&&touchY>SCREEN_H-60) rate=0.06f;
    if (ttTick>=rate){ ttTick=0;
      if (!TtHit(ttPiece,ttRot,ttPX,ttPY+1)) ttPY++;
      else TtLock(); } }
  Backdrop();
  const int OX=104,OY=30,CS=10;
  Panel(OX-6,OY-6,TT_W*CS+12,TT_H*CS+12,0,C_ACCENT,0);
  for (int y=0;y<TT_H;y++) for (int x=0;x<TT_W;x++){
    int px=OX+x*CS,py=OY+y*CS;
    if (ttB[y][x]){
      uint16_t c=Spec(ttB[y][x]-1);
      BlendRectFB(px+1,py+1,CS-2,CS-2,c,A_FILL);
      HLineFB(px+1,py+1,CS-2,Fade(C_TEXT,70));
    } else BlendRectFB(px+1,py+1,CS-2,CS-2,Dim(C_HAIR,2,5),90); }
  if (!ttOver){
    uint16_t s=TT_SHAPES[ttPiece][ttRot&3];
    // ghost
    int gy=ttPY;
    while (!TtHit(ttPiece,ttRot,ttPX,gy+1)) gy++;
    for (int i=0;i<16;i++){
      if (!(s&(0x8000>>i))) continue;
      int x=ttPX+(i%4),y=gy+(i/4);
      if (y<0) continue;
      Bracket(OX+x*CS+1,OY+y*CS+1,CS-2,CS-2,Dim(Spec(ttPiece),3,5),3); }
    for (int i=0;i<16;i++){
      if (!(s&(0x8000>>i))) continue;
      int x=ttPX+(i%4),y=ttPY+(i/4);
      if (y<0) continue;
      uint16_t c=Spec(ttPiece);
      BlendRectFB(OX+x*CS+1,OY+y*CS+1,CS-2,CS-2,c,A_FILL);
      HLineFB(OX+x*CS+1,OY+y*CS+1,CS-2,Fade(C_TEXT,110));
      Glow(OX+x*CS+CS/2,OY+y*CS+CS/2,c,42,0.7f); } }
  Panel(4,30,90,80,"STATS",C_ACCENT,0);
  char b[20];
  snprintf(b,sizeof(b),"%d",ttScore); GlowText(10,52,b,C_DATA,2,70);
  DrawText(10,74,"LINES",C_SAND,1);
  snprintf(b,sizeof(b),"%d",ttLines); DrawText(56,74,b,C_DATA,1);
  DrawText(10,88,"BEST",C_SAND,1);
  snprintf(b,sizeof(b),"%d",hsTetris); DrawText(56,88,b,C_DATA,1);
  Panel(4,116,90,72,"HOW",C_ACCENT,0);
  DrawText(10,136,"TAP",C_ACCENT,1);   DrawText(40,136,"SPIN",C_SAND,1);
  DrawText(10,150,"SWIPE",C_ACCENT,1); DrawText(52,150,"MOVE",C_SAND,1);
  DrawText(10,164,"DOWN",C_ACCENT,1);  DrawText(48,164,"DROP",C_SAND,1);
  DrawText(10,178,"HOLD LOW = FAST",C_SAND,1);
  if (ttOver){
    Scrim(170);
    GlowTextC(160,100,"GAME OVER",C_WARN,3,110);
    snprintf(b,sizeof(b),"SCORE %d",ttScore);
    DrawTextC(160,132,b,C_DATA,1);
    DrawTextC(160,150,"TAP TO RESTART",C_SAND,1); }
  DrawParticles();
  TopBar("TETRIS",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : MEMORY  (match the pairs)
// =====================================================================
#define ME_R 3
#define ME_C 6
#define ME_N (ME_R*ME_C)
static uint8_t meVal[ME_N];
static bool    meUp[ME_N], meDone[ME_N];
static Spring  meFlip[ME_N];
static int     meA=-1,meB=-1,meMoves,mePairs;
static float   meWait;
void ResetMemory(void){
  uint8_t v[ME_N];
  for (int i=0;i<ME_N;i++) v[i]=i/2;
  for (int i=ME_N-1;i>0;i--){
    int j=(int)(Hash((uint32_t)(millis()*7919u)+i*131u)*(i+1));
    uint8_t t=v[i]; v[i]=v[j]; v[j]=t; }
  for (int i=0;i<ME_N;i++){ meVal[i]=v[i]; meUp[i]=false; meDone[i]=false;
    meFlip[i].v=0; meFlip[i].vel=0; }
  meA=meB=-1; meMoves=0; mePairs=0; meWait=0;
}
void ScreenMemory(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int CW=48,CH=54,OX=16,OY=34;
  if (meWait>0){ meWait-=dt;
    if (meWait<=0){
      if (meA>=0&&meB>=0){
        if (meVal[meA]==meVal[meB]){ meDone[meA]=meDone[meB]=true; mePairs++;
          if (mePairs==ME_N/2){
            int sc=clampi(999-meMoves*8,0,999);
            if (sc>hsMemory){ hsMemory=sc; SaveScores(); }
            SpawnBurst(160,120,44,C_HILITE,200.0f,PK_SPARK); } }
        else { meUp[meA]=meUp[meB]=false; }
        meA=meB=-1; } } }
  if (touchDown&&touchY>BACK_H&&meWait<=0){
    if (mePairs==ME_N/2) ResetMemory();
    else for (int i=0;i<ME_N;i++){
      int x=OX+(i%ME_C)*CW, y=OY+(i/ME_C)*CH;
      if (touchX>=x&&touchX<x+CW-4&&touchY>=y&&touchY<y+CH-4){
        if (meUp[i]||meDone[i]) break;
        meUp[i]=true;
        SpawnBurst(x+CW/2,y+CH/2,8,Spec(meVal[i]),90.0f,PK_SPARK);
        if (meA<0) meA=i;
        else if (meB<0){ meB=i; meMoves++; meWait=0.65f; }
        break; } } }
  for (int i=0;i<ME_N;i++)
    SpringTo(meFlip[i],(meUp[i]||meDone[i])?1.0f:0.0f,300.0f,20.0f,dt);
  Backdrop();
  Panel(8,26,304,180,0,C_ACCENT,0);
  for (int i=0;i<ME_N;i++){
    float d=Stagger(enterAnim,i,0.02f,0.4f);
    if (d<=0.01f) continue;
    int x=OX+(i%ME_C)*CW, y=OY+(i/ME_C)*CH+(int)((1.0f-d)*14);
    float f=meFlip[i].v;
    int w=(int)((CW-8)*fabsf(1.0f-2.0f*clampf(f,0,1))*0.5f+ (CW-8)*0.5f*(f>0.5f?1:1));
    // simple horizontal flip: width shrinks to 0 at the halfway point
    float ph=clampf(f,0,1);
    int fw=(int)((CW-8)*fabsf(cosf(ph*(float)M_PI)));
    if (fw<2) fw=2;
    int cx=x+(CW-4)/2;
    bool showFace=(ph>0.5f);
    uint16_t c=showFace?Spec(meVal[i]):Dim(C_ACCENT,1,5);
    if (meDone[i]) c=Fade(Spec(meVal[i]),150);
    BlendRectFB(cx-fw/2,y,fw,CH-8,c,A_FILL);
    Bracket(cx-fw/2,y,fw,CH-8,meDone[i]?Dim(C_TEXT,3,5):C_TEXT,4);
    if (showFace&&fw>16){
      char s[4]; s[0]='A'+meVal[i]; s[1]=0;
      DrawTextC(cx,y+(CH-8)/2-7,s,C_BG,2);
      if (!meDone[i]) Glow(cx,y+(CH-8)/2,c,60,1.2f);
    } else if (!showFace&&fw>16){
      for (int k=0;k<3;k++) HLineFB(cx-6,y+16+k*6,12,Dim(C_ACCENT,3,5)); } }
  char b[32];
  snprintf(b,sizeof(b),"MOVES %d",meMoves); DrawText(10,214,b,C_DATA,1);
  snprintf(b,sizeof(b),"PAIRS %d/%d",mePairs,ME_N/2); DrawText(110,214,b,C_SAND,1);
  snprintf(b,sizeof(b),"BEST %d",hsMemory); DrawText(226,214,b,C_SAND,1);
  if (mePairs==ME_N/2){
    Scrim(170);
    GlowTextC(160,100,"COMPLETE",C_HILITE,3,110);
    snprintf(b,sizeof(b),"IN %d MOVES",meMoves);
    DrawTextC(160,132,b,C_DATA,1);
    DrawTextC(160,150,"TAP TO PLAY AGAIN",C_SAND,1); }
  DrawParticles();
  TopBar("MEMORY",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : SIMON  (repeat the sequence)
// =====================================================================
#define SI_MAX 32
static uint8_t siSeq[SI_MAX];
static int   siLen,siStep,siShow;
static float siTimer,siLit;
static int   siLitIdx=-1;
static bool  siPlaying,siOver;
void ResetSimon(void){
  siLen=1; siStep=0; siShow=0; siTimer=0; siLit=0; siLitIdx=-1;
  siPlaying=false; siOver=false;
  for (int i=0;i<SI_MAX;i++) siSeq[i]=(uint8_t)((int)(Hash((uint32_t)(millis()*7919u)+i*313u)*4)%4);
}
void ScreenSimon(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int PADS[4][2]={{86,84},{234,84},{86,180},{234,180}};
  const int PR=44;
  siLit=Approach(siLit,0,6.0f,dt);
  if (!siPlaying&&!siOver){
    siTimer+=dt;
    if (siTimer>0.55f){
      siTimer=0;
      if (siShow<siLen){ siLitIdx=siSeq[siShow]; siLit=1.0f; siShow++;
        SpawnBurst(PADS[siLitIdx][0],PADS[siLitIdx][1],10,Spec(siLitIdx),100.0f,PK_SPARK); }
      else { siPlaying=true; siStep=0; siLitIdx=-1; } } }
  if (touchDown&&touchY>BACK_H){
    if (siOver) ResetSimon();
    else if (siPlaying){
      for (int i=0;i<4;i++){
        int dx=touchX-PADS[i][0], dy=touchY-PADS[i][1];
        if (dx*dx+dy*dy<PR*PR){
          siLitIdx=i; siLit=1.0f;
          SpawnBurst(PADS[i][0],PADS[i][1],12,Spec(i),110.0f,PK_SPARK);
          if (siSeq[siStep]==i){
            siStep++;
            if (siStep>=siLen){
              siLen++; siShow=0; siPlaying=false; siTimer=-0.4f;
              if (siLen-1>hsSimon){ hsSimon=siLen-1; SaveScores(); }
              if (siLen>=SI_MAX) siLen=SI_MAX-1; }
          } else { siOver=true;
            SpawnBurst(PADS[i][0],PADS[i][1],34,C_WARN,180.0f,PK_EMBER); }
          break; } } } }
  Backdrop();
  for (int i=0;i<4;i++){
    bool lit=(siLitIdx==i&&siLit>0.02f);
    uint16_t c=Spec(i);
    int r=PR+(int)(lit?siLit*4:0);
    CircleFB(PADS[i][0],PADS[i][1],r,lit?c:Dim(c,1,4),A_FILL);
    RingFB(PADS[i][0],PADS[i][1],r,lit?C_TEXT:Dim(c,3,5),255);
    if (lit) Glow(PADS[i][0],PADS[i][1],c,(uint8_t)(160*siLit),2.6f);
    else Glow(PADS[i][0],PADS[i][1],c,26,1.6f); }
  Panel(120,104,80,56,0,C_ACCENT,0);
  char b[16];
  snprintf(b,sizeof(b),"%d",siLen-1);
  GlowTextC(160,116,b,C_DATA,3,90);
  DrawTextC(160,144,"ROUND",C_SAND,1);
  snprintf(b,sizeof(b),"BEST %d",hsSimon);
  DrawTextC(160,226,b,C_SAND,1);
  DrawTextC(160,26,siOver?"WRONG - TAP TO RETRY":(siPlaying?"YOUR TURN":"WATCH"),
            siOver?C_WARN:(siPlaying?C_ACCENT:C_SAND),1);
  if (siOver) Scrim(90);
  DrawParticles();
  TopBar("SIMON",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : MINES  (8x6 minesweeper, tap to reveal, hold to flag)
// =====================================================================
#define MN_W 10
#define MN_H 7
#define MN_MINES 10
static uint8_t mnMine[MN_H][MN_W], mnOpen[MN_H][MN_W], mnFlag[MN_H][MN_W];
static bool   mnOver,mnWin;
static uint32_t mnPressT=0;
static int    mnPX=-1,mnPY=-1;
static int    mnLeft;
void ResetMines(void){
  memset(mnMine,0,sizeof(mnMine));
  memset(mnOpen,0,sizeof(mnOpen));
  memset(mnFlag,0,sizeof(mnFlag));
  int placed=0,guard=0;
  while (placed<MN_MINES&&guard++<800){
    int x=(int)(Hash((uint32_t)(millis()*7919u)+guard*131u)*MN_W);
    int y=(int)(Hash((uint32_t)(millis()*104729u)+guard*313u)*MN_H);
    if (x>=0&&x<MN_W&&y>=0&&y<MN_H&&!mnMine[y][x]){ mnMine[y][x]=1; placed++; } }
  mnOver=false; mnWin=false; mnLeft=MN_W*MN_H-MN_MINES;
}
static int MnCount(int x,int y){
  int n=0;
  for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++){
    int nx=x+dx,ny=y+dy;
    if (nx<0||ny<0||nx>=MN_W||ny>=MN_H) continue;
    n+=mnMine[ny][nx]; }
  return n;
}
static void MnOpen(int x,int y){
  if (x<0||y<0||x>=MN_W||y>=MN_H) return;
  if (mnOpen[y][x]||mnFlag[y][x]) return;
  mnOpen[y][x]=1; mnLeft--;
  if (MnCount(x,y)==0)
    for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++)
      if (dx||dy) MnOpen(x+dx,y+dy);
}
void ScreenMines(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int CS=30,OX=10,OY=30;
  if (touchDown&&touchY>BACK_H){
    if (mnOver||mnWin) ResetMines();
    else { mnPX=(touchX-OX)/CS; mnPY=(touchY-OY)/CS; mnPressT=millis(); } }
  if (touchUp&&mnPX>=0&&!mnOver&&!mnWin){
    if (mnPX<MN_W&&mnPY<MN_H&&mnPX>=0&&mnPY>=0){
      bool longPress=(millis()-mnPressT>420);
      if (longPress){ mnFlag[mnPY][mnPX]^=1;
        SpawnBurst(OX+mnPX*CS+CS/2,OY+mnPY*CS+CS/2,8,C_WARN,90.0f,PK_SPARK);
      } else if (!mnFlag[mnPY][mnPX]){
        if (mnMine[mnPY][mnPX]){ mnOver=true;
          SpawnBurst(OX+mnPX*CS+CS/2,OY+mnPY*CS+CS/2,40,C_WARN,200.0f,PK_EMBER);
        } else { MnOpen(mnPX,mnPY);
          SpawnBurst(OX+mnPX*CS+CS/2,OY+mnPY*CS+CS/2,6,C_ACCENT,70.0f,PK_SPARK);
          if (mnLeft<=0){ mnWin=true; SpawnBurst(160,120,44,C_HILITE,200.0f,PK_SPARK); } } } }
    mnPX=mnPY=-1; }
  Backdrop();
  Panel(OX-6,OY-6,MN_W*CS+12,MN_H*CS+12,0,C_ACCENT,0);
  for (int y=0;y<MN_H;y++) for (int x=0;x<MN_W;x++){
    int px=OX+x*CS,py=OY+y*CS;
    if (mnOpen[y][x]){
      BlendRectFB(px+1,py+1,CS-2,CS-2,Dim(C_PANEL,4,5),A_FILL);
      int n=MnCount(x,y);
      if (n){ char s[2]={(char)('0'+n),0};
        DrawTextC(px+CS/2,py+CS/2-5,s,Spec(n-1),2); }
    } else {
      BlendRectFB(px+1,py+1,CS-2,CS-2,Dim(C_ACCENT,1,5),A_FILL);
      Bracket(px+1,py+1,CS-2,CS-2,Dim(C_ACCENT,3,5),4);
      if (mnFlag[y][x]){
        LineFB(px+10,py+7,px+10,py+22,C_WARN,255);
        BlendRectFB(px+11,py+7,8,6,C_WARN,A_FILL);
        Glow(px+CS/2,py+CS/2,C_WARN,80,0.9f); } }
    if ((mnOver)&&mnMine[y][x]){
      CircleFB(px+CS/2,py+CS/2,7,C_WARN,A_FILL);
      Glow(px+CS/2,py+CS/2,C_WARN,110,1.0f); } }
  char b[28];
  snprintf(b,sizeof(b),"LEFT %d",mnLeft>0?mnLeft:0); DrawText(10,224,b,C_DATA,1);
  DrawText(120,224,"TAP OPEN - HOLD FLAG",C_SAND,1);
  if (mnOver){ Scrim(170); GlowTextC(160,104,"BOOM",C_WARN,3,110);
    DrawTextC(160,138,"TAP TO RETRY",C_SAND,1); }
  if (mnWin){ Scrim(170); GlowTextC(160,104,"CLEARED",C_HILITE,3,110);
    DrawTextC(160,138,"TAP TO PLAY AGAIN",C_SAND,1); }
  DrawParticles();
  TopBar("MINES",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : WHACK  (reaction test)
// =====================================================================
#define WH_N 9
static float whUp[WH_N];
static int   whScore,whMiss;
static float whSpawn,whTime;
static bool  whOver;
void ResetWhack(void){
  for (int i=0;i<WH_N;i++) whUp[i]=0;
  whScore=0; whMiss=0; whSpawn=0; whTime=30.0f; whOver=false;
}
void ScreenWhack(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int OX=52,OY=48,SP=86;
  if (!whOver){
    whTime-=dt;
    if (whTime<=0){ whTime=0; whOver=true;
      if (whScore>hsWhack){ hsWhack=whScore; SaveScores(); } }
    whSpawn-=dt;
    if (whSpawn<=0){
      whSpawn=clampf(0.85f-whScore*0.012f,0.28f,0.85f);
      int slot=(int)(Hash((uint32_t)(millis()*7919u))*WH_N)%WH_N;
      if (whUp[slot]<=0) whUp[slot]=1.0f; }
    for (int i=0;i<WH_N;i++)
      if (whUp[i]>0){ whUp[i]-=dt*0.85f;
        if (whUp[i]<=0){ whUp[i]=0; whMiss++; } } }
  if (touchDown&&touchY>BACK_H){
    if (whOver) ResetWhack();
    else for (int i=0;i<WH_N;i++){
      int cx=OX+(i%3)*SP, cy=OY+(i/3)*56;
      int dx=touchX-cx, dy=touchY-cy;
      if (whUp[i]>0&&dx*dx+dy*dy<26*26){
        whScore+=10; whUp[i]=0;
        SpawnBurst(cx,cy,16,C_HILITE,150.0f,PK_SPARK);
        break; } } }
  Backdrop();
  for (int i=0;i<WH_N;i++){
    int cx=OX+(i%3)*SP, cy=OY+(i/3)*56;
    CircleFB(cx,cy+10,24,Dim(C_HAIR,3,5),A_FILL);
    RingFB(cx,cy+10,24,Dim(C_ACCENT,2,5),200);
    if (whUp[i]>0){
      float p=clampf(whUp[i]*2.2f,0,1);
      int oy=(int)((1.0f-p)*20);
      uint16_t c=Spec(i%6);
      CircleFB(cx,cy+oy,17,c,255);
      Glow(cx,cy+oy,c,120,1.4f);
      PxBlend(cx-5,cy+oy-3,C_BG,255); PxBlend(cx+5,cy+oy-3,C_BG,255);
      BlendRectFB(cx-4,cy+oy+4,8,2,C_BG,200); } }
  char b[28];
  snprintf(b,sizeof(b),"SCORE %d",whScore); GlowText(8,224,b,C_DATA,1,60);
  snprintf(b,sizeof(b),"MISS %d",whMiss);   DrawText(118,224,b,C_WARN,1);
  snprintf(b,sizeof(b),"BEST %d",hsWhack);  DrawText(214,224,b,C_SAND,1);
  { int bw=(int)(300*clampf(whTime/30.0f,0,1));
    HLineFB(10,26,300,C_HAIR);
    FillRectFB(10,26,bw,3,whTime<6.0f?C_WARN:C_ACCENT);
    Glow(10+bw,27,whTime<6.0f?C_WARN:C_HILITE,130,0.7f); }
  if (whOver){
    Scrim(170);
    GlowTextC(160,100,"TIME UP",C_WARN,3,110);
    snprintf(b,sizeof(b),"SCORE %d",whScore);
    DrawTextC(160,132,b,C_DATA,1);
    DrawTextC(160,150,"TAP TO RETRY",C_SAND,1); }
  DrawParticles();
  TopBar("WHACK",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : DODGE  (falling blocks)
// =====================================================================
#define DO_N 12
static float doX[DO_N],doY[DO_N],doSp[DO_N];
static uint8_t doHue[DO_N];
static Spring doPlayer;
static float doTime,doSpeed;
static int   doScore;
static bool  doOver;
void ResetDodge(void){
  for (int i=0;i<DO_N;i++){
    doX[i]=Hash(i*331u)*300.0f+10.0f;
    doY[i]=-20.0f-i*40.0f;
    doSp[i]=90.0f+Hash(i*77u)*60.0f;
    doHue[i]=i%6; }
  doPlayer.v=160; doPlayer.vel=0;
  doTime=0; doSpeed=1.0f; doScore=0; doOver=false;
}
void ScreenDodge(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int PY=206;
  if (touchDown&&doOver) ResetDodge();
  if (touchActive&&touchY>BACK_H&&!doOver)
    SpringTo(doPlayer,clampf((float)touchX,14,SCREEN_W-14),240.0f,19.0f,dt);
  else SpringTo(doPlayer,doPlayer.v,240.0f,19.0f,dt);
  if (!doOver){
    doTime+=dt; doSpeed+=0.055f*dt;
    for (int i=0;i<DO_N;i++){
      doY[i]+=doSp[i]*doSpeed*dt;
      if (doY[i]>SCREEN_H+16){
        doY[i]=-16.0f-Hash((uint32_t)(gTime*511.0f)+i*97u)*90.0f;
        doX[i]=10.0f+Hash((uint32_t)(gTime*733.0f)+i*211u)*300.0f;
        doHue[i]=(uint8_t)(((int)(doTime)+i)%6);
        doScore++; }
      if (fabsf(doX[i]-doPlayer.v)<15&&fabsf(doY[i]-PY)<15){
        doOver=true;
        if (doScore>hsDodge){ hsDodge=doScore; SaveScores(); }
        SpawnBurst(doPlayer.v,PY,44,C_WARN,220.0f,PK_EMBER); } } }
  Backdrop();
  for (int i=0;i<DO_N;i++){
    uint16_t c=Spec(doHue[i]);
    int x=(int)doX[i],y=(int)doY[i];
    BlendRectFB(x-9,y-9,18,18,c,A_FILL);
    Bracket(x-9,y-9,18,18,Fade(C_TEXT,120),4);
    Glow(x,y,c,60,1.1f);
    LineAdd(x,y-10,x,y-22,c,60); }
  { int px=(int)doPlayer.v;
    CircleFB(px,PY,10,C_HILITE,255);
    RingFB(px,PY,13,C_ACCENT,200);
    Glow(px,PY,C_ACCENT,(uint8_t)(110+50*Pulse(gTime,7.0f)),1.6f); }
  char b[28];
  snprintf(b,sizeof(b),"SCORE %d",doScore); GlowText(8,224,b,C_DATA,1,60);
  snprintf(b,sizeof(b),"BEST %d",hsDodge);  DrawText(120,224,b,C_SAND,1);
  snprintf(b,sizeof(b),"SPD %d",(int)(doSpeed*10)); DrawText(230,224,b,C_SAND,1);
  if (doOver){
    Scrim(170);
    GlowTextC(160,100,"CRASHED",C_WARN,3,110);
    snprintf(b,sizeof(b),"SCORE %d",doScore);
    DrawTextC(160,132,b,C_DATA,1);
    DrawTextC(160,150,"TAP TO RETRY",C_SAND,1); }
  DrawParticles();
  TopBar("DODGE",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  GAME : LIGHTS OUT
// =====================================================================
#define LO_N 5
static uint8_t loG[LO_N][LO_N];
static Spring  loS[LO_N][LO_N];
static int     loMoves;
static bool    loWin;
void ResetLights(void){
  for (int y=0;y<LO_N;y++) for (int x=0;x<LO_N;x++){
    loG[y][x]=0; loS[y][x].v=0; loS[y][x].vel=0; }
  // scramble by applying legal toggles, so it is always solvable
  for (int i=0;i<14;i++){
    int x=(int)(Hash((uint32_t)(millis()*7919u)+i*131u)*LO_N)%LO_N;
    int y=(int)(Hash((uint32_t)(millis()*104729u)+i*313u)*LO_N)%LO_N;
    loG[y][x]^=1;
    if (x>0) loG[y][x-1]^=1;
    if (x<LO_N-1) loG[y][x+1]^=1;
    if (y>0) loG[y-1][x]^=1;
    if (y<LO_N-1) loG[y+1][x]^=1; }
  loMoves=0; loWin=false;
}
void ScreenLights(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int CS=38,OX=65,OY=44;
  if (touchDown&&touchY>BACK_H){
    if (loWin) ResetLights();
    else { int x=(touchX-OX)/CS, y=(touchY-OY)/CS;
      if (x>=0&&x<LO_N&&y>=0&&y<LO_N){
        loG[y][x]^=1;
        if (x>0) loG[y][x-1]^=1;
        if (x<LO_N-1) loG[y][x+1]^=1;
        if (y>0) loG[y-1][x]^=1;
        if (y<LO_N-1) loG[y+1][x]^=1;
        loMoves++;
        SpawnBurst(OX+x*CS+CS/2,OY+y*CS+CS/2,12,C_ACCENT,110.0f,PK_SPARK);
        bool all=true;
        for (int yy=0;yy<LO_N&&all;yy++) for (int xx=0;xx<LO_N;xx++) if (loG[yy][xx]){ all=false; break; }
        if (all){ loWin=true; SpawnBurst(160,120,46,C_HILITE,200.0f,PK_SPARK); } } } }
  for (int y=0;y<LO_N;y++) for (int x=0;x<LO_N;x++)
    SpringTo(loS[y][x],loG[y][x]?1.0f:0.0f,300.0f,20.0f,dt);
  Backdrop();
  Panel(OX-10,OY-10,LO_N*CS+20,LO_N*CS+20,0,C_ACCENT,0);
  for (int y=0;y<LO_N;y++) for (int x=0;x<LO_N;x++){
    float d=Stagger(enterAnim,y*LO_N+x,0.015f,0.4f);
    if (d<=0.01f) continue;
    int px=OX+x*CS,py=OY+y*CS;
    float on=loS[y][x].v;
    uint16_t c=Spec((x+y)%6);
    uint16_t fill=on>0.5f?c:Dim(c,1,6);
    int inset=(int)(3+(1.0f-on)*2);
    BlendRectFB(px+inset,py+inset,CS-inset*2,CS-inset*2,fill,(uint8_t)(A_FILL*d));
    Bracket(px+inset,py+inset,CS-inset*2,CS-inset*2,on>0.5f?C_TEXT:Dim(c,3,5),4);
    if (on>0.1f) Glow(px+CS/2,py+CS/2,c,(uint8_t)(110*on),1.5f); }
  char b[28];
  snprintf(b,sizeof(b),"MOVES %d",loMoves); DrawText(10,224,b,C_DATA,1);
  DrawTextC(190,224,"TURN ALL LIGHTS OFF",C_SAND,1);
  if (loWin){
    Scrim(170);
    GlowTextC(160,100,"SOLVED",C_HILITE,3,110);
    snprintf(b,sizeof(b),"IN %d MOVES",loMoves);
    DrawTextC(160,132,b,C_DATA,1);
    DrawTextC(160,150,"TAP FOR A NEW BOARD",C_SAND,1); }
  DrawParticles();
  TopBar("LIGHTS OUT",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  DRAW
// =====================================================================
#define DRAW_TOP 20
static uint8_t drawCol=1,drawSize=3;
static bool    drawErase=false,drawPrev=false;
static float   dpX=0,dpY=0;
static uint16_t BrushCol(int i){
  if (i==0) return C_TEXT;
  if (i==7) return C_PANEL;
  return Spec(i-1);
}
static void CanvasDot(int cx,int cy,int r,uint16_t c){
  for (int y=-r;y<=r;y++){
    int yy=cy+y;
    if (yy<DRAW_TOP||yy>=SCREEN_H-22) continue;
    int w=(int)sqrtf(fmaxf(0.0f,(float)(r*r-y*y)));
    int x0=clampi(cx-w,0,SCREEN_W-1), x1=clampi(cx+w,0,SCREEN_W-1);
    uint16_t *row=&canvas[yy*SCREEN_W];
    for (int x=x0;x<=x1;x++) row[x]=c; }
}
void ScreenDraw(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.2f,0,1);
  const int barY=SCREEN_H-22;
  bool onCanvas=(touchY>=DRAW_TOP&&touchY<barY);
  if (touchDown&&touchY>=barY){
    for (int i=0;i<8;i++){ int bx=4+i*20;
      if (touchX>=bx&&touchX<bx+18){ drawCol=i; drawErase=false;
        SpawnBurst(bx+9,barY+10,8,BrushCol(i),80.0f,PK_SPARK); } }
    if (touchX>=168&&touchX<208){ drawSize=(drawSize>=9)?1:drawSize+2;
      SpawnBurst(188,barY+10,8,C_ACCENT,80.0f,PK_SPARK); }
    if (touchX>=212&&touchX<252){ drawErase=!drawErase;
      SpawnBurst(232,barY+10,8,C_WARN,80.0f,PK_SPARK); }
    if (touchX>=256&&touchX<316){
      for (int i=0;i<FB_PIXELS;i++) canvas[i]=C_BG;
      SpawnBurst(286,barY+10,30,C_WARN,180.0f,PK_SPARK); }
    drawPrev=false; }
  if (touchActive&&onCanvas&&!(pressX<BACK_W&&pressY<BACK_H)){
    uint16_t c=drawErase?C_BG:BrushCol(drawCol);
    if (drawPrev){
      float dx=smoothTX-dpX,dy=smoothTY-dpY;
      int steps=(int)(fmaxf(fabsf(dx),fabsf(dy))/fmaxf(1.0f,drawSize*0.4f))+1;
      for (int i=0;i<=steps;i++){ float t=(float)i/steps;
        CanvasDot((int)(dpX+dx*t),(int)(dpY+dy*t),drawSize,c); }
    } else CanvasDot((int)smoothTX,(int)smoothTY,drawSize,c);
    dpX=smoothTX; dpY=smoothTY; drawPrev=true;
    if (!drawErase&&((int)(gTime*60)&3)==0)
      SpawnBurst(smoothTX,smoothTY,1,c,35.0f,PK_SPARK); }
  if (!touchActive) drawPrev=false;
  memcpy(frame,canvas,FB_BYTES);
  BlendRectFB(0,barY,SCREEN_W,22,C_PANEL,A_FILL);
  HLineFB(0,barY,SCREEN_W,Dim(C_ACCENT,3,5));
  for (int i=0;i<8;i++){
    int bx=4+i*20;
    bool sel=(i==drawCol&&!drawErase);
    FillRectFB(bx,barY+4,18,14,BrushCol(i));
    Bracket(bx,barY+4,18,14,sel?C_TEXT:C_HAIR,4);
    if (sel){ Glow(bx+9,barY+11,BrushCol(i),(uint8_t)(110*Pulse(gTime,4.0f)),0.9f);
      HLineFB(bx,barY+2,18,C_TEXT); } }
  char sz[10]; snprintf(sz,sizeof(sz),"SZ %d",drawSize);
  Button(168,barY+3,40,16,sz,C_ACCENT,false);
  Button(212,barY+3,40,16,"ERASE",C_WARN,drawErase);
  Button(256,barY+3,60,16,"CLEAR",C_WARN,false);
  if (touchActive&&onCanvas){
    RingFB((int)smoothTX,(int)smoothTY,drawSize+3,
           drawErase?C_WARN:C_TEXT,(uint8_t)(A_GLOW+80*Pulse(gTime,6.0f)));
    Glow((int)smoothTX,(int)smoothTY,drawErase?C_WARN:BrushCol(drawCol),60,0.8f); }
  DrawParticles();
  TopBar("DRAW",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  BIG 7-SEGMENT DIGITS  (used by clock / stopwatch / timer)
// =====================================================================
static const uint8_t SEG7[11] = {
  0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F,0x00 };
static void Seg(int x,int y,int w,int h,int t,int digit,uint16_t c,uint8_t glow){
  if (digit<0||digit>10) return;
  uint8_t m=SEG7[digit];
  uint16_t off=Dim(c,1,7);
  int hw=w, hh=h/2;
  struct { int x,y,w,h; uint8_t bit; } S[7] = {
    { x+t,        y,           hw-2*t, t,      0 },  // A top
    { x+hw-t,     y+t,         t,      hh-t,   1 },  // B upper right
    { x+hw-t,     y+hh,        t,      hh-t,   2 },  // C lower right
    { x+t,        y+2*hh-t,    hw-2*t, t,      3 },  // D bottom
    { x,          y+hh,        t,      hh-t,   4 },  // E lower left
    { x,          y+t,         t,      hh-t,   5 },  // F upper left
    { x+t,        y+hh-t/2,    hw-2*t, t,      6 }   // G middle
  };
  for (int i=0;i<7;i++){
    bool on=(m>>S[i].bit)&1;
    FillRectFB(S[i].x,S[i].y,S[i].w,S[i].h,on?c:off);
    if (on&&glow) Glow(S[i].x+S[i].w/2,S[i].y+S[i].h/2,c,glow,
                       (S[i].w>S[i].h)?1.5f:1.2f); }
}
static void SegColon(int x,int y,int h,uint16_t c,bool on,uint8_t glow){
  int s=h/8; if (s<3) s=3;
  uint16_t cc=on?c:Dim(c,1,7);
  FillRectFB(x,y+h/3-s/2,s,s,cc);
  FillRectFB(x,y+2*h/3-s/2,s,s,cc);
  if (on&&glow){ Glow(x+s/2,y+h/3,c,glow,0.8f); Glow(x+s/2,y+2*h/3,c,glow,0.8f); }
}

// =====================================================================
//  CLOCK  --  large numeric readout (no analog dial)
// =====================================================================
void ScreenClock(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  int hh=0,mm=0,ss=0;
  char dstr[40]="NO NTP SYNC";
  if (timeOk){
    struct tm t;
    if (getLocalTime(&t,0)){
      hh=t.tm_hour; mm=t.tm_min; ss=t.tm_sec;
      strftime(dstr,sizeof(dstr),"%A  %d %B %Y",&t); }
  } else {
    uint32_t up=millis()/1000;
    hh=(up/3600)%24; mm=(up/60)%60; ss=up%60;
    snprintf(dstr,sizeof(dstr),"UPTIME - NO WIFI"); }
  Backdrop();
  float e=EaseOutCubic(enterAnim);
  uint8_t gl=(uint8_t)(90*e);
  // HH : MM  large
  int dw=52,dh=76,th=8,x=22,y=52;
  Seg(x,y,dw,dh,th,hh/10,C_ACCENT,gl);
  Seg(x+dw+10,y,dw,dh,th,hh%10,C_ACCENT,gl);
  SegColon(x+2*dw+26,y,dh,C_HILITE,(millis()%1000)<600,gl);
  Seg(x+2*dw+44,y,dw,dh,th,mm/10,C_ACCENT,gl);
  Seg(x+3*dw+54,y,dw,dh,th,mm%10,C_ACCENT,gl);
  // seconds, smaller
  int sx=x+4*dw+68;
  Seg(sx,y+40,20,34,4,ss/10,C_DATA,(uint8_t)(gl/2));
  Seg(sx+24,y+40,20,34,4,ss%10,C_DATA,(uint8_t)(gl/2));
  // second progress bar
  { float fs=ss+(millis()%1000)*0.001f;
    int bw=(int)(276*(fs/60.0f));
    HLineFB(22,144,276,C_HAIR);
    FillRectFB(22,144,bw,3,C_ACCENT);
    Glow(22+bw,145,C_HILITE,140,0.8f); }
  GlowTextC(160,160,dstr,C_TEXT,1,60);
  Panel(22,176,132,52,"ZONE",C_ACCENT,0);
  DrawText(30,196,"IST  UTC +5:30",C_DATA,1);
  DrawText(30,208,timeOk?"NTP SYNCED":"AWAITING NTP",timeOk?C_DATA:C_WARN,1);
  Panel(166,176,132,52,"NETWORK",C_ACCENT,0);
  DrawText(174,196,netUp?"ONLINE":"OFFLINE",netUp?C_DATA:C_WARN,1);
  DrawText(174,208,ipStr,C_SAND,1);
  DrawParticles();
  TopBar("CLOCK",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  STOPWATCH
// =====================================================================
static uint32_t swStart=0,swAccum=0;
static bool     swRun=false;
static uint32_t swLaps[5]; static int swLapN=0;
void ScreenStopwatch(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  uint32_t el=swAccum+(swRun?(millis()-swStart):0);
  Backdrop();
  int mn=(el/60000)%100, sc=(el/1000)%60, cs=(el%1000)/10;
  int dw=44,dh=64,th=7,x=26,y=46;
  uint8_t gl=swRun?(uint8_t)(80+40*Pulse(gTime,5.0f)):60;
  Seg(x,y,dw,dh,th,mn/10,C_ACCENT,gl);
  Seg(x+dw+8,y,dw,dh,th,mn%10,C_ACCENT,gl);
  SegColon(x+2*dw+20,y,dh,C_HILITE,true,gl);
  Seg(x+2*dw+34,y,dw,dh,th,sc/10,C_ACCENT,gl);
  Seg(x+3*dw+42,y,dw,dh,th,sc%10,C_ACCENT,gl);
  Seg(x+4*dw+58,y+30,22,34,4,cs/10,C_DATA,(uint8_t)(gl/2));
  Seg(x+4*dw+84,y+30,22,34,4,cs%10,C_DATA,(uint8_t)(gl/2));
  DrawText(x+4*dw+58,y+70,"CS",C_SAND,1);
  if (Button(20,128,88,28,swRun?"PAUSE":"START",swRun?C_WARN:C_ACCENT,swRun)){
    if (swRun){ swAccum+=millis()-swStart; swRun=false; }
    else { swStart=millis(); swRun=true; }
    SpawnBurst(64,142,14,C_ACCENT,120.0f,PK_SPARK); }
  if (Button(116,128,88,28,"LAP",C_ACCENT,false)){
    if (swRun&&swLapN<5){ swLaps[swLapN++]=el;
      SpawnBurst(160,142,14,C_HILITE,120.0f,PK_SPARK); } }
  if (Button(212,128,88,28,"RESET",C_WARN,false)){
    swRun=false; swAccum=0; swLapN=0;
    SpawnBurst(256,142,18,C_WARN,140.0f,PK_SPARK); }
  Panel(20,164,280,66,"LAPS",C_ACCENT,0);
  for (int i=0;i<swLapN;i++){
    uint32_t l=swLaps[i];
    char b[32];
    snprintf(b,sizeof(b),"%d   %02u:%02u.%02u",i+1,
             (unsigned)((l/60000)%100),(unsigned)((l/1000)%60),(unsigned)((l%1000)/10));
    DrawText(28+(i%2)*140,184+(i/2)*14,b,C_DATA,1); }
  if (!swLapN) DrawText(28,190,"NO LAPS RECORDED",C_SAND,1);
  DrawParticles();
  TopBar("STOPWATCH",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  TIMER  (countdown with alarm)
// =====================================================================
static int      tmSetMin=5,tmSetSec=0;
static uint32_t tmEnd=0,tmRemain=0;
static bool     tmRun=false,tmAlarm=false;
void ScreenTimer(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (tmRun){
    uint32_t now=millis();
    tmRemain=(now<tmEnd)?(tmEnd-now):0;
    if (tmRemain==0&&!tmAlarm){ tmAlarm=true; tmRun=false;
      SpawnBurst(160,110,60,C_WARN,240.0f,PK_EMBER); } }
  Backdrop();
  uint32_t el=tmRun?tmRemain:(uint32_t)((tmSetMin*60+tmSetSec)*1000);
  if (tmAlarm) el=0;
  int mn=(el/60000)%100, sc=(el/1000)%60;
  int dw=52,dh=74,th=8,x=44,y=42;
  bool flash=tmAlarm&&((millis()/250)%2==0);
  uint16_t dc=tmAlarm?(flash?C_WARN:Dim(C_WARN,2,5)):
              (tmRemain<10000&&tmRun?C_WARN:C_ACCENT);
  uint8_t gl=(uint8_t)(tmAlarm&&flash?150:80);
  Seg(x,y,dw,dh,th,mn/10,dc,gl);
  Seg(x+dw+10,y,dw,dh,th,mn%10,dc,gl);
  SegColon(x+2*dw+26,y,dh,dc,true,gl);
  Seg(x+2*dw+44,y,dw,dh,th,sc/10,dc,gl);
  Seg(x+3*dw+54,y,dw,dh,th,sc%10,dc,gl);
  // ring progress
  if (tmRun||tmAlarm){
    uint32_t total=(uint32_t)((tmSetMin*60+tmSetSec)*1000);
    float fr=total?clampf((float)tmRemain/total,0,1):0;
    ArcFB(160,126,116,-1.5708f,-1.5708f+TAU*fr,dc,180); }
  if (!tmRun&&!tmAlarm){
    if (Button(18,132,50,26,"M +",C_ACCENT,false)){ tmSetMin=(tmSetMin+1)%100;
      SpawnBurst(43,145,8,C_ACCENT,80.0f,PK_SPARK); }
    if (Button(74,132,50,26,"M -",C_ACCENT,false)) tmSetMin=(tmSetMin+99)%100;
    if (Button(196,132,50,26,"S +",C_ACCENT,false)) tmSetSec=(tmSetSec+5)%60;
    if (Button(252,132,50,26,"S -",C_ACCENT,false)) tmSetSec=(tmSetSec+55)%60; }
  if (Button(60,170,90,30,tmRun?"PAUSE":"START",tmRun?C_WARN:C_ACCENT,tmRun)){
    if (tmAlarm){ tmAlarm=false; }
    else if (tmRun){ tmSetMin=(tmRemain/60000)%100; tmSetSec=(tmRemain/1000)%60; tmRun=false; }
    else { uint32_t ms=(uint32_t)((tmSetMin*60+tmSetSec)*1000);
      if (ms>0){ tmEnd=millis()+ms; tmRun=true; tmAlarm=false; } }
    SpawnBurst(105,185,16,C_ACCENT,130.0f,PK_SPARK); }
  if (Button(170,170,90,30,"RESET",C_WARN,false)){
    tmRun=false; tmAlarm=false; tmRemain=0;
    SpawnBurst(215,185,16,C_WARN,130.0f,PK_SPARK); }
  DrawTextC(160,212,tmAlarm?"TIME UP - TAP START":
            (tmRun?"COUNTING DOWN":"SET DURATION THEN START"),
            tmAlarm?C_WARN:C_SAND,1);
  if (tmAlarm&&flash) Scrim(60);
  DrawParticles();
  TopBar("TIMER",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  ON-SCREEN KEYBOARD (used by the WIFI module)
// =====================================================================
static char kbBuf[66];
static int  kbLen=0;
static bool kbShift=false, kbNum=false;
static const char *KB_ROWS[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static const char *KB_NUMS[3] = { "1234567890", "-_.:/@#$%", "!?+=*&," };
// returns 1 = OK pressed, 2 = cancel
static int Keyboard(const char *title){
  Panel(4,20,312,52,title,C_ACCENT,0);
  BlendRectFB(10,40,300,24,Dim(C_ACCENT,1,6),A_FILL);
  Bracket(10,40,300,24,C_ACCENT,5);
  char shown[70];
  snprintf(shown,sizeof(shown),"%s",kbBuf);
  DrawText(16,48,shown,C_TEXT,1);
  if ((millis()/400)%2==0){
    int cx=16+TextW(shown,1);
    VLineFB(cx,46,10,C_HILITE); }
  int ret=0;
  const char **rows = kbNum?KB_NUMS:KB_ROWS;
  for (int r=0;r<3;r++){
    int n=strlen(rows[r]);
    int kw=28,kh=30;
    int ox=(SCREEN_W-n*kw)/2;
    for (int i=0;i<n;i++){
      int x=ox+i*kw, y=78+r*34;
      char c=rows[r][i];
      char lbl[2]={ (kbShift||kbNum)?c:(char)(c+32), 0 };
      if (Button(x+1,y,kw-2,kh-2,lbl,C_ACCENT,false)){
        if (kbLen<(int)sizeof(kbBuf)-1){ kbBuf[kbLen++]=lbl[0]; kbBuf[kbLen]=0;
          SpawnBurst(x+kw/2,y+kh/2,6,C_ACCENT,70.0f,PK_SPARK); } } } }
  if (Button(6,180,52,28,kbNum?"ABC":"123",C_ACCENT,kbNum)) kbNum=!kbNum;
  if (Button(62,180,52,28,"SHIFT",C_ACCENT,kbShift)) kbShift=!kbShift;
  if (Button(118,180,84,28,"SPACE",C_ACCENT,false)){
    if (kbLen<(int)sizeof(kbBuf)-1){ kbBuf[kbLen++]=' '; kbBuf[kbLen]=0; } }
  if (Button(206,180,52,28,"DEL",C_WARN,false)){
    if (kbLen>0){ kbBuf[--kbLen]=0; } }
  if (Button(262,180,52,28,"OK",C_ACCENT,false)) ret=1;
  if (Button(6,212,100,24,"CANCEL",C_WARN,false)) ret=2;
  DrawTextC(210,220,"ENTER CREDENTIALS",C_SAND,1);
  return ret;
}

// =====================================================================
//  WIFI TOOLS  --  scan, connect, keyboard entry, live diagnostics
// =====================================================================
static int   wfCount=0, wfSel=-1, wfScroll=0;
static bool  wfScanning=false;
static uint32_t wfPingT=0;
static char  wfList[12][34];
static int32_t wfRssi[12];
static uint8_t wfEnc[12];

static int   wfMode=0;             // 0 status, 1 scan, 2 ssid kb, 3 pass kb
void NetBegin(const char *ssid,const char *pass){
  if (!ssid[0]) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid,pass);
  Serial.printf("WiFi: connecting to \"%s\"\n",ssid);
}
void NetLoop(void){
  static uint32_t last=0, lastTry=0, downSince=0;
  static uint8_t  failCount=0;
  static bool     usingFallback=false;
  if (millis()-last<500) return;
  last=millis();

  bool up=(WiFi.status()==WL_CONNECTED);
  if (up&&!netUp){
    netUp=true; failCount=0; downSince=0;
    snprintf(ipStr,sizeof(ipStr),"%s",WiFi.localIP().toString().c_str());
    configTime(TZ_OFFSET_SEC,TZ_DST_SEC,NTP_1,NTP_2);
    Serial.printf("WiFi up: %s (%s)\n",ipStr,wifiSsid);
  } else if (!up&&netUp){
    netUp=false; timeOk=false;
    downSince=millis();
    snprintf(ipStr,sizeof(ipStr),"OFFLINE");
    Serial.println("WiFi lost -- will auto-reconnect"); }

  // ---- AUTO-RECONNECT -------------------------------------------------
  //  Always keep trying. Every 4th failed attempt we fall back to the
  //  compiled-in DEF_SSID, so a bad saved credential can never strand the
  //  device offline after a restart.
  if (!up && wfMode==0){
    if (downSince==0) downSince=millis();
    uint32_t backoff = 5000u + (uint32_t)failCount*3000u;
    if (backoff>30000u) backoff=30000u;
    if (millis()-lastTry > backoff){
      lastTry=millis();
      failCount++;
      const char *ss=wifiSsid, *pp=wifiPass;
      if ((failCount%4)==0 && DEF_SSID[0]){
        // periodic fallback to the hardcoded network
        ss=DEF_SSID; pp=DEF_PASS;
        usingFallback=true;
        Serial.printf("WiFi retry %u -> FALLBACK \"%s\"\n",failCount,ss);
      } else {
        usingFallback=false;
        Serial.printf("WiFi retry %u -> \"%s\"\n",failCount,ss[0]?ss:"(empty)"); }
      if (!ss[0]){ ss=DEF_SSID; pp=DEF_PASS; }
      if (ss[0]){
        WiFi.disconnect();
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.begin(ss,pp);
        // if the fallback works, adopt it as the stored network
        if (usingFallback){
          snprintf(wifiSsid,sizeof(wifiSsid),"%s",DEF_SSID);
          snprintf(wifiPass,sizeof(wifiPass),"%s",DEF_PASS); } } } }

  if (netUp&&!timeOk){
    struct tm t;
    if (getLocalTime(&t,0)&&t.tm_year>120){ timeOk=true;
      Serial.printf("NTP synced %04d-%02d-%02d %02d:%02d:%02d\n",
        t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec); } }
}
void ScreenWifi(float dt){
  if (wfMode<2&&BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  Backdrop();
  if (wfMode==2||wfMode==3){
    int r=Keyboard(wfMode==2?"NETWORK NAME":"PASSWORD");
    if (r==1){
      if (wfMode==2){ snprintf(wifiSsid,sizeof(wifiSsid),"%s",kbBuf);
        kbBuf[0]=0; kbLen=0; wfMode=3;
      } else { snprintf(wifiPass,sizeof(wifiPass),"%s",kbBuf);
        kbBuf[0]=0; kbLen=0; wfMode=0;
        SaveSettings(); NetBegin(wifiSsid,wifiPass); } }
    else if (r==2){ kbBuf[0]=0; kbLen=0; wfMode=0; }
    DrawParticles();
    return; }
  if (wfMode==1){
    Panel(4,22,312,212,"NETWORKS",C_ACCENT,wfScanning?"SCAN":"DONE");
    if (wfScanning){
      int n=WiFi.scanComplete();
      if (n>=0){
        wfScanning=false;
        wfCount=(n>12)?12:n;
        for (int i=0;i<wfCount;i++){
          snprintf(wfList[i],sizeof(wfList[i]),"%s",WiFi.SSID(i).c_str());
          wfRssi[i]=WiFi.RSSI(i);
          wfEnc[i]=(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN)?1:0; }
        WiFi.scanDelete(); } }
    if (wfScanning){
      GlowTextC(160,110,"SCANNING",C_ACCENT,2,(uint8_t)(90*Pulse(gTime,4.0f)));
      for (int i=0;i<3;i++){
        float ph=fmodf(gTime*0.8f+i*0.33f,1.0f);
        ArcFB(160,150,(int)(10+ph*30),-2.36f,-0.78f,C_ACCENT,(uint8_t)(A_FILL*(1.0f-ph))); }
    } else {
      for (int i=0;i<wfCount&&i<7;i++){
        int idx=i+wfScroll;
        if (idx>=wfCount) break;
        int y=42+i*25;
        bool sel=(idx==wfSel);
        BlendRectFB(10,y,300,22,sel?Dim(C_ACCENT,2,5):Dim(C_PANEL,4,5),A_FILL);
        Bracket(10,y,300,22,sel?C_ACCENT:C_HAIR,4);
        DrawText(16,y+7,wfList[idx],sel?C_TEXT:C_SAND,1);
        int bars=clampi((wfRssi[idx]+100)/12,1,4);
        for (int b=0;b<4;b++)
          FillRectFB(268+b*7,y+16-b*3,5,3+b*3,(b<bars)?C_DATA:Dim(C_HAIR,3,5));
        if (wfEnc[idx]) DrawText(258,y+7,"-",C_SAND,1);
        if (touchDown&&touchX>=10&&touchX<310&&touchY>=y&&touchY<y+22){
          wfSel=idx;
          snprintf(wifiSsid,sizeof(wifiSsid),"%s",wfList[idx]);
          SpawnBurst(160,y+11,12,C_ACCENT,110.0f,PK_SPARK); } }
      if (Button(10,214,90,20,"RESCAN",C_ACCENT,false)){
        WiFi.scanDelete(); WiFi.scanNetworks(true); wfScanning=true; wfCount=0; }
      if (Button(108,214,110,20,"ENTER PASS",C_ACCENT,false)){
        if (wifiSsid[0]){ kbBuf[0]=0; kbLen=0; wfMode=3; } }
      if (Button(226,214,84,20,"BACK",C_WARN,false)) wfMode=0; }
  } else {
    Panel(4,22,152,118,"STATUS",C_ACCENT,netUp?"UP":"DOWN");
    DrawText(12,44,netUp?"CONNECTED":"DISCONNECTED",netUp?C_DATA:C_WARN,1);
    DrawText(12,58,"SSID",C_SAND,1);
    DrawText(12,70,wifiSsid[0]?wifiSsid:"(none)",C_TEXT,1);
    DrawText(12,86,"IP",C_SAND,1);
    DrawText(12,98,ipStr,C_DATA,1);
    DrawText(12,114,timeOk?"NTP OK":"NTP WAIT",timeOk?C_DATA:C_SAND,1);
    Panel(160,22,156,118,"SIGNAL",C_ACCENT,0);
    int rssi=netUp?WiFi.RSSI():-100;
    int bars=clampi((rssi+100)/12,0,4);
    for (int b=0;b<4;b++){
      int h=8+b*9;
      uint16_t c=(b<bars)?C_DATA:Dim(C_HAIR,3,5);
      FillRectFB(176+b*22,116-h,16,h,c);
      if (b<bars) Glow(176+b*22+8,116-h/2,C_DATA,60,1.0f); }
    char b[24];
    snprintf(b,sizeof(b),"%d dBm",rssi);
    DrawText(176,44,netUp?b:"NO LINK",netUp?C_DATA:C_WARN,1);
    snprintf(b,sizeof(b),"CH %d",netUp?WiFi.channel():0);
    DrawText(250,44,b,C_SAND,1);
    if (Button(8,148,98,30,"SCAN",C_ACCENT,false)){
      WiFi.mode(WIFI_STA); WiFi.scanDelete(); WiFi.scanNetworks(true);
      wfScanning=true; wfCount=0; wfSel=-1; wfScroll=0; wfMode=1; }
    if (Button(112,148,98,30,"SET SSID",C_ACCENT,false)){
      kbBuf[0]=0; kbLen=0; wfMode=2; }
    if (Button(216,148,96,30,"SET PASS",C_ACCENT,false)){
      kbBuf[0]=0; kbLen=0; wfMode=3; }
    if (Button(8,186,150,30,netUp?"DISCONNECT":"CONNECT",netUp?C_WARN:C_ACCENT,false)){
      if (netUp){ WiFi.disconnect(); netUp=false; timeOk=false;
        snprintf(ipStr,sizeof(ipStr),"OFFLINE"); }
      else NetBegin(wifiSsid,wifiPass); }
    if (Button(164,186,148,30,"SYNC TIME",C_ACCENT,false)){
      if (netUp){ configTime(TZ_OFFSET_SEC,TZ_DST_SEC,NTP_1,NTP_2); timeOk=false; } }
    char up[32];
    snprintf(up,sizeof(up),"UPTIME %lu S  MAC %s",
             (unsigned long)(millis()/1000),WiFi.macAddress().c_str());
    DrawText(8,224,up,C_SAND,1); }
  DrawParticles();
  TopBar("WIFI",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  SETTINGS  (all on-device; there is no web UI)
// =====================================================================
void ScreenSettings(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  Backdrop();

  // Scrollable list. Rows are 26 px so the touch target is comfortable
  // while the ink stays small. No boxes -- grouping is spacing + label.
  static float scr=0, scrV=0; static bool drag=false; static int dragY=0;
  const int X=GUTTER, W=CONTENT_W, RH=26;
  const int TOP=CONTENT_TOP, BOT=SCREEN_H-8;
  int y=TOP-(int)scr;

  #define ROWY (y+=RH)-RH
  int contentTop=y;

  UiSection(X,y,"Display"); y+=13;
  { char v[8]; snprintf(v,sizeof(v),"%d%%",(gBright*100)/255);
    int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT){
      DrawText(X+2,ry+4,"Brightness",C_TEXT,T_BODY);
      float bv=gBright/255.0f;
      static bool blDirty=false;
      int sw=110, sx=X+W-sw-2;
      bool sdrag=touchActive&&UiHit(sx-8,ry+2,sw+16,RH-4,0);
      if (sdrag){ bv=clampf((float)(touchX-sx)/sw,0.03f,1.0f);
                  SetBrightness((uint8_t)clampf(bv*255.0f,8,255)); blDirty=true; }
      if (!touchActive&&blDirty){ blDirty=false; SaveSettings(); }
      int ty=ry+RH/2-1;
      UiRect(sx,ty,sw,3,C_SURFACE2,255);
      UiRect(sx,ty,(int)(sw*bv),3,C_ACCENT,255);
      UiRect(sx+(int)(sw*bv)-2,ty-4,4,11,sdrag?C_HILITE:C_ACCENT,255);
      DrawText(X+W-TextW(v,T_SMALL)-2,ry+RH-9,v,C_OFF,T_SMALL); } }
  { int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT){
      // Tap cycles; the current theme is named rather than numbered.
      if (UiRow(X,ry,W,RH,"Theme",THEMES[gTheme].name,true)){
        ApplyTheme((gTheme+1)%NUM_THEMES); SaveSettings();
        UiToast(THEMES[gTheme].name,0); }
      { int swx=X+W-64;
        for (int i=0;i<NUM_THEMES;i++)
          UiRect(swx+i*11,ry+RH-8,7,3,
                 (i==gTheme)?C_ACCENT:C_SURFACE2,255); } } }
  y+=8;

  UiSection(X,y,"Effects"); y+=13;
  { static const char *G3[3]={"Off","Low","Full"};
    int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT){
      int h2=UiSegRow(X,ry,W,RH,"Glow",G3,3,gGlowLevel);
      if (h2>=0){ gGlowLevel=h2; SaveSettings(); } } }
  { static const char *G3[3]={"Off","Low","Full"};
    int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT){
      int h2=UiSegRow(X,ry,W,RH,"Particles",G3,3,gFxLevel);
      if (h2>=0){ gFxLevel=h2; SaveSettings(); } } }
  { int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT){
      static const char *ON2[2]={"Off","On"};
      int h2=UiSegRow(X,ry,W,RH,"FPS counter",ON2,2,gShowFps?1:0);
      if (h2>=0){ gShowFps=(h2==1); SaveSettings(); } } }
  y+=8;

  UiSection(X,y,"Touch"); y+=13;
  { int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT)
      if (UiRow(X,ry,W,RH,"Calibration",calLoaded?"Custom":"Default",true)){
        CalibReset(); GoTo(ST_CALIB,240,175,C_ACCENT,TR_HEX); return; } }
  { int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT)
      if (UiRow(X,ry,W,RH,"Reset calibration",NULL,false)){
        ResetCalibration(); UiToast("Calibration reset",C_WARN); } }
  y+=8;

  UiSection(X,y,"System"); y+=13;
  { int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT)
      if (UiRow(X,ry,W,RH,"Clear high scores",NULL,false)){
        hs2048=hsBreak=hsFlappy=hsSnake=hsPong=0;
        hsTetris=hsMemory=hsSimon=hsDodge=hsWhack=hsMaze=0;
        SaveScores(); UiToast("Scores cleared",C_WARN); } }
  { int ry=ROWY;
    if (ry>TOP-RH&&ry<BOT)
      if (UiRow(X,ry,W,RH,"About NEXUS",NEXUS_VER_STR,true)){
        GoTo(ST_SYSTEM,160,120,C_ACCENT,TR_IRIS); return; } }
  #undef ROWY

  // inertial scroll over the whole list
  float contentH=(float)(y+(int)scr-contentTop)+10.0f;
  float viewH=(float)(BOT-TOP);
  float maxS=fmaxf(0.0f,contentH-viewH);
  if (touchActive&&touchY>TOP){
    if (!drag){ drag=true; dragY=touchY; }
    else { int d=touchY-dragY;
           if (abs(d)>2){ scr-=d; scrV=-d/fmaxf(dt,0.004f)*0.02f; dragY=touchY; } }
  } else { drag=false; scr+=scrV; scrV*=powf(0.5f,dt/0.12f);
           if (fabsf(scrV)<0.4f) scrV=0; }
  if (scr<0){ scr+=(0-scr)*clampf(dt*14,0,1); scrV=0; }
  if (scr>maxS){ scr+=(maxS-scr)*clampf(dt*14,0,1); scrV=0; }
  if (maxS>1.0f){
    int bh=(int)(viewH*viewH/contentH);
    int by=TOP+(int)((viewH-bh)*scr/maxS);
    UiRect(SCREEN_W-4,by,2,bh,C_SURFACE2,255); }

  TopBar("Settings",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  SYSTEM
// =====================================================================
static void StatRow(int x,int &y,const char *k,const char *v){
  DrawText(x,y,k,C_SAND,1);
  DrawText(x+62,y,v,C_DATA,1);
  y+=12;
}
void ScreenSystem(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  Backdrop();
  Panel(4,22,152,130,"HARDWARE",C_ACCENT,"S3");
  char b[32]; int y=44;
  snprintf(b,sizeof(b),"%d MHZ",(int)getCpuFrequencyMhz());          StatRow(10,y,"CPU",b);
  snprintf(b,sizeof(b),"%d MB",(int)(ESP.getFlashChipSize()/1048576)); StatRow(10,y,"FLASH",b);
  snprintf(b,sizeof(b),"%d MB",(int)(ESP.getPsramSize()/1048576));   StatRow(10,y,"PSRAM",b);
  snprintf(b,sizeof(b),"%d KB",(int)(ESP.getFreeHeap()/1024));       StatRow(10,y,"HEAP",b);
  snprintf(b,sizeof(b),"%d%%",(gBright*100)/255);                    StatRow(10,y,"BLIGHT",b);
  snprintf(b,sizeof(b),"%s",THEMES[gTheme].name);                    StatRow(10,y,"THEME",b);
  snprintf(b,sizeof(b),"%s",NEXUS_VER_STR);                          StatRow(10,y,"BUILD",b);
  snprintf(b,sizeof(b),"%d",(int)vTop);                              StatRow(10,y,"VERTS",b);
  snprintf(b,sizeof(b),"%d",(int)tTop);                              StatRow(10,y,"TRIS",b);
  Panel(160,22,156,74,"PERFORMANCE",C_ACCENT,"RT");
  snprintf(b,sizeof(b),"%d",(int)(fpsValue+0.5f));
  GlowText(168,46,b,C_DATA,4,80);
  DrawText(168+TextW(b,4)+6,62,"FPS",C_SAND,1);
  snprintf(b,sizeof(b),"%d.%d MS",(int)frameMs,((int)(frameMs*10))%10);
  DrawText(240,46,b,C_DATA,1);
  DrawText(240,58,"FRAME",C_SAND,1);
  DrawText(240,74,gDepthInternal?"Z IN SRAM":"Z IN PSRAM",C_SAND,1);
  DrawText(240,84,gLowDetail?"LOW DETAIL":"FULL DETAIL",C_SAND,1);
  Panel(160,100,156,52,"FRAME TIME",C_ACCENT,0);
  Graph(164,118,148,30,histFrame,histHead,C_DATA);
  Panel(4,156,312,78,"TELEMETRY",C_ACCENT,"LIVE");
  Graph(8,176,150,40,histFps,histHead,C_DATA);
  DrawText(10,168,"FPS",C_SAND,1);
  Graph(162,176,150,40,histLoad,histHead,C_DATA);
  DrawText(164,168,"PSRAM",C_SAND,1);
  float ps=1.0f-(float)ESP.getFreePsram()/(float)ESP.getPsramSize();
  snprintf(b,sizeof(b),"PSRAM %d%%  FREE %d KB",
           (int)(ps*100),(int)(ESP.getFreePsram()/1024));
  DrawText(8,222,b,C_SAND,1);
  ApplyLight();
  RenderMesh(meshOcta,gTime*0.7f,gTime*1.1f,0,0.40f,296.0f,132.0f,3.0f,M_NEON,C_ACCENT);
  // --- hidden: hold the title bar for 1.6 s to reach the Animation Lab
  {
    static uint32_t hold=0;
    if (touchActive && touchY<20 && touchX>60 && touchX<150){
      if (!hold) hold=millis();
      else if (millis()-hold>1600){ hold=0;
        eggFound|=(1u<<EGG_MASCOT);
        GoTo(ST_ANIMLAB,105,10,C_HILITE,TR_IRIS); return; }
      // subtle tell: the title glows harder the longer you hold
      if (hold) Glow(105,9,C_HILITE,(uint8_t)clampf((millis()-hold)*0.12f,0,180),2.0f);
    } else hold=0;
  }
  DrawParticles();
  TopBar("SYSTEM",C_ACCENT);
  EnterOverlay();
}


// #####################################################################
// #                                                                   #
// #            J E E   C O M M A N D   C E N T E R                    #
// #                                                                   #
// #  Interactive companion to the e-paper JEE tracker. The e-paper is #
// #  the calm glanceable dashboard; this is the editable, animated     #
// #  control surface. Same conceptual data model, different job.       #
// #                                                                   #
// #  PERSISTENCE RULES (flash longevity):                              #
// #    * Live timer/second counters stay in RAM ONLY.                  #
// #    * Flash is written on meaningful edits (task add/edit/complete, #
// #      session end, goal change, settings) and at most once every    #
// #      JEE_SAVE_MIN_MS, coalesced through a dirty flag.              #
// #    * Blob carries magic + version + CRC; a bad blob resets only    #
// #      the JEE data, never the whole device.                         #
// #####################################################################

JeeBlob *JB = nullptr;               // lives in PSRAM
static bool     jeeDirty = false;
static uint32_t jeeLastSave = 0;

inline JeeDay &JeeToday(void){ return JB->day[JB->dayHead % JEE_DAYS]; }
void JeeMark(void){ jeeDirty = true; }

uint32_t JeeCRC(const JeeBlob *b){
  // simple FNV-1a over everything after the crc field
  const uint8_t *p = (const uint8_t *)b + offsetof(JeeBlob, targetMin);
  size_t n = sizeof(JeeBlob) - offsetof(JeeBlob, targetMin);
  uint32_t h = 2166136261u;
  for (size_t i=0;i<n;i++){ h ^= p[i]; h *= 16777619u; }
  return h;
}
void JeeDefaults(void){
  memset(JB, 0, sizeof(JeeBlob));
  JB->magic = JEE_MAGIC; JB->ver = JEE_VER;
  JB->targetMin = 300;                 // 5 h
  JB->focusMin = 25; JB->shortMin = 5; JB->longMin = 15; JB->cycles = 4;
  JB->lastSub = SUB_MAT; JB->lastPri = PRI_MED;
  // a few starter tasks so the first run is not an empty void
  const char *t0="COMPLETE TODAYS DPP";
  const char *t1="REVISE THERMODYNAMICS";
  const char *t2="30 PARABOLA QUESTIONS";
  snprintf(JB->task[0].title,JEE_TITLE,"%s",t0);
  JB->task[0].subject=SUB_PHY; JB->task[0].priority=PRI_HIGH;
  JB->task[0].estMin=45; JB->task[0].used=1;
  snprintf(JB->task[1].title,JEE_TITLE,"%s",t1);
  JB->task[1].subject=SUB_CHE; JB->task[1].priority=PRI_MED;
  JB->task[1].estMin=60; JB->task[1].used=1;
  snprintf(JB->task[2].title,JEE_TITLE,"%s",t2);
  JB->task[2].subject=SUB_MAT; JB->task[2].priority=PRI_HIGH;
  JB->task[2].estMin=45; JB->task[2].used=1;
  snprintf(JB->goal[0].title,JEE_TITLE,"STUDY 5 HOURS TODAY");
  JB->goal[0].kind=0; JB->goal[0].target=300; JB->goal[0].isTime=1;
  JB->goal[0].subject=SUB_OTH; JB->goal[0].used=1;
  snprintf(JB->goal[1].title,JEE_TITLE,"50 MATHS QUESTIONS");
  JB->goal[1].kind=1; JB->goal[1].target=50; JB->goal[1].isTime=0;
  JB->goal[1].subject=SUB_MAT; JB->goal[1].used=1;
  snprintf(JB->note[0].title,28,"PARABOLA");
  snprintf(JB->note[0].body,JEE_BODY,
           "FOR Y2=4AX THE NORMAL AT T IS Y+TX=2AT+AT3");
  JB->note[0].subject=SUB_MAT; JB->note[0].used=1;
}
void JeeSave(void){
  if (!JB) return;
  JB->crc = JeeCRC(JB);
  prefs.begin(NVS_NS,false);
  // NVS caps a single entry well below our 5.8 KB blob, so it is stored as
  // four parts. Each part is small enough to be written atomically.
  prefs.putBytes("jHdr", JB, offsetof(JeeBlob,task));
  prefs.putBytes("jTsk", JB->task, sizeof(JB->task));
  prefs.putBytes("jNot", JB->note, sizeof(JB->note));
  prefs.putBytes("jGlD", JB->goal, sizeof(JB->goal)+sizeof(JB->day));
  prefs.end();
  jeeDirty=false; jeeLastSave=millis();
  Serial.printf("JEE saved (%u B in 4 parts)\n",(unsigned)sizeof(JeeBlob));
}
void JeeLoad(void){
  prefs.begin(NVS_NS,true);
  size_t hn = prefs.getBytesLength("jHdr");
  size_t tn = prefs.getBytesLength("jTsk");
  size_t nn = prefs.getBytesLength("jNot");
  size_t gn = prefs.getBytesLength("jGlD");
  bool ok=false;
  if (hn==offsetof(JeeBlob,task) && tn==sizeof(JB->task) &&
      nn==sizeof(JB->note) && gn==sizeof(JB->goal)+sizeof(JB->day)){
    prefs.getBytes("jHdr", JB, hn);
    prefs.getBytes("jTsk", JB->task, tn);
    prefs.getBytes("jNot", JB->note, nn);
    prefs.getBytes("jGlD", JB->goal, gn);
    if (JB->magic==JEE_MAGIC && JB->ver==JEE_VER && JB->crc==JeeCRC(JB)) ok=true;
    else Serial.println("JEE blob failed magic/ver/crc -- resetting JEE data only");
  } else if (hn||tn||nn||gn) {
    Serial.println("JEE blob size mismatch -- resetting JEE data only"); }
  prefs.end();
  if (!ok) { JeeDefaults(); JeeSave(); }
  else Serial.println("JEE data loaded");
  // bounds-check everything we will index with
  if (JB->dayHead>=JEE_DAYS) JB->dayHead=0;
  if (JB->targetMin<30||JB->targetMin>1440) JB->targetMin=300;
  if (JB->focusMin<1||JB->focusMin>180) JB->focusMin=25;
  if (JB->shortMin<1||JB->shortMin>60) JB->shortMin=5;
  if (JB->longMin<1||JB->longMin>90) JB->longMin=15;
  if (JB->cycles<1||JB->cycles>12) JB->cycles=4;
  if (JB->lastSub>=SUB_N) JB->lastSub=0;
  if (JB->lastPri>2) JB->lastPri=1;
  for (int i=0;i<JEE_TASKS;i++){
    JB->task[i].title[JEE_TITLE-1]=0;
    if (JB->task[i].subject>=SUB_N) JB->task[i].subject=0;
    if (JB->task[i].priority>2) JB->task[i].priority=1;
    if (JB->task[i].estMin>600) JB->task[i].estMin=45; }
  for (int i=0;i<JEE_NOTES;i++){
    JB->note[i].title[27]=0; JB->note[i].body[JEE_BODY-1]=0;
    if (JB->note[i].subject>=SUB_N) JB->note[i].subject=0; }
  for (int i=0;i<JEE_GOALS;i++){
    JB->goal[i].title[JEE_TITLE-1]=0;
    if (JB->goal[i].subject>=SUB_N) JB->goal[i].subject=0;
    if (JB->goal[i].kind>2) JB->goal[i].kind=0;
    if (JB->goal[i].progress>JB->goal[i].target)
      JB->goal[i].progress=JB->goal[i].target; }
}
// Called every frame: coalesces writes so flash is never hammered.
void JeeTick(float dt){
  if (jeeDirty && millis()-jeeLastSave > JEE_SAVE_MIN_MS) JeeSave();
}
// Roll the day ring when the calendar date changes (needs NTP).
void JeeRollDay(void){
  if (!timeOk) return;
  struct tm t;
  if (!getLocalTime(&t,0)) return;
  uint32_t stamp = (uint32_t)((t.tm_year+1900)*10000 + (t.tm_mon+1)*100 + t.tm_mday);
  if (JB->lastDayStamp==0){ JB->lastDayStamp=stamp; JeeMark(); return; }
  if (stamp==JB->lastDayStamp) return;
  // streak: only continues if yesterday hit at least half the target
  JeeDay &y = JeeToday();
  bool hit = (y.studyMin*2 >= JB->targetMin);
  if (hit){ JB->streak++; if (JB->streak>JB->bestStreak) JB->bestStreak=JB->streak; }
  else JB->streak = 0;
  JB->dayHead = (JB->dayHead+1) % JEE_DAYS;
  memset(&JeeToday(), 0, sizeof(JeeDay));
  // carry incomplete tasks forward; clear completed ones
  for (int i=0;i<JEE_TASKS;i++)
    if (JB->task[i].used && JB->task[i].done){ JB->task[i].used=0; }
  JB->lastDayStamp = stamp;
  JeeMark(); JeeSave();
  Serial.printf("JEE day rolled -> %u  streak %u\n",(unsigned)stamp,(unsigned)JB->streak);
}
void JeeInit(void){
  bool inPsram=true;
  JB = (JeeBlob *)heap_caps_malloc(sizeof(JeeBlob), MALLOC_CAP_SPIRAM);
  if (!JB){ inPsram=false; JB = (JeeBlob *)malloc(sizeof(JeeBlob)); }
  if (!JB){ Serial.println("JEE alloc FAILED"); return; }
  JeeLoad();
  Serial.printf("JEE blob %u bytes in %s\n",(unsigned)sizeof(JeeBlob),
                inPsram?"PSRAM":"internal heap");
}

// ---- live (RAM-only) session state -----------------------------------
enum { TM_FOCUS=0, TM_SHORT, TM_LONG, TM_CUSTOM };
static const char *TMODE[4] = { "FOCUS","SHORT BREAK","LONG BREAK","CUSTOM" };
static uint8_t  jtMode = TM_FOCUS;
static bool     jtRun = false;
static uint32_t jtEndMs = 0, jtLeftMs = 0, jtTotalMs = 0;
static uint32_t jtAccumSec = 0;         // seconds credited this session
static uint32_t jtLastCredit = 0;
static uint8_t  jtSubject = SUB_MAT;
static uint8_t  jtCycle = 0;
static char     jtTopic[28] = "";
static int      jtCustomMin = 45;

uint32_t JeeTodayMin(void){ return JB ? JeeToday().studyMin : 0; }
uint16_t JeeStreak(void){ return JB ? JB->streak : 0; }
float JeeProgress(void){
  if (!JB||!JB->targetMin) return 0;
  return clampf((float)JeeToday().studyMin/(float)JB->targetMin,0,1);
}
int JeeTaskCount(bool *doneOut){
  int total=0,done=0;
  for (int i=0;i<JEE_TASKS;i++) if (JB->task[i].used){ total++; if (JB->task[i].done) done++; }
  if (doneOut) *doneOut=(total>0&&done==total);
  return (total<<8)|done;
}
// credit elapsed study time once per second, RAM only
void JeeCreditTime(void){
  if (!jtRun || jtMode!=TM_FOCUS) return;
  uint32_t now=millis();
  if (now-jtLastCredit < 1000) return;
  uint32_t secs=(now-jtLastCredit)/1000;
  jtLastCredit += secs*1000;
  jtAccumSec += secs;
  // roll whole minutes into the day record (still RAM; flash on session end)
  while (jtAccumSec>=60){
    jtAccumSec-=60;
    JeeDay &d=JeeToday();
    if (d.studyMin<65000) d.studyMin++;
    if (jtSubject<SUB_N && d.subMin[jtSubject]<65000) d.subMin[jtSubject]++;
    if (JB->totalMin<65000) JB->totalMin++;
    // daily time goals track automatically
    for (int i=0;i<JEE_GOALS;i++){
      JeeGoal &g=JB->goal[i];
      if (g.used&&g.isTime&&g.kind==0&&g.progress<g.target) g.progress++; }
  }
}
void JeeStartTimer(int mode){
  jtMode=mode;
  int mins = (mode==TM_FOCUS)?JB->focusMin:
             (mode==TM_SHORT)?JB->shortMin:
             (mode==TM_LONG )?JB->longMin : jtCustomMin;
  jtTotalMs=(uint32_t)mins*60000u;
  jtEndMs=millis()+jtTotalMs;
  jtLeftMs=jtTotalMs;
  jtRun=true; jtLastCredit=millis();
  JB->lastSub=jtSubject; JeeMark();
}
void JeeEndSession(bool completed){
  if (jtMode==TM_FOCUS){
    JeeCreditTime();
    if (completed){
      jtCycle++;
      SpawnBurst(160,110,44,C_ACCENT,200.0f,PK_SPARK); }
    JeeSave();                     // meaningful checkpoint -> write flash
  }
  jtRun=false; jtLeftMs=0;
}

// =====================================================================
//  FULL-SCREEN TOUCH KEYBOARD
// ---------------------------------------------------------------------
//  Purpose-built for 320x240, NOT a shrunken PC layout.
//    * 4 rows of 40 px-tall keys -> comfortable finger targets
//    * hitboxes are 2 px larger than the drawn key on every side, so
//      near-misses still register (visual size != hitbox)
//    * ABC / 123 / SYM modes, shift + caps lock, cursor keys
//    * suggestion chips for common JEE terms, tap to insert whole word
//    * per-key press animation with spring release
// =====================================================================
#define KB_MAX 120
static char  kbText[KB_MAX+1] = "";
static int   kbLen2 = 0, kbCur = 0;
static char  kbTitle[24] = "";
static int   kbMax = 40;
static int   kbRet = ST_JEE;        // state to return to
static int   kbPurpose = 0;         // what the caller wants done
static bool  kbShiftOn = false, kbCaps = false;
static int   kbLayer = 0;           // 0 abc 1 num 2 sym
static int   kbHeld = -1;
static float kbHeldT = 0;
static uint32_t kbRepeat = 0;

enum { KBP_TASK=0, KBP_NOTE_T, KBP_NOTE_B, KBP_GOAL, KBP_TOPIC,
       KBP_EQ, KBP_SEARCH };
int kbGraphSlot=0;

static const char *KB_L0[4] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", "" };
static const char *KB_L1[4] = { "1234567890", "-/:;()$&@", ".,?!'\"",  "" };
static const char *KB_L2[4] = { "[]{}#%^*+=", "_\\|~<>",   ".,?!",     "" };
// JEE quick-insert vocabulary
static const char *KB_SUGG[8] = {
  "PHYSICS","CHEMISTRY","MATHS","QUESTIONS","REVISION","MOCK TEST","DPP","NCERT" };

void KbOpen(const char *title,const char *init,int maxLen,int ret,int purpose){
  snprintf(kbTitle,sizeof(kbTitle),"%s",title);
  snprintf(kbText,sizeof(kbText),"%s",init?init:"");
  kbLen2=strlen(kbText);
  kbMax=clampi(maxLen,1,KB_MAX);
  if (kbLen2>kbMax){ kbLen2=kbMax; kbText[kbLen2]=0; }
  kbCur=kbLen2;
  kbRet=ret; kbPurpose=purpose;
  kbShiftOn=false; kbLayer=0; kbHeld=-1;
  appState=ST_KBD; enterAnim=0;
}
void KbInsert(char c){
  if (kbLen2>=kbMax) return;
  for (int i=kbLen2;i>kbCur;i--) kbText[i]=kbText[i-1];
  kbText[kbCur]=c; kbLen2++; kbCur++; kbText[kbLen2]=0;
}
void KbBackspace(void){
  if (kbCur<=0) return;
  for (int i=kbCur-1;i<kbLen2;i++) kbText[i]=kbText[i+1];
  kbLen2--; kbCur--; kbText[kbLen2]=0;
}
void KbInsertWord(const char *w){
  if (kbCur>0 && kbText[kbCur-1]!=' ' && kbLen2<kbMax) KbInsert(' ');
  while (*w && kbLen2<kbMax) KbInsert(*w++);
}
// commit the typed text to whatever asked for it
static int kbTaskIdx=-1, kbNoteIdx=-1, kbGoalIdx=-1;
void KbCommit(void){
  // v7 purposes do not touch the JEE blob, so they run before the JB guard
  if (kbPurpose==KBP_EQ){     KbCommitEq(kbText);     return; }
  if (kbPurpose==KBP_SEARCH){ KbCommitSearch(kbText); return; }
  if (!JB) return;
  switch (kbPurpose){
    case KBP_TASK:
      if (kbTaskIdx>=0&&kbTaskIdx<JEE_TASKS&&kbLen2>0){
        snprintf(JB->task[kbTaskIdx].title,JEE_TITLE,"%s",kbText);
        JB->task[kbTaskIdx].used=1; JeeMark(); JeeSave(); }
      break;
    case KBP_NOTE_T:
      if (kbNoteIdx>=0&&kbNoteIdx<JEE_NOTES){
        snprintf(JB->note[kbNoteIdx].title,28,"%s",kbText);
        JB->note[kbNoteIdx].used=1; JeeMark(); JeeSave(); }
      break;
    case KBP_NOTE_B:
      if (kbNoteIdx>=0&&kbNoteIdx<JEE_NOTES){
        snprintf(JB->note[kbNoteIdx].body,JEE_BODY,"%s",kbText);
        JB->note[kbNoteIdx].used=1; JeeMark(); JeeSave(); }
      break;
    case KBP_GOAL:
      if (kbGoalIdx>=0&&kbGoalIdx<JEE_GOALS&&kbLen2>0){
        snprintf(JB->goal[kbGoalIdx].title,JEE_TITLE,"%s",kbText);
        JB->goal[kbGoalIdx].used=1; JeeMark(); JeeSave(); }
      break;
    case KBP_TOPIC:
      snprintf(jtTopic,sizeof(jtTopic),"%s",kbText);
      break; }
}
// one key: forgiving hitbox, spring press, glow
bool KbKey(int x,int y,int w,int h,const char *lbl,uint16_t c,bool active,int id){
  const int PAD=2;                       // hitbox grows, visual does not
  bool hit = touchDown && touchX>=x-PAD && touchX<x+w+PAD &&
                          touchY>=y-PAD && touchY<y+h+PAD;
  bool hov = touchActive && touchX>=x-PAD && touchX<x+w+PAD &&
                            touchY>=y-PAD && touchY<y+h+PAD;
  float press = (kbHeld==id)?clampf(kbHeldT*7.0f,0,1):0;
  int inset=(int)(press*2);
  uint16_t fill = active?Dim(c,2,5):(hov?Dim(c,1,5):Dim(C_PANEL,5,5));
  BlendRectFB(x+inset,y+inset,w-inset*2,h-inset*2,fill,A_FILL);
  Bracket(x+inset,y+inset,w-inset*2,h-inset*2,hov?c:Dim(c,3,5),4);
  if (hov||active){ Glow(x+w/2,y+h/2,c,(uint8_t)(60+70*press),1.3f);
                    HLineFB(x+2,y,w-4,c); }
  DrawTextC(x+w/2,y+(h-7)/2,lbl,(hov||active)?C_TEXT:C_SAND,1);
  if (hit){ kbHeld=id; kbHeldT=0;
    SpawnBurst(x+w/2,y+h/2,4,c,60.0f,PK_SPARK); }
  return hit;
}
void ScreenKeyboard(float dt){
  enterAnim=clampf(enterAnim+dt*3.0f,0,1);
  kbHeldT+=dt;
  if (!touchActive) kbHeld=-1;
  Backdrop();

  // ---------- text field ----------
  Panel(2,2,316,42,0,C_ACCENT,0);
  DrawText(8,7,kbTitle,C_ACCENT,1);
  { char cnt[16]; snprintf(cnt,sizeof(cnt),"%d/%d",kbLen2,kbMax);
    DrawText(316-TextW(cnt,1)-6,7,cnt,kbLen2>=kbMax?C_WARN:C_SAND,1); }
  // text with a visible caret, scrolled so the caret is always on screen
  int maxCh=50;
  int start=0;
  if (kbCur>maxCh) start=kbCur-maxCh;
  char win[52];
  int n=0;
  for (int i=start;i<kbLen2&&n<maxCh;i++) win[n++]=kbText[i];
  win[n]=0;
  DrawText(8,24,win,C_TEXT,1);
  if ((millis()/450)%2==0){
    int cx=8+(kbCur-start)*6;
    VLineFB(cx,22,11,C_HILITE);
    Glow(cx,27,C_HILITE,90,0.5f); }

  // ---------- suggestion chips ----------
  { int sx=4;
    for (int i=0;i<8;i++){
      int w=TextW(KB_SUGG[i],1)+10;
      if (sx+w>316) break;
      bool hit=touchDown&&touchX>=sx&&touchX<sx+w&&touchY>=46&&touchY<64;
      BlendRectFB(sx,46,w,18,Dim(Spec(i),1,6),A_FILL);
      Bracket(sx,46,w,18,Dim(Spec(i),3,5),3);
      DrawText(sx+5,51,KB_SUGG[i],C_SAND,1);
      if (hit){ KbInsertWord(KB_SUGG[i]);
        SpawnBurst(sx+w/2,55,8,Spec(i),80.0f,PK_SPARK); }
      sx+=w+3; } }

  // ---------- key rows ----------
  const char **L = (kbLayer==0)?KB_L0:((kbLayer==1)?KB_L1:KB_L2);
  const int KY=68, KH=38, GAP=2;
  int id=0;
  for (int r=0;r<3;r++){
    int len=strlen(L[r]);
    if (!len) continue;
    int kw=(316-(len-1)*GAP)/len;
    if (kw>32) kw=32;
    int ox=(320-(len*kw+(len-1)*GAP))/2;
    // row 3 leaves room for shift + backspace
    if (r==2){ ox=44; }
    for (int i=0;i<len;i++){
      char raw=L[r][i];
      char show=raw;
      if (kbLayer==0) show=(kbShiftOn||kbCaps)?raw:(char)(raw+32);
      char lbl[2]={show,0};
      int x=ox+i*(kw+GAP), y=KY+r*(KH+GAP);
      if (KbKey(x,y,kw,KH,lbl,C_ACCENT,false,id++)){
        KbInsert(show);
        if (kbShiftOn&&!kbCaps) kbShiftOn=false; } } }

  // row 3 extras: SHIFT (left) and BACKSPACE (right)
  { int y=KY+2*(KH+GAP);
    if (KbKey(2,y,40,KH,kbCaps?"CAPS":"SHFT",kbCaps?C_HILITE:C_ACCENT,
              kbShiftOn||kbCaps,900)){
      if (kbShiftOn&&!kbCaps){ kbCaps=true; kbShiftOn=false; }
      else if (kbCaps) kbCaps=false;
      else kbShiftOn=true; }
    if (KbKey(262,y,56,KH,"DEL",C_WARN,false,901)) KbBackspace(); }

  // ---------- bottom action row ----------
  { int y=KY+3*(KH+GAP);
    const char *lay = (kbLayer==0)?"123":((kbLayer==1)?"SYM":"ABC");
    if (KbKey(2,y,40,KH,lay,C_DATA,false,902)) kbLayer=(kbLayer+1)%3;
    if (KbKey(44,y,26,KH,"<",C_DATA,false,903)) { if (kbCur>0) kbCur--; }
    if (KbKey(72,y,26,KH,">",C_DATA,false,904)) { if (kbCur<kbLen2) kbCur++; }
    if (KbKey(100,y,96,KH,"SPACE",C_ACCENT,false,905)) KbInsert(' ');
    if (KbKey(198,y,50,KH,"CLR",C_WARN,false,906)){
      kbText[0]=0; kbLen2=0; kbCur=0; }
    if (KbKey(250,y,68,KH,"SAVE",C_HILITE,false,907)){
      KbCommit();
      appState=(AppState)kbRet; enterAnim=0;
      SpawnBurst(284,y+KH/2,20,C_HILITE,160.0f,PK_SPARK);
      DrawParticles(); return; } }

  // cancel via the title bar area
  if (touchDown&&touchY<20&&touchX>270){ appState=(AppState)kbRet; enterAnim=0; }
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE SHARED CHROME  --  nav bar with BACK / HOME / QUICK-ADD
// =====================================================================
static int  jeeQuick = 0;          // quick-add sheet: 0 closed
static int  jeeSubSel = SUB_MAT;   // subject picker value
static float jeeScroll = 0, jeeScrollV = 0;
static bool  jeeDrag = false;
static int   jeeDragY0 = 0;
static float jeeDragS0 = 0;
static uint32_t jeeDragT0 = 0;

// Returns true if the caller should abort drawing (navigated away).
bool JeeBar(const char *title,uint16_t ac){
  BlendRectFB(0,0,SCREEN_W,18,C_PANEL,A_FILL);
  HLineFB(0,18,SCREEN_W,Dim(ac,2,5));
  Glow(SCREEN_W/2,18,ac,40,2.0f);
  BlendRectFB(2,1,44,15,Dim(ac,1,5),A_FILL);
  Bracket(2,1,44,15,ac,5);
  DrawText(7,5,"< JEE",C_TEXT,1);
  GlowText(54,5,title,ac,1,70);
  // live mini summary, always visible
  if (JB){
    char s[26];
    snprintf(s,sizeof(s),"%dH%02d  %d%%",
             JeeTodayMin()/60,JeeTodayMin()%60,(int)(JeeProgress()*100));
    DrawText(SCREEN_W-TextW(s,1)-30,5,s,C_DATA,1); }
  // quick add
  BlendRectFB(SCREEN_W-26,1,24,15,Dim(C_HILITE,1,5),A_FILL);
  Bracket(SCREEN_W-26,1,24,15,C_HILITE,4);
  DrawText(SCREEN_W-19,5,"+",C_HILITE,1);
  if (touchDown&&touchX>SCREEN_W-30&&touchY<20){ jeeQuick=1; return false; }
  if (touchDown&&pressX<50&&pressY<20&&transT==0){
    GoTo(ST_JEE,24,10,C_ACCENT,TR_GLITCH); return true; }
  return false;
}
// Bottom section strip: jump straight between JEE sections.
void JeeTabs(int active){
  static const char *TN[9]={"HOME","TIME","TASK","GOAL","STAT","HIST","NOTE","QUOT","SET"};
  static const uint8_t TS[9]={ST_JEE,ST_JTIMER,ST_JTASKS,ST_JGOALS,ST_JSTATS,
                              ST_JHIST,ST_JNOTES,ST_JQUOTE,ST_JSET};
  int w=34;
  for (int i=0;i<9;i++){
    int x=2+i*35, y=SCREEN_H-14;
    bool on=(TS[i]==active);
    BlendRectFB(x,y,w,13,on?Dim(C_ACCENT,2,5):Dim(C_PANEL,5,5),A_FILL);
    if (on){ HLineFB(x,y,w,C_ACCENT); Glow(x+w/2,y+6,C_ACCENT,60,0.8f); }
    DrawTextC(x+w/2,y+3,TN[i],on?C_TEXT:C_HAIR,1);
    if (touchDown&&touchX>=x&&touchX<x+w&&touchY>=y&&transT==0&&!on){
      GoTo(TS[i],x+w/2,y,C_ACCENT,TR_HEX); } }
}

// Smooth inertial list scrolling shared by tasks/notes/history.
void JeeScrollUpdate(float dt,float contentH,float viewH,int topY){
  float maxS = fmaxf(0.0f, contentH-viewH);
  if (touchDown && touchY>topY){ jeeDrag=true; jeeDragY0=touchY;
    jeeDragS0=jeeScroll; jeeDragT0=millis(); jeeScrollV=0; }
  if (touchActive && jeeDrag){
    float d=(float)(jeeDragY0-touchY);
    float ns=jeeDragS0+d;
    jeeScrollV=(ns-jeeScroll)/fmaxf(dt,0.001f)*0.35f;
    jeeScroll=ns; }
  if (touchUp) jeeDrag=false;
  if (!jeeDrag){
    jeeScroll+=jeeScrollV*dt;
    jeeScrollV*=expf(-4.0f*dt);
    if (fabsf(jeeScrollV)<4) jeeScrollV=0; }
  if (jeeScroll<0){ jeeScroll=Approach(jeeScroll,0,14.0f,dt); jeeScrollV=0; }
  if (jeeScroll>maxS){ jeeScroll=Approach(jeeScroll,maxS,14.0f,dt); jeeScrollV=0; }
}
void JeeScrollBar(int x,int y,int h,float contentH,float viewH){
  if (contentH<=viewH) return;
  float f=viewH/contentH;
  int bh=(int)(h*f); if (bh<12) bh=12;
  int by=y+(int)((h-bh)*clampf(jeeScroll/(contentH-viewH),0,1));
  BlendRectFB(x,y,2,h,C_HAIR,A_GLOW);
  BlendRectFB(x,by,2,bh,C_ACCENT,255);
  Glow(x+1,by+bh/2,C_ACCENT,60,0.6f);
}
// A ring progress indicator -- the JEE house style, not a fat bar.
void JeeRing(int cx,int cy,int r,float frac,uint16_t c,const char *big,const char *sub){
  RingFB(cx,cy,r,Dim(C_HAIR,4,5),A_FILL);
  RingFB(cx,cy,r-1,Dim(C_HAIR,3,5),A_GLOW);
  float a0=-1.5708f;
  ArcFB(cx,cy,r,a0,a0+TAU*clampf(frac,0,1),c,255);
  ArcFB(cx,cy,r-1,a0,a0+TAU*clampf(frac,0,1),c,A_FILL);
  // leading dot
  float ae=a0+TAU*clampf(frac,0,1);
  int dx=cx+(int)(fcos(ae)*r), dy=cy+(int)(fsin(ae)*r);
  CircleFB(dx,dy,2,C_TEXT,255);
  Glow(dx,dy,c,140,0.8f);
  if (big) GlowTextC(cx,cy-(sub?8:4),big,C_TEXT,2,70);
  if (sub) DrawTextC(cx,cy+8,sub,C_SAND,1);
}
// Quick-add sheet, reachable from every JEE screen.
bool JeeQuickSheet(void){
  if (!jeeQuick) return false;
  Scrim(180);
  Panel(40,44,240,152,"QUICK ADD",C_HILITE,0);
  int y=68;
  if (Button(52,y,216,26,"+ TASK",C_ACCENT,false)){
    for (int i=0;i<JEE_TASKS;i++) if (!JB->task[i].used){
      kbTaskIdx=i;
      JB->task[i].subject=JB->lastSub; JB->task[i].priority=JB->lastPri;
      JB->task[i].estMin=45; JB->task[i].done=0;
      jeeQuick=0;
      KbOpen("NEW TASK","",JEE_TITLE-1,ST_JTASKS,KBP_TASK);
      return true; }
    jeeQuick=0; }
  y+=30;
  if (Button(52,y,216,26,"+ STUDY SESSION",C_ACCENT,false)){
    jeeQuick=0; GoTo(ST_JTIMER,160,120,C_ACCENT,TR_HEX); return true; }
  y+=30;
  if (Button(52,y,216,26,"+ NOTE",C_ACCENT,false)){
    for (int i=0;i<JEE_NOTES;i++) if (!JB->note[i].used){
      kbNoteIdx=i; JB->note[i].subject=JB->lastSub; JB->note[i].body[0]=0;
      jeeQuick=0;
      KbOpen("NOTE TITLE","",27,ST_JNOTES,KBP_NOTE_T);
      return true; }
    jeeQuick=0; }
  y+=30;
  if (Button(52,y,216,26,"+ GOAL",C_ACCENT,false)){
    for (int i=0;i<JEE_GOALS;i++) if (!JB->goal[i].used){
      kbGoalIdx=i; JB->goal[i].kind=0; JB->goal[i].target=60;
      JB->goal[i].isTime=1; JB->goal[i].progress=0;
      JB->goal[i].subject=JB->lastSub;
      jeeQuick=0;
      KbOpen("NEW GOAL","",JEE_TITLE-1,ST_JGOALS,KBP_GOAL);
      return true; }
    jeeQuick=0; }
  if (Button(52,178,216,14,"CANCEL",C_WARN,false)) jeeQuick=0;
  return true;
}

// =====================================================================
//  JEE HOME  --  the dashboard
// =====================================================================
static Spring jeeCard[6];
static float  jeeCount = 0;        // animated study-minute counter

void ScreenJee(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  JeeRollDay();

  if (JeeQuickSheet()){ /* sheet eats input */ }
  else if (BackHit()) return;

  jeeCount=Approach(jeeCount,(float)JeeTodayMin(),6.0f,dt);
  Backdrop();

  // =================================================================
  //  TODAY  --  progressive disclosure.
  //  Primary   : tasks done + progress bar
  //  Secondary : study time, streak
  //  Tertiary  : everything else, behind the nav rows below.
  //  The old screen showed a ring, two big cards, a subject split, a
  //  7-day chart and six coloured tiles simultaneously; that is the
  //  dashboard density the redesign removes.
  // =================================================================
  const int X=GUTTER, W=CONTENT_W;
  int y=CONTENT_TOP;

  int packed=JeeTaskCount(nullptr);
  int total=packed>>8, done=packed&0xFF;

  UiSection(X,y,"Today"); y+=15;

  // PRIMARY: the one number that matters, set large.
  { char big[16];
    snprintf(big,sizeof(big),"%d / %d",done,total);
    DrawText(X,y,big,C_TEXT,T_TITLE);
    DrawText(X+TextW(big,T_TITLE)+8,y+6,"tasks",C_DIM,T_BODY);
    y+=22; }

  // progress bar -- the single strongest visual on the screen
  { float f=(total>0)?(float)done/total:0.0f;
    f*=EaseOutCubic(enterAnim);
    UiRect(X,y,W,4,C_SURFACE2,255);
    UiRect(X,y,(int)(W*f),4,C_ACCENT,255);
    y+=16; }

  // SECONDARY: two quiet facts, typographic not boxed
  { uint32_t m=(uint32_t)jeeCount;
    char t[24]; snprintf(t,sizeof(t),"%uh %02um",(unsigned)(m/60),(unsigned)(m%60));
    DrawText(X,y,t,C_TEXT,T_BODY);
    DrawText(X,y+11,"studied",C_OFF,T_SMALL);
    char st[20]; snprintf(st,sizeof(st),"%u day",(unsigned)JB->streak);
    int sw=TextW(st,T_BODY);
    DrawText(X+W-sw,y,st,C_TEXT,T_BODY);
    const char *sl="streak";
    DrawText(X+W-TextW(sl,T_SMALL),y+11,sl,C_OFF,T_SMALL);
    y+=28; }

  BlendRectFB(X,y,W,1,C_LINE,A_HAIR); y+=8;

  // TERTIARY: destinations as plain rows. No colour, no boxes.
  { static const char *NM[5]={"Focus timer","Tasks","Goals","Statistics","Notes"};
    static const uint8_t NS[5]={ST_JTIMER,ST_JTASKS,ST_JGOALS,ST_JSTATS,ST_JNOTES};
    static const uint8_t NI[5]={IC_TIMER,IC_CHECK,IC_FLAG,IC_CHART,IC_NOTE};
    const int RH=24;
    for (int i=0;i<5;i++){
      int ry=y+i*RH;
      if (ry+RH>SCREEN_H-6) break;
      bool over=touchActive&&UiHit(X,ry,W,RH,0);
      if (over) UiRect(X-2,ry,W+4,RH,C_SURFACE,160);
      IconPack(NI[i],X+8,ry+RH/2,5,over?C_ACCENT:C_DIM,1);
      DrawText(X+22,ry+(RH-7)/2,NM[i],C_TEXT,T_BODY);
      PenChevron(X+W-6,ry+RH/2,4,1,1,over?C_ACCENT:C_OFF);
      if (touchDown&&over&&transT==0){
        GoTo(NS[i],160,ry+RH/2,C_ACCENT,TR_IRIS); return; } } }

  TopBar("Study",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  JEE TIMER  --  focus / break cycles with subject + topic
// =====================================================================
void ScreenJeeTimer(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }
  JeeCreditTime();
  if (jtRun){
    uint32_t now=millis();
    jtLeftMs=(now<jtEndMs)?(jtEndMs-now):0;
    if (jtLeftMs==0){
      bool wasFocus=(jtMode==TM_FOCUS);
      JeeEndSession(true);
      SpawnBurst(160,104,50,wasFocus?C_ACCENT:C_DATA,220.0f,PK_EMBER);
      // auto-advance: focus -> break -> focus
      if (wasFocus) jtMode=(jtCycle%JB->cycles==0)?TM_LONG:TM_SHORT;
      else jtMode=TM_FOCUS; } }

  Backdrop();
  if (JeeBar("TIMER",C_ACCENT)) return;

  // mode tabs
  for (int i=0;i<4;i++){
    int x=6+i*78;
    if (Button(x,22,74,20,TMODE[i],C_ACCENT,jtMode==i)){
      if (!jtRun){ jtMode=i;
        SpawnBurst(x+37,32,10,C_ACCENT,90.0f,PK_SPARK); } } }

  // ---- the dial ----
  uint32_t showMs = jtRun?jtLeftMs:
    (uint32_t)((jtMode==TM_FOCUS?JB->focusMin:
                jtMode==TM_SHORT?JB->shortMin:
                jtMode==TM_LONG ?JB->longMin : jtCustomMin)*60000u);
  int mm=(showMs/60000)%100, ss=(showMs/1000)%60;
  float frac = jtRun&&jtTotalMs ? (float)jtLeftMs/(float)jtTotalMs : 1.0f;
  uint16_t dc = (jtMode==TM_FOCUS)?C_ACCENT:C_DATA;
  if (jtRun&&jtLeftMs<30000) dc=C_WARN;

  int cx=88, cy=124;
  RingFB(cx,cy,52,Dim(C_HAIR,4,5),A_FILL);
  ArcFB(cx,cy,52,-1.5708f,-1.5708f+TAU*frac,dc,255);
  ArcFB(cx,cy,51,-1.5708f,-1.5708f+TAU*frac,dc,A_FILL);
  if (jtRun){
    float ae=-1.5708f+TAU*frac;
    int dx2=cx+(int)(fcos(ae)*52),dy2=cy+(int)(fsin(ae)*52);
    CircleFB(dx2,dy2,3,C_TEXT,255);
    Glow(dx2,dy2,dc,150,1.0f); }
  { char t[12]; snprintf(t,sizeof(t),"%02d:%02d",mm,ss);
    GlowTextC(cx,cy-10,t,C_TEXT,3,(uint8_t)(jtRun?90:60));
    DrawTextC(cx,cy+14,TMODE[jtMode],C_SAND,1); }

  // ---- session info ----
  Panel(150,46,164,72,"SESSION",C_ACCENT,0);
  DrawText(158,66,"SUBJECT",C_SAND,1);
  { uint16_t sc=SubCol(jtSubject);
    BlendRectFB(158,76,90,16,Dim(sc,2,5),A_FILL);
    Bracket(158,76,90,16,sc,4);
    DrawText(163,80,SUB_NAME[jtSubject],C_TEXT,1);
    if (touchDown&&touchX>=158&&touchX<248&&touchY>=76&&touchY<92&&!jtRun){
      jtSubject=(jtSubject+1)%SUB_N; JB->lastSub=jtSubject; JeeMark();
      SpawnBurst(203,84,10,sc,90.0f,PK_SPARK); } }
  if (Button(252,76,56,16,"TOPIC",C_ACCENT,false)){
    KbOpen("TOPIC",jtTopic,26,ST_JTIMER,KBP_TOPIC); return; }
  DrawText(158,96,jtTopic[0]?jtTopic:"(NO TOPIC SET)",jtTopic[0]?C_DATA:C_HAIR,1);
  { char c[24]; snprintf(c,sizeof(c),"CYCLE %d / %d",jtCycle%JB->cycles+1,JB->cycles);
    DrawText(158,108,c,C_SAND,1); }

  // ---- transport ----
  if (Button(150,124,78,30,jtRun?"PAUSE":"START",jtRun?C_WARN:C_ACCENT,jtRun)){
    if (jtRun){ JeeCreditTime(); jtRun=false;
      jtTotalMs=jtTotalMs; jtEndMs=millis()+jtLeftMs; JeeSave(); }
    else {
      if (jtLeftMs>0&&jtLeftMs<jtTotalMs){ jtEndMs=millis()+jtLeftMs; jtRun=true;
        jtLastCredit=millis(); }
      else JeeStartTimer(jtMode); }
    SpawnBurst(189,139,16,C_ACCENT,130.0f,PK_SPARK); }
  if (Button(232,124,78,30,"RESET",C_WARN,false)){
    JeeEndSession(false); jtLeftMs=0; jtTotalMs=0;
    SpawnBurst(271,139,16,C_WARN,130.0f,PK_SPARK); }

  // ---- durations (only editable when stopped) ----
  Panel(150,160,164,52,"DURATIONS",C_ACCENT,0);
  { int y=180;
    DrawText(156,y,"F",C_SAND,1);
    if (Button(166,y-4,20,16,"-",C_ACCENT,false)&&!jtRun&&JB->focusMin>1){ JB->focusMin--; JeeMark(); }
    { char v[6]; snprintf(v,sizeof(v),"%u",(unsigned)JB->focusMin);
      DrawTextC(196,y,v,C_DATA,1); }
    if (Button(206,y-4,20,16,"+",C_ACCENT,false)&&!jtRun&&JB->focusMin<180){ JB->focusMin++; JeeMark(); }
    DrawText(234,y,"S",C_SAND,1);
    if (Button(244,y-4,18,16,"-",C_ACCENT,false)&&!jtRun&&JB->shortMin>1){ JB->shortMin--; JeeMark(); }
    { char v[6]; snprintf(v,sizeof(v),"%u",(unsigned)JB->shortMin);
      DrawTextC(272,y,v,C_DATA,1); }
    if (Button(282,y-4,18,16,"+",C_ACCENT,false)&&!jtRun&&JB->shortMin<60){ JB->shortMin++; JeeMark(); }
    y+=20;
    DrawText(156,y,"L",C_SAND,1);
    if (Button(166,y-4,20,16,"-",C_ACCENT,false)&&!jtRun&&JB->longMin>1){ JB->longMin--; JeeMark(); }
    { char v[6]; snprintf(v,sizeof(v),"%u",(unsigned)JB->longMin);
      DrawTextC(196,y,v,C_DATA,1); }
    if (Button(206,y-4,20,16,"+",C_ACCENT,false)&&!jtRun&&JB->longMin<90){ JB->longMin++; JeeMark(); }
    DrawText(234,y,"C",C_SAND,1);
    if (Button(244,y-4,18,16,"-",C_ACCENT,false)&&!jtRun&&JB->cycles>1){ JB->cycles--; JeeMark(); }
    { char v[6]; snprintf(v,sizeof(v),"%u",(unsigned)JB->cycles);
      DrawTextC(272,y,v,C_DATA,1); }
    if (Button(282,y-4,18,16,"+",C_ACCENT,false)&&!jtRun&&JB->cycles<12){ JB->cycles++; JeeMark(); } }

  // today's credited total
  { char s[32]; snprintf(s,sizeof(s),"TODAY %02u:%02u",
      (unsigned)(JeeTodayMin()/60),(unsigned)(JeeTodayMin()%60));
    GlowText(12,218,s,C_DATA,1,60); }
  DrawText(150,218,jtRun?"SESSION RUNNING - TIME IS BEING LOGGED":"START TO LOG STUDY TIME",
           jtRun?C_ACCENT:C_SAND,1);
  JeeTabs(ST_JTIMER);
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE TASKS  --  scrolling list, inline complete, edit, priority
// =====================================================================
static int jeeTaskSel=-1;
static int jeeTaskMenu=-1;

void ScreenJeeTasks(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }

  // build a visible index so completed tasks sink to the bottom
  int order[JEE_TASKS], n=0;
  for (int pass=0;pass<2;pass++)
    for (int i=0;i<JEE_TASKS;i++)
      if (JB->task[i].used && ((pass==0)!=(bool)JB->task[i].done)) order[n++]=i;

  const int TOP=42, ROW=34, VIEW=SCREEN_H-TOP-6;
  float contentH=n*ROW+4;
  if (jeeTaskMenu<0) JeeScrollUpdate(dt,contentH,VIEW,TOP);

  Backdrop();
  if (JeeBar("TASKS",C_ACCENT)) return;

  // summary strip
  { int packed=JeeTaskCount(nullptr);
    int total=packed>>8, done=packed&0xFF;
    char s[28]; snprintf(s,sizeof(s),"%d OF %d COMPLETE",done,total);
    DrawText(8,24,s,C_DATA,1);
    int bw=(int)(180.0f*(total?(float)done/total:0));
    HLineFB(130,28,180,C_HAIR);
    FillRectFB(130,28,bw,2,C_ACCENT);
    if (bw>2) Glow(130+bw,29,C_HILITE,110,0.6f); }

  if (!n){
    DrawTextC(160,110,"NO TASKS YET",C_SAND,2);
    DrawTextC(160,134,"TAP + TO ADD ONE",C_HAIR,1);
  } else {
    for (int k=0;k<n;k++){
      int i=order[k];
      int y=TOP+k*ROW-(int)jeeScroll;
      if (y<TOP-ROW||y>SCREEN_H) continue;
      JeeTask &t=JB->task[i];
      float d=Stagger(enterAnim,k,0.03f,0.4f);
      int x=8+(int)((1.0f-d)*24);
      uint16_t sc=SubCol(t.subject);
      // row body
      BlendRectFB(x,y,304,ROW-4,t.done?Dim(C_PANEL,4,5):C_PANEL,(uint8_t)(A_FILL*d));
      Bracket(x,y,304,ROW-4,t.done?Dim(C_HAIR,4,5):Dim(sc,3,5),5);
      // subject colour spine
      BlendRectFB(x,y,3,ROW-4,t.done?Dim(sc,2,5):sc,255);
      // checkbox
      int bx=x+12,by=y+7;
      RingFB(bx+8,by+8,9,t.done?C_ACCENT:Dim(C_HAIR,5,5),255);
      if (t.done){
        LineFB(bx+4,by+8,bx+7,by+12,C_ACCENT,255);
        LineFB(bx+7,by+12,bx+13,by+3,C_ACCENT,255);
        Glow(bx+8,by+8,C_ACCENT,80,0.9f); }
      // title
      DrawText(x+34,y+6,t.title,t.done?C_HAIR:C_TEXT,1);
      // meta line
      char meta[40];
      snprintf(meta,sizeof(meta),"%s  %s  %dM",
               SUB_SHORT[t.subject],PRI_NAME[t.priority],t.estMin);
      DrawText(x+34,y+18,meta,t.done?Dim(C_HAIR,4,5):C_SAND,1);
      // priority pip
      if (!t.done) for (int p=0;p<=t.priority;p++)
        CircleFB(x+292-p*7,y+9,2,PriCol(t.priority),255);
      // hit: left third toggles done, rest opens the row menu
      if (touchDown&&touchY>=y&&touchY<y+ROW-4&&touchY>TOP&&!jeeDrag){
        if (touchX<x+34){
          t.done=!t.done;
          if (t.done){
            JeeToday().tasksDone++;
            SpawnBurst(bx+8,by+8,16,C_ACCENT,130.0f,PK_SPARK);
            for (int g=0;g<JEE_GOALS;g++){
              JeeGoal &gg=JB->goal[g];
              if (gg.used&&!gg.isTime&&gg.progress<gg.target) gg.progress++; }
          } else if (JeeToday().tasksDone) JeeToday().tasksDone--;
          JeeMark(); JeeSave();
        } else { jeeTaskMenu=i; jeeTaskSel=i; } } } }

  JeeScrollBar(314,TOP,VIEW,contentH,VIEW);

  // ---- row action sheet ----
  if (jeeTaskMenu>=0&&jeeTaskMenu<JEE_TASKS){
    JeeTask &t=JB->task[jeeTaskMenu];
    Scrim(180);
    Panel(30,40,260,164,"TASK",C_HILITE,0);
    DrawText(40,60,t.title,C_TEXT,1);
    int y=76;
    { uint16_t sc=SubCol(t.subject);
      DrawText(40,y,"SUBJECT",C_SAND,1);
      if (Button(120,y-4,150,20,SUB_NAME[t.subject],sc,true)){
        t.subject=(t.subject+1)%SUB_N; JB->lastSub=t.subject; JeeMark(); } }
    y+=26;
    DrawText(40,y,"PRIORITY",C_SAND,1);
    if (Button(120,y-4,150,20,PRI_NAME[t.priority],PriCol(t.priority),true)){
      t.priority=(t.priority+1)%3; JB->lastPri=t.priority; JeeMark(); }
    y+=26;
    DrawText(40,y,"ESTIMATE",C_SAND,1);
    if (Button(120,y-4,40,20,"-",C_ACCENT,false)&&t.estMin>5){ t.estMin-=5; JeeMark(); }
    { char v[10]; snprintf(v,sizeof(v),"%d MIN",t.estMin);
      DrawTextC(200,y,v,C_DATA,1); }
    if (Button(230,y-4,40,20,"+",C_ACCENT,false)&&t.estMin<600){ t.estMin+=5; JeeMark(); }
    y+=28;
    if (Button(40,y,72,22,"EDIT",C_ACCENT,false)){
      kbTaskIdx=jeeTaskMenu; jeeTaskMenu=-1;
      KbOpen("EDIT TASK",t.title,JEE_TITLE-1,ST_JTASKS,KBP_TASK); return; }
    if (Button(118,y,72,22,"DELETE",C_WARN,false)){
      t.used=0; t.done=0; t.title[0]=0; jeeTaskMenu=-1;
      JeeMark(); JeeSave();
      SpawnBurst(154,y+11,20,C_WARN,140.0f,PK_SPARK); }
    if (Button(196,y,74,22,"CLOSE",C_DATA,false)) jeeTaskMenu=-1;
  }
  JeeTabs(ST_JTASKS);
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE GOALS
// =====================================================================
void ScreenJeeGoals(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }
  Backdrop();
  if (JeeBar("GOALS",C_ACCENT)) return;

  static const char *KIND[3]={"DAILY","WEEKLY","LONG TERM"};
  int n=0;
  for (int i=0;i<JEE_GOALS;i++) if (JB->goal[i].used) n++;
  if (!n){
    DrawTextC(160,110,"NO GOALS SET",C_SAND,2);
    DrawTextC(160,134,"TAP + TO ADD ONE",C_HAIR,1);
  } else {
    int row=0;
    for (int i=0;i<JEE_GOALS&&row<4;i++){
      JeeGoal &g=JB->goal[i];
      if (!g.used) continue;
      float d=Stagger(enterAnim,row,0.06f,0.45f);
      int y=26+row*50+(int)((1.0f-d)*20);
      uint16_t c=SubCol(g.subject);
      Panel(6,y,308,46,0,c,0);
      DrawText(14,y+7,g.title,C_TEXT,1);
      char meta[36];
      snprintf(meta,sizeof(meta),"%s  %s",KIND[g.kind],SUB_SHORT[g.subject]);
      DrawText(14,y+19,meta,C_SAND,1);
      // progress as a connected segment bar, not a slab
      float f=g.target?clampf((float)g.progress/g.target,0,1):0;
      int bx=14,bw=228;
      HLineFB(bx,y+34,bw,C_HAIR);
      int fw=(int)(bw*f*EaseOutCubic(enterAnim));
      FillRectFB(bx,y+33,fw,3,c);
      if (fw>2) Glow(bx+fw,y+34,C_HILITE,110,0.6f);
      for (int k=0;k<=4;k++){ int tx=bx+bw*k/4;
        VLineFB(tx,y+31,6,Dim(C_HAIR,5,5)); }
      char pv[24];
      if (g.isTime) snprintf(pv,sizeof(pv),"%u/%u MIN",(unsigned)g.progress,(unsigned)g.target);
      else          snprintf(pv,sizeof(pv),"%u/%u",(unsigned)g.progress,(unsigned)g.target);
      DrawText(250,y+19,pv,C_DATA,1);
      { char pc[8]; snprintf(pc,sizeof(pc),"%d%%",(int)(f*100));
        GlowText(250,y+6,pc,f>=1.0f?C_HILITE:C_TEXT,1,f>=1.0f?90:0); }
      if (f>=1.0f){ DrawText(250,y+32,"DONE",C_HILITE,1); }
      // interactions
      if (touchDown&&touchY>=y&&touchY<y+46){
        if (touchX>246){ g.kind=(g.kind+1)%3; JeeMark(); }
        else if (touchX<40){
          if (g.progress<g.target){ g.progress++; JeeMark(); JeeSave();
            SpawnBurst(30,y+23,10,c,90.0f,PK_SPARK); } }
        else if (touchX>200&&touchX<246){
          g.used=0; JeeMark(); JeeSave();
          SpawnBurst(223,y+23,18,C_WARN,130.0f,PK_SPARK); } }
      row++; } }
  DrawText(8,228,"TAP LEFT +1   MID DELETE   RIGHT CYCLE TYPE",C_HAIR,1);
  JeeTabs(ST_JGOALS);
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE STATS  --  analytical, point-connected, no fat bars
// =====================================================================
static int jeeRange = 0;   // 0 = 7d, 1 = 30d, 2 = 90d

void ScreenJeeStats(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }
  Backdrop();
  if (JeeBar("STATS",C_ACCENT)) return;

  static const int RNG[3]={7,30,90};
  static const char *RNM[3]={"7 DAYS","30 DAYS","3 MONTHS"};
  for (int i=0;i<3;i++)
    if (Button(6+i*74,22,70,18,RNM[i],C_ACCENT,jeeRange==i)){
      jeeRange=i; SpawnBurst(41+i*74,31,10,C_ACCENT,90.0f,PK_SPARK); }

  int N=RNG[jeeRange];
  // aggregate
  uint32_t total=0; int active=0, best=0;
  for (int i=0;i<N;i++){
    int idx=(JB->dayHead+JEE_DAYS-(N-1)+i)%JEE_DAYS;
    int m=JB->day[idx].studyMin;
    total+=m; if (m>0) active++;
    if (m>best) best=m; }
  if (best<60) best=60;

  // ---- the graph ----
  Panel(6,44,308,96,0,C_ACCENT,0);
  DrawText(12,48,"STUDY MINUTES",C_SAND,1);
  { char r[16]; snprintf(r,sizeof(r),"PEAK %d",best);
    DrawText(314-TextW(r,1)-6,48,r,C_DATA,1); }
  const int GX=16,GY=62,GW=290,GH=68;
  // horizontal guides
  for (int k=0;k<=3;k++){
    int y=GY+GH*k/3;
    for (int x=GX;x<GX+GW;x+=7) PxBlend(x,y,C_HAIR,90); }
  // target guide
  { int ty=GY+GH-(int)(GH*clampf((float)JB->targetMin/best,0,1));
    for (int x=GX;x<GX+GW;x+=4) PxBlend(x,ty,C_WARN,120);
    DrawText(GX+GW-30,ty-9,"GOAL",C_WARN,1); }
  // connected points; sample if the range is long
  { int stepN=(N>30)?(N/30):1;
    int px=-1,py=0;
    float ez=EaseOutCubic(enterAnim);
    for (int i=0;i<N;i+=stepN){
      int idx=(JB->dayHead+JEE_DAYS-(N-1)+i)%JEE_DAYS;
      int m=JB->day[idx].studyMin;
      int x=GX+(int)((float)i/(N-1)*GW);
      int y=GY+GH-(int)(GH*clampf((float)m/best,0,1)*ez);
      if (px>=0){
        LineFB(px,py,x,y,C_ACCENT,235);
        // soft underfill so the trend reads as area without a slab
        for (int fy=y+1;fy<GY+GH;fy+=3) PxBlend(x,fy,C_ACCENT,26); }
      px=x; py=y; }
    // points last
    for (int i=0;i<N;i+=stepN){
      int idx=(JB->dayHead+JEE_DAYS-(N-1)+i)%JEE_DAYS;
      int m=JB->day[idx].studyMin;
      int x=GX+(int)((float)i/(N-1)*GW);
      int y=GY+GH-(int)(GH*clampf((float)m/best,0,1)*ez);
      bool last=(i+stepN>=N);
      if (N<=30||last){
        CircleFB(x,y,last?3:2,last?C_HILITE:C_ACCENT,255);
        if (last) Glow(x,y,C_HILITE,(uint8_t)(120*Pulse(gTime,3.0f)),1.0f); } } }

  // ---- number cards ----
  { int avg = active?(int)(total/active):0;
    struct { const char *k; char v[14]; uint16_t c; } S[4];
    snprintf(S[0].v,14,"%02u:%02u",(unsigned)(total/60),(unsigned)(total%60)); S[0].k="TOTAL";  S[0].c=C_ACCENT;
    snprintf(S[1].v,14,"%02d:%02d",avg/60,avg%60);                            S[1].k="AVG/DAY";S[1].c=C_DATA;
    snprintf(S[2].v,14,"%u",(unsigned)JB->streak);                            S[2].k="STREAK"; S[2].c=C_WARN;
    snprintf(S[3].v,14,"%u",(unsigned)JB->bestStreak);                        S[3].k="BEST";   S[3].c=C_HILITE;
    for (int i=0;i<4;i++){
      int x=6+i*78;
      float d=Stagger(enterAnim,i,0.05f,0.4f);
      int y=146+(int)((1.0f-d)*14);
      BlendRectFB(x,y,74,40,C_PANEL,(uint8_t)(A_FILL*d));
      Bracket(x,y,74,40,Dim(S[i].c,3,5),5);
      GlowTextC(x+37,y+9,S[i].v,S[i].c,2,60);
      DrawTextC(x+37,y+29,S[i].k,C_SAND,1); } }

  // ---- completion + subject mix ----
  { int packed=JeeTaskCount(nullptr);
    int tot=packed>>8, done=packed&0xFF;
    Panel(6,190,150,44,0,C_ACCENT,0);
    DrawText(12,194,"TASK COMPLETION",C_SAND,1);
    float f=tot?(float)done/tot:0;
    JeeRing(30,214,14,f,C_ACCENT,0,0);
    { char v[10]; snprintf(v,sizeof(v),"%d%%",(int)(f*100));
      DrawTextC(30,211,v,C_TEXT,1); }
    char s[26]; snprintf(s,sizeof(s),"%d OF %d TODAY",done,tot);
    DrawText(52,208,s,C_DATA,1);
    snprintf(s,sizeof(s),"%d ACTIVE DAYS",active);
    DrawText(52,220,s,C_SAND,1); }
  { Panel(162,190,152,44,0,C_ACCENT,0);
    DrawText(168,194,"SUBJECT MIX",C_SAND,1);
    uint32_t sm[SUB_N]={0,0,0,0}, sall=0;
    for (int i=0;i<N;i++){
      int idx=(JB->dayHead+JEE_DAYS-(N-1)+i)%JEE_DAYS;
      for (int k=0;k<SUB_N;k++){ sm[k]+=JB->day[idx].subMin[k]; sall+=JB->day[idx].subMin[k]; } }
    if (!sall) sall=1;
    int x=168;
    for (int k=0;k<SUB_N;k++){
      int w=(int)(138.0f*(float)sm[k]/sall);
      if (w>0){ BlendRectFB(x,208,w,8,SubCol(k),A_FILL);
        Glow(x+w/2,212,SubCol(k),40,0.8f); x+=w; } }
    for (int k=0;k<SUB_N;k++){
      CircleFB(170+k*36,224,2,SubCol(k),255);
      DrawText(176+k*36,221,SUB_SHORT[k],C_SAND,1); } }
  JeeTabs(ST_JSTATS);
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE HISTORY  --  calendar heat grid + day inspector
// =====================================================================
static int jeeHistSel = -1;
void ScreenJeeHist(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }
  Backdrop();
  if (JeeBar("HISTORY",C_ACCENT)) return;

  DrawText(8,24,"LAST 10 WEEKS  -  TAP A DAY",C_SAND,1);
  // 7 rows (weekday) x 10 cols (week), most recent bottom-right
  const int CW=17,CH=17,OX=14,OY=40;
  int maxm=JB->targetMin>0?JB->targetMin:300;
  for (int i=0;i<70;i++){
    int idx=(JB->dayHead+JEE_DAYS-69+i)%JEE_DAYS;
    if (JB->day[idx].studyMin>maxm) maxm=JB->day[idx].studyMin; }
  for (int i=0;i<70;i++){
    int idx=(JB->dayHead+JEE_DAYS-69+i)%JEE_DAYS;
    int col=i/7, row=i%7;
    int x=OX+col*CW, y=OY+row*CH;
    float d=Stagger(enterAnim,col,0.02f,0.4f);
    if (d<=0.02f) continue;
    int m=JB->day[idx].studyMin;
    float f=clampf((float)m/maxm,0,1);
    uint16_t c = (m==0)?Dim(C_HAIR,3,5):Fade(C_ACCENT,(uint8_t)(70+185*f));
    int sz=(int)(13*d);
    BlendRectFB(x,y,sz,sz,c,A_FILL);
    if (i==69){ Bracket(x-1,y-1,15,15,C_HILITE,4);
      Glow(x+6,y+6,C_HILITE,(uint8_t)(90*Pulse(gTime,3.0f)),0.8f); }
    if (jeeHistSel==idx) Bracket(x-1,y-1,15,15,C_TEXT,4);
    if (touchDown&&touchX>=x&&touchX<x+14&&touchY>=y&&touchY<y+14){
      jeeHistSel=idx;
      SpawnBurst(x+6,y+6,10,C_ACCENT,90.0f,PK_SPARK); } }
  // legend
  DrawText(OX,OY+7*CH+2,"LESS",C_HAIR,1);
  for (int k=0;k<5;k++)
    BlendRectFB(OX+30+k*10,OY+7*CH+1,8,8,Fade(C_ACCENT,(uint8_t)(50+50*k)),A_FILL);
  DrawText(OX+84,OY+7*CH+2,"MORE",C_HAIR,1);

  // ---- inspector ----
  Panel(190,40,124,150,"DAY",C_ACCENT,0);
  if (jeeHistSel<0){
    DrawText(198,64,"SELECT A DAY",C_SAND,1);
    DrawText(198,78,"FROM THE GRID",C_SAND,1);
    DrawText(198,100,"EACH SQUARE IS",C_HAIR,1);
    DrawText(198,112,"ONE STUDY DAY",C_HAIR,1);
  } else {
    JeeDay &d=JB->day[jeeHistSel];
    int back=( (JB->dayHead+JEE_DAYS-jeeHistSel)%JEE_DAYS );
    char s[28];
    if (back==0) snprintf(s,sizeof(s),"TODAY");
    else snprintf(s,sizeof(s),"%d DAYS AGO",back);
    GlowText(198,60,s,C_TEXT,1,60);
    snprintf(s,sizeof(s),"%02u:%02u",(unsigned)(d.studyMin/60),(unsigned)(d.studyMin%60));
    GlowText(198,74,s,C_ACCENT,2,70);
    DrawText(198,96,"STUDY TIME",C_SAND,1);
    float f=JB->targetMin?clampf((float)d.studyMin/JB->targetMin,0,1):0;
    JeeRing(252,132,22,f,C_ACCENT,0,0);
    { char pv[8]; snprintf(pv,sizeof(pv),"%d%%",(int)(f*100));
      DrawTextC(252,129,pv,C_TEXT,1); }
    snprintf(s,sizeof(s),"TASKS %u",(unsigned)d.tasksDone);
    DrawText(198,160,s,C_DATA,1);
    int y2=172;
    for (int k=0;k<SUB_N;k++){
      if (!d.subMin[k]) continue;
      snprintf(s,sizeof(s),"%s %u",SUB_SHORT[k],(unsigned)d.subMin[k]);
      DrawText(198,y2,s,SubCol(k),1); y2+=11;
      if (y2>182) break; } }
  DrawText(8,196,"STREAK",C_SAND,1);
  { char s[20]; snprintf(s,sizeof(s),"%u DAYS",(unsigned)JB->streak);
    GlowText(8,206,s,C_WARN,2,80);
    snprintf(s,sizeof(s),"BEST %u",(unsigned)JB->bestStreak);
    DrawText(8,226,s,C_SAND,1); }
  // little flame row
  for (int i=0;i<6;i++){
    float ph=fmodf(gTime*1.8f+i*0.3f,1.0f);
    int fx=110+i*12, fy=222-(int)(ph*10);
    int w=(int)(5*(1.0f-ph));
    if (w>0) BlendRectFB(fx-w/2,fy,w,3,i&1?C_WARN:C_HILITE,(uint8_t)(200*(1.0f-ph))); }
  JeeTabs(ST_JHIST);
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE NOTES
// =====================================================================
static int jeeNoteSel=-1;
void ScreenJeeNotes(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }

  int order[JEE_NOTES],n=0;
  for (int pass=0;pass<2;pass++)
    for (int i=0;i<JEE_NOTES;i++)
      if (JB->note[i].used && ((pass==0)==(bool)JB->note[i].pinned)) order[n++]=i;

  const int TOP=42,ROW=40,VIEW=SCREEN_H-TOP-6;
  float contentH=n*ROW+4;
  if (jeeNoteSel<0) JeeScrollUpdate(dt,contentH,VIEW,TOP);

  Backdrop();
  if (JeeBar("NOTES",C_ACCENT)) return;
  { char s[24]; snprintf(s,sizeof(s),"%d NOTES",n);
    DrawText(8,24,s,C_DATA,1);
    DrawText(90,24,"TAP TO OPEN  -  LEFT EDGE PINS",C_HAIR,1); }

  if (!n){
    DrawTextC(160,110,"NO NOTES YET",C_SAND,2);
    DrawTextC(160,134,"TAP + TO WRITE ONE",C_HAIR,1);
  } else for (int k=0;k<n;k++){
    int i=order[k];
    int y=TOP+k*ROW-(int)jeeScroll;
    if (y<TOP-ROW||y>SCREEN_H) continue;
    JeeNote &nt=JB->note[i];
    float d=Stagger(enterAnim,k,0.03f,0.4f);
    int x=8+(int)((1.0f-d)*20);
    uint16_t sc=SubCol(nt.subject);
    BlendRectFB(x,y,304,ROW-4,C_PANEL,(uint8_t)(A_FILL*d));
    Bracket(x,y,304,ROW-4,Dim(sc,3,5),5);
    BlendRectFB(x,y,3,ROW-4,sc,255);
    if (nt.pinned){
      CircleFB(x+14,y+9,4,C_HILITE,255);
      Glow(x+14,y+9,C_HILITE,80,0.6f); }
    DrawText(x+24,y+6,nt.title,C_TEXT,1);
    // body preview, clipped
    char prev[46];
    snprintf(prev,sizeof(prev),"%s",nt.body);
    DrawText(x+24,y+20,prev,C_SAND,1);
    DrawText(x+276,y+6,SUB_SHORT[nt.subject],sc,1);
    if (touchDown&&touchY>=y&&touchY<y+ROW-4&&touchY>TOP&&!jeeDrag){
      if (touchX<x+22){ nt.pinned=!nt.pinned; JeeMark(); JeeSave();
        SpawnBurst(x+14,y+9,10,C_HILITE,90.0f,PK_SPARK); }
      else jeeNoteSel=i; } }

  JeeScrollBar(314,TOP,VIEW,contentH,VIEW);

  if (jeeNoteSel>=0&&jeeNoteSel<JEE_NOTES){
    JeeNote &nt=JB->note[jeeNoteSel];
    Scrim(180);
    Panel(20,34,280,176,"NOTE",C_HILITE,0);
    GlowText(30,54,nt.title,C_TEXT,2,70);
    // wrapped body
    { const int CPL=44;
      int len=strlen(nt.body), line=0;
      for (int p=0;p<len&&line<5;p+=CPL,line++){
        char ln[CPL+1];
        int c=0;
        for (int q=p;q<len&&c<CPL;q++) ln[c++]=nt.body[q];
        ln[c]=0;
        DrawText(30,78+line*12,ln,C_SAND,1); } }
    { uint16_t sc=SubCol(nt.subject);
      if (Button(30,146,80,22,SUB_NAME[nt.subject],sc,true)){
        nt.subject=(nt.subject+1)%SUB_N; JeeMark(); } }
    if (Button(116,146,58,22,"TITLE",C_ACCENT,false)){
      kbNoteIdx=jeeNoteSel; jeeNoteSel=-1;
      KbOpen("NOTE TITLE",nt.title,27,ST_JNOTES,KBP_NOTE_T); return; }
    if (Button(180,146,58,22,"BODY",C_ACCENT,false)){
      kbNoteIdx=jeeNoteSel; jeeNoteSel=-1;
      KbOpen("NOTE BODY",nt.body,JEE_BODY-1,ST_JNOTES,KBP_NOTE_B); return; }
    if (Button(244,146,46,22,"PIN",C_HILITE,nt.pinned)){
      nt.pinned=!nt.pinned; JeeMark(); JeeSave(); }
    if (Button(30,178,120,22,"DELETE",C_WARN,false)){
      nt.used=0; nt.title[0]=0; nt.body[0]=0; jeeNoteSel=-1;
      JeeMark(); JeeSave(); }
    if (Button(160,178,130,22,"CLOSE",C_DATA,false)) jeeNoteSel=-1; }
  JeeTabs(ST_JNOTES);
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE QUOTES
// =====================================================================
static const char *QUOTE_A[] = {
  "DISCIPLINE IS CHOOSING BETWEEN",
  "SUCCESS IS THE SUM OF SMALL",
  "THE EXPERT IN ANYTHING WAS",
  "DO NOT WATCH THE CLOCK",
  "HARD WORK BEATS TALENT WHEN",
  "IT ALWAYS SEEMS IMPOSSIBLE",
  "THE SECRET OF GETTING AHEAD",
  "YOU DO NOT HAVE TO BE GREAT",
  "LITTLE BY LITTLE",
  "FOCUS ON BEING PRODUCTIVE" };
static const char *QUOTE_B[] = {
  "WHAT YOU WANT NOW AND MOST",
  "EFFORTS REPEATED DAY IN",
  "ONCE A BEGINNER WHO",
  "DO WHAT IT DOES -",
  "TALENT DOES NOT",
  "UNTIL IT IS DONE -",
  "IS GETTING STARTED -",
  "TO START BUT YOU HAVE TO",
  "A LITTLE BECOMES",
  "INSTEAD OF BUSY -" };
static const char *QUOTE_C[] = {
  "WHAT YOU WANT MOST",
  "AND DAY OUT",
  "REFUSED TO GIVE UP",
  "KEEP GOING",
  "WORK HARD",
  "NELSON MANDELA",
  "MARK TWAIN",
  "START TO BE GREAT",
  "A LOT",
  "TIM FERRISS" };
#define QUOTE_N ((int)(sizeof(QUOTE_A)/sizeof(QUOTE_A[0])))
static int jeeQuoteIdx=0;
static float jeeQuoteFade=1;

void ScreenJeeQuote(float dt){
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }
  Backdrop();
  if (JeeBar("QUOTES",C_ACCENT)) return;
  jeeQuoteFade=clampf(jeeQuoteFade+dt*2.4f,0,1);

  Panel(10,40,300,140,0,C_ACCENT,0);
  // decorative quote marks
  GlowText(24,54,"\"",C_ACCENT,3,80);
  float e=EaseOutCubic(jeeQuoteFade);
  uint8_t a=(uint8_t)(255*e);
  int yo=(int)((1.0f-e)*8);
  DrawTextC(160,82+yo,QUOTE_A[jeeQuoteIdx],Fade(C_TEXT,a),1);
  DrawTextC(160,98+yo,QUOTE_B[jeeQuoteIdx],Fade(C_TEXT,a),1);
  GlowTextC(160,120+yo,QUOTE_C[jeeQuoteIdx],C_ACCENT,2,(uint8_t)(70*e));
  { for (int x=100;x<220;x+=4) PxBlend(x,146,C_HAIR,120); }
  { char s[16]; snprintf(s,sizeof(s),"%d / %d",jeeQuoteIdx+1,QUOTE_N);
    DrawTextC(160,158,s,C_SAND,1); }

  if (Button(16,190,88,30,"< PREV",C_ACCENT,false)){
    jeeQuoteIdx=(jeeQuoteIdx+QUOTE_N-1)%QUOTE_N; jeeQuoteFade=0;
    SpawnBurst(60,205,12,C_ACCENT,100.0f,PK_SPARK); }
  if (Button(112,190,96,30,"RANDOM",C_HILITE,false)){
    jeeQuoteIdx=(int)(Hash((uint32_t)millis())*QUOTE_N)%QUOTE_N; jeeQuoteFade=0;
    SpawnBurst(160,205,16,C_HILITE,120.0f,PK_SPARK); }
  if (Button(216,190,88,30,"NEXT >",C_ACCENT,false)){
    jeeQuoteIdx=(jeeQuoteIdx+1)%QUOTE_N; jeeQuoteFade=0;
    SpawnBurst(260,205,12,C_ACCENT,100.0f,PK_SPARK); }
  JeeTabs(ST_JQUOTE);
  DrawParticles();
  EnterOverlay();
}

// =====================================================================
//  JEE SETTINGS
// =====================================================================
void ScreenJeeSet(float dt){
  if (!JB){ appState=ST_HOME; return; }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (JeeQuickSheet()){ DrawParticles(); return; }
  Backdrop();
  if (JeeBar("JEE SETUP",C_ACCENT)) return;

  Panel(6,24,308,58,"DAILY TARGET",C_ACCENT,0);
  { char v[16]; snprintf(v,sizeof(v),"%02u:%02u",
      (unsigned)(JB->targetMin/60),(unsigned)(JB->targetMin%60));
    GlowText(16,44,v,C_TEXT,3,80);
    DrawText(16,72,"HOURS PER DAY",C_SAND,1);
    if (Button(120,42,44,26,"-30",C_ACCENT,false)&&JB->targetMin>60){
      JB->targetMin-=30; JeeMark(); }
    if (Button(168,42,44,26,"+30",C_ACCENT,false)&&JB->targetMin<900){
      JB->targetMin+=30; JeeMark(); }
    float f=JeeProgress();
    JeeRing(272,54,22,f,C_ACCENT,0,0);
    { char p[8]; snprintf(p,sizeof(p),"%d%%",(int)(f*100));
      DrawTextC(272,51,p,C_TEXT,1); } }

  Panel(6,88,150,66,"DATA",C_ACCENT,0);
  if (Button(14,108,134,20,"SAVE NOW",C_ACCENT,false)){
    JeeSave(); SpawnBurst(81,118,18,C_ACCENT,130.0f,PK_SPARK); }
  if (Button(14,132,134,18,"RESET JEE DATA",C_WARN,false)){
    prefs.begin(NVS_NS,false);
    prefs.remove("jHdr"); prefs.remove("jTsk");
    prefs.remove("jNot"); prefs.remove("jGlD");
    prefs.end();
    JeeDefaults(); JeeSave();
    SpawnBurst(81,141,26,C_WARN,150.0f,PK_SPARK); }

  Panel(162,88,152,66,"TOUCH",C_ACCENT,0);
  DrawText(170,108,calLoaded?"CAL: CUSTOM":"CAL: DEFAULT",
           calLoaded?C_DATA:C_SAND,1);
  if (Button(170,120,136,20,"RECALIBRATE",C_ACCENT,false)){
    CalibReset(); GoTo(ST_CALIB,238,130,C_ACCENT,TR_HEX); return; }
  if (Button(170,142,136,10,"RESET CAL",C_WARN,false)){
    ResetCalibration(); SpawnBurst(238,147,16,C_WARN,120.0f,PK_SPARK); }

  Panel(6,160,308,50,"STORAGE",C_ACCENT,0);
  { char s[52];
    snprintf(s,sizeof(s),"BLOB %u B   TASKS %d   NOTES %d   GOALS %d",
             (unsigned)sizeof(JeeBlob),JEE_TASKS,JEE_NOTES,JEE_GOALS);
    DrawText(14,180,s,C_SAND,1);
    snprintf(s,sizeof(s),"HISTORY %d DAYS   %s",JEE_DAYS,
             jeeDirty?"UNSAVED CHANGES":"ALL SAVED");
    DrawText(14,194,s,jeeDirty?C_WARN:C_DATA,1); }
  DrawText(8,218,"FLASH IS WRITTEN ONLY ON REAL EDITS",C_HAIR,1);
  DrawText(8,228,"LIVE TIMER STAYS IN RAM",C_HAIR,1);
  JeeTabs(ST_JSET);
  DrawParticles();
  EnterOverlay();
}

// #####################################################################
// #   N E X U S   v5   --  GRAPHICS ENVIRONMENT                       #
// #   Every effect below is generated in software. No sensors, no     #
// #   network, no extra peripherals -- CPU + PSRAM + framebuffer only.#
// #####################################################################

// =====================================================================
//  MOLECULE / DNA  --  procedural node-and-rod rendering
//  Drawn directly rather than as a mesh: nodes are shaded spheres and
//  bonds are depth-sorted rods, which is what makes it read as chemistry
//  instead of "another solid".
// =====================================================================
void DrawMolecule(float t,int cx,int cy,float sc,bool dna){
  struct P3 { float x,y,z; uint8_t hue; uint8_t r; };
  P3 n[26];
  int cnt=0;
  if (!dna){
    // methane-like: one core + 4 arms + 4 caps
    n[cnt++]={0,0,0,0,9};
    const float A[4][3]={{1,1,1},{-1,-1,1},{-1,1,-1},{1,-1,-1}};
    for (int i=0;i<4;i++){
      n[cnt++]={A[i][0]*0.62f,A[i][1]*0.62f,A[i][2]*0.62f,(uint8_t)(1+i%3),6};
      n[cnt++]={A[i][0]*1.05f,A[i][1]*1.05f,A[i][2]*1.05f,(uint8_t)(4+i%2),4}; }
  } else {
    // double helix
    for (int i=0;i<12;i++){
      float u=i*0.52f+t*0.6f;
      float y=-1.0f+i*0.18f;
      n[cnt++]={fcos(u)*0.55f,y,fsin(u)*0.55f,0,5};
      n[cnt++]={fcos(u+3.1416f)*0.55f,y,fsin(u+3.1416f)*0.55f,3,5}; }
  }
  // rotate + project
  float ry=t*0.7f, rx=0.42f;
  float sy=fsin(ry),cy2=fcos(ry),sx=fsin(rx),cx2=fcos(rx);
  struct S { int x,y,r; float z; uint8_t hue; };
  S out[26];
  for (int i=0;i<cnt;i++){
    float x=n[i].x, y=n[i].y, z=n[i].z;
    float x1=cy2*x+sy*z, z1=-sy*x+cy2*z;
    float y1=cx2*y-sx*z1, z2=sx*y+cx2*z1;
    float pz=z2+3.2f; if (pz<0.4f) pz=0.4f;
    float k=250.0f/pz*sc;
    out[i].x=cx+(int)(x1*k); out[i].y=cy-(int)(y1*k);
    out[i].r=(int)(n[i].r*k*0.055f); if (out[i].r<1) out[i].r=1;
    out[i].z=z2; out[i].hue=n[i].hue; }
  // bonds first (behind), depth-faded
  for (int i=0;i<cnt;i++){
    for (int j=i+1;j<cnt;j++){
      float dx=n[i].x-n[j].x,dy=n[i].y-n[j].y,dz=n[i].z-n[j].z;
      float d2=dx*dx+dy*dy+dz*dz;
      bool bond = dna ? (fabsf(n[i].y-n[j].y)<0.02f) : (d2<0.75f);
      if (!bond) continue;
      float mz=(out[i].z+out[j].z)*0.5f;
      uint8_t a=(uint8_t)clampf(200.0f-mz*46.0f,50,235);
      LineFB(out[i].x,out[i].y,out[j].x,out[j].y,Dim(C_SAND,4,5),a);
      LineFB(out[i].x,out[i].y+1,out[j].x,out[j].y+1,Dim(C_HAIR,4,5),(uint8_t)(a/2)); } }
  // draw nodes far-to-near
  int ord[26];
  for (int i=0;i<cnt;i++) ord[i]=i;
  for (int i=0;i<cnt-1;i++) for (int j=0;j<cnt-1-i;j++)
    if (out[ord[j]].z<out[ord[j+1]].z){ int tmp=ord[j]; ord[j]=ord[j+1]; ord[j+1]=tmp; }
  for (int k=0;k<cnt;k++){
    int i=ord[k];
    uint16_t c=Spec(out[i].hue);
    int r=out[i].r;
    // cheap shaded sphere: 3 nested discs
    CircleFB(out[i].x,out[i].y,r,Dim(c,2,5),255);
    CircleFB(out[i].x-r/4,out[i].y-r/4,(r*2)/3,c,255);
    CircleFB(out[i].x-r/3,out[i].y-r/3,r/3,C_TEXT,180);
    Glow(out[i].x,out[i].y,c,70,0.5f+r*0.05f); }
}
void ScreenMolecule(float dt){
  static bool dnaMode=false;
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  UpdateOrbit(dt);
  Backdrop();
  Panel(4,22,312,190,dnaMode?"DNA HELIX":"MOLECULE",C_ACCENT,"PROC");
  DrawMolecule(gTime+rotY,160,120,1.0f+sCam*0.6f,dnaMode);
  if (Button(10,214,90,20,dnaMode?"SHOW MOLECULE":"SHOW DNA",C_ACCENT,false)){
    dnaMode=!dnaMode; SpawnBurst(55,224,18,C_ACCENT,130.0f,PK_SPARK); }
  DrawText(112,220,dnaMode?"ANIMATED DOUBLE HELIX":"NODES AND BONDS, DEPTH SORTED",C_SAND,1);
  DrawParticles();
  TopBar(dnaMode?"DNA":"MOLECULE",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  PHYSICS PLAYGROUND  --  8 interactive simulations
// =====================================================================
#define PH_MODES 8
static const char *PH_NAME[PH_MODES]={
  "PROJECTILE","PENDULUM","SPRING","GRAVITY","ORBIT","COLLISION","DBL PEND","N-BODY" };
static int   phMode=0;
static bool  phAim=false;
static float phAX,phAY;
// projectile
static float pjX,pjY,pjVX,pjVY; static bool pjFly=false;
static float pjTrail[24][2]; static int pjT=0;
// pendulum + double pendulum
static float pdA=0.8f,pdV=0,pdA2=0.6f,pdV2=0;
// spring
static float spY=0,spV=0,spTarget=0;
// bodies for gravity/orbit/collision/nbody
#define PH_BODY 14
static float bX[PH_BODY],bY[PH_BODY],bVX[PH_BODY],bVY[PH_BODY],bM[PH_BODY];
static uint8_t bHue[PH_BODY];
static int   phN=0;

static void PhysReset(int mode){
  phMode=mode; pjFly=false; pjT=0;
  pdA=0.9f; pdV=0; pdA2=0.5f; pdV2=0;
  spY=0; spV=0; spTarget=0;
  phN=0;
  if (mode==3||mode==7){                 // gravity / n-body
    phN=(mode==7)?12:8;
    for (int i=0;i<phN;i++){
      bX[i]=40+Hash(i*331u)*240; bY[i]=50+Hash(i*733u)*140;
      bVX[i]=(Hash(i*97u)-0.5f)*40; bVY[i]=(Hash(i*51u)-0.5f)*40;
      bM[i]=1.0f+Hash(i*17u)*2.0f; bHue[i]=i%6; } }
  else if (mode==4){                     // orbit
    phN=5;
    bX[0]=160; bY[0]=124; bVX[0]=0; bVY[0]=0; bM[0]=60; bHue[0]=0;
    for (int i=1;i<phN;i++){
      float r=34+i*20;
      bX[i]=160+r; bY[i]=124;
      bVX[i]=0; bVY[i]=sqrtf(2600.0f/r);
      bM[i]=1.2f; bHue[i]=i; } }
  else if (mode==5){                     // collision
    phN=8;
    for (int i=0;i<phN;i++){
      bX[i]=40+i*32; bY[i]=70+((i&1)?40:0);
      bVX[i]=(i&1)?70:-70; bVY[i]=(Hash(i*7u)-0.5f)*40;
      bM[i]=1.6f; bHue[i]=i%6; } }
}
void ScreenPhysics(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (dt>0.05f) dt=0.05f;
  const int TOP=44, BOT=SCREEN_H-18;

  // mode strip
  Backdrop();
  for (int i=0;i<PH_MODES;i++){
    int x=2+i*40;
    if (Button(x,22,38,18,PH_NAME[i],Spec(i),phMode==i)){
      PhysReset(i); SpawnBurst(x+19,31,10,Spec(i),90.0f,PK_SPARK); } }

  bool touchIn = touchActive && touchY>TOP && touchY<BOT;

  switch (phMode){
    case 0: { // PROJECTILE -- drag to aim, release to launch
      if (touchDown&&touchY>TOP&&touchY<BOT){ phAim=true; phAX=touchX; phAY=touchY; }
      if (touchUp&&phAim){
        phAim=false;
        pjX=40; pjY=BOT-10;
        pjVX=(phAX-touchX)*2.2f; pjVY=(phAY-touchY)*2.2f;
        pjFly=true; pjT=0;
        SpawnBurst(pjX,pjY,16,C_ACCENT,140.0f,PK_SPARK); }
      if (pjFly){
        pjVY+=260.0f*dt;
        pjX+=pjVX*dt; pjY+=pjVY*dt;
        if (pjT<24){ pjTrail[pjT][0]=pjX; pjTrail[pjT][1]=pjY; pjT++; }
        else { for (int i=0;i<23;i++){ pjTrail[i][0]=pjTrail[i+1][0]; pjTrail[i][1]=pjTrail[i+1][1]; }
               pjTrail[23][0]=pjX; pjTrail[23][1]=pjY; }
        if (pjY>BOT-4){ pjY=BOT-4; pjVY*=-0.55f; pjVX*=0.8f;
          SpawnBurst(pjX,pjY,10,C_WARN,90.0f,PK_SPARK);
          if (fabsf(pjVY)<24) pjFly=false; }
        if (pjX>SCREEN_W-4||pjX<2) pjVX=-pjVX; }
      HLineFB(0,BOT,SCREEN_W,C_HAIR);
      for (int i=1;i<pjT;i++)
        LineFB((int)pjTrail[i-1][0],(int)pjTrail[i-1][1],
               (int)pjTrail[i][0],(int)pjTrail[i][1],Dim(C_ACCENT,3,5),160);
      if (pjFly||pjT){ CircleFB((int)pjX,(int)pjY,4,C_HILITE,255);
        Glow((int)pjX,(int)pjY,C_ACCENT,140,1.0f); }
      if (phAim){
        LineFB(40,BOT-10,touchX,touchY,C_WARN,200);
        CircleFB(40,BOT-10,5,C_ACCENT,255);
        float p=sqrtf((phAX-touchX)*(phAX-touchX)+(phAY-touchY)*(phAY-touchY));
        char b[24]; snprintf(b,sizeof(b),"POWER %d",(int)p);
        DrawText(touchX+8,touchY-10,b,C_WARN,1); }
      else DrawText(8,BOT+4,"DRAG ANYWHERE TO AIM, RELEASE TO FIRE",C_SAND,1);
    } break;

    case 1: { // PENDULUM
      float L=90.0f;
      if (touchIn){ pdA=atan2f(touchX-160,touchY-TOP-6); pdV=0; }
      else { pdV+=-9.8f*fsin(pdA)*dt*3.4f; pdV*=0.9993f; pdA+=pdV*dt; }
      int px=160+(int)(fsin(pdA)*L), py=TOP+6+(int)(fcos(pdA)*L);
      LineFB(160,TOP+6,px,py,C_HAIR,220);
      CircleFB(160,TOP+6,3,C_SAND,255);
      CircleFB(px,py,11,C_ACCENT,255);
      CircleFB(px-3,py-3,5,C_HILITE,200);
      Glow(px,py,C_ACCENT,120,1.6f);
      for (int i=1;i<=6;i++){
        float a2=pdA-pdV*0.03f*i;
        int tx=160+(int)(fsin(a2)*L), ty=TOP+6+(int)(fcos(a2)*L);
        CircleFB(tx,ty,3,Dim(C_ACCENT,4-(i>>1),5),(uint8_t)(120-i*16)); }
      char b[30]; snprintf(b,sizeof(b),"ANGLE %d  VEL %d",
        (int)(pdA*57.3f),(int)(pdV*57.3f));
      DrawText(8,BOT+4,b,C_DATA,1);
    } break;

    case 2: { // SPRING
      if (touchIn) spTarget=(touchY-124)*0.9f;
      else spTarget=0;
      float k=52.0f, damp=2.4f;
      float acc=(spTarget-spY)*k-spV*damp;
      spV+=acc*dt; spY+=spV*dt;
      int ay=100+(int)spY;
      for (int i=0;i<14;i++){
        int y1=TOP+4+i*((ay-TOP-4)/14);
        int y2=TOP+4+(i+1)*((ay-TOP-4)/14);
        int off=(i&1)?12:-12;
        LineFB(160,y1,160+off,(y1+y2)/2,C_HAIR,220);
        LineFB(160+off,(y1+y2)/2,160,y2,C_HAIR,220); }
      BlendRectFB(136,ay,48,30,C_ACCENT,A_FILL);
      Bracket(136,ay,48,30,C_HILITE,5);
      Glow(160,ay+15,C_ACCENT,110,2.0f);
      char b[30]; snprintf(b,sizeof(b),"DISP %d  VEL %d",(int)spY,(int)spV);
      DrawText(8,BOT+4,b,C_DATA,1);
      DrawText(150,BOT+4,"DRAG TO PULL THE MASS",C_SAND,1);
    } break;

    default: { // body sims: gravity / orbit / collision / n-body / dbl pend
      if (phMode==6){ // DOUBLE PENDULUM
        float L1=56,L2=52,m1=1,m2=1,g=9.8f*2.2f;
        if (touchIn){ pdA=atan2f(touchX-160,touchY-TOP-4); pdV=0; pdV2=0; }
        else {
          float d=pdA-pdA2;
          float den=(2*m1+m2-m2*fcos(2*d));
          float a1=(-g*(2*m1+m2)*fsin(pdA)-m2*g*fsin(pdA-2*pdA2)
                    -2*fsin(d)*m2*(pdV2*pdV2*L2+pdV*pdV*L1*fcos(d)))/(L1*den);
          float a2=(2*fsin(d)*(pdV*pdV*L1*(m1+m2)+g*(m1+m2)*fcos(pdA)
                    +pdV2*pdV2*L2*m2*fcos(d)))/(L2*den);
          pdV+=a1*dt; pdV2+=a2*dt;
          pdV*=0.9995f; pdV2*=0.9995f;
          pdA+=pdV*dt; pdA2+=pdV2*dt; }
        int x1=160+(int)(fsin(pdA)*L1), y1=TOP+4+(int)(fcos(pdA)*L1);
        int x2=x1+(int)(fsin(pdA2)*L2), y2=y1+(int)(fcos(pdA2)*L2);
        LineFB(160,TOP+4,x1,y1,C_HAIR,230);
        LineFB(x1,y1,x2,y2,C_HAIR,230);
        CircleFB(x1,y1,8,Spec(1),255); Glow(x1,y1,Spec(1),110,1.2f);
        CircleFB(x2,y2,8,Spec(4),255); Glow(x2,y2,Spec(4),110,1.2f);
        PxAdd(x2,y2,C_TEXT,255);
        DrawText(8,BOT+4,"CHAOTIC - DRAG TO RESET",C_SAND,1);
        break; }
      // particle bodies
      if (touchIn&&touchDown&&phN<PH_BODY){
        bX[phN]=touchX; bY[phN]=touchY;
        bVX[phN]=(Hash((uint32_t)millis())-0.5f)*60;
        bVY[phN]=(Hash((uint32_t)millis()*7u)-0.5f)*60;
        bM[phN]=1.5f; bHue[phN]=phN%6; phN++;
        SpawnBurst(touchX,touchY,10,C_ACCENT,90.0f,PK_SPARK); }
      for (int i=0;i<phN;i++){
        if (phMode==3){ bVY[i]+=180.0f*dt; }
        if (phMode==4||phMode==7){
          for (int j=0;j<phN;j++){
            if (i==j) continue;
            if (phMode==4&&j!=0) continue;
            float dx=bX[j]-bX[i], dy=bY[j]-bY[i];
            float d2=dx*dx+dy*dy+40.0f;
            float f=(2600.0f*bM[j])/d2;
            float d=sqrtf(d2);
            bVX[i]+=f*dx/d*dt; bVY[i]+=f*dy/d*dt; } }
        if (phMode==4&&i==0) continue;
        bX[i]+=bVX[i]*dt; bY[i]+=bVY[i]*dt;
        // walls
        if (bX[i]<8){ bX[i]=8; bVX[i]=-bVX[i]*0.86f; }
        if (bX[i]>SCREEN_W-8){ bX[i]=SCREEN_W-8; bVX[i]=-bVX[i]*0.86f; }
        if (bY[i]<TOP+8){ bY[i]=TOP+8; bVY[i]=-bVY[i]*0.86f; }
        if (bY[i]>BOT-8){ bY[i]=BOT-8; bVY[i]=-bVY[i]*0.86f; bVX[i]*=0.96f; } }
      if (phMode==5){ // elastic collisions
        for (int i=0;i<phN;i++) for (int j=i+1;j<phN;j++){
          float dx=bX[j]-bX[i], dy=bY[j]-bY[i];
          float d=sqrtf(dx*dx+dy*dy);
          float rr=9+9;
          if (d>0.01f&&d<rr){
            float nx=dx/d, ny=dy/d;
            float p=2.0f*(bVX[i]*nx+bVY[i]*ny-bVX[j]*nx-bVY[j]*ny)/(bM[i]+bM[j]);
            bVX[i]-=p*bM[j]*nx; bVY[i]-=p*bM[j]*ny;
            bVX[j]+=p*bM[i]*nx; bVY[j]+=p*bM[i]*ny;
            float ov=(rr-d)*0.5f;
            bX[i]-=nx*ov; bY[i]-=ny*ov; bX[j]+=nx*ov; bY[j]+=ny*ov;
            SpawnBurst((bX[i]+bX[j])/2,(bY[i]+bY[j])/2,4,C_HILITE,70.0f,PK_SPARK); } } }
      // draw
      if (phMode==4){
        for (int i=1;i<phN;i++){
          float r=sqrtf((bX[i]-bX[0])*(bX[i]-bX[0])+(bY[i]-bY[0])*(bY[i]-bY[0]));
          RingFB((int)bX[0],(int)bY[0],(int)r,Dim(C_HAIR,3,5),70); } }
      for (int i=0;i<phN;i++){
        uint16_t c=Spec(bHue[i]);
        int r=(int)(4+bM[i]*2.2f);
        if (phMode==4&&i==0){ r=13; c=C_HILITE; }
        CircleFB((int)bX[i],(int)bY[i],r,c,255);
        CircleFB((int)bX[i]-r/3,(int)bY[i]-r/3,r/2,C_TEXT,120);
        Glow((int)bX[i],(int)bY[i],c,90,0.9f+r*0.05f);
        LineAdd((int)bX[i],(int)bY[i],
                (int)(bX[i]-bVX[i]*0.05f),(int)(bY[i]-bVY[i]*0.05f),c,90); }
      char b[34]; snprintf(b,sizeof(b),"BODIES %d   TAP TO ADD",phN);
      DrawText(8,BOT+4,b,C_DATA,1);
    } break; }

  if (Button(250,BOT+1,66,16,"RESET",C_WARN,false)) PhysReset(phMode);
  DrawParticles();
  TopBar("PHYSICS",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  PARTICLE SANDBOX  --  attract / repel / vortex / explode / gravity
//  Uses its own PSRAM pool so it never starves the UI particle system.
// =====================================================================
#define SAND_MAX 420
struct SandP { float x,y,vx,vy; uint8_t hue; };
static SandP *sand=nullptr;
static int   sandN=260;
static int   sandTool=0;
static float sandGrav=0, sandFric=0.86f, sandRadius=70;
static const char *SAND_TOOL[6]={"ATTRACT","REPEL","VORTEX","EXPLODE","GRAVITY","TRAIL"};

static void SandInit(void){
  if (!sand) sand=(SandP*)heap_caps_malloc(sizeof(SandP)*SAND_MAX,MALLOC_CAP_SPIRAM);
  if (!sand) return;
  for (int i=0;i<SAND_MAX;i++){
    sand[i].x=Hash(i*331u)*SCREEN_W;
    sand[i].y=40+Hash(i*733u)*(SCREEN_H-70);
    sand[i].vx=(Hash(i*97u)-0.5f)*30;
    sand[i].vy=(Hash(i*51u)-0.5f)*30;
    // Only TWO neighbouring hues, biased toward the accent. Six hues at
    // once read as RGB confetti; a tight ramp reads as one glowing fluid.
    sand[i].hue=(Hash(i*277u)>0.78f)?1:0; }
}
void ScreenSand(float dt){
  if (BackHit()) return;
  if (!sand){ SandInit(); if (!sand){ appState=ST_HOME; return; } }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (dt>0.05f) dt=0.05f;
  const int TOP=42,BOT=SCREEN_H-30;

  // Motion-blur trails MUST accumulate in a dedicated buffer. `frame`
  // alternates between fb[0] and fb[1] every frame, so decaying it only
  // touches each buffer on alternate frames and the display swaps between
  // two different trail states -> 15 Hz strobe. This is the same bug that
  // was fixed in WARP; the particle screen still had it.
  uint16_t *sandSave=frame;
  frame=accum;
  uint32_t *pp=(uint32_t*)accum;
  for (int i=0;i<FB_PIXELS/2;i++){
    uint32_t two=pp[i];
    if (!two) continue;
    uint32_t out=0;
    for (int k=0;k<2;k++){
      uint16_t c=(uint16_t)(two>>(k*16));
      if (c){ int r=(c>>11)&0x1F,g=(c>>5)&0x3F,b=c&0x1F;
        // 15/16 per frame ~= 0.9s visible tail at 30fps. The old 10/16
        // plus a -1 killed a full-bright pixel in 5 frames, which is what
        // produced the strobing.
        int keep=(sandTool==5)?15:14;
        r=(r*keep)>>4; g=(g*keep)>>4; b=(b*keep)>>4;
        c=(uint16_t)((r<<11)|(g<<5)|b); }
      out|=(uint32_t)c<<(k*16); }
    pp[i]=out; }

  bool act = touchActive && touchY>TOP && touchY<BOT;
  float tx=touchX, ty=touchY;
  for (int i=0;i<sandN;i++){
    SandP &p=sand[i];
    if (act){
      float dx=tx-p.x, dy=ty-p.y;
      float d2=dx*dx+dy*dy+24.0f;
      float d=sqrtf(d2);
      if (d<sandRadius*2.4f){
        float f=0;
        switch (sandTool){
          case 0: f= 1700.0f/d2; p.vx+=f*dx/d*dt*60; p.vy+=f*dy/d*dt*60; break;
          case 1: f=-2100.0f/d2; p.vx+=f*dx/d*dt*60; p.vy+=f*dy/d*dt*60; break;
          case 2: { float px=-dy/d, py=dx/d;
                    f=1500.0f/d2;
                    p.vx+=(px*f*1.7f+dx/d*f*0.3f)*dt*60;
                    p.vy+=(py*f*1.7f+dy/d*f*0.3f)*dt*60; } break;
          case 3: if (touchDown){ f=9000.0f/d2;
                    p.vx-=f*dx/d; p.vy-=f*dy/d; } break;
          case 4: p.vy+=240.0f*dt; break;
          case 5: f=900.0f/d2; p.vx+=f*dx/d*dt*60; p.vy+=f*dy/d*dt*60; break; } } }
    p.vy+=sandGrav*220.0f*dt;
    float fr=powf(sandFric,dt*60.0f);
    p.vx*=fr; p.vy*=fr;
    p.x+=p.vx*dt; p.y+=p.vy*dt;
    if (p.x<2){ p.x=2; p.vx=-p.vx*0.7f; }
    if (p.x>SCREEN_W-2){ p.x=SCREEN_W-2; p.vx=-p.vx*0.7f; }
    if (p.y<TOP){ p.y=TOP; p.vy=-p.vy*0.7f; }
    if (p.y>BOT){ p.y=BOT; p.vy=-p.vy*0.7f; }
    // ---- render -------------------------------------------------------
    //  Brightness tracks SPEED with no idle floor, so a settled particle
    //  fades into the trail instead of pulsing. Fast particles stretch into
    //  a motion streak; the core is a 5px cross so it reads as a body
    //  rather than a single flickering pixel.
    float sp=fabsf(p.vx)+fabsf(p.vy);
    float e=clampf(sp/260.0f,0,1);
    uint8_t amt=(uint8_t)(40.0f+180.0f*e);
    // hot particles shift toward the highlight, cool ones sit on the accent
    uint16_t c=(e>0.62f)?C_HILITE:Spec(p.hue);
    int px=(int)p.x, py=(int)p.y;
    if (sp>40.0f){
      float k=clampf(sp*0.00055f,0.010f,0.030f);
      LineAdd(px,py,(int)(p.x-p.vx*k),(int)(p.y-p.vy*k),c,(uint8_t)(amt*0.75f)); }
    PxAdd(px,py,c,amt);
    PxAdd(px+1,py,c,(uint8_t)(amt>>1));
    PxAdd(px-1,py,c,(uint8_t)(amt>>1));
    PxAdd(px,py+1,c,(uint8_t)(amt>>1));
    PxAdd(px,py-1,c,(uint8_t)(amt>>1));
    if (e>0.80f) Glow(px,py,C_HILITE,(uint8_t)(52*e),0.42f); }

  // Trails are complete. Publish accum to the real framebuffer and switch
  // back, so all chrome below draws on the live buffer and never smears
  // into the trail accumulator.
  frame=sandSave;
  memcpy(frame,accum,FB_BYTES);

  if (act){
    RingFB((int)tx,(int)ty,(int)sandRadius,C_HILITE,(uint8_t)(70+60*Pulse(gTime,6.0f)));
    Glow((int)tx,(int)ty,C_HILITE,90,1.6f); }

  // tool strip
  for (int i=0;i<6;i++){
    int x=2+i*53;
    if (Button(x,22,51,18,SAND_TOOL[i],Spec(i),sandTool==i)){
      sandTool=i; SpawnBurst(x+25,31,10,Spec(i),90.0f,PK_SPARK); } }
  // params
  BlendRectFB(0,SCREEN_H-28,SCREEN_W,28,C_PANEL,A_FILL);
  { float pn=(float)sandN/SAND_MAX;
    if (SliderRow(48,SCREEN_H-18,80,"COUNT",&pn,C_ACCENT)) sandN=clampi((int)(pn*SAND_MAX),20,SAND_MAX);
    float g=clampf(sandGrav,0,1);
    if (SliderRow(178,SCREEN_H-18,70,"GRAV",&g,C_ACCENT)) sandGrav=g;
    float fr=clampf((sandFric-0.75f)/0.24f,0,1);
    if (SliderRow(280,SCREEN_H-18,30,"FRIC",&fr,C_ACCENT)) sandFric=0.75f+fr*0.24f; }
  DrawParticles();
  TopBar("PARTICLES",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  SPACE  --  starfield, procedural planets, comets, warp
// =====================================================================
static float spCamX=0, spCamY=0, spWarp=0;
static float spPlanetPh=0;
void ScreenSpace(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  bool hold=touchActive&&touchY>40;
  spWarp=Approach(spWarp,hold?1.0f:0.0f,2.4f,dt);
  if (hold){ spCamX=Approach(spCamX,(touchX-160)*0.35f,3.0f,dt);
             spCamY=Approach(spCamY,(touchY-120)*0.30f,3.0f,dt); }
  spPlanetPh+=dt*0.22f;

  memset(frame,0,FB_BYTES);
  memset(depth,0,FB_BYTES);

  // nebula: cheap layered blobs, no per-pixel noise
  for (int i=0;i<9;i++){
    float a=i*0.7f+gTime*0.05f;
    int nx=(int)(160+fcos(a)*(70+i*11)-spCamX*0.25f);
    int ny=(int)(120+fsin(a*1.3f)*(46+i*7)-spCamY*0.25f);
    Glow(nx,ny,Spec(i%6),(uint8_t)(16+6*fsin(gTime*0.6f+i)),6.0f); }

  // starfield with warp streaks
  for (int i=0;i<NUM_WARP;i++){
    float oz=warp[i].z;
    warp[i].z-=(28.0f+spWarp*640.0f)*dt;
    if (warp[i].z<14.0f){
      warp[i].x=(float)random(-1600,1600);
      warp[i].y=(float)random(-1200,1200);
      warp[i].z=1600.0f; oz=warp[i].z; }
    float k1=260.0f/oz, k2=260.0f/warp[i].z;
    int x1=(int)(160+(warp[i].x-spCamX*4)*k1), y1=(int)(120+(warp[i].y-spCamY*4)*k1);
    int x2=(int)(160+(warp[i].x-spCamX*4)*k2), y2=(int)(120+(warp[i].y-spCamY*4)*k2);
    float br=clampf(k2*1.1f,0.10f,1.0f);
    uint16_t c=Fade(((i&9)==0)?Spec(i%6):C_TEXT,(uint8_t)(br*255));
    if (spWarp>0.05f&&abs(x2-x1)<200&&abs(y2-y1)<200) LineFB(x1,y1,x2,y2,c,255);
    else PxAdd(x2,y2,c,(uint8_t)(br*255));
    if (k2>2.4f) Glow(x2,y2,C_TEXT,70,0.4f); }

  // orbital system, drawn far to near
  ApplyLight();
  { int sx=160-(int)spCamX, sy=120-(int)spCamY;
    Glow(sx,sy,Spec(0),120,4.0f);
    CircleFB(sx,sy,15,Spec(0),255);
    CircleFB(sx-4,sy-4,7,C_HILITE,200);
    for (int i=1;i<=3;i++){
      float r=44.0f+i*30;
      RingFB(sx,sy,(int)r,Dim(C_HAIR,3,5),60);
      float a=spPlanetPh*(1.6f/i)+i;
      int px=sx+(int)(fcos(a)*r), py=sy+(int)(fsin(a)*r*0.42f);
      float sc=0.20f+i*0.05f;
      RenderMesh(gMesh[i==2?12:0],0.4f,gTime*0.5f+i,0,sc,(float)px,(float)py,3.0f,
                 M_SMOOTH,0);
      Glow(px,py,Spec(i+1),60,1.4f); } }

  // comet
  { float ct=fmodf(gTime*0.22f,1.0f);
    int cx=(int)(-30+ct*400)-(int)spCamX, cy=(int)(50+ct*90)-(int)spCamY;
    for (int i=0;i<16;i++){
      int tx=cx-i*5, ty=cy-i*2;
      PxAdd(tx,ty,C_HILITE,(uint8_t)(200-i*12)); }
    CircleFB(cx,cy,3,C_TEXT,255);
    Glow(cx,cy,C_HILITE,150,1.2f); }

  if (spWarp>0.02f){
    for (int r=0;r<3;r++){
      float ph=fmodf(gTime*1.4f+r*0.33f,1.0f);
      RingFB(160,120,(int)(ph*ph*300),C_HILITE,(uint8_t)(A_GLOW*spWarp*(1.0f-ph))); } }

  DrawText(8,SCREEN_H-13,hold?"HYPERSPACE ENGAGED":"HOLD TO WARP - DRAG TO LOOK",
           hold?C_HILITE:C_SAND,1);
  { int bw=(int)(60*spWarp);
    Bracket(SCREEN_W-70,SCREEN_H-16,64,12,C_HILITE,4);
    FillRectFB(SCREEN_W-68,SCREEN_H-14,bw,8,C_HILITE); }
  DrawParticles();
  TopBar("SPACE",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  PLANET GENERATOR  --  procedural terrain / atmosphere / rings
// =====================================================================
static uint32_t pgSeed=12345;
static bool     pgRings=true;
static float    pgSpin=0;
static uint8_t  pgHueA=0,pgHueB=3;
static float    pgZoom=1.0f;

static void PlanetNew(void){
  pgSeed=(uint32_t)(millis()*2654435761u)^(uint32_t)(gTime*10000);
  pgRings=(Hash(pgSeed)>0.45f);
  pgHueA=(uint8_t)((int)(Hash(pgSeed*7u)*6))%6;
  pgHueB=(uint8_t)((int)(Hash(pgSeed*13u)*6))%6;
  if (pgHueB==pgHueA) pgHueB=(pgHueA+3)%6;
}
void ScreenPlanetGen(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (touchActive&&touchY>40&&touchY<200){
    pgSpin+=(touchX-lastTX)*0.01f;
    pgZoom=clampf(pgZoom+(touchY-lastTY)*-0.004f,0.6f,1.9f);
    lastTX=touchX; lastTY=touchY; }
  else pgSpin+=dt*0.34f;

  Backdrop();
  const int CX=160,CY=118;
  int R=(int)(62*pgZoom);

  // rings behind
  if (pgRings)
    for (int i=0;i<26;i++){
      float rr=R*1.35f+i*1.9f;
      uint8_t a=(uint8_t)(70+50*fsin(i*0.7f+pgSeed*0.001f));
      for (int k=0;k<64;k++){
        float a2=TAU*k/64;
        int px=CX+(int)(fcos(a2)*rr), py=CY+(int)(fsin(a2)*rr*0.28f);
        if (py<CY) PxBlend(px,py,Spec(pgHueB),(uint8_t)(a*0.7f)); } }

  // planet body: procedural bands + terrain speckle, lit from the left
  for (int y=-R;y<=R;y++){
    int w=(int)sqrtf(fmaxf(0.0f,(float)(R*R-y*y)));
    for (int x=-w;x<=w;x++){
      float nx=(float)x/R, ny=(float)y/R;
      float nz=sqrtf(fmaxf(0.0f,1.0f-nx*nx-ny*ny));
      // rotate longitude by spin for a turning globe
      float lon=atan2f(nx,nz)+pgSpin;
      float lat=ny;
      // banded terrain from cheap trig noise
      float n=fsin(lat*7.0f+fsin(lon*3.0f+pgSeed*0.0007f)*1.4f)
             +0.5f*fsin(lon*5.0f+lat*9.0f)
             +0.25f*fsin(lat*17.0f+pgSeed*0.0003f);
      float lightF=clampf(nx*-0.55f+nz*0.75f+0.30f,0.06f,1.0f);
      uint16_t base = (n>0.35f)?Spec(pgHueA)
                    : (n>-0.15f)?Dim(Spec(pgHueA),3,5)
                    : Spec(pgHueB);
      if (lat>0.80f||lat<-0.80f) base=C_TEXT;       // ice caps
      // clouds
      float cl=fsin(lon*4.0f+gTime*0.25f)+fsin(lat*8.0f-gTime*0.18f);
      uint16_t c=Fade(base,(uint8_t)(lightF*255));
      if (cl>1.25f) c=Fade(C_TEXT,(uint8_t)(lightF*190));
      PxBlend(CX+x,CY+y,c,255); } }
  // atmosphere rim
  for (int i=0;i<5;i++)
    RingFB(CX,CY,R+i,Spec(pgHueB),(uint8_t)(90-i*16));
  Glow(CX-R/3,CY-R/3,C_TEXT,60,2.2f);

  // rings in front
  if (pgRings)
    for (int i=0;i<26;i++){
      float rr=R*1.35f+i*1.9f;
      uint8_t a=(uint8_t)(70+50*fsin(i*0.7f+pgSeed*0.001f));
      for (int k=0;k<64;k++){
        float a2=TAU*k/64;
        int px=CX+(int)(fcos(a2)*rr), py=CY+(int)(fsin(a2)*rr*0.28f);
        if (py>=CY) PxBlend(px,py,Spec(pgHueB),a); } }

  if (Button(8,206,92,24,"NEW PLANET",C_ACCENT,false)){
    PlanetNew(); SpawnBurst(160,118,44,Spec(pgHueA),200.0f,PK_SPARK); }
  if (Button(106,206,72,24,pgRings?"RINGS ON":"RINGS OFF",C_ACCENT,pgRings)) pgRings=!pgRings;
  if (Button(184,206,60,24,"ZOOM -",C_ACCENT,false)) pgZoom=clampf(pgZoom-0.15f,0.6f,1.9f);
  if (Button(250,206,62,24,"ZOOM +",C_ACCENT,false)) pgZoom=clampf(pgZoom+0.15f,0.6f,1.9f);
  { char b[40]; snprintf(b,sizeof(b),"SEED %08X   DRAG TO ROTATE",(unsigned)pgSeed);
    DrawText(8,196,b,C_SAND,1); }
  DrawParticles();
  TopBar("PLANET GEN",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  FRACTAL LAB  --  Mandelbrot / Julia / Sierpinski
//  Escape-time fractals are expensive, so the result is cached in the
//  `accum` PSRAM buffer and only recomputed when the view changes. The
//  compute is also split across frames so touch never blocks.
// =====================================================================
static int   frType=0;              // 0 mandel 1 julia 2 sierpinski
static float frCX=-0.6f, frCY=0, frScale=1.6f;
static float frJX=-0.79f, frJY=0.15f;
static int   frIter=48, frPalette=0;
static int   frRow=0;               // progressive scanline cursor
static bool  frDirty=true;
static const char *FR_NAME[3]={"MANDELBROT","JULIA","SIERPINSKI"};

static uint16_t FracCol(int it,int maxIt){
  if (it>=maxIt) return C_BG;
  float f=(float)it/maxIt;
  switch (frPalette){
    case 0: return Fade(Spec((int)(f*6.0f)%6),(uint8_t)(90+165*f));
    case 1: return Fade(C_ACCENT,(uint8_t)(40+215*f));
    default: { uint8_t g=(uint8_t)(40+215*f); return RGB565(g,g,g); } }
}
static void FracStep(int rows){
  if (frType==2) return;
  const int TOP=42,BOT=SCREEN_H-22;
  for (int n=0;n<rows;n++){
    if (frRow>=BOT-TOP){ frDirty=false; return; }
    int y=TOP+frRow;
    for (int x=0;x<SCREEN_W;x++){
      float u=(x-SCREEN_W*0.5f)/(SCREEN_W*0.5f)*frScale+frCX;
      float v=(y-(TOP+BOT)*0.5f)/((BOT-TOP)*0.5f)*frScale*0.75f+frCY;
      float zr,zi,cr,ci;
      if (frType==0){ zr=0; zi=0; cr=u; ci=v; }
      else { zr=u; zi=v; cr=frJX; ci=frJY; }
      int it=0;
      while (it<frIter){
        float zr2=zr*zr, zi2=zi*zi;
        if (zr2+zi2>4.0f) break;
        zi=2.0f*zr*zi+ci; zr=zr2-zi2+cr; it++; }
      accum[y*SCREEN_W+x]=FracCol(it,frIter); }
    frRow++; }
}
static void SierpinskiDraw(void){
  const int TOP=48;
  // chaos game: cheap, converges fast, looks great
  float ax=160,ay=(float)TOP, bx=24,by=200, cx2=296,cy2=200;
  float px=160,py=140;
  uint32_t sd=99;
  for (int i=0;i<4200;i++){
    sd=sd*1103515245u+12345u;
    int k=(sd>>16)%3;
    float tx=(k==0)?ax:((k==1)?bx:cx2);
    float ty=(k==0)?ay:((k==1)?by:cy2);
    px=(px+tx)*0.5f; py=(py+ty)*0.5f;
    if (i>12) PxAdd((int)px,(int)py,Spec(k*2),150); }
}
void ScreenFractal(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int TOP=42,BOT=SCREEN_H-22;

  // pan / zoom by drag
  if (touchActive&&touchY>TOP&&touchY<BOT&&frType!=2){
    int dx=touchX-lastTX, dy=touchY-lastTY;
    if (dx||dy){
      frCX-=dx*frScale/160.0f;
      frCY-=dy*frScale/160.0f;
      frDirty=true; frRow=0; }
    lastTX=touchX; lastTY=touchY; }

  if (frDirty) FracStep(10);            // progressive: never blocks touch

  if (frType==2){ Backdrop(); SierpinskiDraw(); }
  else {
    // blit the cached fractal
    for (int y=TOP;y<BOT;y++)
      memcpy(&frame[y*SCREEN_W],&accum[y*SCREEN_W],SCREEN_W*2);
    // top/bottom chrome areas
    BlendRectFB(0,0,SCREEN_W,TOP,C_BG,A_FILL);
    BlendRectFB(0,BOT,SCREEN_W,SCREEN_H-BOT,C_BG,A_FILL);
    if (frDirty){
      int y=TOP+frRow;
      HLineFB(0,y,SCREEN_W,C_HILITE);
      Glow(160,y,C_HILITE,90,2.0f); } }

  for (int i=0;i<3;i++)
    if (Button(2+i*72,22,70,18,FR_NAME[i],Spec(i*2),frType==i)){
      frType=i; frDirty=true; frRow=0;
      SpawnBurst(37+i*72,31,12,Spec(i*2),100.0f,PK_SPARK); }
  if (Button(222,22,44,18,"PAL",C_ACCENT,false)){
    frPalette=(frPalette+1)%3; frDirty=true; frRow=0; }
  if (Button(270,22,46,18,"RESET",C_WARN,false)){
    frCX=(frType==0)?-0.6f:0; frCY=0; frScale=1.6f; frIter=48;
    frDirty=true; frRow=0; }

  BlendRectFB(0,BOT,SCREEN_W,22,C_PANEL,A_FILL);
  if (Button(4,BOT+2,36,18,"Z +",C_ACCENT,false)){ frScale*=0.72f; frDirty=true; frRow=0; }
  if (Button(42,BOT+2,36,18,"Z -",C_ACCENT,false)){ frScale/=0.72f; frDirty=true; frRow=0; }
  if (Button(82,BOT+2,40,18,"IT +",C_ACCENT,false)){ frIter=clampi(frIter+16,16,160); frDirty=true; frRow=0; }
  if (Button(124,BOT+2,40,18,"IT -",C_ACCENT,false)){ frIter=clampi(frIter-16,16,160); frDirty=true; frRow=0; }
  { char b[42];
    snprintf(b,sizeof(b),"IT %d  ZOOM %d  %s",frIter,(int)(1.6f/frScale*100),
             frDirty?"RENDERING":"READY");
    DrawText(170,BOT+7,b,frDirty?C_WARN:C_DATA,1); }
  // --- hidden: hold the readout for a hand-picked deep-zoom preset ----
  {
    static uint32_t fh=0;
    if (touchActive && touchY>BOT && touchX>168){
      if (!fh) fh=millis();
      else if (millis()-fh>1700){ fh=0;
        frType=1; frJX=-0.7269f; frJY=0.1889f;   // dendrite Julia
        frCX=0; frCY=0; frScale=1.35f; frIter=128; frPalette=0;
        frDirty=true; frRow=0;
        EggFire(EGG_RETRO,3.0f,"SEAHORSE VALLEY"); }
    } else fh=0;
  }
  DrawParticles();
  TopBar("FRACTAL",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  MATRIX DIGITAL RAIN  --  layered depth, touch disturbs the columns
// =====================================================================
#define RAIN_COLS 40
static float rnY[RAIN_COLS], rnSpd[RAIN_COLS];
static uint8_t rnLen[RAIN_COLS], rnDepth[RAIN_COLS];
static bool rainInit=false;
void ScreenMatrix(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (!rainInit){
    for (int i=0;i<RAIN_COLS;i++){
      rnY[i]=Hash(i*331u)*SCREEN_H;
      rnDepth[i]=(uint8_t)(Hash(i*97u)*3);
      rnSpd[i]=(30.0f+rnDepth[i]*34.0f)*(0.7f+Hash(i*51u)*0.6f);
      rnLen[i]=(uint8_t)(6+Hash(i*7u)*12); }
    rainInit=true; }
  memset(frame,0,FB_BYTES);
  static const char GLY[]="01ABCDEFXZ+-<>/*#$%";
  const int GN=19;
  for (int i=0;i<RAIN_COLS;i++){
    int x=i*8+2;
    // touch pushes nearby columns
    if (touchActive){
      float d=fabsf((float)touchX-x);
      if (d<46){ rnY[i]+=(46-d)*0.9f;
        rnSpd[i]=Approach(rnSpd[i],220.0f,4.0f,dt); }
      else rnSpd[i]=Approach(rnSpd[i],(30.0f+rnDepth[i]*34.0f),1.5f,dt); }
    rnY[i]+=rnSpd[i]*dt;
    if (rnY[i]>SCREEN_H+rnLen[i]*9){
      rnY[i]=-(float)(rnLen[i]*9);
      rnDepth[i]=(uint8_t)(Hash((uint32_t)(gTime*991)+i)*3); }
    uint8_t scale=(rnDepth[i]>=2)?2:1;
    for (int k=0;k<rnLen[i];k++){
      int y=(int)rnY[i]-k*(7*scale+1);
      if (y<-8||y>SCREEN_H) continue;
      uint32_t h=(uint32_t)(gTime*8)+i*131u+k*17u;
      char c=GLY[(uint32_t)(Hash(h)*GN)%GN];
      char sbuf[2]={c,0};
      uint16_t col;
      if (k==0){ col=C_TEXT; }
      else col=Fade(Spec(rnDepth[i]==2?3:4),(uint8_t)clampf(230.0f-k*(200.0f/rnLen[i]),20,230));
      DrawText(x,y,sbuf,col,scale);
      if (k==0) Glow(x+3,y+4,Spec(3),120,0.8f); } }
  if (touchActive) Glow(touchX,touchY,C_HILITE,110,2.4f);
  DrawText(6,SCREEN_H-12,"TOUCH TO DISTURB THE RAIN",Dim(C_SAND,3,5),1);
  DrawParticles();
  TopBar("DIGITAL RAIN",Spec(3));
  EnterOverlay();
}

// =====================================================================
//  FIELD SIM  --  wave propagation on a coarse grid, touch injects
//  A 40x30 height field integrated with the classic wave equation, then
//  drawn as an illuminated vector/contour field. Coarse grid keeps it
//  well inside the frame budget.
// =====================================================================
#define FW 40
#define FH 30
static float *fldA=nullptr,*fldB=nullptr;
static int   fldMode=0;   // 0 ripple 1 vectors 2 contour
static void FieldInit(void){
  if (!fldA) fldA=(float*)heap_caps_malloc(sizeof(float)*FW*FH,MALLOC_CAP_SPIRAM);
  if (!fldB) fldB=(float*)heap_caps_malloc(sizeof(float)*FW*FH,MALLOC_CAP_SPIRAM);
  if (fldA&&fldB){ for (int i=0;i<FW*FH;i++){ fldA[i]=0; fldB[i]=0; } }
}
void ScreenField(float dt){
  if (BackHit()) return;
  if (!fldA||!fldB){ FieldInit(); if (!fldA||!fldB){ appState=ST_HOME; return; } }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int TOP=42;
  const int CW=SCREEN_W/FW, CH=(SCREEN_H-TOP-20)/FH;

  if (touchActive&&touchY>TOP&&touchY<SCREEN_H-20){
    int gx=clampi((touchX)/CW,1,FW-2);
    int gy=clampi((touchY-TOP)/CH,1,FH-2);
    fldA[gy*FW+gx]+=touchDown?9.0f:3.4f; }

  // wave equation, 2 sub-steps for stability
  for (int s=0;s<2;s++){
    for (int y=1;y<FH-1;y++)
      for (int x=1;x<FW-1;x++){
        int i=y*FW+x;
        float lap=fldA[i-1]+fldA[i+1]+fldA[i-FW]+fldA[i+FW]-4.0f*fldA[i];
        float v=(fldA[i]-fldB[i])*0.986f+lap*0.20f;
        fldB[i]=fldA[i]+v; }
    float *tmp=fldA; fldA=fldB; fldB=tmp; }

  Backdrop();
  for (int y=1;y<FH-1;y++)
    for (int x=1;x<FW-1;x++){
      int i=y*FW+x;
      float h=fldA[i];
      int px=x*CW, py=TOP+y*CH;
      if (fldMode==0){
        float m=clampf(fabsf(h)*0.55f,0,1);
        if (m<0.03f) continue;
        uint16_t c=(h>0)?Spec(4):Spec(1);
        BlendRectFB(px,py,CW,CH,c,(uint8_t)(m*230));
        if (m>0.55f) Glow(px+CW/2,py+CH/2,c,(uint8_t)(m*70),0.7f);
      } else if (fldMode==1){
        float gx=fldA[i+1]-fldA[i-1];
        float gy=fldA[i+FW]-fldA[i-FW];
        float m=sqrtf(gx*gx+gy*gy);
        if (m<0.05f) continue;
        int ex=px+CW/2+(int)clampf(gx*7.0f,-6,6);
        int ey=py+CH/2+(int)clampf(gy*7.0f,-6,6);
        uint16_t c=Spec((int)(clampf(m*3.0f,0,5)));
        LineFB(px+CW/2,py+CH/2,ex,ey,c,(uint8_t)clampf(m*300,60,255));
        PxAdd(ex,ey,C_TEXT,180);
      } else {
        float m=fabsf(h);
        int band=(int)(m*4.0f);
        if (band<1) continue;
        uint16_t c=Spec(band%6);
        PxBlend(px+CW/2,py+CH/2,c,(uint8_t)clampf(m*240,40,255));
        if (band>=2) PxBlend(px+CW/2+1,py+CH/2,c,(uint8_t)clampf(m*160,30,200)); } }

  static const char *FM[3]={"RIPPLE","VECTORS","CONTOUR"};
  for (int i=0;i<3;i++)
    if (Button(2+i*72,22,70,18,FM[i],Spec(i+2),fldMode==i)) fldMode=i;
  if (Button(230,22,86,18,"CLEAR",C_WARN,false))
    for (int i=0;i<FW*FH;i++){ fldA[i]=0; fldB[i]=0; }
  DrawText(6,SCREEN_H-14,"TOUCH TO INJECT ENERGY - WAVES PROPAGATE AND INTERFERE",C_SAND,1);
  DrawParticles();
  TopBar("FIELD SIM",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  TOUCH PLAYGROUND
// =====================================================================
#define TP_RIP 8
static float tpX[TP_RIP],tpY[TP_RIP],tpT[TP_RIP];
static int   tpMode=0;
static float tpTrail[40][2]; static int tpTN=0;
void ScreenTouchPlay(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  static const char *TPM[5]={"RIPPLE","PARTICLE","TRAIL","WELL","BURST"};

  if (touchDown&&touchY>42){
    for (int i=0;i<TP_RIP;i++) if (tpT[i]<=0){ tpX[i]=touchX; tpY[i]=touchY; tpT[i]=1.0f; break; }
    if (tpMode==1) SpawnBurst(touchX,touchY,26,Spec((int)(gTime*3)%6),170.0f,PK_SPARK);
    if (tpMode==4) SpawnBurst(touchX,touchY,50,C_HILITE,260.0f,PK_EMBER);
    tpTN=0; }
  if (touchActive&&touchY>42){
    if (tpMode==2){
      if (tpTN<40){ tpTrail[tpTN][0]=smoothTX; tpTrail[tpTN][1]=smoothTY; tpTN++; }
      else { for (int i=0;i<39;i++){ tpTrail[i][0]=tpTrail[i+1][0]; tpTrail[i][1]=tpTrail[i+1][1]; }
             tpTrail[39][0]=smoothTX; tpTrail[39][1]=smoothTY; } }
    if (tpMode==1&&((int)(gTime*60)&1)==0)
      SpawnBurst(smoothTX,smoothTY,2,Spec((int)(gTime*2)%6),70.0f,PK_SPARK); }

  Backdrop();
  // ripples
  for (int i=0;i<TP_RIP;i++){
    if (tpT[i]<=0) continue;
    tpT[i]-=dt*1.25f;
    float p=1.0f-clampf(tpT[i],0,1);
    int r=(int)(EaseOutQuint(p)*90);
    uint8_t a=(uint8_t)(230*clampf(tpT[i],0,1)*clampf(tpT[i],0,1));
    RingFB((int)tpX[i],(int)tpY[i],r,C_HILITE,a);
    RingFB((int)tpX[i],(int)tpY[i],r-4,Spec(i%6),(uint8_t)(a*0.6f));
    if (p<0.4f) Glow((int)tpX[i],(int)tpY[i],C_HILITE,(uint8_t)(a*0.5f),1.6f); }
  // trail
  if (tpMode==2)
    for (int i=1;i<tpTN;i++){
      uint8_t a=(uint8_t)(255*i/tpTN);
      int w=1+(i*4)/tpTN;
      LineAdd((int)tpTrail[i-1][0],(int)tpTrail[i-1][1],
              (int)tpTrail[i][0],(int)tpTrail[i][1],Spec((i/6)%6),a);
      if (w>2) CircleFB((int)tpTrail[i][0],(int)tpTrail[i][1],w/2,Spec((i/6)%6),a); }
  // gravity well warps a background lattice
  if (tpMode==3&&touchActive){
    for (int gy=50;gy<SCREEN_H-10;gy+=14)
      for (int gx=8;gx<SCREEN_W-6;gx+=14){
        float dx=gx-smoothTX, dy=gy-smoothTY;
        float d=sqrtf(dx*dx+dy*dy)+8;
        float pull=1400.0f/(d*d);
        int px=gx-(int)(dx/d*pull*10), py=gy-(int)(dy/d*pull*10);
        PxAdd(px,py,C_ACCENT,(uint8_t)clampf(60+pull*900,60,255)); }
    Glow((int)smoothTX,(int)smoothTY,C_WARN,150,2.6f); }
  if (touchActive&&tpMode!=3){
    RingFB((int)smoothTX,(int)smoothTY,14,C_HILITE,(uint8_t)(120+80*Pulse(gTime,7.0f)));
    Glow((int)smoothTX,(int)smoothTY,C_HILITE,110,1.4f); }

  for (int i=0;i<5;i++)
    if (Button(2+i*64,22,62,18,TPM[i],Spec(i),tpMode==i)){
      tpMode=i; tpTN=0; SpawnBurst(33+i*64,31,10,Spec(i),90.0f,PK_SPARK); }
  { char b[40]; snprintf(b,sizeof(b),"TOUCH  X %3d  Y %3d  %s",
      touchX,touchY,touchActive?"DOWN":"UP");
    DrawText(6,SCREEN_H-12,b,touchActive?C_DATA:C_SAND,1); }
  DrawParticles();
  TopBar("TOUCH LAB",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  CONWAY'S GAME OF LIFE  --  bit-packed, draw/erase by touch
// =====================================================================
#define LF_W 64
#define LF_H 44
static uint8_t *lifeA=nullptr,*lifeB=nullptr;
static bool  lfRun=false, lfErase=false;
static float lfAcc=0, lfSpeed=12.0f;
static uint32_t lfGen=0;
static void LifeAlloc(void){
  if (!lifeA) lifeA=(uint8_t*)heap_caps_malloc(LF_W*LF_H,MALLOC_CAP_SPIRAM);
  if (!lifeB) lifeB=(uint8_t*)heap_caps_malloc(LF_W*LF_H,MALLOC_CAP_SPIRAM);
}
static void LifeRandom(void){
  if (!lifeA) return;
  for (int i=0;i<LF_W*LF_H;i++)
    lifeA[i]=(Hash((uint32_t)(millis()*2654435761u)+i*7919u)>0.72f)?1:0;
  lfGen=0;
}
static void LifeStep(void){
  if (!lifeA||!lifeB) return;
  for (int y=0;y<LF_H;y++){
    int ym=((y-1)+LF_H)%LF_H, yp=(y+1)%LF_H;
    for (int x=0;x<LF_W;x++){
      int xm=((x-1)+LF_W)%LF_W, xp=(x+1)%LF_W;
      int n=lifeA[ym*LF_W+xm]+lifeA[ym*LF_W+x]+lifeA[ym*LF_W+xp]
           +lifeA[y *LF_W+xm]                +lifeA[y *LF_W+xp]
           +lifeA[yp*LF_W+xm]+lifeA[yp*LF_W+x]+lifeA[yp*LF_W+xp];
      uint8_t c=lifeA[y*LF_W+x];
      lifeB[y*LF_W+x]=(c&&(n==2||n==3))||(!c&&n==3);
    } }
  uint8_t *t=lifeA; lifeA=lifeB; lifeB=t;
  lfGen++;
}
void ScreenLife(float dt){
  if (BackHit()) return;
  if (!lifeA||!lifeB){ LifeAlloc();
    if (!lifeA||!lifeB){ appState=ST_HOME; return; }
    LifeRandom(); }
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int TOP=42,CS=4;
  if (lfRun){
    lfAcc+=dt*lfSpeed;
    int guard=0;
    while (lfAcc>=1.0f&&guard<4){ LifeStep(); lfAcc-=1.0f; guard++; } }
  // draw / erase with the finger
  if (touchActive&&touchY>TOP&&touchY<TOP+LF_H*CS){
    int gx=clampi((touchX-32)/CS,0,LF_W-1);
    int gy=clampi((touchY-TOP)/CS,0,LF_H-1);
    lifeA[gy*LF_W+gx]=lfErase?0:1;
    if (gx>0) lifeA[gy*LF_W+gx-1]=lfErase?0:1; }

  Backdrop();
  BlendRectFB(30,TOP-2,LF_W*CS+4,LF_H*CS+4,C_PANEL,A_FILL);
  Bracket(30,TOP-2,LF_W*CS+4,LF_H*CS+4,C_ACCENT,6);
  int alive=0;
  for (int y=0;y<LF_H;y++)
    for (int x=0;x<LF_W;x++)
      if (lifeA[y*LF_W+x]){
        alive++;
        FillRectFB(32+x*CS,TOP+y*CS,CS-1,CS-1,Spec((x+y)%6&3)); }
  if (alive) Glow(160,TOP+LF_H*CS/2,C_ACCENT,26,6.0f);

  if (Button(2,22,44,18,lfRun?"PAUSE":"PLAY",lfRun?C_WARN:C_ACCENT,lfRun)) lfRun=!lfRun;
  if (Button(48,22,44,18,"STEP",C_ACCENT,false)){ LifeStep(); lfRun=false; }
  if (Button(94,22,50,18,"RANDOM",C_ACCENT,false)) LifeRandom();
  if (Button(146,22,44,18,"CLEAR",C_WARN,false)){
    memset(lifeA,0,LF_W*LF_H); lfGen=0; }
  if (Button(192,22,54,18,lfErase?"ERASING":"DRAWING",lfErase?C_WARN:C_ACCENT,lfErase))
    lfErase=!lfErase;
  if (Button(248,22,32,18,"S-",C_ACCENT,false)) lfSpeed=clampf(lfSpeed-4,2,40);
  if (Button(282,22,34,18,"S+",C_ACCENT,false)) lfSpeed=clampf(lfSpeed+4,2,40);
  { char b[52];
    snprintf(b,sizeof(b),"GEN %lu   ALIVE %d   SPEED %d",
             (unsigned long)lfGen,alive,(int)lfSpeed);
    GlowText(6,SCREEN_H-12,b,C_DATA,1,50); }
  DrawParticles();
  TopBar("LIFE",C_ACCENT);
  EnterOverlay();
}

// =====================================================================
//  DEMO MODE  --  automatic guided tour with morphing transitions
// =====================================================================
static int   dmStep=0;
static float dmT=0;
static bool  dmActive=false;
struct DemoStep { uint8_t target; float dur; const char *cap; };
static const DemoStep DEMO[] = {
  { ST_HOME,      3.0f, "NEXUS OS" },
  { ST_LAB,       4.0f, "REAL-TIME 3D" },
  { ST_OBJECTS,   3.5f, "15 SOLIDS" },
  { ST_MODES,     3.5f, "11 RENDER MODES" },
  { ST_MOLECULE,  3.5f, "PROCEDURAL CHEMISTRY" },
  { ST_PSAND,     4.0f, "PARTICLE PHYSICS" },
  { ST_SPACE,     4.5f, "PROCEDURAL UNIVERSE" },
  { ST_PLANETGEN, 4.0f, "PLANET GENERATOR" },
  { ST_FRACTAL,   4.5f, "FRACTAL MATHEMATICS" },
  { ST_PHYS,      4.0f, "PHYSICS ENGINE" },
  { ST_LIFE,      3.5f, "CELLULAR AUTOMATA" },
  { ST_FIELD,     3.5f, "WAVE SIMULATION" },
  { ST_MATRIX,    3.0f, "DIGITAL RAIN" },
  { ST_TOUCHPLAY, 3.0f, "TOUCH ENGINE" },
  { ST_JEE,       3.5f, "JEE COMMAND CENTER" },
  { ST_HOME,      2.5f, "NEXUS OS" }
};
#define DEMO_N ((int)(sizeof(DEMO)/sizeof(DEMO[0])))
static void DemoBegin(void){
  dmActive=true; dmStep=0; dmT=0;
  appState=(AppState)DEMO[0].target; enterAnim=0;
}
// Overlay drawn on top of whatever the demo is showing.
static void DemoOverlay(float dt){
  if (!dmActive) return;
  dmT+=dt;
  // drive the showcased screens so they look alive
  if (DEMO[dmStep].target==ST_LAB){ rotY+=dt*0.8f; }
  if (DEMO[dmStep].target==ST_PSAND&&sand){
    // auto-swirl the sandbox
    sandTool=2;
    float a=gTime*1.4f;
    touchX=(int)(160+fcos(a)*70); touchY=(int)(130+fsin(a)*46);
    touchActive=true; }
  if (DEMO[dmStep].target==ST_FIELD&&fldA&&((int)(gTime*3)%2==0)){
    int gx=8+(int)(Hash((uint32_t)(gTime*7))*(FW-16));
    int gy=6+(int)(Hash((uint32_t)(gTime*11))*(FH-12));
    fldA[gy*FW+gx]+=6.0f; }
  if (DEMO[dmStep].target==ST_TOUCHPLAY&&((int)(gTime*2)%2==0)){
    for (int i=0;i<TP_RIP;i++) if (tpT[i]<=0){
      tpX[i]=40+Hash((uint32_t)(gTime*13))*240;
      tpY[i]=70+Hash((uint32_t)(gTime*17))*130;
      tpT[i]=1.0f; break; } }

  float d=DEMO[dmStep].dur;
  float fadeIn=clampf(dmT/0.6f,0,1);
  float fadeOut=clampf((d-dmT)/0.6f,0,1);
  float a=fminf(fadeIn,fadeOut);

  // caption bar
  int bh=(int)(26*EaseOutCubic(a));
  if (bh>2){
    BlendRectFB(0,SCREEN_H-bh,SCREEN_W,bh,C_PANEL,(uint8_t)(A_FILL*a));
    HLineFB(0,SCREEN_H-bh,SCREEN_W,Fade(C_ACCENT,(uint8_t)(255*a)));
    GlowTextC(160,SCREEN_H-bh+9,DEMO[dmStep].cap,C_TEXT,1,(uint8_t)(80*a));
    // progress pips
    for (int i=0;i<DEMO_N;i++){
      int px=6+i*((SCREEN_W-12)/DEMO_N);
      BlendRectFB(px,SCREEN_H-4,(SCREEN_W-12)/DEMO_N-2,2,
                  i<=dmStep?C_ACCENT:C_HAIR,(uint8_t)(220*a)); } }
  // corner brand
  BlendRectFB(0,0,SCREEN_W,16,C_PANEL,(uint8_t)(A_FILL*a));
  GlowText(6,4,"NEXUS DEMO",C_ACCENT,1,(uint8_t)(80*a));
  { char b[24]; snprintf(b,sizeof(b),"%d / %d",dmStep+1,DEMO_N);
    DrawText(SCREEN_W-TextW(b,1)-52,4,b,C_SAND,1); }
  if (Button(SCREEN_W-46,1,44,14,"STOP",C_WARN,false)){
    dmActive=false; touchActive=false;
    GoTo(ST_HOME,160,120,C_ACCENT,TR_IRIS); return; }

  if (dmT>=d){
    dmStep++;
    dmT=0;
    touchActive=false;
    if (dmStep>=DEMO_N){ dmActive=false;
      GoTo(ST_HOME,160,120,C_ACCENT,TR_SHOCK); }
    else GoTo(DEMO[dmStep].target,160,120,Spec(dmStep%6),
              (dmStep%4==0)?TR_HEX:((dmStep%4==1)?TR_IRIS:
              ((dmStep%4==2)?TR_SHOCK:TR_GLITCH))); }
}
void ScreenDemo(float dt){
  // entering ST_DEMO just kicks off the tour
  if (!dmActive) DemoBegin();
  ScreenJee(dt);   // placeholder frame; DemoBegin immediately switches state
}

// #####################################################################
// #   E A S T E R   E G G   E N G I N E   (v6)                        #
// ---------------------------------------------------------------------
//  SAFETY CONTRACT -- every egg obeys all of these:
//    * temporary RAM state only; nothing is written to NVS
//    * never touches calibration, theme, JEE data or settings
//    * always self-terminates on a timer AND can be dismissed by touch
//    * allocates nothing while running (all pools are pre-sized)
//    * cannot change appState permanently -- overlays draw on top
//  Eggs are OVERLAYS: the underlying screen keeps running beneath them,
//  so navigation can never be trapped.
// #####################################################################

static const char *EGG_NAME[EGG_COUNT] = {
  "", "CHAOS MODE", "SINGULARITY", "SYSTEM GLITCH", "MIRROR",
  "RETRO 1983", "DEEP RAIN", "?", "TERMINAL", "DEV MODE", "SUPERNOVA" };

static uint8_t  eggActive = EGG_NONE;
static float    eggT = 0, eggDur = 0;
uint16_t eggFound = 0;         // bitmask, RAM only (resets on reboot)
static float    eggToast = 0;
static char     eggToastMsg[26] = "";

// ---- detectors (all RAM, all cheap) ---------------------------------
static uint8_t  egLogoTaps = 0;
static uint32_t egLogoLast = 0;
static uint8_t  egCornerSeq = 0;      // 4-corner sequence progress
static uint32_t egCornerLast = 0;
static uint8_t  egRapid = 0;          // rapid taps anywhere
static uint32_t egRapidLast = 0;
static uint32_t egHoldStart = 0;      // long-press on dead space
static uint32_t egIdleSince = 0;
static uint8_t  egPageFlips = 0;
static uint32_t egPageLast = 0;
static float    egSpinAccum = 0;      // total 3D rotation travelled

void EggFire(uint8_t id, float dur, const char *msg){
  if (eggActive) return;
  eggActive=id; eggT=0; eggDur=dur;
  eggFound |= (1u<<id);
  snprintf(eggToastMsg,sizeof(eggToastMsg),"%s",msg?msg:EGG_NAME[id]);
  eggToast=2.2f;
  Serial.printf("EGG: %s\n",EGG_NAME[id]);
}
// Any touch dismisses an egg early -- guarantees no trap.
static void EggDismissCheck(void){
  if (eggActive && eggT>0.45f && touchDown) eggT=eggDur;
}

// ---------------------------------------------------------------------
//  DETECTION  -- called once per frame from loop(), before the screen.
//  Deliberately reads only existing touch state; adds no I2C traffic.
// ---------------------------------------------------------------------
static void EggDetect(float dt){
  uint32_t now=millis();
  if (eggActive) return;

  // idle tracker (mascot appears when the device is left alone)
  if (touchActive || touchDown) egIdleSince=now;
  if (egIdleSince==0) egIdleSince=now;

  // --- COMMON: 7 rapid taps on the NEXUS wordmark (home only) ---------
  if (touchDown && appState==ST_HOME && touchX<96 && touchY<24){
    if (now-egLogoLast<650) egLogoTaps++; else egLogoTaps=1;
    egLogoLast=now;
    if (egLogoTaps>=7){ egLogoTaps=0;
      EggFire(EGG_CHAOS,3.6f,"CHAOS UNLEASHED"); return; } }

  // --- COMMON: 12 very fast taps anywhere -----------------------------
  if (touchDown){
    if (now-egRapidLast<260) egRapid++; else egRapid=1;
    egRapidLast=now;
    if (egRapid>=12){ egRapid=0;
      EggFire(EGG_SUPERNOVA,3.2f,"SUPERNOVA"); return; } }

  // --- UNCOMMON: touch 4 corners clockwise from top-left --------------
  if (touchDown){
    const int M=42;
    int c=-1;
    if (touchX<M && touchY<M) c=0;
    else if (touchX>SCREEN_W-M && touchY<M) c=1;
    else if (touchX>SCREEN_W-M && touchY>SCREEN_H-M) c=2;
    else if (touchX<M && touchY>SCREEN_H-M) c=3;
    if (c>=0){
      if (c==egCornerSeq && now-egCornerLast<2600){
        egCornerSeq++; egCornerLast=now;
        if (egCornerSeq>=4){ egCornerSeq=0;
          EggFire(EGG_SINGULARITY,3.4f,"SINGULARITY"); return; } }
      else { egCornerSeq=(c==0)?1:0; egCornerLast=now; } } }

  // --- UNCOMMON: hold dead space at the very bottom-centre for 3s -----
  if (touchActive && touchY>SCREEN_H-16 && touchX>130 && touchX<190){
    if (!egHoldStart) egHoldStart=now;
    else if (now-egHoldStart>3000){ egHoldStart=0;
      EggFire(EGG_TERMINAL,5.0f,"TERMINAL"); return; } }
  else if (!touchActive) egHoldStart=0;

  // --- RARE: flip hub pages 6 times in 3 seconds ----------------------
  if (appState==ST_HOME){
    static int lastPage=-1;
    if (lastPage>=0 && hubPage!=lastPage){
      if (now-egPageLast<3000) egPageFlips++; else egPageFlips=1;
      egPageLast=now;
      if (egPageFlips>=6){ egPageFlips=0;
        EggFire(EGG_GLITCH,2.6f,"REALITY DESYNC"); return; } }
    lastPage=hubPage; }

  // --- RARE: spin a 3D object through ~6 full turns without stopping --
  if (appState==ST_LAB||appState==ST_INSPECT||appState==ST_OBJECTS){
    if (touchActive) egSpinAccum+=fabsf(velY)*dt+fabsf((float)(touchX-lastTX))*0.01f;
    else egSpinAccum*=expf(-0.6f*dt);
    if (egSpinAccum>38.0f){ egSpinAccum=0;
      EggFire(EGG_MIRROR,3.0f,"MIRROR WORLD"); return; } }

  // --- RARE: idle for 25 s -> the mascot wanders in -------------------
  if (now-egIdleSince>25000 && appState!=ST_DEMO && appState!=ST_CALIB){
    egIdleSince=now;
    EggFire(EGG_MASCOT,7.0f,0); return; }

  // --- ULTRA RARE: hold BOTH bottom corners region then tap centre ----
  {
    static uint8_t stage=0; static uint32_t st=0;
    if (touchActive && touchY>SCREEN_H-30 && touchX<40){ if(!stage){stage=1; st=now;} }
    if (stage==1 && touchActive && touchY>SCREEN_H-30 && touchX>SCREEN_W-40) stage=2;
    if (stage==2 && touchDown && touchY>100 && touchY<150 && touchX>130 && touchX<190){
      stage=0; EggFire(EGG_DEVMODE,6.0f,"DEV MODE"); return; }
    if (now-st>5000) stage=0;
  }

  // --- ULTRA RARE: in MATRIX, hold the exact centre for 2.5 s ---------
  if (appState==ST_MATRIX && touchActive &&
      abs(touchX-160)<22 && abs(touchY-120)<22){
    static uint32_t mh=0;
    if (!mh) mh=now;
    else if (now-mh>2500){ mh=0; EggFire(EGG_RAIN,5.0f,"FOLLOW THE RAIN"); return; }
  }

  // --- RARE: retro mode -- triple long-press on the FPS readout -------
  if (touchDown && gShowFps && touchX>SCREEN_W-40 && touchY<20){
    static uint8_t fpsTaps=0; static uint32_t ft=0;
    if (now-ft<900) fpsTaps++; else fpsTaps=1;
    ft=now;
    if (fpsTaps>=5){ fpsTaps=0; EggFire(EGG_RETRO,4.2f,"RETRO 1983"); return; } }
}

// ---------------------------------------------------------------------
//  MASCOT  --  an original stick figure with squash/stretch and
//  anticipation. Used both by the idle egg and the Animation Lab.
//  Pure procedural: no sprites, no allocation.
// ---------------------------------------------------------------------
// struct Stick is defined ABOVE the #include block (Arduino injects
// prototypes for StickDraw/StickUpdate right after the last #include,
// so the type must already be known there).
static Stick mascot;

void StickDraw(const Stick &s,uint16_t c,float t){
  int x=(int)s.x, y=(int)s.y;
  float sq=clampf(s.squash,0.45f,1.7f);
  int H=(int)(17*sq);                    // body height reacts to squash
  int W=(int)(9/sq);                     // and width inversely: volume-ish
  int hx=x+(int)(s.lean*7);
  int hy=y-H-5;
  // head
  CircleFB(hx,hy,4,c,255);
  RingFB(hx,hy,4,C_TEXT,180);
  // eyes look toward travel
  PxBlend(hx+s.face*2,hy-1,C_BG,255);
  // spine
  LineFB(hx,hy+4,x,y-2,c,255);
  // arms swing opposite the legs
  float sw=fsin(s.anim*2.0f)*(s.grounded?1.0f:0.35f);
  int ax=x+(int)(s.lean*3);
  LineFB(ax,y-H+3,ax-W+(int)(sw*5),y-H+11,c,255);
  LineFB(ax,y-H+3,ax+W-(int)(sw*5),y-H+11,c,255);
  // legs
  if (s.grounded){
    LineFB(x,y-2,x-(int)(sw*6)-2,y+6,c,255);
    LineFB(x,y-2,x+(int)(sw*6)+2,y+6,c,255);
  } else {
    LineFB(x,y-2,x-5,y+3,c,255);
    LineFB(x,y-2,x+4,y+6,c,255); }
  Glow(hx,hy,c,60,0.7f);
}
// simple physics + AI: chase a target, jump obstacles, react to taps
void StickUpdate(Stick &s,float dt,float tx,float ty,bool excited){
  const float GROUND=SCREEN_H-26;
  float dx=tx-s.x;
  float want=clampf(dx*2.6f,-95.0f,95.0f);
  if (excited) want*=1.5f;
  s.vx=Approach(s.vx,want,6.0f,dt);
  if (fabsf(s.vx)>3) s.face=(s.vx>0)?1:-1;
  s.vy+=430.0f*dt;
  s.x+=s.vx*dt; s.y+=s.vy*dt;
  s.grounded=false;
  if (s.y>=GROUND){
    // landing: squash then spring back (follow-through)
    if (s.vy>150.0f) s.squash=0.62f;
    s.y=GROUND; s.vy=0; s.grounded=true; }
  // anticipate a jump when the target is well above
  if (s.grounded && ty<s.y-40 && fabsf(dx)<70){
    s.squash=0.7f;                      // crouch = anticipation
    s.vy=-235.0f; }
  if (!s.grounded) s.squash=Approach(s.squash,1.22f,7.0f,dt);   // stretch airborne
  else             s.squash=Approach(s.squash,1.0f,9.0f,dt);    // settle
  s.lean=Approach(s.lean,clampf(s.vx*0.010f,-0.55f,0.55f),8.0f,dt);
  s.anim+=dt*(3.0f+fabsf(s.vx)*0.075f);
  s.x=clampf(s.x,10,SCREEN_W-10);
}

// ---------------------------------------------------------------------
//  EGG RENDERERS  --  each draws OVER the live screen, then expires.
// ---------------------------------------------------------------------
static void EggRender(float dt){
  if (!eggActive) return;
  eggT+=dt;
  float p=clampf(eggT/eggDur,0,1);
  float fadeIn=clampf(eggT/0.35f,0,1);
  float fadeOut=clampf((eggDur-eggT)/0.5f,0,1);
  float a=fminf(fadeIn,fadeOut);

  switch (eggActive){
    case EGG_CHAOS: {
      // everything shakes, hue-cycles and throws sparks -- then settles
      int sh=(int)(fsin(gTime*47.0f)*9*a);
      for (int y=0;y<SCREEN_H;y+=4){
        int off=(int)(fsin(gTime*13.0f+y*0.09f)*10*a);
        if (!off) continue;
        uint16_t *row=&frame[y*SCREEN_W];
        if (off>0) memmove(row+off,row,(SCREEN_W-off)*2);
        else       memmove(row,row-off,(SCREEN_W+off)*2); }
      for (int i=0;i<5;i++){
        float u=gTime*3.0f+i*1.25f;
        int cx=160+(int)(fcos(u)*(90*a)), cy=120+(int)(fsin(u*1.3f)*(64*a));
        Glow(cx,cy,Spec(i),(uint8_t)(150*a),3.0f);
        if (((int)(gTime*20)%7)==i) SpawnBurst(cx,cy,7,Spec(i),190.0f,PK_SPARK); }
      GlowTextC(160+sh,110,"CHAOS",C_WARN,4,(uint8_t)(140*a));
    } break;

    case EGG_SINGULARITY: {
      // every pixel row gets pulled toward the centre, then blows out
      float pull=(p<0.62f)?EaseInCubic(p/0.62f):0.0f;
      float blast=(p>=0.62f)?EaseOutQuint((p-0.62f)/0.38f):0.0f;
      for (int y=0;y<SCREEN_H;y+=2){
        float dy=(y-120)/120.0f;
        int off=(int)(dy*-46.0f*pull + dy*70.0f*blast);
        if (!off) continue;
        int sy=clampi(y+off,0,SCREEN_H-1);
        if (sy!=y) memcpy(&frame[y*SCREEN_W],&frame[sy*SCREEN_W],SCREEN_W*2); }
      int r=(int)((1.0f-pull)*70+blast*300);
      RingFB(160,120,r,C_HILITE,(uint8_t)(230*a));
      RingFB(160,120,r-5,Spec(0),(uint8_t)(150*a));
      CircleFB(160,120,(int)(16*(1.0f-pull)+blast*8),C_TEXT,(uint8_t)(255*a));
      Glow(160,120,C_HILITE,(uint8_t)(200*a),4.0f);
      if (p>0.60f&&p<0.66f) SpawnBurst(160,120,46,C_HILITE,300.0f,PK_SPARK);
    } break;

    case EGG_GLITCH: {
      // controlled, obviously-fake corruption that always self-heals
      for (int i=0;i<9;i++){
        int by=(int)(Hash(i*911u+(int)(gTime*11))*SCREEN_H);
        int bh=4+(int)(Hash(i*77u)*14);
        int off=(int)((Hash(i*333u+(int)(gTime*17))-0.5f)*86*a);
        for (int y=by;y<by+bh&&y<SCREEN_H;y++){
          uint16_t *row=&frame[y*SCREEN_W];
          if (off>0) memmove(row+off,row,(SCREEN_W-off)*2);
          else if (off<0) memmove(row,row-off,(SCREEN_W+off)*2); }
        if (Hash(i*17u+(int)(gTime*9))>0.72f)
          BlendRectFB(0,by,SCREEN_W,bh,Spec(i%6),(uint8_t)(120*a)); }
      if (((int)(gTime*14))&1) BlendRectFB(0,0,SCREEN_W,SCREEN_H,C_WARN,(uint8_t)(22*a));
      DrawText(8,SCREEN_H/2-4,"SIGNAL LOSS",Fade(C_WARN,(uint8_t)(255*a)),2);
      DrawText(8,SCREEN_H/2+14,"RECOVERING",Fade(C_SAND,(uint8_t)(200*a)),1);
    } break;

    case EGG_MIRROR: {
      // mirror the left half onto the right through a moving seam
      int seam=(int)(160+fsin(gTime*1.2f)*40);
      for (int y=0;y<SCREEN_H;y++){
        uint16_t *row=&frame[y*SCREEN_W];
        for (int x=0;x<SCREEN_W-seam && x<seam;x++){
          int src=seam-1-x, dst=seam+x;
          if (dst<SCREEN_W && src>=0) BlendInto(&row[dst],row[src],(uint8_t)(255*a)); } }
      VLineFB(seam,0,SCREEN_H,C_HILITE);
      Glow(seam,120,C_HILITE,(uint8_t)(120*a),3.0f);
      GlowTextC(160,14,"MIRROR",C_HILITE,1,(uint8_t)(90*a));
    } break;

    case EGG_RETRO: {
      // 1983 phosphor terminal: quantise to green, add scanlines + curve
      for (int y=0;y<SCREEN_H;y++){
        uint16_t *row=&frame[y*SCREEN_W];
        bool scan=((y&1)==0);
        for (int x=0;x<SCREEN_W;x+=1){
          uint16_t c=row[x];
          int lum=(((c>>11)&0x1F)*77+((c>>5)&0x3F)*75+(c&0x1F)*29)>>7;
          lum=clampi(lum,0,63);
          int step=(lum>>3)<<3;                    // 8 levels: chunky CRT
          uint16_t g=RGB565(step,step*4,step);
          if (scan) g=Dim(g,3,5);
          BlendInto(&row[x],g,(uint8_t)(235*a)); } }
      // vignette corners for tube curvature
      for (int i=0;i<26;i++){
        uint8_t v=(uint8_t)((26-i)*7*a);
        HLineFB(0,i,SCREEN_W,Fade(C_BG,v));
        HLineFB(0,SCREEN_H-1-i,SCREEN_W,Fade(C_BG,v)); }
      DrawText(6,6,"NEXUS-1983 READY",RGB565(60,240,60),1);
      if (((millis()/400)%2)==0) DrawText(6,18,"_",RGB565(60,240,60),1);
    } break;

    case EGG_RAIN: {
      // dense luminous rain that briefly swallows the UI
      Scrim((uint8_t)(210*a));
      static const char GL[]="01+-<>*#$%XZ";
      for (int i=0;i<52;i++){
        int x=(i*6+3)%SCREEN_W;
        float sp=60+Hash(i*331u)*260;
        float y=fmodf(gTime*sp+Hash(i*77u)*400,(float)(SCREEN_H+90))-40;
        for (int k=0;k<9;k++){
          int yy=(int)y-k*9;
          if (yy<0||yy>SCREEN_H) continue;
          char sb[2]={GL[(int)(Hash(i*13u+k*7u+(int)(gTime*7))*12)%12],0};
          uint16_t c=(k==0)?C_TEXT:Fade(Spec(3),(uint8_t)((200-k*22)*a));
          DrawText(x,yy,sb,c,1); } }
      GlowTextC(160,116,"WAKE UP",C_TEXT,3,(uint8_t)(120*a));
    } break;

    case EGG_SUPERNOVA: {
      // one enormous particle bloom, budget-capped
      if (eggT<0.1f){
        for (int k=0;k<5;k++) SpawnBurst(160,120,40,Spec(k),340.0f,PK_SPARK);
        SpawnBurst(160,120,40,C_HILITE,180.0f,PK_EMBER); }
      float g=EaseOutQuint(p);
      for (int i=0;i<4;i++){
        int r=(int)(g*(150+i*46));
        RingFB(160,120,r,i&1?C_HILITE:Spec(i),(uint8_t)(200*(1.0f-p)*a)); }
      Glow(160,120,C_HILITE,(uint8_t)(220*(1.0f-p)*a),6.0f);
    } break;

    case EGG_MASCOT: {
      // the mascot strolls in, notices your finger, chases it, waves goodbye
      if (eggT<dt*2){ mascot.x=-12; mascot.y=SCREEN_H-26;
        mascot.vx=0; mascot.vy=0; mascot.squash=1; mascot.face=1;
        mascot.anim=0; mascot.lean=0; }
      float tx = touchActive ? (float)touchX : (40.0f+fsin(gTime*0.8f)*90.0f+120.0f);
      float ty = touchActive ? (float)touchY : (float)(SCREEN_H-26);
      if (p>0.86f) tx=SCREEN_W+30;                 // exit stage right
      StickUpdate(mascot,dt,tx,ty,touchActive);
      // shadow grounds the character
      if (mascot.grounded)
        BlendRectFB((int)mascot.x-7,SCREEN_H-21,14,3,C_BG,140);
      StickDraw(mascot,C_HILITE,gTime);
      if (touchActive){
        // surprise marks when you poke near it
        float d=fabsf(mascot.x-touchX);
        if (d<40){
          DrawText((int)mascot.x+8,(int)mascot.y-40,"!",C_WARN,2);
          Glow((int)mascot.x+10,(int)mascot.y-36,C_WARN,110,0.8f); } }
    } break;

    case EGG_TERMINAL: {
      Scrim((uint8_t)(215*a));
      Panel(10,26,300,188,"NEXUS SHELL",C_ACCENT,"ROOT");
      static const char *L[10]={
        "> sys.identify()",
        "  NEXUS OS " NEXUS_VER_STR "  ESP32-S3 N16R8",
        "> render.stats()",
        "  75 screens / 15 solids / 13 modes",
        "> eggs.count()",
        "  11 hidden routines compiled in",
        "> hint()",
        "  corners... rapid taps... patience...",
        "> exit",
        "  session closed" };
      int shown=(int)(eggT*4.2f);
      for (int i=0;i<10&&i<shown;i++)
        DrawText(20,46+i*16,L[i],(L[i][0]=='>')?C_HILITE:C_DATA,1);
      if (shown<10 && ((millis()/300)%2)==0)
        DrawText(20+TextW(L[shown<10?shown:9],1),46+(shown)*16,"_",C_HILITE,1);
    } break;

    case EGG_DEVMODE: {
      Scrim((uint8_t)(200*a));
      Panel(6,22,308,196,"DEVELOPER",C_WARN,"DBG");
      char b[52]; int y=44;
      snprintf(b,sizeof(b),"STATE      %d / %d",(int)appState,(int)ST_COUNT); DrawText(16,y,b,C_DATA,1); y+=13;
      snprintf(b,sizeof(b),"FPS        %.1f  (%.1f ms)",fpsValue,frameMs);    DrawText(16,y,b,C_DATA,1); y+=13;
      snprintf(b,sizeof(b),"HEAP       %u B",(unsigned)ESP.getFreeHeap());    DrawText(16,y,b,C_DATA,1); y+=13;
      snprintf(b,sizeof(b),"PSRAM FREE %u B",(unsigned)ESP.getFreePsram());   DrawText(16,y,b,C_DATA,1); y+=13;
      snprintf(b,sizeof(b),"GEOMETRY   %u v / %u t",(unsigned)vTop,(unsigned)tTop); DrawText(16,y,b,C_DATA,1); y+=13;
      snprintf(b,sizeof(b),"TOUCH      %d,%d %s",touchX,touchY,touchActive?"DOWN":"UP"); DrawText(16,y,b,C_DATA,1); y+=13;
      snprintf(b,sizeof(b),"CAL        %s",calLoaded?"CUSTOM":"DEFAULT");     DrawText(16,y,b,C_DATA,1); y+=13;
      snprintf(b,sizeof(b),"DEPTH BUF  %s",gDepthInternal?"SRAM":"PSRAM");    DrawText(16,y,b,C_DATA,1); y+=16;
      // egg discovery board -- the only place progress is ever shown
      DrawText(16,y,"HIDDEN ROUTINES FOUND",C_SAND,1); y+=13;
      int found=0;
      for (int i=1;i<EGG_COUNT;i++){
        bool f=(eggFound>>i)&1;
        if (f) found++;
        int bx=16+((i-1)%6)*46, by=y+((i-1)/6)*16;
        BlendRectFB(bx,by,42,12,f?Dim(C_ACCENT,2,5):Dim(C_HAIR,3,5),A_FILL);
        DrawText(bx+3,by+3,f?EGG_NAME[i]:"?????",f?C_TEXT:C_HAIR,1); }
      snprintf(b,sizeof(b),"%d / %d",found,EGG_COUNT-1);
      DrawText(250,y+8,b,C_HILITE,1);
      // the overlay is the doorway: tap ENTER to stay in the full dev room
      { bool hov=touchActive&&touchX>240&&touchX<306&&touchY>192&&touchY<212;
        BlendRectFB(240,192,66,18,hov?Dim(C_WARN,2,5):Dim(C_WARN,1,6),A_FILL);
        Bracket(240,192,66,18,C_WARN,5);
        DrawTextC(273,197,"ENTER",hov?C_TEXT:C_SAND,1);
        if (touchDown&&hov&&transT==0){
          eggActive=EGG_NONE; eggT=0;
          GoTo(ST_DEVROOM,273,201,C_WARN,TR_GLITCH); return; } }
    } break; }

  // universal exit affordance
  if (eggT>0.5f && eggActive!=EGG_MASCOT)
    DrawTextC(160,SCREEN_H-8,"TAP TO DISMISS",Fade(C_SAND,(uint8_t)(150*a)),1);
  EggDismissCheck();
  if (eggT>=eggDur){ eggActive=EGG_NONE; eggT=0; }
}
// small non-intrusive toast when something is discovered
static void EggToast(float dt){
  if (eggToast<=0) return;
  eggToast-=dt;
  float a=clampf(eggToast/0.6f,0,1);
  int w=TextW(eggToastMsg,1)+22;
  int x=(SCREEN_W-w)/2, y=24;
  BlendRectFB(x,y,w,16,C_PANEL,(uint8_t)(A_FILL*a));
  Bracket(x,y,w,16,C_HILITE,4);
  DrawText(x+11,y+5,eggToastMsg,Fade(C_TEXT,(uint8_t)(255*a)),1);
  Glow(x+w/2,y+8,C_HILITE,(uint8_t)(70*a),2.0f);
}

// =====================================================================
//  ANIMATION LAB  --  original stick-figure scenes built on real
//  animation principles: anticipation, impact, follow-through, settle.
//  Reachable from SYSTEM (long-press the title) -- not advertised.
// =====================================================================
static int   alScene=0;
static float alT=0;
static bool  alAuto=true;
#define AL_SCENES 6
static const char *AL_NAME[AL_SCENES]={
  "IMMOVABLE CUBE","THE WALL","PORTAL DROP","SPARK CHASE","LOGO LIFT","LAUNCH" };

// scene-local actors, reused every scene (no allocation)
static Stick alS;
static float alBoxX,alBoxY,alBoxVX,alBoxSquash;
static float alShake;

static void AlReset(void){
  alT=0;
  alS.x=54; alS.y=SCREEN_H-26; alS.vx=0; alS.vy=0;
  alS.squash=1; alS.lean=0; alS.face=1; alS.anim=0; alS.grounded=true;
  alBoxX=210; alBoxY=SCREEN_H-40; alBoxVX=0; alBoxSquash=1;
  alShake=0;
}
static void AlGround(void){
  HLineFB(0,SCREEN_H-20,SCREEN_W,C_HAIR);
  for (int x=0;x<SCREEN_W;x+=18)
    LineFB(x,SCREEN_H-19,x+9,SCREEN_H-11,Dim(C_HAIR,3,5),160);
}
static void AlBox(float x,float y,float sq,uint16_t c){
  int w=(int)(30/sq), h=(int)(30*sq);
  BlendRectFB((int)x-w/2,(int)y-h,w,h,c,A_FILL);
  Bracket((int)x-w/2,(int)y-h,w,h,C_TEXT,5);
  Glow((int)x,(int)y-h/2,c,50,1.4f);
}
void ScreenAnimLab(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  alT+=dt;
  alShake=Approach(alShake,0,7.0f,dt);
  Backdrop();
  int sh=(int)(fsin(gTime*54.0f)*alShake);

  switch (alScene){
    case 0: {  // IMMOVABLE CUBE: strain, strain, sudden launch, get dragged
      AlGround();
      float t=alT;
      if (t<1.1f){                       // approach
        alS.x=Approach(alS.x,alBoxX-26,3.0f,dt); alS.face=1;
        alS.anim+=dt*7; alS.lean=0.22f;
      } else if (t<3.4f){                // straining: tiny shakes, big lean
        alS.lean=0.52f+fsin(t*26.0f)*0.05f;
        alS.squash=0.92f+fsin(t*24.0f)*0.03f;
        alBoxX+=fsin(t*30.0f)*0.35f;
        if (((int)(t*9))%3==0) SpawnBurst(alBoxX-16,SCREEN_H-24,1,C_SAND,25.0f,PK_SPARK);
        DrawText((int)alS.x-6,(int)alS.y-52,"...",C_SAND,1);
      } else if (t<3.62f){               // ANTICIPATION: wind back
        alS.lean=-0.30f; alS.squash=0.74f;
      } else if (t<5.4f){                // IMPACT + follow-through
        if (alBoxVX==0){ alBoxVX=430; alBoxSquash=0.66f; alShake=7.0f;
          SpawnBurst(alBoxX,SCREEN_H-34,26,C_HILITE,240.0f,PK_SPARK); }
        alBoxVX*=expf(-1.5f*dt);
        alBoxX+=alBoxVX*dt;
        alBoxSquash=Approach(alBoxSquash,1.0f,7.0f,dt);
        alS.x+=alBoxVX*dt*0.42f;         // dragged along
        alS.lean=0.85f; alS.squash=1.14f;
        alS.anim+=dt*16;
      } else { alS.lean=Approach(alS.lean,0,5.0f,dt);
               alS.squash=Approach(alS.squash,1.0f,6.0f,dt); }
      if (alBoxX>SCREEN_W+30) alBoxX=SCREEN_W+30;
      AlBox(alBoxX+sh,SCREEN_H-20,alBoxSquash,Spec(4));
      alS.y=SCREEN_H-26;
      StickDraw(alS,C_HILITE,gTime);
      if (alT>7.0f&&alAuto){ alScene=(alScene+1)%AL_SCENES; AlReset(); }
    } break;

    case 1: {  // THE WALL: walk, bonk, ponder, charge, get launched
      AlGround();
      float t=alT;
      int wx=250;
      BlendRectFB(wx,60,16,SCREEN_H-80,Dim(C_SAND,3,5),A_FILL);
      Bracket(wx,60,16,SCREEN_H-80,C_SAND,6);
      if (t<1.5f){ alS.x=Approach(alS.x,wx-16,2.4f,dt); alS.anim+=dt*8; alS.face=1; }
      else if (t<1.62f){ alShake=6.0f; alS.squash=0.66f;
        if (alBoxVX==0){ alBoxVX=1; SpawnBurst(wx-8,alS.y-24,14,C_WARN,150.0f,PK_SPARK); } }
      else if (t<2.9f){                  // rub head, look up
        alS.squash=Approach(alS.squash,1.0f,6.0f,dt);
        alS.x=Approach(alS.x,wx-30,3.0f,dt);
        DrawText(wx-52,(int)alS.y-54,"?",C_SAND,2);
      } else if (t<3.15f){ alS.squash=0.70f; alS.lean=-0.35f; }  // anticipation
      else if (t<4.8f){                  // charge and rebound
        alS.x=Approach(alS.x,wx-14,7.0f,dt); alS.lean=0.5f;
        if (alS.x>wx-19 && alS.vx>-10){
          alS.vx=-330; alS.vy=-190; alShake=9.0f;
          SpawnBurst(wx-8,alS.y-22,30,C_WARN,260.0f,PK_SPARK); }
        alS.vx*=expf(-1.1f*dt); alS.vy+=430*dt;
        alS.x+=alS.vx*dt; alS.y+=alS.vy*dt;
        if (alS.y>SCREEN_H-26){ alS.y=SCREEN_H-26; alS.vy=0; alS.squash=0.7f; }
        alS.squash=Approach(alS.squash,1.0f,7.0f,dt);
      } else { alS.y=SCREEN_H-26; alS.lean=Approach(alS.lean,0,4.0f,dt); }
      StickDraw(alS,C_HILITE,gTime);
      if (alT>6.5f&&alAuto){ alScene=(alScene+1)%AL_SCENES; AlReset(); }
    } break;

    case 2: {  // PORTAL DROP: fall in, pause, spat out the top
      float t=alT;
      int px=160, py=SCREEN_H-24;
      for (int i=0;i<3;i++){
        float ph=fmodf(gTime*0.9f+i*0.33f,1.0f);
        RingFB(px,py,(int)(10+ph*26),Spec(5),(uint8_t)(220*(1.0f-ph))); }
      RingFB(px,40,22,Spec(2),200);
      if (t<1.6f){ alS.x=Approach(alS.x,px,2.6f,dt); alS.anim+=dt*8; alS.y=SCREEN_H-26; }
      else if (t<2.0f){ alS.squash=0.72f; }                 // anticipation
      else if (t<2.9f){ alS.y+=340*dt; alS.squash=1.3f; }   // fall in, stretch
      else if (t<3.3f){ alS.y=-40; }                        // gone
      else if (t<5.0f){                                     // spat from the top
        if (alS.vy==0) alS.vy=-40;
        alS.y=Approach(alS.y,SCREEN_H-26,2.6f,dt);
        alS.x=px; alS.squash=1.18f;
        if (alS.y>SCREEN_H-34){ alS.squash=0.68f; }
      } else { alS.y=SCREEN_H-26; alS.squash=Approach(alS.squash,1.0f,6.0f,dt); }
      AlGround();
      if (alS.y>-30&&alS.y<SCREEN_H+30) StickDraw(alS,C_HILITE,gTime);
      if (alT>6.2f&&alAuto){ alScene=(alScene+1)%AL_SCENES; AlReset(); }
    } break;

    case 3: {  // SPARK CHASE: particles hunt the figure, it flees + dodges
      AlGround();
      float tx=(touchActive)?(float)touchX:160+fsin(alT*1.1f)*110;
      float ty=(touchActive)?(float)touchY:SCREEN_H-26;
      // the figure runs AWAY from the pointer
      float flee=alS.x+(alS.x-tx);
      StickUpdate(alS,dt,clampf(flee,14,SCREEN_W-14),ty,true);
      for (int i=0;i<7;i++){
        float u=alT*2.4f+i*0.9f;
        float rr=26+fsin(alT*1.7f+i)*13;
        int cx=(int)(tx+fcos(u)*rr), cy=(int)(ty+fsin(u)*rr*0.6f);
        PxAdd(cx,cy,Spec(i%6),230);
        LineAdd(cx,cy,(int)tx,(int)ty,Spec(i%6),50);
        Glow(cx,cy,Spec(i%6),70,0.6f); }
      if (fabsf(alS.x-tx)<34)
        DrawText((int)alS.x+8,(int)alS.y-42,"!",C_WARN,2);
      StickDraw(alS,C_HILITE,gTime);
      DrawTextC(160,26,"DRAG - IT RUNS FROM YOUR FINGER",C_SAND,1);
      if (alT>9.0f&&alAuto){ alScene=(alScene+1)%AL_SCENES; AlReset(); }
    } break;

    case 4: {  // LOGO LIFT: strain under the NEXUS wordmark, then press it up
      AlGround();
      float t=alT;
      float lift=0;
      if (t>1.4f&&t<2.6f) lift=EaseOutCubic((t-1.4f)/1.2f)*26.0f;
      else if (t>=2.6f)   lift=26.0f+fsin(t*3.0f)*1.5f;
      int ly=64-(int)lift;
      GlowText(96,ly,"NEXUS",C_ACCENT,3,110);
      if (t<1.4f){ alS.x=Approach(alS.x,150,2.6f,dt); alS.anim+=dt*7;
        alS.squash=0.9f; alS.lean=0; }
      else { alS.x=150; alS.squash=0.80f+fsin(t*17.0f)*0.035f;
        // arms up: draw explicit strain arms over the figure
        LineFB(150-9,(int)alS.y-24,150-13,ly+22,C_HILITE,255);
        LineFB(150+9,(int)alS.y-24,150+13,ly+22,C_HILITE,255);
        if (((int)(t*8))%2==0) SpawnBurst(150,alS.y-30,1,C_SAND,20.0f,PK_SPARK); }
      alS.y=SCREEN_H-26;
      StickDraw(alS,C_HILITE,gTime);
      if (alT>6.0f&&alAuto){ alScene=(alScene+1)%AL_SCENES; AlReset(); }
    } break;

    default: { // LAUNCH: crouch, explode, arc across, skid, settle
      AlGround();
      float t=alT;
      if (t<1.0f){ alS.x=48; alS.y=SCREEN_H-26; alS.anim+=dt*5; alS.squash=1.0f; }
      else if (t<1.5f){ alS.squash=0.60f; alS.lean=-0.2f; }   // deep anticipation
      else if (t<1.58f){
        if (alS.vy==0){ alS.vy=-330; alS.vx=205; alShake=8.0f;
          SpawnBurst(alS.x,SCREEN_H-22,40,C_HILITE,300.0f,PK_SPARK);
          SpawnBurst(alS.x,SCREEN_H-22,18,Spec(1),180.0f,PK_EMBER); } }
      if (t>=1.5f){
        alS.vy+=430*dt;
        alS.x+=alS.vx*dt; alS.y+=alS.vy*dt;
        alS.squash=clampf(1.0f+alS.vy*-0.0014f,0.7f,1.35f);
        alS.lean=clampf(alS.vx*0.0022f,0,0.7f);
        if (alS.y>SCREEN_H-26){
          alS.y=SCREEN_H-26;
          if (alS.vy>120){ alShake=6.0f; alS.squash=0.62f;
            SpawnBurst(alS.x,SCREEN_H-22,16,C_SAND,140.0f,PK_SPARK); }
          alS.vy=0; alS.vx*=0.72f; }
        alS.x=clampf(alS.x,12,SCREEN_W-12); }
      StickDraw(alS,C_HILITE,gTime);
      if (alT>6.0f&&alAuto){ alScene=(alScene+1)%AL_SCENES; AlReset(); }
    } break; }

  // scene chrome
  if (Button(4,22,26,16,"<",C_ACCENT,false)){
    alScene=(alScene+AL_SCENES-1)%AL_SCENES; AlReset(); }
  if (Button(290,22,26,16,">",C_ACCENT,false)){
    alScene=(alScene+1)%AL_SCENES; AlReset(); }
  GlowTextC(160,24,AL_NAME[alScene],C_ACCENT,1,70);
  if (Button(4,SCREEN_H-18,58,16,alAuto?"AUTO":"MANUAL",C_ACCENT,alAuto)) alAuto=!alAuto;
  if (Button(66,SCREEN_H-18,50,16,"REPLAY",C_ACCENT,false)) AlReset();
  { char b[20]; snprintf(b,sizeof(b),"%d / %d",alScene+1,AL_SCENES);
    DrawText(SCREEN_W-46,SCREEN_H-14,b,C_SAND,1); }
  DrawParticles();
  TopBar("ANIM LAB",C_ACCENT);
  EnterOverlay();
}
// =====================================================================
//  BOOT  (the original v2 sequence you preferred)
// =====================================================================
void BootSequence(void){
  const float DUR=3.7f;
  uint32_t t0=millis();
  float t=0;
  bool ig=false;
  while (t<DUR){
    t=(millis()-t0)*0.001f;
    gTime=t;
    fbIndex^=1; frame=fb[fbIndex];
    memset(frame,0,FB_BYTES);
    memset(depth,0,FB_BYTES);
    float p0=clampf(t/0.55f,0,1);
    int sw=(int)(EaseInOutCubic(p0)*SCREEN_H);
    for (int y=8;y<SCREEN_H;y+=16){
      if (y>sw) break;
      for (int x=8;x<SCREEN_W;x+=16) PxBlend(x,y,C_HAIR,A_GLOW); }
    if (p0<1.0f){
      BlendRectFB(0,sw,SCREEN_W,2,C_HILITE,A_FILL);
      BlendRectFB(0,sw-6,SCREEN_W,6,C_ACCENT,60);
      for (int i=0;i<6;i++) Glow(i*64+32,sw,C_ACCENT,120,1.4f); }
    float p1=clampf((t-0.5f)/0.9f,0,1);
    if (p1>0&&p1<1.0f)
      for (int i=0;i<30;i++){
        float a=TAU*i/30.0f+t*1.2f;
        float rad=(1.0f-EaseOutCubic(p1))*150.0f+30.0f;
        int px=160+(int)(fcos(a)*rad), py=96+(int)(fsin(a)*rad*0.7f);
        PxAdd(px,py,C_ACCENT,(uint8_t)(255*p1));
        if (p1>0.5f) Glow(px,py,C_ACCENT,(uint8_t)(90*p1),0.5f); }
    float p2=clampf((t-1.3f)/0.8f,0,1);
    if (p2>0){
      float ring=EaseOutCubic(p2);
      ArcFB(160,96,(int)(ring*50),t*8.0f,t*8.0f+2.4f,C_ACCENT,A_FILL);
      ArcFB(160,96,(int)(ring*44),-t*6.0f,-t*6.0f+1.8f,C_DATA,A_GLOW);
      RingFB(160,96,(int)(ring*58),C_HAIR,A_GLOW);
      ApplyLight();
      RenderMesh(gMesh[3],t*3.0f,t*5.0f,0,0.78f*EaseOutBack(p2),
                 160.0f,96.0f,3.0f,M_NEON,C_ACCENT);
      Glow(160,96,C_ACCENT,(uint8_t)(70*p2),3.0f); }
    float p3=clampf((t-2.05f)/0.85f,0,1);
    if (p3>0){
      DrawTextDecode(160-TextW("NEXUS OS",3)/2,142,"NEXUS OS",C_ACCENT,3,p3);
      if (p3>0.9f) Glow(160,152,C_ACCENT,60,3.0f); }
    float p4=clampf((t-2.85f)/0.55f,0,1);
    if (p4>0){
      DrawTextDecode(160-TextW("ESP32-S3 N16R8",1)/2,170,"ESP32-S3 N16R8",
                     C_TEXT,1,clampf(p4*2.0f,0,1));
      DrawTextDecode(160-TextW("ST7789 320X240   CST328 TOUCH",1)/2,182,
                     "ST7789 320X240   CST328 TOUCH",C_SAND,1,clampf(p4*1.6f-0.2f,0,1));
      char ps[32];
      snprintf(ps,sizeof(ps),"PSRAM %d MB READY",(int)(ESP.getPsramSize()/1048576));
      DrawTextDecode(160-TextW(ps,1)/2,194,ps,C_DATA,1,clampf(p4*1.4f-0.4f,0,1));
      int bw=(int)(196*EaseInOutCubic(p4));
      HLineFB(62,212,196,C_HAIR);
      FillRectFB(62,212,bw,3,C_ACCENT);
      Glow(62+bw,213,C_HILITE,160,0.8f); }
    if (t>3.38f){
      if (!ig){ ig=true;
        SpawnBurst(160,96,50,C_HILITE,260.0f,PK_SPARK);
        SpawnBurst(160,96,34,C_ACCENT,170.0f,PK_EMBER); }
      float fp=clampf((t-3.38f)/0.32f,0,1);
      CircleFB(160,96,(int)(EaseInCubic(fp)*420),C_HILITE,(uint8_t)(A_FILL*fp)); }
    UpdateParticles(FIXED_DT);
    DrawParticles();
    Bracket(6,6,308,228,C_ACCENT,14);
    PushFrame(); }
  gTime=0;
}

// =====================================================================
//  SETUP
// =====================================================================
void setup(){
  Serial.begin(115200);
  delay(500);
  LoadSettings();
  AchLoad();
  CreatorLoad();
  ApplyTheme(gTheme);
  // NOTE: the backlight LEDC attach happens AFTER tft.init() below.
  // TFT_eSPI reclaims TFT_BL as a plain GPIO during init and would
  // silently detach the PWM channel if we attached it here.
  Serial.println();
  Serial.println("==============================");
  Serial.println("ESP32-S3 3D GRAPHICS ENGINE");
  Serial.println("==============================");
  Serial.println();
  bool hasPsram=psramFound();
  Serial.println("PSRAM:");
  Serial.println(hasPsram?"FOUND":"NOT FOUND");
  Serial.println();
  Serial.println("PSRAM SIZE:");
  Serial.printf("%u bytes\n",(unsigned)ESP.getPsramSize());
  Serial.println();
  for (int i=0;i<TRIG_SIZE;i++) sinTab[i]=sinf(TAU*i/TRIG_SIZE);
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  // Backlight PWM must be (re)attached after tft.init(), which drives
  // TFT_BL as a plain output and detaches any prior LEDC binding.
  BacklightAttach();
  SetBrightness(gBright);
  // DMA bands must exist before the first PushFrame (the boot sequence
  // pushes frames). initDMA() reports whether the driver took the config.
  bool dmaOk = tft.initDMA();
  if (!DmaBandsAlloc())
    Serial.println("DMA band alloc failed -> CPU push fallback");
  if (!dmaOk){
    gDmaReady = false;
    Serial.println("TFT initDMA() refused -> CPU push fallback"); }
  tft.fillScreen(TFT_BLACK);
  Serial.printf("Display: %d x %d (ST7789, write-only SPI)\n",tft.width(),tft.height());
#ifdef SPI_FREQUENCY
  { float ms=(FB_BYTES*8.0f)/(float)SPI_FREQUENCY*1000.0f;
    Serial.printf("SPI %u Hz -> frame push %.1f ms -> ceiling %.0f FPS\n",
                  (unsigned)SPI_FREQUENCY,ms,1000.0f/ms);
    if (SPI_FREQUENCY<40000000UL)
      Serial.println("WARNING: raise SPI_FREQUENCY to 80000000 in User_Setup.h"); }
#endif
  Wire.begin(CST328_SDA_PIN,CST328_SCL_PIN,I2C_HZ);
  CST328_Reset();
  pinMode(CST328_INT_PIN,INPUT);
  Serial.println("Touch: CST328 @ 0x1A ready");
  if (!hasPsram||!AllocBuffers()){
    Serial.println("\n!!! PSRAM ALLOCATION FAILED !!!");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED,TFT_BLACK);
    tft.drawString("PSRAM ALLOCATION FAILED",10,100,4);
    while (true) delay(1000); }
  frame=fb[0];
  memset(accum,0,FB_BYTES);
  for (int i=0;i<FB_PIXELS;i++) canvas[i]=C_BG;
  BuildGlowLUT();
  BuildAll();
  Serial.printf("Geometry: %u/%d verts, %u/%d tris [%s]\n",
                (unsigned)vTop,MAX_VERTS,(unsigned)tTop,MAX_TRIS,
                (vTop<=MAX_VERTS&&tTop<=MAX_TRIS)?"OK":"OVERFLOW");
  Serial.printf("Depth buffer in %s\n",gDepthInternal?"INTERNAL SRAM":"PSRAM");
  Serial.printf("Depth buffer in %s\n", gDepthInternal?"INTERNAL SRAM":"PSRAM");
  Serial.printf("Free heap %u | Free PSRAM %u\n",
                (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
  Reset2048(); ResetBreak(); ResetFlappy(); ResetSnake(); ResetPong();
  ResetTetris(); ResetMemory(); ResetSimon(); ResetMines(); ResetWhack();
  ResetDodge(); ResetLights(); ResetMaze();
  // Always come up on a network: prefer the stored SSID, else the
  // compiled-in one. NetLoop() keeps retrying if this first try fails.
  if (!wifiSsid[0] && DEF_SSID[0]){
    snprintf(wifiSsid,sizeof(wifiSsid),"%s",DEF_SSID);
    snprintf(wifiPass,sizeof(wifiPass),"%s",DEF_PASS); }
  JeeInit();

  if (wifiSsid[0]) NetBegin(wifiSsid,wifiPass);
  else Serial.println("WiFi: no SSID configured");
  BootSequence();
  enterAnim=0;
  fpsTimer=millis();
  Serial.println("\nNEXUS OS " NEXUS_VER_STR " ready.");
}

// =====================================================================
//  v7 EXPANSION  --  SHARED SYSTEMS
//  Achievements, bullet-time, impact camera, gesture recognition,
//  UI physics and magnetism. All bounded, all preallocated.
// =====================================================================

// ---- ACHIEVEMENTS ---------------------------------------------------
#define ACH_N 12
struct AchDef { const char *name; const char *hint; };
static const AchDef ACH[ACH_N] = {
  { "FIRST LIGHT",        "BOOT NEXUS"                 },
  { "DIGITAL ARCHAEOLOG", "FIND 5 HIDDEN THINGS"       },
  { "PARTICLE GOD",       "RUN A HUGE PARTICLE FIELD"  },
  { "FRACTAL EXPLORER",   "DIVE DEEP INTO A FRACTAL"   },
  { "TOUCH MASTER",       "LIVE IN THE TOUCH LAB"      },
  { "NEXUS PILOT",        "VISIT EVERY SECTION"        },
  { "GEOMETER",           "MORPH BETWEEN ALL SHAPES"   },
  { "SIGN LANGUAGE",      "DRAW EVERY GESTURE"         },
  { "WORLD BUILDER",      "RUN A CREATOR SCENE"        },
  { "SWARM KEEPER",       "TEND A LIVING SIMULATION"   },
  { "TIME BENDER",        "SLOW THE WORLD DOWN"        },
  { "ARCHITECT",          "SAVE A SCENE PRESET"        }
};
enum { A_BOOT=0, A_EGGS, A_PARTS, A_FRAC, A_TOUCH, A_PILOT,
       A_MORPH, A_GEST, A_CREATE, A_SWARM, A_SLOW, A_PRESET };
static uint16_t achBits   = 0;         // one bit per achievement
static uint8_t  achToastI = 0xFF;
static float    achToastT = 0;
static bool     achDirty  = false;
static uint32_t achSaveMs = 0;
static uint32_t visitMask[2] = {0,0}; // ST_COUNT bits, for NEXUS PILOT

void AchSave(void){
  prefs.begin(NVS_NS,false);
  prefs.putUShort("ach",achBits);
  prefs.putUInt("vis0",visitMask[0]);
  prefs.putUInt("vis1",visitMask[1]);
  prefs.end();
  achDirty=false;
}
void AchLoad(void){
  prefs.begin(NVS_NS,true);
  achBits    = prefs.getUShort("ach",0);
  visitMask[0]=prefs.getUInt("vis0",0);
  visitMask[1]=prefs.getUInt("vis1",0);
  prefs.end();
}
// Grant is idempotent and never writes flash inline -- it only marks dirty.
void AchGrant(int id){
  if (id<0||id>=ACH_N) return;
  if (achBits & (1u<<id)) return;
  achBits |= (1u<<id);
  achToastI=(uint8_t)id; achToastT=0;
  achDirty=true;
  SpawnBurst(160,34,22,C_HILITE,150.0f,PK_SPARK);
}
static inline bool AchHas(int id){ return (achBits>>id)&1u; }
static inline int AchCount(void){
  int n=0; for (int i=0;i<ACH_N;i++) if (AchHas(i)) n++; return n;
}
// mark the current screen visited; award NEXUS PILOT at full coverage
void AchVisit(int st){
  if (st<0||st>=64) return;
  int w=st>>5, b=st&31;
  if (visitMask[w]&(1u<<b)) return;
  visitMask[w]|=(1u<<b);
  achDirty=true;
  int seen=0;
  for (int i=0;i<ST_COUNT&&i<64;i++) if (visitMask[i>>5]&(1u<<(i&31))) seen++;
  if (seen>=ST_COUNT-4) AchGrant(A_PILOT);
}
// coalesced writer -- called from loop, never more than once per 6 s
void AchTick(float dt){
  if (achToastT<3.0f) achToastT+=dt;
  uint32_t now=millis();
  if (achDirty && now-achSaveMs>6000){ achSaveMs=now; AchSave(); }
}
void AchToast(void){
  if (achToastI>=ACH_N || achToastT>=2.8f) return;
  float p=achToastT;
  float slide = (p<0.35f)?EaseOutCubic(p/0.35f)
              : (p>2.35f)?(1.0f-EaseOutCubic((p-2.35f)/0.45f)) : 1.0f;
  int w=196,h=34;
  int x=(SCREEN_W-w)/2, y=(int)(-h+ (h+26)*slide);
  BlendRectFB(x,y,w,h,C_PANEL,A_FILL);
  Bracket(x,y,w,h,C_HILITE,6);
  HLineFB(x+2,y,w-4,C_HILITE);
  Glow(x+w/2,y+h/2,C_HILITE,(uint8_t)(50*slide),2.2f);
  DrawText(x+9,y+7,"ACHIEVEMENT",C_SAND,1);
  GlowText(x+9,y+19,ACH[achToastI].name,C_HILITE,1,80);
  { char b[10]; snprintf(b,sizeof(b),"%d/%d",AchCount(),ACH_N);
    DrawText(x+w-TextW(b,1)-8,y+19,b,C_DATA,1); }
}

// ---- BULLET TIME ----------------------------------------------------
// A global multiplier applied to simulation dt inside v7 screens.
// Hold anywhere in the lower band of a supporting screen -> world slows.
static float btScale = 1.0f;         // smoothed 0.15 .. 1.0
static float btTarget = 1.0f;
static bool  btArmed = false;        // screen opted in this frame
static float btHoldT = 0;

void BulletReset(void){ btScale=1.0f; btTarget=1.0f; btHoldT=0; btArmed=false; }
// call at the top of a screen that supports it; returns the scaled dt
float BulletDt(float dt,bool holding){
  btArmed=true;
  btTarget = holding ? 0.18f : 1.0f;
  if (holding){ btHoldT+=dt; if (btHoldT>1.2f) AchGrant(A_SLOW); }
  else btHoldT=0;
  // smooth, frame-rate independent approach (half-life 0.10 s)
  float k=1.0f-powf(0.5f,dt/0.10f);
  btScale += (btTarget-btScale)*k;
  return dt*btScale;
}
void BulletOverlay(void){
  if (btScale>0.965f) return;
  float s=1.0f-btScale;
  // cheap vignette: edge columns only, never a full-screen blend
  for (int y=0;y<SCREEN_H;y+=2){
    BlendRectFB(0,y,26,1,C_BG,(uint8_t)(150*s));
    BlendRectFB(SCREEN_W-26,y,26,1,C_BG,(uint8_t)(150*s)); }
  char b[20]; snprintf(b,sizeof(b),"x%d.%02d",(int)btScale,(int)(btScale*100)%100);
  DrawText(6,SCREEN_H-11,b,C_HILITE,1);
  DrawText(46,SCREEN_H-11,"TIME",Dim(C_HILITE,3,5),1);
  { int bw=(int)(40*btScale);
    HLineFB(80,SCREEN_H-8,40,C_HAIR); FillRectFB(80,SCREEN_H-8,bw,2,C_HILITE); }
}

// ---- IMPACT CAMERA --------------------------------------------------
// Shake is applied as a draw-space offset. Screens read ImpactOX/OY.
static float impT = 0, impMag = 0;
static uint32_t impLastMs = 0;
void Impact(float mag){
  uint32_t now=millis();
  if (now-impLastMs < 260) return;      // rate limited -- never overused
  impLastMs=now;
  impT=1.0f; impMag=clampf(mag,0.5f,6.0f);
}
void ImpactTick(float dt){ if (impT>0) impT=clampf(impT-dt*3.6f,0,1); }
static inline int ImpactOX(void){
  if (impT<=0) return 0;
  return (int)((Hash((uint32_t)(gTime*400.0f))-0.5f)*2.0f*impMag*impT);
}
static inline int ImpactOY(void){
  if (impT<=0) return 0;
  return (int)((Hash((uint32_t)(gTime*400.0f)+77u)-0.5f)*2.0f*impMag*impT);
}
void ImpactFlash(void){
  if (impT<=0.55f) return;
  uint8_t a=(uint8_t)(70*(impT-0.55f)/0.45f);
  Scrim(a);
}

// ---- UI PHYSICS -----------------------------------------------------
// A throwable card: momentum, friction, wall bounce, elastic snap home.
// struct PhysCard is defined ABOVE the #include block -- see the note
// there. The IDE prototypes Card*() before this point.
static void CardInit(PhysCard &c,float hx,float hy){
  c.x=hx; c.y=hy; c.hx=hx; c.hy=hy;
  c.vx=c.vy=0; c.held=false; c.grabX=c.grabY=0; c.ang=0; c.angV=0;
}
// w/h are the card half-extents used for wall collision
static void CardUpdate(PhysCard &c,float dt,int w,int h,bool snapHome,
                float gravity,float bounce){
  if (c.held){
    float nx=touchX-c.grabX, ny=touchY-c.grabY;
    // velocity from finger delta, so a release throws it
    if (dt>1e-4f){ c.vx=(nx-c.x)/dt*0.55f; c.vy=(ny-c.y)/dt*0.55f; }
    c.vx=clampf(c.vx,-1600,1600); c.vy=clampf(c.vy,-1600,1600);
    c.x=nx; c.y=ny;
    c.angV+=(c.vx*0.00025f-c.angV)*0.2f;
  } else {
    c.vy+=gravity*dt;
    if (snapHome){
      // critically-ish damped spring back to home
      float k=180.0f, d=17.0f;
      c.vx+=(c.hx-c.x)*k*dt; c.vy+=(c.hy-c.y)*k*dt;
      c.vx-=c.vx*d*dt;       c.vy-=c.vy*d*dt;
    } else {
      float f=powf(0.5f,dt/0.55f);   // half-life friction, fps independent
      c.vx*=f; c.vy*=f; }
    c.x+=c.vx*dt; c.y+=c.vy*dt;
    // walls
    if (c.x<w){        c.x=w;            c.vx=-c.vx*bounce; Impact(fabsf(c.vx)*0.004f); }
    if (c.x>SCREEN_W-w){c.x=SCREEN_W-w;  c.vx=-c.vx*bounce; Impact(fabsf(c.vx)*0.004f); }
    if (c.y<h+20){     c.y=h+20;         c.vy=-c.vy*bounce; }
    if (c.y>SCREEN_H-h){c.y=SCREEN_H-h;  c.vy=-c.vy*bounce; Impact(fabsf(c.vy)*0.004f); }
  }
  c.ang+=c.angV*dt*60.0f;
  c.angV*=powf(0.5f,dt/0.4f);
  if (c.ang> 3.14159f) c.ang-=6.28318f;
  if (c.ang<-3.14159f) c.ang+=6.28318f;
}
static bool CardGrab(PhysCard &c,int w,int h){
  if (!touchDown) return false;
  if (touchX<c.x-w||touchX>c.x+w||touchY<c.y-h||touchY>c.y+h) return false;
  c.held=true; c.grabX=touchX-c.x; c.grabY=touchY-c.y;
  c.vx=c.vy=0;
  return true;
}
static void CardRelease(PhysCard &c){ c.held=false; }

// ---- MAGNETIC UI ----------------------------------------------------
// Two elements attract inside RANGE, snap inside SNAP, and resist parting.
#define MAG_RANGE 46.0f
#define MAG_SNAP  13.0f
// returns 0..1 "bond strength"; nudges a toward b when close and free
float MagnetPull(float &ax,float &ay,float bx,float by,bool aHeld,float dt){
  float dx=bx-ax, dy=by-ay;
  float d=sqrtf(dx*dx+dy*dy);
  if (d>MAG_RANGE||d<0.001f) return 0.0f;
  float t=1.0f-(d/MAG_RANGE);
  float strength=t*t;
  if (!aHeld){
    // free element is drawn in and settles
    float pull=strength*220.0f*dt;
    if (pull>d) pull=d;
    ax+=dx/d*pull; ay+=dy/d*pull;
  } else if (d<MAG_SNAP*1.8f){
    // held element feels elastic resistance -- a soft tug back toward b
    float pull=strength*60.0f*dt;
    ax+=dx/d*pull; ay+=dy/d*pull;
  }
  return strength;
}
void MagnetViz(float ax,float ay,float bx,float by,float s){
  if (s<=0.02f) return;
  uint8_t a=(uint8_t)(A_GLOW+120*s);
  int seg=6;
  for (int i=0;i<seg;i++){
    float t0=(float)i/seg, t1=(float)(i+1)/seg;
    if (((i+(int)(gTime*7))&1)) continue;         // dashed, animated
    LineFB((int)(ax+(bx-ax)*t0),(int)(ay+(by-ay)*t0),
           (int)(ax+(bx-ax)*t1),(int)(ay+(by-ay)*t1),C_HILITE,a); }
  if (s>0.72f){ Glow((int)((ax+bx)/2),(int)((ay+by)/2),C_HILITE,(uint8_t)(110*s),0.9f); }
}

// ---- GESTURE RECOGNITION -------------------------------------------
// Resamples the raw stroke to GST_N points, then scores a handful of
// cheap rotation-tolerant features. Deliberately forgiving.
#define GST_RAW 96
#define GST_N   24
enum { GS_NONE=0, GS_LINE, GS_CIRCLE, GS_TRIANGLE, GS_SQUARE,
       GS_ZIGZAG, GS_SPIRAL, GS_ARROW, GS_COUNT };
static const char *GST_NAME[GS_COUNT] = {
  "-","LINE","CIRCLE","TRIANGLE","SQUARE","ZIGZAG","SPIRAL","ARROW" };
static float gstRX[GST_RAW], gstRY[GST_RAW];
static int   gstRN = 0;
static float gstNX[GST_N], gstNY[GST_N];
static uint16_t gstFound = 0;          // bitmask of gestures ever drawn

void GestClear(void){ gstRN=0; }
void GestPush(float x,float y){
  if (gstRN>=GST_RAW) return;
  if (gstRN){
    float dx=x-gstRX[gstRN-1], dy=y-gstRY[gstRN-1];
    if (dx*dx+dy*dy < 9.0f) return;    // 3 px minimum spacing
  }
  gstRX[gstRN]=x; gstRY[gstRN]=y; gstRN++;
}
// arc-length resample into gstNX/gstNY
static bool GestResample(void){
  if (gstRN<6) return false;
  float total=0;
  for (int i=1;i<gstRN;i++){
    float dx=gstRX[i]-gstRX[i-1], dy=gstRY[i]-gstRY[i-1];
    total+=sqrtf(dx*dx+dy*dy); }
  if (total<44.0f) return false;
  float step=total/(GST_N-1), acc=0;
  int o=0; gstNX[o]=gstRX[0]; gstNY[o]=gstRY[0]; o++;
  float px=gstRX[0], py=gstRY[0];
  for (int i=1;i<gstRN&&o<GST_N;){
    float dx=gstRX[i]-px, dy=gstRY[i]-py;
    float d=sqrtf(dx*dx+dy*dy);
    if (d<1e-4f){ i++; continue; }
    if (acc+d>=step){
      float t=(step-acc)/d;
      px=px+dx*t; py=py+dy*t;
      gstNX[o]=px; gstNY[o]=py; o++;
      acc=0;
    } else { acc+=d; px=gstRX[i]; py=gstRY[i]; i++; }
  }
  while (o<GST_N){ gstNX[o]=gstRX[gstRN-1]; gstNY[o]=gstRY[gstRN-1]; o++; }
  return true;
}
int GestRecognize(void){
  if (!GestResample()) return GS_NONE;
  // bounding box + centroid
  float mnx=gstNX[0],mxx=gstNX[0],mny=gstNY[0],mxy=gstNY[0],cx=0,cy=0;
  for (int i=0;i<GST_N;i++){
    if (gstNX[i]<mnx) mnx=gstNX[i]; if (gstNX[i]>mxx) mxx=gstNX[i];
    if (gstNY[i]<mny) mny=gstNY[i]; if (gstNY[i]>mxy) mxy=gstNY[i];
    cx+=gstNX[i]; cy+=gstNY[i]; }
  cx/=GST_N; cy/=GST_N;
  float bw=mxx-mnx, bh=mxy-mny;
  float diag=sqrtf(bw*bw+bh*bh);
  if (diag<26.0f) return GS_NONE;
  // closure: distance between the two ends vs the extent
  float ex=gstNX[GST_N-1]-gstNX[0], ey=gstNY[GST_N-1]-gstNY[0];
  float endGap=sqrtf(ex*ex+ey*ey)/diag;
  bool closed = endGap<0.30f;
  // path length vs straight distance
  float plen=0;
  for (int i=1;i<GST_N;i++){
    float dx=gstNX[i]-gstNX[i-1], dy=gstNY[i]-gstNY[i-1];
    plen+=sqrtf(dx*dx+dy*dy); }
  float straight = (plen>1e-3f)? (sqrtf(ex*ex+ey*ey)/plen) : 0;
  // total signed turning, and count of sharp corners
  float turnSum=0, turnAbs=0;
  int corners=0, flips=0;
  float prevTurn=0;
  for (int i=1;i<GST_N-1;i++){
    float ax=gstNX[i]-gstNX[i-1], ay=gstNY[i]-gstNY[i-1];
    float bx2=gstNX[i+1]-gstNX[i], by2=gstNY[i+1]-gstNY[i];
    float la=sqrtf(ax*ax+ay*ay), lb=sqrtf(bx2*bx2+by2*by2);
    if (la<1e-4f||lb<1e-4f) continue;
    float cross=(ax*by2-ay*bx2)/(la*lb);
    float dot  =(ax*bx2+ay*by2)/(la*lb);
    float ang=atan2f(cross,dot);
    turnSum+=ang; turnAbs+=fabsf(ang);
    if (fabsf(ang)>0.85f) corners++;
    if (i>1 && ang*prevTurn<-0.02f && fabsf(ang)>0.45f) flips++;
    prevTurn=ang; }
  float absTurn=fabsf(turnSum);
  // radial variance -> circles are uniform, polygons are not
  float rMean=0;
  for (int i=0;i<GST_N;i++){
    float dx=gstNX[i]-cx, dy=gstNY[i]-cy;
    rMean+=sqrtf(dx*dx+dy*dy); }
  rMean/=GST_N;
  float rVar=0;
  for (int i=0;i<GST_N;i++){
    float dx=gstNX[i]-cx, dy=gstNY[i]-cy;
    float d=sqrtf(dx*dx+dy*dy)-rMean;
    rVar+=d*d; }
  rVar=sqrtf(rVar/GST_N)/(rMean>1e-3f?rMean:1.0f);

  // --- classify -----------------------------------------------------
  // Thresholds below are measured, not guessed: over 200 jittered strokes
  // per shape the separating features are `straight` (line .95 / arrow .48 /
  // zigzag .27) and `rVar` (circle .04 / square .12 / triangle .25).
  // `corners` is NOT reliable -- resampling smears a 90 deg corner across
  // two points so a square reads as 3 corners, and `flips` reads 0 on a
  // real zigzag. Both are used only as weak tie-breakers.
  (void)corners; (void)flips;

  // 1. spiral: wraps well past a full turn
  if (absTurn>8.2f){ gstFound|=(1u<<GS_SPIRAL); return GS_SPIRAL; }

  // 2. open strokes are separated purely by straightness
  if (!closed && absTurn<4.2f){
    if (straight>0.78f){ gstFound|=(1u<<GS_LINE);   return GS_LINE; }
    // zigzag: heavily wiggling but going nowhere in a straight line
    if (straight<0.62f && turnAbs>5.0f){
      gstFound|=(1u<<GS_ZIGZAG); return GS_ZIGZAG; }
    // arrow: one shaft plus a hook -- middling straightness, open ends
    if (straight>0.30f && straight<=0.78f && endGap>0.45f){
      gstFound|=(1u<<GS_ARROW); return GS_ARROW; }
    if (turnAbs>5.0f){ gstFound|=(1u<<GS_ZIGZAG); return GS_ZIGZAG; }
    gstFound|=(1u<<GS_LINE); return GS_LINE; }

  // 3. closed shapes: radial uniformity tells them apart
  if (rVar<0.085f){ gstFound|=(1u<<GS_CIRCLE);   return GS_CIRCLE; }
  if (rVar<0.185f){ gstFound|=(1u<<GS_SQUARE);   return GS_SQUARE; }
  gstFound|=(1u<<GS_TRIANGLE); return GS_TRIANGLE;
}
void GestTrail(uint16_t c){
  for (int i=1;i<gstRN;i++){
    uint8_t a=(uint8_t)(70+185.0f*i/(gstRN>1?gstRN:1));
    LineFB((int)gstRX[i-1],(int)gstRY[i-1],(int)gstRX[i],(int)gstRY[i],c,a); }
  if (gstRN) Glow((int)gstRX[gstRN-1],(int)gstRY[gstRN-1],C_HILITE,110,0.8f);
}

// =====================================================================
//  v7  --  SHARED SIMULATION POOLS
//  Every pool is a fixed-size static array. Nothing here ever allocates
//  at runtime, so no fragmentation and no runaway growth.
// =====================================================================
#define SIMP_MAX  360        // shared agent pool (boids / fish / ants / charges)
struct SimAgent {
  float x,y,vx,vy;
  float a,b;                 // per-sim scratch (energy, phase, load...)
  uint8_t k;                 // kind / team / state
};
static SimAgent simA[SIMP_MAX];
static int      simN = 0;
static bool     simSeeded = false;
static int      simOwner = -1;      // which screen last initialised the pool

// Re-seed the shared pool when a different screen takes ownership.
static bool SimClaim(int owner,int n){
  if (simOwner==owner && simSeeded) return false;
  simOwner=owner; simSeeded=true;
  simN=clampi(n,1,SIMP_MAX);
  return true;                       // caller should fill the agents
}

// =====================================================================
//  GRAVITY WELLS  (shared by GRAVWELL and CHARGES)
// =====================================================================
#define WELL_MAX 4
struct Well { float x,y,m; bool on; };
static Well gWell[WELL_MAX];
static int  gWellN = 0;

static void WellClear(void){ gWellN=0; for (int i=0;i<WELL_MAX;i++) gWell[i].on=false; }
static void WellAdd(float x,float y,float m){
  if (gWellN<WELL_MAX){ gWell[gWellN].x=x; gWell[gWellN].y=y;
                        gWell[gWellN].m=m; gWell[gWellN].on=true; gWellN++; }
  else { // recycle oldest
    for (int i=1;i<WELL_MAX;i++) gWell[i-1]=gWell[i];
    gWell[WELL_MAX-1].x=x; gWell[WELL_MAX-1].y=y;
    gWell[WELL_MAX-1].m=m; gWell[WELL_MAX-1].on=true; }
}

// =====================================================================
//  1. GRAVITY WELL
// =====================================================================
static float gwTrail = 0.55f;
void ScreenGravWell(float dt){
  if (BackHit()) return;
  AchVisit(ST_GRAVWELL);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  bool hold = touchActive && touchY>BACK_H && touchY<SCREEN_H-24;
  float sdt = BulletDt(dt,touchActive&&touchY>SCREEN_H-24);

  if (SimClaim(ST_GRAVWELL,300)){
    WellClear();
    WellAdd(160,120,1.0f);
    for (int i=0;i<simN;i++){
      float u=Hash(i*911u)*TAU, r=26.0f+Hash(i*337u)*84.0f;
      simA[i].x=160+fcos(u)*r; simA[i].y=120+fsin(u)*r*0.8f;
      float sp=sqrtf(2600.0f/(r+9.0f));
      simA[i].vx=-fsin(u)*sp; simA[i].vy=fcos(u)*sp*0.8f;
      simA[i].a=Hash(i*7717u); simA[i].k=(Hash(i*613u)>0.8f)?1:0; }
  }
  // tap places a well, long band at the bottom is the time control
  if (touchDown && touchY>BACK_H && touchY<SCREEN_H-24)
    WellAdd(touchX,touchY,1.0f);

  // motion-blur trails live in accum, never in the alternating framebuffer
  if (accum){
    uint16_t *ac=(uint16_t*)accum;
    int decay=(int)(11.0f+gwTrail*4.0f);
    for (int i=0;i<FB_PIXELS;i++){
      uint16_t v=ac[i];
      if (!v) continue;
      uint16_t r=((v>>11)&0x1F)*decay>>4, g=((v>>5)&0x3F)*decay>>4, b=(v&0x1F)*decay>>4;
      ac[i]=(r<<11)|(g<<5)|b; }
  }
  for (int i=0;i<simN;i++){
    SimAgent &p=simA[i];
    for (int w=0;w<gWellN;w++){
      if (!gWell[w].on) continue;
      float dx=gWell[w].x-p.x, dy=gWell[w].y-p.y;
      float d2=dx*dx+dy*dy+130.0f;
      float f=26000.0f*gWell[w].m/(d2*sqrtf(d2));
      p.vx+=dx*f*sdt; p.vy+=dy*f*sdt; }
    if (hold){                      // finger is an extra attractor
      float dx=touchX-p.x, dy=touchY-p.y;
      float d2=dx*dx+dy*dy+200.0f;
      float f=17000.0f/(d2*sqrtf(d2));
      p.vx+=dx*f*sdt; p.vy+=dy*f*sdt; }
    float sp2=p.vx*p.vx+p.vy*p.vy;
    if (sp2>360000.0f){ float s=600.0f/sqrtf(sp2); p.vx*=s; p.vy*=s; }
    p.x+=p.vx*sdt; p.y+=p.vy*sdt;
    // wrap softly rather than deleting -- population is constant
    if (p.x<-14) p.x=SCREEN_W+14; if (p.x>SCREEN_W+14) p.x=-14;
    if (p.y<-14) p.y=SCREEN_H+14; if (p.y>SCREEN_H+14) p.y=-14;
    if (accum){
      uint16_t *ac=(uint16_t*)accum;
      int ix=(int)p.x, iy=(int)p.y;
      if (ix>=0&&ix<SCREEN_W&&iy>=0&&iy<SCREEN_H){
        float e=clampf(sqrtf(sp2)/300.0f,0,1);
        uint16_t c=(e>0.66f)?C_HILITE:(p.k?Spec(1):C_ACCENT);
        uint8_t amt=(uint8_t)(60+180*e);
        uint16_t *d=&ac[iy*SCREEN_W+ix];
        int r=((*d>>11)&0x1F)+(((c>>11)&0x1F)*amt>>8);
        int g=((*d>>5)&0x3F)+(((c>>5)&0x3F)*amt>>8);
        int b=(*d&0x1F)+((c&0x1F)*amt>>8);
        if (r>31)r=31; if (g>63)g=63; if (b>31)b=31;
        *d=(r<<11)|(g<<5)|b; } }
  }
  // compose: accum over a plain background (cheap, one pass)
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t*)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  if (accum){
    uint16_t *ac=(uint16_t*)accum, *fr=(uint16_t*)frame;
    for (int i=0;i<FB_PIXELS;i++) if (ac[i]) fr[i]=ac[i]; }

  for (int w=0;w<gWellN;w++){
    if (!gWell[w].on) continue;
    int wx=(int)gWell[w].x, wy=(int)gWell[w].y;
    for (int r=3;r<15;r+=3)
      RingFB(wx,wy,r+(int)(2*Pulse(gTime*1.4f-r*0.1f,1.0f)),
             Dim(C_HILITE,15-r,15),(uint8_t)(A_GLOW+40)); 
    Glow(wx,wy,C_HILITE,120,1.3f);
    CircleFB(wx,wy,2,C_BG,255); }
  if (hold) RingFB(touchX,touchY,10+(int)(4*Pulse(gTime,5.0f)),C_ACCENT,150);

  if (Button(2,22,58,18,"CLEAR",C_WARN,false)){ WellClear(); if (accum) memset(accum,0,FB_BYTES); }
  if (Button(62,22,64,18,"RESEED",C_ACCENT,false)){ simSeeded=false; simOwner=-1; }
  SliderRow(140,30,110,"TRAIL",&gwTrail,C_ACCENT);
  { char b[40]; snprintf(b,sizeof(b),"BODIES %d   WELLS %d",simN,gWellN);
    DrawText(6,SCREEN_H-11,b,C_DATA,1); }
  DrawText(SCREEN_W-118,SCREEN_H-11,"HOLD LOW = SLOW-MO",C_HAIR,1);
  BulletOverlay();
  TopBar("GRAVITY WELL",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// =====================================================================
//  2. BOIDS
// =====================================================================
static float bdSep=0.6f, bdAli=0.55f, bdCoh=0.45f;
static int   bdTouchMode=0;         // 0 predator, 1 attractor, 2 obstacle
void ScreenBoids(float dt){
  if (BackHit()) return;
  AchVisit(ST_BOIDS);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  float sdt=clampf(dt,0.0f,0.05f);

  if (SimClaim(ST_BOIDS,120)){
    for (int i=0;i<simN;i++){
      simA[i].x=Hash(i*131u)*SCREEN_W;
      simA[i].y=44+Hash(i*571u)*(SCREEN_H-70);
      float u=Hash(i*929u)*TAU;
      simA[i].vx=fcos(u)*60.0f; simA[i].vy=fsin(u)*60.0f;
      simA[i].k=(uint8_t)(Hash(i*283u)*3.0f); }
  }
  bool tAct = touchActive && touchY>BACK_H && touchY<SCREEN_H-16;
  // O(n * k) using a strided neighbour sample -- bounded work per frame
  const int STRIDE=7;
  for (int i=0;i<simN;i++){
    SimAgent &b=simA[i];
    float sx=0,sy=0,ax=0,ay=0,cx=0,cy=0; int n=0;
    for (int j=(i%STRIDE);j<simN;j+=STRIDE){
      if (j==i) continue;
      float dx=simA[j].x-b.x, dy=simA[j].y-b.y;
      float d2=dx*dx+dy*dy;
      if (d2>2400.0f||d2<0.01f) continue;
      float d=sqrtf(d2);
      if (d<19.0f){ sx-=dx/d; sy-=dy/d; }
      ax+=simA[j].vx; ay+=simA[j].vy;
      cx+=simA[j].x;  cy+=simA[j].y;
      n++; }
    if (n){
      ax/=n; ay/=n; cx/=n; cy/=n;
      b.vx+=(ax-b.vx)*bdAli*sdt*2.4f;
      b.vy+=(ay-b.vy)*bdAli*sdt*2.4f;
      b.vx+=(cx-b.x)*bdCoh*sdt*0.85f;
      b.vy+=(cy-b.y)*bdCoh*sdt*0.85f; }
    b.vx+=sx*bdSep*260.0f*sdt; b.vy+=sy*bdSep*260.0f*sdt;
    if (tAct){
      float dx=touchX-b.x, dy=touchY-b.y;
      float d=sqrtf(dx*dx+dy*dy)+1.0f;
      if (d<92.0f){
        float f=(1.0f-d/92.0f);
        if (bdTouchMode==0){ b.vx-=dx/d*f*440.0f*sdt; b.vy-=dy/d*f*440.0f*sdt; }
        else if (bdTouchMode==1){ b.vx+=dx/d*f*300.0f*sdt; b.vy+=dy/d*f*300.0f*sdt; }
        else if (d<38.0f){ b.vx-=dx/d*f*700.0f*sdt; b.vy-=dy/d*f*700.0f*sdt; } } }
    // soft bounds
    if (b.x<14)          b.vx+=170.0f*sdt;
    if (b.x>SCREEN_W-14) b.vx-=170.0f*sdt;
    if (b.y<40)          b.vy+=170.0f*sdt;
    if (b.y>SCREEN_H-22) b.vy-=170.0f*sdt;
    float sp=sqrtf(b.vx*b.vx+b.vy*b.vy);
    if (sp>1e-3f){
      float want=clampf(sp,42.0f,120.0f);
      b.vx=b.vx/sp*want; b.vy=b.vy/sp*want; }
    b.x+=b.vx*sdt; b.y+=b.vy*sdt;
    b.x=clampf(b.x,2,SCREEN_W-2); b.y=clampf(b.y,22,SCREEN_H-2); }

  Backdrop();
  for (int i=0;i<simN;i++){
    SimAgent &b=simA[i];
    float sp=sqrtf(b.vx*b.vx+b.vy*b.vy)+1e-3f;
    float ux=b.vx/sp, uy=b.vy/sp;
    uint16_t c=(b.k==0)?C_ACCENT:((b.k==1)?Spec(1):C_DATA);
    int hx=(int)(b.x+ux*5), hy=(int)(b.y+uy*5);
    LineFB(hx,hy,(int)(b.x-ux*3+uy*3),(int)(b.y-uy*3-ux*3),c,220);
    LineFB(hx,hy,(int)(b.x-ux*3-uy*3),(int)(b.y-uy*3+ux*3),c,220);
    PxAdd(hx,hy,C_HILITE,120); }
  if (tAct){
    uint16_t tc=(bdTouchMode==0)?C_WARN:((bdTouchMode==1)?C_HILITE:C_SAND);
    RingFB(touchX,touchY,(int)(92*(bdTouchMode==2?0.42f:1.0f)),tc,70);
    RingFB(touchX,touchY,7,tc,200); }

  if (Button(2,22,72,18,
             bdTouchMode==0?"PREDATOR":(bdTouchMode==1?"ATTRACT":"OBSTACLE"),
             C_ACCENT,false)) bdTouchMode=(bdTouchMode+1)%3;
  SliderRow(84,30,66,"SEP",&bdSep,C_ACCENT);
  SliderRow(160,30,66,"ALI",&bdAli,C_ACCENT);
  SliderRow(236,30,66,"COH",&bdCoh,C_ACCENT);
  { char b[30]; snprintf(b,sizeof(b),"FLOCK %d",simN);
    DrawText(6,SCREEN_H-11,b,C_DATA,1); }
  if (gTime>6.0f) AchGrant(A_SWARM);
  DrawParticles();
  TopBar("BOIDS",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// =====================================================================
//  3. AQUARIUM
// =====================================================================
#define FOOD_MAX 14
struct Food { float x,y,vy; bool on; };
static Food aqFood[FOOD_MAX];
static float aqCaustic=0;
void ScreenAquarium(float dt){
  if (BackHit()) return;
  AchVisit(ST_AQUARIUM);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  float sdt=clampf(dt,0,0.05f);
  aqCaustic+=sdt*0.6f;

  if (SimClaim(ST_AQUARIUM,26)){
    for (int i=0;i<FOOD_MAX;i++) aqFood[i].on=false;
    for (int i=0;i<simN;i++){
      simA[i].x=Hash(i*151u)*SCREEN_W;
      simA[i].y=44+Hash(i*613u)*(SCREEN_H-72);
      simA[i].vx=(Hash(i*977u)-0.5f)*70.0f;
      simA[i].vy=(Hash(i*331u)-0.5f)*26.0f;
      simA[i].a=Hash(i*47u)*TAU;                 // tail phase
      simA[i].b=6.0f+Hash(i*811u)*5.0f;          // size
      simA[i].k=(uint8_t)(Hash(i*191u)*3.0f); }
  }
  // touch drops food
  if (touchDown && touchY>BACK_H && touchY<SCREEN_H-14){
    for (int i=0;i<FOOD_MAX;i++)
      if (!aqFood[i].on){ aqFood[i].on=true; aqFood[i].x=touchX;
                          aqFood[i].y=touchY; aqFood[i].vy=14.0f; break; } }
  for (int i=0;i<FOOD_MAX;i++){
    if (!aqFood[i].on) continue;
    aqFood[i].y+=aqFood[i].vy*sdt;
    aqFood[i].x+=fsin(gTime*1.3f+i)*5.0f*sdt;
    if (aqFood[i].y>SCREEN_H-6) aqFood[i].on=false; }

  for (int i=0;i<simN;i++){
    SimAgent &f=simA[i];
    // find nearest food
    int best=-1; float bd=1e9f;
    for (int k=0;k<FOOD_MAX;k++){
      if (!aqFood[k].on) continue;
      float dx=aqFood[k].x-f.x, dy=aqFood[k].y-f.y;
      float d=dx*dx+dy*dy;
      if (d<bd){ bd=d; best=k; } }
    if (best>=0 && bd<14000.0f){
      float dx=aqFood[best].x-f.x, dy=aqFood[best].y-f.y;
      float d=sqrtf(bd)+1e-3f;
      f.vx+=dx/d*130.0f*sdt; f.vy+=dy/d*130.0f*sdt;
      if (d<7.0f){ aqFood[best].on=false;
                   SpawnBurst(f.x,f.y,5,Spec(1),50.0f,PK_SPARK); }
    } else {
      // light schooling + wander
      float ax=0,ay=0; int n=0;
      for (int j=(i&3);j<simN;j+=4){
        if (j==i) continue;
        float dx=simA[j].x-f.x, dy=simA[j].y-f.y;
        float d2=dx*dx+dy*dy;
        if (d2<3600.0f&&d2>1.0f){ ax+=simA[j].vx; ay+=simA[j].vy; n++;
          if (d2<420.0f){ float d=sqrtf(d2); f.vx-=dx/d*46.0f*sdt; f.vy-=dy/d*46.0f*sdt; } } }
      if (n){ f.vx+=(ax/n-f.vx)*0.7f*sdt; f.vy+=(ay/n-f.vy)*0.7f*sdt; }
      f.vx+=(Hash((uint32_t)(gTime*5)+i*97u)-0.5f)*40.0f*sdt;
      f.vy+=(Hash((uint32_t)(gTime*5)+i*271u)-0.5f)*30.0f*sdt; }
    // flee the finger when it is held down (a hand in the tank)
    if (touchActive){
      float dx=f.x-touchX, dy=f.y-touchY;
      float d=sqrtf(dx*dx+dy*dy)+1e-3f;
      if (d<46.0f){ f.vx+=dx/d*(1.0f-d/46.0f)*300.0f*sdt;
                    f.vy+=dy/d*(1.0f-d/46.0f)*300.0f*sdt; } }
    // glass
    if (f.x<12)          f.vx+=180.0f*sdt;
    if (f.x>SCREEN_W-12) f.vx-=180.0f*sdt;
    if (f.y<42)          f.vy+=180.0f*sdt;
    if (f.y>SCREEN_H-14) f.vy-=180.0f*sdt;
    float sp=sqrtf(f.vx*f.vx+f.vy*f.vy);
    if (sp>96.0f){ f.vx=f.vx/sp*96.0f; f.vy=f.vy/sp*96.0f; }
    f.x+=f.vx*sdt; f.y+=f.vy*sdt;
    f.a+=sdt*(3.0f+sp*0.07f); }

  // water
  uint16_t deep=Dim(C_DATA,1,7);
  for (int y=0;y<SCREEN_H;y++){
    uint8_t sh=(uint8_t)(y*255/SCREEN_H);
    FillRectFB(0,y,SCREEN_W,1,Dim(deep,(uint8_t)(6-sh/64),6)); }
  // caustics: sparse animated highlights, cheap
  for (int i=0;i<26;i++){
    float u=aqCaustic+i*0.9f;
    int cx2=(int)(fmodf(i*47.0f+fsin(u)*30.0f,(float)SCREEN_W));
    int cy2=(int)(40+fmodf(i*31.0f+fcos(u*0.8f)*20.0f,(float)(SCREEN_H-50)));
    PxAdd(cx2,cy2,C_HILITE,(uint8_t)(50+50*fsin(u*2)));
    PxAdd(cx2+1,cy2,C_HILITE,30); }
  for (int i=0;i<FOOD_MAX;i++)
    if (aqFood[i].on){ CircleFB((int)aqFood[i].x,(int)aqFood[i].y,2,Spec(1),230);
                       Glow((int)aqFood[i].x,(int)aqFood[i].y,Spec(1),60,0.4f); }
  for (int i=0;i<simN;i++){
    SimAgent &f=simA[i];
    float sp=sqrtf(f.vx*f.vx+f.vy*f.vy)+1e-3f;
    float ux=f.vx/sp, uy=f.vy/sp;
    uint16_t c=(f.k==0)?C_ACCENT:((f.k==1)?Spec(1):Spec(4));
    int L=(int)f.b;
    // body: a tapered ellipse along the heading
    for (int s=-L;s<=L;s++){
      float tt=1.0f-fabsf((float)s)/L;
      int hh=(int)(tt*tt*L*0.55f)+1;
      int bx=(int)(f.x+ux*s), by=(int)(f.y+uy*s);
      for (int o=-hh;o<=hh;o++)
        PxBlend(bx-(int)(uy*o),by+(int)(ux*o),
                (o<0)?c:Dim(c,3,4),(uint8_t)(200+40*tt)); }
    // tail
    float tw=fsin(f.a)*L*0.55f;
    int tx=(int)(f.x-ux*L), ty=(int)(f.y-uy*L);
    LineFB(tx,ty,(int)(tx-ux*L*0.7f-uy*tw),(int)(ty-uy*L*0.7f+ux*tw),c,210);
    LineFB(tx,ty,(int)(tx-ux*L*0.7f+uy*tw*0.4f),(int)(ty-uy*L*0.7f-ux*tw*0.4f),Dim(c,3,5),170);
    PxBlend((int)(f.x+ux*L*0.55f-uy*2),(int)(f.y+uy*L*0.55f+ux*2),C_BG,255); }
  // sand floor + weeds
  FillRectFB(0,SCREEN_H-8,SCREEN_W,8,Dim(C_SAND,2,6));
  for (int i=0;i<9;i++){
    int wx=14+i*36;
    for (int h=0;h<16;h++)
      PxBlend(wx+(int)(fsin(gTime*0.9f+i+h*0.28f)*(h*0.18f)),SCREEN_H-8-h,
              Dim(Spec(2),3,6),190); }

  { char b[40]; int nf=0; for (int i=0;i<FOOD_MAX;i++) if (aqFood[i].on) nf++;
    snprintf(b,sizeof(b),"FISH %d   FOOD %d",simN,nf);
    DrawText(6,26,b,C_DATA,1); }
  DrawText(SCREEN_W-116,26,"TAP TO FEED",C_HAIR,1);
  if (gTime>8.0f) AchGrant(A_SWARM);
  DrawParticles();
  TopBar("AQUARIUM",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// =====================================================================
//  4. ANT COLONY   (pheromone grid, bounded)
// =====================================================================
#define ANT_GW 64
#define ANT_GH 40
static uint8_t antPhHome[ANT_GW*ANT_GH];
static uint8_t antPhFood[ANT_GW*ANT_GH];
static uint8_t antCell[ANT_GW*ANT_GH];     // 0 empty 1 wall 2 food
static float   antColX=40, antColY=180;
static int     antTool=0;                  // 0 food 1 wall 2 colony
static int     antDelivered=0;
static float   antDecay=0;
void ScreenAnts(float dt){
  if (BackHit()) return;
  AchVisit(ST_ANTS);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  float sdt=clampf(dt,0,0.05f);
  const int TOP=42, CS=4;

  if (SimClaim(ST_ANTS,150)){
    memset(antPhHome,0,sizeof(antPhHome));
    memset(antPhFood,0,sizeof(antPhFood));
    memset(antCell,0,sizeof(antCell));
    antDelivered=0;
    antColX=52; antColY=TOP+ANT_GH*CS/2;
    // a couple of starter food piles so it is alive on entry
    for (int p=0;p<3;p++){
      int fx=34+p*12, fy=6+p*11;
      for (int oy=-2;oy<=2;oy++) for (int ox=-2;ox<=2;ox++){
        int gx=fx+ox, gy=fy+oy;
        if (gx>=0&&gx<ANT_GW&&gy>=0&&gy<ANT_GH) antCell[gy*ANT_GW+gx]=2; } }
    for (int i=0;i<simN;i++){
      simA[i].x=antColX; simA[i].y=antColY;
      float u=Hash(i*443u)*TAU;
      simA[i].vx=fcos(u)*30.0f; simA[i].vy=fsin(u)*30.0f;
      simA[i].k=0;                 // 0 = searching, 1 = carrying
      simA[i].a=0; }
  }
  // paint with the finger
  if (touchActive && touchY>TOP && touchY<TOP+ANT_GH*CS){
    int gx=clampi((touchX-32)/CS,0,ANT_GW-1);
    int gy=clampi((touchY-TOP)/CS,0,ANT_GH-1);
    if (antTool==0) antCell[gy*ANT_GW+gx]=2;
    else if (antTool==1) antCell[gy*ANT_GW+gx]=1;
    else { antColX=(float)(32+gx*CS); antColY=(float)(TOP+gy*CS); } }

  // pheromone decay, spread over 8 frames so it never spikes the budget
  antDecay+=sdt;
  if (antDecay>0.05f){
    antDecay=0;
    static int band=0;
    int y0=band*(ANT_GH/8), y1=y0+(ANT_GH/8);
    for (int y=y0;y<y1;y++) for (int x=0;x<ANT_GW;x++){
      int i=y*ANT_GW+x;
      if (antPhHome[i]) antPhHome[i]--;
      if (antPhFood[i]) antPhFood[i]--; }
    band=(band+1)&7; }

  for (int i=0;i<simN;i++){
    SimAgent &a=simA[i];
    int gx=clampi((int)((a.x-32)/CS),0,ANT_GW-1);
    int gy=clampi((int)((a.y-TOP)/CS),0,ANT_GH-1);
    int ci=gy*ANT_GW+gx;
    // lay trail
    if (a.k==1){ if (antPhFood[ci]<230) antPhFood[ci]+=8; }
    else       { if (antPhHome[ci]<200) antPhHome[ci]+=5; }
    // state change
    if (a.k==0 && antCell[ci]==2){
      a.k=1; antCell[ci]=0;
      a.vx=-a.vx; a.vy=-a.vy;
      SpawnBurst(a.x,a.y,3,Spec(1),40.0f,PK_SPARK); }
    if (a.k==1){
      float dxc=antColX-a.x, dyc=antColY-a.y;
      if (dxc*dxc+dyc*dyc<90.0f){
        a.k=0; antDelivered++;
        a.vx=-a.vx; a.vy=-a.vy;
        if (antDelivered==25) AchGrant(A_SWARM); } }
    // sense three cells ahead: left / centre / right
    float sp=sqrtf(a.vx*a.vx+a.vy*a.vy)+1e-3f;
    float ux=a.vx/sp, uy=a.vy/sp;
    float best=-1; float bax=ux, bay=uy;
    for (int s=-1;s<=1;s++){
      float ang=s*0.7f;
      float rx=ux*fcos(ang)-uy*fsin(ang);
      float ry=ux*fsin(ang)+uy*fcos(ang);
      int sx2=clampi((int)((a.x+rx*9-32)/CS),0,ANT_GW-1);
      int sy2=clampi((int)((a.y+ry*9-TOP)/CS),0,ANT_GH-1);
      int si=sy2*ANT_GW+sx2;
      if (antCell[si]==1) continue;                 // wall: never steer in
      float v = (a.k==1)? (float)antPhHome[si] : (float)antPhFood[si];
      if (a.k==0 && antCell[si]==2) v+=900.0f;      // sees food directly
      if (a.k==1){
        float dxc=antColX-(32+sx2*CS), dyc=antColY-(TOP+sy2*CS);
        v+=900.0f/(sqrtf(dxc*dxc+dyc*dyc)+8.0f); }
      v+=Hash((uint32_t)(gTime*60)+i*131u+s*7u)*22.0f;   // exploration noise
      if (v>best){ best=v; bax=rx; bay=ry; } }
    a.vx=bax*36.0f; a.vy=bay*36.0f;
    float nx=a.x+a.vx*sdt, ny=a.y+a.vy*sdt;
    int ngx=clampi((int)((nx-32)/CS),0,ANT_GW-1);
    int ngy=clampi((int)((ny-TOP)/CS),0,ANT_GH-1);
    if (antCell[ngy*ANT_GW+ngx]==1){ a.vx=-a.vx; a.vy=-a.vy; }
    else { a.x=nx; a.y=ny; }
    if (a.x<33){ a.x=33; a.vx=-a.vx; }
    if (a.x>32+ANT_GW*CS-1){ a.x=(float)(32+ANT_GW*CS-1); a.vx=-a.vx; }
    if (a.y<TOP+1){ a.y=TOP+1; a.vy=-a.vy; }
    if (a.y>TOP+ANT_GH*CS-1){ a.y=(float)(TOP+ANT_GH*CS-1); a.vy=-a.vy; } }

  Backdrop();
  BlendRectFB(30,TOP-2,ANT_GW*CS+4,ANT_GH*CS+4,C_PANEL,A_FILL);
  Bracket(30,TOP-2,ANT_GW*CS+4,ANT_GH*CS+4,C_ACCENT,6);
  int foodLeft=0;
  for (int y=0;y<ANT_GH;y++) for (int x=0;x<ANT_GW;x++){
    int i=y*ANT_GW+x;
    int px=32+x*CS, py=TOP+y*CS;
    uint8_t ph=antPhHome[i], pf=antPhFood[i];
    if (ph|pf){
      if (pf>=ph) BlendRectFB(px,py,CS-1,CS-1,Spec(1),(uint8_t)(pf>>2));
      else        BlendRectFB(px,py,CS-1,CS-1,C_DATA,(uint8_t)(ph>>2)); }
    if (antCell[i]==1) FillRectFB(px,py,CS-1,CS-1,Dim(C_SAND,3,6));
    else if (antCell[i]==2){ foodLeft++; FillRectFB(px,py,CS-1,CS-1,Spec(1)); } }
  CircleFB((int)antColX,(int)antColY,6,Dim(C_ACCENT,3,5),220);
  RingFB((int)antColX,(int)antColY,6+(int)(2*Pulse(gTime,1.5f)),C_ACCENT,200);
  Glow((int)antColX,(int)antColY,C_ACCENT,90,0.9f);
  for (int i=0;i<simN;i++){
    SimAgent &a=simA[i];
    uint16_t c=(a.k==1)?Spec(1):C_TEXT;
    PxBlend((int)a.x,(int)a.y,c,255);
    PxBlend((int)a.x-1,(int)a.y,Dim(c,3,5),200);
    if (a.k==1) PxAdd((int)a.x,(int)a.y-1,Spec(1),140); }

  if (Button(2,22,52,18,antTool==0?"FOOD":(antTool==1?"WALL":"COLONY"),
             C_ACCENT,false)) antTool=(antTool+1)%3;
  if (Button(56,22,50,18,"CLEAR",C_WARN,false)){
    memset(antCell,0,sizeof(antCell));
    memset(antPhHome,0,sizeof(antPhHome));
    memset(antPhFood,0,sizeof(antPhFood)); }
  if (Button(108,22,54,18,"RESEED",C_ACCENT,false)){ simSeeded=false; simOwner=-1; }
  { char b[52]; snprintf(b,sizeof(b),"ANTS %d  FOOD %d  DELIVERED %d",
                         simN,foodLeft,antDelivered);
    DrawText(168,28,b,C_DATA,1); }
  DrawParticles();
  TopBar("ANT COLONY",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// =====================================================================
//  5. MAGNETIC PARTICLE LAB  (charges + field lines)
// =====================================================================
static bool chShowField=true;
static int  chSign=1;
void ScreenCharges(float dt){
  if (BackHit()) return;
  AchVisit(ST_CHARGES);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  float sdt=BulletDt(clampf(dt,0,0.05f),touchActive&&touchY>SCREEN_H-22);

  if (SimClaim(ST_CHARGES,180)){
    WellClear();
    WellAdd(110,120, 1.0f);
    WellAdd(210,120,-1.0f);
    for (int i=0;i<simN;i++){
      simA[i].x=20+Hash(i*211u)*(SCREEN_W-40);
      simA[i].y=46+Hash(i*733u)*(SCREEN_H-72);
      simA[i].vx=simA[i].vy=0;
      simA[i].k=(Hash(i*457u)>0.5f)?1:0; }   // test charge sign
  }
  if (touchDown && touchY>BACK_H && touchY<SCREEN_H-22)
    WellAdd(touchX,touchY,(float)chSign);

  for (int i=0;i<simN;i++){
    SimAgent &p=simA[i];
    float q=(p.k?1.0f:-1.0f);
    for (int w=0;w<gWellN;w++){
      if (!gWell[w].on) continue;
      float dx=p.x-gWell[w].x, dy=p.y-gWell[w].y;
      float d2=dx*dx+dy*dy+90.0f;
      // like repels, unlike attracts
      float f=-q*gWell[w].m*22000.0f/(d2*sqrtf(d2));
      p.vx+=dx*f*sdt; p.vy+=dy*f*sdt; }
    p.vx*=powf(0.5f,sdt/0.35f);           // viscous medium
    p.vy*=powf(0.5f,sdt/0.35f);
    float sp2=p.vx*p.vx+p.vy*p.vy;
    if (sp2>250000.0f){ float s=500.0f/sqrtf(sp2); p.vx*=s; p.vy*=s; }
    p.x+=p.vx*sdt; p.y+=p.vy*sdt;
    if (p.x<4){p.x=4;p.vx=-p.vx*0.5f;} if (p.x>SCREEN_W-4){p.x=SCREEN_W-4;p.vx=-p.vx*0.5f;}
    if (p.y<42){p.y=42;p.vy=-p.vy*0.5f;} if (p.y>SCREEN_H-4){p.y=SCREEN_H-4;p.vy=-p.vy*0.5f;} }

  Backdrop();
  if (chShowField && gWellN){
    // trace a sparse set of field lines from each positive charge
    for (int w=0;w<gWellN;w++){
      if (!gWell[w].on||gWell[w].m<0) continue;
      for (int k=0;k<10;k++){
        float u=TAU*k/10.0f;
        float fx=gWell[w].x+fcos(u)*8.0f, fy=gWell[w].y+fsin(u)*8.0f;
        for (int s=0;s<34;s++){
          float ex=0,ey=0;
          for (int v=0;v<gWellN;v++){
            if (!gWell[v].on) continue;
            float dx=fx-gWell[v].x, dy=fy-gWell[v].y;
            float d2=dx*dx+dy*dy+40.0f;
            float f=gWell[v].m/(d2*sqrtf(d2));
            ex+=dx*f; ey+=dy*f; }
          float el=sqrtf(ex*ex+ey*ey);
          if (el<1e-7f) break;
          fx+=ex/el*5.0f; fy+=ey/el*5.0f;
          if (fx<0||fx>=SCREEN_W||fy<40||fy>=SCREEN_H) break;
          PxBlend((int)fx,(int)fy,Dim(C_DATA,2,6),(uint8_t)(A_GLOW+30)); } } } }
  for (int i=0;i<simN;i++){
    SimAgent &p=simA[i];
    uint16_t c=p.k?Spec(1):C_DATA;
    float sp=sqrtf(p.vx*p.vx+p.vy*p.vy);
    float e=clampf(sp/220.0f,0,1);
    PxAdd((int)p.x,(int)p.y,(e>0.6f)?C_HILITE:c,(uint8_t)(110+140*e));
    if (sp>26.0f){
      float ux=p.vx/(sp+1e-3f), uy=p.vy/(sp+1e-3f);
      LineFB((int)p.x,(int)p.y,(int)(p.x-ux*4),(int)(p.y-uy*4),c,(uint8_t)(70+80*e)); } }
  for (int w=0;w<gWellN;w++){
    if (!gWell[w].on) continue;
    int wx=(int)gWell[w].x, wy=(int)gWell[w].y;
    uint16_t c=(gWell[w].m>0)?Spec(1):C_DATA;
    CircleFB(wx,wy,5,Dim(c,3,5),240);
    RingFB(wx,wy,7+(int)(2*Pulse(gTime*1.3f+w,1.0f)),c,200);
    Glow(wx,wy,c,110,1.0f);
    HLineFB(wx-3,wy,7,C_TEXT);
    if (gWell[w].m>0) VLineFB(wx,wy-3,7,C_TEXT); }

  if (Button(2,22,44,18,chSign>0?"+ CHG":"- CHG",
             chSign>0?Spec(1):C_DATA,false)) chSign=-chSign;
  if (Button(48,22,56,18,"FIELD",C_ACCENT,chShowField)) chShowField=!chShowField;
  if (Button(106,22,50,18,"CLEAR",C_WARN,false)) WellClear();
  if (Button(158,22,56,18,"RESEED",C_ACCENT,false)){ simSeeded=false; simOwner=-1; }
  { char b[40]; snprintf(b,sizeof(b),"CHARGES %d   TEST %d",gWellN,simN);
    DrawText(6,SCREEN_H-11,b,C_DATA,1); }
  BulletOverlay();
  DrawParticles();
  TopBar("CHARGE LAB",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// =====================================================================
//  v7  --  3D FEATURES
//  Morph, kaleidoscope, exploded view, voxel, impossible geometry,
//  infinite tunnel. All reuse the existing rasteriser and mesh pool.
// =====================================================================

// ---------------------------------------------------------------------
//  6. 3D MORPHING
//  Two meshes are sampled on a shared spherical parameterisation so any
//  pair can cross-fade. The interpolated mesh lives in a dedicated
//  static buffer -- nothing is allocated per frame.
// ---------------------------------------------------------------------
#define MORPH_U 22
#define MORPH_V 14
#define MORPH_NV (MORPH_U*MORPH_V)
#define MORPH_NT (MORPH_U*(MORPH_V-1)*2)
static Vert mrPos[MORPH_NV], mrNrm[MORPH_NV];
static Tri  mrTri[MORPH_NT];
static Mesh mrMesh;
static bool mrBuilt=false;
static int  mrA=0, mrB=1;
static float mrT=0.0f, mrAuto=1.0f;
static uint16_t mrSeen=0;              // which shapes have been morphed to

#define MORPH_SHAPES 8
static const char *MORPH_NAME[MORPH_SHAPES] = {
  "SPHERE","CUBE","TORUS","CRYSTAL","ICOSA","OCTA","PYRAMID","WAVE" };

// Radius/point for shape s at spherical angles (u = azimuth, v = polar).
static void MorphPoint(int s,float u,float v,float &x,float &y,float &z){
  float su=fsin(u), cu=fcos(u), sv=fsin(v), cv=fcos(v);
  float bx=sv*cu, by=cv, bz=sv*su;         // unit sphere
  switch (s){
    case 0: x=bx; y=by; z=bz; break;                       // sphere
    case 1: {                                              // cube (sphere->cube)
      float m=fmaxf(fabsf(bx),fmaxf(fabsf(by),fabsf(bz)));
      if (m<1e-4f) m=1e-4f;
      x=bx/m; y=by/m; z=bz/m; } break;
    case 2: {                                              // torus
      float R=0.72f, r=0.30f;
      x=(R+r*cv)*cu; y=r*sv; z=(R+r*cv)*su; } break;
    case 3: {                                              // crystal
      float k=0.55f+0.45f*fabsf(fcos(u*3.0f))*fabsf(fsin(v*2.0f));
      x=bx*k*1.25f; y=by*1.35f; z=bz*k*1.25f; } break;
    case 4: {                                              // icosa-ish faceting
      float f=floorf(u/(TAU/5.0f))*(TAU/5.0f)+(TAU/10.0f);
      float g=floorf(v/(3.14159f/3.0f))*(3.14159f/3.0f)+(3.14159f/6.0f);
      float fx=fsin(g)*fcos(f), fy=fcos(g), fz=fsin(g)*fsin(f);
      x=bx*0.35f+fx*0.72f; y=by*0.35f+fy*0.72f; z=bz*0.35f+fz*0.72f; } break;
    case 5: {                                              // octahedron
      float m=fabsf(bx)+fabsf(by)+fabsf(bz);
      if (m<1e-4f) m=1e-4f;
      x=bx/m; y=by/m; z=bz/m; } break;
    case 6: {                                              // pyramid
      float h=(by+1.0f)*0.5f;                              // 0 base .. 1 apex
      float m=fmaxf(fabsf(bx),fabsf(bz));
      if (m<1e-4f) m=1e-4f;
      float w=(1.0f-h)*1.05f;
      x=bx/m*w; y=by*1.1f; z=bz/m*w; } break;
    default: {                                             // rippled sphere
      float k=1.0f+0.24f*fsin(u*4.0f+v*3.0f);
      x=bx*k; y=by*k; z=bz*k; } break; }
}
static void MorphBuildTopology(void){
  if (mrBuilt) return;
  int t=0;
  for (int v=0;v<MORPH_V-1;v++)
    for (int u=0;u<MORPH_U;u++){
      int u2=(u+1)%MORPH_U;
      uint16_t i0=v*MORPH_U+u,  i1=v*MORPH_U+u2;
      uint16_t i2=(v+1)*MORPH_U+u, i3=(v+1)*MORPH_U+u2;
      if (t+1<MORPH_NT){
        mrTri[t].a=i0; mrTri[t].b=i1; mrTri[t].c=i3;
        mrTri[t].cr=180; mrTri[t].cg=150; mrTri[t].cb=120; t++;
        mrTri[t].a=i0; mrTri[t].b=i3; mrTri[t].c=i2;
        mrTri[t].cr=180; mrTri[t].cg=150; mrTri[t].cb=120; t++; } }
  mrMesh.pos=mrPos; mrMesh.nrm=mrNrm; mrMesh.tri=mrTri;
  mrMesh.nv=MORPH_NV; mrMesh.nt=(uint16_t)t;
  mrBuilt=true;
}
static void MorphEvaluate(int a,int b,float k){
  MorphBuildTopology();
  for (int v=0;v<MORPH_V;v++){
    float pv=3.14159f*(float)v/(MORPH_V-1);
    for (int u=0;u<MORPH_U;u++){
      float pu=TAU*(float)u/MORPH_U;
      float ax,ay,az,bx2,by2,bz2;
      MorphPoint(a,pu,pv,ax,ay,az);
      MorphPoint(b,pu,pv,bx2,by2,bz2);
      int i=v*MORPH_U+u;
      mrPos[i].x=ax+(bx2-ax)*k;
      mrPos[i].y=ay+(by2-ay)*k;
      mrPos[i].z=az+(bz2-az)*k; } }
  // normals from the position field (finite difference on the grid)
  for (int v=0;v<MORPH_V;v++)
    for (int u=0;u<MORPH_U;u++){
      int i=v*MORPH_U+u;
      int iu=v*MORPH_U+((u+1)%MORPH_U);
      int iv=(v<MORPH_V-1? (v+1):(v-1))*MORPH_U+u;
      float e1x=mrPos[iu].x-mrPos[i].x, e1y=mrPos[iu].y-mrPos[i].y, e1z=mrPos[iu].z-mrPos[i].z;
      float e2x=mrPos[iv].x-mrPos[i].x, e2y=mrPos[iv].y-mrPos[i].y, e2z=mrPos[iv].z-mrPos[i].z;
      float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
      if (v>=MORPH_V-1){ nx=-nx; ny=-ny; nz=-nz; }
      float l=sqrtf(nx*nx+ny*ny+nz*nz);
      if (l<1e-5f){ // degenerate: fall back to the radial direction
        nx=mrPos[i].x; ny=mrPos[i].y; nz=mrPos[i].z;
        l=sqrtf(nx*nx+ny*ny+nz*nz); if (l<1e-5f){ nx=0;ny=1;nz=0;l=1; } }
      mrNrm[i].x=nx/l; mrNrm[i].y=ny/l; mrNrm[i].z=nz/l; }
}
void ScreenMorph(float dt){
  if (BackHit()) return;
  AchVisit(ST_MORPH);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  UpdateOrbit(dt);
  ApplyLight();

  if (mrAuto>0.02f){
    mrT+=dt*0.30f*mrAuto;
    if (mrT>=1.0f){ mrT=0; mrA=mrB; mrB=(mrB+1)%MORPH_SHAPES;
                    Impact(1.4f); SpawnBurst(110,120,14,C_ACCENT,120.0f,PK_SPARK); } }
  mrSeen|=(1u<<mrA); mrSeen|=(1u<<mrB);
  { int n=0; for (int i=0;i<MORPH_SHAPES;i++) if (mrSeen&(1u<<i)) n++;
    if (n>=MORPH_SHAPES) AchGrant(A_MORPH); }

  MorphEvaluate(mrA,mrB,EaseOutCubic(clampf(mrT,0,1))*0.5f
                        +clampf(mrT,0,1)*0.5f);
  Backdrop();
  Panel(4,22,182,190,"MORPH",C_ACCENT,"LIVE");
  int ox=ImpactOX(), oy=ImpactOY();
  RenderMesh(mrMesh,rotX,rotY,rotZ,1.0f,95.0f+ox,118.0f+oy,
             2.7f+(1.0f-sCam)*2.2f,gMode,
             (gMode==M_WIRE||gMode==M_NEON||gMode==M_POINTS||gMode==M_HOLO)?C_ACCENT:0);
  ImpactFlash();

  Panel(190,22,126,190,"CONTROL",C_ACCENT,"CFG");
  DrawTextC(253,40,MORPH_NAME[mrA],C_SAND,1);
  DrawTextC(253,50,"|",C_HAIR,1);
  DrawTextC(253,60,MORPH_NAME[mrB],C_HILITE,1);
  { // the morph slider doubles as the readout
    float mv=mrT;
    if (SliderRow(196,86,110,"MORPH",&mv,C_HILITE)){ mrT=mv; mrAuto=0; }
    else mrT=clampf(mrT,0,1); }
  SliderRow(196,116,110,"AUTO",&mrAuto,C_ACCENT);
  SliderRow(196,146,110,"CAMERA",&sCam,C_ACCENT);
  if (Button(194,168,58,17,"FROM",C_ACCENT,false)){
    mrA=(mrA+1)%MORPH_SHAPES; SpawnBurst(223,176,10,C_ACCENT,90.0f,PK_SPARK); }
  if (Button(256,168,58,17,"TO",C_ACCENT,false)){
    mrB=(mrB+1)%MORPH_SHAPES; SpawnBurst(285,176,10,C_ACCENT,90.0f,PK_SPARK); }
  if (Button(194,188,120,17,MODE_NAME[gMode],C_ACCENT,false))
    gMode=(gMode+1)%NUM_MODES;
  { char b[24]; snprintf(b,sizeof(b),"T %d%%",(int)(mrT*100));
    DrawText(10,200,b,C_DATA,1);
    snprintf(b,sizeof(b),"TRIS %d",mrMesh.nt); DrawText(70,200,b,C_DATA,1);
    snprintf(b,sizeof(b),"V %d",mrMesh.nv);    DrawText(140,200,b,C_DATA,1); }
  DrawParticles();
  TopBar("MORPH LAB",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  7. 3D KALEIDOSCOPE
//  The same mesh rendered N times around the axis, mirrored alternately.
// ---------------------------------------------------------------------
static const uint8_t KAL_DIV[5]={3,4,6,8,12};
static int   kalDivI=2;
static float kalZoom=0.45f, kalSpeed=0.42f, kalSpin=0;
void ScreenKaleido(float dt){
  if (BackHit()) return;
  AchVisit(ST_KALEIDO);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  ApplyLight();
  kalSpin+=dt*(0.12f+kalSpeed*1.5f);
  if (kalSpin>TAU) kalSpin-=TAU;
  // dragging the upper area steers pitch, as everywhere else
  if (touchActive&&pressY>BACK_H&&pressY<SCREEN_H-42){
    rotX+=(touchY-lastTY)*0.010f;
    rotX=clampf(rotX,-1.35f,1.35f); }

  Backdrop();
  int n=KAL_DIV[kalDivI];
  int cx=SCREEN_W/2, cy=120;
  float sc=0.42f+kalZoom*0.85f;
  float camz=3.4f-kalZoom*0.9f;
  const Mesh &m=gMesh[gObj];
  // faint symmetry guides
  for (int k=0;k<n;k++){
    float u=TAU*k/n+kalSpin*0.25f;
    LineFB(cx,cy,cx+(int)(fcos(u)*150),cy+(int)(fsin(u)*150),Dim(C_HAIR,3,5),A_GLOW); }
  for (int k=0;k<n;k++){
    float u=TAU*k/n+kalSpin;
    float r=44.0f+22.0f*fsin(kalSpin*1.3f);
    int px=cx+(int)(fcos(u)*r), py=cy+(int)(fsin(u)*r*0.82f);
    // alternate the spin direction so adjacent copies mirror
    float rz=(k&1)? -kalSpin*1.7f : kalSpin*1.7f;
    uint16_t tint=Spec(k%6);
    RenderMesh(m,rotX,u*(k&1?-1.0f:1.0f)+kalSpin,rz,sc,
               (float)px,(float)py,camz,
               (k&1)?M_NEON:gMode,
               (gMode==M_WIRE||gMode==M_NEON||gMode==M_POINTS||
                gMode==M_HOLO||(k&1))?tint:0); }
  // central core
  RenderMesh(m,rotX,-kalSpin*2.0f,kalSpin,sc*0.78f,(float)cx,(float)cy,camz*0.9f,
             M_HOLO,C_HILITE);
  Glow(cx,cy,C_HILITE,50,3.0f);

  if (Button(2,22,52,18,"SYM",C_ACCENT,false)) kalDivI=(kalDivI+1)%5;
  { char b[12]; snprintf(b,sizeof(b),"%d",n); DrawTextC(28,40,b,C_DATA,1); }
  if (Button(56,22,52,18,"OBJ",C_ACCENT,false)) gObj=(gObj+1)%NUM_OBJ;
  DrawTextC(82,40,OBJ_NAME[gObj],C_SAND,1);
  if (Button(110,22,56,18,"MODE",C_ACCENT,false)) gMode=(gMode+1)%NUM_MODES;
  SliderRow(176,30,64,"ZOOM",&kalZoom,C_ACCENT);
  SliderRow(248,30,64,"SPEED",&kalSpeed,C_ACCENT);
  DrawParticles();
  TopBar("KALEIDOSCOPE",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  8. EXPLODED VIEW
//  Triangles are grouped into shells by centroid radius; each shell
//  slides outward along its own normal. Pieces are draggable.
// ---------------------------------------------------------------------
#define EXP_PIECES 10
struct ExpPiece { float dx,dy,dz; float ox,oy; bool held; };
static ExpPiece exPc[EXP_PIECES];
static float exSpread=0.0f, exAuto=0;
static bool  exInit=false;
static int   exHeld=-1;
static void ExplodeInit(void){
  for (int i=0;i<EXP_PIECES;i++){
    float u=TAU*i/EXP_PIECES, v=(i*0.37f)-0.5f;
    exPc[i].dx=fcos(u); exPc[i].dy=v; exPc[i].dz=fsin(u);
    float l=sqrtf(exPc[i].dx*exPc[i].dx+exPc[i].dy*exPc[i].dy+exPc[i].dz*exPc[i].dz);
    exPc[i].dx/=l; exPc[i].dy/=l; exPc[i].dz/=l;
    exPc[i].ox=exPc[i].oy=0; exPc[i].held=false; }
  exInit=true;
}
void ScreenExplode(float dt){
  if (BackHit()) return;
  AchVisit(ST_EXPLODE);
  if (!exInit) ExplodeInit();
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  ApplyLight();
  if (exAuto>0.5f){
    exSpread+=dt*0.35f;
    if (exSpread>1.0f){ exSpread=1.0f; } }

  const Mesh &m=gMesh[gObj];
  Backdrop();
  int cx=150, cy=122;
  // find the mesh radius once so shells are stable
  float maxR=0.001f;
  for (int i=0;i<m.nv;i++){
    float r=sqrtf(m.pos[i].x*m.pos[i].x+m.pos[i].y*m.pos[i].y+m.pos[i].z*m.pos[i].z);
    if (r>maxR) maxR=r; }
  // render each shell as its own offset draw
  float sc=1.0f, camz=3.1f+(1.0f-sCam)*2.0f;
  for (int p=0;p<EXP_PIECES;p++){
    float off=exSpread*0.85f;
    float px=cx+exPc[p].dx*off*74.0f+exPc[p].ox;
    float py=cy-exPc[p].dy*off*66.0f+exPc[p].oy;
    // a shell is a contiguous band of triangles -- draw via a temp view
    Mesh shell;
    shell.pos=m.pos; shell.nrm=m.nrm;
    int t0=(int)((long)m.nt*p/EXP_PIECES);
    int t1=(int)((long)m.nt*(p+1)/EXP_PIECES);
    if (t1<=t0) continue;
    shell.tri=&m.tri[t0];
    shell.nt=(uint16_t)(t1-t0);
    shell.nv=m.nv;
    uint16_t tint=(exHeld==p)?C_HILITE:0;
    RenderMesh(shell,rotX,rotY,rotZ,sc,px,py,camz,
               (exHeld==p)?M_NEON:gMode,
               (gMode==M_WIRE||gMode==M_NEON||gMode==M_POINTS||
                gMode==M_HOLO||exHeld==p)?(tint?tint:Spec(p%6)):0);
    if (exSpread>0.08f){
      // leader line back to the origin, so the assembly stays legible
      LineFB((int)px,(int)py,cx,cy,Dim(C_HAIR,3,5),(uint8_t)(A_GLOW*exSpread));
      if (exHeld==p) Glow((int)px,(int)py,C_HILITE,100,1.1f); } }
  if (exSpread<0.06f) UpdateOrbit(dt);

  // drag pieces when exploded
  if (exSpread>0.2f){
    if (touchDown&&touchY>BACK_H&&touchY<SCREEN_H-24){
      float best=1e9f; int bi=-1;
      for (int p=0;p<EXP_PIECES;p++){
        float px=cx+exPc[p].dx*exSpread*0.85f*74.0f+exPc[p].ox;
        float py=cy-exPc[p].dy*exSpread*0.85f*66.0f+exPc[p].oy;
        float d=(touchX-px)*(touchX-px)+(touchY-py)*(touchY-py);
        if (d<best){ best=d; bi=p; } }
      if (best<1500.0f) exHeld=bi; }
    if (!touchActive) exHeld=-1;
    if (exHeld>=0&&touchActive){
      exPc[exHeld].ox+=(touchX-lastTX);
      exPc[exHeld].oy+=(touchY-lastTY);
      exPc[exHeld].ox=clampf(exPc[exHeld].ox,-90,90);
      exPc[exHeld].oy=clampf(exPc[exHeld].oy,-80,80); } }
  else { exHeld=-1;
    // reassembled: pieces spring their manual offsets away
    for (int p=0;p<EXP_PIECES;p++){
      exPc[p].ox*=powf(0.5f,dt/0.20f);
      exPc[p].oy*=powf(0.5f,dt/0.20f); } }

  Panel(238,44,78,150,"PARTS",C_ACCENT,"ASM");
  for (int p=0;p<EXP_PIECES;p++){
    int ry=58+p*13;
    uint16_t c=(exHeld==p)?C_HILITE:Spec(p%6);
    BlendRectFB(244,ry,7,7,c,A_FILL);
    char b[16]; snprintf(b,sizeof(b),"SEG %02d",p+1);
    DrawText(256,ry,b,(exHeld==p)?C_TEXT:C_SAND,1); }

  { float sv=exSpread;
    if (SliderRow(8,32,150,"SPREAD",&sv,C_ACCENT)){ exSpread=sv; exAuto=0; } }
  if (Button(176,24,56,17,"EXPLODE",C_ACCENT,exAuto>0.5f)){
    exAuto=1.0f; exSpread=0.02f; Impact(2.2f);
    SpawnBurst(cx,cy,26,C_ACCENT,190.0f,PK_SPARK); }
  if (Button(176,SCREEN_H-22,56,18,"ASSEMBLE",C_HILITE,false)){
    exAuto=0; exSpread=0;
    for (int p=0;p<EXP_PIECES;p++){ exPc[p].ox=0; exPc[p].oy=0; }
    Impact(1.6f); }
  if (Button(8,SCREEN_H-22,54,18,"OBJ",C_ACCENT,false)) gObj=(gObj+1)%NUM_OBJ;
  DrawText(68,SCREEN_H-17,OBJ_NAME[gObj],C_SAND,1);
  ImpactFlash();
  DrawParticles();
  TopBar("EXPLODED VIEW",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  9. VOXEL MODE
// ---------------------------------------------------------------------
static float vxSize=0.45f, vxLight=0.55f, vxVar=0.4f;
void ScreenVoxel(float dt){
  if (BackHit()) return;
  AchVisit(ST_VOXEL);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  UpdateOrbit(dt);
  sLight=vxLight; ApplyLight();
  gVoxSize=1.4f+vxSize*4.6f;
  Backdrop();
  Panel(4,22,196,190,"VOXEL",C_ACCENT,"CUBE");
  // colour variation is applied by tinting alternate draws
  uint16_t tint=0;
  if (vxVar>0.05f){
    int k=(int)(gTime*0.7f)%6;
    tint=Dim(Spec(k),(uint8_t)(3+vxVar*2),5); }
  RenderMesh(gMesh[gObj],rotX,rotY,rotZ,1.0f,102.0f,118.0f,
             2.9f+(1.0f-sCam)*2.2f,M_VOXEL,tint);
  Panel(204,22,112,190,"CONTROL",C_ACCENT,"CFG");
  SliderRow(210,48,96,"SIZE",&vxSize,C_ACCENT);
  SliderRow(210,78,96,"LIGHT",&vxLight,C_ACCENT);
  SliderRow(210,108,96,"COLOR",&vxVar,C_ACCENT);
  SliderRow(210,138,96,"CAMERA",&sCam,C_ACCENT);
  SliderRow(210,168,96,"SPIN",&sSpin,C_ACCENT);
  if (Button(208,190,104,18,OBJ_NAME[gObj],C_ACCENT,false)) gObj=(gObj+1)%NUM_OBJ;
  { char b[28]; snprintf(b,sizeof(b),"VOX %d PX",(int)gVoxSize);
    DrawText(10,200,b,C_DATA,1);
    snprintf(b,sizeof(b),"CELLS %d",gMesh[gObj].nt);
    DrawText(96,200,b,C_DATA,1); }
  DrawParticles();
  TopBar("VOXEL MODE",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  10. IMPOSSIBLE GEOMETRY
//  Drawn in 2D isometric projection: the illusion only closes at one
//  camera angle, so the camera is constrained near it and the readout
//  tells the user how far the illusion has been broken.
// ---------------------------------------------------------------------
static int   imFig=0;
static float imRot=0, imTilt=0;
static const char *IM_NAME[3]={ "PENROSE TRIANGLE","IMPOSSIBLE STAIRS","IMPOSSIBLE CUBE" };
// isometric projector with a small controllable perturbation
static inline void IsoPt(float x,float y,float z,float rot,float tilt,
                         int cx,int cy,float s,int &sx,int &sy){
  float cr=fcos(rot), sr=fsin(rot);
  float rx=x*cr - z*sr;
  float rz=x*sr + z*cr;
  sx=cx+(int)((rx-rz)*0.866f*s);
  sy=cy+(int)(((rx+rz)*0.5f - y*(1.0f+tilt))*s);
}
static void ImBar(float x0,float y0,float z0,float x1,float y1,float z1,
                  float w,uint16_t c,int cx,int cy,float s){
  // a beam drawn as a filled quad plus edges -- reads as solid
  int ax,ay,bx2,by2;
  IsoPt(x0,y0,z0,imRot,imTilt,cx,cy,s,ax,ay);
  IsoPt(x1,y1,z1,imRot,imTilt,cx,cy,s,bx2,by2);
  float dx=(float)(bx2-ax), dy=(float)(by2-ay);
  float l=sqrtf(dx*dx+dy*dy); if (l<1e-3f) return;
  float px=-dy/l*w*s*0.5f, py=dx/l*w*s*0.5f;
  int steps=(int)(w*s);
  if (steps<1) steps=1;
  for (int i=-steps;i<=steps;i++){
    float t=(float)i/steps;
    LineFB(ax+(int)(px*t),ay+(int)(py*t),
           bx2+(int)(px*t),by2+(int)(py*t),
           Dim(c,(uint8_t)(3+2*(1.0f-fabsf(t))),5),230); }
  LineFB(ax+(int)px,ay+(int)py,bx2+(int)px,by2+(int)py,C_HILITE,180);
  LineFB(ax-(int)px,ay-(int)py,bx2-(int)px,by2-(int)py,Dim(c,2,5),200);
}
void ScreenImpossible(float dt){
  if (BackHit()) return;
  AchVisit(ST_IMPOSSIBLE);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (touchActive&&pressY>BACK_H&&pressY<SCREEN_H-26){
    imRot+=(touchX-lastTX)*0.006f;
    imTilt+=(touchY-lastTY)*0.004f;
    // the illusion only survives near the canonical view; clamp gently
    imRot=clampf(imRot,-0.42f,0.42f);
    imTilt=clampf(imTilt,-0.30f,0.30f); }
  else {
    // ease back to the perfect viewing angle when released
    imRot *=powf(0.5f,dt/0.9f);
    imTilt*=powf(0.5f,dt/0.9f); }

  Backdrop();
  int cx=160, cy=126;
  float s=1.0f;
  uint16_t c=C_ACCENT;
  if (imFig==0){
    // Penrose triangle: three beams whose ends are hidden by draw order
    const float L=2.05f, W=0.52f;
    s=34.0f;
    ImBar(-L*0.5f,-L*0.55f,0,  L*0.5f,-L*0.55f,0,  W,Spec(0),cx,cy,s);
    ImBar( L*0.5f,-L*0.55f,0,  0,       L*0.62f,0, W,Spec(2),cx,cy,s);
    ImBar( 0,      L*0.62f,0, -L*0.5f,-L*0.55f,0.9f,W,Spec(4),cx,cy,s);
    // the closing beam is drawn last and overlaps the first -> impossible
    ImBar(-L*0.5f,-L*0.55f,0.9f,-L*0.5f,-L*0.55f,0, W,Spec(4),cx,cy,s);
  } else if (imFig==1){
    // impossible staircase: 4 flights that always ascend
    s=17.0f;
    for (int i=0;i<16;i++){
      float a=TAU*i/16.0f;
      float rr=2.7f;
      float x=fcos(a)*rr, z=fsin(a)*rr;
      float y=(float)(i%4)*0.42f + (i/4)*0.02f;
      float x2=fcos(a+TAU/16.0f)*rr, z2=fsin(a+TAU/16.0f)*rr;
      float y2=(float)((i+1)%4)*0.42f + ((i+1)/4)*0.02f;
      ImBar(x,y,z,x2,y2,z2,0.55f,Spec(i%6),cx,cy,s);
      // the riser
      ImBar(x2,y2,z2,x2,y2-0.40f,z2,0.5f,Dim(Spec(i%6),3,5),cx,cy,s); }
  } else {
    // impossible cube: a wireframe with two edges deliberately swapped
    s=40.0f;
    const float V[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                         {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1}};
    const int E[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
                        {6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    // draw far edges first, then near, but swap two so depth contradicts
    const int ORDER[12]={4,5,6,7,8,9,11,0,1,2,3,10};
    for (int k=0;k<12;k++){
      int e=ORDER[k];
      ImBar(V[E[e][0]][0],V[E[e][0]][1],V[E[e][0]][2],
            V[E[e][1]][0],V[E[e][1]][1],V[E[e][1]][2],
            0.30f,Spec(e%6),cx,cy,s); } }

  float broke=clampf((fabsf(imRot)/0.42f+fabsf(imTilt)/0.30f)*0.5f,0,1);
  if (broke>0.04f){
    char b[36];
    snprintf(b,sizeof(b),"ILLUSION %d%%",(int)((1.0f-broke)*100));
    DrawTextC(160,SCREEN_H-30,b,broke>0.6f?C_WARN:C_DATA,1);
    int bw=(int)(120*(1.0f-broke));
    HLineFB(100,SCREEN_H-20,120,C_HAIR);
    FillRectFB(100,SCREEN_H-20,bw,2,broke>0.6f?C_WARN:C_HILITE); }
  else DrawTextC(160,SCREEN_H-24,"DRAG TO BREAK THE ILLUSION",C_HAIR,1);

  if (Button(2,22,84,18,"FIGURE",C_ACCENT,false)){
    imFig=(imFig+1)%3; imRot=0; imTilt=0;
    SpawnBurst(44,40,14,C_ACCENT,110.0f,PK_SPARK); }
  DrawText(94,27,IM_NAME[imFig],C_SAND,1);
  DrawParticles();
  TopBar("ESCHER LAB",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  11. INFINITE 3D TUNNEL
// ---------------------------------------------------------------------
#define TUN_RINGS 26
#define TUN_SEG   12
static float tunZ=0, tunSpeed=0.5f, tunTargetSpeed=0.5f;
static float tunSteerX=0, tunSteerY=0;
static int   tunStyle=0;
void ScreenTunnel(float dt){
  if (BackHit()) return;
  AchVisit(ST_TUNNEL);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  // cinematic accel: the target eases, so speed changes feel weighted
  if (touchActive&&touchY>BACK_H&&touchY<SCREEN_H-22){
    tunTargetSpeed=1.0f;
    tunSteerX+=((touchX-160)*0.35f-tunSteerX)*clampf(dt*3.4f,0,1);
    tunSteerY+=((touchY-120)*0.30f-tunSteerY)*clampf(dt*3.4f,0,1);
  } else {
    tunTargetSpeed=0.42f;
    tunSteerX*=powf(0.5f,dt/0.8f);
    tunSteerY*=powf(0.5f,dt/0.8f); }
  tunSpeed+=(tunTargetSpeed-tunSpeed)*clampf(dt*1.5f,0,1);
  tunZ+=dt*(2.2f+tunSpeed*9.0f);

  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t*)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;

  const float SPACING=1.05f;
  int cx=160, cy=120;
  // draw far to near
  for (int r=TUN_RINGS-1;r>=0;r--){
    float z=fmodf(tunZ,SPACING)+r*SPACING;
    if (z<0.35f) continue;
    float iz=1.0f/z;
    float scale=230.0f*iz;
    // the tunnel bends -- centre drifts with depth and with steering
    float bend=fsin(tunZ*0.12f+z*0.20f)*26.0f;
    float ox=cx+bend*(1.0f-iz)+tunSteerX*iz*2.6f;
    float oy=cy+fcos(tunZ*0.09f+z*0.17f)*18.0f*(1.0f-iz)+tunSteerY*iz*2.6f;
    float twist=tunZ*0.11f+z*0.13f;
    uint8_t a=(uint8_t)(clampf(iz*2.6f,0.06f,1.0f)*230);
    uint16_t c=Spec(((int)(z*0.6f+tunZ*0.3f))%6);
    if (tunStyle==0){
      // ring of struts
      int px=0,py=0;
      for (int s2=0;s2<=TUN_SEG;s2++){
        float u=TAU*s2/TUN_SEG+twist;
        int sx=(int)(ox+fcos(u)*scale), sy=(int)(oy+fsin(u)*scale*0.86f);
        if (s2) LineFB(px,py,sx,sy,c,a);
        px=sx; py=sy; }
    } else if (tunStyle==1){
      // hex frames
      HexFB((int)ox,(int)oy,(int)scale,c,a,false);
      if ((r&3)==0) HexFB((int)ox,(int)oy,(int)(scale*0.82f),Dim(c,3,5),(uint8_t)(a>>1),false);
    } else {
      // radial spokes / gate structures
      for (int s2=0;s2<TUN_SEG;s2++){
        float u=TAU*s2/TUN_SEG+twist;
        int x0=(int)(ox+fcos(u)*scale*0.72f), y0=(int)(oy+fsin(u)*scale*0.62f);
        int x1=(int)(ox+fcos(u)*scale),       y1=(int)(oy+fsin(u)*scale*0.86f);
        LineFB(x0,y0,x1,y1,c,a); } }
    // connecting rails between rings, only every few rings (cheap)
    if ((r&1)==0 && iz>0.14f){
      for (int s2=0;s2<TUN_SEG;s2+=3){
        float u=TAU*s2/TUN_SEG+twist;
        PxAdd((int)(ox+fcos(u)*scale),(int)(oy+fsin(u)*scale*0.86f),
              C_HILITE,(uint8_t)(a>>1)); } }
    // depth particles
    if ((r%5)==0){
      float u=Hash(r*971u)*TAU;
      float pr=scale*(0.25f+0.6f*Hash(r*331u));
      PxAdd((int)(ox+fcos(u+twist)*pr),(int)(oy+fsin(u+twist)*pr*0.86f),
            C_HILITE,(uint8_t)clampf(a*1.2f,0,255)); } }
  // speed streaks toward the vanishing point when fast
  if (tunSpeed>0.6f){
    int n=(int)((tunSpeed-0.6f)*44.0f);
    for (int i=0;i<n;i++){
      float u=Hash(i*617u+(uint32_t)(gTime*6))*TAU;
      float r0=40+Hash(i*911u)*150.0f;
      float r1=r0+18.0f*tunSpeed;
      LineFB(cx+(int)(fcos(u)*r0),cy+(int)(fsin(u)*r0*0.86f),
             cx+(int)(fcos(u)*r1),cy+(int)(fsin(u)*r1*0.86f),
             C_HILITE,(uint8_t)(60+90*tunSpeed)); } }
  Glow(cx,cy,C_ACCENT,(uint8_t)(40+50*tunSpeed),3.4f);

  if (Button(2,22,58,18,"STYLE",C_ACCENT,false)) tunStyle=(tunStyle+1)%3;
  { char b[36]; snprintf(b,sizeof(b),"VELOCITY %d",(int)(tunSpeed*100));
    DrawText(SCREEN_W-TextW(b,1)-6,26,b,C_DATA,1); }
  DrawTextC(160,SCREEN_H-12,"HOLD TO ACCELERATE   DRAG TO STEER",C_HAIR,1);
  { int bw=(int)(60*tunSpeed);
    HLineFB(66,31,60,C_HAIR); FillRectFB(66,31,bw,3,C_HILITE); }
  DrawParticles();
  TopBar("INFINITE TUNNEL",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// =====================================================================
//  v7  --  MATHEMATICS
// =====================================================================

// ---------------------------------------------------------------------
//  12. EQUATION GRAPHER
//  A tiny recursive-descent parser over a fixed token buffer. No String,
//  no allocation, bounded recursion depth.
// ---------------------------------------------------------------------
#define EQ_SLOTS 3
#define EQ_LEN   40
static char  eqSrc[EQ_SLOTS][EQ_LEN] = { "sin(x)", "x*x/3-2", "" };
static bool  eqOn[EQ_SLOTS]  = { true, true, false };
static int   eqEdit = 0;
static float eqCX=0, eqCY=0, eqScale=26.0f;
static bool  eqShowInt=true;

// --- parser ---
static const char *epP;      // cursor
static float epX;            // current x
static int   epDepth;        // recursion guard
static bool  epErr;
static float EpExpr(void);
static void EpSkip(void){ while (*epP==' ') epP++; }
static float EpNum(void){
  float v=0;
  while (*epP>='0'&&*epP<='9'){ v=v*10.0f+(*epP-'0'); epP++; }
  if (*epP=='.'){ epP++; float f=0.1f;
    while (*epP>='0'&&*epP<='9'){ v+=(*epP-'0')*f; f*=0.1f; epP++; } }
  return v;
}
static bool EpWord(const char *w){
  int n=0; while (w[n]) n++;
  for (int i=0;i<n;i++){
    char a=epP[i]; if (a>='A'&&a<='Z') a=(char)(a+32);
    if (a!=w[i]) return false; }
  epP+=n; return true;
}
static float EpAtom(void){
  EpSkip();
  if (epDepth>12){ epErr=true; return 0; }
  epDepth++;
  float v=0;
  if (*epP=='-'){ epP++; v=-EpAtom(); epDepth--; return v; }
  if (*epP=='+'){ epP++; v= EpAtom(); epDepth--; return v; }
  if (*epP=='('){ epP++; v=EpExpr(); EpSkip(); if (*epP==')') epP++; else epErr=true;
                  epDepth--; return v; }
  if (EpWord("sin")){ v=fsin(EpAtom()); epDepth--; return v; }
  if (EpWord("cos")){ v=fcos(EpAtom()); epDepth--; return v; }
  if (EpWord("tan")){ float a=EpAtom(); float c=fcos(a);
                      v=(fabsf(c)<1e-4f)?1e4f:(fsin(a)/c); epDepth--; return v; }
  if (EpWord("sqrt")){ float a=EpAtom(); v=(a<0)?0:sqrtf(a); epDepth--; return v; }
  if (EpWord("abs")) { v=fabsf(EpAtom()); epDepth--; return v; }
  if (EpWord("exp")) { float a=clampf(EpAtom(),-20,20); v=expf(a); epDepth--; return v; }
  if (EpWord("log")) { float a=EpAtom(); v=(a<=0)?-20.0f:logf(a); epDepth--; return v; }
  if (EpWord("pi"))  { v=3.14159265f; epDepth--; return v; }
  if (EpWord("e"))   { v=2.71828183f; epDepth--; return v; }
  if (*epP=='x'||*epP=='X'){ epP++; epDepth--; return epX; }
  if ((*epP>='0'&&*epP<='9')||*epP=='.'){ v=EpNum(); epDepth--; return v; }
  epErr=true; epDepth--; return 0;
}
static float EpPow(void){
  float b=EpAtom();
  EpSkip();
  if (*epP=='^'){
    epP++;
    float e=EpPow();
    if (b<0){ int ei=(int)e;
      if (fabsf(e-ei)<1e-4f){ float r=1; int n=ei<0?-ei:ei;
        for (int i=0;i<n&&i<24;i++) r*=b;
        return ei<0?(fabsf(r)>1e-9f?1.0f/r:0.0f):r; }
      return 0; }
    if (b<1e-9f) return 0;
    float lg=logf(b)*e;
    if (lg>20) lg=20; if (lg<-20) lg=-20;
    return expf(lg); }
  return b;
}
static float EpTerm(void){
  float v=EpPow();
  for (;;){
    EpSkip();
    if (*epP=='*'){ epP++; v*=EpPow(); }
    else if (*epP=='/'){ epP++; float d=EpPow();
                         v=(fabsf(d)<1e-6f)?(v>=0?1e5f:-1e5f):(v/d); }
    else break; }
  return v;
}
static float EpExpr(void){
  float v=EpTerm();
  for (;;){
    EpSkip();
    if (*epP=='+'){ epP++; v+=EpTerm(); }
    else if (*epP=='-'){ epP++; v-=EpTerm(); }
    else break; }
  return v;
}
// evaluate src at x; returns false when the expression is malformed
static bool EqEval(const char *src,float x,float &out){
  if (!src||!*src) return false;
  // skip a leading "y=" if the user typed one
  if ((src[0]=='y'||src[0]=='Y')&&src[1]=='=') src+=2;
  epP=src; epX=x; epDepth=0; epErr=false;
  float v=EpExpr();
  EpSkip();
  if (epErr||*epP) return false;
  if (!(v==v)) return false;             // NaN check without isfinite
  if (v>1e6f||v<-1e6f) return false;
  out=v; return true;
}
void ScreenGrapher(float dt){
  if (BackHit()) return;
  AchVisit(ST_GRAPHER);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int GY0=42, GY1=SCREEN_H-26;
  // pan + pinch-free zoom (drag vertically at the right edge)
  if (touchActive&&pressY>GY0&&pressY<GY1&&pressX<SCREEN_W-30){
    eqCX-=(touchX-lastTX)/eqScale;
    eqCY+=(touchY-lastTY)/eqScale; }
  if (touchActive&&pressX>=SCREEN_W-30&&pressY>GY0&&pressY<GY1){
    eqScale*=powf(1.011f,(float)(lastTY-touchY));
    eqScale=clampf(eqScale,3.0f,400.0f); }

  Backdrop();
  int ox=SCREEN_W/2-(int)(eqCX*eqScale);
  int oy=(GY0+GY1)/2+(int)(eqCY*eqScale);
  // grid: choose a decade step so lines stay ~30 px apart
  float step=1.0f;
  while (step*eqScale<26.0f) step*=2.0f;
  while (step*eqScale>90.0f) step*=0.5f;
  for (float gx=ceilf((0-ox)/eqScale/step)*step;;gx+=step){
    int sx=ox+(int)(gx*eqScale);
    if (sx>=SCREEN_W) break;
    if (sx<0) continue;
    VLineFB(sx,GY0,GY1-GY0,(fabsf(gx)<1e-4f)?Dim(C_ACCENT,2,5):Dim(C_HAIR,3,5)); }
  for (float gy=ceilf((GY0-oy)/eqScale/step)*step;;gy+=step){
    int sy=oy+(int)(gy*eqScale);
    if (sy>=GY1) break;
    if (sy<GY0) continue;
    HLineFB(0,sy,SCREEN_W,(fabsf(gy)<1e-4f)?Dim(C_ACCENT,2,5):Dim(C_HAIR,3,5)); }

  // curves
  static float col[EQ_SLOTS][SCREEN_W];
  static bool  ok[EQ_SLOTS][SCREEN_W];
  for (int s=0;s<EQ_SLOTS;s++){
    if (!eqOn[s]||!eqSrc[s][0]) continue;
    uint16_t c=Spec(s*2);
    int px=-1, py=0;
    for (int sx=0;sx<SCREEN_W;sx++){
      float x=(sx-ox)/eqScale;
      float y;
      ok[s][sx]=EqEval(eqSrc[s],x,y);
      col[s][sx]=y;
      if (!ok[s][sx]){ px=-1; continue; }
      int sy=oy-(int)(y*eqScale);
      if (sy<GY0-400||sy>GY1+400){ px=-1; continue; }
      int cy0=clampi(sy,GY0,GY1-1);
      if (px>=0){
        int cp=clampi(py,GY0,GY1-1);
        // only join when the step is sane -- avoids vertical asymptote bars
        if (abs(sy-py)<(GY1-GY0)) LineFB(px,cp,sx,cy0,c,235);
        else PxBlend(sx,cy0,c,235);
      } else PxBlend(sx,cy0,c,235);
      px=sx; py=sy; } }
  // intersections
  if (eqShowInt&&eqOn[0]&&eqOn[1]&&eqSrc[0][0]&&eqSrc[1][0]){
    int found=0;
    for (int sx=1;sx<SCREEN_W&&found<8;sx++){
      if (!ok[0][sx]||!ok[1][sx]||!ok[0][sx-1]||!ok[1][sx-1]) continue;
      float d0=col[0][sx-1]-col[1][sx-1];
      float d1=col[0][sx]  -col[1][sx];
      if (d0*d1<=0 && fabsf(d0-d1)<1e3f){
        int sy=oy-(int)(col[0][sx]*eqScale);
        if (sy>GY0&&sy<GY1){
          RingFB(sx,sy,4,C_HILITE,220);
          Glow(sx,sy,C_HILITE,110,0.6f);
          found++; } } } }

  // slot rows
  for (int s=0;s<EQ_SLOTS;s++){
    int ry=GY1+2+s*8;
    if (ry>SCREEN_H-6) break; }
  Panel(0,20,SCREEN_W,22,NULL,C_ACCENT,NULL);
  for (int s=0;s<EQ_SLOTS;s++){
    int bx=4+s*104;
    bool sel=(eqEdit==s);
    uint16_t c=Spec(s*2);
    BlendRectFB(bx,23,100,16,sel?Dim(c,2,5):Dim(C_PANEL,5,5),A_FILL);
    Bracket(bx,23,100,16,eqOn[s]?c:Dim(c,2,5),4);
    char lab[EQ_LEN+4];
    snprintf(lab,sizeof(lab),"y=%s",eqSrc[s][0]?eqSrc[s]:"...");
    DrawText(bx+4,27,lab,eqOn[s]?C_TEXT:C_HAIR,1);
    if (touchDown&&touchX>=bx&&touchX<bx+100&&touchY>=23&&touchY<39){
      if (eqEdit==s) eqOn[s]=!eqOn[s];
      eqEdit=s; } }

  if (Button(2,SCREEN_H-22,52,18,"EDIT",C_HILITE,false)){
    kbGraphSlot=eqEdit;
    KbOpen("EQUATION y=",eqSrc[eqEdit],EQ_LEN-1,ST_GRAPHER,KBP_EQ); return; }
  if (Button(56,SCREEN_H-22,52,18,"RESET",C_ACCENT,false)){
    eqCX=0; eqCY=0; eqScale=26.0f; }
  if (Button(110,SCREEN_H-22,58,18,"INTERSEC",C_ACCENT,eqShowInt)) eqShowInt=!eqShowInt;
  { char b[46];
    snprintf(b,sizeof(b),"X %d.%02d  ZOOM %d",(int)eqCX,abs((int)(eqCX*100))%100,(int)eqScale);
    DrawText(174,SCREEN_H-17,b,C_DATA,1); }
  VLineFB(SCREEN_W-30,GY0,GY1-GY0,Dim(C_HAIR,3,5));
  DrawText(SCREEN_W-28,GY0+4,"Z",C_HAIR,1);
  DrawText(SCREEN_W-28,GY0+14,"O",C_HAIR,1);
  DrawText(SCREEN_W-28,GY0+24,"O",C_HAIR,1);
  DrawText(SCREEN_W-28,GY0+34,"M",C_HAIR,1);
  DrawParticles();
  TopBar("GRAPHER",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  13. PARAMETRIC CURVES
// ---------------------------------------------------------------------
#define PC_KINDS 6
static const char *PC_NAME[PC_KINDS]={
  "CIRCLE","SPIRAL","LISSAJOUS","EPICYCLOID","HYPOTROCHOID","ROSE" };
static int   pcKind=2;
static float pcA=0.42f, pcB=0.6f, pcC=0.35f, pcTrace=1.0f;
static float pcPhase=0;
void ScreenParametric(float dt){
  if (BackHit()) return;
  AchVisit(ST_PARAMETRIC);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  pcPhase+=dt*0.35f;
  Backdrop();
  int cx=132, cy=126;
  float R=82.0f;
  int N=420;
  float a=1.0f+pcA*7.0f, b=1.0f+pcB*7.0f, cc=0.05f+pcC*0.95f;
  int drawN=(int)(N*clampf(pcTrace,0.02f,1.0f));
  int px=0,py=0;
  for (int i=0;i<=drawN;i++){
    float t=TAU*3.0f*i/N;
    float x=0,y=0;
    switch (pcKind){
      case 0: x=fcos(t)*R; y=fsin(t)*R; break;
      case 1: { float r=R*i/(float)N; x=fcos(t*a)*r; y=fsin(t*a)*r; } break;
      case 2: x=fsin(t*a+pcPhase)*R; y=fsin(t*b)*R; break;
      case 3: { float k=a;
                x=(R*0.66f)*((k+1)*fcos(t)-cc*fcos((k+1)*t));
                y=(R*0.66f)*((k+1)*fsin(t)-cc*fsin((k+1)*t));
                x/=(k+1.4f); y/=(k+1.4f); x*=1.5f; y*=1.5f; } break;
      case 4: { float k=a, l=cc;
                x=(R*0.7f)*((1-l)*fcos(t)+l*k*fcos(t*(1-l)/(l<0.02f?0.02f:l)));
                y=(R*0.7f)*((1-l)*fsin(t)-l*k*fsin(t*(1-l)/(l<0.02f?0.02f:l)));
                x*=0.8f; y*=0.8f; } break;
      default:{ float r=R*fcos(a*t); x=fcos(t)*r; y=fsin(t)*r; } break; }
    int sx=cx+(int)x, sy=cy+(int)y;
    if (sx<-200||sx>520||sy<-200||sy>440){ px=0; continue; }
    if (i){
      uint16_t c=Spec((int)(i*6.0f/N));
      uint8_t al=(uint8_t)(120+135.0f*i/(drawN>0?drawN:1));
      LineFB(px,py,sx,sy,c,al); }
    px=sx; py=sy; }
  if (drawN>0){ Glow(px,py,C_HILITE,140,0.9f); CircleFB(px,py,2,C_TEXT,255); }
  RingFB(cx,cy,2,Dim(C_HAIR,3,5),160);

  Panel(220,22,96,190,"PARAMS",C_ACCENT,"CFG");
  if (Button(224,40,88,17,PC_NAME[pcKind],C_ACCENT,false)) pcKind=(pcKind+1)%PC_KINDS;
  SliderRow(226,80,80,"A",&pcA,C_ACCENT);
  SliderRow(226,110,80,"B",&pcB,C_ACCENT);
  SliderRow(226,140,80,"C",&pcC,C_ACCENT);
  SliderRow(226,170,80,"TRACE",&pcTrace,C_HILITE);
  { char b[36]; snprintf(b,sizeof(b),"PTS %d",drawN);
    DrawText(226,192,b,C_DATA,1); }
  DrawParticles();
  TopBar("PARAMETRIC",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  14. 3D FUNCTION SURFACE
// ---------------------------------------------------------------------
#define SF_MAX 26
static int   sfRes=18;
static int   sfFunc=0;
static float sfHeight=0.5f, sfSpin=0;
static bool  sfWire=false;
static const char *SF_NAME[5]={
  "sin(x)cos(y)","ripple","saddle","peaks","waves" };
static float SurfF(int f,float x,float y,float t){
  switch (f){
    case 0: return fsin(x*2.2f)*fcos(y*2.2f);
    case 1: { float r=sqrtf(x*x+y*y)*4.0f; return fsin(r-t*2.0f)/(1.0f+r*0.42f); }
    case 2: return (x*x-y*y)*1.1f;
    case 3: { float d1=(x-0.4f)*(x-0.4f)+(y-0.3f)*(y-0.3f);
              float d2=(x+0.5f)*(x+0.5f)+(y+0.4f)*(y+0.4f);
              return expf(-d1*6.0f)-0.8f*expf(-d2*7.0f); }
    default:return fsin(x*3.0f+t)*0.5f+fcos(y*2.0f-t*0.6f)*0.5f; }
}
void ScreenSurface(float dt){
  if (BackHit()) return;
  AchVisit(ST_SURFACE);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (touchActive&&pressY>BACK_H&&pressY<SCREEN_H-24&&pressX<214){
    rotY+=(touchX-lastTX)*0.011f;
    rotX=clampf(rotX+(touchY-lastTY)*0.009f,-1.3f,0.1f);
  } else sfSpin+=dt*0.20f;

  Backdrop();
  int R=clampi(sfRes,6,SF_MAX);
  float ry=rotY+sfSpin, rx=rotX-0.55f;
  float cxr=fcos(rx), sxr=fsin(rx), cyr=fcos(ry), syr=fsin(ry);
  int cx=110, cy=126;
  float S=78.0f, hs=sfHeight*1.5f;
  // project the grid once into a reusable static buffer
  static int16_t sxA[SF_MAX+1][SF_MAX+1], syA[SF_MAX+1][SF_MAX+1];
  static float   zA [SF_MAX+1][SF_MAX+1];
  for (int j=0;j<=R;j++) for (int i=0;i<=R;i++){
    float u=-1.0f+2.0f*i/R, v=-1.0f+2.0f*j/R;
    float h=SurfF(sfFunc,u,v,gTime)*hs;
    float X=u, Y=h, Z=v;
    float x1=X*cyr - Z*syr, z1=X*syr + Z*cyr;
    float y1=Y*cxr - z1*sxr, z2=Y*sxr + z1*cxr;
    float d=3.4f+z2;
    if (d<0.4f) d=0.4f;
    sxA[j][i]=(int16_t)(cx+x1*S*2.1f/d);
    syA[j][i]=(int16_t)(cy-y1*S*2.1f/d);
    zA [j][i]=h; }
  // painter's order: back rows first
  for (int j=0;j<R;j++)
    for (int i=0;i<R;i++){
      float h=(zA[j][i]+zA[j][i+1]+zA[j+1][i]+zA[j+1][i+1])*0.25f;
      float n=clampf((h/(hs+1e-3f))*0.5f+0.5f,0,1);
      uint16_t c=Spec((int)(n*5.99f));
      uint8_t a=(uint8_t)(120+120*n);
      if (sfWire){
        LineFB(sxA[j][i],syA[j][i],sxA[j][i+1],syA[j][i+1],c,a);
        LineFB(sxA[j][i],syA[j][i],sxA[j+1][i],syA[j+1][i],c,a);
      } else {
        // fill the quad by scanning between the two diagonals (small quads)
        int x0=sxA[j][i],   y0=syA[j][i];
        int x1=sxA[j][i+1], y1=syA[j][i+1];
        int x2=sxA[j+1][i+1],y2=syA[j+1][i+1];
        int x3=sxA[j+1][i], y3=syA[j+1][i];
        int mnY=y0,mxY=y0;
        if (y1<mnY)mnY=y1; if (y1>mxY)mxY=y1;
        if (y2<mnY)mnY=y2; if (y2>mxY)mxY=y2;
        if (y3<mnY)mnY=y3; if (y3>mxY)mxY=y3;
        if (mxY-mnY>60){ continue; }
        for (int sy=mnY;sy<=mxY;sy++){
          if (sy<20||sy>=SCREEN_H) continue;
          int lo=32767, hi=-32768;
          int qx[4]={x0,x1,x2,x3}, qy[4]={y0,y1,y2,y3};
          for (int e=0;e<4;e++){
            int nEdge=(e+1)&3;
            int ay2=qy[e], by2=qy[nEdge];
            if ((sy>=ay2&&sy<by2)||(sy>=by2&&sy<ay2)){
              float t=(float)(sy-ay2)/(float)(by2-ay2);
              int xx=qx[e]+(int)((qx[nEdge]-qx[e])*t);
              if (xx<lo) lo=xx; if (xx>hi) hi=xx; } }
          if (lo>hi) continue;
          lo=clampi(lo,0,SCREEN_W-1); hi=clampi(hi,0,SCREEN_W-1);
          BlendRectFB(lo,sy,hi-lo+1,1,c,a); }
        LineFB(x0,y0,x1,y1,Dim(c,4,5),(uint8_t)(a>>1)); } }

  Panel(216,22,100,190,"SURFACE",C_ACCENT,"FN");
  if (Button(220,40,92,17,SF_NAME[sfFunc],C_ACCENT,false)) sfFunc=(sfFunc+1)%5;
  SliderRow(222,80,84,"HEIGHT",&sfHeight,C_ACCENT);
  { float rr=(float)(sfRes-6)/(SF_MAX-6);
    if (SliderRow(222,110,84,"RES",&rr,C_ACCENT)) sfRes=6+(int)(rr*(SF_MAX-6)); }
  SliderRow(222,140,84,"CAMERA",&sCam,C_ACCENT);
  if (Button(220,164,92,17,sfWire?"WIREFRAME":"SOLID",C_ACCENT,sfWire)) sfWire=!sfWire;
  { char b[28]; snprintf(b,sizeof(b),"QUADS %d",R*R);
    DrawText(222,190,b,C_DATA,1); }
  DrawParticles();
  TopBar("3D SURFACE",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  15. VECTOR PLAYGROUND
// ---------------------------------------------------------------------
static float vpAx=70, vpAy=-40, vpBx=40, vpBy=52;
static int   vpDrag=0;                 // 0 none, 1 A, 2 B
static int   vpShow=0;                 // 0 sum, 1 diff, 2 both
static void VpArrow(int cx,int cy,float vx,float vy,uint16_t c,const char *lab){
  int tx=cx+(int)vx, ty=cy+(int)vy;
  LineFB(cx,cy,tx,ty,c,240);
  LineFB(cx,cy+1,tx,ty+1,Dim(c,3,5),160);
  float l=sqrtf(vx*vx+vy*vy);
  if (l>4.0f){
    float ux=vx/l, uy=vy/l;
    int hx=tx-(int)(ux*9), hy=ty-(int)(uy*9);
    LineFB(tx,ty,hx-(int)(uy*5),hy+(int)(ux*5),c,240);
    LineFB(tx,ty,hx+(int)(uy*5),hy-(int)(ux*5),c,240);
    LineFB(hx-(int)(uy*5),hy+(int)(ux*5),hx+(int)(uy*5),hy-(int)(ux*5),Dim(c,3,5),200); }
  if (lab) DrawText(tx+4,ty-4,lab,c,1);
  Glow(tx,ty,c,60,0.5f);
}
void ScreenVectors(float dt){
  if (BackHit()) return;
  AchVisit(ST_VECTORS);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  int cx=110, cy=128;
  if (touchDown&&touchY>BACK_H){
    float da=(touchX-(cx+vpAx))*(touchX-(cx+vpAx))+(touchY-(cy+vpAy))*(touchY-(cy+vpAy));
    float db=(touchX-(cx+vpBx))*(touchX-(cx+vpBx))+(touchY-(cy+vpBy))*(touchY-(cy+vpBy));
    if (da<520.0f&&da<=db) vpDrag=1;
    else if (db<520.0f)    vpDrag=2;
    else vpDrag=0; }
  if (!touchActive) vpDrag=0;
  if (vpDrag==1){ vpAx=clampf(touchX-cx,-105,105); vpAy=clampf(touchY-cy,-96,96); }
  if (vpDrag==2){ vpBx=clampf(touchX-cx,-105,105); vpBy=clampf(touchY-cy,-96,96); }

  Backdrop();
  // axes + unit grid
  for (int g=-4;g<=4;g++){
    VLineFB(cx+g*26,32,SCREEN_H-46,Dim(C_HAIR,3,5));
    HLineFB(4,cy+g*26,208,Dim(C_HAIR,3,5)); }
  VLineFB(cx,32,SCREEN_H-46,Dim(C_ACCENT,2,5));
  HLineFB(4,cy,208,Dim(C_ACCENT,2,5));
  // parallelogram construction
  if (vpShow==0||vpShow==2){
    LineFB(cx+(int)vpAx,cy+(int)vpAy,cx+(int)(vpAx+vpBx),cy+(int)(vpAy+vpBy),Dim(C_HAIR,4,5),120);
    LineFB(cx+(int)vpBx,cy+(int)vpBy,cx+(int)(vpAx+vpBx),cy+(int)(vpAy+vpBy),Dim(C_HAIR,4,5),120);
    VpArrow(cx,cy,vpAx+vpBx,vpAy+vpBy,C_HILITE,"A+B"); }
  if (vpShow==1||vpShow==2)
    VpArrow(cx,cy,vpAx-vpBx,vpAy-vpBy,Spec(3),"A-B");
  VpArrow(cx,cy,vpAx,vpAy,C_ACCENT,"A");
  VpArrow(cx,cy,vpBx,vpBy,C_DATA,"B");
  if (vpDrag){ int hx=cx+(int)(vpDrag==1?vpAx:vpBx), hy=cy+(int)(vpDrag==1?vpAy:vpBy);
               RingFB(hx,hy,8+(int)(2*Pulse(gTime,6.0f)),C_HILITE,180); }

  // maths panel -- note screen Y is inverted for a maths reading
  float ax=vpAx/26.0f, ay=-vpAy/26.0f, bx=vpBx/26.0f, by=-vpBy/26.0f;
  float la=sqrtf(ax*ax+ay*ay), lb=sqrtf(bx*bx+by*by);
  float dot=ax*bx+ay*by;
  float crs=ax*by-ay*bx;
  float angA=atan2f(ay,ax)*57.2958f;
  float angB=atan2f(by,bx)*57.2958f;
  float between=(la>1e-3f&&lb>1e-3f)?acosf(clampf(dot/(la*lb),-1,1))*57.2958f:0;
  Panel(216,22,100,190,"ALGEBRA",C_ACCENT,"VEC");
  int ry=40; char b[36];
  #define VROW(k,v) do{ DrawText(222,ry,k,C_SAND,1); \
                        DrawText(316-TextW(v,1)-6,ry,v,C_DATA,1); ry+=13; }while(0)
  snprintf(b,sizeof(b),"%d.%01d,%d.%01d",(int)ax,abs((int)(ax*10))%10,(int)ay,abs((int)(ay*10))%10);
  VROW("A",b);
  snprintf(b,sizeof(b),"%d.%01d,%d.%01d",(int)bx,abs((int)(bx*10))%10,(int)by,abs((int)(by*10))%10);
  VROW("B",b);
  snprintf(b,sizeof(b),"%d.%02d",(int)la,abs((int)(la*100))%100); VROW("|A|",b);
  snprintf(b,sizeof(b),"%d.%02d",(int)lb,abs((int)(lb*100))%100); VROW("|B|",b);
  snprintf(b,sizeof(b),"%d",(int)angA);    VROW("ANG A",b);
  snprintf(b,sizeof(b),"%d",(int)angB);    VROW("ANG B",b);
  snprintf(b,sizeof(b),"%d",(int)between); VROW("A^B",b);
  snprintf(b,sizeof(b),"%d.%02d",(int)dot,abs((int)(dot*100))%100); VROW("A.B",b);
  snprintf(b,sizeof(b),"%d.%02d",(int)crs,abs((int)(crs*100))%100); VROW("AxB",b);
  #undef VROW
  if (Button(220,190,92,17,vpShow==0?"SUM":(vpShow==1?"DIFF":"BOTH"),C_ACCENT,false))
    vpShow=(vpShow+1)%3;
  DrawText(6,SCREEN_H-11,"DRAG THE ARROW TIPS",C_HAIR,1);
  DrawParticles();
  TopBar("VECTORS",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  16. MATRIX VISUALIZER
// ---------------------------------------------------------------------
static float mvA=1, mvB=0, mvC=0, mvD=1;      // the live 2x2
static float mvTA=1,mvTB=0,mvTC=0,mvTD=1;     // animated target
static int   mvMode=0;                        // 0 = 2D shape, 1 = 3D object
static int   mvOp=0;
static const char *MV_OP[5]={"IDENTITY","ROTATE","SCALE","SHEAR","REFLECT"};
void ScreenMatrixViz(float dt){
  if (BackHit()) return;
  AchVisit(ST_MATRIXVIZ);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  // smooth interpolation toward the target matrix
  float k=1.0f-powf(0.5f,dt/0.16f);
  mvA+=(mvTA-mvA)*k; mvB+=(mvTB-mvB)*k;
  mvC+=(mvTC-mvC)*k; mvD+=(mvTD-mvD)*k;

  Backdrop();
  int cx=112, cy=126;
  float S=30.0f;
  // transformed grid
  for (int g=-3;g<=3;g++){
    int p0x=cx+(int)((mvA*g*1.0f+mvB*-3.0f)*S), p0y=cy-(int)((mvC*g+mvD*-3.0f)*S);
    int p1x=cx+(int)((mvA*g*1.0f+mvB* 3.0f)*S), p1y=cy-(int)((mvC*g+mvD* 3.0f)*S);
    LineFB(p0x,p0y,p1x,p1y,(g==0)?Dim(C_ACCENT,2,5):Dim(C_HAIR,3,5),150);
    int q0x=cx+(int)((mvA*-3.0f+mvB*g)*S), q0y=cy-(int)((mvC*-3.0f+mvD*g)*S);
    int q1x=cx+(int)((mvA* 3.0f+mvB*g)*S), q1y=cy-(int)((mvC* 3.0f+mvD*g)*S);
    LineFB(q0x,q0y,q1x,q1y,(g==0)?Dim(C_ACCENT,2,5):Dim(C_HAIR,3,5),150); }
  if (mvMode==0){
    // the unit square + a house shape so orientation is readable
    const float P[7][2]={{0,0},{1,0},{1,1},{0.5f,1.5f},{0,1},{0,0},{1,1}};
    int px=0,py=0;
    for (int i=0;i<6;i++){
      float x=P[i][0], y=P[i][1];
      int sx=cx+(int)((mvA*x+mvB*y)*S), sy=cy-(int)((mvC*x+mvD*y)*S);
      if (i) LineFB(px,py,sx,sy,C_HILITE,240);
      CircleFB(sx,sy,2,Spec(i),230);
      px=sx; py=sy; }
    // basis vectors
    VpArrow(cx,cy,mvA*S,-mvC*S,C_ACCENT,"i");
    VpArrow(cx,cy,mvB*S,-mvD*S,C_DATA,"j");
  } else {
    // apply the 2x2 as a shear/scale on the X/Y of a 3D object
    ApplyLight();
    RenderMesh(gMesh[gObj],rotX,rotY+mvB*1.4f,rotZ+mvC*1.2f,
               clampf((fabsf(mvA)+fabsf(mvD))*0.5f,0.25f,2.2f),
               (float)cx,(float)cy,3.2f,gMode,
               (gMode==M_WIRE||gMode==M_NEON||gMode==M_HOLO)?C_ACCENT:0);
    UpdateOrbit(dt); }

  Panel(216,22,100,190,"MATRIX",C_ACCENT,"2x2");
  { char b[20];
    snprintf(b,sizeof(b),"%s%d.%02d",mvA<0?"-":" ",abs((int)mvA),abs((int)(mvA*100))%100);
    DrawText(222,44,b,C_DATA,1);
    snprintf(b,sizeof(b),"%s%d.%02d",mvB<0?"-":" ",abs((int)mvB),abs((int)(mvB*100))%100);
    DrawText(272,44,b,C_DATA,1);
    snprintf(b,sizeof(b),"%s%d.%02d",mvC<0?"-":" ",abs((int)mvC),abs((int)(mvC*100))%100);
    DrawText(222,58,b,C_DATA,1);
    snprintf(b,sizeof(b),"%s%d.%02d",mvD<0?"-":" ",abs((int)mvD),abs((int)(mvD*100))%100);
    DrawText(272,58,b,C_DATA,1);
    VLineFB(218,40,32,C_ACCENT); VLineFB(313,40,32,C_ACCENT);
    float det=mvA*mvD-mvB*mvC;
    snprintf(b,sizeof(b),"DET %s%d.%02d",det<0?"-":"",abs((int)det),abs((int)(det*100))%100);
    DrawText(222,78,b,det<0?C_WARN:C_HILITE,1);
    snprintf(b,sizeof(b),"TR  %d.%02d",(int)(mvA+mvD),abs((int)((mvA+mvD)*100))%100);
    DrawText(222,90,b,C_DATA,1); }
  for (int i=0;i<5;i++){
    if (Button(220,106+i*18,92,16,MV_OP[i],C_ACCENT,mvOp==i)){
      mvOp=i;
      float a2=gTime*0.0f+0.6f;
      switch (i){
        case 0: mvTA=1; mvTB=0; mvTC=0; mvTD=1; break;
        case 1: mvTA=fcos(a2); mvTB=-fsin(a2); mvTC=fsin(a2); mvTD=fcos(a2); break;
        case 2: mvTA=1.6f; mvTB=0; mvTC=0; mvTD=0.62f; break;
        case 3: mvTA=1; mvTB=0.8f; mvTC=0; mvTD=1; break;
        default:mvTA=-1; mvTB=0; mvTC=0; mvTD=1; break; }
      Impact(1.0f); } }
  if (Button(220,196,92,15,mvMode?"3D OBJECT":"2D SHAPE",C_HILITE,false))
    mvMode=!mvMode;
  DrawParticles();
  TopBar("MATRIX LAB",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  17. FOURIER VISUALIZER
//  The user draws a waveform; it is sampled to FO_N points and
//  decomposed with a direct DFT into FO_H harmonics (epicycles).
// ---------------------------------------------------------------------
#define FO_N 96
#define FO_H 14
static float foWave[FO_N];
static float foRe[FO_H], foIm[FO_H];
static bool  foHave=false;
static int   foHarm=8;
static float foPhase=0;
static bool  foDrawing=false;
static int   foLastX=-1;
static void FourierAnalyse(void){
  for (int h=0;h<FO_H;h++){
    float re=0, im=0;
    for (int n=0;n<FO_N;n++){
      float a=TAU*h*n/FO_N;
      re+=foWave[n]*fcos(a);
      im-=foWave[n]*fsin(a); }
    foRe[h]=re*2.0f/FO_N;
    foIm[h]=im*2.0f/FO_N; }
  foRe[0]*=0.5f; foIm[0]*=0.5f;
  foHave=true;
}
void ScreenFourier(float dt){
  if (BackHit()) return;
  AchVisit(ST_FOURIER);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int DY0=40, DY1=118, DMID=(DY0+DY1)/2;
  if (!foHave){
    for (int i=0;i<FO_N;i++)
      foWave[i]=fsin(TAU*i/FO_N)*0.6f+fsin(TAU*3*i/FO_N)*0.2f;
    FourierAnalyse(); }
  foPhase+=dt*0.85f;
  if (foPhase>TAU) foPhase-=TAU;

  // draw into the top strip
  if (touchActive&&touchY>=DY0&&touchY<=DY1&&touchX>=8&&touchX<8+FO_N*3){
    int idx=clampi((touchX-8)/3,0,FO_N-1);
    float v=clampf((DMID-touchY)/(float)((DY1-DY0)/2),-1,1);
    if (!foDrawing){ foDrawing=true; foLastX=idx; }
    int a=foLastX<idx?foLastX:idx, b=foLastX<idx?idx:foLastX;
    for (int i=a;i<=b;i++) foWave[i]=v;
    foLastX=idx; }
  else if (foDrawing){ foDrawing=false; FourierAnalyse(); }

  Backdrop();
  // source waveform
  BlendRectFB(6,DY0-2,FO_N*3+4,DY1-DY0+4,C_PANEL,A_FILL);
  Bracket(6,DY0-2,FO_N*3+4,DY1-DY0+4,C_ACCENT,5);
  HLineFB(8,DMID,FO_N*3,Dim(C_HAIR,3,5));
  for (int i=1;i<FO_N;i++)
    LineFB(8+(i-1)*3,DMID-(int)(foWave[i-1]*((DY1-DY0)/2)),
           8+i*3,    DMID-(int)(foWave[i]  *((DY1-DY0)/2)),C_DATA,220);
  DrawText(10,DY0-12,"DRAW A WAVEFORM HERE",C_HAIR,1);

  // epicycles
  int ex=246, ey=88;
  float px=(float)ex, py=(float)ey;
  int nh=clampi(foHarm,1,FO_H);
  for (int h=0;h<nh;h++){
    float amp=sqrtf(foRe[h]*foRe[h]+foIm[h]*foIm[h])*34.0f;
    if (amp<0.4f) continue;
    float ph=atan2f(foIm[h],foRe[h]);
    float a=h*foPhase+ph;
    float nx=px+fcos(a)*amp, ny=py+fsin(a)*amp;
    RingFB((int)px,(int)py,(int)amp,Dim(Spec(h%6),2,6),120);
    LineFB((int)px,(int)py,(int)nx,(int)ny,Spec(h%6),220);
    px=nx; py=ny; }
  CircleFB((int)px,(int)py,2,C_HILITE,255);
  Glow((int)px,(int)py,C_HILITE,110,0.7f);

  // reconstruction trace, drawn from the coefficients (no history buffer)
  int ry0=176;
  BlendRectFB(6,ry0-30,FO_N*3+4,60,C_PANEL,A_FILL);
  Bracket(6,ry0-30,FO_N*3+4,60,C_HILITE,5);
  HLineFB(8,ry0,FO_N*3,Dim(C_HAIR,3,5));
  int lx=0,ly=0;
  for (int i=0;i<FO_N;i++){
    float t=TAU*i/FO_N;
    float v=0;
    for (int h=0;h<nh;h++) v+=foRe[h]*fcos(h*t)-foIm[h]*fsin(h*t);
    int sx=8+i*3, sy=ry0-(int)(clampf(v,-1.6f,1.6f)*26.0f);
    if (i) LineFB(lx,ly,sx,sy,C_HILITE,235);
    lx=sx; ly=sy; }
  DrawText(10,ry0+22,"RECONSTRUCTION",C_HAIR,1);
  // link the epicycle tip to the trace head
  { int hi=(int)(foPhase/TAU*FO_N)%FO_N;
    LineFB((int)px,(int)py,8+hi*3,ry0-(int)(foWave[hi]*26.0f),Dim(C_HILITE,2,6),90); }

  { char b[30]; snprintf(b,sizeof(b),"HARMONICS %d",nh);
    DrawText(216,150,b,C_DATA,1); }
  if (Button(214,162,44,17,"H-",C_ACCENT,false)) foHarm=clampi(foHarm-1,1,FO_H);
  if (Button(262,162,44,17,"H+",C_ACCENT,false)) foHarm=clampi(foHarm+1,1,FO_H);
  if (Button(214,184,92,17,"ANALYSE",C_HILITE,false)) FourierAnalyse();
  if (Button(214,204,92,15,"SQUARE",C_ACCENT,false)){
    for (int i=0;i<FO_N;i++) foWave[i]=(i<FO_N/2)?0.75f:-0.75f;
    FourierAnalyse(); }
  DrawParticles();
  TopBar("FOURIER",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// =====================================================================
//  v7  --  ANIMATION, CREATOR, SEARCH, SYSTEM
// =====================================================================

// ---------------------------------------------------------------------
//  18. RAGDOLL LAB
//  Verlet particles + distance constraints. Original simple figures.
// ---------------------------------------------------------------------
#define RD_MAX      3
#define RD_PARTS    10
#define RD_LINKS    12
struct RdPt { float x,y,px,py; bool pin; };
struct RdLk { uint8_t a,b; float len; };
struct Ragdoll { RdPt p[RD_PARTS]; RdLk l[RD_LINKS]; int nl; bool alive; uint16_t col; };
static Ragdoll rdoll[RD_MAX];
static int  rdN=0;
static int  rdGrabDoll=-1, rdGrabPt=-1;
static float rdGrav=0.55f, rdBounce=0.35f;
// joints: 0 head 1 neck 2 hip 3 Lsh 4 Lha 5 Rsh 6 Rha 7 Lfo 8 Rfo 9 chest
static void RdSpawn(float x,float y,uint16_t col){
  if (rdN>=RD_MAX) {                       // recycle the oldest
    for (int i=1;i<RD_MAX;i++) rdoll[i-1]=rdoll[i];
    rdN=RD_MAX-1; }
  Ragdoll &r=rdoll[rdN];
  const float P[RD_PARTS][2]={
    {0,-26},{0,-16},{0,4},{-9,-13},{-15,2},{9,-13},{15,2},{-6,22},{6,22},{0,-6} };
  for (int i=0;i<RD_PARTS;i++){
    r.p[i].x=x+P[i][0]; r.p[i].y=y+P[i][1];
    r.p[i].px=r.p[i].x-(Hash(i*331u+rdN*77u)-0.5f)*3.0f;
    r.p[i].py=r.p[i].y;
    r.p[i].pin=false; }
  const uint8_t L[RD_LINKS][2]={
    {0,1},{1,9},{9,2},{1,3},{3,4},{1,5},{5,6},{2,7},{2,8},{9,3},{9,5},{7,8} };
  r.nl=RD_LINKS;
  for (int i=0;i<RD_LINKS;i++){
    r.l[i].a=L[i][0]; r.l[i].b=L[i][1];
    float dx=r.p[L[i][1]].x-r.p[L[i][0]].x;
    float dy=r.p[L[i][1]].y-r.p[L[i][0]].y;
    r.l[i].len=sqrtf(dx*dx+dy*dy); }
  r.alive=true; r.col=col;
  rdN++;
}
void ScreenRagdoll(float dt){
  if (BackHit()) return;
  AchVisit(ST_RAGDOLL);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  if (rdN==0){ RdSpawn(120,90,C_ACCENT); RdSpawn(200,70,Spec(1)); }
  bool slow = touchActive && touchY>SCREEN_H-22;
  float sdt = BulletDt(clampf(dt,0,0.033f),slow);
  const float FLOOR=SCREEN_H-8.0f;

  // grab a limb
  if (touchDown&&touchY>BACK_H&&touchY<SCREEN_H-22){
    float best=420.0f;
    rdGrabDoll=-1; rdGrabPt=-1;
    for (int d=0;d<rdN;d++){
      if (!rdoll[d].alive) continue;
      for (int i=0;i<RD_PARTS;i++){
        float dx=touchX-rdoll[d].p[i].x, dy=touchY-rdoll[d].p[i].y;
        float q=dx*dx+dy*dy;
        if (q<best){ best=q; rdGrabDoll=d; rdGrabPt=i; } } } }
  if (!touchActive){
    if (rdGrabDoll>=0&&rdGrabPt>=0) rdoll[rdGrabDoll].p[rdGrabPt].pin=false;
    rdGrabDoll=-1; rdGrabPt=-1; }

  // verlet integrate
  for (int d=0;d<rdN;d++){
    Ragdoll &r=rdoll[d];
    if (!r.alive) continue;
    for (int i=0;i<RD_PARTS;i++){
      RdPt &p=r.p[i];
      if (d==rdGrabDoll&&i==rdGrabPt&&touchActive){
        p.px=p.x; p.py=p.y;
        p.x=(float)touchX; p.y=(float)touchY;
        continue; }
      float vx=(p.x-p.px)*0.992f, vy=(p.y-p.py)*0.992f;
      // clamp so a hard throw cannot explode the solver
      vx=clampf(vx,-26,26); vy=clampf(vy,-26,26);
      p.px=p.x; p.py=p.y;
      p.x+=vx; p.y+=vy+rdGrav*900.0f*sdt*sdt*60.0f*0.016f; }
    // constraint relaxation
    for (int it=0;it<4;it++){
      for (int k=0;k<r.nl;k++){
        RdPt &a=r.p[r.l[k].a]; RdPt &b=r.p[r.l[k].b];
        float dx=b.x-a.x, dy=b.y-a.y;
        float d2=dx*dx+dy*dy;
        if (d2<1e-6f) continue;
        float dd=sqrtf(d2);
        float diff=(dd-r.l[k].len)/dd*0.5f;
        bool aFix=(d==rdGrabDoll&&(int)r.l[k].a==rdGrabPt&&touchActive);
        bool bFix=(d==rdGrabDoll&&(int)r.l[k].b==rdGrabPt&&touchActive);
        if (!aFix){ a.x+=dx*diff*(bFix?1.0f:1.0f); a.y+=dy*diff*(bFix?1.0f:1.0f); }
        if (!bFix){ b.x-=dx*diff; b.y-=dy*diff; } }
      // floor + walls
      for (int i=0;i<RD_PARTS;i++){
        RdPt &p=r.p[i];
        if (p.y>FLOOR){
          float vy=p.y-p.py;
          p.y=FLOOR; p.py=p.y+vy*rdBounce;
          p.px=p.x-(p.x-p.px)*0.86f;                 // friction
          if (vy>7.0f&&it==0) Impact(clampf(vy*0.12f,0.5f,3.0f)); }
        if (p.y<26){ float vy=p.y-p.py; p.y=26; p.py=p.y+vy*0.4f; }
        if (p.x<5){  float vx=p.x-p.px; p.x=5; p.px=p.x+vx*rdBounce; }
        if (p.x>SCREEN_W-5){ float vx=p.x-p.px; p.x=SCREEN_W-5; p.px=p.x+vx*rdBounce; } } } }

  Backdrop();
  FillRectFB(0,(int)FLOOR,SCREEN_W,SCREEN_H-(int)FLOOR,Dim(C_SAND,2,7));
  HLineFB(0,(int)FLOOR,SCREEN_W,C_ACCENT);
  for (int d=0;d<rdN;d++){
    Ragdoll &r=rdoll[d];
    if (!r.alive) continue;
    uint16_t c=r.col;
    // limbs
    for (int k=0;k<r.nl;k++){
      if (k==11) continue;                    // the foot brace stays hidden
      RdPt &a=r.p[r.l[k].a]; RdPt &b=r.p[r.l[k].b];
      LineFB((int)a.x,(int)a.y,(int)b.x,(int)b.y,c,240);
      LineFB((int)a.x,(int)a.y+1,(int)b.x,(int)b.y+1,Dim(c,3,5),150); }
    // head
    CircleFB((int)r.p[0].x,(int)r.p[0].y,5,Dim(c,3,5),235);
    RingFB((int)r.p[0].x,(int)r.p[0].y,5,c,255);
    if (d==rdGrabDoll&&rdGrabPt>=0){
      RingFB((int)r.p[rdGrabPt].x,(int)r.p[rdGrabPt].y,
             8+(int)(2*Pulse(gTime,6.0f)),C_HILITE,190); }
    for (int i=0;i<RD_PARTS;i++) PxAdd((int)r.p[i].x,(int)r.p[i].y,C_HILITE,90); }
  ImpactFlash();

  if (Button(2,22,50,18,"ADD",C_ACCENT,false)){
    RdSpawn(60+Hash((uint32_t)(gTime*100))*200.0f,60,Spec((int)(gTime)%6));
    Impact(1.0f); }
  if (Button(54,22,52,18,"CLEAR",C_WARN,false)) rdN=0;
  if (Button(108,22,58,18,"LAUNCH",C_HILITE,false)){
    for (int d=0;d<rdN;d++) for (int i=0;i<RD_PARTS;i++){
      rdoll[d].p[i].px=rdoll[d].p[i].x+(Hash(i*97u+d*31u)-0.5f)*16.0f;
      rdoll[d].p[i].py=rdoll[d].p[i].y+9.0f; }
    Impact(4.0f); SpawnBurst(160,140,30,C_HILITE,240.0f,PK_SPARK); }
  SliderRow(176,30,64,"GRAV",&rdGrav,C_ACCENT);
  SliderRow(248,30,64,"BOUNCE",&rdBounce,C_ACCENT);
  { char b[30]; snprintf(b,sizeof(b),"DOLLS %d",rdN);
    DrawText(6,SCREEN_H-11,b,C_DATA,1); }
  DrawText(SCREEN_W-124,SCREEN_H-11,"HOLD LOW = SLOW-MO",C_HAIR,1);
  BulletOverlay();
  DrawParticles();
  TopBar("RAGDOLL LAB",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  19. ANIMATION TIMELINE
//  A tiny keyframe editor: one object, up to TL_KEYS keys of
//  position / rotation / scale, linearly interpolated.
// ---------------------------------------------------------------------
#define TL_KEYS 8
struct TlKey { float t; float x,y,rot,scl; bool used; };
static TlKey tlK[TL_KEYS];
static int   tlSel=0;
static float tlTime=0, tlDur=4.0f;
static bool  tlPlay=false, tlLoop=true;
static bool  tlInit=false;
static int   tlDragKey=-1;
static void TlReset(void){
  for (int i=0;i<TL_KEYS;i++) tlK[i].used=false;
  tlK[0].used=true; tlK[0].t=0.0f;  tlK[0].x=60;  tlK[0].y=140; tlK[0].rot=0;    tlK[0].scl=0.7f;
  tlK[1].used=true; tlK[1].t=0.45f; tlK[1].x=160; tlK[1].y=80;  tlK[1].rot=2.2f; tlK[1].scl=1.3f;
  tlK[2].used=true; tlK[2].t=1.0f;  tlK[2].x=262; tlK[2].y=150; tlK[2].rot=5.0f; tlK[2].scl=0.8f;
  tlSel=0; tlTime=0; tlInit=true;
}
static void TlSample(float t,float &x,float &y,float &rot,float &scl){
  int a=-1,b=-1;
  for (int i=0;i<TL_KEYS;i++){
    if (!tlK[i].used) continue;
    if (tlK[i].t<=t && (a<0||tlK[i].t>tlK[a].t)) a=i;
    if (tlK[i].t>=t && (b<0||tlK[i].t<tlK[b].t)) b=i; }
  if (a<0&&b<0){ x=160; y=120; rot=0; scl=1; return; }
  if (a<0) a=b;
  if (b<0) b=a;
  float span=tlK[b].t-tlK[a].t;
  float k=(span>1e-4f)?clampf((t-tlK[a].t)/span,0,1):0.0f;
  k=EaseOutCubic(k)*0.5f+k*0.5f;               // gentle ease, still readable
  x  =tlK[a].x  +(tlK[b].x  -tlK[a].x  )*k;
  y  =tlK[a].y  +(tlK[b].y  -tlK[a].y  )*k;
  rot=tlK[a].rot+(tlK[b].rot-tlK[a].rot)*k;
  scl=tlK[a].scl+(tlK[b].scl-tlK[a].scl)*k;
}
void ScreenTimeline(float dt){
  if (BackHit()) return;
  AchVisit(ST_TIMELINE);
  if (!tlInit) TlReset();
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  const int TLY=SCREEN_H-40, TLX=14, TLW=SCREEN_W-28;
  if (tlPlay){
    tlTime+=dt/(tlDur>0.2f?tlDur:0.2f);
    if (tlTime>1.0f){ if (tlLoop) tlTime-=1.0f; else { tlTime=1.0f; tlPlay=false; } } }

  Backdrop();
  // ghost trail of the whole path
  { float px=0,py=0;
    for (int i=0;i<=40;i++){
      float t=(float)i/40.0f, x,y,r,s;
      TlSample(t,x,y,r,s);
      if (i) LineFB((int)px,(int)py,(int)x,(int)y,Dim(C_HAIR,3,5),110);
      px=x; py=y; } }
  // the animated object
  { float x,y,r,s;
    TlSample(tlTime,x,y,r,s);
    ApplyLight();
    RenderMesh(gMesh[gObj],r*0.4f,r,r*0.7f,clampf(s,0.15f,2.2f),
               x,y,3.0f,gMode,(gMode==M_WIRE||gMode==M_NEON||gMode==M_HOLO)?C_ACCENT:0);
    RingFB((int)x,(int)y,3,C_HILITE,150); }
  // keys on the track
  BlendRectFB(TLX-4,TLY-10,TLW+8,26,C_PANEL,A_FILL);
  Bracket(TLX-4,TLY-10,TLW+8,26,C_ACCENT,5);
  HLineFB(TLX,TLY,TLW,C_HAIR);
  for (int i=0;i<=8;i++) VLineFB(TLX+TLW*i/8,TLY-3,7,Dim(C_HAIR,4,5));
  for (int i=0;i<TL_KEYS;i++){
    if (!tlK[i].used) continue;
    int kx=TLX+(int)(tlK[i].t*TLW);
    uint16_t c=(i==tlSel)?C_HILITE:C_ACCENT;
    for (int o=-4;o<=4;o++){
      int hh=4-abs(o);
      VLineFB(kx+o,TLY-hh,hh*2+1,c); }
    if (i==tlSel) Glow(kx,TLY,C_HILITE,110,0.7f); }
  { int ph=TLX+(int)(tlTime*TLW);
    VLineFB(ph,TLY-10,22,C_HILITE);
    Glow(ph,TLY,C_HILITE,130,0.6f);
    CircleFB(ph,TLY-11,2,C_TEXT,255); }
  // scrub / select on the track
  if (touchDown&&touchY>TLY-14&&touchY<TLY+16){
    float t=clampf((float)(touchX-TLX)/TLW,0,1);
    int near=-1; float bd=0.055f;
    for (int i=0;i<TL_KEYS;i++){
      if (!tlK[i].used) continue;
      float d=fabsf(tlK[i].t-t);
      if (d<bd){ bd=d; near=i; } }
    if (near>=0){ tlSel=near; tlDragKey=near; }
    else { tlTime=t; tlPlay=false; tlDragKey=-1; } }
  if (!touchActive) tlDragKey=-1;
  if (tlDragKey>=0&&touchActive&&tlDragKey!=0)
    tlK[tlDragKey].t=clampf((float)(touchX-TLX)/TLW,0,1);
  // drag the object itself to author the selected key
  if (touchActive&&touchY>BACK_H+20&&touchY<TLY-20&&tlK[tlSel].used){
    tlK[tlSel].x=clampf((float)touchX,20,SCREEN_W-20);
    tlK[tlSel].y=clampf((float)touchY,40,(float)(TLY-30));
    tlTime=tlK[tlSel].t; tlPlay=false; }

  if (Button(2,22,44,18,tlPlay?"PAUSE":"PLAY",tlPlay?C_WARN:C_HILITE,tlPlay))
    tlPlay=!tlPlay;
  if (Button(48,22,40,18,"LOOP",C_ACCENT,tlLoop)) tlLoop=!tlLoop;
  if (Button(90,22,40,18,"ADD",C_ACCENT,false)){
    for (int i=0;i<TL_KEYS;i++)
      if (!tlK[i].used){
        float x,y,r,s; TlSample(tlTime,x,y,r,s);
        tlK[i].used=true; tlK[i].t=tlTime;
        tlK[i].x=x; tlK[i].y=y; tlK[i].rot=r; tlK[i].scl=s;
        tlSel=i; SpawnBurst(TLX+tlTime*TLW,TLY,10,C_HILITE,90.0f,PK_SPARK);
        break; } }
  if (Button(132,22,40,18,"DEL",C_WARN,false)){
    int used=0; for (int i=0;i<TL_KEYS;i++) if (tlK[i].used) used++;
    if (used>2&&tlK[tlSel].used){ tlK[tlSel].used=false;
      for (int i=0;i<TL_KEYS;i++) if (tlK[i].used){ tlSel=i; break; } } }
  if (Button(174,22,48,18,"CLEAR",C_WARN,false)) TlReset();
  if (Button(224,22,40,18,"OBJ",C_ACCENT,false)) gObj=(gObj+1)%NUM_OBJ;
  if (Button(266,22,50,18,"MODE",C_ACCENT,false)) gMode=(gMode+1)%NUM_MODES;
  { char b[52]; int used=0;
    for (int i=0;i<TL_KEYS;i++) if (tlK[i].used) used++;
    snprintf(b,sizeof(b),"KEY %d/%d   T %d%%   DUR %ds",
             tlSel+1,used,(int)(tlTime*100),(int)tlDur);
    DrawText(8,SCREEN_H-12,b,C_DATA,1); }
  if (Button(228,SCREEN_H-16,40,14,"D-",C_ACCENT,false)) tlDur=clampf(tlDur-1,1,12);
  if (Button(272,SCREEN_H-16,40,14,"D+",C_ACCENT,false)) tlDur=clampf(tlDur+1,1,12);
  DrawParticles();
  TopBar("TIMELINE",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  20. DYNAMIC CLOCKS
// ---------------------------------------------------------------------
static int ckStyle=0;
static const char *CK_NAME[5]={ "ORBITAL","PARTICLE","BINARY","3D ROTOR","HUD" };
void ScreenClocks(float dt){
  if (BackHit()) return;
  AchVisit(ST_CLOCKS);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  int hh=0,mm=0,ss=0;
  if (timeOk){
    time_t now=time(nullptr);
    struct tm ti;
    localtime_r(&now,&ti);
    hh=ti.tm_hour; mm=ti.tm_min; ss=ti.tm_sec;
  } else {
    uint32_t up=millis()/1000;
    hh=(int)((up/3600)%24); mm=(int)((up/60)%60); ss=(int)(up%60); }
  float fs=ss+fmodf(gTime,1.0f);
  Backdrop();
  int cx=160, cy=126;
  switch (ckStyle){
    case 0: {   // orbital -- three rings, one body each
      const int RR[3]={92,68,44};
      float frac[3]={ (hh%12+mm/60.0f)/12.0f, (mm+ss/60.0f)/60.0f, fs/60.0f };
      for (int i=0;i<3;i++){
        RingFB(cx,cy,RR[i],Dim(C_HAIR,3,5),160);
        float a=frac[i]*TAU-1.5708f;
        // trailing arc
        for (int k=0;k<40;k++){
          float u=-1.5708f+(a+1.5708f)*k/40.0f;
          PxBlend(cx+(int)(fcos(u)*RR[i]),cy+(int)(fsin(u)*RR[i]),
                  Spec(i*2),(uint8_t)(60+140.0f*k/40.0f)); }
        int bx=cx+(int)(fcos(a)*RR[i]), by=cy+(int)(fsin(a)*RR[i]);
        CircleFB(bx,by,i==2?2:4,Spec(i*2),255);
        Glow(bx,by,Spec(i*2),120,0.8f); }
      char b[12]; snprintf(b,sizeof(b),"%02d:%02d",hh,mm);
      GlowTextC(cx,cy-8,b,C_TEXT,2,80);
      snprintf(b,sizeof(b),"%02d",ss); DrawTextC(cx,cy+10,b,C_DATA,1);
    } break;
    case 1: {   // particle -- digits made of orbiting dots
      char b[12]; snprintf(b,sizeof(b),"%02d%02d%02d",hh,mm,ss);
      for (int d=0;d<6;d++){
        int dx=cx-132+d*46+(d/2)*8;
        int digit=b[d]-'0';
        for (int p=0;p<26;p++){
          float u=TAU*p/26.0f+gTime*(0.5f+d*0.1f);
          float rr=13.0f+5.0f*fsin(gTime*2.0f+p);
          int px=dx+(int)(fcos(u)*rr);
          int py=cy+(int)(fsin(u)*rr*1.5f);
          uint8_t a=(uint8_t)(70+120*Hash(p*97u+d*31u));
          PxAdd(px,py,Spec(d),a); }
        char one[2]={b[d],0};
        GlowTextC(dx,cy-8,one,(d<2)?C_TEXT:((d<4)?C_ACCENT:C_DATA),2,90);
        if (d==1||d==3) DrawTextC(dx+26,cy-8,":",C_HAIR,2); }
    } break;
    case 2: {   // binary
      const char *lab[3]={"H","M","S"};
      int val[3]={hh,mm,ss};
      for (int r=0;r<3;r++){
        DrawText(58,84+r*34,lab[r],C_SAND,1);
        for (int bit=5;bit>=0;bit--){
          bool on=(val[r]>>bit)&1;
          int bx=80+(5-bit)*32, by=78+r*34;
          if (on){ BlendRectFB(bx,by,24,20,Spec(r*2),A_FILL);
                   Bracket(bx,by,24,20,C_HILITE,5);
                   Glow(bx+12,by+10,Spec(r*2),90,0.9f); }
          else   { BlendRectFB(bx,by,24,20,C_PANEL,A_FILL);
                   Bracket(bx,by,24,20,Dim(C_HAIR,3,5),5); }
          char n[4]; snprintf(n,sizeof(n),"%d",1<<bit);
          DrawTextC(bx+12,by+7,on?"1":"0",on?C_TEXT:C_HAIR,1); } }
      char b[16]; snprintf(b,sizeof(b),"%02d:%02d:%02d",hh,mm,ss);
      DrawTextC(cx,SCREEN_H-26,b,C_DATA,1);
    } break;
    case 3: {   // rotating 3D rotor
      ApplyLight();
      RenderMesh(gMesh[1],0.45f,fs/60.0f*TAU,0,0.85f,(float)cx,(float)cy,3.0f,
                 M_HOLO,C_ACCENT);
      for (int i=0;i<12;i++){
        float a=TAU*i/12.0f-1.5708f;
        int tx=cx+(int)(fcos(a)*104), ty=cy+(int)(fsin(a)*88);
        char n[4]; snprintf(n,sizeof(n),"%d",i==0?12:i);
        bool cur=(i==(hh%12));
        DrawTextC(tx,ty-3,n,cur?C_HILITE:C_HAIR,1);
        if (cur) Glow(tx,ty,C_HILITE,90,0.7f); }
      float am=(mm+ss/60.0f)/60.0f*TAU-1.5708f;
      LineFB(cx,cy,cx+(int)(fcos(am)*80),cy+(int)(fsin(am)*68),C_HILITE,230);
      char b[12]; snprintf(b,sizeof(b),"%02d:%02d:%02d",hh,mm,ss);
      GlowTextC(cx,SCREEN_H-30,b,C_TEXT,2,80);
    } break;
    default: {  // HUD
      Panel(28,54,264,132,"CHRONO",C_ACCENT,"SYS");
      char b[16]; snprintf(b,sizeof(b),"%02d:%02d",hh,mm);
      GlowTextC(cx,88,b,C_TEXT,4,100);
      snprintf(b,sizeof(b),":%02d",ss);
      GlowTextC(cx+2,132,b,C_ACCENT,2,90);
      int bw=(int)(220.0f*fs/60.0f);
      HLineFB(50,158,220,C_HAIR);
      FillRectFB(50,158,bw,3,C_HILITE);
      Glow(50+bw,159,C_HILITE,140,0.8f);
      for (int i=0;i<=12;i++){
        int tx=50+220*i/12;
        VLineFB(tx,162,(i%3)?3:6,Dim(C_HAIR,4,5)); }
      DrawTextC(cx,170,timeOk?"NTP LOCKED":"UPTIME",timeOk?C_DATA:C_WARN,1);
    } break; }

  if (Button(2,22,72,18,"STYLE",C_ACCENT,false)){
    ckStyle=(ckStyle+1)%5; SpawnBurst(38,40,14,C_ACCENT,110.0f,PK_SPARK); }
  DrawText(82,27,CK_NAME[ckStyle],C_SAND,1);
  DrawParticles();
  TopBar("CLOCKS",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  21. GESTURE LAB
// ---------------------------------------------------------------------
static int   gsLast=GS_NONE;
static float gsShowT=99.0f;
static float gsFxT=0;
static int   gsFx=GS_NONE;
static float gsFxX=160, gsFxY=120;
void ScreenGesture(float dt){
  if (BackHit()) return;
  AchVisit(ST_GESTURE);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  gsShowT+=dt;
  if (gsFxT>0) gsFxT=clampf(gsFxT-dt*0.55f,0,1);

  if (touchDown&&touchY>BACK_H+18&&touchY<SCREEN_H-18) GestClear();
  if (touchActive&&touchY>BACK_H+18&&touchY<SCREEN_H-18)
    GestPush((float)touchX,(float)touchY);
  if (touchUp&&gstRN>4){
    int g=GestRecognize();
    if (g!=GS_NONE){
      gsLast=g; gsShowT=0;
      gsFx=g; gsFxT=1.0f; gsFxX=gstRX[gstRN/2]; gsFxY=gstRY[gstRN/2];
      SpawnBurst(gsFxX,gsFxY,20,C_HILITE,150.0f,PK_SPARK);
      // gesture -> real result, as requested
      switch (g){
        case GS_CIRCLE:   gObj=0;  Impact(1.2f); break;   // sphere/planet
        case GS_TRIANGLE: gObj=13; Impact(1.6f); break;   // pyramid
        case GS_SQUARE:   gObj=2;  Impact(1.6f); break;   // cube
        case GS_SPIRAL:   gObj=11; Impact(1.0f); break;   // helix / vortex
        case GS_ZIGZAG:   Impact(4.5f);
                          SpawnBurst(gsFxX,gsFxY,40,Spec(1),300.0f,PK_SPARK); break;
        case GS_ARROW:    gObj=5;  Impact(1.4f); break;   // ship
        default: break; }
      // all seven drawn at least once
      uint16_t all=0;
      for (int i=1;i<GS_COUNT;i++) all|=(1u<<i);
      if ((gstFound&all)==all) AchGrant(A_GEST);
    } else { gsLast=GS_NONE; gsShowT=0; }
    GestClear(); }

  Backdrop();
  // result effect plays out in the world
  if (gsFxT>0.02f){
    float e=gsFxT;
    switch (gsFx){
      case GS_CIRCLE: case GS_TRIANGLE: case GS_SQUARE:
      case GS_SPIRAL: case GS_ARROW: {
        ApplyLight();
        RenderMesh(gMesh[gObj],0.4f,gTime*1.1f,0,
                   clampf((1.0f-e)*1.25f,0.05f,1.25f),
                   gsFxX,gsFxY,3.4f,M_NEON,C_HILITE);
        RingFB((int)gsFxX,(int)gsFxY,(int)(90*(1.0f-e)),C_HILITE,(uint8_t)(160*e));
      } break;
      case GS_ZIGZAG: {
        // energy discharge: branching bolts from the stroke centre
        for (int b=0;b<7;b++){
          float a=TAU*b/7.0f+gTime*0.4f;
          float px=gsFxX, py=gsFxY;
          for (int s=0;s<9;s++){
            float nx=px+fcos(a)*15.0f+(Hash(b*71u+s*13u+(uint32_t)(gTime*30))-0.5f)*17.0f;
            float ny=py+fsin(a)*15.0f+(Hash(b*97u+s*29u+(uint32_t)(gTime*30))-0.5f)*17.0f;
            LineFB((int)px,(int)py,(int)nx,(int)ny,
                   (s&1)?C_HILITE:Spec(1),(uint8_t)(230*e));
            px=nx; py=ny; } }
        Glow((int)gsFxX,(int)gsFxY,C_HILITE,(uint8_t)(200*e),2.4f);
      } break;
      default: break; } }
  // live stroke
  GestTrail(C_ACCENT);

  Panel(6,SCREEN_H-34,148,30,NULL,C_ACCENT,NULL);
  if (gsShowT<2.4f&&gsLast!=GS_NONE){
    float p=clampf(1.0f-gsShowT/2.4f,0,1);
    GlowText(14,SCREEN_H-25,GST_NAME[gsLast],C_HILITE,2,(uint8_t)(110*p));
  } else if (gsShowT<2.4f){
    DrawText(14,SCREEN_H-22,"NOT RECOGNISED",C_WARN,1);
  } else DrawText(14,SCREEN_H-22,"DRAW A SHAPE",C_HAIR,1);

  // legend of what has been found
  Panel(160,SCREEN_H-34,154,30,NULL,C_ACCENT,NULL);
  for (int i=1;i<GS_COUNT;i++){
    int bx=166+(i-1)*21;
    bool have=(gstFound>>i)&1u;
    BlendRectFB(bx,SCREEN_H-28,17,17,have?Dim(C_HILITE,2,5):Dim(C_PANEL,5,5),A_FILL);
    Bracket(bx,SCREEN_H-28,17,17,have?C_HILITE:Dim(C_HAIR,3,5),4);
    char c[2]={GST_NAME[i][0],0};
    DrawTextC(bx+8,SCREEN_H-24,c,have?C_TEXT:C_HAIR,1); }

  DrawTextC(160,26,"CIRCLE  TRIANGLE  SQUARE  LINE  ZIGZAG  SPIRAL  ARROW",C_HAIR,1);
  ImpactFlash();
  DrawParticles();
  TopBar("GESTURE LAB",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  22. CREATOR MODE  --  scene composition + presets
// ---------------------------------------------------------------------
#define CR_ROWS 8
#define CR_PRESETS 4
struct CreatorCfg {
  uint8_t obj, mode, particles, physics, trail, camera, backdrop, effect;
};
static CreatorCfg crCfg = { 1, M_NEON, 1, 1, 1, 1, 1, 0 };
static bool  crRun=false;
static float crRunT=0;
static int   crSel=0;
static float crSpin=0;
static PhysCard crCard;
static bool  crCardInit=false;
static CreatorCfg crSlot[CR_PRESETS];
static bool  crSlotUsed[CR_PRESETS]={false,false,false,false};
static char  crSlotName[CR_PRESETS][14]={"","","",""};
static int   crSaveSel=0;
static bool  crShowPresets=false;

static const char *CR_LBL[CR_ROWS]={
  "OBJECT","RENDER","PARTICLES","PHYSICS","TRAIL","CAMERA","BACKGROUND","EFFECT" };
static const char *CR_PART[3]={"OFF","SPARKS","STORM"};
static const char *CR_PHYS[3]={"OFF","GRAVITY","ORBIT"};
static const char *CR_ONOFF[2]={"OFF","ON"};
static const char *CR_CAM[3]={"STATIC","ORBIT","SWEEP"};
static const char *CR_BG[4]={"VOID","STARFIELD","GRID","NEBULA"};
static const char *CR_FX[4]={"NONE","SCANLINES","BLOOM","GLITCH"};

static void CreatorSave(void){
  prefs.begin(NVS_NS,false);
  prefs.putBytes("crCfg",crSlot,sizeof(crSlot));
  prefs.putBytes("crUse",crSlotUsed,sizeof(crSlotUsed));
  prefs.putBytes("crNam",crSlotName,sizeof(crSlotName));
  prefs.end();
}
void CreatorLoad(void){
  prefs.begin(NVS_NS,true);
  if (prefs.getBytesLength("crCfg")==sizeof(crSlot))
    prefs.getBytes("crCfg",crSlot,sizeof(crSlot));
  if (prefs.getBytesLength("crUse")==sizeof(crSlotUsed))
    prefs.getBytes("crUse",crSlotUsed,sizeof(crSlotUsed));
  if (prefs.getBytesLength("crNam")==sizeof(crSlotName))
    prefs.getBytes("crNam",crSlotName,sizeof(crSlotName));
  prefs.end();
  // sanity-clamp anything that came back out of range
  for (int i=0;i<CR_PRESETS;i++){
    crSlot[i].obj      =(uint8_t)clampi(crSlot[i].obj,0,NUM_OBJ-1);
    crSlot[i].mode     =(uint8_t)clampi(crSlot[i].mode,0,NUM_MODES-1);
    crSlot[i].particles=(uint8_t)clampi(crSlot[i].particles,0,2);
    crSlot[i].physics  =(uint8_t)clampi(crSlot[i].physics,0,2);
    crSlot[i].trail    =(uint8_t)clampi(crSlot[i].trail,0,1);
    crSlot[i].camera   =(uint8_t)clampi(crSlot[i].camera,0,2);
    crSlot[i].backdrop =(uint8_t)clampi(crSlot[i].backdrop,0,3);
    crSlot[i].effect   =(uint8_t)clampi(crSlot[i].effect,0,3);
    crSlotName[i][13]=0; }
}
static uint8_t *CrField(int row){
  switch (row){
    case 0: return &crCfg.obj;
    case 1: return &crCfg.mode;
    case 2: return &crCfg.particles;
    case 3: return &crCfg.physics;
    case 4: return &crCfg.trail;
    case 5: return &crCfg.camera;
    case 6: return &crCfg.backdrop;
    default:return &crCfg.effect; }
}
static int CrRange(int row){
  switch (row){
    case 0: return NUM_OBJ;
    case 1: return NUM_MODES;
    case 2: case 3: case 5: return 3;
    case 4: return 2;
    default:return 4; }
}
static const char *CrValue(int row){
  switch (row){
    case 0: return OBJ_NAME[crCfg.obj%NUM_OBJ];
    case 1: return MODE_NAME[crCfg.mode%NUM_MODES];
    case 2: return CR_PART[crCfg.particles%3];
    case 3: return CR_PHYS[crCfg.physics%3];
    case 4: return CR_ONOFF[crCfg.trail%2];
    case 5: return CR_CAM[crCfg.camera%3];
    case 6: return CR_BG[crCfg.backdrop%4];
    default:return CR_FX[crCfg.effect%4]; }
}
// the running scene
static void CreatorScene(float dt){
  crRunT+=dt;
  crSpin+=dt*(0.4f+(crCfg.camera==2?1.1f:0.0f));
  // background
  if (crCfg.trail && accum){
    uint16_t *ac=(uint16_t*)accum;
    for (int i=0;i<FB_PIXELS;i++){
      uint16_t v=ac[i]; if (!v) continue;
      uint16_t r=((v>>11)&0x1F)*14>>4, g=((v>>5)&0x3F)*14>>4, b=(v&0x1F)*14>>4;
      ac[i]=(r<<11)|(g<<5)|b; } }
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t*)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  switch (crCfg.backdrop){
    case 1:
      for (int i=0;i<110;i++){
        float sx=fmodf(Hash(i*131u)*SCREEN_W+crRunT*(6.0f+Hash(i*77u)*22.0f),(float)SCREEN_W);
        int sy=(int)(Hash(i*911u)*SCREEN_H);
        PxAdd((int)sx,sy,C_TEXT,(uint8_t)(60+170*Hash(i*57u))); }
      break;
    case 2:
      for (int y=20;y<SCREEN_H;y+=16) HLineFB(0,y,SCREEN_W,Dim(C_HAIR,3,5));
      for (int x=0;x<SCREEN_W;x+=16)  VLineFB(x,20,SCREEN_H-20,Dim(C_HAIR,3,5));
      break;
    case 3:
      for (int y=20;y<SCREEN_H;y+=3){
        float n=fsin(y*0.05f+crRunT*0.4f)*fcos(y*0.021f-crRunT*0.25f);
        BlendRectFB(0,y,SCREEN_W,2,Spec((int)(fabsf(n)*5.99f)),
                    (uint8_t)(14+22*fabsf(n))); }
      break;
    default: break; }
  if (crCfg.trail && accum){
    uint16_t *ac=(uint16_t*)accum, *fr=(uint16_t*)frame;
    for (int i=0;i<FB_PIXELS;i++) if (ac[i]){
      uint16_t v=ac[i];
      int r=((fr[i]>>11)&0x1F)+((v>>11)&0x1F); if (r>31)r=31;
      int g=((fr[i]>>5)&0x3F)+((v>>5)&0x3F);   if (g>63)g=63;
      int b=(fr[i]&0x1F)+(v&0x1F);             if (b>31)b=31;
      fr[i]=(r<<11)|(g<<5)|b; } }

  // physics-driven object position
  if (!crCardInit){ CardInit(crCard,160,120); crCardInit=true; }
  if (crCfg.physics==1){
    if (CardGrab(crCard,34,34)) {}
    if (!touchActive) CardRelease(crCard);
    CardUpdate(crCard,dt,34,34,false,520.0f,0.62f);
  } else if (crCfg.physics==2){
    crCard.x=160+fcos(crSpin*0.8f)*72.0f;
    crCard.y=118+fsin(crSpin*1.1f)*44.0f;
  } else { crCard.x=160; crCard.y=118; }

  ApplyLight();
  float camz=(crCfg.camera==0)?3.0f:(3.1f+fsin(crRunT*0.5f)*0.7f);
  float ry=(crCfg.camera==0)?crSpin*0.5f:crSpin;
  uint16_t tint=(crCfg.mode==M_WIRE||crCfg.mode==M_NEON||
                 crCfg.mode==M_POINTS||crCfg.mode==M_HOLO)?C_ACCENT:0;
  RenderMesh(gMesh[crCfg.obj%NUM_OBJ],
             (crCfg.camera==2)?fsin(crRunT*0.4f)*0.6f:0.4f,
             ry,crSpin*0.3f,1.05f,crCard.x,crCard.y,camz,
             crCfg.mode%NUM_MODES,tint);
  // particles
  if (crCfg.particles){
    int n=(crCfg.particles==1)?2:5;
    if (((int)(crRunT*30.0f))%2==0)
      SpawnBurst(crCard.x+(Hash((uint32_t)(crRunT*90))-0.5f)*50.0f,
                 crCard.y+(Hash((uint32_t)(crRunT*90)+7u)-0.5f)*50.0f,
                 n,Spec((int)(crRunT)%6),80.0f,PK_EMBER); }
  // trail accumulation
  if (crCfg.trail && accum){
    uint16_t *ac=(uint16_t*)accum;
    int ix=(int)crCard.x, iy=(int)crCard.y;
    for (int o=-2;o<=2;o++){
      int px=ix+o, py=iy;
      if (px>=0&&px<SCREEN_W&&py>=0&&py<SCREEN_H){
        uint16_t *d=&ac[py*SCREEN_W+px];
        uint16_t c=C_ACCENT;
        int r=((*d>>11)&0x1F)+(((c>>11)&0x1F)>>2); if (r>31)r=31;
        int g=((*d>>5)&0x3F)+(((c>>5)&0x3F)>>2);   if (g>63)g=63;
        int b=(*d&0x1F)+((c&0x1F)>>2);             if (b>31)b=31;
        *d=(r<<11)|(g<<5)|b; } } }
  // post effects
  switch (crCfg.effect){
    case 1:
      for (int y=20;y<SCREEN_H;y+=3) BlendRectFB(0,y,SCREEN_W,1,C_BG,70);
      break;
    case 2:
      Glow((int)crCard.x,(int)crCard.y,C_ACCENT,60,4.0f);
      Glow((int)crCard.x,(int)crCard.y,C_HILITE,34,6.5f);
      break;
    case 3: {
      int n=3+(int)(Hash((uint32_t)(crRunT*7.0f))*4.0f);
      for (int i=0;i<n;i++){
        int gy=20+(int)(Hash(i*331u+(uint32_t)(crRunT*11))*(SCREEN_H-24));
        int gh=1+(int)(Hash(i*77u+(uint32_t)(crRunT*13))*5);
        int sh=(int)((Hash(i*911u+(uint32_t)(crRunT*17))-0.5f)*22.0f);
        for (int y=gy;y<gy+gh&&y<SCREEN_H;y++){
          uint16_t *row=&((uint16_t*)frame)[y*SCREEN_W];
          if (sh>0){ for (int x=SCREEN_W-1;x>=sh;x--) row[x]=row[x-sh]; }
          else if (sh<0){ for (int x=0;x<SCREEN_W+sh;x++) row[x]=row[x-sh]; } } }
    } break;
    default: break; }
  DrawParticles();
}
void ScreenCreator(float dt){
  if (crRun){
    // running: a single small chrome strip, tap it to return to the editor
    CreatorScene(dt);
    BlendRectFB(0,0,SCREEN_W,18,C_PANEL,A_FILL);
    HLineFB(0,18,SCREEN_W,Dim(C_ACCENT,2,5));
    BlendRectFB(2,1,52,15,Dim(C_ACCENT,1,5),A_FILL);
    Bracket(2,1,52,15,C_ACCENT,5);
    DrawText(7,5,"< EDIT",C_TEXT,1);
    GlowText(62,5,"SCENE RUNNING",C_ACCENT,1,70);
    if (gShowFps) DrawText(SCREEN_W-TextW(fpsStr,1)-6,5,fpsStr,C_DATA,1);
    if (touchDown&&touchX<BACK_W&&touchY<BACK_H){
      crRun=false; enterAnim=0;
      if (accum) memset(accum,0,FB_BYTES); }
    AchToast();
    return; }

  if (BackHit()) return;
  AchVisit(ST_CREATOR);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  Backdrop();
  // live preview thumbnail
  Panel(196,22,120,132,"PREVIEW",C_ACCENT,"LIVE");
  crSpin+=dt*0.6f;
  ApplyLight();
  RenderMesh(gMesh[crCfg.obj%NUM_OBJ],0.4f,crSpin,0,0.62f,256.0f,92.0f,3.5f,
             crCfg.mode%NUM_MODES,
             (crCfg.mode==M_WIRE||crCfg.mode==M_NEON||
              crCfg.mode==M_POINTS||crCfg.mode==M_HOLO)?C_ACCENT:0);

  if (!crShowPresets){
    Panel(4,22,188,190,"SCENE",C_ACCENT,"CFG");
    for (int r=0;r<CR_ROWS;r++){
      int ry=38+r*21;
      bool sel=(crSel==r);
      if (sel){ BlendRectFB(8,ry-2,180,19,Dim(C_ACCENT,1,5),A_FILL);
                Bracket(8,ry-2,180,19,C_ACCENT,4); }
      DrawText(14,ry+3,CR_LBL[r],sel?C_TEXT:C_SAND,1);
      DrawText(186-TextW(CrValue(r),1)-4,ry+3,CrValue(r),sel?C_HILITE:C_DATA,1);
      if (touchDown&&touchY>=ry-2&&touchY<ry+17&&touchX<192){
        if (crSel==r){
          uint8_t *f=CrField(r);
          *f=(uint8_t)((*f+1)%CrRange(r));
          SpawnBurst(touchX,touchY,6,C_ACCENT,70.0f,PK_SPARK); }
        crSel=r; } }
    if (Button(200,160,54,18,"PRESETS",C_ACCENT,false)) crShowPresets=true;
    if (Button(258,160,56,18,"RANDOM",C_ACCENT,false)){
      uint32_t h=(uint32_t)(gTime*1000.0f);
      crCfg.obj      =(uint8_t)(Hash(h)*NUM_OBJ);
      crCfg.mode     =(uint8_t)(Hash(h+1u)*NUM_MODES);
      crCfg.particles=(uint8_t)(Hash(h+2u)*3);
      crCfg.physics  =(uint8_t)(Hash(h+3u)*3);
      crCfg.trail    =(uint8_t)(Hash(h+4u)*2);
      crCfg.camera   =(uint8_t)(Hash(h+5u)*3);
      crCfg.backdrop =(uint8_t)(Hash(h+6u)*4);
      crCfg.effect   =(uint8_t)(Hash(h+7u)*4); }
    if (Button(200,182,114,26,"RUN SCENE",C_HILITE,false)){
      crRun=true; crRunT=0; crCardInit=false;
      if (accum) memset(accum,0,FB_BYTES);
      AchGrant(A_CREATE);
      SpawnBurst(256,195,26,C_HILITE,200.0f,PK_SPARK); }
  } else {
    Panel(4,22,188,190,"PRESETS",C_ACCENT,"NVS");
    for (int i=0;i<CR_PRESETS;i++){
      int ry=40+i*30;
      bool sel=(crSaveSel==i);
      if (sel){ BlendRectFB(8,ry-3,180,26,Dim(C_ACCENT,1,5),A_FILL);
                Bracket(8,ry-3,180,26,C_ACCENT,4); }
      char lab[24];
      if (crSlotUsed[i]) snprintf(lab,sizeof(lab),"%d  %s",i+1,
                                  crSlotName[i][0]?crSlotName[i]:"SCENE");
      else               snprintf(lab,sizeof(lab),"%d  -- EMPTY --",i+1);
      DrawText(14,ry+2,lab,crSlotUsed[i]?C_TEXT:C_HAIR,1);
      if (crSlotUsed[i]){
        char sub[30];
        snprintf(sub,sizeof(sub),"%s / %s",
                 OBJ_NAME[crSlot[i].obj%NUM_OBJ],MODE_NAME[crSlot[i].mode%NUM_MODES]);
        DrawText(14,ry+12,sub,C_DATA,1); }
      if (touchDown&&touchY>=ry-3&&touchY<ry+23&&touchX<192) crSaveSel=i; }
    if (Button(200,160,54,18,"BACK",C_ACCENT,false)) crShowPresets=false;
    if (Button(258,160,56,18,"SAVE",C_HILITE,false)){
      crSlot[crSaveSel]=crCfg;
      crSlotUsed[crSaveSel]=true;
      snprintf(crSlotName[crSaveSel],14,"%s",OBJ_NAME[crCfg.obj%NUM_OBJ]);
      CreatorSave();                      // explicit user action only
      AchGrant(A_PRESET);
      SpawnBurst(286,169,16,C_HILITE,120.0f,PK_SPARK); }
    if (Button(200,182,54,26,"LOAD",C_ACCENT,false)){
      if (crSlotUsed[crSaveSel]){ crCfg=crSlot[crSaveSel]; crShowPresets=false; } }
    if (Button(258,182,56,26,"DELETE",C_WARN,false)){
      crSlotUsed[crSaveSel]=false;
      crSlotName[crSaveSel][0]=0;
      CreatorSave(); } }

  DrawParticles();
  TopBar("CREATOR",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  23. UNIVERSAL SEARCH
// ---------------------------------------------------------------------
struct SearchEntry { const char *name; const char *tags; uint8_t target; };
static const SearchEntry SRCH[] = {
  { "3D LAB",         "3d object render mesh lab viewport",      ST_LAB       },
  { "OBJECTS",        "3d object gallery shapes solids",         ST_OBJECTS   },
  { "RENDER MODES",   "mode wireframe shader hologram voxel",    ST_MODES     },
  { "INSPECT",        "3d inspect detail analyse",               ST_INSPECT   },
  { "WARP",           "warp stars space speed tunnel",           ST_WARP      },
  { "MAZE 3D",        "maze raycast game 3d",                    ST_MAZE      },
  { "2048",           "2048 game puzzle numbers tiles",          ST_2048      },
  { "BREAKOUT",       "breakout brick game arcade",              ST_BREAK     },
  { "FLAPPY",         "flappy bird game arcade",                 ST_FLAPPY    },
  { "SNAKE",          "snake game arcade classic",               ST_SNAKE     },
  { "PONG",           "pong game arcade paddle",                 ST_PONG      },
  { "TETRIS",         "tetris blocks game puzzle",               ST_TETRIS    },
  { "MEMORY",         "memory match game cards",                 ST_MEMORY    },
  { "SIMON",          "simon sequence game colors",              ST_SIMON     },
  { "MINESWEEPER",    "mines minesweeper game puzzle",           ST_MINES     },
  { "WHACK",          "whack mole game reaction",                ST_WHACK     },
  { "DODGE",          "dodge game reaction avoid",               ST_DODGE     },
  { "LIGHTS OUT",     "lights out game puzzle",                  ST_LIGHTS    },
  { "DRAW",           "draw paint canvas art brush",             ST_DRAW      },
  { "CLOCK",          "clock time digital numbers",              ST_CLOCK     },
  { "STOPWATCH",      "stopwatch timer lap time",                ST_STOPW     },
  { "TIMER",          "timer countdown alarm",                   ST_TIMER     },
  { "WIFI",           "wifi network scan connect internet",      ST_WIFI      },
  { "SETTINGS",       "settings config theme brightness",        ST_SETTINGS  },
  { "CALIBRATE",      "calibrate touch screen calibration",      ST_CALIB     },
  { "SYSTEM",         "system info heap psram cpu stats",        ST_SYSTEM    },
  { "JEE CENTER",     "jee study exam productivity tasks",       ST_JEE       },
  { "JEE TIMER",      "jee timer study session pomodoro",        ST_JTIMER    },
  { "JEE TASKS",      "jee task todo list homework",             ST_JTASKS    },
  { "JEE GOALS",      "jee goal target milestone",               ST_JGOALS    },
  { "JEE STATS",      "jee stats progress analytics",            ST_JSTATS    },
  { "JEE NOTES",      "jee note write text memo",                ST_JNOTES    },
  { "PHYSICS",        "physics pendulum spring gravity orbit",   ST_PHYS      },
  { "PARTICLES",      "particle sand fluid attract repel",       ST_PSAND     },
  { "SPACE",          "space stars planets galaxy cosmos",       ST_SPACE     },
  { "PLANET GEN",     "planet generator world terrain",          ST_PLANETGEN },
  { "FRACTAL LAB",    "fractal mandelbrot julia zoom",           ST_FRACTAL   },
  { "MATRIX RAIN",    "matrix rain digital green code",          ST_MATRIX    },
  { "FIELD SIM",      "field wave ripple interference",          ST_FIELD     },
  { "TOUCH LAB",      "touch test playground input",             ST_TOUCHPLAY },
  { "LIFE",           "life conway cellular automata",           ST_LIFE      },
  { "MOLECULE",       "molecule dna chemistry atoms",            ST_MOLECULE  },
  { "DEMO",           "demo tour showcase auto",                 ST_DEMO      },
  { "ANIMATION LAB",  "animation stick figure scene story",      ST_ANIMLAB   },
  { "CREATOR",        "creator scene build compose make",        ST_CREATOR   },
  { "GESTURE LAB",    "gesture draw shape circle recognise",     ST_GESTURE   },
  { "MORPH LAB",      "morph transform shape interpolate",       ST_MORPH     },
  { "KALEIDOSCOPE",   "kaleidoscope symmetry mirror pattern",    ST_KALEIDO   },
  { "EXPLODED VIEW",  "explode assemble parts components",       ST_EXPLODE   },
  { "VOXEL MODE",     "voxel cube blocks minecraft style",       ST_VOXEL     },
  { "ESCHER LAB",     "impossible penrose illusion escher",      ST_IMPOSSIBLE},
  { "TUNNEL",         "tunnel infinite fly speed corridor",      ST_TUNNEL    },
  { "GRAVITY WELL",   "gravity well orbit attract slingshot",    ST_GRAVWELL  },
  { "BOIDS",          "boids flock birds swarm separation",      ST_BOIDS     },
  { "AQUARIUM",       "aquarium fish tank water swim",           ST_AQUARIUM  },
  { "ANT COLONY",     "ant colony pheromone food swarm",         ST_ANTS      },
  { "CHARGE LAB",     "charge magnet electric field positive",   ST_CHARGES   },
  { "RAGDOLL LAB",    "ragdoll physics doll throw limbs",        ST_RAGDOLL   },
  { "TIMELINE",       "timeline keyframe animate editor",        ST_TIMELINE  },
  { "GRAPHER",        "graph equation parabola sin function",    ST_GRAPHER   },
  { "PARAMETRIC",     "parametric curve spiral lissajous rose",  ST_PARAMETRIC},
  { "3D SURFACE",     "surface 3d function mesh plot",           ST_SURFACE   },
  { "VECTORS",        "vector dot cross magnitude angle",        ST_VECTORS   },
  { "MATRIX LAB",     "matrix transform rotate shear determinant",ST_MATRIXVIZ},
  { "FOURIER",        "fourier harmonic epicycle wave transform",ST_FOURIER   },
  { "CLOCKS",         "clock orbital binary particle style",     ST_CLOCKS    },
  { "AWARDS",         "achievement award trophy progress",       ST_ACHIEVE   }
};
#define SRCH_N ((int)(sizeof(SRCH)/sizeof(SRCH[0])))
static char  srQuery[24]="";
static int   srHits[10];
static int   srHitN=0;
static int   srRecent[4]={-1,-1,-1,-1};
static uint32_t srFav=0;              // not persisted per-entry beyond 32
// case-insensitive substring
static bool SrContains(const char *hay,const char *needle){
  if (!*needle) return true;
  for (const char *h=hay;*h;h++){
    const char *a=h, *b=needle;
    while (*a&&*b){
      char ca=*a, cb=*b;
      if (ca>='A'&&ca<='Z') ca=(char)(ca+32);
      if (cb>='A'&&cb<='Z') cb=(char)(cb+32);
      if (ca!=cb) break;
      a++; b++; }
    if (!*b) return true; }
  return false;
}
static void SearchRun(void){
  srHitN=0;
  // pass 1: name matches rank first
  for (int i=0;i<SRCH_N&&srHitN<10;i++)
    if (SrContains(SRCH[i].name,srQuery)) srHits[srHitN++]=i;
  // pass 2: tag matches
  for (int i=0;i<SRCH_N&&srHitN<10;i++){
    bool dup=false;
    for (int k=0;k<srHitN;k++) if (srHits[k]==i) dup=true;
    if (!dup&&SrContains(SRCH[i].tags,srQuery)) srHits[srHitN++]=i; }
}
// keyboard thunks -- declared up top so KbCommit can reach them
void KbCommitEq(const char *t){
  if (kbGraphSlot>=0&&kbGraphSlot<EQ_SLOTS){
    snprintf(eqSrc[kbGraphSlot],EQ_LEN,"%s",t?t:"");
    eqOn[kbGraphSlot]=(t&&t[0]); }
}
void KbCommitSearch(const char *t){
  snprintf(srQuery,sizeof(srQuery),"%s",t?t:"");
  SearchRun();
}
void SearchRun2(void){ SearchRun(); }
void TimelineReset(void){ if (!tlInit) TlReset();
  rdGrabDoll=-1; rdGrabPt=-1; exHeld=-1; crRun=false; }
static void SearchPush(int idx){
  for (int i=0;i<4;i++) if (srRecent[i]==idx) return;
  for (int i=3;i>0;i--) srRecent[i]=srRecent[i-1];
  srRecent[0]=idx;
}
void ScreenSearch(float dt){
  if (BackHit()) return;
  AchVisit(ST_SEARCH);
  enterAnim=clampf(enterAnim+dt*2.4f,0,1);
  if (srHitN==0&&!srQuery[0]) SearchRun();
  Backdrop();
  GlowTextC(160,30,"WHAT DO YOU WANT?",C_ACCENT,1,80);
  // query box
  BlendRectFB(16,42,256,24,C_PANEL,A_FILL);
  Bracket(16,42,256,24,C_HILITE,6);
  DrawText(24,50,srQuery[0]?srQuery:"TAP TO TYPE",srQuery[0]?C_TEXT:C_HAIR,1);
  if (((int)(gTime*2.2f))&1)
    VLineFB(26+TextW(srQuery,1),47,13,C_HILITE);
  if (touchDown&&touchY>=42&&touchY<66&&touchX>=16&&touchX<272){
    KbOpen("SEARCH",srQuery,20,ST_SEARCH,KBP_SEARCH); return; }
  if (Button(276,42,40,24,"CLR",C_WARN,false)){ srQuery[0]=0; SearchRun(); }

  // results
  int ry=74;
  if (srHitN==0){
    DrawTextC(160,110,"NO RESULTS",C_WARN,1);
    DrawTextC(160,126,"TRY: fractal  planet  snake  parabola",C_HAIR,1);
  } else {
    for (int i=0;i<srHitN&&ry<SCREEN_H-34;i++){
      const SearchEntry &e=SRCH[srHits[i]];
      bool hov=touchActive&&touchY>=ry&&touchY<ry+15;
      if (hov) BlendRectFB(14,ry-1,292,16,Dim(C_ACCENT,1,5),A_FILL);
      Bracket(14,ry-1,292,16,hov?C_ACCENT:Dim(C_HAIR,3,5),4);
      DrawText(22,ry+3,e.name,hov?C_TEXT:C_SAND,1);
      // show the tag that matched, as a hint
      DrawText(300-TextW("GO",1)-4,ry+3,"GO",hov?C_HILITE:C_HAIR,1);
      if ((srFav>>(srHits[i]&31))&1u) DrawText(300-40,ry+3,"*",C_HILITE,1);
      if (touchDown&&touchY>=ry&&touchY<ry+15&&transT==0){
        SearchPush(srHits[i]);
        GoTo(e.target,160,ry+7,C_ACCENT,TR_IRIS);
        return; }
      ry+=18; } }

  // recents
  if (srRecent[0]>=0&&ry<SCREEN_H-22){
    DrawText(16,SCREEN_H-30,"RECENT",C_HAIR,1);
    int rx=64;
    for (int i=0;i<4;i++){
      if (srRecent[i]<0) continue;
      const char *n=SRCH[srRecent[i]].name;
      int w=TextW(n,1)+8;
      if (rx+w>SCREEN_W-6) break;
      BlendRectFB(rx,SCREEN_H-33,w,14,Dim(C_PANEL,5,5),A_FILL);
      Bracket(rx,SCREEN_H-33,w,14,Dim(C_ACCENT,3,5),3);
      DrawText(rx+4,SCREEN_H-30,n,C_DATA,1);
      if (touchDown&&touchY>=SCREEN_H-33&&touchY<SCREEN_H-19&&
          touchX>=rx&&touchX<rx+w&&transT==0){
        GoTo(SRCH[srRecent[i]].target,rx+w/2,SCREEN_H-26,C_ACCENT,TR_IRIS);
        return; }
      rx+=w+4; } }
  { char b[30]; snprintf(b,sizeof(b),"%d OF %d",srHitN,SRCH_N);
    DrawText(SCREEN_W-TextW(b,1)-8,SCREEN_H-13,b,C_DATA,1); }
  DrawParticles();
  TopBar("SEARCH",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  24. ACHIEVEMENTS SCREEN
// ---------------------------------------------------------------------
static float acScroll=0;
void ScreenAchieve(float dt){
  if (BackHit()) return;
  AchVisit(ST_ACHIEVE);
  enterAnim=clampf(enterAnim+dt*2.0f,0,1);
  Backdrop();
  int have=AchCount();
  Panel(4,22,312,190,"AWARDS",C_ACCENT,"LOG");
  { char b[30]; snprintf(b,sizeof(b),"%d / %d UNLOCKED",have,ACH_N);
    GlowText(12,32,b,C_HILITE,1,70);
    int bw=(int)(140.0f*have/ACH_N);
    HLineFB(168,36,140,C_HAIR);
    FillRectFB(168,36,bw,3,C_HILITE);
    Glow(168+bw,37,C_HILITE,120,0.7f); }
  const int ROWH=14, VIEW=160, TOPY=46;
  float contentH=ACH_N*ROWH;
  if (touchActive&&touchY>TOPY&&touchY<TOPY+VIEW)
    acScroll=clampf(acScroll-(touchY-lastTY),0,fmaxf(0,contentH-VIEW));
  for (int i=0;i<ACH_N;i++){
    int ry=TOPY+i*ROWH-(int)acScroll;
    if (ry<TOPY-ROWH||ry>TOPY+VIEW) continue;
    bool got=AchHas(i);
    uint16_t c=got?C_HILITE:Dim(C_HAIR,3,5);
    // medal
    if (got){ HexFB(20,ry+6,5,Dim(C_HILITE,2,5),A_FILL,true);
              HexFB(20,ry+6,5,C_HILITE,255,false);
              Glow(20,ry+6,C_HILITE,60,0.5f); }
    else      HexFB(20,ry+6,5,Dim(C_HAIR,3,5),180,false);
    DrawText(34,ry+2,got?ACH[i].name:"?????????",got?C_TEXT:C_HAIR,1);
    DrawText(180,ry+2,ACH[i].hint,got?C_DATA:Dim(C_HAIR,4,5),1); }
  if (contentH>VIEW){
    int bh=(int)(VIEW*VIEW/contentH);
    int by=TOPY+(int)((VIEW-bh)*acScroll/(contentH-VIEW));
    VLineFB(310,TOPY,VIEW,Dim(C_HAIR,3,5));
    FillRectFB(309,by,3,bh,C_ACCENT); }
  DrawParticles();
  TopBar("AWARDS",C_ACCENT);
  EnterOverlay();
  AchToast();
}

// ---------------------------------------------------------------------
//  25. SECRET DEVELOPER ROOM
//  Reached only from the hidden EGG_DEVMODE sequence -- no hub tile.
// ---------------------------------------------------------------------
static float dvTouchLat=0;
static uint32_t dvLastTouchMs=0;
static uint8_t dvHist[64];
static int dvHead=0;
static float dvAcc=0;
void ScreenDevRoom(float dt){
  if (BackHit()) return;
  enterAnim=clampf(enterAnim+dt*3.0f,0,1);
  dvAcc+=dt;
  if (dvAcc>0.08f){
    dvAcc=0;
    dvHist[dvHead]=(uint8_t)clampi((int)(dt*1000.0f*4.0f),0,255);
    dvHead=(dvHead+1)&63; }
  if (touchDown){
    uint32_t now=millis();
    dvTouchLat=(float)(now-dvLastTouchMs);
    dvLastTouchMs=now; }

  // a deliberately raw, unstyled backdrop -- this room is "backstage"
  uint16_t bg=Dim(C_BG,3,5);
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t*)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  for (int y=20;y<SCREEN_H;y+=4) BlendRectFB(0,y,SCREEN_W,1,Dim(C_DATA,1,7),40);

  int tri=0;
  for (int i=0;i<NUM_OBJ;i++) tri+=gMesh[i].nt;
  int liveP=0;
  for (int i=0;i<NUM_PART;i++) if (parts[i].life>0) liveP++;

  Panel(4,22,152,190,"RUNTIME",C_DATA,"DBG");
  int ry=38; char b[46];
  #define DROW(k,...) do{ snprintf(b,sizeof(b),__VA_ARGS__); \
                          DrawText(10,ry,k,C_SAND,1); \
                          DrawText(152-TextW(b,1)-4,ry,b,C_DATA,1); ry+=13; }while(0)
  DROW("FPS",       "%s",fpsStr);
  DROW("FRAME MS",  "%d.%02d",(int)(dt*1000),abs((int)(dt*100000))%100);
  DROW("HEAP",      "%u",(unsigned)ESP.getFreeHeap());
  DROW("MIN HEAP",  "%u",(unsigned)ESP.getMinFreeHeap());
  DROW("PSRAM",     "%u",(unsigned)ESP.getFreePsram());
  DROW("PSRAM TOT", "%u",(unsigned)ESP.getPsramSize());
  DROW("CPU MHZ",   "%u",(unsigned)ESP.getCpuFreqMHz());
  DROW("UPTIME S",  "%lu",(unsigned long)(millis()/1000));
  DROW("TOUCH LAT", "%d MS",(int)dvTouchLat);
  DROW("STATE",     "%d",(int)appState);
  DROW("TRIS POOL", "%d",tri);
  DROW("PARTICLES", "%d/%d",liveP,NUM_PART);
  #undef DROW

  Panel(160,22,156,96,"FRAME TIME",C_DATA,"MS");
  { // scope trace
    int px=0,py=0;
    for (int i=0;i<64;i++){
      int idx=(dvHead+i)&63;
      int sx=164+i*145/64;
      int sy=110-(dvHist[idx]*76/255);
      if (i) LineFB(px,py,sx,sy,C_HILITE,220);
      px=sx; py=sy; }
    HLineFB(164,110-(16*4*76/255),145,Dim(C_WARN,3,5));   // 16 ms marker
    DrawText(166,34,"16MS",Dim(C_WARN,4,5),1); }

  Panel(160,122,156,90,"POOLS",C_DATA,"MEM");
  ry=138;
  struct { const char *n; int used, cap; } POOL[4]={
    { "VERTS", 0, MAX_VERTS }, { "TRIS", 0, MAX_TRIS },
    { "PARTS", liveP, NUM_PART }, { "AGENTS", simN, SIMP_MAX } };
  { int vv=0,tt=0;
    for (int i=0;i<NUM_OBJ;i++){ vv+=gMesh[i].nv; tt+=gMesh[i].nt; }
    POOL[0].used=vv; POOL[1].used=tt; }
  for (int i=0;i<4;i++){
    DrawText(166,ry,POOL[i].n,C_SAND,1);
    int bw=(POOL[i].cap>0)?(80*POOL[i].used/POOL[i].cap):0;
    bw=clampi(bw,0,80);
    HLineFB(220,ry+3,80,Dim(C_HAIR,3,5));
    FillRectFB(220,ry+3,bw,3,(bw>68)?C_WARN:C_HILITE);
    snprintf(b,sizeof(b),"%d",POOL[i].used);
    DrawText(304-TextW(b,1),ry,b,C_DATA,1);
    ry+=16; }

  DrawText(6,SCREEN_H-11,"NEXUS DEV ROOM  --  BUILD " NEXUS_VER_STR,Dim(C_DATA,3,5),1);
  TopBar("DEV",C_DATA);
  AchToast();
}

// ---------------------------------------------------------------------
//  26. SCREENSAVER
// ---------------------------------------------------------------------
static int   ssMode=0;
static float ssT=0;
static int   ssPrevState=ST_HOME;
uint32_t ssIdleMs=0;
#define SS_IDLE_MS 90000UL
void SaverEnter(void){
  if (appState==ST_SAVER) return;
  // never hijack an active game, the keyboard, calibration or a transition
  if (appState==ST_KBD||appState==ST_CALIB||appState==ST_DEMO||transT>0) return;
  ssPrevState=appState;
  ssMode=(int)(Hash(millis())*8.0f)%8;
  ssT=0;
  appState=ST_SAVER;
  enterAnim=0;
}
void ScreenSaver(float dt){
  ssT+=dt;
  // ANY touch leaves immediately, back to where the user was
  if (touchDown||touchActive){
    appState=(AppState)ssPrevState;
    enterAnim=0;
    lastTouchMs=millis();
    return; }
  switch (ssMode){
    case 0: {   // rotating planet
      Backdrop(); ApplyLight();
      RenderMesh(gMesh[0],0.4f,ssT*0.35f,0,1.25f,160.0f,120.0f,2.9f,M_SMOOTH,0);
    } break;
    case 1: {   // particle bloom
      Backdrop();
      for (int i=0;i<180;i++){
        float u=TAU*i/180.0f+ssT*0.3f;
        float r=40.0f+50.0f*fsin(ssT*0.6f+i*0.09f);
        PxAdd(160+(int)(fcos(u)*r),120+(int)(fsin(u)*r*0.7f),
              Spec(i%6),(uint8_t)(90+120*fabsf(fsin(ssT+i*0.2f)))); }
    } break;
    case 2: {   // wireframe tunnel
      uint16_t bg=C_BG; uint32_t two=((uint32_t)bg<<16)|bg;
      uint32_t *q=(uint32_t*)frame;
      for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
      for (int r=20;r>=0;r--){
        float z=fmodf(ssT*3.0f,1.1f)+r*1.1f;
        float sc=230.0f/z;
        HexFB(160,120,(int)sc,Spec(r%6),
              (uint8_t)clampf(220.0f/z,20,230),false); }
    } break;
    case 3: {   // life
      Backdrop();
      if (lifeA){
        static float acc=0; acc+=dt;
        if (acc>0.12f){ acc=0; LifeStep(); }
        for (int y=0;y<LF_H;y++) for (int x=0;x<LF_W;x++)
          if (lifeA[y*LF_W+x]) FillRectFB(32+x*4,42+y*4,3,3,Spec((x+y)%6&3)); }
    } break;
    case 4: {   // digital rain
      uint16_t bg=C_BG; uint32_t two=((uint32_t)bg<<16)|bg;
      uint32_t *q=(uint32_t*)frame;
      for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
      for (int c=0;c<40;c++){
        float sp=40.0f+Hash(c*331u)*90.0f;
        float head=fmodf(ssT*sp+Hash(c*77u)*400.0f,(float)(SCREEN_H+90))-40.0f;
        for (int k=0;k<12;k++){
          int y=(int)head-k*9;
          if (y<0||y>=SCREEN_H) continue;
          char ch=(char)('0'+(int)(Hash(c*911u+k*13u+(uint32_t)(ssT*3))*10));
          char s2[2]={ch,0};
          DrawText(c*8+2,y,s2,(k==0)?C_HILITE:Dim(C_ACCENT,4-k/4,5),1); } }
    } break;
    case 5: {   // aurora
      for (int y=0;y<SCREEN_H;y++){
        float n=fsin(y*0.035f+ssT*0.6f)*fcos(y*0.017f-ssT*0.4f);
        FillRectFB(0,y,SCREEN_W,1,Dim(Spec((int)(fabsf(n)*5.99f)),
                                      (uint8_t)(1+3*fabsf(n)),7)); }
      for (int i=0;i<80;i++)
        PxAdd((int)(Hash(i*131u)*SCREEN_W),
              (int)(fmodf(Hash(i*911u)*SCREEN_H+ssT*10.0f,(float)SCREEN_H)),
              C_TEXT,(uint8_t)(60+150*Hash(i*57u)));
    } break;
    case 6: {   // procedural geometry parade
      Backdrop(); ApplyLight();
      int o=((int)(ssT/4.0f))%NUM_OBJ;
      RenderMesh(gMesh[o],fsin(ssT*0.3f)*0.5f,ssT*0.5f,ssT*0.2f,1.1f,
                 160.0f,120.0f,3.0f,
                 (OBJ_STYLE[o]>=0)?(uint8_t)OBJ_STYLE[o]:M_SMOOTH,0);
      DrawTextC(160,SCREEN_H-16,OBJ_NAME[o],Dim(C_SAND,3,5),1);
    } break;
    default: {  // orbiting physics bodies
      Backdrop();
      for (int i=0;i<7;i++){
        float u=ssT*(0.3f+i*0.11f)+i;
        float r=26.0f+i*13.0f;
        int px=160+(int)(fcos(u)*r), py=120+(int)(fsin(u)*r*0.6f);
        CircleFB(px,py,2+i/2,Spec(i),230);
        Glow(px,py,Spec(i),70,0.7f);
        RingFB(160,120,(int)r,Dim(C_HAIR,3,5),70); }
      Glow(160,120,C_HILITE,110,1.6f);
    } break; }
  // a whisper-quiet clock, bottom right
  { int hh=0,mm=0;
    if (timeOk){ time_t now=time(nullptr); struct tm ti; localtime_r(&now,&ti);
                 hh=ti.tm_hour; mm=ti.tm_min; }
    else { uint32_t up=millis()/1000; hh=(int)((up/3600)%24); mm=(int)((up/60)%60); }
    char b[10]; snprintf(b,sizeof(b),"%02d:%02d",hh,mm);
    DrawText(SCREEN_W-TextW(b,1)-8,SCREEN_H-12,b,Dim(C_DATA,2,5),1); }
  DrawParticles();
}


// =====================================================================
//  V8 NAVIGATION MODEL
//  Home shows 6 categories. Tapping one opens a list of its entries.
//  Every V7 feature appears in exactly one category, so nothing is
//  orphaned, and Home never has to grow when features are added.
// =====================================================================
enum { CAT_STUDY=0, CAT_LAB, CAT_CREATE, CAT_PLAY, CAT_SYSTEM, CAT_N };

struct NavEntry { const char *name; const char *sub; uint8_t target;
                  uint8_t icon; uint8_t cat; };
static const NavEntry NAV[] = {
  // ---- STUDY ----
  { "COMMAND CENTER","Overview and today",   ST_JEE,      IC_SIGMA,     CAT_STUDY },
  { "FOCUS TIMER",   "Timed study sessions", ST_JTIMER,   IC_TIMER,     CAT_STUDY },
  { "TASKS",         "Daily task list",      ST_JTASKS,   IC_CHECK,     CAT_STUDY },
  { "GOALS",         "Long term targets",    ST_JGOALS,   IC_FLAG,      CAT_STUDY },
  { "STATISTICS",    "Progress and streak",  ST_JSTATS,   IC_CHART,     CAT_STUDY },
  { "HISTORY",       "Past sessions",        ST_JHIST,    IC_HISTORY,   CAT_STUDY },
  { "NOTES",         "Written notes",        ST_JNOTES,   IC_NOTE,      CAT_STUDY },
  { "QUOTES",        "Daily motivation",     ST_JQUOTE,   IC_QUOTE,     CAT_STUDY },
  { "GRAPHER",       "Plot equations",       ST_GRAPHER,  IC_FUNCTION,  CAT_STUDY },
  { "STUDY SETUP",   "Tracker preferences",  ST_JSET,     IC_SLIDERS,   CAT_STUDY },
  // ---- LAB ----
  { "3D LAB",        "Viewport and shading", ST_LAB,       IC_CUBE,      CAT_LAB },
  { "OBJECTS",       "Solid library",        ST_OBJECTS,   IC_GRID,      CAT_LAB },
  { "RENDER MODES",  "13 shading modes",     ST_MODES,     IC_LAYERS,    CAT_LAB },
  { "INSPECT",       "Examine geometry",     ST_INSPECT,   IC_TARGET,    CAT_LAB },
  { "MORPH",         "Shape interpolation",  ST_MORPH,     IC_MORPH,     CAT_LAB },
  { "KALEIDOSCOPE",  "Radial symmetry",      ST_KALEIDO,   IC_KALEIDO,   CAT_LAB },
  { "EXPLODED VIEW", "Component assembly",   ST_EXPLODE,   IC_EXPLODE,   CAT_LAB },
  { "VOXEL",         "Volumetric render",    ST_VOXEL,     IC_VOXEL,     CAT_LAB },
  { "ESCHER",        "Impossible geometry",  ST_IMPOSSIBLE,IC_IMPOSSIBLE,CAT_LAB },
  { "TUNNEL",        "Infinite corridor",    ST_TUNNEL,    IC_TUNNEL,    CAT_LAB },
  { "WARP",          "Faster than light",    ST_WARP,      IC_WARP,      CAT_LAB },
  { "PHYSICS",       "8 simulations",        ST_PHYS,      IC_ATOM,      CAT_LAB },
  { "PARTICLES",     "Interactive field",    ST_PSAND,     IC_PARTICLE,  CAT_LAB },
  { "GRAVITY WELL",  "Orbital mechanics",    ST_GRAVWELL,  IC_WELL,      CAT_LAB },
  { "CHARGE LAB",    "Electric fields",      ST_CHARGES,   IC_MAGNET,    CAT_LAB },
  { "FIELD SIM",     "Wave propagation",     ST_FIELD,     IC_WAVE,      CAT_LAB },
  { "FRACTALS",      "Mandelbrot and Julia", ST_FRACTAL,   IC_FRACTAL,   CAT_LAB },
  { "SPACE",         "Deep field",           ST_SPACE,     IC_STAR,      CAT_LAB },
  { "PLANET GEN",    "Procedural worlds",    ST_PLANETGEN, IC_PLANET,    CAT_LAB },
  { "MOLECULE",      "Chemistry viewer",     ST_MOLECULE,  IC_MOLECULE,  CAT_LAB },
  { "LIFE",          "Cellular automata",    ST_LIFE,      IC_LIFE,      CAT_LAB },
  { "BOIDS",         "Flocking behaviour",   ST_BOIDS,     IC_FLOCK,     CAT_LAB },
  { "AQUARIUM",      "Artificial life",      ST_AQUARIUM,  IC_FISH,      CAT_LAB },
  { "ANT COLONY",    "Emergent pathfinding", ST_ANTS,      IC_ANT,       CAT_LAB },
  { "MATRIX RAIN",   "Digital rain",         ST_MATRIX,    IC_RAIN,      CAT_LAB },
  { "SURFACE",       "3D function plot",     ST_SURFACE,   IC_SURFACE,   CAT_LAB },
  { "PARAMETRIC",    "Curve studio",         ST_PARAMETRIC,IC_CURVE,     CAT_LAB },
  { "VECTORS",       "Vector algebra",       ST_VECTORS,   IC_VECTOR,    CAT_LAB },
  { "MATRIX LAB",    "Linear transforms",    ST_MATRIXVIZ, IC_MATRIX,    CAT_LAB },
  { "FOURIER",       "Harmonic analysis",    ST_FOURIER,   IC_FOURIER,   CAT_LAB },
  { "TOUCH LAB",     "Input diagnostics",    ST_TOUCHPLAY, IC_TOUCH,     CAT_LAB },
  // ---- CREATE ----
  { "CREATOR",       "Compose a scene",      ST_CREATOR,   IC_COMPOSE,   CAT_CREATE },
  { "ANIMATION LAB", "Scripted scenes",      ST_ANIMLAB,   IC_FIGURE,    CAT_CREATE },
  { "TIMELINE",      "Keyframe editor",      ST_TIMELINE,  IC_TIMELINE,  CAT_CREATE },
  { "RAGDOLL",       "Physics characters",   ST_RAGDOLL,   IC_FIGURE,    CAT_CREATE },
  { "GESTURE LAB",   "Draw to create",       ST_GESTURE,   IC_GESTURE,   CAT_CREATE },
  { "DRAW",          "Freehand canvas",      ST_DRAW,      IC_BRUSH,     CAT_CREATE },
  { "DEMO REEL",     "Automatic tour",       ST_DEMO,      IC_PLAYBTN,   CAT_CREATE },
  // ---- PLAY ----
  { "MAZE 3D",       "First person maze",    ST_MAZE,   IC_MAZE,    CAT_PLAY },
  { "2048",          "Sliding puzzle",       ST_2048,   IC_BLOCKS,  CAT_PLAY },
  { "BREAKOUT",      "Brick breaker",        ST_BREAK,  IC_BRICK,   CAT_PLAY },
  { "FLAPPY",        "Endless flyer",        ST_FLAPPY, IC_BIRD,    CAT_PLAY },
  { "SNAKE",         "Grow and survive",     ST_SNAKE,  IC_SNAKE,   CAT_PLAY },
  { "PONG",          "Two paddle classic",   ST_PONG,   IC_PADDLE,  CAT_PLAY },
  { "TETRIS",        "Falling blocks",       ST_TETRIS, IC_BLOCKS,  CAT_PLAY },
  { "MEMORY",        "Match the pairs",      ST_MEMORY, IC_CARDS,   CAT_PLAY },
  { "SIMON",         "Repeat the sequence",  ST_SIMON,  IC_BULB,    CAT_PLAY },
  { "MINESWEEPER",   "Logic and luck",       ST_MINES,  IC_MINE,    CAT_PLAY },
  { "WHACK",         "Reaction test",        ST_WHACK,  IC_HAMMER,  CAT_PLAY },
  { "DODGE",         "Survive the field",    ST_DODGE,  IC_SHIELD,  CAT_PLAY },
  { "LIGHTS OUT",    "Switch puzzle",        ST_LIGHTS, IC_BULB,    CAT_PLAY },
  // ---- SYSTEM ----
  { "CLOCK",         "Time and date",        ST_CLOCK,    IC_CLOCK,      CAT_SYSTEM },
  { "CLOCK STYLES",  "5 clock faces",        ST_CLOCKS,   IC_TIMER,      CAT_SYSTEM },
  { "STOPWATCH",     "Elapsed time",         ST_STOPW,    IC_STOPWATCH,  CAT_SYSTEM },
  { "TIMER",         "Countdown",            ST_TIMER,    IC_TIMER,      CAT_SYSTEM },
  { "AWARDS",        "Achievements",         ST_ACHIEVE,  IC_TROPHY,     CAT_SYSTEM },
  { "NETWORK",       "WiFi and status",      ST_WIFI,     IC_WIFI,       CAT_SYSTEM },
  { "DISPLAY",       "Theme and brightness", ST_SETTINGS, IC_DISPLAY,    CAT_SYSTEM },
  { "TOUCH",         "Calibration",          ST_CALIB,    IC_CROSSHAIR,  CAT_SYSTEM },
  { "ABOUT",         "Hardware and build",   ST_SYSTEM,   IC_INFO,       CAT_SYSTEM }
};
#define NAV_N ((int)(sizeof(NAV)/sizeof(NAV[0])))

struct CatDef { const char *name; const char *tagline; uint8_t icon; };
static const CatDef CATS[CAT_N] = {
  { "STUDY",  "Tracker, focus, analysis", IC_STUDY  },
  { "LAB",    "3D, physics, mathematics", IC_LAB    },
  { "CREATE", "Compose, animate, draw",   IC_CREATE },
  { "PLAY",   "Games and diversions",     IC_PLAY   },
  { "SYSTEM", "Clock, network, settings", IC_SYSTEM }
};
static inline int CatCount(int cat){
  int n=0; for (int i=0;i<NAV_N;i++) if (NAV[i].cat==cat) n++; return n; }

// transition style per category, so navigation has a sense of place
static inline int CatTransition(int cat){
  switch (cat){
    case CAT_STUDY:  return TR_IRIS;
    case CAT_LAB:    return TR_HEX;
    case CAT_CREATE: return TR_SHOCK;
    case CAT_PLAY:   return TR_GLITCH;
    default:         return TR_IRIS; }
}

// =====================================================================
//  HOME  --  command center. Greeting, category rows, live status.
// =====================================================================
static Spring homeS[CAT_N];
static float  homeEnter=0;
static int    catOpen=-1;         // -1 = home, else the open category
static float  catScroll=0, catScrollV=0;
static bool   catDrag=false;
static int    catDragY=0;
static float  catEnter=0;

static const char *Greeting(int hh){
  if (hh<5)  return "Still up.";
  if (hh<12) return "Good morning.";
  if (hh<17) return "Good afternoon.";
  if (hh<22) return "Good evening.";
  return "Working late.";
}
void ScreenHomeV8(float dt){
  homeEnter=clampf(homeEnter+dt*2.6f,0,1);
  enterAnim=homeEnter;
  int hh=0,mm=0;
  if (timeOk){ time_t now=time(nullptr); struct tm ti; localtime_r(&now,&ti);
               hh=ti.tm_hour; mm=ti.tm_min; }
  else { uint32_t up=millis()/1000; hh=(int)((up/3600)%24); mm=(int)((up/60)%60); }

  // --- flat background. No horizon, no grid, no dust. ---
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t*)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;

  if (catOpen<0){
    // ---------------- HOME ----------------
    // masthead
    char tb[10]; snprintf(tb,sizeof(tb),"%02d:%02d",hh,mm);
    DrawText(GUTTER,10,"NEXUS",C_TEXT,T_TITLE);
    DrawText(SCREEN_W-GUTTER-TextW(tb,T_BODY),13,tb,C_DATA,T_BODY);
    UiDivider(GUTTER,30,CONTENT_W);
    DrawText(GUTTER,38,Greeting(hh),C_DIM,T_BODY);

    // category rows
    const int RH=30, TOP=54;
    int hit=-1;
    for (int i=0;i<CAT_N;i++){
      int ry=TOP+i*(RH+CARD_GAP);
      if (touchX>=GUTTER&&touchX<GUTTER+CONTENT_W&&touchY>=ry&&touchY<ry+RH) hit=i; }
    for (int i=0;i<CAT_N;i++)
      SpringTo(homeS[i],(touchActive&&hit==i)?1.0f:0.0f,420.0f,26.0f,dt);
    for (int i=0;i<CAT_N;i++){
      float d=clampf((homeEnter-i*0.06f)/0.5f,0,1);
      if (d<=0.001f) continue;
      float vis=EaseOutCubic(d);
      int ry=TOP+i*(RH+CARD_GAP)+(int)((1.0f-vis)*8.0f);
      float press=clampf(homeS[i].v,0,1);
      int st=(press>0.05f)?UST_PRESSED:UST_NORMAL;
      UiCard(GUTTER,ry,CONTENT_W,RH,st,C_ACCENT);
      IconV8(CATS[i].icon,GUTTER+20,ry+RH/2,(press>0.3f)?C_ACCENT:C_TEXT,8);
      DrawText(GUTTER+38,ry+7,CATS[i].name,C_TEXT,T_BODY);
      DrawText(GUTTER+38,ry+18,CATS[i].tagline,C_DIM,T_SMALL);
      char cb[6]; snprintf(cb,sizeof(cb),"%d",CatCount(i));
      DrawText(GUTTER+CONTENT_W-CARD_PAD-8-TextW(cb,T_SMALL),ry+RH/2-3,cb,C_DIM,T_SMALL);
      // chevron
      int chx=GUTTER+CONTENT_W-CARD_PAD, chy=ry+RH/2;
      for (int k=0;k<4;k++){
        PxBlend(chx-3+k,chy-k,(press>0.3f)?C_ACCENT:C_OFF,255);
        PxBlend(chx-3+k,chy+k,(press>0.3f)?C_ACCENT:C_OFF,255); } }
    if (touchUp&&hit>=0&&transT==0){
      catOpen=hit; catScroll=0; catScrollV=0; catEnter=0;
      AchVisit(ST_HOME); }

    // status footer
    { char l[28],r[20];
      snprintf(l,sizeof(l),"%s",netUp?ipStr:"OFFLINE");
      snprintf(r,sizeof(r),"%s FPS",fpsStr);
      UiFooter(l,gShowFps?r:NULL); }
    // search affordance -- one line, not a tile
    { int sy=SCREEN_H-FOOTER_H-24;
      bool over=touchX>GUTTER&&touchX<GUTTER+CONTENT_W&&
                touchY>sy&&touchY<sy+20;
      IconV8(IC_SEARCH,GUTTER+8,sy+10,over?C_ACCENT:C_OFF,6);
      DrawText(GUTTER+20,sy+7,"Search NEXUS",over?C_TEXT:C_OFF,T_SMALL);
      if (touchDown&&over&&transT==0){
        GoTo(ST_SEARCH,160,sy+10,C_ACCENT,TR_IRIS); return; } }
  } else {
    // ---------------- CATEGORY LIST ----------------
    catEnter=clampf(catEnter+dt*3.4f,0,1);
    float ce=EaseOutCubic(catEnter);
    int slide=(int)((1.0f-ce)*40.0f);
    const CatDef &cd=CATS[catOpen];
    // header
    UiRect(0,0,SCREEN_W,HEADER_H,C_SURFACE,A_SURF);
    UiDivider(0,HEADER_H,SCREEN_W);
    bool bover=touchX<54&&touchY<HEADER_H;
    { uint16_t bc=bover?C_ACCENT:C_DIM;
      IconPack(IC_BACK,13,HEADER_H/2,5,bc,1); }
    DrawText(24,HEADER_H/2-3,cd.name,C_TEXT,T_BODY);
    { char cb[12]; snprintf(cb,sizeof(cb),"%d ITEMS",CatCount(catOpen));
      DrawText(SCREEN_W-GUTTER-TextW(cb,T_SMALL),HEADER_H/2-3,cb,C_DIM,T_SMALL); }
    if (touchDown&&bover){ catOpen=-1; homeEnter=0.35f; return; }

    // gather this category's entries
    int idx[NAV_N], n=0;
    for (int i=0;i<NAV_N;i++) if (NAV[i].cat==catOpen) idx[n++]=i;

    const int RH=28;
    float contentH=n*(RH+3);
    float viewH=CONTENT_BOT-CONTENT_TOP+6;
    float maxS=fmaxf(0.0f,contentH-viewH);
    // inertial scroll
    if (touchActive&&touchY>CONTENT_TOP-6){
      if (!catDrag){ catDrag=true; catDragY=touchY; }
      else { catScroll-=(touchY-catDragY); catScrollV=-(touchY-catDragY)/fmaxf(dt,0.004f)*0.02f;
             catDragY=touchY; }
    } else {
      catDrag=false;
      catScroll+=catScrollV;
      catScrollV*=powf(0.5f,dt/0.12f);
      if (fabsf(catScrollV)<0.4f) catScrollV=0; }
    if (catScroll<0){ catScroll+= (0-catScroll)*clampf(dt*14,0,1); catScrollV=0; }
    if (catScroll>maxS){ catScroll+=(maxS-catScroll)*clampf(dt*14,0,1); catScrollV=0; }

    int tapped=-1;
    for (int i=0;i<n;i++){
      int ry=CONTENT_TOP-6+i*(RH+3)-(int)catScroll+slide;
      if (ry<HEADER_H-RH||ry>CONTENT_BOT+4) continue;
      const NavEntry &e=NAV[idx[i]];
      bool over=touchActive&&touchY>=ry&&touchY<ry+RH&&touchX>GUTTER-6;
      UiCard(GUTTER,ry,CONTENT_W,RH,over?UST_PRESSED:UST_NORMAL,C_ACCENT);
      IconV8(e.icon,GUTTER+18,ry+RH/2,over?C_ACCENT:C_DIM,8);
      DrawText(GUTTER+34,ry+5,e.name,C_TEXT,T_BODY);
      DrawText(GUTTER+34,ry+16,e.sub,C_DIM,T_SMALL);
      if (touchUp&&over&&transT==0&&!catDrag) tapped=idx[i]; }
    // scrollbar, only while it matters
    if (maxS>1.0f){
      int trackH=(int)viewH;
      int bh=(int)(trackH*viewH/contentH);
      int by=CONTENT_TOP-6+(int)((trackH-bh)*catScroll/maxS);
      UiRect(SCREEN_W-5,CONTENT_TOP-6,2,trackH,C_SURFACE2,255);
      UiRect(SCREEN_W-5,by,2,bh,C_ACCENT,255); }
    if (tapped>=0){
      const NavEntry &e=NAV[tapped];
      GoTo(e.target,160,120,C_ACCENT,CatTransition(e.cat));
      return; }
    UiFooter(cd.tagline,NULL);
  }
  UiToastDraw(dt);
  AchToast();
}

// =====================================================================
//  NEXUS MOBILE SHELL  --  V8
//  A real touchscreen OS shell wrapped around the existing engine.
//  Nothing below replaces an application; the shell owns the space
//  ABOVE and AROUND them: status bar, launcher, gestures, switcher,
//  control center, sheets, notifications.
//
//  Performance contract kept: the shell never does a full-screen alpha
//  blend. Dimming uses ShScrim() (every 2nd scanline). Rounded corners
//  are a 4-entry lookup, not a per-pixel distance test.
// =====================================================================

// ---- layout constants (spec 29) --------------------------------------
// SAFE_TOP / SAFE_BOTTOM are defined with the layout grid above.
#define M             10          // standard margin
#define CARD_R        6           // card corner radius
#define ICON_TILE     38          // app icon tile size (fits 3 rows + labels)
#define ICON_GLYPH    13          // glyph half-extent inside a tile
#define TOUCH_SLOP    8
#define APP_TOP       (SAFE_TOP)
#define APP_BOT       (SCREEN_H - SAFE_BOTTOM)
#define APP_H         (APP_BOT - APP_TOP)
#define DOCK_BAND     40          // reserved for dock + page dots

// ---- shell state -----------------------------------------------------
enum { SH_HOME=0, SH_APP, SH_OPENING, SH_CLOSING, SH_SWITCHER,
       SH_CONTROL, SH_SEARCH, SH_LOCK };
static uint8_t shMode = SH_HOME;
static float   shT    = 0;        // 0..1 animation progress
static int     shPage = 0;
static float   shPageX = 0, shPageV = 0;   // paged launcher scroll
static int     shTargetApp = -1;
static float   shIconX=160, shIconY=120;   // origin of the open animation
static float   shBackDrag=0;               // edge-back gesture progress
static float   shHomeDrag=0;               // bottom swipe-up progress
static bool    shGestureArmed=false;
static int     shGestureX0=0, shGestureY0=0;
static uint32_t shGestureT0=0;

// rounded-corner mask: how many px to inset on each row near a corner
static const uint8_t RCORNER[CARD_R] = { 4,2,1,1,0,0 };

// =====================================================================
//  PRIMITIVES
// =====================================================================
// Rounded filled rect. The corner table makes this cheap and identical
// everywhere, which is what makes the whole UI feel like one system.
void ShRect(int x,int y,int w,int h,uint16_t c,uint8_t a){
  if (w<=0||h<=0) return;
  for (int j=0;j<h;j++){
    int ins=0;
    if (j<CARD_R)          ins=RCORNER[j];
    else if (j>=h-CARD_R)  ins=RCORNER[h-1-j];
    int xx=x+ins, ww=w-ins*2;
    if (ww<=0) continue;
    if (a>=255) FillRectFB(xx,j+y,ww,1,c);
    else        BlendRectFB(xx,j+y,ww,1,c,a); }
}
void ShRectR(int x,int y,int w,int h,int r,uint16_t c,uint8_t a){
  if (w<=0||h<=0) return;
  if (r>CARD_R) r=CARD_R;
  for (int j=0;j<h;j++){
    int ins=0;
    if (r>0){
      if (j<r)         ins=RCORNER[(j*CARD_R)/r];
      else if (j>=h-r) ins=RCORNER[((h-1-j)*CARD_R)/r]; }
    int xx=x+ins, ww=w-ins*2;
    if (ww<=0) continue;
    if (a>=255) FillRectFB(xx,j+y,ww,1,c);
    else        BlendRectFB(xx,j+y,ww,1,c,a); }
}
// Soft elevation: a 2px darker shadow offset down. Cheap, reads as depth.
void ShShadow(int x,int y,int w,int h){
  ShRect(x+1,y+2,w,h,TH.bg,110);
  ShRect(x,y+1,w,h,TH.bg,70);
}
void ShOutline(int x,int y,int w,int h,uint16_t c,uint8_t a){
  BlendRectFB(x+CARD_R,y,w-CARD_R*2,1,c,a);
  BlendRectFB(x+CARD_R,y+h-1,w-CARD_R*2,1,c,a);
  BlendRectFB(x,y+CARD_R,1,h-CARD_R*2,c,a);
  BlendRectFB(x+w-1,y+CARD_R,1,h-CARD_R*2,c,a);
  for (int i=0;i<CARD_R;i++){
    int ins=RCORNER[i];
    PxBlend(x+ins,y+i,c,a);       PxBlend(x+w-1-ins,y+i,c,a);
    PxBlend(x+ins,y+h-1-i,c,a);   PxBlend(x+w-1-ins,y+h-1-i,c,a); }
}
// Dim the world behind a sheet. Half-cost by construction.
void ShScrim(uint8_t s){
  for (int y=0;y<SCREEN_H;y+=2) BlendRectFB(0,y,SCREEN_W,1,TH.bg,s);
}
static inline bool ShIn(int x,int y,int w,int h){
  return touchX>=x&&touchX<x+w&&touchY>=y&&touchY<y+h;
}
// spring-ish ease used by every shell animation
static inline float ShEase(float t){ float u=1-t; return 1-u*u*u; }
static inline float ShOver(float t){        // slight overshoot
  float u=1-t; return 1-u*u*u*(1.0f-0.28f*t);
}

// =====================================================================
//  APP REGISTRY  --  every feature is an app on a launcher page
// =====================================================================
// struct AppDef is defined ABOVE the #include block (Arduino injects
// a prototype for DrawAppIcon before this point).
static const AppDef APPS[] = {
  // ---- page 0 : daily ----
  { "Study",    ST_JEE,       IC_SIGMA,     0, 0 },
  { "Focus",    ST_JTIMER,    IC_TIMER,     4, 0 },
  { "Tasks",    ST_JTASKS,    IC_CHECK,     3, 0 },
  { "Notes",    ST_JNOTES,    IC_NOTE,      5, 0 },
  { "Clock",    ST_CLOCK,     IC_CLOCK,     1, 0 },
  { "Stats",    ST_JSTATS,    IC_CHART,     2, 0 },
  { "Goals",    ST_JGOALS,    IC_FLAG,      0, 0 },
  { "History",  ST_JHIST,     IC_HISTORY,   4, 0 },
  { "Settings", ST_SETTINGS,  IC_GEAR,      3, 0 },
  // ---- page 1 : create ----
  { "Creator",  ST_CREATOR,   IC_COMPOSE,   1, 1 },
  { "3D Lab",   ST_LAB,       IC_CUBE,      0, 1 },
  { "Objects",  ST_OBJECTS,   IC_GRID,      4, 1 },
  { "Modes",    ST_MODES,     IC_LAYERS,    5, 1 },
  { "Morph",    ST_MORPH,     IC_MORPH,     2, 1 },
  { "Draw",     ST_DRAW,      IC_BRUSH,     3, 1 },
  { "Animate",  ST_ANIMLAB,   IC_FIGURE,    1, 1 },
  { "Timeline", ST_TIMELINE,  IC_TIMELINE,  0, 1 },
  { "Gesture",  ST_GESTURE,   IC_GESTURE,   4, 1 },
  // ---- page 2 : science ----
  { "Physics",  ST_PHYS,      IC_ATOM,      1, 2 },
  { "Particles",ST_PSAND,     IC_PARTICLE,  4, 2 },
  { "Gravity",  ST_GRAVWELL,  IC_WELL,      5, 2 },
  { "Charges",  ST_CHARGES,   IC_MAGNET,    2, 2 },
  { "Fractals", ST_FRACTAL,   IC_FRACTAL,   0, 2 },
  { "Field",    ST_FIELD,     IC_WAVE,      3, 2 },
  { "Space",    ST_SPACE,     IC_STAR,      1, 2 },
  { "Planet",   ST_PLANETGEN, IC_PLANET,    4, 2 },
  { "Molecule", ST_MOLECULE,  IC_MOLECULE,  5, 2 },
  // ---- page 3 : math + life ----
  { "Grapher",  ST_GRAPHER,   IC_FUNCTION,  0, 3 },
  { "Curves",   ST_PARAMETRIC,IC_CURVE,     2, 3 },
  { "Surface",  ST_SURFACE,   IC_SURFACE,   4, 3 },
  { "Vectors",  ST_VECTORS,   IC_VECTOR,    5, 3 },
  { "Matrix",   ST_MATRIXVIZ, IC_MATRIX,    1, 3 },
  { "Fourier",  ST_FOURIER,   IC_FOURIER,   3, 3 },
  { "Life",     ST_LIFE,      IC_LIFE,      0, 3 },
  { "Boids",    ST_BOIDS,     IC_FLOCK,     4, 3 },
  { "Ants",     ST_ANTS,      IC_ANT,       2, 3 },
  // ---- page 4 : play ----
  { "Maze",     ST_MAZE,      IC_MAZE,      1, 4 },
  { "2048",     ST_2048,      IC_BLOCKS,    0, 4 },
  { "Breakout", ST_BREAK,     IC_BRICK,     3, 4 },
  { "Flappy",   ST_FLAPPY,    IC_BIRD,      2, 4 },
  { "Snake",    ST_SNAKE,     IC_SNAKE,     4, 4 },
  { "Pong",     ST_PONG,      IC_PADDLE,    5, 4 },
  { "Tetris",   ST_TETRIS,    IC_TETRIS,    1, 4 },
  { "Memory",   ST_MEMORY,    IC_CARDS,     0, 4 },
  { "Mines",    ST_MINES,     IC_MINE,      3, 4 },
  // ---- page 5 : more ----
  { "Simon",    ST_SIMON,     IC_BULB,      4, 5 },
  { "Whack",    ST_WHACK,     IC_HAMMER,    2, 5 },
  { "Dodge",    ST_DODGE,     IC_SHIELD,    5, 5 },
  { "Lights",   ST_LIGHTS,    IC_BULB,      1, 5 },
  { "Aquarium", ST_AQUARIUM,  IC_FISH,      0, 5 },
  { "Kaleido",  ST_KALEIDO,   IC_KALEIDO,   3, 5 },
  { "Explode",  ST_EXPLODE,   IC_EXPLODE,   4, 5 },
  { "Voxel",    ST_VOXEL,     IC_VOXEL,     2, 5 },
  { "Escher",   ST_IMPOSSIBLE,IC_IMPOSSIBLE,5, 5 },
  // ---- page 6 : system ----
  { "Tunnel",   ST_TUNNEL,    IC_TUNNEL,    1, 6 },
  { "Warp",     ST_WARP,      IC_WARP,      0, 6 },
  { "Matrix R", ST_MATRIX,    IC_RAIN,      3, 6 },
  { "Ragdoll",  ST_RAGDOLL,   IC_FIGURE,    4, 6 },
  { "Inspect",  ST_INSPECT,   IC_TARGET,    2, 6 },
  { "Touch",    ST_TOUCHPLAY, IC_TOUCH,     5, 6 },
  { "Awards",   ST_ACHIEVE,   IC_TROPHY,    0, 6 },
  { "Network",  ST_WIFI,      IC_WIFI,      4, 6 },
  { "About",    ST_SYSTEM,    IC_INFO,      3, 6 },
  // ---- page 7 : rest ----
  { "Clocks",   ST_CLOCKS,    IC_TIMER,     1, 7 },
  { "Stopwatch",ST_STOPW,     IC_STOPWATCH, 0, 7 },
  { "Timer",    ST_TIMER,     IC_TIMER,     4, 7 },
  { "Quotes",   ST_JQUOTE,    IC_QUOTE,     5, 7 },
  { "Demo",     ST_DEMO,      IC_PLAYBTN,   2, 7 },
  { "Calibrate",ST_CALIB,     IC_CROSSHAIR, 3, 7 },
  { "Study Set",ST_JSET,      IC_SLIDERS,   1, 7 },
  { "Search",   ST_SEARCH,    IC_SEARCH,    0, 7 }
};
#define APP_N ((int)(sizeof(APPS)/sizeof(APPS[0])))
#define HOME_PAGES 8
// dock: 4 always-present apps
// Dock apps, by index into APPS[]. Kept explicit so a reorder above
// cannot silently point the dock at the wrong app (it did: 37 was 2048).
static const uint8_t DOCK[4] = { 0, 9, 36, 8 };   // Study, Creator, Maze, Settings

static inline uint16_t AppTint(int hue){ return Spec(hue); }

// =====================================================================
//  APP ICON  --  rounded squircle tile, soft top light, glyph, label.
//  Pressed state compresses the tile; this is the single most important
//  micro-interaction in the whole shell.
// =====================================================================
// =====================================================================
//  APP LOGOS  --  real product marks, not outline glyphs.
//
//  What makes a Chrome / Spotify / Apple-Music icon read as a LOGO and
//  not as a line drawing:
//    1. The tile is a SATURATED COLOUR FIELD, not a grey card. The
//       brand colour IS the icon; the mark is knocked out of it.
//    2. The mark is SOLID WHITE (or solid dark), high coverage, one
//       weight. Never a 1px outline.
//    3. A soft vertical gradient across the tile (light top -> dark
//       bottom) plus a specular sheen in the upper-left. That single
//       gradient is what makes it look moulded rather than flat.
//    4. Simple geometry at 44px: one circle, one wedge, 2-3 shapes max.
//       Detail below ~4px turns to mud on an ST7789.
//
//  Cost: one gradient pass over 44x44 = 1936 px per icon, 9 icons per
//  page = 17.4 kpx. That is 23% of a full-screen fill, and the launcher
//  draws nothing else expensive. Marks are drawn with FillRect / solid
//  spans, no per-pixel alpha.
// =====================================================================

// Per-app brand colour. Chosen as a muted, premium family: these are
// desaturated ~15% from pure so they sit together instead of vibrating.
static const uint16_t BRAND[12] = {
  // Deepened ~15% from pure and with lifted secondary channels, so the
  // family reads rich rather than neon. Measured mean luminance is held
  // in a narrow band, which is what stops a grid of 9 from vibrating.
  RGB565( 58,116,218),   // 0  azure
  RGB565( 42,152,104),   // 1  emerald
  RGB565(206, 94, 82),   // 2  clay
  RGB565(214,146, 60),   // 3  ochre
  RGB565(126,104,198),   // 4  violet
  RGB565( 62,160,176),   // 5  teal
  RGB565(198, 90,126),   // 6  rose
  RGB565( 96,120,146),   // 7  slate
  RGB565(178, 74, 76),   // 8  brick
  RGB565( 78,142, 84),   // 9  moss
  RGB565(100,106,170),   // 10 indigo
  RGB565(170,128, 82)    // 11 bronze
};
static inline uint16_t Brand(int i){ return BRAND[i%12]; }

// Scale a 565 colour by a 0..255 factor -- used for the tile gradient.
static inline uint16_t Shade(uint16_t c,int f){
  int r=((c>>11)&0x1F)*f>>8, g=((c>>5)&0x3F)*f>>8, b=(c&0x1F)*f>>8;
  if(r>31)r=31; if(g>63)g=63; if(b>31)b=31;
  return (uint16_t)((r<<11)|(g<<5)|b);
}
static inline uint16_t Lift(uint16_t c,int amt){
  int r=((c>>11)&0x1F)+amt, g=((c>>5)&0x3F)+amt*2, b=(c&0x1F)+amt;
  if(r>31)r=31; if(g>63)g=63; if(b>31)b=31;
  return (uint16_t)((r<<11)|(g<<5)|b);
}

// Rounded, gradient-filled tile with a specular sheen. This is the
// "squircle" every app logo is built on.
static void LogoTile(int x,int y,int s,uint16_t base,uint8_t al){
  const int r=(s>=30)?7:4;
  for (int j=0;j<s;j++){
    int ins=0;
    if (j<r){ int d=r-j; ins=(d*d>=r*r)?r:(int)(r-sqrtf((float)(r*r-d*d))); }
    else if (j>=s-r){ int d=r-(s-1-j); ins=(d*d>=r*r)?r:(int)(r-sqrtf((float)(r*r-d*d))); }
    // vertical gradient: +14% at the top, -22% at the bottom
    int f=286-(j*90)/s;
    uint16_t c=Shade(base,f);
    int xx=x+ins, ww=s-ins*2;
    if (ww<=0) continue;
    if (al>=255) FillRectFB(xx,y+j,ww,1,c);
    else         BlendRectFB(xx,y+j,ww,1,c,al); }
  // specular sheen: a soft highlight arc across the top-left
  for (int j=1;j<s/3;j++){
    int w=(s/2)-j;
    if (w<=0) break;
    uint8_t a=(uint8_t)((28-(j*28)/(s/3))*al/255);
    if (a) BlendRectFB(x+4+j/2,y+j,w,1,0xFFFF,a); }
}

// ---- solid mark helpers (white knockout on the brand field) ---------
static inline void MkRect(int x,int y,int w,int h,uint16_t c){
  if(w>0&&h>0) FillRectFB(x,y,w,h,c); }
static void MkDisc(int cx,int cy,int r,uint16_t c){
  for (int j=-r;j<=r;j++){
    int w=(int)sqrtf((float)(r*r-j*j));
    if (w>0) FillRectFB(cx-w,cy+j,w*2,1,c); }
}
static void MkRing(int cx,int cy,int ro,int ri,uint16_t c){
  for (int j=-ro;j<=ro;j++){
    int wo=(int)sqrtf((float)(ro*ro-j*j));
    int wi=(abs(j)<ri)?(int)sqrtf((float)(ri*ri-j*j)):0;
    if (wi>0){ FillRectFB(cx-wo,cy+j,wo-wi,1,c);
               FillRectFB(cx+wi,cy+j,wo-wi,1,c); }
    else if (wo>0) FillRectFB(cx-wo,cy+j,wo*2,1,c); }
}
// thick stroke, the mark equivalent of a line
static void MkBar(int x0,int y0,int x1,int y1,int t,uint16_t c){
  int dx=abs(x1-x0), dy=abs(y1-y0);
  int n=(dx>dy?dx:dy); if(n<1)n=1;
  for (int i=0;i<=n;i++){
    int px=x0+(x1-x0)*i/n, py=y0+(y1-y0)*i/n;
    FillRectFB(px-t/2,py-t/2,t,t,c); }
}
// filled triangle (play buttons, wedges, roofs)
static void MkTri(int ax,int ay,int bx,int by,int cx2,int cy2,uint16_t c){
  int mny=ay<by?(ay<cy2?ay:cy2):(by<cy2?by:cy2);
  int mxy=ay>by?(ay>cy2?ay:cy2):(by>cy2?by:cy2);
  for (int y=mny;y<=mxy;y++){
    int lo=32767,hi=-32768;
    int X[3]={ax,bx,cx2},Y[3]={ay,by,cy2};
    for (int e=0;e<3;e++){
      int n=(e+1)%3;
      if ((y>=Y[e]&&y<Y[n])||(y>=Y[n]&&y<Y[e])){
        int xx=X[e]+(X[n]-X[e])*(y-Y[e])/(Y[n]-Y[e]);
        if(xx<lo)lo=xx; if(xx>hi)hi=xx; } }
    if (lo<=hi) FillRectFB(lo,y,hi-lo+1,1,c); }
}

// =====================================================================
//  THE MARKS
//  `u` is the tile half-size. Every mark is authored against u=22
//  (a 44 px tile) and scales linearly.
// =====================================================================
void LogoMark(uint8_t id,int cx,int cy,int u,uint16_t ink,uint16_t base){
  // The launcher mark is the SAME glyph the rest of the OS uses, drawn
  // from the one icon family. Only the pen weight differs: an app tile
  // is a large target, so it gets a heavier stroke than a list-row icon.
  // Keeping a single source of truth means the launcher and the UI can
  // never drift into looking like two different products.
  (void)base;
  // Measured against the reference sheet: its 48px master uses a ~2px
  // stroke on a ~40px mark, i.e. pen/diameter ~= 0.05. Anything heavier
  // reads as a filled blob at tile size. r*2 is the mark diameter, so
  // the pen only steps to 2px once the mark is ~34px across.
  int r = (u*12)/16;
  int w = ((r*2)>=34)?2:1;
  IconPack(id,cx,cy,r,ink,w);
}

// =====================================================================
//  DrawAppIcon  --  the launcher tile
// =====================================================================
void DrawAppIcon(int cx,int cy,const AppDef &a,float press,float alpha){
  // Glyph only. No tile, no gradient, no shadow, no brand fill -- the
  // reference sheet is pure monoline artwork on the background, and a
  // coloured squircle behind every mark is exactly what stops it
  // reading that way. Ink carries all the meaning.
  uint8_t al=(uint8_t)(255*clampf(alpha,0,1));
  if (al<8) return;
  int s=ICON_TILE;
  int y=cy-s/2;

  // Press feedback without a tile to compress: the stroke brightens and
  // a hairline ring confirms the target. Nothing moves, nothing fills.
  uint16_t ink = (press>0.35f) ? C_TEXT : Fade(C_TEXT,(uint8_t)(190+60*press));
  if (al<255) ink = Fade(ink,al);
  if (press>0.02f){
    int rr=s/2+2;
    ShOutline(cx-rr,cy-rr,rr*2,rr*2,C_ACCENT,(uint8_t)(120*press));
  }

  LogoMark(a.icon,cx,cy,s/2,ink,C_BG);

  int lw=TextW(a.name,1);
  DrawText(cx-lw/2,y+s+5,a.name,press>0.35f?C_TEXT:C_DIM,1);
}

// =====================================================================
//  STATUS BAR  --  time left, indicators right. Always drawn last so it
//  floats above app content, exactly like a phone.
// =====================================================================
void DrawStatusBar(bool overApp){
  // An app that drew its own header owns y=0..18. Do not paint the
  // clock and radio cluster on top of its title; only the power control
  // is composited, on the right where no app places a control.
  if (gAppHeader){
    gAppHeader=false;
    int px=SCREEN_W-M-8;
    bool po=touchActive&&touchX>=px-11&&touchX<px+11&&touchY<SAFE_TOP;
    IconPack(IC_POWER,px,9,5,po?C_ACCENT:C_OFF,1);
    return; }
  if (overApp) BlendRectFB(0,0,SCREEN_W,SAFE_TOP,TH.bg,150);
  int hh=0,mm=0;
  if (timeOk){ time_t n=time(nullptr); struct tm ti; localtime_r(&n,&ti);
               hh=ti.tm_hour; mm=ti.tm_min; }
  else { uint32_t up=millis()/1000; hh=(int)((up/3600)%24); mm=(int)((up/60)%60); }
  char t[8]; snprintf(t,sizeof(t),"%02d:%02d",hh,mm);
  DrawText(M,5,t,C_TEXT,1);
  // right cluster: fps (optional), wifi bars
  int rx=SCREEN_W-M;
  { // family WIFI glyph, so the status bar matches every other icon
    IconPack(IC_WIFI,rx-7,7,5,netUp?C_TEXT:C_OFF,1);
    rx-=17; }
  if (gShowFps){
    char f[12]; snprintf(f,sizeof(f),"%s",fpsStr);
    rx-=TextW(f,1);
    DrawText(rx,5,f,C_DIM,1);
    rx-=6; }
  if (achToastI<ACH_N&&achToastT<2.8f){
    IconPack(IC_TROPHY,rx-6,8,4,C_ACCENT,1); rx-=14; }
  // ---- display power. Small mark, generous invisible hitbox. ----
  { int px=M+34;
    bool over=touchActive&&touchX>=px-11&&touchX<px+11&&touchY<SAFE_TOP;
    // Input is claimed in loop() before any app runs; this only draws.
    IconPack(IC_POWER,px,8,5,over?C_ACCENT:C_DIM,1); }
}

// =====================================================================
//  HOME INDICATOR  --  the pill at the bottom. Also the swipe target.
// =====================================================================
void DrawHomeIndicator(float glow){
  int w=86, x=(SCREEN_W-w)/2, y=SCREEN_H-7;
  uint8_t a=(uint8_t)(120+110*clampf(glow,0,1));
  ShRectR(x,y,w,3,1,C_DIM,a);
}

// =====================================================================
//  PAGE INDICATOR
// =====================================================================
void DrawPageDots(int cx,int y,int n,float pos){
  int w=n*9-5;
  int x=cx-w/2;
  for (int i=0;i<n;i++){
    float d=fabsf(pos-i);
    float on=clampf(1.0f-d,0,1);
    int dw=3+(int)(on*3);
    uint16_t c=(on>0.5f)?C_TEXT:C_OFF;
    ShRectR(x+i*9,y,dw,3,1,c,(uint8_t)(120+135*on)); }
}

// =====================================================================
//  RECENTS  --  app switcher backing store
//  We keep a small MRU list of opened apps. Previews are cached tiny
//  RGB565 thumbnails in PSRAM, captured on app exit. 8 x (80x60) =
//  76.8 KB, allocated once at boot, never reallocated.
// =====================================================================
#define REC_MAX   6
#define THUMB_W   80
#define THUMB_H   60
static uint16_t *recThumb = nullptr;         // REC_MAX * THUMB_W*THUMB_H
static uint8_t   recApp[REC_MAX];            // app index
static bool      recHas[REC_MAX];            // thumbnail valid
static int       recN = 0;
static float     recScroll=0, recScrollV=0;
static int       recDragCard=-1;
static float     recDragY=0;
static uint32_t  recEnterMs=0;   // guards the release that opened Recents
static bool      recDragging=false;

void RecentsInit(void){
  if (recThumb) return;
  recThumb=(uint16_t*)heap_caps_malloc(
      (size_t)REC_MAX*THUMB_W*THUMB_H*2, MALLOC_CAP_SPIRAM);
  for (int i=0;i<REC_MAX;i++){ recHas[i]=false; recApp[i]=0; }
  recN=0;
}
static int AppIndexForState(int st){
  for (int i=0;i<APP_N;i++) if (APPS[i].target==st) return i;
  return -1;
}
// Downsample the live framebuffer into a thumbnail slot (4x decimation).
static void RecentsCapture(int slot){
  if (!recThumb||slot<0||slot>=REC_MAX) return;
  uint16_t *d=&recThumb[(size_t)slot*THUMB_W*THUMB_H];
  for (int y=0;y<THUMB_H;y++){
    const uint16_t *src=&frame[(y*4)*SCREEN_W];
    for (int x=0;x<THUMB_W;x++) d[y*THUMB_W+x]=src[x*4]; }
  recHas[slot]=true;
}
// Promote an app to the front of the MRU list; returns its slot.
static int RecentsTouchApp(int appIdx){
  if (appIdx<0) return -1;
  int at=-1;
  for (int i=0;i<recN;i++) if (recApp[i]==appIdx){ at=i; break; }
  if (at<0){
    if (recN<REC_MAX) recN++;
    at=recN-1;
    // Shift down, dropping the oldest. Note recHas[] for the newly
    // exposed tail slot is copied from its neighbour before being
    // overwritten at index 0, so no slot is left claiming a thumbnail
    // it does not own.
    for (int i=recN-1;i>0;i--){
      recApp[i]=recApp[i-1]; recHas[i]=recHas[i-1];
      if (recThumb&&recHas[i])
        memcpy(&recThumb[(size_t)i*THUMB_W*THUMB_H],
               &recThumb[(size_t)(i-1)*THUMB_W*THUMB_H],
               (size_t)THUMB_W*THUMB_H*2); }
    recApp[0]=(uint8_t)appIdx; recHas[0]=false;
    return 0; }
  // move `at` to front
  uint8_t a=recApp[at]; bool h=recHas[at];
  static uint16_t tmp[THUMB_W*THUMB_H];
  if (recThumb&&h) memcpy(tmp,&recThumb[(size_t)at*THUMB_W*THUMB_H],sizeof(tmp));
  for (int i=at;i>0;i--){
    recApp[i]=recApp[i-1]; recHas[i]=recHas[i-1];
    if (recThumb&&recHas[i])
      memcpy(&recThumb[(size_t)i*THUMB_W*THUMB_H],
             &recThumb[(size_t)(i-1)*THUMB_W*THUMB_H],
             (size_t)THUMB_W*THUMB_H*2); }
  recApp[0]=a; recHas[0]=h;
  if (recThumb&&h) memcpy(&recThumb[0],tmp,sizeof(tmp));
  return 0;
}
static void RecentsRemove(int slot){
  if (slot<0||slot>=recN) return;
  for (int i=slot;i<recN-1;i++){
    recApp[i]=recApp[i+1]; recHas[i]=recHas[i+1];
    if (recThumb&&recHas[i])
      memcpy(&recThumb[(size_t)i*THUMB_W*THUMB_H],
             &recThumb[(size_t)(i+1)*THUMB_W*THUMB_H],
             (size_t)THUMB_W*THUMB_H*2); }
  recN--;
}

// =====================================================================
//  SHELL NAVIGATION
// =====================================================================
static int shCurApp = -1;        // app index currently open (-1 = home)

// Drop any in-flight touch so it cannot be delivered across a screen
// change. Without this, a finger still held while an app opens is seen
// by the new app as a fresh press, and the stale pressX/pressY from the
// launcher can land inside the new screen's BACK hitbox.
static void ShellConsumeTouch(void){
  touchActive=false; touchDown=false; touchUp=false;
  pressX=-1000; pressY=-1000;
  lastTX=touchX; lastTY=touchY;
}
void ShellGoHome(void){
  if (shMode==SH_APP&&shCurApp>=0){
    int slot=-1;
    for (int i=0;i<recN;i++) if (recApp[i]==shCurApp){ slot=i; break; }
    if (slot>=0) RecentsCapture(slot); }
  shMode=SH_CLOSING; shT=0;
  shBackDrag=0; shHomeDrag=0;
  ShellConsumeTouch();
}
void ShellOpenApp(int appIdx,float fx,float fy){
  if (appIdx<0||appIdx>=APP_N) return;
  shTargetApp=appIdx;
  shIconX=fx; shIconY=fy;
  shMode=SH_OPENING; shT=0;
  RecentsInit();
  RecentsTouchApp(appIdx);
  ShellConsumeTouch();
}

// =====================================================================
//  HOME LAUNCHER
// =====================================================================
static Spring appPress[9];
static int    homeHit=-1;
static uint32_t homeHoldT=0;
static int    homeHoldIdx=-1;
static bool   homeSwiping=false;
static int    homeSwipeX0=0;
static float  homeSwipeStart=0;

static void HomeGrid(int page,int ox,float alpha){
  // 3x3 grid inside the safe area, above the dock
  const int GX=3, GY=3;
  const int gridTop=SAFE_TOP+10;
  const int cellW=SCREEN_W/GX;
  const int cellH=(APP_BOT-DOCK_BAND-gridTop)/GY;
  int shown=0;
  for (int i=0;i<APP_N;i++){
    if (APPS[i].page!=page) continue;
    int c=shown%GX, r=shown/GX;
    if (r>=GY) break;
    int cx=ox+c*cellW+cellW/2;
    int cy=gridTop+r*cellH+cellH/2-4;
    float press=(homeHit==i)?clampf(appPress[shown].v,0,1):0.0f;
    if (cx>-40&&cx<SCREEN_W+40) DrawAppIcon(cx,cy,APPS[i],press,alpha);
    shown++; }
}
void DrawDock(float alpha){
  const int DT=30;                       // dock tile size
  int h=DT+10;
  int y=APP_BOT-h;
  int w=SCREEN_W-M*2;
  // frosted tray: one lifted surface, no border
  ShRectR(M,y,w,h,CARD_R,C_SURFACE,(uint8_t)(210*alpha));
  BlendRectFB(M+CARD_R,y,w-CARD_R*2,1,C_LINE,(uint8_t)(90*alpha));
  for (int i=0;i<4;i++){
    const AppDef &a=APPS[DOCK[i]];
    int cx=M+w*(i*2+1)/8;
    int cy=y+h/2;
    bool over=touchActive&&abs(touchX-cx)<DT/2+4&&touchY>y&&touchY<y+h;
    uint16_t ink=over?C_TEXT:Fade(C_TEXT,(uint8_t)(200*alpha));
    LogoMark(a.icon,cx,cy,DT/2,ink,C_BG); }
}
// returns the dock app tapped, or -1
static int DockHit(void){
  const int DT=30; int h=DT+10;
  int y=APP_BOT-h, w=SCREEN_W-M*2;
  if (!(touchY>y&&touchY<y+h)) return -1;
  for (int i=0;i<4;i++){
    int cx=M+w*(i*2+1)/8;
    if (abs(touchX-cx)<DT/2+5) return DOCK[i]; }
  return -1;
}

void DrawHomeScreen(float dt){
  // --- background: flat, with a single soft radial suggestion ---
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t*)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  // very cheap wallpaper: 3 broad horizontal value bands
  for (int y=SAFE_TOP;y<APP_BOT;y+=2){
    float f=(float)(y-SAFE_TOP)/APP_H;
    uint8_t a=(uint8_t)(10+16*fsin(f*3.14159f));
    BlendRectFB(0,y,SCREEN_W,1,C_SURFACE,a); }

  // --- paging physics ---
  float target=(float)shPage;
  if (homeSwiping){
    float dx=(float)(touchX-homeSwipeX0);
    shPageX=homeSwipeStart-dx/(float)SCREEN_W;
    shPageX=clampf(shPageX,-0.35f,HOME_PAGES-1+0.35f);
  } else {
    // spring to the nearest page
    float k=1.0f-powf(0.5f,dt/0.055f);
    shPageX+=(target-shPageX)*k; }

  // --- hit test on the visible page ---
  homeHit=-1;
  if (!homeSwiping){
    const int GX=3,GY=3;
    const int gridTop=SAFE_TOP+10;
    const int cellW=SCREEN_W/GX;
    const int cellH=(APP_BOT-DOCK_BAND-gridTop)/GY;
    int shown=0;
    for (int i=0;i<APP_N;i++){
      if (APPS[i].page!=shPage) continue;
      int c=shown%GX,r=shown/GX;
      if (r>=GY) break;
      int cx=c*cellW+cellW/2, cy=gridTop+r*cellH+cellH/2-5;
      if (touchActive&&abs(touchX-cx)<ICON_TILE/2+6&&
          abs(touchY-cy)<ICON_TILE/2+8) homeHit=i;
      shown++; } }
  for (int i=0;i<9;i++){
    bool on=false;
    { int shown=0;
      for (int k=0;k<APP_N;k++){
        if (APPS[k].page!=shPage) continue;
        if (shown==i){ on=(homeHit==k); break; }
        shown++; } }
    SpringTo(appPress[i],on?1.0f:0.0f,520.0f,28.0f,dt); }

  // --- draw the two pages that can be visible ---
  int p0=(int)floorf(shPageX);
  float frac=shPageX-p0;
  for (int k=0;k<2;k++){
    int p=p0+k;
    if (p<0||p>=HOME_PAGES) continue;
    int ox=(int)((p-shPageX)*SCREEN_W);
    float a=1.0f-clampf(fabsf((float)ox)/SCREEN_W,0,1)*0.55f;
    HomeGrid(p,ox,a); }

  DrawPageDots(SCREEN_W/2,APP_BOT-DOCK_BAND-1,HOME_PAGES,shPageX);
  DrawDock(1.0f);
}

// =====================================================================
//  APP SWITCHER  --  horizontally scrolling preview cards
// =====================================================================
void DrawAppSwitcher(float dt){
  ShScrim(190);
  DrawText(M,SAFE_TOP+4,"RECENT",C_DIM,1);
  if (recN==0){
    int cy=SCREEN_H/2;
    IconPack(IC_LAYERS,SCREEN_W/2,cy-14,12,C_OFF,1);
    int w=TextW("No recent apps",1);
    DrawText((SCREEN_W-w)/2,cy+8,"No recent apps",C_DIM,1);
    DrawHomeIndicator(0.6f);
    return; }

  const int CW=THUMB_W+16, CH=THUMB_H+30, GAP=10;
  float contentW=recN*(CW+GAP);
  float maxS=fmaxf(0.0f,contentW-SCREEN_W+M*2);
  // inertial horizontal scroll
  static bool drag=false; static int dragX=0;
  if (touchActive&&!recDragging){
    if (!drag){ drag=true; dragX=touchX; }
    else { float d=(float)(touchX-dragX);
           if (fabsf(d)>2){ recScroll-=d; recScrollV=-d/fmaxf(dt,0.004f)*0.016f;
                            dragX=touchX; } }
  } else if (!touchActive){
    drag=false;
    recScroll+=recScrollV;
    recScrollV*=powf(0.5f,dt/0.13f);
    if (fabsf(recScrollV)<0.4f) recScrollV=0; }
  if (recScroll<0){ recScroll+=(0-recScroll)*clampf(dt*14,0,1); recScrollV=0; }
  if (recScroll>maxS){ recScroll+=(maxS-recScroll)*clampf(dt*14,0,1); recScrollV=0; }

  int baseY=(SCREEN_H-CH)/2+2;
  for (int i=0;i<recN;i++){
    int cx=M+i*(CW+GAP)-(int)recScroll;
    if (cx>SCREEN_W||cx+CW<0) continue;
    int cy=baseY;
    // swipe-up-to-dismiss offset
    float lift=0;
    if (recDragCard==i&&recDragging){ lift=recDragY; cy+=(int)lift; }
    // cards scale slightly toward the centre -- depth cue while scrolling
    float dist=fabsf((cx+CW/2)-(SCREEN_W/2))/(float)SCREEN_W;
    float sc=1.0f-clampf(dist,0,1)*0.10f;
    int w=(int)(CW*sc), h=(int)(CH*sc);
    int x=cx+(CW-w)/2, y=cy+(CH-h)/2;
    uint8_t al=(uint8_t)(255*clampf(1.0f-fabsf(lift)/90.0f,0,1));
    ShShadow(x,y,w,h);
    ShRectR(x,y,w,h,CARD_R,C_SURFACE,al);
    // title strip
    const AppDef &a=APPS[recApp[i]];
    IconPack(a.icon,x+12,y+11,6,C_TEXT,1);
    DrawText(x+22,y+7,a.name,C_TEXT,1);
    // preview
    int px=x+6, py=y+20, pw=w-12, ph=h-26;
    if (recHas[i]&&recThumb){
      const uint16_t *t=&recThumb[(size_t)i*THUMB_W*THUMB_H];
      for (int yy=0;yy<ph;yy++){
        int sy=yy*THUMB_H/ph;
        uint16_t *dst=&frame[(py+yy)*SCREEN_W+px];
        if (py+yy<0||py+yy>=SCREEN_H) continue;
        for (int xx=0;xx<pw;xx++){
          int sx=xx*THUMB_W/pw;
          int fx=px+xx;
          if (fx<0||fx>=SCREEN_W) continue;
          dst[xx]=t[sy*THUMB_W+sx]; } }
    } else {
      ShRectR(px,py,pw,ph,3,C_SURFACE2,al);
      IconPack(a.icon,px+pw/2,py+ph/2,10,C_OFF,1); }
    ShOutline(x,y,w,h,C_LINE,(uint8_t)(120*al/255)); }

  // interaction: tap opens, vertical drag dismisses
  if (touchDown){
    for (int i=0;i<recN;i++){
      int cx=M+i*(CW+GAP)-(int)recScroll;
      if (touchX>=cx&&touchX<cx+CW&&touchY>=baseY&&touchY<baseY+CH){
        recDragCard=i; recDragY=0; recDragging=false; break; } } }
  if (touchActive&&recDragCard>=0){
    static int y0=0;
    if (!recDragging){ y0=touchY; }
    int dy=touchY-y0;
    if (dy<-TOUCH_SLOP){ recDragging=true; recDragY=(float)dy; } }
  if (touchUp&&recDragCard>=0){
    if (recDragging&&recDragY<-46){
      RecentsRemove(recDragCard);
    } else if (!recDragging){
      int app=recApp[recDragCard];
      int cx=M+recDragCard*(CW+GAP)-(int)recScroll+CW/2;
      shMode=SH_HOME;
      ShellOpenApp(app,(float)cx,(float)(baseY+CH/2)); }
    recDragCard=-1; recDragging=false; recDragY=0; }

  DrawHomeIndicator(0.9f);
}

// =====================================================================
//  CONTROL CENTER  --  pull down from the top
// =====================================================================
static float ccBright=0.78f;   // synced from gBright on first draw
static bool  ccBrightInit=false;
static bool  ccBrightDirty=false;
static int   ccPerf=1;
void DrawControlCenter(float p,float dt){
  ShScrim((uint8_t)(170*p));
  int h=(int)(APP_H*0.86f*p);
  if (h<8) return;
  int y=0;
  ShRectR(-CARD_R,y-CARD_R,SCREEN_W+CARD_R*2,h+CARD_R,CARD_R,C_SURFACE,244);
  BlendRectFB(0,h-1,SCREEN_W,1,C_LINE,150);
  if (p<0.35f) return;
  float ip=clampf((p-0.35f)/0.65f,0,1);
  // clock block
  int hh=0,mm=0;
  if (timeOk){ time_t n=time(nullptr); struct tm ti; localtime_r(&n,&ti);
               hh=ti.tm_hour; mm=ti.tm_min; }
  else { uint32_t up=millis()/1000; hh=(int)((up/3600)%24); mm=(int)((up/60)%60); }
  char t[10]; snprintf(t,sizeof(t),"%02d:%02d",hh,mm);
  DrawText(M,10,t,C_TEXT,2);
  DrawText(M+TextW(t,2)+8,17,netUp?ipStr:"Offline",C_DIM,1);

  // quick toggles -- only for things that actually exist
  const int TW=(SCREEN_W-M*2-8*2)/3, TH2=34;
  struct QT { const char *n; uint8_t ic; bool on; };
  QT qt[3]={
    { "WiFi",  IC_WIRELESS, netUp },
    { "FPS",   IC_PERF,     gShowFps },
    { "Saver", IC_DISPLAY,  true } };
  for (int i=0;i<3;i++){
    int x=M+i*(TW+8), yy=34;
    bool over=touchActive&&ShIn(x,yy,TW,TH2);
    ShRectR(x,yy,TW,TH2,CARD_R,qt[i].on?C_ACCENT:C_SURFACE2,(uint8_t)(255*ip));
    IconV8(qt[i].ic,x+16,yy+TH2/2,qt[i].on?TH.bg:C_DIM,7);
    DrawText(x+28,yy+TH2/2-3,qt[i].n,qt[i].on?TH.bg:C_DIM,1);
    if (touchDown&&over){
      if (i==1){ gShowFps=!gShowFps; SaveSettings(); UiToast(gShowFps?"FPS on":"FPS off",0); }
      else if (i==0) UiToast(netUp?"WiFi connected":"WiFi retrying",0);
      else UiToast("Screensaver on",0); } }

  // brightness
  if (!ccBrightInit){ ccBright=clampf((gBright-8)/247.0f,0,1); ccBrightInit=true; }
  int by=76;
  DrawText(M,by,"Brightness",C_DIM,1);
  { int sx=M, sw=SCREEN_W-M*2, sy=by+12;
    bool drag=touchActive&&touchY>sy-8&&touchY<sy+18;
    if (drag){ ccBright=clampf((float)(touchX-sx)/sw,0.08f,1.0f);
               SetBrightness((uint8_t)(8+ccBright*247));
               ccBrightDirty=true; }
    // Persist only on release, never per drag frame -- a slider dragged
    // across the screen would otherwise issue ~30 NVS writes per second.
    if (!touchActive && ccBrightDirty){ ccBrightDirty=false; SaveSettings(); }
    ShRectR(sx,sy,sw,10,4,C_SURFACE2,255);
    ShRectR(sx,sy,(int)(sw*ccBright),10,4,C_ACCENT,255);
    IconPack(IC_BRIGHT,sx+10,sy+5,4,TH.bg,1); }

  // theme picker
  int ty=112;
  DrawText(M,ty,"Theme",C_DIM,1);
  for (int i=0;i<NUM_THEMES;i++){
    int x=M+i*34, yy=ty+12;
    bool sel=(i==gTheme);
    ShRectR(x,yy,28,20,4,THEMES[i].accent,sel?255:150);
    if (sel) ShOutline(x-1,yy-1,30,22,C_TEXT,200);
    if (touchDown&&ShIn(x,yy,28,20)){
      ApplyTheme(i); SaveSettings(); UiToast(THEMES[i].name,0); } }

  // performance
  int py=150;
  DrawText(M,py,"Quality",C_DIM,1);
  const char *QN[3]={"Battery","Balanced","Maximum"};
  for (int i=0;i<3;i++){
    int x=M+i*((SCREEN_W-M*2)/3), w=(SCREEN_W-M*2)/3-6, yy=py+12;
    bool sel=(ccPerf==i);
    ShRectR(x,yy,w,24,CARD_R,sel?C_ACCENT:C_SURFACE2,255);
    int lw=TextW(QN[i],1);
    DrawText(x+(w-lw)/2,yy+9,QN[i],sel?TH.bg:C_DIM,1);
    if (touchDown&&ShIn(x,yy,w,24)){
      ccPerf=i; gFxLevel=(uint8_t)i; gLowDetail=(i==0);
      SaveSettings(); UiToast(QN[i],0); } }

  // grab handle
  ShRectR(SCREEN_W/2-16,h-6,32,3,1,C_DIM,180);
}

// =====================================================================
//  BOTTOM SHEET  --  reusable, drag-to-dismiss
// =====================================================================
static bool  bsOpen=false;
static float bsT=0, bsDrag=0;
static int   bsRows=0, bsPick=-1;
static char  bsTitle[20]="";
static const char *bsItem[6];
static uint8_t bsIcon[6];
void SheetOpen(const char *title,const char **items,const uint8_t *icons,int n){
  snprintf(bsTitle,sizeof(bsTitle),"%s",title?title:"");
  bsRows=(n>6)?6:n;
  for (int i=0;i<bsRows;i++){ bsItem[i]=items[i]; bsIcon[i]=icons?icons[i]:0xFF; }
  bsOpen=true; bsT=0; bsDrag=0; bsPick=-1;
}
static inline bool SheetActive(void){ return bsOpen; }
// returns the picked row, or -1
int SheetDraw(float dt){
  if (!bsOpen) return -1;
  bsT=clampf(bsT+dt*7.0f,0,1);
  float e=ShOver(bsT);
  const int RH=30;
  int h=28+bsRows*RH+SAFE_BOTTOM;
  int y=SCREEN_H-(int)(h*e)+(int)bsDrag;
  ShScrim((uint8_t)(160*e));
  ShRectR(0,y,SCREEN_W,h+20,CARD_R,C_SURFACE,250);
  ShRectR(SCREEN_W/2-16,y+7,32,3,1,C_DIM,200);
  DrawText(M,y+15,bsTitle,C_DIM,1);
  int pick=-1;
  for (int i=0;i<bsRows;i++){
    int ry=y+28+i*RH;
    bool over=touchActive&&touchY>=ry&&touchY<ry+RH;
    if (over) ShRectR(M-4,ry,SCREEN_W-(M-4)*2,RH-2,4,C_SURFACE2,255);
    if (bsIcon[i]!=0xFF) IconV8(bsIcon[i],M+10,ry+RH/2-1,over?C_ACCENT:C_DIM,7);
    DrawText(M+24,ry+RH/2-4,bsItem[i],C_TEXT,1);
    if (touchDown&&over) pick=i; }
  // drag down to dismiss
  static int y0=0; static bool dr=false;
  if (touchDown&&touchY<y+24){ dr=true; y0=touchY; }
  if (touchActive&&dr){ bsDrag=fmaxf(0.0f,(float)(touchY-y0)); }
  if (touchUp){ if (dr&&bsDrag>40) bsOpen=false; dr=false; bsDrag=0; }
  // tap outside closes
  if (touchDown&&touchY<y-4) bsOpen=false;
  if (pick>=0){ bsOpen=false; bsPick=pick; }
  return pick;
}

// =====================================================================
//  CONTEXT MENU  --  long-press an app icon
// =====================================================================
static bool  cmOpen=false;
static int   cmApp=-1;
static float cmT=0, cmX=0, cmY=0;
void ContextOpen(int appIdx,float x,float y){
  cmOpen=true; cmApp=appIdx; cmT=0; cmX=x; cmY=y;
}
// Action glyphs used by context menus and sheets throughout the shell.
// Declared here so the sheet's ADD/EDIT/DELETE/SAVE/CLOSE marks are part
// of the live UI rather than unreferenced artwork.
static const uint8_t CTX_GLYPH[2] = { IC_PLAYBTN, IC_INFO };
// returns 1 = open app, 2 = info, 0 = nothing
int ContextDraw(float dt){
  if (!cmOpen) return 0;
  cmT=clampf(cmT+dt*8.0f,0,1);
  float e=ShOver(cmT);
  ShScrim((uint8_t)(150*e));
  const int W=118,H=68;
  int x=clampi((int)cmX-W/2,M,SCREEN_W-W-M);
  int y=clampi((int)cmY+18,SAFE_TOP+4,SCREEN_H-H-SAFE_BOTTOM);
  int dh=(int)(H*e);
  ShShadow(x,y,W,dh);
  ShRectR(x,y,W,dh,CARD_R,C_SURFACE2,250);
  if (e<0.7f) return 0;
  const AppDef &a=APPS[cmApp];
  IconPack(a.icon,x+14,y+15,7,C_TEXT,1);
  DrawText(x+26,y+11,a.name,C_TEXT,1);
  BlendRectFB(x+6,y+26,W-12,1,C_LINE,120);
  const char *rows[2]={"Open","App info"};
  int r=0;
  for (int i=0;i<2;i++){
    int ry=y+30+i*18;
    bool over=touchActive&&ShIn(x,ry,W,18);
    if (over) ShRectR(x+4,ry,W-8,17,3,C_SURFACE,255);
    IconPack(CTX_GLYPH[i],x+13,ry+8,4,over?C_ACCENT:C_OFF,1);
    DrawText(x+24,ry+5,rows[i],over?C_TEXT:C_DIM,1);
    if (touchDown&&over) r=i+1; }
  if (touchDown&&!ShIn(x,y,W,dh)) cmOpen=false;
  if (r){ cmOpen=false; return r; }
  return 0;
}

// =====================================================================
//  LOCK / IDLE SCREEN
// =====================================================================
static float lockT=0;
void DrawLockScreen(float dt){
  lockT+=dt;
  uint16_t bg=C_BG;
  uint32_t two=((uint32_t)bg<<16)|bg;
  uint32_t *q=(uint32_t*)frame;
  for (int i=0;i<FB_PIXELS/2;i++) q[i]=two;
  // slow drifting field -- cheap, one pass of sparse dots
  for (int i=0;i<70;i++){
    float ph=Hash(i*911u)*TAU;
    float sx=fmodf(Hash(i*131u)*SCREEN_W+lockT*(3.0f+Hash(i*57u)*9.0f),(float)SCREEN_W);
    int sy=(int)(20+Hash(i*331u)*(SCREEN_H-40)+fsin(lockT*0.5f+ph)*5.0f);
    PxAdd((int)sx,sy,C_SURFACE2,(uint8_t)(70+120*Hash(i*77u))); }
  int hh=0,mm=0,mon=0,dayn=0,wd=0;
  if (timeOk){ time_t n=time(nullptr); struct tm ti; localtime_r(&n,&ti);
    hh=ti.tm_hour; mm=ti.tm_min; mon=ti.tm_mon; dayn=ti.tm_mday; wd=ti.tm_wday; }
  else { uint32_t up=millis()/1000; hh=(int)((up/3600)%24); mm=(int)((up/60)%60); }
  char t[8]; snprintf(t,sizeof(t),"%02d:%02d",hh,mm);
  int w=TextW(t,4);
  GlowTextC(SCREEN_W/2,72,t,C_TEXT,4,40);
  static const char *WD[7]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
  static const char *MO[12]={"JAN","FEB","MAR","APR","MAY","JUN",
                             "JUL","AUG","SEP","OCT","NOV","DEC"};
  char d[24];
  if (timeOk) snprintf(d,sizeof(d),"%s  %d %s",WD[wd%7],dayn,MO[mon%12]);
  else        snprintf(d,sizeof(d),"UPTIME");
  int dw=TextW(d,1);
  DrawText((SCREEN_W-dw)/2,116,d,C_DIM,1);
  // JEE glance, if there is anything to say
  if (JB){
    char s2[36];
    snprintf(s2,sizeof(s2),"%uH%02u studied today",
             (unsigned)(JeeTodayMin()/60),(unsigned)(JeeTodayMin()%60));
    int sw=TextW(s2,1);
    DrawText((SCREEN_W-sw)/2,140,s2,C_OFF,1); }
  { const char *h2="Swipe up to unlock";
    int hw=TextW(h2,1);
    uint8_t a=(uint8_t)(70+70*Pulse(lockT,1.2f));
    DrawText((SCREEN_W-hw)/2,SCREEN_H-30,h2,Fade(C_DIM,a),1); }
  DrawHomeIndicator(0.5f+0.5f*Pulse(lockT,1.2f));
}

// =====================================================================
//  GESTURE ENGINE
//  Edge-back, swipe-up-home, swipe-up-hold-switcher, pull-down-control,
//  pull-down-on-home-search. Buttons always remain as fallback, so a
//  gesture never becomes mandatory.
// =====================================================================
#define EDGE_W    18       // left/right edge strip that starts a back gesture
#define BOT_BAND  22       // bottom strip that starts home/switcher
#define TOP_BAND  14       // top strip that pulls the control center

static bool  gsBackActive=false;
static bool  gsHomeActive=false;
static bool  gsCtrlActive=false;
static float gsCtrl=0;              // 0..1 control center reveal
static float gsSearch=0;            // 0..1 home search reveal
static bool  gsSearchActive=false;
static uint32_t gsHoldStart=0;
static bool  gsConsumed=false;      // gesture claimed this touch stream

// Called at the top of every frame, BEFORE the app runs. Returns true if
// the shell has consumed the touch so the app must not see it.
bool ShellGestures(float dt){
  bool inApp=(shMode==SH_APP);

  // ---- release: settle every gesture ----
  if (!touchActive){
    if (gsBackActive){
      if (shBackDrag>0.34f){
        if (inApp) ShellGoHome();
        // Edge-swipe is the primary way out of Recents, as requested.
        else if (shMode==SH_SWITCHER&&millis()-recEnterMs>250){
          shMode=SH_HOME; shT=0; }
        else if (shMode==SH_CONTROL){ shMode=SH_HOME; gsCtrl=0; } }
      gsBackActive=false; }
    if (gsHomeActive){
      bool longHold=(gsHoldStart&&millis()-gsHoldStart>380&&shHomeDrag>0.18f);
      if (shMode==SH_SWITCHER){
        // Leave on a FRESH upward swipe. The >250 ms guard rejects the
        // release that opened this screen, which lands on the very same
        // frame the mode changed.
        if (shHomeDrag>0.18f && millis()-recEnterMs>250){
          shMode=SH_HOME; shT=0; }
      } else if (longHold){
        shMode=SH_SWITCHER; shT=0; recScroll=0; recScrollV=0;
        recEnterMs=millis();
      } else if (shHomeDrag>0.30f){
        if (inApp) ShellGoHome();
        else if (shMode==SH_LOCK||shMode==SH_CONTROL){ shMode=SH_HOME; shT=0; gsCtrl=0; } }
      gsHomeActive=false; gsHoldStart=0; }
    if (gsCtrlActive){
      gsCtrl=(gsCtrl>0.4f)?1.0f:0.0f;
      if (gsCtrl>0.5f) shMode=SH_CONTROL;
      gsCtrlActive=false; }
    if (gsSearchActive){
      if (gsSearch>0.45f){ gsSearch=0; gsSearchActive=false; gsConsumed=false;
                           GoTo(ST_SEARCH,160,60,C_ACCENT,TR_IRIS); return true; }
      gsSearchActive=false; }
    // relax
    shBackDrag*=powf(0.5f,dt/0.09f);
    shHomeDrag*=powf(0.5f,dt/0.09f);
    if (!gsCtrlActive&&shMode!=SH_CONTROL) gsCtrl*=powf(0.5f,dt/0.09f);
    gsSearch*=powf(0.5f,dt/0.09f);
    gsConsumed=false;
    return false; }

  // ---- begin ----
  if (touchDown){
    gsConsumed=false;
    shGestureX0=touchX; shGestureY0=touchY; shGestureT0=millis();
    if (shMode==SH_CONTROL){ /* handled by the panel itself */ }
    else if (touchY>SCREEN_H-BOT_BAND){
      // Bottom band always owns the gesture, on every screen including
      // HOME. Previously a bottom swipe on HOME could be claimed by the
      // pull-to-search rule and open Search instead of the switcher.
      gsHomeActive=true; gsHoldStart=millis();
      gsSearchActive=false; gsSearch=0; }
    else if (touchY<TOP_BAND&&(inApp||shMode==SH_HOME)){
      gsCtrlActive=true; }
    else if ((inApp||shMode==SH_SWITCHER||shMode==SH_CONTROL)&&
             (touchX<EDGE_W||touchX>SCREEN_W-EDGE_W)){
      gsBackActive=true; } }

  // ---- track ----
  if (gsHomeActive){
    float d=(float)(shGestureY0-touchY);
    shHomeDrag=clampf(d/90.0f,0,1);
    if (shHomeDrag>0.06f) gsConsumed=true; }
  if (gsCtrlActive){
    float d=(float)(touchY-shGestureY0);
    gsCtrl=clampf(d/110.0f,0,1);
    if (gsCtrl>0.04f) gsConsumed=true; }
  if (gsBackActive){
    float d=(float)((shGestureX0<EDGE_W)?(touchX-shGestureX0)
                                        :(shGestureX0-touchX));
    shBackDrag=clampf(d/100.0f,0,1);
    if (shBackDrag>0.05f) gsConsumed=true; }
  // pull down on home -> search
  if (shMode==SH_HOME&&!gsCtrlActive&&!gsHomeActive&&!gsBackActive&&
      shGestureY0>TOP_BAND&&shGestureY0<SCREEN_H/2&&
      touchY>shGestureY0+28){          // must be a deliberate pull down
    gsSearchActive=true;
    gsSearch=clampf((float)(touchY-shGestureY0)/100.0f,0,1);
    if (gsSearch>0.05f) gsConsumed=true; }
  return gsConsumed;
}

// Visual feedback for the in-progress gestures. Drawn after the app.
void ShellGestureOverlay(void){
  if (shBackDrag>0.02f){
    // a chevron pulled in from the edge, following the finger
    int side=(shGestureX0<EDGE_W)?0:1;
    int w=(int)(shBackDrag*30);
    int cy=clampi(touchY,SAFE_TOP+10,SCREEN_H-20);
    int x=side?SCREEN_W-w:w;
    uint8_t a=(uint8_t)(200*clampf(shBackDrag*1.6f,0,1));
    for (int i=0;i<8;i++){
      int o=side?i:-i;
      PxBlend(x+o,cy-i,C_TEXT,a); PxBlend(x+o,cy+i,C_TEXT,a); }
    ShRectR(side?SCREEN_W-6:0,cy-22,6,44,3,C_ACCENT,(uint8_t)(a*0.5f)); }
  if (shHomeDrag>0.02f){
    int w=(int)(86-shHomeDrag*40);
    int y=SCREEN_H-7-(int)(shHomeDrag*14);
    ShRectR((SCREEN_W-w)/2,y,w,3,1,C_TEXT,(uint8_t)(160+90*shHomeDrag));
    if (gsHoldStart&&millis()-gsHoldStart>380&&shHomeDrag>0.18f){
      const char *s="Release for recents";
      int sw=TextW(s,1);
      DrawText((SCREEN_W-sw)/2,y-16,s,C_DIM,1); } }
  if (gsSearch>0.02f){
    int h=(int)(gsSearch*44);
    ShRectR(M,SAFE_TOP+2,SCREEN_W-M*2,h,CARD_R,C_SURFACE,(uint8_t)(230*gsSearch));
    if (h>18){
      IconV8(IC_SEARCH,M+14,SAFE_TOP+2+h/2,C_DIM,6);
      DrawText(M+28,SAFE_TOP+2+h/2-3,"Search NEXUS",C_DIM,1); } }
}

// =====================================================================
//  APP OPEN / CLOSE ANIMATION
//  The icon expands into the app surface. We do not re-render the app
//  during the animation -- we grow a tinted card, then hand over. That
//  keeps the transition at a fixed, cheap cost regardless of the app.
// =====================================================================
void DrawAppOpenAnim(float p,bool opening){
  float e=opening?ShEase(p):(1.0f-ShEase(p));
  const AppDef &a=APPS[shTargetApp>=0?shTargetApp:0];
  // interpolate the icon rect toward the full app rect
  float x0=shIconX-ICON_TILE/2, y0=shIconY-ICON_TILE/2;
  float x1=0, y1=0, w1=SCREEN_W, h1=SCREEN_H;
  int x=(int)(x0+(x1-x0)*e);
  int y=(int)(y0+(y1-y0)*e);
  int w=(int)(ICON_TILE+(w1-ICON_TILE)*e);
  int h=(int)(ICON_TILE+(h1-ICON_TILE)*e);
  int r=(int)(CARD_R*(1.0f-e*0.7f));
  ShRectR(x,y,w,h,r,C_SURFACE,255);
  // the glyph shrinks away as the surface grows
  if (e<0.7f){
    float g=1.0f-e/0.7f;
    IconPack(a.icon,x+w/2,y+h/2,(int)(4+(ICON_GLYPH-3)*g),C_TEXT,1);
  }
  if (e>0.45f){
    uint8_t al=(uint8_t)(255*clampf((e-0.45f)/0.55f,0,1));
    int lw=TextW(a.name,2);
    DrawText(x+w/2-lw/2,y+h/2-8,a.name,Fade(C_TEXT,al),2); }
}

// =====================================================================
//  SHELL DRIVER
//  Owns the frame. Decides whether an app runs at all this frame, and
//  composites the shell chrome on top. Applications are untouched --
//  they still draw exactly as they always did, into `frame`.
// =====================================================================
static bool shSuppressApp=false;     // shell is covering the app entirely
static uint32_t shIdleMs=0;
#define LOCK_IDLE_MS 120000UL

// Long-press tracking for the launcher context menu
static uint32_t lpStart=0;
static int      lpIdx=-1;

// Which app index the current appState belongs to (for recents/labels)
static inline const char *CurAppName(void){
  int i=AppIndexForState((int)appState);
  return (i>=0)?APPS[i].name:"NEXUS";
}

// Runs BEFORE the app draws. Returns true when the app should be skipped.
bool ShellPreFrame(float dt){
  RecentsInit();
  if (touchActive||touchDown) shIdleMs=millis();

  // idle -> lock screen (never from an app that owns the screen)
  if (shMode==SH_HOME && millis()-shIdleMs>LOCK_IDLE_MS){
    shMode=SH_LOCK; lockT=0; }

  bool consumed=ShellGestures(dt);

  switch (shMode){
    case SH_OPENING:
      shT+=dt*4.4f;                      // ~230 ms
      if (shT>=1.0f){
        shT=0; shMode=SH_APP;
        shCurApp=shTargetApp;
        if (shTargetApp>=0){
          int st=APPS[shTargetApp].target;
          // Reuse the existing transition machinery so every per-app
          // reset (SandInit, FieldInit, frDirty, ...) still happens.
          transT=0;
          appState=(AppState)st;
          enterAnim=0;
          AchVisit(st);
          // per-app entry work, mirroring the legacy transition block
          if (st==ST_WARP&&accum) memset(accum,0,FB_BYTES);
          if (st==ST_CALIB) CalibReset();
          if (st==ST_MAZE)  ResetMaze();
          if (st==ST_WIFI)  wfMode=0;
          if (st==ST_PSAND){ SandInit(); if (accum) memset(accum,0,FB_BYTES); }
          if (st==ST_FIELD) FieldInit();
          if (st==ST_LIFE){ LifeAlloc(); if (lifeA&&!lfGen) LifeRandom(); }
          if (st==ST_FRACTAL){ frDirty=true; frRow=0; }
          if (st==ST_PHYS)  PhysReset(phMode);
          if (st==ST_ANIMLAB) AlReset();
          if (st==ST_GRAVWELL||st==ST_CREATOR){ if (accum) memset(accum,0,FB_BYTES); }
          if (st==ST_GESTURE) GestClear();
          if (st==ST_SEARCH)  SearchRun2();
          if (st==ST_TIMELINE) TimelineReset();
          BulletReset();
          // The finger may still be down from the launcher tap. Clear it
          // so the app's first frame starts from a clean input state.
          ShellConsumeTouch();
        }
        return false; }
      return true;                        // app suppressed during open
    case SH_CLOSING:
      shT+=dt*4.8f;
      if (shT>=1.0f){ shT=0; shMode=SH_HOME; shCurApp=-1;
                      appState=ST_HOME; enterAnim=0; }
      return true;
    case SH_SWITCHER:
    case SH_CONTROL:
    case SH_LOCK:
    case SH_HOME:
      return true;                        // shell owns the screen
    default:
      break; }
  return false;
}

// Runs AFTER the app draws. Composites chrome, handles shell modes.
void ShellPostFrame(float dt){
  switch (shMode){
    case SH_HOME: {
      DrawHomeScreen(dt);
      // --- launcher input ---
      if (touchDown&&touchY>SAFE_TOP&&touchY<APP_BOT-30){
        homeSwiping=false; homeSwipeX0=touchX; homeSwipeStart=shPageX;
        lpStart=millis(); lpIdx=homeHit; }
      if (touchActive&&!cmOpen){
        int dx=touchX-homeSwipeX0;
        if (abs(dx)>TOUCH_SLOP&&touchY<APP_BOT-30){ homeSwiping=true; lpStart=0; }
        if (lpStart&&lpIdx>=0&&homeHit==lpIdx&&millis()-lpStart>520){
          ContextOpen(lpIdx,(float)touchX,(float)touchY);
          lpStart=0;
          // hidden: long-pressing an icon 3 separate times fires an egg
          static uint8_t lpCount=0; static uint32_t lpLast=0;
          if (millis()-lpLast<4000) lpCount++; else lpCount=1;
          lpLast=millis();
          if (lpCount>=4){ lpCount=0; EggFire(EGG_GLITCH,2.6f,"REALITY DESYNC"); } } }
      if (touchUp){
        if (homeSwiping){
          int dx=touchX-homeSwipeX0;
          if (dx<-40&&shPage<HOME_PAGES-1) shPage++;
          else if (dx>40&&shPage>0) shPage--;
          homeSwiping=false;
        } else if (!cmOpen&&homeHit>=0&&lpStart){
          int hx,hy;
          { const int GX=3,GY=3, gridTop=SAFE_TOP+10;
            const int cellW=SCREEN_W/GX, cellH=(APP_BOT-DOCK_BAND-gridTop)/GY;
            int shown=0; hx=160; hy=120;
            for (int i=0;i<APP_N;i++){
              if (APPS[i].page!=shPage) continue;
              if (i==homeHit){ hx=(shown%GX)*cellW+cellW/2;
                               hy=gridTop+(shown/GX)*cellH+cellH/2-5; break; }
              shown++; } }
          ShellOpenApp(homeHit,(float)hx,(float)hy);
        } else if (!cmOpen){
          int d=DockHit();
          if (d>=0) ShellOpenApp(d,(float)touchX,(float)(APP_BOT-14)); }
        lpStart=0; lpIdx=-1; }
      DrawStatusBar(false);
      DrawHomeIndicator(0.35f);
    } break;

    case SH_OPENING:  DrawAppOpenAnim(clampf(shT,0,1),true);  break;
    case SH_CLOSING:  DrawAppOpenAnim(clampf(shT,0,1),false); break;

    case SH_SWITCHER: {
      DrawHomeScreen(dt);
      DrawAppSwitcher(dt);
      DrawStatusBar(true);
      // Three independent ways out, so this screen can never trap you:
      //   1. swipe up          (handled in ShellGestures)
      //   2. tap the status bar
      //   3. tap any empty space that is not a card
      // Exit is the deliberate edge gesture (see ShellGestures). Tapping
      // empty space no longer dismisses -- the release that OPENED this
      // screen was being counted as exactly that tap, so Recents closed
      // the instant it appeared. Status bar stays as a fallback.
      if (touchDown&&touchY<SAFE_TOP&&millis()-recEnterMs>250) shMode=SH_HOME;
      { const char *h="Swipe from edge to close";
        int hw=TextW(h,1);
        DrawText((SCREEN_W-hw)/2,SCREEN_H-SAFE_BOTTOM-10,h,C_OFF,1); }
    } break;

    case SH_CONTROL:
      DrawHomeScreen(dt);
      gsCtrl+=(1.0f-gsCtrl)*clampf(dt*12,0,1);
      DrawControlCenter(gsCtrl,dt);
      DrawStatusBar(true);
      // swipe up anywhere, or tap below the panel, closes
      if (touchDown&&touchY>APP_H*0.86f){ shMode=SH_HOME; gsCtrl=0; }
      break;

    case SH_LOCK:
      DrawLockScreen(dt);
      if (touchDown||shHomeDrag>0.25f){ shMode=SH_HOME; shIdleMs=millis(); }
      break;

    case SH_APP: {
      // app already drew this frame; add chrome on top
      if (gsCtrl>0.01f) DrawControlCenter(gsCtrl,dt);
      DrawStatusBar(true);
      DrawHomeIndicator(0.30f);
    } break;
  }
  // shared overlays, drawn above everything
  int cm=ContextDraw(dt);
  if (cm==1&&cmApp>=0) ShellOpenApp(cmApp,cmX,cmY);
  else if (cm==2&&cmApp>=0) UiToast(APPS[cmApp].name,AppTint(APPS[cmApp].hue));
  SheetDraw(dt);
  ShellGestureOverlay();
  UiToastDraw(dt);
}


// =====================================================================
//  LOOP
// =====================================================================
static void StepAnimation(float fdt){
  UpdateParticles(fdt);
  for (int i=0;i<NUM_DUST;i++){
    Dust &d=dust[i];
    d.y-=(6.0f+d.z*20.0f)*fdt;
    d.x+=fsin(gTime*0.4f+d.ph)*8.0f*d.z*fdt;
    if (d.y<-2){ d.y=SCREEN_H+2; d.x=Hash((uint32_t)(gTime*97)+i)*SCREEN_W; }
    if (d.x<-2) d.x=SCREEN_W+2;
    if (d.x>SCREEN_W+2) d.x=-2; }
}

void loop(){
  static bool v7FirstFrame=true;
  if (v7FirstFrame){ v7FirstFrame=false; AchGrant(A_BOOT); ssIdleMs=millis(); }
  static uint32_t lastUs=micros();
  uint32_t nowUs=micros();
  float raw=(nowUs-lastUs)*1e-6f;
  lastUs=nowUs;
  float dt=FilterDt(raw);
  gTime+=dt;

  // ---- DISPLAY ASLEEP: no rendering, no animation, no frame push ----
  // IMPORTANT: do NOT call esp_light_sleep_start() here.
  // On ESP32-S3 + USB-CDC, light sleep tears down the USB serial stack
  // (Serial goes blank), and with WiFi connected it is a known reboot
  // source when the radio is not put into wifi-aware sleep first.
  // Display sleep is a PANEL power state, not a CPU sleep state.
  // We idle the render loop, poll touch for wake, and keep NetLoop alive.
  if (displaySleeping){
    // Service WiFi/NTP so the link does not drop while the panel is dark.
    NetLoop();

    // Wake on a NEW touch, not the finger that pressed power.
    // CST328 INT is active-LOW while a finger is down. We require:
    //   1) at least one sample with no contact (released), then
    //   2) a later contact after a short guard time.
    // Polling the controller directly avoids depending on gpio wake.
    uint16_t rx=0, ry=0;
    bool down = ReadRawTouch(rx, ry);
    if (!down) {
      dispTouchReleased = true;
    } else if (dispTouchReleased && (millis() - dispSleepMs > 400)) {
      DisplayWake();
      // fall through so the current screen is redrawn this frame
      lastUs = micros();
    }

    if (displaySleeping){
      // ~20 Hz idle. Cheap, keeps USB-CDC and WiFi happy, no light sleep.
      delay(50);
      return;
    }
  }

  PollTouch();
  // ---- system-tray claim -------------------------------------------
  // The tray sits inside the app's BACK hitbox. Apps render before the
  // shell composites its chrome, so without this the BACK button eats
  // the power tap and closes the app instead of sleeping the display.
  if (touchDown && touchY<SAFE_TOP){
    // Power sits left on HOME (no app header) and right inside an app,
    // where no legacy screen places a control.
    const int px = (shMode==SH_APP) ? (SCREEN_W-M-8) : (M+34);
    if (touchX>=px-11 && touchX<px+11){
      DisplaySleep();
      touchDown=false; touchUp=false; touchActive=false;
      pressX=-1000; pressY=-1000;
      return; } }
  // The tap that woke the panel must not also press whatever sits under
  // the finger. Consume exactly one touch stream, then behave normally.
  if (dispEatTouch){
    if (touchActive||touchDown){ touchDown=false; touchUp=false;
                                 touchActive=false;
                                 pressX=-1000; pressY=-1000; }
    else dispEatTouch=false; }
  NetLoop();
  if (JB) JeeTick(dt);
  EggDetect(dt);
  AchTick(dt);
  ImpactTick(dt);
  // screensaver: only from idle screens, never mid-transition or in an app
  // that owns the whole screen. Any touch exits (handled in ScreenSaver).
  if (touchActive||touchDown) ssIdleMs=millis();
  else if (appState!=ST_SAVER && millis()-ssIdleMs>SS_IDLE_MS &&
           (appState==ST_HOME||appState==ST_CLOCK||appState==ST_CLOCKS||
            appState==ST_SYSTEM||appState==ST_ACHIEVE))
    SaverEnter();

  animAccum+=dt;
  int steps=0;
  while (animAccum>=FIXED_DT&&steps<8){ StepAnimation(FIXED_DT); animAccum-=FIXED_DT; steps++; }
  if (animAccum>0.25f) animAccum=0;

  fbIndex^=1;
  frame=fb[fbIndex];

  // ---- MOBILE SHELL: may suppress the app entirely this frame ----
  bool shellOwns=ShellPreFrame(dt);

  if (transT>0){
    float prev=transT;
    transT+=dt*2.6f;
    if (prev<1.0f&&transT>=1.0f){
      appState=(AppState)transTarget;
      enterAnim=0;
      rotX=0.35f; rotY=0; velX=0; velY=0.35f;
      touchActive=false;
      drawPrev=false;
      if (appState==ST_WARP) memset(accum,0,FB_BYTES);
      if (appState==ST_CALIB) CalibReset();
      if (appState==ST_MAZE)  ResetMaze();
      if (appState==ST_WIFI)  wfMode=0;
      if (appState==ST_PSAND){ SandInit(); if (accum) memset(accum,0,FB_BYTES); }
      if (appState==ST_FIELD) FieldInit();
      if (appState==ST_LIFE){ LifeAlloc(); if (lifeA&&!lfGen) LifeRandom(); }
      if (appState==ST_FRACTAL){ frDirty=true; frRow=0; }
      if (appState==ST_PHYS)  PhysReset(phMode);
      if (appState==ST_ANIMLAB) AlReset();
      // ---- v7 entry resets ----
      BulletReset();
      ssIdleMs=millis();
      if (appState==ST_GRAVWELL||appState==ST_CREATOR)
        { if (accum) memset(accum,0,FB_BYTES); }
      if (appState==ST_GESTURE)  GestClear();
      if (appState==ST_SEARCH)   SearchRun2();
      if (appState==ST_TIMELINE) TimelineReset();
      if (appState==ST_JTASKS||appState==ST_JNOTES||appState==ST_JHIST){
        jeeScroll=0; jeeScrollV=0; jeeDrag=false; }
      if (appState==ST_JTASKS) jeeTaskMenu=-1;
      if (appState==ST_JNOTES) jeeNoteSel=-1;
      jeeQuick=0;
      SpawnBurst(transX,transY,18,transCol,190.0f,PK_SPARK); }
    if (transT>=2.0f) transT=0; }

  if (!shellOwns)
  switch (appState){
    case ST_HOME:     ScreenHomeV8(dt);    break;
    case ST_LAB:      ScreenLab(dt);       break;
    case ST_OBJECTS:  ScreenObjects(dt);   break;
    case ST_MODES:    ScreenModes(dt);     break;
    case ST_INSPECT:  ScreenInspect(dt);   break;
    case ST_WARP:     ScreenWarp(dt);      break;
    case ST_MAZE:     ScreenMaze(dt);      break;
    case ST_2048:     Screen2048(dt);      break;
    case ST_BREAK:    ScreenBreak(dt);     break;
    case ST_FLAPPY:   ScreenFlappy(dt);    break;
    case ST_SNAKE:    ScreenSnake(dt);     break;
    case ST_PONG:     ScreenPong(dt);      break;
    case ST_TETRIS:   ScreenTetris(dt);    break;
    case ST_MEMORY:   ScreenMemory(dt);    break;
    case ST_SIMON:    ScreenSimon(dt);     break;
    case ST_MINES:    ScreenMines(dt);     break;
    case ST_WHACK:    ScreenWhack(dt);     break;
    case ST_DODGE:    ScreenDodge(dt);     break;
    case ST_LIGHTS:   ScreenLights(dt);    break;
    case ST_DRAW:     ScreenDraw(dt);      break;
    case ST_CLOCK:    ScreenClock(dt);     break;
    case ST_STOPW:    ScreenStopwatch(dt); break;
    case ST_TIMER:    ScreenTimer(dt);     break;
    case ST_WIFI:     ScreenWifi(dt);      break;
    case ST_SETTINGS: ScreenSettings(dt);  break;
    case ST_CALIB:    ScreenCalib(dt);     break;
    case ST_SYSTEM:   ScreenSystem(dt);    break;
    case ST_JEE:      ScreenJee(dt);       break;
    case ST_JTIMER:   ScreenJeeTimer(dt);  break;
    case ST_JTASKS:   ScreenJeeTasks(dt);  break;
    case ST_JGOALS:   ScreenJeeGoals(dt);  break;
    case ST_JSTATS:   ScreenJeeStats(dt);  break;
    case ST_JHIST:    ScreenJeeHist(dt);   break;
    case ST_JNOTES:   ScreenJeeNotes(dt);  break;
    case ST_JQUOTE:   ScreenJeeQuote(dt);  break;
    case ST_JSET:     ScreenJeeSet(dt);    break;
    case ST_KBD:      ScreenKeyboard(dt);  break;
    case ST_PHYS:     ScreenPhysics(dt);   break;
    case ST_PSAND:    ScreenSand(dt);      break;
    case ST_SPACE:    ScreenSpace(dt);     break;
    case ST_PLANETGEN:ScreenPlanetGen(dt); break;
    case ST_FRACTAL:  ScreenFractal(dt);   break;
    case ST_MATRIX:   ScreenMatrix(dt);    break;
    case ST_FIELD:    ScreenField(dt);     break;
    case ST_TOUCHPLAY:ScreenTouchPlay(dt); break;
    case ST_LIFE:     ScreenLife(dt);      break;
    case ST_MOLECULE: ScreenMolecule(dt);  break;
    case ST_ANIMLAB:  ScreenAnimLab(dt);   break;
    case ST_DEMO:     ScreenDemo(dt);      break;
    // ---- v7 ----
    case ST_SEARCH:    ScreenSearch(dt);      break;
    case ST_CREATOR:   ScreenCreator(dt);     break;
    case ST_GESTURE:   ScreenGesture(dt);     break;
    case ST_MORPH:     ScreenMorph(dt);       break;
    case ST_KALEIDO:   ScreenKaleido(dt);     break;
    case ST_EXPLODE:   ScreenExplode(dt);     break;
    case ST_VOXEL:     ScreenVoxel(dt);       break;
    case ST_IMPOSSIBLE:ScreenImpossible(dt);  break;
    case ST_TUNNEL:    ScreenTunnel(dt);      break;
    case ST_GRAVWELL:  ScreenGravWell(dt);    break;
    case ST_BOIDS:     ScreenBoids(dt);       break;
    case ST_AQUARIUM:  ScreenAquarium(dt);    break;
    case ST_ANTS:      ScreenAnts(dt);        break;
    case ST_CHARGES:   ScreenCharges(dt);     break;
    case ST_RAGDOLL:   ScreenRagdoll(dt);     break;
    case ST_TIMELINE:  ScreenTimeline(dt);    break;
    case ST_GRAPHER:   ScreenGrapher(dt);     break;
    case ST_PARAMETRIC:ScreenParametric(dt);  break;
    case ST_SURFACE:   ScreenSurface(dt);     break;
    case ST_VECTORS:   ScreenVectors(dt);     break;
    case ST_MATRIXVIZ: ScreenMatrixViz(dt);   break;
    case ST_FOURIER:   ScreenFourier(dt);     break;
    case ST_CLOCKS:    ScreenClocks(dt);      break;
    case ST_ACHIEVE:   ScreenAchieve(dt);     break;
    case ST_DEVROOM:   ScreenDevRoom(dt);     break;
    case ST_SAVER:     ScreenSaver(dt);       break;
    default:          appState=ST_HOME;    break; }

  // ---- MOBILE SHELL: chrome, launcher, gestures, sheets ----
  ShellPostFrame(dt);

  // The tray power button may have slept the panel partway through this
  // frame. Do not push pixels to a display that is already in SLPIN.
  if (displaySleeping) return;

  EggRender(dt);
  EggToast(dt);
  DemoOverlay(dt);
  if (transT>0) TransitionDraw(transT);
  PushFrame();

  frameCount++;
  uint32_t nowMs=millis();
  if (nowMs-fpsTimer>=250){
    fpsValue=frameCount*1000.0f/(float)(nowMs-fpsTimer);
    frameMs=(fpsValue>0.1f)?(1000.0f/fpsValue):0;
    frameCount=0; fpsTimer=nowMs;
    snprintf(fpsStr,sizeof(fpsStr),"%d",(int)(fpsValue+0.5f));
    // adaptive detail keeps interaction smooth if we dip under target
    // Adaptive detail. It must not fight an explicit user choice: if the
    // user picked a Quality preset in the Control Center, only the
    // emergency low-FPS clamp may engage, and it may never re-enable
    // detail the user turned off.
    if (fpsValue<18.0f) gLowDetail=true;
    else if (fpsValue>24.0f && ccPerf!=0) gLowDetail=false;
    histHead=(histHead+1)%GRAPH_LEN;
    histFps[histHead]=(uint8_t)clampf(fpsValue*4.2f,0,255);
    histFrame[histHead]=(uint8_t)clampf(frameMs*3.4f,0,255);
    float ps=1.0f-(float)ESP.getFreePsram()/(float)ESP.getPsramSize();
    histLoad[histHead]=(uint8_t)clampf(ps*255.0f,0,255);
    static uint8_t div=0;
    if (++div>=20){ div=0;
      Serial.printf("screen %d | %.1f FPS | %.1f ms | free PSRAM %u\n",
        (int)appState,fpsValue,frameMs,(unsigned)ESP.getFreePsram()); } }
}
