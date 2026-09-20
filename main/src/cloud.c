/**
 * @name Cloud identity and relay
 * @file cloud.c
 * @author xqe2011
 */
#include "cloud.h"
#include "config.h"
#include "http_server.h"
#include "ntp.h"
#include "wifi.h"

#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <mbedtls/base64.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <bootloader_random.h>
#include <esp_efuse.h>
#include <esp_efuse_chip.h>
#include <esp_log.h>
#include <esp_random.h>
#include <psa/crypto.h>
#include <psa_crypto_driver_esp_ecdsa.h>
#include <soc/soc_caps.h>
#include <stdbool.h>
#include <string.h>

#if !defined(CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN) || !CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN
#error "Cloud identity requires CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN"
#endif

#if !defined(CONFIG_MBEDTLS_ECDSA_DETERMINISTIC) || !CONFIG_MBEDTLS_ECDSA_DETERMINISTIC
#error "Cloud identity requires CONFIG_MBEDTLS_ECDSA_DETERMINISTIC"
#endif

static const char* tag = "SAIHUB-Cloud";

#define CLOUD_HASH_BYTES 32
#define CLOUD_KEY_GENERATION_ATTEMPTS 8
#define CLOUD_RNG_WARMUP_BYTES 256
#define CLOUD_CHALLENGE_MAX_BYTES 64
#define CLOUD_SIGNATURE_ALGORITHM PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)

static psa_key_id_t signingKey = PSA_KEY_ID_NULL;
static uint8_t publicKey[CLOUD_PUBLIC_KEY_BYTES];
static char publicKeyDigest[CLOUD_DIGEST_CHARS + 1];
static bool initialized;
static esp_err_t Cloud_StartRelay(void);

static void Cloud_SecureZero(void* data, size_t len)
{
  volatile uint8_t* bytes = (volatile uint8_t*)data;
  while (len-- > 0) {
    *bytes++ = 0;
  }
}

static void Cloud_Hex(const uint8_t* input, size_t inputLen, char* output)
{
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < inputLen; i++) {
    output[i * 2] = digits[input[i] >> 4];
    output[i * 2 + 1] = digits[input[i] & 0x0f];
  }
  output[inputLen * 2] = '\0';
}

static esp_err_t Cloud_Base58Check(const uint8_t* payload, size_t payloadLen, char* output, size_t outputCap)
{
  static const char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t checksumInput[CLOUD_HASH_BYTES] = {0};
  uint8_t checksum[CLOUD_HASH_BYTES] = {0};
  uint8_t encoded[CLOUD_DIGEST_PAYLOAD_BYTES + CLOUD_DIGEST_CHECKSUM_BYTES] = {0};
  uint8_t digits[CLOUD_DIGEST_CHARS] = {0};
  size_t checksumInputLen = 0;
  size_t checksumLen = 0;
  size_t digitsLen = 0;
  size_t leadingZeroes = 0;

  if (payload == NULL || output == NULL || payloadLen != CLOUD_DIGEST_PAYLOAD_BYTES || outputCap == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, payload, payloadLen, checksumInput, sizeof(checksumInput),
                                         &checksumInputLen);
  if (status != PSA_SUCCESS || checksumInputLen != sizeof(checksumInput)) return ESP_FAIL;
  status = psa_hash_compute(PSA_ALG_SHA_256, checksumInput, sizeof(checksumInput), checksum, sizeof(checksum),
                            &checksumLen);
  if (status != PSA_SUCCESS || checksumLen != sizeof(checksum)) {
    Cloud_SecureZero(checksumInput, sizeof(checksumInput));
    return ESP_FAIL;
  }

  memcpy(encoded, payload, payloadLen);
  memcpy(encoded + payloadLen, checksum, CLOUD_DIGEST_CHECKSUM_BYTES);
  Cloud_SecureZero(checksumInput, sizeof(checksumInput));
  Cloud_SecureZero(checksum, sizeof(checksum));

  while (leadingZeroes < sizeof(encoded) && encoded[leadingZeroes] == 0) leadingZeroes++;
  for (size_t i = leadingZeroes; i < sizeof(encoded); i++) {
    unsigned carry = encoded[i];
    for (size_t j = 0; j < digitsLen; j++) {
      unsigned value = (unsigned)digits[j] * 256U + carry;
      digits[j] = (uint8_t)(value % 58U);
      carry = value / 58U;
    }
    while (carry > 0) {
      if (digitsLen >= sizeof(digits)) return ESP_ERR_INVALID_SIZE;
      digits[digitsLen++] = (uint8_t)(carry % 58U);
      carry /= 58U;
    }
  }

  size_t outputLen = leadingZeroes + digitsLen;
  if (outputLen + 1 > outputCap) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i < leadingZeroes; i++) output[i] = '1';
  for (size_t i = 0; i < digitsLen; i++) output[leadingZeroes + i] = alphabet[digits[digitsLen - 1 - i]];
  output[outputLen] = '\0';
  return ESP_OK;
}

static psa_key_attributes_t Cloud_KeyAttributes(psa_key_usage_t usage, psa_key_lifetime_t lifetime)
{
  psa_key_attributes_t attributes = psa_key_attributes_init();
  psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attributes, 256);
  psa_set_key_algorithm(&attributes, CLOUD_SIGNATURE_ALGORITHM);
  psa_set_key_usage_flags(&attributes, usage);
  psa_set_key_lifetime(&attributes, lifetime);
  return attributes;
}

static void Cloud_DestroyKey(void)
{
  if (signingKey != PSA_KEY_ID_NULL) {
    psa_destroy_key(signingKey);
    signingKey = PSA_KEY_ID_NULL;
  }
}

static void Cloud_ClearIdentity(void)
{
  memset(publicKey, 0, sizeof(publicKey));
  publicKeyDigest[0] = '\0';
}

static esp_err_t Cloud_FindEfuseKey(esp_efuse_block_t* blockOut)
{
  if (blockOut == NULL) return ESP_ERR_INVALID_ARG;
  if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY, blockOut)) {
    ESP_LOGE(tag, "no P-256 ECDSA eFuse key found");
    return ESP_ERR_NOT_FOUND;
  }
  if (esp_efuse_get_key_purpose(*blockOut) != ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY ||
      !esp_efuse_get_key_dis_read(*blockOut) || !esp_efuse_get_key_dis_write(*blockOut)) {
    ESP_LOGE(tag, "ECDSA eFuse key block %d is not fully protected", (int)*blockOut);
    return ESP_ERR_INVALID_STATE;
  }
  ESP_LOGI(tag, "using ECDSA eFuse key block %d", (int)*blockOut);
  return ESP_OK;
}

static void Cloud_ReverseBytes(const uint8_t* input, uint8_t* output, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    output[i] = input[len - 1 - i];
  }
}

static void Cloud_WarmUpRng(void)
{
  /* Discard initial output after enabling the bootloader entropy source. */
  uint8_t discarded[CLOUD_RNG_WARMUP_BYTES];
  esp_fill_random(discarded, sizeof(discarded));
  Cloud_SecureZero(discarded, sizeof(discarded));
}

static esp_err_t Cloud_ProvisionEfuseKey(void)
{
  esp_efuse_block_t efuseBlock = esp_efuse_find_unused_key_block();
  if (efuseBlock == EFUSE_BLK_KEY_MAX) {
    ESP_LOGE(tag, "no unused eFuse key block available for ECDSA");
    return ESP_ERR_NOT_FOUND;
  }

  psa_key_id_t softwareKey = PSA_KEY_ID_NULL;
  uint8_t privateKey[32] = {0};
  uint8_t efuseKey[32] = {0};
  esp_err_t err = ESP_FAIL;

  psa_key_attributes_t attributes =
      Cloud_KeyAttributes(PSA_KEY_USAGE_SIGN_HASH, PSA_KEY_LIFETIME_VOLATILE);
  psa_status_t status = PSA_ERROR_INVALID_ARGUMENT;
  for (size_t attempt = 0; attempt < CLOUD_KEY_GENERATION_ATTEMPTS; attempt++) {
    esp_fill_random(privateKey, sizeof(privateKey));
    status = psa_import_key(&attributes, privateKey, sizeof(privateKey), &softwareKey);
    if (status != PSA_ERROR_INVALID_ARGUMENT) break;
  }
  psa_reset_key_attributes(&attributes);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(tag, "provisioning key validation failed: %d", (int)status);
    goto cleanup;
  }

  /* PSA imports P-256 private values big-endian; eFuse ECDSA expects little-endian. */
  Cloud_ReverseBytes(privateKey, efuseKey, sizeof(efuseKey));
  ESP_LOGW(tag, "provisioning ECDSA key into eFuse block %d; this is irreversible", (int)efuseBlock);
  err = esp_efuse_write_key(efuseBlock, ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY, efuseKey, sizeof(efuseKey));
  if (err != ESP_OK) {
    ESP_LOGE(tag, "ECDSA eFuse provisioning failed: %s", esp_err_to_name(err));
  }

cleanup:
  Cloud_SecureZero(privateKey, sizeof(privateKey));
  Cloud_SecureZero(efuseKey, sizeof(efuseKey));
  if (softwareKey != PSA_KEY_ID_NULL) psa_destroy_key(softwareKey);
  return err;
}

static esp_err_t Cloud_VerifyIdentity(void)
{
  uint8_t challenge[32] = {0};
  uint8_t challengeHash[CLOUD_HASH_BYTES] = {0};
  uint8_t signature[CLOUD_SIGNATURE_BYTES] = {0};
  size_t challengeHashLen = 0;
  size_t signatureLen = 0;
  psa_key_id_t verifyKey = PSA_KEY_ID_NULL;
  psa_key_attributes_t attributes = psa_key_attributes_init();
  psa_status_t status;
  esp_err_t err = ESP_FAIL;

  /* Cloud_Init keeps the bootloader entropy source active during this test. */
  esp_fill_random(challenge, sizeof(challenge));
  status = psa_hash_compute(PSA_ALG_SHA_256, challenge, sizeof(challenge), challengeHash, sizeof(challengeHash),
                            &challengeHashLen);
  if (status != PSA_SUCCESS || challengeHashLen != sizeof(challengeHash)) {
    ESP_LOGE(tag, "factory challenge hash failed: %d", (int)status);
    goto cleanup;
  }

  status = psa_sign_hash(signingKey, CLOUD_SIGNATURE_ALGORITHM, challengeHash, sizeof(challengeHash), signature,
                         sizeof(signature), &signatureLen);
  if (status != PSA_SUCCESS || signatureLen != sizeof(signature)) {
    ESP_LOGE(tag, "factory challenge signing failed: %d", (int)status);
    goto cleanup;
  }

  psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attributes, 256);
  psa_set_key_algorithm(&attributes, CLOUD_SIGNATURE_ALGORITHM);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
  status = psa_import_key(&attributes, publicKey, sizeof(publicKey), &verifyKey);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(tag, "public key self-test import failed: %d", (int)status);
    goto cleanup;
  }

  status = psa_verify_hash(verifyKey, CLOUD_SIGNATURE_ALGORITHM, challengeHash, sizeof(challengeHash), signature,
                           signatureLen);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(tag, "public key self-test verification failed: %d", (int)status);
    goto cleanup;
  }
  err = ESP_OK;

cleanup:
  if (verifyKey != PSA_KEY_ID_NULL) psa_destroy_key(verifyKey);
  psa_reset_key_attributes(&attributes);
  Cloud_SecureZero(challenge, sizeof(challenge));
  Cloud_SecureZero(challengeHash, sizeof(challengeHash));
  Cloud_SecureZero(signature, sizeof(signature));
  return err;
}

static esp_err_t Cloud_ImportHardwareKey(void)
{
  esp_efuse_block_t efuseBlock = EFUSE_BLK_KEY0;
  esp_err_t err = Cloud_FindEfuseKey(&efuseBlock);
  if (err != ESP_OK) return err;

  esp_ecdsa_opaque_key_t opaqueKey = {
      .curve = ESP_ECDSA_CURVE_SECP256R1,
#if SOC_KEY_MANAGER_SUPPORTED
      .key_recovery_info = NULL,
#endif
      .efuse_block = (uint8_t)efuseBlock,
  };

  psa_key_attributes_t attributes =
      Cloud_KeyAttributes(PSA_KEY_USAGE_SIGN_HASH, PSA_KEY_LIFETIME_ESP_ECDSA_VOLATILE);
  psa_status_t status = psa_import_key(&attributes, (const uint8_t*)&opaqueKey, sizeof(opaqueKey), &signingKey);
  psa_reset_key_attributes(&attributes);
  if (status != PSA_SUCCESS) {
    signingKey = PSA_KEY_ID_NULL;
    ESP_LOGE(tag, "ECDSA eFuse key import failed: %d", (int)status);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t Cloud_LoadPublicKey(void)
{
  size_t publicKeyLen = 0;
  psa_status_t status = psa_export_public_key(signingKey, publicKey, sizeof(publicKey), &publicKeyLen);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(tag, "public key export failed: %d", (int)status);
    return ESP_FAIL;
  }
  if (publicKeyLen != CLOUD_PUBLIC_KEY_BYTES || publicKey[0] != 0x04) {
    ESP_LOGE(tag, "public key export returned an invalid P-256 key");
    return ESP_FAIL;
  }

  uint8_t digest[CLOUD_HASH_BYTES] = {0};
  size_t digestLen = 0;
  status = psa_hash_compute(PSA_ALG_SHA_256, publicKey, publicKeyLen, digest, sizeof(digest), &digestLen);
  if (status != PSA_SUCCESS || digestLen != CLOUD_HASH_BYTES) {
    ESP_LOGE(tag, "public key digest failed: %d", (int)status);
    Cloud_SecureZero(digest, sizeof(digest));
    return ESP_FAIL;
  }
  esp_err_t err = Cloud_Base58Check(digest, CLOUD_DIGEST_PAYLOAD_BYTES, publicKeyDigest, sizeof(publicKeyDigest));
  Cloud_SecureZero(digest, sizeof(digest));
  if (err != ESP_OK) {
    ESP_LOGE(tag, "public key digest encoding failed: %s", esp_err_to_name(err));
    return err;
  }
  return ESP_OK;
}

static void Cloud_LogIdentity(void)
{
  char publicKeyHex[CLOUD_PUBLIC_KEY_BYTES * 2 + 1];
  Cloud_Hex(publicKey, sizeof(publicKey), publicKeyHex);
  ESP_LOGI(tag, "public key: %s", publicKeyHex);
  ESP_LOGI(tag, "public key digest: %s", publicKeyDigest);
}

esp_err_t Cloud_Init(void)
{
  bool entropyEnabled = false;
  bool provisioned = false;
  esp_err_t err = ESP_FAIL;

  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    ESP_LOGE(tag, "PSA crypto init failed: %d", (int)status);
    goto cleanup;
  }

  err = Cloud_ImportHardwareKey();
  if (err == ESP_ERR_NOT_FOUND) {
    bootloader_random_enable();
    entropyEnabled = true;
    Cloud_WarmUpRng();
    err = Cloud_ProvisionEfuseKey();
    if (err == ESP_OK) {
      provisioned = true;
      err = Cloud_ImportHardwareKey();
    }
  }
  if (err != ESP_OK) goto cleanup;

  err = Cloud_LoadPublicKey();
  if (err != ESP_OK) goto cleanup;

  if (provisioned) {
    err = Cloud_VerifyIdentity();
    if (err != ESP_OK) goto cleanup;
  }

  initialized = true;

cleanup:
  if (entropyEnabled) bootloader_random_disable();
  if (err == ESP_OK) {
    Cloud_LogIdentity();
    err = Cloud_StartRelay();
  } else {
    Cloud_DestroyKey();
    Cloud_ClearIdentity();
  }
  return err;
}

esp_err_t Cloud_GetPublicKey(uint8_t out[CLOUD_PUBLIC_KEY_BYTES], size_t* lenOut)
{
  if (!initialized) return ESP_ERR_INVALID_STATE;
  if (out == NULL) return ESP_ERR_INVALID_ARG;
  memcpy(out, publicKey, sizeof(publicKey));
  if (lenOut != NULL) *lenOut = sizeof(publicKey);
  return ESP_OK;
}

const char* Cloud_GetPublicKeyDigest(void)
{
  return publicKeyDigest;
}

esp_err_t Cloud_SignChallenge(const uint8_t* challenge, size_t challengeLen,
                              uint8_t signature[CLOUD_SIGNATURE_BYTES], size_t* signatureLenOut)
{
  if (signatureLenOut != NULL) *signatureLenOut = 0;
  if (!initialized) return ESP_ERR_INVALID_STATE;
  if (challenge == NULL || challengeLen == 0 || challengeLen > CLOUD_CHALLENGE_MAX_BYTES || signature == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(signature, 0, CLOUD_SIGNATURE_BYTES);

  /* signedMessage = UTF8(CLOUD_AUTH_DOMAIN) || 0x00 || challenge */
  const size_t domainLen = sizeof(CLOUD_AUTH_DOMAIN); /* includes trailing NUL */
  uint8_t signedMessage[sizeof(CLOUD_AUTH_DOMAIN) + CLOUD_CHALLENGE_MAX_BYTES];
  memcpy(signedMessage, CLOUD_AUTH_DOMAIN, domainLen);
  memcpy(signedMessage + domainLen, challenge, challengeLen);

  uint8_t challengeHash[CLOUD_HASH_BYTES] = {0};
  size_t challengeHashLen = 0;
  psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, signedMessage, domainLen + challengeLen, challengeHash,
                                         sizeof(challengeHash), &challengeHashLen);
  Cloud_SecureZero(signedMessage, domainLen + challengeLen);
  if (status != PSA_SUCCESS || challengeHashLen != CLOUD_HASH_BYTES) {
    ESP_LOGE(tag, "challenge hash failed: %d", (int)status);
    Cloud_SecureZero(challengeHash, sizeof(challengeHash));
    return ESP_FAIL;
  }

  size_t signatureLen = 0;
  status = psa_sign_hash(signingKey, CLOUD_SIGNATURE_ALGORITHM, challengeHash, sizeof(challengeHash), signature,
                         CLOUD_SIGNATURE_BYTES, &signatureLen);
  Cloud_SecureZero(challengeHash, sizeof(challengeHash));
  if (status != PSA_SUCCESS || signatureLen != CLOUD_SIGNATURE_BYTES) {
    ESP_LOGE(tag, "challenge signing failed: %d", (int)status);
    memset(signature, 0, CLOUD_SIGNATURE_BYTES);
    return ESP_FAIL;
  }
  if (signatureLenOut != NULL) *signatureLenOut = signatureLen;
  return ESP_OK;
}

static esp_websocket_client_handle_t client;
static QueueHandle_t incoming;
static SemaphoreHandle_t sendMutex;
static TimerHandle_t heartbeatTimer;
static atomic_uint generation;
static atomic_bool authenticated;
static atomic_bool restart;

static void Cloud_RequestRestart(const char* reason)
{
  ESP_LOGW(tag, "relay restart requested: %s", reason);
  atomic_store(&restart, true);
}
static _Atomic(TaskHandle_t) streamOwner;
static char* receiveBuffer;
static size_t receiveLength;
static unsigned receiveGeneration;

typedef struct { char* text; unsigned generation; } Cloud_Message;

static void Cloud_ResetReceive(void)
{
  free(receiveBuffer);
  receiveBuffer = NULL;
  receiveLength = 0;
}

static bool Cloud_ConnectionCurrent(unsigned expected)
{
  return expected == atomic_load(&generation) && !atomic_load(&restart) &&
         esp_websocket_client_is_connected(client);
}

/* One fragmented WebSocket message at a time, including async route responses. */
static esp_err_t Cloud_Write(void* user, HttpServer_CloudWriteKind kind, const void* data, size_t length)
{
  unsigned expected = (unsigned)(uintptr_t)user;
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (kind == HTTP_CLOUD_ABORT) {
    if (streamOwner == self) {
      Cloud_RequestRestart("cloud stream aborted");
      streamOwner = NULL;
      xSemaphoreGive(sendMutex);
    }
    return ESP_OK;
  }
  if (kind == HTTP_CLOUD_BEGIN) {
    if (xSemaphoreTake(sendMutex, pdMS_TO_TICKS(65000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!Cloud_ConnectionCurrent(expected) || !atomic_load(&authenticated)) {
      xSemaphoreGive(sendMutex);
      return ESP_ERR_INVALID_STATE;
    }
    streamOwner = self;
  } else if (streamOwner != self) return ESP_ERR_INVALID_STATE;

  if (!Cloud_ConnectionCurrent(expected)) return ESP_ERR_INVALID_STATE;
  int sent = kind == HTTP_CLOUD_BEGIN
      ? esp_websocket_client_send_text_partial(client, data, length, pdMS_TO_TICKS(10000))
      : esp_websocket_client_send_cont_msg(client, data, length, pdMS_TO_TICKS(10000));
  if (sent != (int)length) return ESP_FAIL;
  if (kind == HTTP_CLOUD_END) {
    if (esp_websocket_client_send_fin(client, pdMS_TO_TICKS(10000)) < 0) return ESP_FAIL;
    streamOwner = NULL;
    xSemaphoreGive(sendMutex);
  }
  return ESP_OK;
}

static bool Cloud_Base64Encode(const uint8_t* data, size_t length, char* out, size_t capacity)
{
  size_t written = 0;
  if (mbedtls_base64_encode((unsigned char*)out, capacity, &written, data, length) != 0) return false;
  while (written && out[written - 1] == '=') written--;
  out[written] = '\0';
  for (size_t i = 0; i < written; i++) {
    if (out[i] == '+') out[i] = '-';
    if (out[i] == '/') out[i] = '_';
  }
  return true;
}

static void Cloud_Authenticate(cJSON* message, unsigned expected)
{
  cJSON* challenge = cJSON_GetObjectItemCaseSensitive(message, "challenge");
  if (!cJSON_IsString(challenge) || strlen(challenge->valuestring) != 43) { Cloud_RequestRestart("invalid authentication challenge"); return; }
  char encoded[45];
  memcpy(encoded, challenge->valuestring, 43);
  for (size_t i = 0; i < 43; i++) {
    if (encoded[i] == '-') encoded[i] = '+';
    if (encoded[i] == '_') encoded[i] = '/';
  }
  encoded[43] = '=';
  encoded[44] = '\0';
  uint8_t bytes[32], signature[CLOUD_SIGNATURE_BYTES], publicKey[CLOUD_PUBLIC_KEY_BYTES];
  size_t length = 0;
  if (mbedtls_base64_decode(bytes, sizeof(bytes), &length, (unsigned char*)encoded, 44) != 0 || length != 32 ||
      Cloud_SignChallenge(bytes, length, signature, NULL) != ESP_OK || Cloud_GetPublicKey(publicKey, NULL) != ESP_OK) {
    Cloud_RequestRestart("authentication challenge signing failed");
    return;
  }
  char signatureText[89], publicKeyText[89];
  if (!Cloud_Base64Encode(signature, sizeof(signature), signatureText, sizeof(signatureText)) ||
      !Cloud_Base64Encode(publicKey, sizeof(publicKey), publicKeyText, sizeof(publicKeyText))) return;
  cJSON* response = cJSON_CreateObject();
  if (!response) return;
  cJSON_AddStringToObject(response, "type", "authResponse");
  cJSON_AddStringToObject(response, "devicePublicKey", publicKeyText);
  cJSON_AddStringToObject(response, "devicePublicKeyDigest", Cloud_GetPublicKeyDigest());
  cJSON_AddStringToObject(response, "version", esp_app_get_description()->version);
  cJSON_AddStringToObject(response, "response", signatureText);
  char* text = cJSON_PrintUnformatted(response);
  cJSON_Delete(response);
  if (!text) return;
  if (xSemaphoreTake(sendMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (Cloud_ConnectionCurrent(expected) && esp_websocket_client_send_text(client, text, strlen(text), pdMS_TO_TICKS(10000)) < 0)
      Cloud_RequestRestart("cloud authentication response send failed");
    xSemaphoreGive(sendMutex);
  } else Cloud_RequestRestart("cloud authentication response send lock timeout");
  free(text);
}

static void Cloud_Event(void* arg, esp_event_base_t base, int32_t event, void* eventData)
{
  (void)arg;
  (void)base;
  if (event == WEBSOCKET_EVENT_CONNECTED || event == WEBSOCKET_EVENT_DISCONNECTED || event == WEBSOCKET_EVENT_CLOSED) {
    atomic_fetch_add(&generation, 1);
    atomic_store(&authenticated, false);
    Cloud_ResetReceive();
    if (event == WEBSOCKET_EVENT_CLOSED) Cloud_RequestRestart("cloud socket closed");
    return;
  }
  if (event != WEBSOCKET_EVENT_DATA) return;
  esp_websocket_event_data_t* data = eventData;
  if (data->op_code != 1 && data->op_code != 0) return; /* Control frames may interrupt fragments. */
  if (data->op_code == 1 && data->payload_offset == 0) {
    Cloud_ResetReceive();
    receiveGeneration = atomic_load(&generation);
    receiveBuffer = malloc(1);
  }
  if (!receiveBuffer || data->data_len < 0 || receiveLength + data->data_len > CONFIG_CLOUD_MAX_MESSAGE_BYTES) {
    Cloud_ResetReceive();
    Cloud_RequestRestart("cloud message buffer invalid or too large");
    return;
  }
  char* grown = realloc(receiveBuffer, receiveLength + data->data_len + 1);
  if (!grown) { Cloud_ResetReceive(); Cloud_RequestRestart("cloud message buffer allocation failed"); return; }
  receiveBuffer = grown;
  memcpy(receiveBuffer + receiveLength, data->data_ptr, data->data_len);
  receiveLength += data->data_len;
  receiveBuffer[receiveLength] = '\0';
  if (!data->fin || data->payload_offset + data->data_len != data->payload_len) return;
  Cloud_Message message = {.text = receiveBuffer, .generation = receiveGeneration};
  if (xQueueSend(incoming, &message, 0) != pdTRUE) { free(message.text); Cloud_RequestRestart("cloud message queue full"); }
  receiveBuffer = NULL;
  receiveLength = 0;
}

/* Timer callbacks must not wait on a streamed response or network writes. */
static void Cloud_HeartbeatTimer(TimerHandle_t timer)
{
  (void)timer;
  unsigned expected = atomic_load(&generation);
  if (!Cloud_ConnectionCurrent(expected)) return;
  if (xSemaphoreTake(sendMutex, 0) != pdTRUE) return;
  if (Cloud_ConnectionCurrent(expected) &&
      esp_websocket_client_send_text(client, "ping", 4, 0) != 4)
    Cloud_RequestRestart("cloud heartbeat send failed");
  xSemaphoreGive(sendMutex);
}

static void Cloud_RelayTask(void* arg)
{
  (void)arg;
  xTimerStart(heartbeatTimer, portMAX_DELAY);
  bool running = false;
  for (;;) {
    bool ready = Wifi_IsConnected() && !Wifi_IsPairing() && Ntp_IsSynced();
    if (running && (!ready || atomic_load(&restart))) {
      atomic_store(&authenticated, false);
      atomic_fetch_add(&generation, 1);
      /* Writers notice the generation change and release their fragment lock. */
      xSemaphoreTake(sendMutex, portMAX_DELAY);
      esp_websocket_client_stop(client);
      xSemaphoreGive(sendMutex);
      Cloud_ResetReceive();
      running = false;
      vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (!running && ready) {
      atomic_store(&restart, false);
      running = esp_websocket_client_start(client) == ESP_OK;
    }
    Cloud_Message message;
    if (xQueueReceive(incoming, &message, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    if (!Cloud_ConnectionCurrent(message.generation)) { free(message.text); continue; }
    if (strcmp(message.text, "pong") == 0) { free(message.text); continue; }
    cJSON* root = cJSON_Parse(message.text);
    cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    bool dispatched = false;
    if (cJSON_IsString(type)) {
      if (strcmp(type->valuestring, "authRequest") == 0 && !atomic_load(&authenticated)) Cloud_Authenticate(root, message.generation);
      else if (strcmp(type->valuestring, "authResult") == 0) {
        bool success = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success"));
        atomic_store(&authenticated, success);
        if (!success) Cloud_RequestRestart("cloud authentication rejected");
        ESP_LOGI(tag, "cloud authentication %s", success ? "ready" : "rejected");
      } else if (strcmp(type->valuestring, "request") == 0 && atomic_load(&authenticated)) {
        /* Match the local HTTP server: dispatch synchronously on the relay task.
         * Cloud-side timeout handling bounds how long this can occupy the relay. */
        if (Cloud_ConnectionCurrent(message.generation)) {
          HttpServer_DispatchCloud(root, Cloud_Write, (void*)(uintptr_t)message.generation);
          dispatched = true;
        }
        if (!dispatched) Cloud_RequestRestart("cloud request dispatch failed");
      }
    }
    if (!dispatched) cJSON_Delete(root);
    free(message.text);
  }
}

static esp_err_t Cloud_StartRelay(void)
{
  if (CONFIG_CLOUD_URL[0] == '\0') { ESP_LOGI(tag, "cloud relay disabled; set CONFIG_CLOUD_URL"); return ESP_OK; }
  if (strncmp(CONFIG_CLOUD_URL, "wss://", 6) != 0 && strncmp(CONFIG_CLOUD_URL, "ws://", 5) != 0) {
    ESP_LOGE(tag, "CONFIG_CLOUD_URL must start with ws:// or wss://");
    return ESP_ERR_INVALID_ARG;
  }
  if (!Cloud_GetPublicKeyDigest()[0]) return ESP_ERR_INVALID_STATE;
  char uri[512];
  int length = snprintf(uri, sizeof(uri), "%s/cloud/device/%s", CONFIG_CLOUD_URL, Cloud_GetPublicKeyDigest());
  if (length < 0 || length >= sizeof(uri)) return ESP_ERR_INVALID_SIZE;
  incoming = xQueueCreate(8, sizeof(Cloud_Message));
  sendMutex = xSemaphoreCreateMutex();
  if (!incoming || !sendMutex) goto failed;
  esp_websocket_client_config_t config = {
    .uri = uri, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048,
    .task_stack = 6144, .reconnect_timeout_ms = 3000, .network_timeout_ms = 10000,
  };
  client = esp_websocket_client_init(&config);
  if (!client) goto failed;
  if (esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, Cloud_Event, NULL) != ESP_OK) goto failed;
  heartbeatTimer = xTimerCreate("cloud-heartbeat", pdMS_TO_TICKS(10000), pdTRUE, NULL, Cloud_HeartbeatTimer);
  if (!heartbeatTimer) goto failed;
  if (xTaskCreate(Cloud_RelayTask, "cloud-relay", 6144, NULL, 5, NULL) != pdPASS) goto failed;
  return ESP_OK;
failed:
  if (heartbeatTimer) { xTimerDelete(heartbeatTimer, portMAX_DELAY); heartbeatTimer = NULL; }
  if (client) esp_websocket_client_destroy(client);
  if (incoming) vQueueDelete(incoming);
  if (sendMutex) vSemaphoreDelete(sendMutex);
  return ESP_ERR_NO_MEM;
}
