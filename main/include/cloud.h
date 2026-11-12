/**
 * @name Cloud identity
 * @file cloud.h
 * @author xqe2011
 */
#ifndef CLOUD_H__
#define CLOUD_H__

#include <cJSON.h>
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOUD_PUBLIC_KEY_BYTES 65
#define CLOUD_SIGNATURE_BYTES 64
#define CLOUD_DIGEST_PAYLOAD_BYTES 20
#define CLOUD_DIGEST_CHECKSUM_BYTES 4
#define CLOUD_DIGEST_CHARS 33
/* Domain separation for challenge signing: UTF-8 "saihub/cloud-auth/v1" + NUL. */
#define CLOUD_AUTH_DOMAIN "saihub/cloud-auth/v1"

typedef struct {
  bool connected;             /* authenticated WebSocket to the cloud relay */
  const char* landingPageUrl; /* empty when cloud relay is disabled */
  char reason[192];           /* empty when connected; otherwise why not */
} Cloud_Status;

/* The public key is SEC1's uncompressed P-256 point: 0x04 || X || Y. */
/* The private key is kept in the hardware ECDSA key block. */
/* Cloud_Init provisions the first unused key block if no P-256 key exists. */
/* The digest is Base58Check(payload = SHA-256(publicKey)[0..19]). */
esp_err_t Cloud_GetPublicKey(uint8_t out[CLOUD_PUBLIC_KEY_BYTES], size_t* lenOut);
const char* Cloud_GetPublicKeyDigest(void);
/** HTTPS/HTTP landing page for this digest, or "" when cloud relay is disabled. */
const char* Cloud_GetLandingPageUrl(void);
/** Live cloud relay status for GET /cloud/status and get_cloud_status. */
void Cloud_GetStatus(Cloud_Status* out);

typedef void (*Cloud_ConnectedCallback)(void);
typedef void (*Cloud_DisconnectedCallback)(void);
esp_err_t Cloud_RegisterConnectedCallback(Cloud_ConnectedCallback callback);
esp_err_t Cloud_RegisterDisconnectedCallback(Cloud_DisconnectedCallback callback);

/* Signs SHA-256(CLOUD_AUTH_DOMAIN || NUL || challenge); signature is raw r || s. */
esp_err_t Cloud_SignChallenge(const uint8_t* challenge, size_t challengeLen,
                              uint8_t signature[CLOUD_SIGNATURE_BYTES], size_t* signatureLenOut);

esp_err_t Cloud_Init(void);

bool Cloud_HasGrantSecret(const char* grantSecret);
cJSON* Cloud_ListGrantSecrets(void);
esp_err_t Cloud_RevokeGrantSecret(const char* grantSecret);
bool Cloud_PairingSessionIsLive(void);
void Cloud_PairingApprove(void);

bool Cloud_FormatTcpStreamUrl(const char* service, char* out, size_t outLen);

/** Listen wake fd for select. -1 when the service name is unknown. */
int Cloud_TcpEventFd(const char* service);
/**
 * Half-open → open (ESP_OK) returns clientFd after tcpStreamOpenResponse success.
 * Half-open → fail (err) sends reason and returns -1. -1 if nothing is pending.
 */
int Cloud_TcpAccept(const char* service, esp_err_t err, const char* reason);
/** Like recv on a cloud clientFd. >0 data, 0 empty, -1 peer EOF. */
int Cloud_TcpRecv(int fd, void* buf, size_t len);
esp_err_t Cloud_TcpSend(int fd, const void* data, size_t len);
void Cloud_TcpClose(int fd);
/** Session task exit: fail pending accepts and drop the listen eventfd. */
void Cloud_TcpRelease(const char* service);

#endif
