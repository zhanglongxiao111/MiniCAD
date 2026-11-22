#include "PngWriter.h"
#include "Core/PathUtils.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace MiniCAD::SaTool
{
    namespace
    {
        uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0)
        {
            static const std::array<uint32_t, 256> kTable = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t n = 0; n < 256; ++n)
                {
                    uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                    t[n] = c;
                }
                return t;
            }();

            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
                crc = kTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
            return ~crc;
        }

        uint32_t Adler32(const uint8_t* data, size_t size)
        {
            constexpr uint32_t kMod = 65521u;
            uint32_t a = 1u, b = 0u;
            size_t i = 0;
            while (i < size)
            {
                const size_t chunk = std::min<size_t>(size - i, 5552u);
                for (size_t j = 0; j < chunk; ++j)
                {
                    a += data[i + j];
                    b += a;
                }
                a %= kMod;
                b %= kMod;
                i += chunk;
            }
            return (b << 16) | a;
        }

        class BitWriter
        {
        public:
            explicit BitWriter(std::vector<uint8_t>& out) : m_out(out) {}

            void Write(uint32_t bits, int count)
            {
                m_buffer |= static_cast<uint64_t>(bits) << m_count;
                m_count  += count;
                while (m_count >= 8)
                {
                    m_out.push_back(static_cast<uint8_t>(m_buffer & 0xFFu));
                    m_buffer >>= 8;
                    m_count   -= 8;
                }
            }

            void WriteHuffman(uint32_t code, int length)
            {
                uint32_t rev = 0;
                for (int i = 0; i < length; ++i)
                    rev |= ((code >> i) & 1u) << (length - 1 - i);
                Write(rev, length);
            }

            void Flush()
            {
                if (m_count > 0)
                    m_out.push_back(static_cast<uint8_t>(m_buffer & 0xFFu));
                m_buffer = 0;
                m_count  = 0;
            }

        private:
            std::vector<uint8_t>& m_out;
            uint64_t              m_buffer = 0;
            int                   m_count  = 0;
        };

        void WriteLiteralOrLength(BitWriter& bw, int sym)
        {
            if (sym <= 143)      bw.WriteHuffman(static_cast<uint32_t>(0x30 + sym), 8);
            else if (sym <= 255) bw.WriteHuffman(static_cast<uint32_t>(0x190 + (sym - 144)), 9);
            else if (sym <= 279) bw.WriteHuffman(static_cast<uint32_t>(sym - 256), 7);
            else                 bw.WriteHuffman(static_cast<uint32_t>(0xC0 + (sym - 280)), 8);
        }

        constexpr int kLenBase[29]   = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
        constexpr int kLenExtra[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
        constexpr int kDistBase[30]  = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
        constexpr int kDistExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

        void WriteMatch(BitWriter& bw, int length, int distance)
        {
            int li = 28;
            while (kLenBase[li] > length) --li;
            WriteLiteralOrLength(bw, 257 + li);
            bw.Write(static_cast<uint32_t>(length - kLenBase[li]), kLenExtra[li]);

            int di = 29;
            while (kDistBase[di] > distance) --di;
            bw.WriteHuffman(static_cast<uint32_t>(di), 5);
            bw.Write(static_cast<uint32_t>(distance - kDistBase[di]), kDistExtra[di]);
        }

        // 快速 zlib 压缩（固定 Huffman + 哈希链 LZ77，对大面积纯色 CAD 图纸高速高压缩比）
        std::vector<uint8_t> ZlibCompress(const std::vector<uint8_t>& data)
        {
            constexpr int kWindow   = 32768;
            constexpr int kHashBits = 15;
            constexpr int kHashMask = (1 << kHashBits) - 1;
            constexpr int kMaxChain = 12;
            constexpr int kMinMatch = 3;
            constexpr int kMaxMatch = 258;

            std::vector<uint8_t> out;
            out.reserve(data.size() / 8 + 256);
            out.push_back(0x78);
            out.push_back(0x01);

            BitWriter bw(out);
            bw.Write(1, 1);   // BFINAL
            bw.Write(1, 2);   // BTYPE = 01（固定 Huffman）

            const int n = static_cast<int>(data.size());
            const uint8_t* ptr = data.data();
            std::vector<int> head(1 << kHashBits, -1);
            std::vector<int> prev(static_cast<size_t>(kWindow), -1);

            auto hash3 = [ptr](int i) -> int {
                return ((static_cast<int>(ptr[i]) << 10) ^ (static_cast<int>(ptr[i + 1]) << 5) ^ static_cast<int>(ptr[i + 2])) & kHashMask;
            };
            auto insert = [&](int i) {
                if (i + kMinMatch > n) return;
                const int h = hash3(i);
                prev[static_cast<size_t>(i & (kWindow - 1))] = head[static_cast<size_t>(h)];
                head[static_cast<size_t>(h)] = i;
            };

            int i = 0;
            while (i < n)
            {
                int bestLen = 0, bestDist = 0;
                if (i + kMinMatch <= n)
                {
                    const int maxLen = std::min(kMaxMatch, n - i);

                    // 快速路径：CAD 背景经 Sub 过滤后有大量连续 0 字节，直接检查 dist=1 或 dist=4 的游程
                    if (i >= 1 && ptr[i - 1] == ptr[i])
                    {
                        int len = 1;
                        while (len < maxLen && ptr[i - 1 + len] == ptr[i + len])
                            ++len;
                        if (len >= kMinMatch)
                        {
                            bestLen = len;
                            bestDist = 1;
                        }
                    }

                    if (bestLen < maxLen)
                    {
                        int cand = head[static_cast<size_t>(hash3(i))];
                        int chain = 0;
                        while (cand >= 0 && i - cand <= kWindow && chain++ < kMaxChain)
                        {
                            if (ptr[cand + bestLen] == ptr[i + bestLen] &&
                                ptr[cand] == ptr[i] && ptr[cand + 1] == ptr[i + 1] && ptr[cand + 2] == ptr[i + 2])
                            {
                                int len = 3;
                                while (len < maxLen && ptr[cand + len] == ptr[i + len])
                                    ++len;
                                if (len > bestLen)
                                {
                                    bestLen = len;
                                    bestDist = i - cand;
                                    if (len == maxLen) break;
                                }
                            }
                            const int next = prev[static_cast<size_t>(cand & (kWindow - 1))];
                            if (next >= cand) break;
                            cand = next;
                        }
                    }
                }

                if (bestLen >= kMinMatch)
                {
                    WriteMatch(bw, bestLen, bestDist);
                    // 长游程跳着插入哈希表即可，显著提速
                    if (bestLen <= 32)
                    {
                        for (int k = 0; k < bestLen; ++k)
                            insert(i + k);
                    }
                    else
                    {
                        insert(i);
                        insert(i + bestLen - 1);
                    }
                    i += bestLen;
                }
                else
                {
                    WriteLiteralOrLength(bw, ptr[i]);
                    insert(i);
                    ++i;
                }
            }

            WriteLiteralOrLength(bw, 256);
            bw.Flush();

            const uint32_t adler = Adler32(data.data(), data.size());
            out.push_back(static_cast<uint8_t>(adler >> 24));
            out.push_back(static_cast<uint8_t>(adler >> 16));
            out.push_back(static_cast<uint8_t>(adler >> 8));
            out.push_back(static_cast<uint8_t>(adler));
            return out;
        }

        // 使用 Sub 过滤器（1）：对大面积纯色 CAD 图面几乎全变为 0，速度和压缩率双优
        std::vector<uint8_t> FilterScanlinesSub(const uint8_t* rgba, int width, int height)
        {
            const size_t stride = static_cast<size_t>(width) * 4u;
            std::vector<uint8_t> raw;
            raw.resize((stride + 1u) * static_cast<size_t>(height));

            uint8_t* dst = raw.data();
            for (int y = 0; y < height; ++y)
            {
                const uint8_t* row = rgba + stride * static_cast<size_t>(y);
                *dst++ = 1;   // Filter type 1 = Sub
                if (stride >= 4u)
                {
                    dst[0] = row[0]; dst[1] = row[1]; dst[2] = row[2]; dst[3] = row[3];
                    for (size_t x = 4; x < stride; ++x)
                        dst[x] = static_cast<uint8_t>(row[x] - row[x - 4]);
                }
                dst += stride;
            }
            return raw;
        }

        void PutU32(std::vector<uint8_t>& v, uint32_t x)
        {
            v.push_back(static_cast<uint8_t>(x >> 24));
            v.push_back(static_cast<uint8_t>(x >> 16));
            v.push_back(static_cast<uint8_t>(x >> 8));
            v.push_back(static_cast<uint8_t>(x));
        }

        void PutChunk(std::vector<uint8_t>& file, const char type[4], const uint8_t* payload, size_t size)
        {
            PutU32(file, static_cast<uint32_t>(size));
            const size_t start = file.size();
            file.insert(file.end(), type, type + 4);
            if (size > 0)
                file.insert(file.end(), payload, payload + size);
            PutU32(file, Crc32(file.data() + start, file.size() - start));
        }
    }

    std::vector<uint8_t> EncodePngRgba(const uint8_t* rgba, int width, int height)
    {
        if (!rgba || width <= 0 || height <= 0)
            return {};

        std::vector<uint8_t> file = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

        std::vector<uint8_t> ihdr;
        ihdr.reserve(13);
        PutU32(ihdr, static_cast<uint32_t>(width));
        PutU32(ihdr, static_cast<uint32_t>(height));
        ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 });   // 8-bit RGBA
        PutChunk(file, "IHDR", ihdr.data(), ihdr.size());

        const std::vector<uint8_t> filtered = FilterScanlinesSub(rgba, width, height);
        const std::vector<uint8_t> idat     = ZlibCompress(filtered);
        PutChunk(file, "IDAT", idat.data(), idat.size());
        PutChunk(file, "IEND", nullptr, 0);
        return file;
    }

    bool WritePngFile(const std::string& utf8Path, const uint8_t* rgba, int width, int height, std::string* error)
    {
        if (utf8Path.empty() || !rgba || width <= 0 || height <= 0)
        {
            if (error) *error = "invalid PNG output parameters";
            return false;
        }
        const std::vector<uint8_t> png = EncodePngRgba(rgba, width, height);
        if (png.empty())
        {
            if (error) *error = "failed to encode PNG";
            return false;
        }

        const std::filesystem::path p = MiniCAD::Utf8ToPath(utf8Path);
        std::error_code ec;
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);

        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
        {
            if (error) *error = "无法打开 PNG 文件进行写入：" + utf8Path;
            return false;
        }
        f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        f.close();
        if (!f)
        {
            if (error) *error = "写入 PNG 文件失败：" + utf8Path;
            return false;
        }
        return true;
    }

    bool WritePngFile(const std::string& utf8Path, const RgbaImage& image, std::string* error)
    {
        return WritePngFile(utf8Path, image.Pixels.data(), image.Width, image.Height, error);
    }
}
