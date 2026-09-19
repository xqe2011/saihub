/**
 * @name Cloud identity
 * @file cloud.c
 * @author xqe2011
 */
#include "cloud.h"

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
#define CLOUD_SIGNATURE_ALGORITHM PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)

static psa_key_id_t signingKey = PSA_KEY_ID_NULL;
static uint8_t publicKey[CLOUD_PUBLIC_KEY_BYTES];
static char publicKeyDigest[CLOUD_DIGEST_CHARS + 1];
static bool initialized;

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
  if (challenge == NULL || challengeLen == 0 || signature == NULL) return ESP_ERR_INVALID_ARG;

  memset(signature, 0, CLOUD_SIGNATURE_BYTES);

  uint8_t challengeHash[CLOUD_HASH_BYTES] = {0};
  size_t challengeHashLen = 0;
  psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, challenge, challengeLen, challengeHash,
                                         sizeof(challengeHash), &challengeHashLen);
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
