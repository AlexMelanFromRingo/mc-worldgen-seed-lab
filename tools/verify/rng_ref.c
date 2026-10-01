// Референсный C-порт RNG Minecraft 26.x (одинаков для 26.1/26.2/26.3, проверен против RngDump.java).
// gcc -O2 -o rng_ref rng_ref.c -lm && ./rng_ref > rng-c.txt ; сравнить с tools/verify/run_rngdump.sh V
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef uint64_t u64; typedef int64_t i64; typedef uint32_t u32; typedef int32_t i32;

/* ---------- MD5 (RFC 1321), нужен для fromHashOf ---------- */
static void md5(const unsigned char *msg, size_t len, unsigned char out[16]) {
    static const u32 K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int R[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    u32 a0=0x67452301,b0=0xefcdab89,c0=0x98badcfe,d0=0x10325476;
    unsigned char buf[128+64]; size_t nb=((len+8)/64+1)*64; if(nb>sizeof buf) { nb=0; }
    memset(buf,0,sizeof buf); memcpy(buf,msg,len); buf[len]=0x80; u64 bits=(u64)len*8; for(int i=0;i<8;i++) buf[nb-8+i]=(bits>>(8*i))&0xff;
    for(size_t off=0;off<nb;off+=64){ u32 M[16]; for(int i=0;i<16;i++) M[i]=buf[off+4*i]|(buf[off+4*i+1]<<8)|(buf[off+4*i+2]<<16)|((u32)buf[off+4*i+3]<<24);
        u32 A=a0,B=b0,C=c0,D=d0; for(int i=0;i<64;i++){ u32 F; int g; if(i<16){F=(B&C)|(~B&D);g=i;} else if(i<32){F=(D&B)|(~D&C);g=(5*i+1)%16;} else if(i<48){F=B^C^D;g=(3*i+5)%16;} else {F=C^(B|~D);g=(7*i)%16;}
            F=F+A+K[i]+M[g]; A=D;D=C;C=B;B=B+((F<<R[i])|(F>>(32-R[i]))); }
        a0+=A;b0+=B;c0+=C;d0+=D; }
    u32 r[4]={a0,b0,c0,d0}; for(int i=0;i<4;i++) for(int j=0;j<4;j++) out[4*i+j]=(r[i]>>(8*j))&0xff;
}

/* ---------- Mth.getSeed (x*3129871 — int-умножение с переполнением!) ---------- */
static i64 mth_getSeed(i32 x, i32 y, i32 z) {
    u64 a = (u64)(i64)(i32)((u32)x * 3129871u);            // int * int -> int, затем знаковое расширение
    u64 b = (u64)(i64)z * 116129781ull;                    // int * long
    u64 seed = a ^ b ^ (u64)(i64)y;
    seed = seed * seed * 42317861ull + seed * 11ull;
    return (i64)seed >> 16;                                // арифметический сдвиг
}

/* ---------- LegacyRandomSource (48-бит LCG) ---------- */
typedef struct { u64 s; } Lcg;
static void lcg_set(Lcg *r, i64 seed) { r->s = ((u64)seed ^ 0x5DEECE66Dull) & ((1ull<<48)-1); }
static i32 lcg_next(Lcg *r, int bits) { r->s = (r->s * 0x5DEECE66Dull + 0xBull) & ((1ull<<48)-1); return (i32)(r->s >> (48 - bits)); }

/* ---------- Xoroshiro128++ ---------- */
typedef struct { u64 lo, hi; } Xr;
static u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }
static u64 mixStafford13(u64 z) { z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
static void xr_init(Xr *r, u64 lo, u64 hi) { if ((lo | hi) == 0) { lo = 0x9E3779B97F4A7C15ull; hi = 0x6A09E667F3BCC909ull; } r->lo = lo; r->hi = hi; }
static void xr_setSeed(Xr *r, i64 seed) { u64 lo = (u64)seed ^ 0x6A09E667F3BCC909ull; u64 hi = lo + 0x9E3779B97F4A7C15ull; xr_init(r, mixStafford13(lo), mixStafford13(hi)); }
static u64 xr_nextLong(Xr *r) { u64 s0 = r->lo, s1 = r->hi; u64 res = rotl(s0 + s1, 17) + s0; s1 ^= s0; r->lo = rotl(s0, 49) ^ s1 ^ (s1 << 21); r->hi = rotl(s1, 28); return res; }

/* ---------- Единый "RandomSource" для теста: 4 варианта ---------- */
enum { L, X, WL, WX };
typedef struct { int kind; Lcg l; Xr x; } Rs;
static void rs_setSeed(Rs *r, i64 seed) { if (r->kind == L || r->kind == WL) lcg_set(&r->l, seed); else xr_setSeed(&r->x, seed); }
static i32 rs_bits(Rs *r, int bits) {   // BitRandomSource.next(bits); для WX: (int)(nextLong() >>> 64-bits)
    if (r->kind == L || r->kind == WL) return lcg_next(&r->l, bits);
    return (i32)(xr_nextLong(&r->x) >> (64 - bits));
}
static i32 rs_nextInt(Rs *r) { return r->kind == X ? (i32)xr_nextLong(&r->x) : rs_bits(r, 32); }
static i32 rs_nextIntB(Rs *r, i32 bound) {
    if (r->kind == X) {  // XoroshiroRandomSource.nextInt(bound): умножение Lemire
        u64 bits = (u32)rs_nextInt(r); u64 m = bits * (u64)bound; u64 f = m & 0xFFFFFFFFull;
        if (f < (u64)bound) { u32 thr = (u32)(-(u32)bound) % (u32)bound; while (f < thr) { bits = (u32)rs_nextInt(r); m = bits * (u64)bound; f = m & 0xFFFFFFFFull; } }
        return (i32)(m >> 32);
    }
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * rs_bits(r, 31)) >> 31);
    i32 sample, mod; do { sample = rs_bits(r, 31); mod = sample % bound; } while ((i32)((u32)sample - (u32)mod + (u32)(bound - 1)) < 0);
    return mod;
}
static i64 rs_nextLong(Rs *r) {
    if (r->kind == X) return (i64)xr_nextLong(&r->x);
    i32 hi = rs_bits(r, 32), lo = rs_bits(r, 32); return (i64)(((u64)(i64)hi << 32) + (u64)(i64)lo);
}
static float rs_nextFloat(Rs *r) { if (r->kind == X) return (float)(xr_nextLong(&r->x) >> 40) * 5.9604645E-8F; return rs_bits(r, 24) * 5.9604645E-8F; }
static double rs_nextDouble(Rs *r) {
    const double U = 1.1102230246251565E-16;   // (double)1.110223E-16F == 2^-53 точно
    if (r->kind == X) return (double)(xr_nextLong(&r->x) >> 11) * U;
    i32 up = rs_bits(r, 26), lo = rs_bits(r, 27); i64 c = ((i64)up << 27) + lo; return (double)c * U;
}
static int rs_nextBool(Rs *r) { if (r->kind == X) return (xr_nextLong(&r->x) & 1) != 0; return rs_bits(r, 1) != 0; }
static void rs_consume(Rs *r, int n) { for (int i = 0; i < n; i++) { if (r->kind == X) xr_nextLong(&r->x); else rs_nextInt(r); } }
static Rs rs_new(int kind, i64 seed) { Rs r; memset(&r, 0, sizeof r); r.kind = kind; rs_setSeed(&r, seed); return r; }

/* ---------- WorldgenRandom-методы ---------- */
static i64 wg_setDecorationSeed(Rs *r, i64 seed, i32 cx, i32 cz) {
    rs_setSeed(r, seed);
    u64 xs = (u64)rs_nextLong(r) | 1ull, zs = (u64)rs_nextLong(r) | 1ull;
    u64 res = ((u64)(i64)cx * xs + (u64)(i64)cz * zs) ^ (u64)seed; rs_setSeed(r, (i64)res); return (i64)res;
}
static void wg_setFeatureSeed(Rs *r, i64 seed, i32 index, i32 step) { rs_setSeed(r, (i64)((u64)seed + (u64)(i64)index + 10000ull * (u64)(i64)step)); }
static void wg_setLargeFeatureSeed(Rs *r, i64 seed, i32 cx, i32 cz) {
    rs_setSeed(r, seed); u64 xs = (u64)rs_nextLong(r), zs = (u64)rs_nextLong(r);
    rs_setSeed(r, (i64)(((u64)(i64)cx * xs) ^ ((u64)(i64)cz * zs) ^ (u64)seed));
}
static void wg_setLargeFeatureWithSalt(Rs *r, i64 seed, i32 x, i32 z, i32 blend) {
    rs_setSeed(r, (i64)((u64)(i64)x * 341873128712ull + (u64)(i64)z * 132897987541ull + (u64)seed + (u64)(i64)blend));
}
static Rs wg_seedSlime(i32 x, i32 z, i64 seed, i64 salt) {
    i32 xx = (i32)((u32)x * (u32)x * 4987142u);               // int: x*x*4987142
    i32 x1 = (i32)((u32)x * 5947611u);                        // int: x*5947611
    i64 zz = (i64)((u64)(i64)((i32)((u32)z*(u32)z)) * 4392871ull);  // (z*z) int, потом *long 4392871L
    i32 z1 = (i32)((u32)z * 389711u);                         // int: z*389711
    // seed + xx + x1 + zz + z1 ^ salt   (Java: + слева направо, затем ^)
    u64 v = (u64)seed + (u64)(i64)xx; v += (u64)(i64)x1; v += (u64)zz; v += (u64)(i64)z1;
    return rs_new(L, (i64)(v ^ (u64)salt));   // SingleThreadedRandomSource: тот же LCG
}

/* ---------- forkPositional / fromHashOf ---------- */
static u64 be64(const unsigned char *p) { u64 v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
static void seedFromHashOf(const char *name, u64 *lo, u64 *hi) { unsigned char h[16]; md5((const unsigned char *)name, strlen(name), h); *lo = be64(h); *hi = be64(h + 8); }
static i32 javaStringHash(const char *s) { u32 h = 0; for (; *s; s++) h = 31 * h + (unsigned char)*s; return (i32)h; }  // ASCII

/* ---------- placement ---------- */
static i32 floorDiv(i32 a, i32 b) { i32 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static void spread_potential(i64 seed, i32 sx, i32 sz, i32 spacing, i32 separation, int triangular, i32 salt, i32 *ox, i32 *oz) {
    i32 gx = floorDiv(sx, spacing), gz = floorDiv(sz, spacing);
    Rs r = rs_new(WL, 0); wg_setLargeFeatureWithSalt(&r, seed, gx, gz, salt);
    i32 lim = spacing - separation, a, b;
    if (triangular) { a = (rs_nextIntB(&r, lim) + rs_nextIntB(&r, lim)) / 2; b = (rs_nextIntB(&r, lim) + rs_nextIntB(&r, lim)) / 2; }
    else { a = rs_nextIntB(&r, lim); b = rs_nextIntB(&r, lim); }
    *ox = gx * spacing + a; *oz = gz * spacing + b;
}
static int freq_should(int method, i64 seed, i32 salt, i32 sx, i32 sz, float prob) {
    Rs r = rs_new(WL, 0);
    switch (method) {
    case 0: wg_setLargeFeatureWithSalt(&r, seed, salt, sx, sz); return rs_nextFloat(&r) < prob;           // default: (x=salt, z=sx, blend=sz)
    case 1: { i32 cx = sx >> 4, cz = sz >> 4; rs_setSeed(&r, (i64)((u64)(i64)(cx ^ (i32)((u32)cz << 4)) ^ (u64)seed)); rs_nextInt(&r); return rs_nextIntB(&r, (i32)(1.0F / prob)) == 0; }
    case 2: wg_setLargeFeatureWithSalt(&r, seed, sx, sz, 10387320); return rs_nextFloat(&r) < prob;
    default: wg_setLargeFeatureSeed(&r, seed, sx, sz); return rs_nextDouble(&r) < (double)prob;
    }
}

static const i64 SEEDS[] = {0LL, 1LL, -1LL, 42LL, 123456789012345LL, 0x7FFFFFFFFFFFFFFFLL, (i64)0x8000000000000000ull, (i64)0x9E3779B97F4A7C15ull, -4172144997902289642LL, 8682522807148012LL};
static const int XZ[8][2] = {{0,0},{1,-1},{-1,1},{5,-7},{-123,456},{1000,-1000},{-1875000,1875000},{1874999,1874999}};
static const int XYZ[6][3] = {{0,0,0},{1,2,3},{-1,-64,-1},{100,200,-300},{29999999,319,-29999999},{-123456,7,654321}};
static const char *NAMES[] = {"minecraft:temperature","minecraft:continentalness","octave_-3","minecraft:terrain","bedrock_floor","minecraft:aquifer",""};

static void basic(const char *tag, Rs r) {
    u32 i0 = (u32)rs_nextInt(&r); u32 i1 = (u32)rs_nextInt(&r);   // порядок вычисления аргументов printf в C не определён
    printf("%s %016llx %016llx", tag, (unsigned long long)i0, (unsigned long long)i1);
    static const int B[] = {16,16,10,1000,1000,7,7,1,2,3,100,65536,1<<30,1000000007,34-8,32-5,1<<20,24,96,3};
    for (unsigned i = 0; i < sizeof B / sizeof *B; i++) printf(" %d", rs_nextIntB(&r, B[i]));
    printf(" %016llx", (unsigned long long)rs_nextLong(&r));
    float f = rs_nextFloat(&r); i32 fb; memcpy(&fb, &f, 4); printf(" %d", fb);
    double d = rs_nextDouble(&r); u64 db; memcpy(&db, &d, 8); printf(" %016llx", (unsigned long long)db);
    int b1 = rs_nextBool(&r), b2 = rs_nextBool(&r); printf(" %s%s", b1 ? "true" : "false", b2 ? "true" : "false");
    printf(" %d", 5 + rs_nextIntB(&r, 12));
    rs_consume(&r, 17); printf(" %016llx\n", (unsigned long long)rs_nextLong(&r));
}
#define H(v) ((unsigned long long)(v))
int main(void) {
    for (unsigned si = 0; si < sizeof SEEDS / sizeof *SEEDS; si++) {
        i64 s = SEEDS[si];
        printf("seed %016llx\n", H(s));
        basic("L.basic", rs_new(L, s)); basic("X.basic", rs_new(X, s)); basic("WL.basic", rs_new(WL, s)); basic("WX.basic", rs_new(WX, s));
        { u64 lo = (u64)s ^ 0x6A09E667F3BCC909ull, hi = lo + 0x9E3779B97F4A7C15ull;
          printf("upgrade %016llx %016llx unmixed %016llx %016llx\n", H(mixStafford13(lo)), H(mixStafford13(hi)), H(lo), H(hi)); }
        printf("stafford %016llx\n", H(mixStafford13((u64)s)));
        for (unsigned ni = 0; ni < sizeof NAMES / sizeof *NAMES; ni++) {
            const char *name = NAMES[ni]; u64 hl, hh; seedFromHashOf(name, &hl, &hh);
            Rs src = rs_new(X, s); u64 flo = xr_nextLong(&src.x), fhi = xr_nextLong(&src.x);   // forkPositional
            Rs r; r.kind = X; xr_init(&r.x, hl ^ flo, hh ^ fhi);
            u64 a = xr_nextLong(&r.x), b = xr_nextLong(&r.x);
            printf("X.fromHashOf[%s] %016llx %016llx -> %016llx %016llx\n", name, H(hl), H(hh), H(a), H(b));
            Rs ls = rs_new(L, s); i64 lseed = rs_nextLong(&ls); i32 jh = javaStringHash(name);
            Rs lr = rs_new(L, (i64)((u64)(i64)jh ^ (u64)lseed));
            printf("L.fromHashOf[%s] %d -> %016llx\n", name, jh, H(rs_nextLong(&lr)));
        }
        for (int i = 0; i < 6; i++) {
            i32 x = XYZ[i][0], y = XYZ[i][1], z = XYZ[i][2];
            Rs src = rs_new(X, s); u64 flo = xr_nextLong(&src.x), fhi = xr_nextLong(&src.x);
            Rs r; r.kind = X; xr_init(&r.x, (u64)mth_getSeed(x, y, z) ^ flo, fhi);
            printf("X.at %d,%d,%d %016llx -> %016llx\n", x, y, z, H(mth_getSeed(x, y, z)), H(xr_nextLong(&r.x)));
            Rs ls = rs_new(L, s); i64 lseed = rs_nextLong(&ls);
            Rs lr = rs_new(L, (i64)((u64)mth_getSeed(x, y, z) ^ (u64)lseed));
            printf("L.at %d,%d,%d %016llx\n", x, y, z, H(rs_nextLong(&lr)));
        }
        { Rs src = rs_new(X, s); u64 lo = xr_nextLong(&src.x), hi = xr_nextLong(&src.x); Rs r; r.kind = X; xr_init(&r.x, lo, hi); printf("X.fork %016llx\n", H(xr_nextLong(&r.x)));
          Rs ls = rs_new(L, s); Rs l2 = rs_new(L, rs_nextLong(&ls)); printf("L.fork %016llx\n", H(rs_nextLong(&l2)));
          Rs s7 = rs_new(X, 7); u64 f7l = xr_nextLong(&s7.x), f7h = xr_nextLong(&s7.x); Rs fr; fr.kind = X; xr_init(&fr.x, (u64)s ^ f7l, (u64)s ^ f7h); printf("X.fromSeed %016llx\n", H(xr_nextLong(&fr.x))); }
        for (int i = 0; i < 8; i++) {
            i32 cx = XZ[i][0], cz = XZ[i][1];
            Rs w = rs_new(WX, 1234567); i64 dec = wg_setDecorationSeed(&w, s, cx * 16, cz * 16);
            printf("WX.decoration %d,%d %016llx", cx, cz, H(dec));
            for (int k = 0; k < 3; k++) printf(" %d", rs_nextIntB(&w, 100));
            printf(" %016llx", H(rs_nextLong(&w)));
            { float f = rs_nextFloat(&w); i32 fb; memcpy(&fb, &f, 4); printf(" %d", fb); double d = rs_nextDouble(&w); u64 db; memcpy(&db, &d, 8); printf(" %016llx\n", H(db)); }
            printf("WX.feature %d,%d ", cx, cz);
            static const int IS[3][2] = {{0,0},{5,3},{123,9}};
            for (int k = 0; k < 3; k++) { wg_setFeatureSeed(&w, dec, IS[k][0], IS[k][1]); int a = rs_nextIntB(&w, 1000); printf(" %d %016llx", a, H(rs_nextLong(&w))); }
            printf("\n");
            Rs wl = rs_new(WL, 999); i64 decl = wg_setDecorationSeed(&wl, s, cx * 16, cz * 16);
            { int a = rs_nextIntB(&wl, 100); printf("WL.decoration %d,%d %016llx %d %016llx\n", cx, cz, H(decl), a, H(rs_nextLong(&wl))); }
            wg_setLargeFeatureSeed(&wl, s, cx, cz);
            { int a = rs_nextIntB(&wl, 100); i64 lg = rs_nextLong(&wl); float f = rs_nextFloat(&wl); i32 fb; memcpy(&fb, &f, 4); double d = rs_nextDouble(&wl); u64 db; memcpy(&db, &d, 8);
              printf("WL.largeFeature %d,%d %d %016llx %d %016llx\n", cx, cz, a, H(lg), fb, H(db)); }
            wg_setLargeFeatureWithSalt(&wl, s, cx, cz, 10387312); { int a = rs_nextIntB(&wl, 100); printf("WL.largeFeatureSalt %d,%d %d %016llx\n", cx, cz, a, H(rs_nextLong(&wl))); }
            wg_setLargeFeatureWithSalt(&wl, s, cx, cz, 0); { int a = rs_nextIntB(&wl, 100); printf("WL.largeFeatureSalt0 %d,%d %d %016llx\n", cx, cz, a, H(rs_nextLong(&wl))); }
            Rs sl = wg_seedSlime(cx, cz, s, 987234911LL);
            printf("slime %d,%d %s\n", cx, cz, rs_nextIntB(&sl, 10) == 0 ? "true" : "false");
            printf("slimeXZ %d,%d %d %lld\n", cx, cz, (i32)((u32)cx * (u32)cx * 4987142u), (long long)((i64)(i32)((u32)cz * (u32)cz) * 4392871LL));
            i32 ox, oz;
            spread_potential(s, cx, cz, 34, 8, 0, 10387312, &ox, &oz); printf("spread.linear34/8/10387312 %d,%d %d %d\n", cx, cz, ox, oz);
            spread_potential(s, cx, cz, 32, 5, 1, 10387313, &ox, &oz); printf("spread.tri32/5/10387313 %d,%d %d %d\n", cx, cz, ox, oz);
            spread_potential(s, cx, cz, 4096, 1, 0, 0, &ox, &oz); printf("spread.linear4096/1/0 %d,%d %d %d\n", cx, cz, ox, oz);
        }
        for (int m = 0; m < 4; m++) {
            static const char *MN[] = {"DEFAULT", "LEGACY_TYPE_1", "LEGACY_TYPE_2", "LEGACY_TYPE_3"};
            printf("freq.%s ", MN[m]);
            for (int i = 0; i < 8; i++) { static const float FR[3] = {0.2f, 0.5f, 0.9f}; for (int k = 0; k < 3; k++) putchar(freq_should(m, s, 10387313, XZ[i][0], XZ[i][1], FR[k]) ? '1' : '0'); }
            putchar('\n');
        }
    }
    return 0;
}
