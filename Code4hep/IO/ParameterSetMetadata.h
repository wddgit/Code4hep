#ifndef Code4hep_IO_ParameterSetMetadata_h
#define Code4hep_IO_ParameterSetMetadata_h

#include <vector>

#include "DataFormats/Provenance/interface/ParameterSetID.h"
#include "podio/Frame.h"

namespace c4h {
  inline constexpr char parameterSetVersionKey[] = "Code4hep.edm.ParameterSetRegistry.version";
  inline constexpr char parameterSetIDsKey[] = "Code4hep.edm.ParameterSetRegistry.ids";
  inline constexpr char parameterSetBlobsKey[] = "Code4hep.edm.ParameterSetRegistry.blobs";
  inline constexpr char parameterSetLengthsKey[] = "Code4hep.edm.ParameterSetRegistry.lengths";

  // Registry snapshot: vector<int> fields, version {1}, four ints per compact ID.
  // Blobs use ceil(length/4) words each, with byte lengths in 0..INT_MAX.
  // Byte 0 occupies bits 0..7; each blob's padding is zero.
  podio::Frame saveParameterSets();

  // Fill the ParameterSet registry using the contents
  // of the metadata podio::Frame read from an input file.
  void restoreParameterSets(podio::Frame const& metadata);

  // Validate the packed layout and decode file membership without copying blobs.
  // An absent version yields no IDs, just as restoration ignores that frame.
  std::vector<edm::ParameterSetID> readParameterSetIDs(podio::Frame const& metadata);
}  // namespace c4h

#endif
