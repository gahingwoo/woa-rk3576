/*
 * aic_crypto.c - the primitives WPA2-PSK needs on the host: SHA-1 (FIPS
 * 180-4), HMAC-SHA1 (RFC 2104), PBKDF2 (RFC 8018), the IEEE 802.11 PRF,
 * AES-128 (FIPS 197) and the AES key wrap (RFC 3394).
 *
 * Written from the standards for this driver; tools/aictest checks each
 * against the published test vectors (RFC 3174/2202/6070/3394, FIPS 197
 * appendix C, IEEE 802.11 annex J).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic.h"

/* ------------------------------------------------------------- SHA-1 */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void
sha1_block(u32 H[5], const u8 *B)
{
    u32 w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((u32)B[4 * i] << 24) | ((u32)B[4 * i + 1] << 16) | ((u32)B[4 * i + 2] << 8) | B[4 * i + 3];
    }
    for (; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = H[0]; b = H[1]; c = H[2]; d = H[3]; e = H[4];
    for (i = 0; i < 80; i++) {
        u32 f, k;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e;
}

void
aic_sha1_init(aic_sha1 *S)
{
    S->H[0] = 0x67452301;
    S->H[1] = 0xEFCDAB89;
    S->H[2] = 0x98BADCFE;
    S->H[3] = 0x10325476;
    S->H[4] = 0xC3D2E1F0;
    S->Len = 0;
    S->BufLen = 0;
}

void
aic_sha1_update(aic_sha1 *S, const u8 *Data, u32 Len)
{
    S->Len += Len;
    while (Len) {
        u32 n = 64 - S->BufLen;

        if (n > Len) {
            n = Len;
        }
        aic_memcpy(S->Buf + S->BufLen, Data, n);
        S->BufLen += n;
        Data += n;
        Len -= n;
        if (S->BufLen == 64) {
            sha1_block(S->H, S->Buf);
            S->BufLen = 0;
        }
    }
}

void
aic_sha1_final(aic_sha1 *S, u8 Out[20])
{
    u64 bits = S->Len * 8;
    u8 pad = 0x80, zero = 0, len[8];
    int i;

    aic_sha1_update(S, &pad, 1);
    while (S->BufLen != 56) {
        aic_sha1_update(S, &zero, 1);
    }
    for (i = 0; i < 8; i++) {
        len[i] = (u8)(bits >> (56 - 8 * i));
    }
    aic_sha1_update(S, len, 8);
    for (i = 0; i < 5; i++) {
        Out[4 * i] = (u8)(S->H[i] >> 24);
        Out[4 * i + 1] = (u8)(S->H[i] >> 16);
        Out[4 * i + 2] = (u8)(S->H[i] >> 8);
        Out[4 * i + 3] = (u8)S->H[i];
    }
}

/* ---------------------------------------------------------- HMAC-SHA1 */

void
aic_hmac_sha1_vec(const u8 *Key, u32 KeyLen, u32 Count, const u8 *const *Data,
                  const u32 *Len, u8 Out[20])
{
    u8 k[64], pad[64], inner[20];
    aic_sha1 s;
    u32 i;

    aic_memset(k, 0, sizeof(k));
    if (KeyLen > 64) {
        aic_sha1_init(&s);
        aic_sha1_update(&s, Key, KeyLen);
        aic_sha1_final(&s, k);
    } else {
        aic_memcpy(k, Key, KeyLen);
    }
    for (i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    aic_sha1_init(&s);
    aic_sha1_update(&s, pad, 64);
    for (i = 0; i < Count; i++) {
        aic_sha1_update(&s, Data[i], Len[i]);
    }
    aic_sha1_final(&s, inner);
    for (i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5C;
    }
    aic_sha1_init(&s);
    aic_sha1_update(&s, pad, 64);
    aic_sha1_update(&s, inner, 20);
    aic_sha1_final(&s, Out);
}

void
aic_hmac_sha1(const u8 *Key, u32 KeyLen, const u8 *Data, u32 Len, u8 Out[20])
{
    aic_hmac_sha1_vec(Key, KeyLen, 1, &Data, &Len, Out);
}

void
aic_pbkdf2_sha1(const u8 *Pass, u32 PassLen, const u8 *Salt, u32 SaltLen,
                u32 Iter, u8 *Out, u32 OutLen)
{
    u32 block = 1;

    while (OutLen) {
        u8 cnt[4], u[20], t[20];
        const u8 *v[2];
        u32 l[2], i, j, n;

        cnt[0] = (u8)(block >> 24); cnt[1] = (u8)(block >> 16);
        cnt[2] = (u8)(block >> 8); cnt[3] = (u8)block;
        v[0] = Salt; l[0] = SaltLen;
        v[1] = cnt; l[1] = 4;
        aic_hmac_sha1_vec(Pass, PassLen, 2, v, l, u);
        aic_memcpy(t, u, 20);
        for (i = 1; i < Iter; i++) {
            aic_hmac_sha1(Pass, PassLen, u, 20, u);
            for (j = 0; j < 20; j++) {
                t[j] ^= u[j];
            }
        }
        n = OutLen < 20 ? OutLen : 20;
        aic_memcpy(Out, t, n);
        Out += n;
        OutLen -= n;
        block++;
    }
}

void
aic_prf_sha1(const u8 *Key, u32 KeyLen, const char *Label, const u8 *Data,
             u32 DataLen, u8 *Out, u32 OutLen)
{
    u8 zero = 0, cnt = 0, h[20];
    const u8 *v[4];
    u32 l[4], llen = 0;

    while (Label[llen]) {
        llen++;
    }
    v[0] = (const u8 *)Label; l[0] = llen;
    v[1] = &zero; l[1] = 1;
    v[2] = Data; l[2] = DataLen;
    v[3] = &cnt; l[3] = 1;
    while (OutLen) {
        u32 n = OutLen < 20 ? OutLen : 20;

        aic_hmac_sha1_vec(Key, KeyLen, 4, v, l, h);
        aic_memcpy(Out, h, n);
        Out += n;
        OutLen -= n;
        cnt++;
    }
}

/* ------------------------------------------------------------ AES-128 */

static u8
xtime(u8 x)
{
    return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static u8
gmul(u8 a, u8 b)
{
    u8 p = 0;

    while (b) {
        if (b & 1) {
            p ^= a;
        }
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

static const u8 k_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static u8 k_inv_sbox[256];
static int k_inv_ready;

static void
expand_key(const u8 Key[16], u8 Rk[176])
{
    u8 rcon = 1;
    int i;

    aic_memcpy(Rk, Key, 16);
    for (i = 16; i < 176; i += 4) {
        u8 t[4];

        aic_memcpy(t, Rk + i - 4, 4);
        if ((i % 16) == 0) {
            u8 x = t[0];

            t[0] = (u8)(k_sbox[t[1]] ^ rcon);
            t[1] = k_sbox[t[2]];
            t[2] = k_sbox[t[3]];
            t[3] = k_sbox[x];
            rcon = xtime(rcon);
        }
        Rk[i] = Rk[i - 16] ^ t[0];
        Rk[i + 1] = Rk[i - 15] ^ t[1];
        Rk[i + 2] = Rk[i - 14] ^ t[2];
        Rk[i + 3] = Rk[i - 13] ^ t[3];
    }
}

void
aic_aes128_encrypt(const u8 Key[16], const u8 In[16], u8 Out[16])
{
    u8 rk[176], s[16], t[16];
    int r, i, c;

    expand_key(Key, rk);
    for (i = 0; i < 16; i++) {
        s[i] = In[i] ^ rk[i];
    }
    for (r = 1; r <= 10; r++) {
        for (i = 0; i < 16; i++) {          /* SubBytes + ShiftRows */
            t[i] = k_sbox[s[(i + 4 * (i % 4)) % 16]];
        }
        if (r != 10) {                      /* MixColumns */
            for (c = 0; c < 4; c++) {
                u8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];

                s[4 * c] = (u8)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                s[4 * c + 1] = (u8)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                s[4 * c + 2] = (u8)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                s[4 * c + 3] = (u8)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        } else {
            aic_memcpy(s, t, 16);
        }
        for (i = 0; i < 16; i++) {
            s[i] ^= rk[16 * r + i];
        }
    }
    aic_memcpy(Out, s, 16);
}

void
aic_aes128_decrypt(const u8 Key[16], const u8 In[16], u8 Out[16])
{
    u8 rk[176], s[16], t[16];
    int r, i, c;

    if (!k_inv_ready) {
        for (i = 0; i < 256; i++) {
            k_inv_sbox[k_sbox[i]] = (u8)i;
        }
        k_inv_ready = 1;
    }
    expand_key(Key, rk);
    for (i = 0; i < 16; i++) {
        s[i] = In[i] ^ rk[160 + i];
    }
    for (r = 9; r >= 0; r--) {
        for (i = 0; i < 16; i++) {          /* InvShiftRows + InvSubBytes */
            t[(i + 4 * (i % 4)) % 16] = k_inv_sbox[s[i]];
        }
        for (i = 0; i < 16; i++) {
            t[i] ^= rk[16 * r + i];
        }
        if (r != 0) {                       /* InvMixColumns */
            for (c = 0; c < 4; c++) {
                u8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];

                s[4 * c] = (u8)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                s[4 * c + 1] = (u8)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                s[4 * c + 2] = (u8)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                s[4 * c + 3] = (u8)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        } else {
            aic_memcpy(s, t, 16);
        }
    }
    aic_memcpy(Out, s, 16);
}

/* ------------------------------------------------------- key wrap */

int
aic_aes_unwrap(const u8 Kek[16], const u8 *In, u32 InLen, u8 *Out)
{
    u8 a[8], b[16];
    u32 n = InLen / 8 - 1, i;
    int j;

    if (InLen < 24 || (InLen % 8) != 0) {
        return AIC_E_FORMAT;
    }
    aic_memcpy(a, In, 8);
    aic_memcpy(Out, In + 8, InLen - 8);
    for (j = 5; j >= 0; j--) {
        for (i = n; i >= 1; i--) {
            u32 t = n * (u32)j + i;

            aic_memcpy(b, a, 8);
            b[7] ^= (u8)t;
            b[6] ^= (u8)(t >> 8);
            b[5] ^= (u8)(t >> 16);
            b[4] ^= (u8)(t >> 24);
            aic_memcpy(b + 8, Out + 8 * (i - 1), 8);
            aic_aes128_decrypt(Kek, b, b);
            aic_memcpy(a, b, 8);
            aic_memcpy(Out + 8 * (i - 1), b + 8, 8);
        }
    }
    for (i = 0; i < 8; i++) {
        if (a[i] != 0xA6) {
            return AIC_E_MIC;
        }
    }
    return AIC_OK;
}

void
aic_aes_wrap(const u8 Kek[16], const u8 *In, u32 InLen, u8 *Out)
{
    u8 a[8], b[16];
    u32 n = InLen / 8, i, j;

    aic_memset(a, 0xA6, 8);
    aic_memcpy(Out + 8, In, InLen);
    for (j = 0; j <= 5; j++) {
        for (i = 1; i <= n; i++) {
            u32 t = n * j + i;

            aic_memcpy(b, a, 8);
            aic_memcpy(b + 8, Out + 8 * i, 8);
            aic_aes128_encrypt(Kek, b, b);
            aic_memcpy(a, b, 8);
            a[7] ^= (u8)t;
            a[6] ^= (u8)(t >> 8);
            a[5] ^= (u8)(t >> 16);
            a[4] ^= (u8)(t >> 24);
            aic_memcpy(Out + 8 * i, b + 8, 8);
        }
    }
    aic_memcpy(Out, a, 8);
}
