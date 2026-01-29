/**
 * Stub implementations for CTP 6.6.9 darwin static library
 * These symbols are required by the static libs but not provided separately.
 * They are normally linked into the Linux .so but split out on darwin.
 */
#include <cstring>
#include <cstdio>

extern "C" {
    const char* g_strSupportVersion = "6.6.9";
    // UseNetCompressLog / UseProcessLog may already be defined in CTP .a (CLogger.o),
    // use weak linkage to avoid duplicate symbol errors
    __attribute__((weak)) int UseNetCompressLog = 0;
    __attribute__((weak)) int UseProcessLog = 0;

    // OpenSSL 3 renamed SSL_get_peer_certificate to SSL_get1_peer_certificate
    // Provide compatibility shim for CTP which uses the old name
    struct ssl_st;
    struct x509_st;
    x509_st* SSL_get1_peer_certificate(const ssl_st*);
    x509_st* SSL_get_peer_certificate(const ssl_st* ssl) {
        return SSL_get1_peer_certificate(ssl);
    }
}

// AES_DecodeCollectData(unsigned char*)
unsigned char* AES_DecodeCollectData(unsigned char* data) {
    return data;
}

// EncodeDataUsingAesKey(unsigned char*, unsigned char*, unsigned char*)
void EncodeDataUsingAesKey(unsigned char* /*in*/, unsigned char* /*key*/, unsigned char* /*out*/) {
}

// ApiEncryptFrontShakeHandData(unsigned char*, int, unsigned char*, int*, char const*)
void ApiEncryptFrontShakeHandData(unsigned char* /*in*/, int /*inLen*/, unsigned char* /*out*/, int* outLen, const char* /*key*/) {
    if (outLen) *outLen = 0;
}

// ApidecryptFrontShakeHandData(unsigned char*, int, unsigned char*, int*, char const*)
void ApidecryptFrontShakeHandData(unsigned char* /*in*/, int /*inLen*/, unsigned char* /*out*/, int* outLen, const char* /*key*/) {
    if (outLen) *outLen = 0;
}
