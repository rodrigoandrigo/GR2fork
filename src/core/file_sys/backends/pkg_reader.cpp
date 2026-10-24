// Adapted from Prosperity/PS4Delta pkg_filesystem.cc, Copyright 2019-2020 Force67.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/file_sys/backends/pkg_reader.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <openssl/aes.h>
#include <openssl/bn.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <zlib.h>

namespace Core::FileSys {
namespace {
class PackageSource {
  public:
    explicit PackageSource(std::unique_ptr<IFile> value) : file(std::move(value)) {
        if (!file || !file->IsOpen())
            throw std::runtime_error("Não foi possível abrir o PKG.");
    }
    void Seek(u64 offset, Common::FS::SeekOrigin origin) {
        if (offset > static_cast<u64>(INT64_MAX) || !file->Seek(static_cast<s64>(offset), origin))
            throw std::runtime_error("Offset PKG inválido.");
    }
    void Read(void* data, u64 length) {
        const auto position = file->Tell();
        if (position > file->Size() || length > file->Size() - position)
            throw std::runtime_error("Leitura fora do PKG.");
        u64 done{};
        while (done < length) {
            const auto got = file->Read(static_cast<u8*>(data) + done, length - done);
            if (got <= 0)
                throw std::runtime_error("Leitura incompleta do PKG.");
            done += static_cast<u64>(got);
        }
    }
    u64 GetSize() const { return file->Size(); }
    bool IsOpen() const { return file->IsOpen(); }

  private:
    std::unique_ptr<IFile> file;
};

static const char* kNFake = "c6cf71e7e59af0d12a2c458bf92a0ec143058bc37117801dcd497dde359d259b"
                            "a0d7a0f27d6c087eaa5502682b23c644b84418eb56cf16a24803c9e74f87eb3d"
                            "30c31588bf20e79dff770cde1d241e63a94f8abf5bbe601968333bfced9f474e"
                            "5ff8eacb3d00bd6701f92c6dc6ac1364e76714f3dc52696ab9832c4230131bb2"
                            "d8a5020d79ed96b10df8cc0cdf81954f035809570e80692efeff5277ea7528a8"
                            "fbc9bebf9fbbb7798e1805e180bd50349481d353c269a2d24ccf6cf4572c104a"
                            "3ffb22fd8b97e2c95ba62bcdd61b6bdb687f4bc2a05034c005e58def2467ff93"
                            "40cf2d62a2a050b1f13aa83dfd80d1f9b80522afc8354590588ee33a7cbd3e27";
static const char* kDFake = "7f76cd0ee2d4de051cc6d9a80e8dfa7bca1eaa271a40f8f1228735dddbfdeef8"
                            "c2bcbd01fb8be23e63b2b1225c56496e11be07440b9a2666d1492c8fd31bcfa4"
                            "a1b8d1fba49ed2212883098af6a00ba3d60f9b6368ccbc0c4e145b27a4a9f42b"
                            "b9b87bc0e651ad1d77d46bb9ce20d126667e5e9ea2e96b90f373b8528f441103"
                            "0c1397393d132258d5438249da6e7ca1c58ca5b009e0ce3ddff49d3c9715e26a"
                            "c72b3c509323dbba4a226644ac78bb0e1a2743b57167aff4ab48469373d042ab"
                            "9363e56c9ade5024c0237d99793f2207e0c148561bdf830912b42d456bc9c068"
                            "85999079961ad7f54d1f3783404aec3937a680927dc580c7d66ffe8a7989c6b1";
static const char* kNDk3 = "d212fc335f6ddb831609628b0356273782d477853529392d526b8c4c8cfb06c1"
                           "845be7d4f7bcd24e6245cd2abbd77776453655273fb3f5f98eda4befaa59aeb3"
                           "9bea5498d206326a58312ae0d44f90b50a7decf43a9c52672d99318e0c43e682"
                           "fe0746e12e50d41f2d2f7ed908ba06b3bf2e203f4e3ffe44ffaa504357916994"
                           "49158282e40f4c8d9d2cc95b1d64bf888bd4c594e76547841ee57910fb989347"
                           "b97d8512a640982cf792bc951932ede890560d65c1aa78c62e54fd5f54a1f67e"
                           "e5e05f61c120b4b9b4330870e4df8956ed012946775f8cb8a9f51e2eb3b9bfe0"
                           "09b78d28d4a6c3b81e1f07ebb4120b95b88530fddc3913d07cdc8fedf9c9a3c1";
static const char* kDDk3 = "32d903908fbdb08f572b285e0b8db3ea5cd17ea890888cdd6a80bbb1dfc1f70d"
                           "aa32f0b77ccb88800e8b64b0be4cd60e9b8c1e2a64e1f35cd77601415e935c94"
                           "fedd4662c31b5ae2a0bc2debc3980aa7b7856970682b644ab31fcc7ddc7c26f4"
                           "77f65cf2ae5a442dd3ab16620419bafb90ffe23050896ecb56b2ebc09116925e"
                           "308eaec7945dfd35e120f8ad3ebc08bfc036749fd5bb5208fd0666f37ab304f4"
                           "75295de95faa1030b20f5a1ac12ab3fecb21ad80ec8f20091cdbc55894c29cc6"
                           "ce82653e5790bca98b06b4f072f677df9864f1ecfe372dbcae8c08811fc3c989"
                           "1ac742824b2edc8e8d73ceb1cc01d90870873c4408ec498f815ae240ff77fc0d";

inline u16 Rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 Rd32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
inline u64 Rd64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<u64>(p[i]) << (8 * i);
    return v;
}
inline u32 Be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}

std::vector<u8> RsaDecrypt(const u8* ct, size_t ct_len, const char* n_hex, const char* d_hex) {
    std::vector<u8> result;
    std::unique_ptr<BIGNUM, decltype(&BN_free)> n(nullptr, BN_free), d(nullptr, BN_free),
        c(BN_bin2bn(ct, static_cast<int>(ct_len), nullptr), BN_free), m(BN_new(), BN_free);
    BIGNUM* raw_n{};
    BIGNUM* raw_d{};
    BN_hex2bn(&raw_n, n_hex);
    BN_hex2bn(&raw_d, d_hex);
    n.reset(raw_n);
    d.reset(raw_d);
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> context(BN_CTX_new(), BN_CTX_free);
    u8 em[0x100]{};
    const bool ok = n && d && c && m && context && BN_cmp(c.get(), n.get()) < 0 &&
                    BN_mod_exp(m.get(), c.get(), d.get(), n.get(), context.get()) == 1 &&
                    BN_bn2binpad(m.get(), em, sizeof(em)) == sizeof(em);
    if (!ok || em[0] != 0 || em[1] != 2)
        return result;
    size_t sep = 2;
    while (sep < sizeof(em) && em[sep] != 0)
        ++sep;
    if (sep < 10 || sep >= sizeof(em))
        return result;
    result.assign(em + sep + 1, em + sizeof(em));
    return result;
}

void Sha256(const u8* in, size_t len, u8 out[32]) {
    if (!SHA256(in, len, out))
        throw std::runtime_error("PKG SHA256");
}

void HmacSha256(const u8* key, size_t key_len, const u8* in, size_t len, u8 out[32]) {
    unsigned size{};
    if (!HMAC(EVP_sha256(), key, static_cast<int>(key_len), in, len, out, &size) || size != 32)
        throw std::runtime_error("PKG HMAC");
}

void Aes128CbcDecrypt(const u8 key[16], const u8 iv[16], const u8* in, size_t len, u8* out) {
    AES_KEY c;
    if (AES_set_decrypt_key(key, 128, &c))
        throw std::runtime_error("PKG AES");
    u8 iv_copy[16];
    std::memcpy(iv_copy, iv, 16);
    AES_cbc_encrypt(in, out, len, &c, iv_copy, AES_DECRYPT);
}

struct DataSource {
    virtual ~DataSource() = default;
    virtual void Read(u64 off, u64 len, u8* dst) = 0;
};

class RawSource : public DataSource {
  public:
    RawSource(PackageSource& f, u64 base) : f_(f), base_(base) {}
    void Read(u64 off, u64 len, u8* dst) override {
        std::memset(dst, 0, len);
        if (off > UINT64_MAX - base_)
            throw std::runtime_error("Offset PKG inválido.");
        f_.Seek(base_ + off, Common::FS::SeekOrigin::SetOrigin);
        f_.Read(dst, len);
    }

  private:
    PackageSource& f_;
    u64 base_;
};

class SubSource : public DataSource {
  public:
    SubSource(DataSource* inner, u64 off) : inner_(inner), off_(off) {}
    void Read(u64 off, u64 len, u8* dst) override {
        if (off > UINT64_MAX - off_)
            throw std::runtime_error("Offset PFS inválido.");
        inner_->Read(off_ + off, len, dst);
    }

  private:
    DataSource* inner_;
    u64 off_;
};

class XtsSource : public DataSource {
  public:
    XtsSource(DataSource* inner, const u8 tweak_key[16], const u8 data_key[16], u32 skip_sectors)
        : inner_(inner), skip_(skip_sectors) {
        if (AES_set_encrypt_key(tweak_key, 128, &tweak_))
            throw std::runtime_error("PKG AES key");
        if (AES_set_decrypt_key(data_key, 128, &data_))
            throw std::runtime_error("PKG AES key");
    }

    void Read(u64 off, u64 len, u8* dst) override {
        u8 sector[0x1000];
        u64 p = off, end = off + len;
        while (p < end) {
            u64 si = p / 0x1000, so = p % 0x1000;
            u64 take = std::min<u64>(0x1000 - so, end - p);
            inner_->Read(si * 0x1000, 0x1000, sector);
            if (si >= skip_)
                DecryptSector(si, sector);
            std::memcpy(dst, sector + so, take);
            dst += take;
            p += take;
        }
    }

  private:
    void DecryptSector(u64 si, u8* buf) {
        u8 seed[16] = {0};
        for (int i = 0; i < 8; ++i)
            seed[i] = static_cast<u8>(si >> (8 * i));
        u8 tweak[16];
        AES_encrypt(seed, tweak, &tweak_);
        for (u32 b = 0; b < 0x1000; b += 16) {
            u8 x[16], p[16];
            for (int k = 0; k < 16; ++k)
                x[k] = buf[b + k] ^ tweak[k];
            AES_decrypt(x, p, &data_);
            for (int k = 0; k < 16; ++k)
                buf[b + k] = p[k] ^ tweak[k];
            u8 carry = 0;
            for (int k = 0; k < 16; ++k) {
                u8 v = tweak[k];
                tweak[k] = static_cast<u8>((v << 1) | carry);
                carry = v >> 7;
            }
            if (carry)
                tweak[0] ^= 0x87;
        }
    }

    DataSource* inner_;
    AES_KEY tweak_;
    AES_KEY data_;
    u32 skip_;
};

class PfscSource : public DataSource {
  public:
    explicit PfscSource(DataSource* inner) : inner_(inner) {
        u8 h[0x30];
        inner_->Read(0, sizeof(h), h);
        bs_ = Rd64(h + 0x10);
        u64 bo = Rd64(h + 0x18);
        u64 dl = Rd64(h + 0x28);
        if (std::memcmp(h, "PFSC", 4) || bs_ < 512 || bs_ > 16 * 1024 * 1024 || (bs_ & (bs_ - 1)) ||
            !dl || dl > UINT64_MAX - bs_)
            throw std::runtime_error("Cabeçalho PFSC inválido.");
        n_ = (dl + bs_ - 1) / bs_;
        if (n_ > 8 * 1024 * 1024)
            throw std::runtime_error("Tabela PFSC excede o limite.");
        map_.resize(n_ + 1);
        std::vector<u8> raw((n_ + 1) * 8);
        inner_->Read(bo, raw.size(), raw.data());
        for (u64 i = 0; i <= n_; ++i)
            map_[i] = Rd64(raw.data() + i * 8);
    }

    void Read(u64 off, u64 len, u8* dst) override {
        auto& block = cached_block_;
        block.resize(bs_);
        std::vector<u8> comp;
        while (len > 0) {
            u64 bi = off / bs_, bo = off % bs_;
            if (bi >= n_)
                throw std::runtime_error("Leitura fora do PFSC.");
            if (cached_index_ != bi) {
                u64 a = map_[bi], b = map_[bi + 1];
                if (b <= a || b - a > bs_)
                    throw std::runtime_error("Bloco PFSC inválido.");
                u64 clen = b - a;
                comp.resize(clen);
                inner_->Read(a, clen, comp.data());
                if (clen == bs_) {
                    std::memcpy(block.data(), comp.data(), bs_);
                } else {
                    uLongf out = static_cast<uLongf>(bs_);
                    if (uncompress(block.data(), &out, comp.data(), static_cast<uLong>(clen)) !=
                            Z_OK ||
                        out > bs_)
                        throw std::runtime_error("Falha na descompressão PFSC.");
                    std::fill(block.begin() + out, block.end(), 0);
                }
                cached_index_ = bi;
            }
            u64 take = std::min<u64>(bs_ - bo, len);
            std::memcpy(dst, block.data() + bo, take);
            dst += take;
            off += take;
            len -= take;
        }
    }

  private:
    DataSource* inner_;
    u64 bs_ = 0;
    u64 n_ = 0;
    std::vector<u64> map_;
    std::vector<u8> cached_block_;
    u64 cached_index_{UINT64_MAX};
};
} // namespace

struct PkgImpl {
    PackageSource pkg;
    std::mutex io;
    std::vector<std::unique_ptr<DataSource>> nodes;
    DataSource* inner = nullptr;
    u32 inner_bs = 0;
    std::unordered_map<std::string, PkgReader::Node> files;

    std::unordered_map<std::string, std::string> files_ci;
    bool valid = false;

    explicit PkgImpl(std::unique_ptr<IFile> source) : pkg(std::move(source)) {
        u8 magic[4];
        pkg.Read(magic, sizeof(magic));
        if (Be32(magic) != 0x7f434e54)
            throw std::runtime_error("Arquivo não é um PKG PS4 válido.");
        Build();
    }

    template <typename T, typename... Args> T* Make(Args&&... args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = &*p;
        nodes.emplace_back(std::move(p));
        return raw;
    }

    bool GetEkpfs(u8 out[32]) {
        auto r = [&](u64 o, u64 n, u8* dst) {
            pkg.Seek(o, Common::FS::SeekOrigin::SetOrigin);
            pkg.Read(dst, n);
        };
        u8 tmp[4];
        r(0x10, 4, tmp);
        u32 ec = Be32(tmp);
        r(0x18, 4, tmp);
        u32 to = Be32(tmp);
        if (ec == 0 || ec > 0x10000)
            return false;

        std::vector<u8> tb(static_cast<size_t>(ec) * 0x20);
        r(to, tb.size(), tb.data());

        const u8* row20 = nullptr;
        u32 ik_off = 0, ik_sz = 0, ek_off = 0, ek_sz = 0;
        for (u32 i = 0; i < ec; ++i) {
            const u8* e = tb.data() + i * 0x20;
            u32 id = Be32(e);
            if (id == 0x20) {
                row20 = e;
                ik_off = Be32(e + 16);
                ik_sz = Be32(e + 20);
            } else if (id == 0x10) {
                ek_off = Be32(e + 16);
                ek_sz = Be32(e + 20);
            }
        }
        if (!row20 || !ek_off || ek_sz < 0x500 || ek_sz > 1024 * 1024 || ik_sz < 0x100 ||
            ik_sz > 1024 * 1024 || (ik_sz % 16))
            return false;

        std::vector<u8> ek(ek_sz);
        r(ek_off, ek_sz, ek.data());
        auto dk3 = RsaDecrypt(ek.data() + 0x400, 0x100, kNDk3, kDDk3);
        if (dk3.empty())
            return false;

        std::vector<u8> ivk_in(row20, row20 + 0x20);
        ivk_in.insert(ivk_in.end(), dk3.begin(), dk3.end());
        u8 ivk[32];
        Sha256(ivk_in.data(), ivk_in.size(), ivk);

        std::vector<u8> ik(ik_sz), imdec(ik_sz);
        r(ik_off, ik_sz, ik.data());
        Aes128CbcDecrypt(ivk + 16, ivk, ik.data(), ik_sz, imdec.data());

        auto ekpfs = RsaDecrypt(imdec.data(), imdec.size(), kNFake, kDFake);
        if (ekpfs.size() < 32)
            return false;
        std::memcpy(out, ekpfs.data(), 32);
        return true;
    }

    DataSource* BuildPfs(DataSource* raw, const u8 ekpfs[32], u32& bs, bool& signed_flag, u64& nd,
                         u64& ndb) {
        u8 h[0x400];
        raw->Read(0, sizeof(h), h);
        bs = Rd32(h + 0x20);
        u16 mode = Rd16(h + 0x1c);
        nd = Rd64(h + 0x30);
        ndb = Rd64(h + 0x40);
        signed_flag = mode & 1;
        if (bs < 0x1000 || bs > 1024 * 1024 || (bs & (bs - 1)) || nd > 1024 * 1024 ||
            ndb > 1024 * 1024 || ndb > UINT64_MAX / bs)
            throw std::runtime_error("Superbloco PFS inválido.");

        DataSource* reader = raw;
        if (mode & 4) {
            u8 msg[20] = {1, 0, 0, 0};
            std::memcpy(msg + 4, h + 0x370, 16);
            u8 d[32];
            HmacSha256(ekpfs, 32, msg, sizeof(msg), d);
            reader = Make<XtsSource>(raw, d, d + 16, bs / 0x1000);
        }
        return reader;
    }

    struct Inode {
        u16 type;
        u64 size;
        u32 blocks;
        u32 start;
    };
    std::vector<Inode> ino;
    std::unordered_set<u32> visited;

    void ReadInodes(DataSource* r, u32 bs, bool signed_flag, u64 nd, u64 ndb) {
        u32 dsz = signed_flag ? 0x2C8 : 0xA8;
        u32 per = bs / dsz;
        std::vector<u8> blk(bs);
        u64 tot = 0;
        u64 count = ndb < 1 ? 1 : ndb;
        for (u64 bi = 0; bi < count && tot < nd; ++bi) {
            r->Read((1 + bi) * bs, bs, blk.data());
            for (u32 j = 0; j < per && tot < nd; ++j) {
                const u8* d = blk.data() + j * dsz;
                Inode n;
                n.type = Rd16(d);
                n.size = Rd64(d + 8);
                n.blocks = Rd32(d + 0x60);
                n.start = signed_flag ? Rd32(d + 0x84) : Rd32(d + 0x64);
                ino.push_back(n);
                ++tot;
            }
        }
    }

    s64 FindChildDir(u32 dir_inode, const char* name, DataSource* r, u32 bs) {
        if (dir_inode >= ino.size())
            return -1;
        u32 blocks = ino[dir_inode].blocks, st = ino[dir_inode].start;
        std::vector<u8> d(bs);
        for (u32 bb = 0; bb < blocks; ++bb) {
            r->Read((u64)(st + bb) * bs, bs, d.data());
            u64 o = 0;
            while (o + 17 < bs) {
                u32 ch = Rd32(d.data() + o);
                u32 ty = Rd32(d.data() + o + 4);
                u32 nl = Rd32(d.data() + o + 8);
                u32 es = Rd32(d.data() + o + 12);
                if (es == 0)
                    break;
                if (es < 16 || es > bs - o || nl > es - 16)
                    throw std::runtime_error("Entrada PFS inválida.");
                if (ty == 3 && nl > 0 && nl < 256 && o + 16 + nl <= bs) {
                    std::string nm(reinterpret_cast<const char*>(d.data() + o + 16), nl);
                    if (nm == name && ch < ino.size())
                        return static_cast<s64>(ch);
                }
                o += es;
            }
        }
        return -1;
    }

    void Walk(u32 i, const std::string& pre, DataSource* r, u32 bs) {
        if (pre.size() > 4096 || !visited.insert(i).second)
            throw std::runtime_error("Diretório PFS cíclico ou profundo.");
        if (i >= ino.size())
            return;
        u32 blocks = ino[i].blocks, st = ino[i].start;
        std::vector<u8> d(bs);
        for (u32 bb = 0; bb < blocks; ++bb) {
            r->Read((u64)(st + bb) * bs, bs, d.data());
            u64 o = 0;
            while (o + 17 < bs) {
                u32 ch = Rd32(d.data() + o);
                u32 ty = Rd32(d.data() + o + 4);
                u32 nl = Rd32(d.data() + o + 8);
                u32 es = Rd32(d.data() + o + 12);
                if (es == 0)
                    break;
                if (es < 16 || es > bs - o || nl > es - 16)
                    throw std::runtime_error("Entrada PFS inválida.");
                if (nl > 0 && nl < 256 && o + 16 + nl <= bs) {
                    std::string nm(reinterpret_cast<const char*>(d.data() + o + 16), nl);
                    if (nm.find_first_of("/\\\0", 0, 3) != std::string::npos)
                        throw std::runtime_error("Nome PFS inválido.");
                    if (ty == 2 && nm != "." && nm != ".." && ch < ino.size()) {
                        files[pre + "/" + nm] = {ino[ch].size, ino[ch].start};
                    } else if (ty == 3 && nm != "." && nm != "..") {
                        files[pre + "/" + nm] = {0, 0, true};
                        Walk(ch, pre + "/" + nm, r, bs);
                    }
                }
                o += es;
            }
        }
    }

    void Build() {
        u8 ekpfs[32];
        if (!GetEkpfs(ekpfs)) {

            return;
        }

        u8 off[8];
        pkg.Seek(0x410, Common::FS::SeekOrigin::SetOrigin);
        pkg.Read(off, 8);
        u64 pfs_off = (static_cast<u64>(Be32(off)) << 32) | Be32(off + 4);
        if (pfs_off < 0x1000 || pfs_off >= pkg.GetSize())
            pfs_off = 0x700000;

        auto* raw = Make<RawSource>(pkg, pfs_off);
        u32 outer_bs = 0;
        bool outer_signed = false;
        u64 outer_nd = 0, outer_ndb = 0;
        DataSource* outer = BuildPfs(raw, ekpfs, outer_bs, outer_signed, outer_nd, outer_ndb);
        if (outer_bs == 0)
            return;

        u64 inner_blk = 11;
        bool found = false;
        u64 max_blk = (pkg.GetSize() - pfs_off) / outer_bs;
        for (u64 bi = 1; bi < max_blk; ++bi) {
            u8 magic[4];
            outer->Read(bi * outer_bs, sizeof(magic), magic);
            if (magic[0] == 'P' && magic[1] == 'F' && magic[2] == 'S' && magic[3] == 'C') {
                inner_blk = bi;
                found = true;
                break;
            }
        }
        if (!found)
            throw std::runtime_error("Imagem PFSC não encontrada no PKG.");
        auto* sub = Make<SubSource>(outer, inner_blk * outer_bs);
        auto* pfsc = Make<PfscSource>(sub);

        bool inner_signed = false;
        u64 inner_nd = 0, inner_ndb = 0;
        inner = BuildPfs(pfsc, ekpfs, inner_bs, inner_signed, inner_nd, inner_ndb);
        if (inner_bs == 0)
            return;

        ReadInodes(inner, inner_bs, inner_signed, inner_nd, inner_ndb);
        if (ino.empty())
            return;

        s64 root = FindChildDir(0, "uroot", inner, inner_bs);
        Walk(root >= 0 ? static_cast<u32>(root) : 0, "", inner, inner_bs);

        for (const auto& kv : files) {
            std::string lower(kv.first);
            for (auto& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            files_ci.emplace(std::move(lower), kv.first);
        }

        valid = true;
    }

    s64 ReadNode(const PkgReader::Node& n, void* buf, s64 off, s64 len) {
        if (off < 0 || len < 0)
            return -1;
        if (static_cast<u64>(off) >= n.size)
            return 0;
        u64 take = std::min<u64>(len, n.size - static_cast<u64>(off));
        std::lock_guard<std::mutex> lk(io);
        u64 inner_off = static_cast<u64>(n.start_block) * inner_bs + static_cast<u64>(off);
        inner->Read(inner_off, take, static_cast<u8*>(buf));
        return static_cast<s64>(take);
    }

    bool ReadEntry(u32 want_id, std::vector<u8>& out) {
        if (!pkg.IsOpen())
            return false;
        std::lock_guard<std::mutex> lk(io);
        auto r = [&](u64 o, u64 n, u8* dst) {
            pkg.Seek(o, Common::FS::SeekOrigin::SetOrigin);
            pkg.Read(dst, n);
        };
        u8 tmp[4];
        r(0x10, 4, tmp);
        u32 ec = Be32(tmp);
        r(0x18, 4, tmp);
        u32 to = Be32(tmp);
        if (ec == 0 || ec > 0x10000)
            return false;
        std::vector<u8> tb(static_cast<size_t>(ec) * 0x20);
        r(to, tb.size(), tb.data());
        for (u32 i = 0; i < ec; ++i) {
            const u8* e = tb.data() + i * 0x20;
            if (Be32(e) != want_id)
                continue;
            u32 off = Be32(e + 16), sz = Be32(e + 20);
            const u32 max_size =
                want_id == 0x1000 ? 1u << 20 : (want_id == 0x1200 ? 16u << 20 : UINT32_MAX);
            if (sz == 0 || sz > max_size || static_cast<u64>(off) + sz > pkg.GetSize())
                return false;
            out.resize(sz);
            r(off, sz, out.data());
            return true;
        }
        return false;
    }
};

PkgReader::PkgReader(std::unique_ptr<IFile> source)
    : impl_(std::make_unique<PkgImpl>(std::move(source))) {}
PkgReader::~PkgReader() = default;

bool PkgReader::Valid() const { return impl_ && impl_->valid; }

const PkgReader::Node* PkgReader::Find(const char* rel_path) const {
    if (!impl_ || !rel_path)
        return nullptr;

    std::string key;
    key.reserve(std::strlen(rel_path) + 1);
    key.push_back('/');
    for (const char* p = rel_path; *p; ++p) {
        if (*p == '/' && (key.empty() || key.back() == '/'))
            continue;
        key.push_back(*p);
    }
    auto it = impl_->files.find(key);
    if (it != impl_->files.end())
        return &it->second;

    std::string lower(key);
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto ci = impl_->files_ci.find(lower);
    if (ci != impl_->files_ci.end()) {
        it = impl_->files.find(ci->second);
        if (it != impl_->files.end())
            return &it->second;
    }
    return nullptr;
}

s64 PkgReader::Read(const Node& node, void* buf, s64 off, s64 len) {
    return impl_->ReadNode(node, buf, off, len);
}

void PkgReader::Paths(std::vector<std::string>& out) const {
    if (!impl_)
        return;
    out.reserve(impl_->files.size());
    for (const auto& kv : impl_->files)
        out.push_back(kv.first);
}

s64 PkgReader::ReadPkgEntry(u32 entry_id, std::vector<u8>& out) {
    if (!impl_)
        return -1;
    return impl_->ReadEntry(entry_id, out) ? static_cast<s64>(out.size()) : -1;
}
} // namespace Core::FileSys