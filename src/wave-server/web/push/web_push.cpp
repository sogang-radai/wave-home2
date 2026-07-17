#include "web_push.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <json/json.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

#include "../../core/logger.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace push {
namespace
{
    constexpr size_t kAesKeyLen = 16;
    constexpr size_t kNonceLen = 12;
    constexpr size_t kSaltLen = 16;
    constexpr size_t kRecordSize = 4096;

    std::string base64_url_encode(const uint8_t* data, size_t len)
    {
        static const char kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((len + 2) / 3 * 4);

        size_t i = 0;
        while (i + 2 < len)
        {
            const uint32_t n = (static_cast<uint32_t>(data[i]) << 16)
                | (static_cast<uint32_t>(data[i + 1]) << 8)
                | static_cast<uint32_t>(data[i + 2]);
            out.push_back(kTable[(n >> 18) & 0x3F]);
            out.push_back(kTable[(n >> 12) & 0x3F]);
            out.push_back(kTable[(n >> 6) & 0x3F]);
            out.push_back(kTable[n & 0x3F]);
            i += 3;
        }

        if (i < len)
        {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            out.push_back(kTable[(n >> 18) & 0x3F]);
            if (i + 1 < len)
            {
                n |= static_cast<uint32_t>(data[i + 1]) << 8;
                out.push_back(kTable[(n >> 12) & 0x3F]);
                out.push_back(kTable[(n >> 6) & 0x3F]);
            }
            else
            {
                out.push_back(kTable[(n >> 12) & 0x3F]);
            }
        }

        for (auto& ch : out)
        {
            if (ch == '+') ch = '-';
            else if (ch == '/') ch = '_';
        }
        while (!out.empty() && out.back() == '=')
            out.pop_back();
        return out;
    }

    std::vector<uint8_t> base64_url_decode(const std::string& in)
    {
        std::string padded = in;
        for (auto& ch : padded)
        {
            if (ch == '-') ch = '+';
            else if (ch == '_') ch = '/';
        }
        while (padded.size() % 4 != 0)
            padded.push_back('=');

        std::vector<uint8_t> out((padded.size() / 4) * 3);
        const int len = EVP_DecodeBlock(out.data(),
            reinterpret_cast<const unsigned char*>(padded.data()),
            static_cast<int>(padded.size()));
        if (len < 0)
            return {};

        size_t pad = 0;
        if (!padded.empty() && padded[padded.size() - 1] == '=') pad++;
        if (padded.size() > 1 && padded[padded.size() - 2] == '=') pad++;
        if (out.size() >= pad)
            out.resize(out.size() - pad);
        return out;
    }

    bool hkdf_sha256(
        const uint8_t* secret,
        size_t secret_len,
        const uint8_t* salt,
        size_t salt_len,
        const uint8_t* info,
        size_t info_len,
        uint8_t* out,
        size_t out_len)
    {
        std::array<uint8_t, 32> prk {};
        if (HMAC(EVP_sha256(), salt, static_cast<int>(salt_len), secret, secret_len, prk.data(), nullptr) == nullptr)
            return false;

        size_t offset = 0;
        uint8_t counter = 1;
        std::vector<uint8_t> previous;

        while (offset < out_len)
        {
            std::vector<uint8_t> input;
            input.insert(input.end(), previous.begin(), previous.end());
            if (info_len > 0)
                input.insert(input.end(), info, info + info_len);
            input.push_back(counter);

            std::array<uint8_t, 32> block {};
            if (HMAC(EVP_sha256(), prk.data(), prk.size(), input.data(), input.size(), block.data(), nullptr) == nullptr)
                return false;

            const size_t copy_len = std::min(out_len - offset, block.size());
            std::memcpy(out + offset, block.data(), copy_len);
            offset += copy_len;
            previous.assign(block.begin(), block.end());
            counter += 1;
        }

        return true;
    }

    EVP_PKEY* import_private_key_p256(const std::vector<uint8_t>& private_key)
    {
        if (private_key.size() != 32)
            return nullptr;

        OSSL_PARAM_BLD* param_bld = OSSL_PARAM_BLD_new();
        if (!param_bld)
            return nullptr;

        OSSL_PARAM_BLD_push_utf8_string(param_bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
        OSSL_PARAM_BLD_push_octet_string(param_bld, OSSL_PKEY_PARAM_PRIV_KEY, private_key.data(), private_key.size());

        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(param_bld);
        OSSL_PARAM_BLD_free(param_bld);
        if (!params)
            return nullptr;

        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (!ctx)
        {
            OSSL_PARAM_free(params);
            return nullptr;
        }

        EVP_PKEY* pkey = nullptr;
        if (EVP_PKEY_fromdata_init(ctx) > 0)
            EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params);

        EVP_PKEY_CTX_free(ctx);
        OSSL_PARAM_free(params);
        return pkey;
    }

    EVP_PKEY* import_public_key_p256(const std::vector<uint8_t>& public_key)
    {
        if (public_key.size() != 65 || public_key[0] != 0x04)
            return nullptr;

        OSSL_PARAM_BLD* param_bld = OSSL_PARAM_BLD_new();
        if (!param_bld)
            return nullptr;

        OSSL_PARAM_BLD_push_utf8_string(param_bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
        OSSL_PARAM_BLD_push_octet_string(param_bld, OSSL_PKEY_PARAM_PUB_KEY, public_key.data(), public_key.size());

        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(param_bld);
        OSSL_PARAM_BLD_free(param_bld);
        if (!params)
            return nullptr;

        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (!ctx)
        {
            OSSL_PARAM_free(params);
            return nullptr;
        }

        EVP_PKEY* pkey = nullptr;
        if (EVP_PKEY_fromdata_init(ctx) > 0)
            EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);

        EVP_PKEY_CTX_free(ctx);
        OSSL_PARAM_free(params);
        return pkey;
    }

    std::vector<uint8_t> export_uncompressed_public_key(EVP_PKEY* pkey)
    {
        size_t len = 0;
        if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &len) <= 0)
            return {};

        std::vector<uint8_t> out(len);
        if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, out.data(), len, &len) <= 0)
            return {};
        out.resize(len);
        return out;
    }

    std::vector<uint8_t> ecdh_secret(EVP_PKEY* local, EVP_PKEY* peer)
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(local, nullptr);
        if (!ctx)
            return {};
        if (EVP_PKEY_derive_init(ctx) <= 0)
        {
            EVP_PKEY_CTX_free(ctx);
            return {};
        }
        if (EVP_PKEY_derive_set_peer(ctx, peer) <= 0)
        {
            EVP_PKEY_CTX_free(ctx);
            return {};
        }

        size_t len = 0;
        if (EVP_PKEY_derive(ctx, nullptr, &len) <= 0)
        {
            EVP_PKEY_CTX_free(ctx);
            return {};
        }

        std::vector<uint8_t> secret(len);
        if (EVP_PKEY_derive(ctx, secret.data(), &len) <= 0)
            secret.clear();
        else
            secret.resize(len);

        EVP_PKEY_CTX_free(ctx);
        return secret;
    }

    std::string jwt_es256(const std::string& signing_input, EVP_PKEY* private_key)
    {
        EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
        if (!md_ctx)
            return {};

        std::string signature;
        if (EVP_DigestSignInit(md_ctx, nullptr, EVP_sha256(), nullptr, private_key) <= 0)
        {
            EVP_MD_CTX_free(md_ctx);
            return {};
        }

        size_t sig_len = 0;
        if (EVP_DigestSign(md_ctx, nullptr, &sig_len,
                reinterpret_cast<const unsigned char*>(signing_input.data()),
                signing_input.size()) <= 0)
        {
            EVP_MD_CTX_free(md_ctx);
            return {};
        }

        std::vector<uint8_t> der(sig_len);
        if (EVP_DigestSign(md_ctx,
                der.data(),
                &sig_len,
                reinterpret_cast<const unsigned char*>(signing_input.data()),
                signing_input.size()) <= 0)
        {
            EVP_MD_CTX_free(md_ctx);
            return {};
        }
        der.resize(sig_len);
        EVP_MD_CTX_free(md_ctx);

        const unsigned char* ptr = der.data();
        ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &ptr, static_cast<long>(der.size()));
        if (!sig)
            return {};

        const BIGNUM* r = nullptr;
        const BIGNUM* s = nullptr;
        ECDSA_SIG_get0(sig, &r, &s);

        std::array<uint8_t, 64> raw {};
        BN_bn2binpad(r, raw.data(), 32);
        BN_bn2binpad(s, raw.data() + 32, 32);
        ECDSA_SIG_free(sig);

        return base64_url_encode(raw.data(), raw.size());
    }

    std::string extract_audience(const std::string& endpoint)
    {
        const auto scheme_end = endpoint.find("://");
        if (scheme_end == std::string::npos)
            return endpoint;

        const auto host_start = scheme_end + 3;
        const auto path_start = endpoint.find('/', host_start);
        if (path_start == std::string::npos)
            return endpoint;
        return endpoint.substr(0, path_start);
    }

    std::string build_vapid_authorization(
        const std::string& audience,
        const VapidConfig& vapid,
        EVP_PKEY* private_key)
    {
        const std::string header = base64_url_encode(
            reinterpret_cast<const uint8_t*>(R"({"typ":"JWT","alg":"ES256"})"),
            strlen(R"({"typ":"JWT","alg":"ES256"})"));

        Json::Value claims;
        claims["aud"] = audience;
        claims["exp"] = static_cast<Json::Int64>(std::time(nullptr) + 12 * 3600);
        claims["sub"] = vapid.subject;
        const std::string payload_json = Json::writeString(Json::StreamWriterBuilder(), claims);
        const std::string payload = base64_url_encode(
            reinterpret_cast<const uint8_t*>(payload_json.data()),
            payload_json.size());

        const std::string signing_input = header + "." + payload;
        const std::string signature = jwt_es256(signing_input, private_key);
        if (signature.empty())
            return {};

        return "vapid t=" + header + "." + payload + "." + signature + ", k=" + vapid.public_key;
    }

    std::vector<uint8_t> encrypt_payload(
        const std::vector<uint8_t>& plaintext,
        const std::vector<uint8_t>& client_public,
        const std::vector<uint8_t>& auth_secret)
    {
        std::array<uint8_t, kSaltLen> salt {};
        if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
            return {};

        EVP_PKEY_CTX* key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (!key_ctx)
            return {};
        EVP_PKEY* local_key = nullptr;
        if (EVP_PKEY_keygen_init(key_ctx) <= 0 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_ctx, NID_X9_62_prime256v1) <= 0
            || EVP_PKEY_keygen(key_ctx, &local_key) <= 0)
        {
            EVP_PKEY_CTX_free(key_ctx);
            return {};
        }
        EVP_PKEY_CTX_free(key_ctx);

        EVP_PKEY* client_key = import_public_key_p256(client_public);
        if (!client_key)
        {
            EVP_PKEY_free(local_key);
            return {};
        }

        const auto shared_secret = ecdh_secret(local_key, client_key);
        EVP_PKEY_free(client_key);
        if (shared_secret.empty())
        {
            EVP_PKEY_free(local_key);
            return {};
        }

        const auto local_public = export_uncompressed_public_key(local_key);
        EVP_PKEY_free(local_key);
        if (local_public.empty())
            return {};

        std::string auth_info = "WebPush: info";
        auth_info.push_back('\0');
        auth_info.append(reinterpret_cast<const char*>(client_public.data()), client_public.size());
        auth_info.append(reinterpret_cast<const char*>(local_public.data()), local_public.size());

        std::array<uint8_t, 32> ikm {};
        if (!hkdf_sha256(
                auth_secret.data(),
                auth_secret.size(),
                shared_secret.data(),
                shared_secret.size(),
                reinterpret_cast<const uint8_t*>(auth_info.data()),
                auth_info.size(),
                ikm.data(),
                ikm.size()))
            return {};

        const std::string key_info = "Content-Encoding: aes128gcm\0";
        const std::string nonce_info = "Content-Encoding: nonce\0";

        std::array<uint8_t, kAesKeyLen> content_key {};
        std::array<uint8_t, kNonceLen> nonce {};
        if (!hkdf_sha256(ikm.data(), ikm.size(), salt.data(), salt.size(),
                reinterpret_cast<const uint8_t*>(key_info.data()), key_info.size(),
                content_key.data(), content_key.size()))
            return {};
        if (!hkdf_sha256(ikm.data(), ikm.size(), salt.data(), salt.size(),
                reinterpret_cast<const uint8_t*>(nonce_info.data()), nonce_info.size(),
                nonce.data(), nonce.size()))
            return {};

        std::vector<uint8_t> padded = plaintext;
        padded.push_back(0x02);

        std::vector<uint8_t> ciphertext(padded.size() + 16);
        int out_len = 0;
        EVP_CIPHER_CTX* cipher_ctx = EVP_CIPHER_CTX_new();
        if (!cipher_ctx)
            return {};

        bool ok = EVP_EncryptInit_ex(cipher_ctx, EVP_aes_128_gcm(), nullptr, content_key.data(), nonce.data()) == 1
            && EVP_EncryptUpdate(cipher_ctx, ciphertext.data(), &out_len, padded.data(), static_cast<int>(padded.size())) == 1;

        int final_len = 0;
        ok = ok && EVP_EncryptFinal_ex(cipher_ctx, ciphertext.data() + out_len, &final_len) == 1;

        std::array<uint8_t, 16> tag {};
        ok = ok && EVP_CIPHER_CTX_ctrl(cipher_ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1;
        EVP_CIPHER_CTX_free(cipher_ctx);
        if (!ok)
            return {};

        ciphertext.resize(static_cast<size_t>(out_len + final_len));
        ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());

        std::vector<uint8_t> body;
        body.reserve(kSaltLen + 4 + 1 + local_public.size() + ciphertext.size());
        body.insert(body.end(), salt.begin(), salt.end());

        const uint32_t rs_be = static_cast<uint32_t>(kRecordSize);
        body.push_back(static_cast<uint8_t>((rs_be >> 24) & 0xFF));
        body.push_back(static_cast<uint8_t>((rs_be >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((rs_be >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(rs_be & 0xFF));

        body.push_back(static_cast<uint8_t>(local_public.size()));
        body.insert(body.end(), local_public.begin(), local_public.end());
        body.insert(body.end(), ciphertext.begin(), ciphertext.end());
        return body;
    }
}

std::optional<PreparedRequest> prepareRequest(
    const Subscription& subscription,
    const VapidConfig& vapid,
    const Message& message)
{
    if (subscription.endpoint.empty() || vapid.public_key.empty() || vapid.private_key.empty())
        return std::nullopt;

    const auto client_public = base64_url_decode(subscription.p256dh);
    const auto auth_secret = base64_url_decode(subscription.auth);
    const auto private_key_bytes = base64_url_decode(vapid.private_key);
    if (client_public.empty() || auth_secret.empty() || private_key_bytes.empty())
    {
        WLOG_ERROR("Invalid push subscription or VAPID key encoding");
        return std::nullopt;
    }

    EVP_PKEY* private_key = import_private_key_p256(private_key_bytes);
    if (!private_key)
    {
        WLOG_ERROR("Failed to import VAPID private key");
        return std::nullopt;
    }

    Json::Value payload;
    payload["title"] = message.title;
    payload["body"] = message.body;
    payload["url"] = message.url;
    const std::string payload_json = Json::writeString(Json::StreamWriterBuilder(), payload);
    const std::vector<uint8_t> plaintext(payload_json.begin(), payload_json.end());

    const auto body = encrypt_payload(plaintext, client_public, auth_secret);
    if (body.empty())
    {
        EVP_PKEY_free(private_key);
        WLOG_ERROR("Failed to encrypt web push payload");
        return std::nullopt;
    }

    const std::string audience = extract_audience(subscription.endpoint);
    const std::string authorization = build_vapid_authorization(audience, vapid, private_key);
    EVP_PKEY_free(private_key);
    if (authorization.empty())
    {
        WLOG_ERROR("Failed to build VAPID authorization header");
        return std::nullopt;
    }

    const size_t id_len_offset = kSaltLen + 4;
    const uint8_t id_len = body[id_len_offset];
    const uint8_t* server_public = body.data() + id_len_offset + 1;
    const std::string dh_header = base64_url_encode(server_public, id_len);

    PreparedRequest request;
    request.endpoint = subscription.endpoint;
    request.body = body;
    request.headers["Authorization"] = authorization;
    request.headers["Crypto-Key"] = "dh=" + dh_header;
    request.headers["Encryption"] = "aes128gcm";
    request.headers["Content-Encoding"] = "aes128gcm";
    request.headers["Content-Type"] = "application/octet-stream";
    request.headers["TTL"] = "60";
    request.headers["Urgency"] = "normal";
    return request;
}

} // namespace push
} // namespace web
WAVE_NAMESPACE_END
