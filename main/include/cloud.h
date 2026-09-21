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

/* The public key is SEC1's uncompressed P-256 point: 0x04 || X || Y. */
/* The private key is kept in the hardware ECDSA key block. */
/* Cloud_Init provisions the first unused key block if no P-256 key exists. */
/* The digest is Base58Check(payload = SHA-256(publicKey)[0..19]). */
esp_err_t Cloud_GetPublicKey(uint8_t out[CLOUD_PUBLIC_KEY_BYTES], size_t* lenOut);
const char* Cloud_GetPublicKeyDigest(void);

/* Signs SHA-256(CLOUD_AUTH_DOMAIN || NUL || challenge); signature is raw r || s. */
esp_err_t Cloud_SignChallenge(const uint8_t* challenge, size_t challengeLen,
                              uint8_t signature[CLOUD_SIGNATURE_BYTES], size_t* signatureLenOut);

esp_err_t Cloud_Init(void);

bool Cloud_HasGrantSecret(const char* grantSecret);
cJSON* Cloud_ListGrantSecrets(void);
esp_err_t Cloud_RevokeGrantSecret(const char* grantSecret);
bool Cloud_PairingSessionIsLive(void);
void Cloud_PairingApprove(void);

#endif
