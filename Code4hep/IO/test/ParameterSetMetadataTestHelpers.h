#ifndef Code4hep_IO_ParameterSetMetadataTestHelpers_h
#define Code4hep_IO_ParameterSetMetadataTestHelpers_h

#include <bit>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../plugins/ParameterSetMetadata.h"

namespace c4h::test {
  inline std::vector<int> packedWords(std::vector<std::string> const& entries) {
    std::vector<int> words;
    for (auto const& entry : entries) {
      for (std::size_t offset = 0; offset < entry.size();) {
        std::uint32_t word = 0;
        for (unsigned byte = 0; byte != 4 && offset < entry.size(); ++byte, ++offset)
          word |= std::uint32_t(static_cast<unsigned char>(entry[offset])) << (8 * byte);
        words.push_back(std::bit_cast<int>(word));
      }
    }
    return words;
  }

  inline podio::Frame metadataFrame(std::vector<std::string> const& ids, std::vector<std::string> const& blobs) {
    std::vector<int> lengths;
    for (auto const& blob : blobs)
      lengths.push_back(static_cast<int>(blob.size()));
    podio::Frame metadata;
    metadata.putParameter(parameterSetVersionKey, 1);
    metadata.putParameter(parameterSetIDsKey, packedWords(ids));
    metadata.putParameter(parameterSetBlobsKey, packedWords(blobs));
    metadata.putParameter(parameterSetLengthsKey, std::move(lengths));
    return metadata;
  }
}  // namespace c4h::test

#endif
