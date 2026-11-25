/* crypto_cfg.h — What mbedTLS's crypto half (TF-PSA-Crypto) has to do for tls.c. */
#ifndef N540_CRYPTO_CFG_H
#define N540_CRYPTO_CFG_H
#define PSA_WANT_ALG_ECDH                        1
#define PSA_WANT_ALG_ECDSA                       1
#define PSA_WANT_ALG_DETERMINISTIC_ECDSA         1
#define PSA_WANT_ALG_GCM                         1
#define PSA_WANT_ALG_HMAC                        1
#define PSA_WANT_ALG_SHA_256                     1
#define PSA_WANT_ALG_SHA_384                     1  /* only to recognise how a certificate was signed */
#define PSA_WANT_ALG_SHA_512                     1
#define PSA_WANT_ALG_TLS12_PRF                   1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN           1  /* authorities that sign with RSA */
#define PSA_WANT_ECC_SECP_R1_256                 1
#define PSA_WANT_KEY_TYPE_AES                    1
#define PSA_WANT_KEY_TYPE_HMAC                   1
#define PSA_WANT_KEY_TYPE_DERIVE                 1
#define PSA_WANT_KEY_TYPE_RAW_DATA               1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY         1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC     1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT    1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT    1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE  1
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY         1

#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY              /* the chip's generator, see tls.c */
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC               /* keys on the TLS heap */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_MD_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
#define MBEDTLS_MEMORY_DEBUG                        /* heap figures for `https` */
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_MPI_MAX_SIZE 512
#endif
