// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <httplib.h>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/ssl2.h"
#include "core/libraries/network/ssl2_error.h"

namespace Libraries::Ssl2 {

namespace {
struct CertificateBlock {
    std::vector<std::vector<u8>> bytes;
    std::vector<OrbisSslData> entries;
    u64 size{};
};

struct SslContext {
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> tls{nullptr, SSL_CTX_free};
    std::unordered_map<void*, std::unique_ptr<CertificateBlock>> certificates;
    u64 pool_size{};
    u64 in_use{};
    u64 peak_in_use{};
};

std::mutex context_mutex;
std::unordered_map<s32, SslContext> contexts;
s32 next_context_id{};
} // namespace

int PS4_SYSV_ABI CA_MGMT_extractKeyBlobEx() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI CA_MGMT_extractPublicKeyInfo() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI CA_MGMT_freeKeyBlob() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI CRYPTO_initAsymmetricKey() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI CRYPTO_uninitAsymmetricKey() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI RSA_verifySignature() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslCheckRecvPending() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslClose() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslConnect() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslCreateConnection() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslCreateSslConnection() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslDeleteConnection() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslDeleteSslConnection() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslDisableOption() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslDisableOptionInternal() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslDisableOptionInternalInsecure() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslDisableVerifyOption() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslEnableOption() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslEnableOptionInternal() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslEnableVerifyOption() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslFreeCaCerts(s32 context_id, OrbisSslCaCerts* certs) {
    if (!certs) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    std::scoped_lock lock{context_mutex};
    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    const auto allocation = context->second.certificates.find(certs->pool);
    if (allocation == context->second.certificates.end() ||
        allocation->second->entries.data() != certs->certs) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    context->second.in_use -= allocation->second->size;
    context->second.certificates.erase(allocation);
    *certs = {};
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslFreeCaList() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslFreeSslCertName() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetAlpnSelected() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetCaCerts(s32 context_id, OrbisSslCaCerts* certs) {
    if (!certs) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    std::scoped_lock lock{context_mutex};
    const auto entry = contexts.find(context_id);
    if (entry == contexts.end()) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    auto& context = entry->second;
    try {
        auto block = std::make_unique<CertificateBlock>();
        const auto* certificates =
            X509_STORE_get0_objects(SSL_CTX_get_cert_store(context.tls.get()));
        const u64 available = context.pool_size - context.in_use;
        for (int i = 0; i < sk_X509_OBJECT_num(certificates); ++i) {
            const auto* object = sk_X509_OBJECT_value(certificates, i);
            if (X509_OBJECT_get_type(object) != X509_LU_X509) {
                continue;
            }
            auto* certificate = X509_OBJECT_get0_X509(object);
            const int size = i2d_X509(certificate, nullptr);
            if (size <= 0) {
                continue;
            }
            const u64 allocation = sizeof(OrbisSslData) + static_cast<u64>(size);
            if (allocation > available - block->size) {
                return ORBIS_KERNEL_ERROR_ENOMEM;
            }
            auto& bytes = block->bytes.emplace_back(size);
            auto* destination = bytes.data();
            if (i2d_X509(certificate, &destination) != size) {
                return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
            }
            block->size += allocation;
        }
        for (auto& bytes : block->bytes) {
            block->entries.push_back({reinterpret_cast<char*>(bytes.data()), bytes.size()});
        }
        OrbisSslCaCerts result{block->entries.data(), block->entries.size(), block.get()};
        const auto bytes = block->size;
        context.certificates.emplace(block.get(), std::move(block));
        context.in_use += bytes;
        context.peak_in_use = std::max(context.peak_in_use, context.in_use);
        *certs = result;
        return ORBIS_OK;
    } catch (const std::bad_alloc&) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
}

int PS4_SYSV_ABI sceSslGetCaList() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetFingerprint() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetIssuerName() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetMemoryPoolStats(s32 context_id, OrbisSslMemoryPoolStats* stats) {
    if (!stats) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    std::scoped_lock lock{context_mutex};
    const auto entry = contexts.find(context_id);
    if (entry == contexts.end()) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    const auto& context = entry->second;
    *stats = {context.pool_size, context.peak_in_use, context.in_use, 0};
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetNameEntryCount() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetNameEntryInfo() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetNanoSSLModuleId() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetNotAfter() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetNotBefore() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetPeerCert() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetPem() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetSerialNumber() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetSslError() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslGetSubjectName() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslInit(std::size_t pool_size) {
    if (!pool_size) {
        return ORBIS_SSL_ERROR_INVALID_ARGUMENT;
    }
    try {
        SslContext context;
        context.tls.reset(SSL_CTX_new(TLS_client_method()));
        if (!context.tls) {
            return ORBIS_KERNEL_ERROR_ENOMEM;
        }
        SSL_CTX_set_verify(context.tls.get(), SSL_VERIFY_PEER, nullptr);
        if (!httplib::tls::load_system_certs(context.tls.get())) {
            LOG_WARNING(Lib_Ssl2, "No host trust certificates available for SSL context");
        }
        context.pool_size = pool_size;
        std::scoped_lock lock{context_mutex};
        if (next_context_id == std::numeric_limits<s32>::max()) {
            return ORBIS_KERNEL_ERROR_ENOMEM;
        }
        const s32 id = next_context_id + 1;
        contexts.emplace(id, std::move(context));
        next_context_id = id;
        LOG_INFO(Lib_Ssl2, "Initialized SSL context {}, pool size {}", id, pool_size);
        return id;
    } catch (const std::bad_alloc&) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
}

int PS4_SYSV_ABI sceSslLoadCert() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslLoadRootCACert() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslRead() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslRecv() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslReuseConnection() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslSend() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslSetAlpn() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslSetMinSslVersion() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslSetSslVersion() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslSetVerifyCallback() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslTerm(s32 context_id) {
    std::scoped_lock lock{context_mutex};
    return contexts.erase(context_id) ? ORBIS_OK : ORBIS_SSL_ERROR_INVALID_ARGUMENT;
}

int PS4_SYSV_ABI sceSslUnloadCert() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceSslWrite() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI VLONG_freeVlongQueue() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_22E76E60BC0587D7() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_28F8791A771D39C7() {
    LOG_ERROR(Lib_Ssl2, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    {
        std::scoped_lock lock{context_mutex};
        contexts.clear();
        next_context_id = 0;
    }
    LIB_FUNCTION("Md+HYkCBZB4", "libSceSsl", 1, "libSceSsl", CA_MGMT_extractKeyBlobEx);
    LIB_FUNCTION("9bKYzKP6kYU", "libSceSsl", 1, "libSceSsl", CA_MGMT_extractPublicKeyInfo);
    LIB_FUNCTION("ipLIammTj2Q", "libSceSsl", 1, "libSceSsl", CA_MGMT_freeKeyBlob);
    LIB_FUNCTION("PRWr3-ytpdg", "libSceSsl", 1, "libSceSsl", CRYPTO_initAsymmetricKey);
    LIB_FUNCTION("cW7VCIMCh9A", "libSceSsl", 1, "libSceSsl", CRYPTO_uninitAsymmetricKey);
    LIB_FUNCTION("pBwtarKd7eg", "libSceSsl", 1, "libSceSsl", RSA_verifySignature);
    LIB_FUNCTION("1VM0h1JrUfA", "libSceSsl", 1, "libSceSsl", sceSslCheckRecvPending);
    LIB_FUNCTION("viRXSHZYd0c", "libSceSsl", 1, "libSceSsl", sceSslClose);
    LIB_FUNCTION("zXvd6iNyfgc", "libSceSsl", 1, "libSceSsl", sceSslConnect);
    LIB_FUNCTION("tuscfitnhEo", "libSceSsl", 1, "libSceSsl", sceSslCreateConnection);
    LIB_FUNCTION("P14ATpXc4J8", "libSceSsl", 1, "libSceSsl", sceSslCreateSslConnection);
    LIB_FUNCTION("HJ1n138CQ2g", "libSceSsl", 1, "libSceSsl", sceSslDeleteConnection);
    LIB_FUNCTION("hwrHV6Pprk4", "libSceSsl", 1, "libSceSsl", sceSslDeleteSslConnection);
    LIB_FUNCTION("iLKz4+ukLqk", "libSceSsl", 1, "libSceSsl", sceSslDisableOption);
    LIB_FUNCTION("-WqxBRAUVM4", "libSceSsl", 1, "libSceSsl", sceSslDisableOptionInternal);
    LIB_FUNCTION("w1+L-27nYas", "libSceSsl", 1, "libSceSsl", sceSslDisableOptionInternalInsecure);
    LIB_FUNCTION("PwsHbErG+e8", "libSceSsl", 1, "libSceSsl", sceSslDisableVerifyOption);
    LIB_FUNCTION("m-zPyAsIpco", "libSceSsl", 1, "libSceSsl", sceSslEnableOption);
    LIB_FUNCTION("g-zCwUKstEQ", "libSceSsl", 1, "libSceSsl", sceSslEnableOptionInternal);
    LIB_FUNCTION("po1X86mgHDU", "libSceSsl", 1, "libSceSsl", sceSslEnableVerifyOption);
    LIB_FUNCTION("qIvLs0gYxi0", "libSceSsl", 1, "libSceSsl", sceSslFreeCaCerts);
    LIB_FUNCTION("+DzXseDVkeI", "libSceSsl", 1, "libSceSsl", sceSslFreeCaList);
    LIB_FUNCTION("RwXD8grHZHM", "libSceSsl", 1, "libSceSsl", sceSslFreeSslCertName);
    LIB_FUNCTION("4O7+bRkRUe8", "libSceSsl", 1, "libSceSsl", sceSslGetAlpnSelected);
    LIB_FUNCTION("TDfQqO-gMbY", "libSceSsl", 1, "libSceSsl", sceSslGetCaCerts);
    LIB_FUNCTION("qOn+wm28wmA", "libSceSsl", 1, "libSceSsl", sceSslGetCaList);
    LIB_FUNCTION("brRtwGBu4A8", "libSceSsl", 1, "libSceSsl", sceSslGetFingerprint);
    LIB_FUNCTION("7whYpYfHP74", "libSceSsl", 1, "libSceSsl", sceSslGetIssuerName);
    LIB_FUNCTION("-PoIzr3PEk0", "libSceSsl", 1, "libSceSsl", sceSslGetMemoryPoolStats);
    LIB_FUNCTION("R1ePzopYPYM", "libSceSsl", 1, "libSceSsl", sceSslGetNameEntryCount);
    LIB_FUNCTION("7RBSTKGrmDA", "libSceSsl", 1, "libSceSsl", sceSslGetNameEntryInfo);
    LIB_FUNCTION("AzUipl-DpIw", "libSceSsl", 1, "libSceSsl", sceSslGetNanoSSLModuleId);
    LIB_FUNCTION("xHpt6+2pGYk", "libSceSsl", 1, "libSceSsl", sceSslGetNotAfter);
    LIB_FUNCTION("Eo0S65Jy28Q", "libSceSsl", 1, "libSceSsl", sceSslGetNotBefore);
    LIB_FUNCTION("-TbZc8pwPNc", "libSceSsl", 1, "libSceSsl", sceSslGetPeerCert);
    LIB_FUNCTION("kLB5aGoUJXg", "libSceSsl", 1, "libSceSsl", sceSslGetPem);
    LIB_FUNCTION("DOwXL+FQMEY", "libSceSsl", 1, "libSceSsl", sceSslGetSerialNumber);
    LIB_FUNCTION("0XcZknp7-Wc", "libSceSsl", 1, "libSceSsl", sceSslGetSslError);
    LIB_FUNCTION("dQReuBX9sD8", "libSceSsl", 1, "libSceSsl", sceSslGetSubjectName);
    LIB_FUNCTION("hdpVEUDFW3s", "libSceSsl", 1, "libSceSsl", sceSslInit);
    LIB_FUNCTION("Ab7+DH+gYyM", "libSceSsl", 1, "libSceSsl", sceSslLoadCert);
    LIB_FUNCTION("3-643mGVFJo", "libSceSsl", 1, "libSceSsl", sceSslLoadRootCACert);
    LIB_FUNCTION("jltWpVKtetg", "libSceSsl", 1, "libSceSsl", sceSslRead);
    LIB_FUNCTION("hi0veU3L2pU", "libSceSsl", 1, "libSceSsl", sceSslRecv);
    LIB_FUNCTION("50R2xYaYZwE", "libSceSsl", 1, "libSceSsl", sceSslReuseConnection);
    LIB_FUNCTION("p5bM5PPufFY", "libSceSsl", 1, "libSceSsl", sceSslSend);
    LIB_FUNCTION("TL86glUrmUw", "libSceSsl", 1, "libSceSsl", sceSslSetAlpn);
    LIB_FUNCTION("QWSxBzf6lAg", "libSceSsl", 1, "libSceSsl", sceSslSetMinSslVersion);
    LIB_FUNCTION("bKaEtQnoUuQ", "libSceSsl", 1, "libSceSsl", sceSslSetSslVersion);
    LIB_FUNCTION("E4a-ahM57QQ", "libSceSsl", 1, "libSceSsl", sceSslSetVerifyCallback);
    LIB_FUNCTION("0K1yQ6Lv-Yc", "libSceSsl", 1, "libSceSsl", sceSslTerm);
    LIB_FUNCTION("UQ+3Qu7v3cA", "libSceSsl", 1, "libSceSsl", sceSslUnloadCert);
    LIB_FUNCTION("iNjkt9Poblw", "libSceSsl", 1, "libSceSsl", sceSslWrite);
    LIB_FUNCTION("wcVuyTUr5ys", "libSceSsl", 1, "libSceSsl", VLONG_freeVlongQueue);
    LIB_FUNCTION("IuduYLwFh9c", "libSceSsl", 1, "libSceSsl", Func_22E76E60BC0587D7);
    LIB_FUNCTION("KPh5GncdOcc", "libSceSsl", 1, "libSceSsl", Func_28F8791A771D39C7);
};

} // namespace Libraries::Ssl2