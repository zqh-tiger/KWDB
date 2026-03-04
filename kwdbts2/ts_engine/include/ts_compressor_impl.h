// Copyright (c) 2022-present, Shanghai Yunxi Technology Co, Ltd.
//
// This software (KWDB) is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
//          http://license.coscl.org.cn/MulanPSL2
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PSL v2 for more details.

#pragma once

#include <lz4.h>
#include <zstd.h>
#include <zlib.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
#include "ts_bufferbuilder.h"
#include "ts_sliceguard.h"

using std::string;

#include "libkwdbts2.h"
#include "snappy.h"
#include "snappy-sinksource.h"
#include "ts_bitmap.h"
#include "ts_compressor.h"

namespace kwdbts {

class TsCompressorBase {
 public:
  virtual ~TsCompressorBase() = default;
  virtual bool Compress(TSSlice raw, const TsBitmapBase *bitmap, uint32_t count,
                        TsBufferBuilder *out) const = 0;
  virtual bool Decompress(TSSlice raw, const TsBitmapBase *bitmap, uint32_t count,
                          TsSliceGuard *out) const = 0;
};

class GenCompressorBase {
 public:
  virtual ~GenCompressorBase() = default;
  virtual bool Compress(TSSlice raw, TsBufferBuilder *out, uint8_t level) const = 0;
  virtual bool Decompress(TSSlice raw, TsSliceGuard *out) const = 0;
};

class CompressorImpl {
 protected:
  CompressorImpl() = default;

 public:
  virtual ~CompressorImpl() = default;
  CompressorImpl(const CompressorImpl &) = delete;
  void operator=(const CompressorImpl &) = delete;
  virtual bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level = 1) const = 0;
  virtual bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const = 0;
  virtual size_t GetUncompressedSize(TSSlice data, uint64_t count) const = 0;
};

class GorillaInt : public CompressorImpl {
 private:
  GorillaInt() = default;

 public:
  static GorillaInt &GetInstance() {
    static GorillaInt inst;
    return inst;
  }
  static constexpr int stride = 8;
  bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override;
  bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override;
  size_t GetUncompressedSize(TSSlice data, uint64_t count) const override { return stride * count; }
};

template <class T>
class GorillaIntV2 : public CompressorImpl {
 private:
  GorillaIntV2() = default;

 public:
  static GorillaIntV2 &GetInstance() {
    static GorillaIntV2 inst;
    return inst;
  }
  static constexpr int stride = sizeof(T);
  bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override;
  bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override;
  size_t GetUncompressedSize(TSSlice data, uint64_t count) const override { return stride * count; }
};

template <class T>
class Simple8BInt : public CompressorImpl {
 private:
  Simple8BInt() = default;

 public:
  static Simple8BInt &GetInstance() {
    static Simple8BInt inst;
    return inst;
  }
  static constexpr int stride = sizeof(T);
  bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override;
  bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override;
  size_t GetUncompressedSize(TSSlice data, uint64_t count) const override { return stride * count; }
};

template <class T>
class Simple8BIntV2 : public CompressorImpl {
 private:
  Simple8BIntV2() = default;

 public:
  static Simple8BIntV2 &GetInstance() {
    static Simple8BIntV2 inst;
    return inst;
  }
  static constexpr int stride = sizeof(T);
  bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override;
  bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override;
  size_t GetUncompressedSize(TSSlice data, uint64_t count) const override { return stride * count; }
};

class BitPacking : public CompressorImpl {
 private:
  BitPacking() = default;

 public:
  static BitPacking &GetInstance() {
    static BitPacking inst;
    return inst;
  }
  static constexpr int stride = 1;
  bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override;
  bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override;
  size_t GetUncompressedSize(TSSlice data, uint64_t count) const override { return stride * count; }
};

class EntropyEncode : public CompressorImpl {};

template <class T>
class Chimp : public CompressorImpl {
  static_assert(std::is_floating_point_v<T>);

 private:
  Chimp() = default;

 public:
  static Chimp &GetInstance() {
    static Chimp inst;
    return inst;
  }
  static constexpr int stride = sizeof(T);
  bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override;
  bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override;
  size_t GetUncompressedSize(TSSlice data, uint64_t count) const override { return stride * count; }
};

class SnappyString : public CompressorImpl {
 private:
  SnappyString() = default;
  class BufferSink : public snappy::Sink {
    TsBufferBuilder *out_;

   public:
    explicit BufferSink(TsBufferBuilder *out) : out_(out) {}
    void Append(const char *bytes, size_t n) override { out_->append({bytes, n}); }
  };

 public:
  static constexpr int stride = -1;
  static SnappyString &GetInstance() {
    static SnappyString inst;
    return inst;
  }
  bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override {
    out->clear();
    snappy::ByteArraySource src(data.data, data.len);
    BufferSink sink(out);
    snappy::Compress(&src, &sink);
    return true;
  }
  bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override {
    TsBufferBuilder builder;
    BufferSink sink(&builder);

    snappy::ByteArraySource src(data.data, data.len);
    bool ok = snappy::Uncompress(&src, &sink);
    if (!ok) {
      return false;
    }
    *out = builder.GetBuffer();
    return true;
  }
  size_t GetUncompressedSize(TSSlice data, uint64_t count) const override {
    size_t result;
    bool ok = snappy::GetUncompressedLength(data.data, data.len, &result);
    if (ok) {
      return result;
    }
    return -1;
  }
};

// LZ4
class LZ4String : public CompressorImpl {
 private:
  LZ4String() = default;

 public:
  static constexpr int stride = -1;
  static LZ4String &GetInstance() {
    static LZ4String inst;
    return inst;
  }

    bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override {
      int dst_capacity  = LZ4_compressBound(data.len);
      std::vector<char> compressed(dst_capacity);
      if (dst_capacity != 0) {
        int compressed_size = LZ4_compress_fast(data.data, compressed.data(), data.len, dst_capacity, level);
        if (compressed_size == 0) {
          LOG_ERROR("LZ4 Compress Failed!");
          out->append(data);
          return false;
        }
        PutFixed64(out, data.len);
        out->append(compressed.data(), compressed_size);
        return true;
      }
      LOG_ERROR("LZ4 Compress Failed! Input size is incorrect (too large or negative).");
      // maybe lz4frame if too large?
      return false;
    }

    bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override {
      uint64_t org_size = DecodeFixed64(data.data);
      TsBufferBuilder builder(org_size);
      if (org_size != 0) {
        int ret_size = LZ4_decompress_safe(data.data + 8, builder.data(), data.len - 8, org_size);
        if (ret_size != org_size) {
          LOG_ERROR("LZ4 Decompress Failed!");
          builder.assign(data.data, data.len);
          *out = builder.GetBuffer();
          return false;
        }
        *out = builder.GetBuffer();
        return true;
      }
      LOG_ERROR("LZ4 Decompress Failed! Incorrect original data size.")
      return false;
    }

    size_t GetUncompressedSize(TSSlice data, uint64_t count) const override {
      uint64_t org_size = DecodeFixed64(data.data);
      return org_size == 0 ? -1 : org_size;
    }
};

// ZSTD
class ZSTDString : public CompressorImpl {
 private:
  ZSTDString() = default;

 public:
  static constexpr int stride = -1;
  static ZSTDString &GetInstance() {
    static ZSTDString inst;
    return inst;
  }

    bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override {
      size_t dst_capacity  = ZSTD_compressBound(data.len);
      std::vector<char> compressed(dst_capacity);
      if (dst_capacity != 0) {
        size_t compressed_size = ZSTD_compress(compressed.data(), dst_capacity, data.data, data.len, level);
        if (ZSTD_isError(compressed_size)) {
          LOG_ERROR("ZSTD Compress Failed!");
          out->append(data);
          return false;
        }
        PutFixed64(out, data.len);
        out->append(compressed.data(), compressed_size);
        return true;
      }
      LOG_ERROR("ZSTD Compress Failed! Input size is incorrect (too large or negative).");
      return false;
    }

    bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override {
      uint64_t org_size = DecodeFixed64(data.data);
      TsBufferBuilder builder(org_size);
      if (org_size != 0) {
        size_t ret_size = ZSTD_decompress(builder.data(), org_size, data.data + 8, data.len - 8);
        if (ZSTD_isError(ret_size) || ret_size != org_size) {
          LOG_ERROR("ZSTD Decompress Failed!");
          builder.assign(data.data, data.len);
          *out = builder.GetBuffer();
          return false;
        }
        *out = builder.GetBuffer();
        return true;
      }
      LOG_ERROR("ZSTD Decompress Failed! Incorrect original data size.")
      return false;
    }

    size_t GetUncompressedSize(TSSlice data, uint64_t count) const override {
      uint64_t org_size = DecodeFixed64(data.data);
      return org_size == 0 ? -1 : org_size;
    }
};

// ZLIB
class ZLIBString : public CompressorImpl {
 private:
  ZLIBString() = default;

 public:
  static constexpr int stride = -1;
  static ZLIBString &GetInstance() {
    static ZLIBString inst;
    return inst;
  }

    bool Compress(TSSlice data, uint64_t count, TsBufferBuilder *out, uint8_t level) const override {
      z_stream zs = {};
      if (deflateInit(&zs, level) != Z_OK) {
        LOG_ERROR("Zlib deflateInit failed!");
        return false;
      }

      zs.next_in = reinterpret_cast<Bytef*>(data.data);
      zs.avail_in = data.len;

      std::vector<Bytef> out_buffer(deflateBound(&zs, data.len));
      zs.next_out = out_buffer.data();
      zs.avail_out = out_buffer.size();

      int ret = deflate(&zs, Z_FINISH);
      if (ret != Z_STREAM_END) {
        LOG_ERROR("Zlib deflate failed during compression! Error code:%d", ret);
        deflateEnd(&zs);
        return false;
      }

      size_t compressed_size = zs.total_out;
      deflateEnd(&zs);
      PutFixed64(out, data.len);
      out->append(reinterpret_cast<const char*>(out_buffer.data()), compressed_size);
      return true;
    }

    bool Decompress(TSSlice data, uint64_t count, TsSliceGuard *out) const override {
      z_stream zs;
      memset(&zs, 0, sizeof(zs));
      if (inflateInit(&zs) != Z_OK) {
        LOG_ERROR("Zlib inflateInit failed!");
        return false;
      }

      uint64_t org_size = DecodeFixed64(data.data);
      TsBufferBuilder builder(org_size);
      zs.next_out = reinterpret_cast<Bytef*>(builder.data());
      zs.avail_out = org_size;

      zs.next_in = reinterpret_cast<Bytef*>(data.data + 8);
      zs.avail_in = data.len - 8;

      int  ret = inflate(&zs, Z_FINISH);
      if (ret != Z_STREAM_END) {
        LOG_ERROR("Zlib inflate failed during decompression! Error code: %d", ret);
        inflateEnd(&zs);
        return false;
      }
      inflateEnd(&zs);
      *out = builder.GetBuffer();
      return true;
    }

    size_t GetUncompressedSize(TSSlice data, uint64_t count) const override {
      uint64_t org_size = DecodeFixed64(data.data);
      return org_size == 0 ? -1 : org_size;
    }
};

template <class Compressor>
class ConcreateTsCompressor : public TsCompressorBase {
 private:
  ConcreateTsCompressor() = default;

 public:
  static TsCompressorBase &GetInstance() {
    static ConcreateTsCompressor inst;
    return inst;
  }
  bool Compress(TSSlice raw, const TsBitmapBase *bitmap, uint32_t count,
                TsBufferBuilder *out) const override {
    out->clear();
    assert(bitmap == nullptr || bitmap->GetCount() == count);
    int stride = Compressor::stride;
    if (stride < 0 || bitmap == nullptr || bitmap->IsAllValid()) {
      return Compressor::GetInstance().Compress(raw, count, out, 0);
    }
    assert(raw.len == count * stride);

    TsBufferBuilder valid_data;
    const char *data = raw.data;
    out->reserve(bitmap->GetValidCount() * stride);
    for (int i = 0; i < count; ++i) {
      if ((*bitmap)[i] != kValid) continue;
      valid_data.append(data + i * stride, stride);
    }
    auto buffer = valid_data.GetBuffer();

    return Compressor::GetInstance().Compress(buffer.AsSlice(), bitmap->GetValidCount(), out, 0);
  }

  bool Decompress(TSSlice raw, const TsBitmapBase *bitmap, uint32_t count,
                  TsSliceGuard *out) const override {
    int stride = Compressor::stride;
    if (stride < 0 || bitmap == nullptr || bitmap->IsAllValid()) {
      return Compressor::GetInstance().Decompress(raw, count, out);
    }
    TsSliceGuard buf;
    bool ok = Compressor::GetInstance().Decompress(raw, bitmap->GetValidCount(), &buf);
    if (!ok) {
      return false;
    }

    assert(buf.size() == bitmap->GetValidCount() * stride);
    const char *ptr = buf.data();
    std::string empty_buf(stride, 0);
    TsBufferBuilder builder;
    for (int i = 0; i < count; ++i) {
      if ((*bitmap)[i] == kValid) {
        builder.append(ptr, stride);
        ptr += stride;
      } else {
        builder.append(empty_buf);
      }
    }
    *out = builder.GetBuffer();
    return true;
  }
};

template <class Compressor>
class ConcreateGenCompressor : public GenCompressorBase {
 private:
  ConcreateGenCompressor() = default;

 public:
  static GenCompressorBase &GetInstance() {
    static ConcreateGenCompressor inst;
    return inst;
  }
  bool Compress(TSSlice raw, TsBufferBuilder *out, uint8_t level) const override {
    const CompressorImpl &comp = Compressor::GetInstance();
    return comp.Compress(raw, 0, out, level);
  }
  bool Decompress(TSSlice raw, TsSliceGuard *out) const override {
    const CompressorImpl &comp = Compressor::GetInstance();
    return comp.Decompress(raw, 0, out);
  }
};

}  // namespace kwdbts
