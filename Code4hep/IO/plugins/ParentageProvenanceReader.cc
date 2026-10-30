#include <exception>
#include <memory>
#include <utility>
#include "ParentageProvenanceReader.h"

namespace c4h {
  ParentageProvenanceReader::ParentageProvenanceReader(std::set<edm::ProductProvenance> records)
      : records_(std::move(records)) {}

  std::set<edm::ProductProvenance> ParentageProvenanceReader::readProvenance(unsigned int) const {
    return records_;
  }

  void ParentageProvenanceReader::readProvenanceAsync(
      edm::WaitingTaskHolder task, edm::ModuleCallingContext const*, unsigned int,
      std::atomic<std::set<edm::ProductProvenance> const*>& writeTo) const noexcept {
    try {
      auto copy = std::make_unique<std::set<edm::ProductProvenance> const>(records_);
      std::set<edm::ProductProvenance> const* expected = nullptr;
      if (writeTo.compare_exchange_strong(expected, copy.get()))
        copy.release();
    } catch (...) {
      task.doneWaiting(std::current_exception());
      return;
    }
    task.doneWaiting({});
  }
}
