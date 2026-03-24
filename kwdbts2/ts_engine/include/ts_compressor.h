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

#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "data_type.h"
#include "lg_api.h"
#include "libkwdbts2.h"
#include "ts_common.h"
#include "ts_bitmap.h"
#include "ts_bufferbuilder.h"
#include "ts_coding.h"
#include "ts_sliceguard.h"

namespace kwdbts {

enum class BitmapType : uint8_t {
  kRaw = 0,
  kAllValid = 1,
  kAllNone = 2,
  kAllNull = 3,

  BITMAP_COMP_ALG_LAST
};

class TsCompressorBase;
class GenCompressorBase;
class CompressorManager {
 private:
  class TwoLevelCompressor {
   private:
    const TsCompressorBase* first_;
    const GenCompressorBase* second_;

    TsCompAlg first_algo_;
    GenCompAlg second_algo_;

    void EncodeAlgorithm(TsBufferBuilder* out, TsCompAlg first, GenCompAlg second) const {
      PutFixed16(out, static_cast<uint16_t>(first));
      PutFixed16(out, static_cast<uint16_t>(second));
    }

   public:
    TwoLevelCompressor(const TsCompressorBase* first, const GenCompressorBase* second,
                       TsCompAlg first_algo, GenCompAlg second_algo)
        : first_(first), second_(second) {
      first_algo_ = first == nullptr ? TsCompAlg::kPlain : first_algo;
      second_algo_ = second == nullptr ? GenCompAlg::kPlain : second_algo;
    }
    void Compress(TSSlice raw, const TsBitmapBase* bitmap, uint32_t count, TsBufferBuilder* out, int level) const;

    bool Decompress(TSSlice raw, const TsBitmapBase* bitmap, uint32_t count, TsSliceGuard* out) const;
    bool IsPlain() const { return (first_ == nullptr && second_ == nullptr); }

    std::tuple<TsCompAlg, GenCompAlg> GetAlgorithms() const;
  };

  std::unordered_map<TsCompAlg, TsCompressorBase*> ts_comp_;
  std::unordered_map<GenCompAlg, GenCompressorBase*> general_compressor_;
  // Regardless of how EngineOptions::compress_stage is set, default_algs_ maintains the encoding and compression
  // algorithms corresponding to different data types.
  std::unordered_map<DATATYPE, std::tuple<TsCompAlg, GenCompAlg>> default_algs_;
  std::unordered_map<GenCompAlg, std::array<int, 4>> algs_level_;

  CompressorManager();

  bool DoDecompressData(TsSliceGuard&& input, const TsBitmapBase* bitmap, uint64_t count,
                        TsSliceGuard* out) const;
  bool DoDecompressVarchar(GenCompAlg alg, TsSliceGuard&& input, TsSliceGuard* out) const;

 public:
  static CompressorManager& GetInstance() {
    static CompressorManager mgr;
    return mgr;
  }
  CompressorManager(const CompressorManager&) = delete;
  void operator=(const CompressorManager&) = delete;

  TwoLevelCompressor GetCompressor(TsCompAlg first, GenCompAlg second) const;
  std::tuple<TsCompAlg, GenCompAlg> GetAlgorithm(DATATYPE dtype, const AttributeInfo& attr_info) const;
  std::tuple<TsCompAlg, GenCompAlg> GetDefaultAlgorithm(DATATYPE dtype) const;
  TwoLevelCompressor GetDefaultCompressor(DATATYPE dtype) const;

  bool CompressData(TSSlice input, const TsBitmapBase* bitmap, uint64_t count, TsBufferBuilder* output,
                    TsCompAlg first, GenCompAlg second, int level) const;
  bool CompressVarchar(TSSlice input, TsBufferBuilder* output, GenCompAlg alg, int level) const;
  bool DecompressData(TsSliceGuard&& input, const TsBitmapBase* bitmap, uint64_t count, TsSliceGuard* out) const {
    if (input.size() < 4) {
      LOG_ERROR("Invalid input length %lu, too short", input.size());
      return false;
    }
    auto v = DecodeFixed32(input.data());
    if (v == 0) {
      input.RemovePrefix(sizeof(uint32_t));
      *out = std::move(input);
      return true;
    }

    return DoDecompressData(std::move(input), bitmap, count, out);
  }
  bool DecompressVarchar(TsSliceGuard&& input, TsSliceGuard* out) const {
    if (input.size() < 2) {
      LOG_ERROR("Invalid input length %lu, too short", input.size());
      return false;
    }
    uint16_t v;
    GetFixed16(&input, &v);
    auto alg = static_cast<GenCompAlg>(v);
    if (alg == GenCompAlg::kPlain) {
      *out = std::move(input);
      return true;
    }
    return DoDecompressVarchar(alg, std::move(input), out);
  }

  bool CompressBitmap(TsBitmapBase* bitmap, TsBufferBuilder* output) const;
  bool DecompressBitmap(TSSlice input, std::unique_ptr<TsBitmapBase>* bitmap, uint64_t count,
                        uint64_t* bytes_consumed) const;
};

}  //  namespace kwdbts
