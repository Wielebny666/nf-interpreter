//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// System.Net.Security is linked in so that the assembly loads and plain sockets
// work, but there is no TLS engine behind it yet. Every attempt to start a
// secure session fails at setup, which the managed side reports as an exception
// rather than carrying on unencrypted.

#include <nanoCLR_Runtime.h>

#if !defined(NANOCLR_POSIX_SOCKETS)
#error "Ssl_POSIX_stub.cpp needs NANOCLR_POSIX_SOCKETS so that nanoPAL.h pulls in the socket declarations."
#endif

bool SSL_Initialize()
{
    return true;
}

bool SSL_Uninitialize()
{
    return true;
}

SslError SSL_ServerInit(
    int sslMode,
    int sslVerify,
    const char *certificate,
    int certLength,
    const uint8_t *privateKey,
    int privateKeyLength,
    const char *privateKeyPassword,
    int privateKeyPasswordLength,
    int &sslContextHandle,
    bool useDeviceCertificate)
{
    (void)sslMode;
    (void)sslVerify;
    (void)certificate;
    (void)certLength;
    (void)privateKey;
    (void)privateKeyLength;
    (void)privateKeyPassword;
    (void)privateKeyPasswordLength;
    (void)useDeviceCertificate;

    sslContextHandle = -1;
    return SslError_SetupFailed;
}

SslError SSL_ClientInit(
    int sslMode,
    int sslVerify,
    const char *certificate,
    int certLength,
    const uint8_t *privateKey,
    int privateKeyLength,
    const char *privateKeyPassword,
    int privateKeyPasswordLength,
    int &sslContextHandle,
    bool useDeviceCertificate)
{
    return SSL_ServerInit(
        sslMode,
        sslVerify,
        certificate,
        certLength,
        privateKey,
        privateKeyLength,
        privateKeyPassword,
        privateKeyPasswordLength,
        sslContextHandle,
        useDeviceCertificate);
}

bool SSL_AddCertificateAuthority(int sslContextHandle, const char *certificate, int certLength)
{
    (void)sslContextHandle;
    (void)certificate;
    (void)certLength;
    return false;
}

bool SSL_ExitContext(int sslContextHandle)
{
    (void)sslContextHandle;
    return false;
}

SslError SSL_Accept(int socket, int sslContextHandle, int *mbedtlsCode)
{
    (void)socket;
    (void)sslContextHandle;
    (void)mbedtlsCode;
    return SslError_SetupFailed;
}

SslError SSL_Connect(int socket, const char *szTargetHost, int sslContextHandle, int *mbedtlsCode)
{
    (void)socket;
    (void)szTargetHost;
    (void)sslContextHandle;
    (void)mbedtlsCode;
    return SslError_SetupFailed;
}

int SSL_Write(int socket, const char *Data, size_t size)
{
    (void)socket;
    (void)Data;
    (void)size;
    return -1;
}

int SSL_Read(int socket, char *Data, size_t size)
{
    (void)socket;
    (void)Data;
    (void)size;
    return -1;
}

int SSL_CloseSocket(int socket)
{
    (void)socket;
    return 0;
}

bool SSL_ParseCertificate(const char *certificate, size_t certLength, X509CertData *certData)
{
    (void)certificate;
    (void)certLength;
    (void)certData;
    return false;
}

int SSL_DecodePrivateKey(
    const unsigned char *key,
    size_t keyLength,
    const unsigned char *password,
    size_t passwordLength)
{
    (void)key;
    (void)keyLength;
    (void)password;
    (void)passwordLength;
    return -1;
}

int SSL_DataAvailable(int socket)
{
    (void)socket;
    return 0;
}
