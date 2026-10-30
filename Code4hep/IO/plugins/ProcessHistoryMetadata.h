#ifndef Code4hep_IO_ProcessHistoryMetadata_h
#define Code4hep_IO_ProcessHistoryMetadata_h

#include <map>
#include <string>

#include "DataFormats/Provenance/interface/ProcessHistoryRegistry.h"
#include "podio/Frame.h"

namespace c4h {
  inline constexpr char processHistoriesVersionKey[] = "Code4hep.edm.ProcessHistories.version";
  inline constexpr char processHistoriesIDsKey[] = "Code4hep.edm.ProcessHistories.ids";
  inline constexpr char processHistoriesPrefix[] = "Code4hep.edm.ProcessHistories.";
  inline constexpr char eventProcessHistoryKey[] = "Code4hep.edm.Event.processHistoryID";
  inline constexpr char runProcessHistoryKey[] = "Code4hep.edm.Event.runProcessHistoryID";
  inline constexpr char lumiProcessHistoryKey[] = "Code4hep.edm.Event.lumiProcessHistoryID";
  inline constexpr char luminosityBlockKey[] = "Code4hep.edm.Event.luminosityBlock";

  // Return the hexadecimal ID, using the empty history's ID if invalid.
  std::string historyIDString(edm::ProcessHistoryID const& id);

  // Store ProcessHistory IDs and ordered process configurations in the
  // metadata podio::Frame for an output file, as vectors of strings.
  void saveProcessHistories(podio::Frame& metadata,
                           std::map<edm::ProcessHistoryID, edm::ProcessHistory> const& histories);

  // Fill the supplied registry from the metadata podio::Frame read from
  // an input file. Return false if this metadata is absent.
  bool restoreProcessHistories(podio::Frame const& metadata, edm::ProcessHistoryRegistry& registry);

  // Read a ProcessHistory reference and verify it exists in the registry.
  edm::ProcessHistoryID readHistoryID(podio::Frame const& frame,
                                    std::string const& key,
                                    edm::ProcessHistoryRegistry const& registry);
}  // namespace c4h

#endif
