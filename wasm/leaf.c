/* 求解器葉節點(剩 1 步)的計算核心,與 index.html 引擎裡 leafVal 的 JS 版逐行對應、結果逐位相同。
 * 編譯(產生 leaf.wasm 與要貼進 index.html 的 base64):見 wasm/build.sh
 * 這裡不用 libc,所有狀態都是靜態變數;JS 端透過匯出的位址讀寫盤面與機率。 */
#include <stdint.h>
typedef int32_t i32;
typedef uint32_t u32;
typedef int8_t i8;
typedef uint8_t u8;

#define N 5
#define CELLS 25
#define K 5
#define EXPORT(n) __attribute__((export_name(n)))

/* 橫向三連起點只能在第 0~2 欄;NC0/NC4:非第 0 欄/非第 4 欄的格(避免位移時跨列)。每列 5 格,5 列 */
#define ROWS_ONE 1082401 /* 1 + 2^5 + 2^10 + 2^15 + 2^20 */
#define HST (7 * ROWS_ONE)
#define NC0 (30 * ROWS_ONE)
#define NC4 (15 * ROWS_ONE)

static u8 board[28];       /* 輸入盤面(每格 0~4) */
static double weights[K];  /* 各顏色機率 */
static double cumw[K];     /* 累積機率 */
static i32 cfg[4];         /* [0]=R [1]=IGN [2]=UNI [3]=保留 */

static i32 COLB[CELLS], COLM[N], MOVEDM[CELLS], NWM[CELLS];
static i8 AFFN[CELLS][64], AFFT2[CELLS][8];
static i32 NAFFN[CELLS], NAFFT2[CELLS];

EXPORT("board_ptr") i32 board_ptr(void) { return (i32)(intptr_t)board; }
EXPORT("weights_ptr") i32 weights_ptr(void) { return (i32)(intptr_t)weights; }
EXPORT("cum_ptr") i32 cum_ptr(void) { return (i32)(intptr_t)cumw; }
EXPORT("cfg_ptr") i32 cfg_ptr(void) { return (i32)(intptr_t)cfg; }

EXPORT("init") void init(void) {
  for (int i = 0; i < CELLS; i++) COLB[i] = 1 << (i % N);
  for (int c = 0; c < N; c++) {
    COLM[c] = 0;
    for (int r = 0; r < N; r++) COLM[c] |= 1 << (r * N + c);
  }
  for (int idx = 0; idx < CELLS; idx++) {
    int col = idx % N, row = idx / N, m = 0;
    for (int r = 0; r <= row; r++) m |= 1 << (r * N + col);
    MOVEDM[idx] = m;
    NWM[idx] = 1 << col;
    int nn = 0, nt = 0;
    /* 與 JS 的 AFFN/AFFT 相同:點下 idx 後可能新形成三連的線(經過同欄、該列以上的格子) */
    for (int pass = 0; pass < 2; pass++) {
      for (int a0 = 0; a0 < CELLS; a0++) {
        int r = a0 / N, c = a0 % N;
        int tri[3];
        if (pass == 0) {
          if (c > N - 3) continue;
          tri[0] = a0; tri[1] = a0 + 1; tri[2] = a0 + 2;
        } else {
          if (r > N - 3) continue;
          tri[0] = a0; tri[1] = a0 + N; tri[2] = a0 + 2 * N;
        }
        int touch = 0, hasCol = 0;
        for (int j = 0; j < 3; j++) {
          if (tri[j] % N == col && tri[j] / N <= row) touch = 1;
          if (tri[j] == col) hasCol = 1;
        }
        if (!touch) continue;
        if (hasCol) {
          for (int j = 0; j < 3; j++)
            if (tri[j] != col) AFFT2[idx][nt++] = (i8)tri[j];
        } else {
          for (int j = 0; j < 3; j++) AFFN[idx][nn++] = (i8)tri[j];
        }
      }
    }
    NAFFN[idx] = nn;
    NAFFT2[idx] = nt;
  }
}

static i32 rs;
static i32 CM[5];
static i32 clrCols;

static inline u32 rnd32(void) {
  rs = (i32)((u32)rs + 0x6D2B79F5u);
  u32 r = (u32)rs;
  u32 t = (r ^ (r >> 15)) * (1u | r);
  t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
  return t ^ (t >> 14);
}
static inline i32 draw(void) {
  u32 k = rnd32();
  if (cfg[2]) return (i32)((double)k * K / 4294967296.0);
  double r = (double)k / 4294967296.0;
  for (int i = 0; i < K; i++)
    if (r < cumw[i]) return i;
  return K - 1;
}

/* 單一顏色 m 的消除貢獻(橫向/直向三連展開成整段,段必須含位移格 mv、且(IGN 時)含舊牌 old) */
static inline i32 colorMk(i32 m, i32 mv, i32 old) {
  i32 mk = 0;
  i32 hs = m & (m >> 1) & (m >> 2) & HST, vs = m & (m >> N) & (m >> (2 * N));
  if (hs) {
    i32 th = hs | (i32)((u32)hs << 1) | (i32)((u32)hs << 2);
    i32 x = th & mv, y = th & old;
    if (x && y) {
      for (int it = 0; it < 4; it++) {
        x |= (((i32)((u32)x << 1) & NC0) | ((x >> 1) & NC4)) & th;
        y |= (((i32)((u32)y << 1) & NC0) | ((y >> 1) & NC4)) & th;
      }
      mk |= x & y;
    }
  }
  if (vs) {
    i32 tv = vs | (i32)((u32)vs << N) | (i32)((u32)vs << (2 * N));
    i32 x = tv & mv, y = tv & old;
    if (x && y) {
      for (int it = 0; it < 4; it++) {
        x |= ((i32)((u32)x << N) | (x >> N)) & tv;
        y |= ((i32)((u32)y << N) | (y >> N)) & tv;
      }
      mk |= x & y;
    }
  }
  return mk;
}
/* CM 已是盤面的顏色遮罩;回傳要消除的格(位元),並同步從 CM 移除 */
static inline i32 clearMk(i32 mv, i32 nwm) {
  i32 old = cfg[1] ? ~nwm : -1;
  i32 mk = colorMk(CM[0], mv, old) | colorMk(CM[1], mv, old) | colorMk(CM[2], mv, old) | colorMk(CM[3], mv, old) | colorMk(CM[4], mv, old);
  if (mk == 0) return 0;
  i32 keep = ~mk;
  CM[0] &= keep; CM[1] &= keep; CM[2] &= keep; CM[3] &= keep; CM[4] &= keep;
  return mk;
}
static inline i32 applyMk(i8 *b, i32 mk) {
  i32 total = 0, cols = 0;
  for (u32 m = (u32)mk; m != 0; m &= m - 1) {
    int i = __builtin_ctz(m);
    b[i] = -1; total++; cols |= COLB[i];
  }
  clrCols = cols;
  return total;
}

/* 已完成第一輪消除:繼續做完後面的重力、補牌與連鎖,回傳總消除張數。
 * 遮罩更新:被消除的格已從 CM 移除;下移的牌只需 XOR 一次(清掉舊位、設上新位),沒位移的欄完全不必動遮罩 */
static i32 cascadeRest(i8 *b, i32 ac, i32 removed) {
  const i32 uni = cfg[2];
  u32 rr = (u32)rs;
  for (;;) {
    i32 mv = 0, nwm = 0;
    for (int c = 0; c < N; c++) {
      if (!((ac >> c) & 1)) continue;
      int w = N - 1;
      for (int r = N - 1; r >= 0; r--) {
        i32 v = b[r * N + c];
        if (v >= 0) {
          if (w != r) {
            int q = w * N + c;
            b[q] = (i8)v; mv |= 1 << q;
            CM[v] ^= (1 << (r * N + c)) | (1 << q);
          }
          w--;
        }
      }
      for (int r = w; r >= 0; r--) {
        int q = r * N + c;
        rr += 0x6D2B79F5u;
        u32 u = (rr ^ (rr >> 15)) * (1u | rr);
        u = (u + ((u ^ (u >> 7)) * (61u | u))) ^ u;
        u32 kk = u ^ (u >> 14);
        i32 v;
        if (uni) v = (i32)((double)kk * K / 4294967296.0);
        else { double f = (double)kk / 4294967296.0; v = K - 1; for (int i = 0; i < K; i++) if (f < cumw[i]) { v = i; break; } }
        b[q] = (i8)v; CM[v] |= 1 << q; mv |= 1 << q; nwm |= 1 << q;
      }
    }
    i32 mk = clearMk(mv, nwm);
    if (!mk) break;
    removed += applyMk(b, mk);
    ac = clrCols;
  }
  rs = (i32)rr;
  return removed;
}

/* 與 JS leafVal 的計算部分相同:sd = seedOf(hi,lo,1),回傳「最佳一步的期望消除數」 */
EXPORT("leaf") double leaf(i32 sd) {
  const i32 R = cfg[0];
  const i32 uni = cfg[2];
  i8 bb[32], b3[32], b4[32];
  i8 CN[CELLS];
  double QA[CELLS];
  i32 BM[5] = {0, 0, 0, 0, 0}, SB[5];
  i8 NP[CELLS], NV[CELLS], SNP[CELLS], SB4[32];
  double best = 0;
  for (int i = 0; i < 32; i++) { bb[i] = i < 25 ? (i8)board[i] : 0; b3[i] = 0; b4[i] = 0; }
  for (int i = 0; i < CELLS; i++) BM[board[i]] |= 1 << i;
  for (int a = 0; a < CELLS; a++) {
    const int c = a % N;
    int cn = a;
    if (a >= N && board[a] == board[a - N]) {
      cn = CN[a - N];
      int v = board[a], rb = a - c, l = c, r = c;
      while (l > 0 && board[rb + l - 1] == v) l--;
      while (r < N - 1 && board[rb + r + 1] == v) r++;
      if (r - l < 2 && !(a + 2 * N < CELLS && board[a + N] == v && board[a + 2 * N] == v)) {
        CN[a] = (i8)cn; QA[a] = QA[a - N]; continue;
      }
    }
    CN[a] = (i8)cn;
    rs = (i32)((u32)sd ^ ((u32)(cn + 1) * 0x27D4EB2Fu));
    for (int i = a; i >= N; i -= N) bb[i] = bb[i - N];
    int baseHit = 0;
    for (int j = 0; j < NAFFN[a]; j += 3) {
      i8 x = bb[(int)AFFN[a][j]];
      if (x == bb[(int)AFFN[a][j + 1]] && x == bb[(int)AFFN[a][j + 2]]) { baseHit = 1; break; }
    }
    double q = 0;
    int sb = 0, hm = 31, tm = 0;
    for (int j = 0; j < NAFFT2[a]; j += 2) { /* 補入哪些顏色會在頂端新牌處成線 */
      i8 x = bb[(int)AFFT2[a][j]];
      if (x == bb[(int)AFFT2[a][j + 1]]) tm |= 1 << x;
    }
    if (!baseHit) hm = tm;
    /* 頂端新牌不在任何三連裡的顏色(d 不在 tm):第一輪消除的集合、壓實與補牌位置都和 d 無關,只有頂端那張牌的顏色不同,
     * 所以第一個這種 d 算完整,之後的 d 直接沿用、只把那張牌改色 */
    int shv = 0, shd = 0, tp = 0, cur_sh = 0; /* shv:已有可共用的狀態;cur_sh:目前的 c0~c4、mv、b4... 是不是共用狀態 */
    i32 sn1 = 0, smv = 0, snwm = 0, sc0 = 0, sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0, stp = 0;
    int snn = 0;
    i32 mv = 0, nwm = 0, ac = 0, c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0;
    int nn = 0;
    for (int d = 0; d < K; d++) {
      double p = weights[d];
      if (!(p > 0)) continue;
      if (!((hm >> d) & 1)) { q += p; continue; }
      bb[c] = (i8)d;
      i32 n1;
      if (shv && !((tm >> d) & 1)) {
        n1 = sn1;
        if (n1) {
          if (!cur_sh) { /* 中間算過別的顏色:把共用狀態還原回來(頂端新牌顏色為 shd) */
            c0 = sc0; c1 = sc1; c2 = sc2; c3 = sc3; c4 = sc4; mv = smv; nwm = snwm; nn = snn; tp = stp;
            __builtin_memcpy(b4, SB4, 32);
            for (int i = 0; i < nn; i++) NP[i] = SNP[i];
            cur_sh = 1;
          }
          const i32 tb = 1 << tp;
          if (shd == 0) c0 &= ~tb; else if (shd == 1) c1 &= ~tb; else if (shd == 2) c2 &= ~tb; else if (shd == 3) c3 &= ~tb; else c4 &= ~tb;
          if (d == 0) c0 |= tb; else if (d == 1) c1 |= tb; else if (d == 2) c2 |= tb; else if (d == 3) c3 |= tb; else c4 |= tb;
          b4[tp] = (i8)d;
          sc0 = c0; sc1 = c1; sc2 = c2; sc3 = c3; sc4 = c4; SB4[tp] = (i8)d;
        }
        shd = d;
      } else {
        if (!sb) {
          i32 mm = MOVEDM[a], nm = ~mm;
          for (int k = 0; k < 5; k++) { i32 m = BM[k]; SB[k] = (m & nm) | (((i32)((u32)(m & mm) << N)) & mm); }
          sb = 1;
        }
        for (int k = 0; k < 5; k++) CM[k] = SB[k];
        CM[d] |= 1 << c;
        __builtin_memcpy(b3, bb, 32);
        i32 mk0 = clearMk(MOVEDM[a], NWM[a]);
        n1 = mk0 ? applyMk(b3, mk0) : 0;
        if (n1) {
          ac = clrCols; mv = 0; nwm = 0; nn = 0; tp = c;
          for (int cc = 0; cc < N; cc++) {
            if (!((ac >> cc) & 1)) continue;
            int w = N - 1;
            for (int r = N - 1; r >= 0; r--) {
              i32 v = b3[r * N + cc];
              if (v >= 0) {
                int qq = w * N + cc;
                if (w != r) { b3[qq] = (i8)v; mv |= 1 << qq; CM[v] ^= (1 << (r * N + cc)) | (1 << qq); }
                if (cc == c && r == 0) tp = qq; /* 頂端新牌壓實後的位置 */
                w--;
              }
            }
            for (int r = w; r >= 0; r--) { int qq = r * N + cc; NP[nn++] = (i8)qq; mv |= 1 << qq; nwm |= 1 << qq; }
          }
          __builtin_memcpy(b4, b3, 32);
          c0 = CM[0]; c1 = CM[1]; c2 = CM[2]; c3 = CM[3]; c4 = CM[4];
        }
        if (!((tm >> d) & 1)) { /* 存成共用狀態 */
          shv = 1; sn1 = n1; shd = d; cur_sh = 1;
          if (n1) {
            sc0 = c0; sc1 = c1; sc2 = c2; sc3 = c3; sc4 = c4; smv = mv; snwm = nwm; snn = nn; stp = tp;
            __builtin_memcpy(SB4, b4, 32);
            for (int i = 0; i < nn; i++) SNP[i] = NP[i];
          }
        } else cur_sh = 0;
      }
      i32 s = 0;
      if (n1) {
        for (int k = 0; k < R; k++) {
          CM[0] = c0; CM[1] = c1; CM[2] = c2; CM[3] = c3; CM[4] = c4;
          { /* 抽牌用區域變數(與 draw() 相同的公式),避免每張都讀寫全域 rs */
            u32 r = (u32)rs;
            for (int t = 0; t < nn; t++) {
              r += 0x6D2B79F5u;
              u32 u = (r ^ (r >> 15)) * (1u | r);
              u = (u + ((u ^ (u >> 7)) * (61u | u))) ^ u;
              u32 kk = u ^ (u >> 14);
              i32 v;
              if (uni) v = (i32)((double)kk * K / 4294967296.0);
              else { double f = (double)kk / 4294967296.0; v = K - 1; for (int i = 0; i < K; i++) if (f < cumw[i]) { v = i; break; } }
              NV[t] = (i8)v; CM[v] |= 1 << NP[t];
            }
            rs = (i32)r;
          }
          i32 mk = clearMk(mv, nwm);
          if (mk == 0) { s += n1; continue; }
          __builtin_memcpy(b3, b4, 32);
          for (int t = 0; t < nn; t++) b3[NP[t]] = NV[t];
          i32 n2 = applyMk(b3, mk);
          s += cascadeRest(b3, clrCols, n1 + n2);
        }
      }
      q += p * (1 + (double)s / (double)R);
    }
    for (int i = c; i <= a; i += N) bb[i] = (i8)board[i];
    QA[a] = q;
    if (q > best) best = q;
  }
  return best;
}

/* ======== 前緣(剩 2 步)盤面的計算:與 JS 的 evalBoards → vals(H=2) → expand/chain/addK 逐行對應 ========
 * 每個前緣盤面:rs 以盤面編碼播種,依序對 25 個點擊 expand(補入顏色×連鎖取樣 R 次、相同結果合併成一筆,
 * 合併與累加順序同 JS),每筆結果的葉節點值取自快取(沒有就用 leaf 算)。 */
#define BATCH 1024
static u8 inbuf[BATCH * 25];
static double outbuf[BATCH * 2];
EXPORT("inbuf_ptr") i32 inbuf_ptr(void) { return (i32)(intptr_t)inbuf; }
EXPORT("outbuf_ptr") i32 outbuf_ptr(void) { return (i32)(intptr_t)outbuf; }

#define LC_BITS 19
#define LC_M ((1 << LC_BITS) - 1)
static i32 LCH[1 << LC_BITS], LCL[1 << LC_BITS];
static double LCV[1 << LC_BITS];
static int lc_used = 0;
EXPORT("clear_cache") void clear_cache(void) {
  if (!lc_used) return;
  for (int i = 0; i <= LC_M; i++) LCH[i] = 0;
  lc_used = 0;
}

static inline i32 seedOf(i32 hi, i32 lo, i32 H) {
  u32 t = (u32)hi ^ ((u32)lo * 0x85EBCA6Bu);
  t = t * 0x9E3779B1u + (u32)H * 0x632BE5ABu;
  return (i32)t;
}

static void buildCM(const i8 *b) {
  CM[0] = 0; CM[1] = 0; CM[2] = 0; CM[3] = 0; CM[4] = 0;
  for (int i = 0; i < CELLS; i++) CM[b[i]] |= 1 << i;
}
/* 對應 JS cascade:回傳總消除張數,*cr = 這次是否有補過新牌 */
static i32 cascadeFull(i8 *b, i32 mv, i32 nwm, int *cr) {
  buildCM(b);
  i32 mk = clearMk(mv, nwm);
  if (!mk) { *cr = 0; return 0; }
  i32 n = applyMk(b, mk);
  *cr = 1;
  return cascadeRest(b, clrCols, n);
}

typedef struct { double p; i32 removed, hi, lo; i8 b[32]; } Out;
static Out outs[64];
static int nout;
static void addOut(double p, const i8 *bd, i32 rm) {
  i32 hi = 0, lo = 0;
  for (int i = 0; i < 12; i++) hi = hi * 5 + bd[i];
  for (int i = 12; i < CELLS; i++) lo = lo * 5 + bd[i];
  for (int i = 0; i < nout; i++)
    if (outs[i].lo == lo && outs[i].hi == hi && outs[i].removed == rm) { outs[i].p += p; return; }
  Out *o = &outs[nout++];
  o->p = p; o->removed = rm; o->hi = hi; o->lo = lo;
  for (int i = 0; i < 32; i++) o->b[i] = i < CELLS ? bd[i] : 0;
}
static void expandAt(const u8 *brd, int idx) {
  const i32 R = cfg[0];
  i8 bb[32], b2[32], b3[32];
  for (int i = 0; i < 32; i++) bb[i] = i < CELLS ? (i8)brd[i] : 0;
  const int c = idx % N;
  for (int i = idx; i >= N; i -= N) bb[i] = bb[i - N];
  nout = 0;
  int baseHit = 0;
  for (int j = 0; j < NAFFN[idx]; j += 3) {
    i8 x = bb[(int)AFFN[idx][j]];
    if (x == bb[(int)AFFN[idx][j + 1]] && x == bb[(int)AFFN[idx][j + 2]]) { baseHit = 1; break; }
  }
  for (int d = 0; d < K; d++) {
    double p = weights[d];
    if (!(p > 0)) continue;
    bb[c] = (i8)d;
    int hit = baseHit;
    if (!hit)
      for (int j = 0; j < NAFFT2[idx]; j += 2)
        if (bb[(int)AFFT2[idx][j]] == d && bb[(int)AFFT2[idx][j + 1]] == d) { hit = 1; break; }
    if (!hit) { addOut(p, bb, 1); continue; }
    /* chain:補入後有連鎖,以補牌後的盤面跑完整 cascade(R 次取樣) */
    for (int i = 0; i < 32; i++) { b2[i] = bb[i]; b3[i] = bb[i]; }
    int cr;
    i32 rm = cascadeFull(b3, MOVEDM[idx], NWM[idx], &cr);
    if (!cr) { addOut(p, b3, 1 + rm); continue; }
    addOut(p / (double)R, b3, 1 + rm);
    for (int k = 1; k < R; k++) {
      for (int i = 0; i < 32; i++) b3[i] = b2[i];
      rm = cascadeFull(b3, MOVEDM[idx], NWM[idx], &cr);
      addOut(p / (double)R, b3, 1 + rm);
    }
  }
}
static double leafCached(const Out *o) {
  u32 h = (u32)o->hi * 0x9E3779B1u ^ (u32)o->lo * 0x85EBCA6Bu;
  h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 13;
  const int i = (int)(h & LC_M);
  if (LCH[i] == o->hi + 1 && LCL[i] == o->lo) return LCV[i];
  for (int j = 0; j < 25; j++) board[j] = (u8)o->b[j];
  const i32 saved = rs;
  double v = leaf(seedOf(o->hi, o->lo, 1));
  rs = saved;
  LCH[i] = o->hi + 1; LCL[i] = o->lo; LCV[i] = v; lc_used = 1;
  return v;
}
/* inbuf 裡的 n 個前緣盤面(每盤 25 格),結果寫進 outbuf[2*i]=連點 1 步、[2*i+1]=連點 2 步的最大期望 */
EXPORT("eval2") void eval2(i32 n) {
  for (int bi = 0; bi < n; bi++) {
    const u8 *brd = inbuf + bi * 25;
    i32 hi = 0, lo = 0;
    for (int i = 0; i < 12; i++) hi = hi * 5 + brd[i];
    for (int i = 12; i < CELLS; i++) lo = lo * 5 + brd[i];
    rs = seedOf(hi, lo, 2);
    double best1 = 0, best2 = 0;
    for (int a = 0; a < CELLS; a++) {
      expandAt(brd, a);
      double q1 = 0, q2 = 0;
      for (int i = 0; i < nout; i++) {
        const Out *o = &outs[i];
        q1 += o->p * o->removed;
        q2 += o->p * (o->removed + leafCached(o));
      }
      if (q1 > best1) best1 = q1;
      if (q2 > best2) best2 = q2;
    }
    outbuf[2 * bi] = best1;
    outbuf[2 * bi + 1] = best2;
  }
}
