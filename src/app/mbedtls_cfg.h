/* mbedtls_cfg.h — TLS 1.2. Server: one suite, ECDHE-ECDSA P-256 with AES-128-GCM. Client (for
 * renewing): also ECDHE-RSA, which AWS needs. The crypto half is configured in crypto_cfg.h. */
#ifndef N540_MBEDTLS_CFG_H
#define N540_MBEDTLS_CFG_H
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_X509_CSR_WRITE_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_TICKET_C
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384       /* browsers fill records when uploading */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4352        /* the certificate chain goes out in one message */
#define MBEDTLS_SSL_CIPHERSUITES MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
                                 MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
#endif
