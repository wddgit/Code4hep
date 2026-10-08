#ifndef Code4hep_IO_ParentageProvenanceReader_h
#define Code4hep_IO_ParentageProvenanceReader_h

#include <atomic>
#include <set>
#include "FWCore/Framework/interface/ProductProvenanceRetriever.h"

namespace c4h {
  class ParentageProvenanceReader final : public edm::ProvenanceReaderBase {
  public:
    explicit ParentageProvenanceReader(std::set<edm::ProductProvenance> records);
    std::set<edm::ProductProvenance> readProvenance(unsigned int) const override;
    void readProvenanceAsync(edm::WaitingTaskHolder task, edm::ModuleCallingContext const*, unsigned int,
                             std::atomic<std::set<edm::ProductProvenance> const*>& writeTo) const noexcept override;
  private:
    std::set<edm::ProductProvenance> const records_;
  };
}

#endif
