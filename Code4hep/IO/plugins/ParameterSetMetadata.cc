#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/Utilities/interface/EDMException.h"

#include "ParameterSetMetadata.h"

namespace c4h {
  namespace {
    static_assert(sizeof(int) == sizeof(std::int32_t) && std::numeric_limits<int>::digits == 31);
    static_assert(std::numeric_limits<unsigned char>::digits == 8);

    std::size_t wordCount(std::size_t bytes) { return bytes / 4 + (bytes % 4 != 0); }

    void pack(std::string_view bytes, std::vector<int>& words) {
      for (std::size_t offset = 0; offset < bytes.size();) {
        std::uint32_t word = 0;
        for (unsigned byte = 0; byte != 4 && offset < bytes.size(); ++byte, ++offset)
          word |= std::uint32_t(static_cast<unsigned char>(bytes[offset])) << (8 * byte);
        words.push_back(std::bit_cast<int>(word));
      }
    }

    std::string unpack(std::vector<int> const& words, std::size_t offset, std::size_t length) {
      std::string bytes(length, '\0');
      for (std::size_t i = 0; i != length; ++i) {
        auto const word = std::bit_cast<std::uint32_t>(words[offset + i / 4]);
        bytes[i] = std::bit_cast<char>(static_cast<unsigned char>((word >> (8 * (i % 4))) & 0xffU));
      }
      return bytes;
    }

    void invalid(char const* detail) {
      throw edm::Exception(edm::errors::FormatIncompatibility)
          << "Invalid Code4hep ParameterSet registry metadata: " << detail;
    }

    struct Layout {
      std::vector<int> const* ids = nullptr;
      std::vector<int> const* blobs = nullptr;
      std::vector<int> const* lengths = nullptr;
    };

    Layout validate(podio::Frame const& metadata) {
      auto const& ints = metadata.getParameters().getMap<int>();
      auto const version = ints.find(parameterSetVersionKey);
      if (version == ints.end()) {
        return {};
      }
      auto const ids = ints.find(parameterSetIDsKey);
      auto const blobs = ints.find(parameterSetBlobsKey);
      auto const lengths = ints.find(parameterSetLengthsKey);
      if (version->second != std::vector<int>{1})
        invalid("unsupported version");
      if (ids == ints.end() || blobs == ints.end() || lengths == ints.end())
        invalid("IDs, blobs and lengths must all be integer vectors");
      if (ids->second.size() % 4 != 0 || ids->second.size() / 4 != lengths->second.size())
        invalid("ID word count does not match blob count");

      std::size_t offset = 0;
      for (int length : lengths->second) {
        if (length < 0)
          invalid("negative blob length");
        auto const bytes = static_cast<std::size_t>(length);
        auto const count = wordCount(bytes);
        // Subtraction bounds both the sum and all subsequent word accesses.
        if (count > blobs->second.size() - offset)
          invalid("blob lengths exceed packed word count");
        if (bytes % 4 != 0 &&
            (std::bit_cast<std::uint32_t>(blobs->second[offset + count - 1]) >> (8 * (bytes % 4))) != 0)
          invalid("nonzero blob padding");
        offset += count;
      }
      if (offset != blobs->second.size())
        invalid("extra packed blob words");
      return {&ids->second, &blobs->second, &lengths->second};
    }

    std::vector<edm::ParameterSetID> decodeIDs(Layout const& layout) {
      std::vector<edm::ParameterSetID> ids;
      if (!layout.ids)
        return ids;
      ids.reserve(layout.ids->size() / 4);
      for (std::size_t offset = 0; offset != layout.ids->size(); offset += 4) {
        ids.emplace_back(unpack(*layout.ids, offset, 16));
        if (!ids.back().isValid())
          invalid("invalid ParameterSetID");
      }
      return ids;
    }
  }  // namespace

  podio::Frame saveParameterSets() {
    edm::pset::Registry::regmap_type persistentSets;
    edm::pset::Registry::instance()->fillMap(persistentSets);
    std::vector<int> ids, blobs, lengths;
    if (persistentSets.size() > ids.max_size() / 4)
      throw edm::Exception(edm::errors::FormatIncompatibility)
          << "Code4hep ParameterSet ID word count exceeds vector max_size";
    std::size_t blobWords = 0;
    for (auto const& [id, blob] : persistentSets) {
      auto const& bytes = blob.pset();
      if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw edm::Exception(edm::errors::FormatIncompatibility)
            << "Code4hep ParameterSet blob length exceeds INT_MAX";
      auto const count = wordCount(bytes.size());
      if (count > blobs.max_size() - blobWords)
        throw edm::Exception(edm::errors::FormatIncompatibility)
            << "Code4hep ParameterSet blob word count exceeds vector max_size";
      blobWords += count;
    }
    ids.reserve(persistentSets.size() * 4);
    blobs.reserve(blobWords);
    lengths.reserve(persistentSets.size());
    for (auto const& [id, blob] : persistentSets) {
      auto const& bytes = blob.pset();
      pack(id.compactForm(), ids);
      pack(bytes, blobs);
      lengths.push_back(static_cast<int>(bytes.size()));
    }
    podio::Frame metadata;
    metadata.putParameter(parameterSetVersionKey, 1);
    metadata.putParameter(parameterSetIDsKey, std::move(ids));
    metadata.putParameter(parameterSetBlobsKey, std::move(blobs));
    metadata.putParameter(parameterSetLengthsKey, std::move(lengths));
    return metadata;
  }

  std::vector<edm::ParameterSetID> readParameterSetIDs(podio::Frame const& metadata) {
    return decodeIDs(validate(metadata));
  }

  void restoreParameterSets(podio::Frame const& metadata) {
    auto const layout = validate(metadata);
    if (!layout.ids)
      return;
    auto const ids = decodeIDs(layout);
    // Decode everything before inserting; nested references resolve after insertion.
    std::vector<edm::ParameterSet> decoded;
    decoded.reserve(ids.size());
    std::size_t offset = 0;
    try {
      for (std::size_t i = 0; i != ids.size(); ++i) {
        auto const length = static_cast<std::size_t>((*layout.lengths)[i]);
        auto const blob = unpack(*layout.blobs, offset, length);
        decoded.emplace_back(std::string_view(blob.data(), blob.size()));
        decoded.back().setID(ids[i]);
        offset += wordCount(length);
      }
    } catch (edm::Exception const& ex) {
      if (ex.categoryCode() == edm::errors::Configuration)
        throw edm::Exception(edm::errors::FormatIncompatibility) << ex.message();
      throw;
    }
    auto& registry = *edm::pset::Registry::instance();
    for (auto const& pset : decoded)
      registry.insertMapped(pset);
  }
}  // namespace c4h
