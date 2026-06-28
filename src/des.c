/* mvci_des.c — DES-ECB backend.
 *
 *   Linux  : OpenSSL libcrypto (DES_ecb_encrypt)
 *   Windows: CNG / BCrypt (BCRYPT_DES_ALGORITHM, ECB) — built into Windows,
 *            no external dependency.
 *
 * Both expose the same stateless API declared in mvci_serial.h:
 *   void mvci_des_encrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks);
 *   void mvci_des_decrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks);
 */

#include <mvci/serial.h>
#include <string.h>

/* ====================================================================== */
#ifdef _WIN32
/* ------------------------------- Windows CNG ------------------------- */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

/* Cache the algorithm provider (ECB) once; key handles are per-call. */
static BCRYPT_ALG_HANDLE g_alg = NULL;

static BCRYPT_ALG_HANDLE des_alg(void)
{
    if (!g_alg) {
        BCRYPT_ALG_HANDLE a = NULL;
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&a, BCRYPT_DES_ALGORITHM, NULL, 0)))
            return NULL;
        BCryptSetProperty(a, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          (ULONG)((wcslen(BCRYPT_CHAIN_MODE_ECB) + 1) * sizeof(WCHAR)), 0);
        /* benign race: if two threads set it, both store the same handle */
        if (InterlockedCompareExchangePointer((PVOID *)&g_alg, a, NULL) != NULL)
            BCryptCloseAlgorithmProvider(a, 0);
    }
    return g_alg;
}

static void des_run(const uint8_t key[8], uint8_t *buf, size_t nblocks, int enc)
{
    BCRYPT_ALG_HANDLE alg = des_alg();
    if (!alg || nblocks == 0) return;

    BCRYPT_KEY_HANDLE k = NULL;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key, 8, 0)))
        return;

    ULONG done = 0;
    ULONG len = (ULONG)(nblocks * 8);
    if (enc)
        BCryptEncrypt(k, buf, len, NULL, NULL, 0, buf, len, &done, 0);
    else
        BCryptDecrypt(k, buf, len, NULL, NULL, 0, buf, len, &done, 0);

    BCryptDestroyKey(k);
}

void mvci_des_encrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{ des_run(key, buf, nblocks, 1); }

void mvci_des_decrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{ des_run(key, buf, nblocks, 0); }

/* ====================================================================== */
#else
/* ------------------------------- OpenSSL ----------------------------- */
#include <openssl/des.h>

void mvci_des_encrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{
    DES_cblock k;
    DES_key_schedule ks;
    memcpy(k, key, 8);
    DES_set_key_unchecked(&k, &ks);
    for (size_t i = 0; i < nblocks; i++)
        DES_ecb_encrypt((DES_cblock *)(buf + i * 8),
                        (DES_cblock *)(buf + i * 8), &ks, DES_ENCRYPT);
}

void mvci_des_decrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{
    DES_cblock k;
    DES_key_schedule ks;
    memcpy(k, key, 8);
    DES_set_key_unchecked(&k, &ks);
    for (size_t i = 0; i < nblocks; i++)
        DES_ecb_encrypt((DES_cblock *)(buf + i * 8),
                        (DES_cblock *)(buf + i * 8), &ks, DES_DECRYPT);
}

#endif
