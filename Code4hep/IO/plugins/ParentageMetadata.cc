#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "DataFormats/Provenance/interface/ParentageRegistry.h"
#include "DataFormats/Provenance/interface/ProductProvenanceLookup.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "ParentageMetadata.h"

namespace c4h {
  namespace {
    static_assert(sizeof(int) == sizeof(edm::BranchID::value_type));

    [[noreturn]] void invalid(std::string const& reason) {
      throw edm::Exception(edm::errors::FormatIncompatibility) << "Invalid Code4hep parentage metadata: " << reason;
    }

    edm::BranchID branchID(std::string const& text) {
      unsigned int value = 0;
      auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 ||
          std::to_string(value) != text)
        invalid("invalid BranchID '" + text + "'");
      return edm::BranchID(value);
    }

    edm::ParentageID parentageID(std::string const& text) {
      if (text.size() != 32 || text.find_first_not_of("0123456789abcdef") != std::string::npos)
        invalid("ParentageID must be 32 lowercase hexadecimal characters");
      edm::ParentageID id(text);
      if (!id.isValid() || parentageIDString(id) != text)
        invalid("invalid ParentageID");
      return id;
    }

    std::vector<int> ints(podio::Frame const& frame, char const* key) {
      auto value = frame.getParameter<std::vector<int>>(key);
      if (!value)
        invalid(std::string("missing key ") + key);
      return *value;
    }
  }  // namespace

  std::string parentageIDString(edm::ParentageID const& id) {
    std::string text;
    if (id.isValid())
      id.toString(text);
    return text;
  }

  std::size_t ParentageDictionary::insert(edm::ParentageID const& id, edm::Parentage const& parents) {
    if (!id.isValid() || parents.parents().empty() || parents.id() != id)
      invalid("dictionary requires a nondefault ID matching a nonempty parent vector hash");
    // The ID is the hash of the parent vector, so an existing entry has the same parents.
    if (auto found = indexes_.find(id); found != indexes_.end())
      return found->second;
    std::set<edm::BranchID> seen;
    for (auto parent : parents.parents())
      if (!parent.isValid() || !seen.insert(parent).second)
        invalid("invalid or duplicate parent BranchID");
    indexes_.emplace(id, entries_.size());
    entries_.emplace_back(id, parents);
    return entries_.size() - 1;
  }

  edm::Parentage const* ParentageDictionary::find(edm::ParentageID const& id) const {
    auto found = indexes_.find(id);
    return found == indexes_.end() ? nullptr : &entries_[found->second].second;
  }

  int ParentageDictionary::index(edm::ParentageID const& id) const {
    if (!id.isValid())
      return noRecordedParentsIndex;
    auto found = indexes_.find(id);
    if (found == indexes_.end())
      invalid("event ParentageID is not in this file's dictionary");
    if (found->second > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      invalid("dictionary index exceeds INT_MAX");
    return static_cast<int>(found->second);
  }

  edm::ParentageID ParentageDictionary::id(int index) const {
    if (index == noRecordedParentsIndex)
      return {};
    if (index < 0 || static_cast<std::size_t>(index) >= entries_.size())
      invalid("event ParentageID index " + std::to_string(index) + " is not in this file's dictionary");
    return entries_[index].first;
  }

  std::set<edm::BranchID> ParentageDictionary::parentBranchIDs() const {
    std::set<edm::BranchID> result;
    for (auto const& entry : entries_)
      result.insert(entry.second.parents().begin(), entry.second.parents().end());
    return result;
  }

  void ParentageDictionary::save(podio::Frame& metadata) const {
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (auto const& [id, parents] : entries_) {
      auto text = parentageIDString(id);
      ids.push_back(text);
      std::vector<std::string> branches;
      branches.reserve(parents.parents().size());
      for (auto parent : parents.parents())
        branches.push_back(std::to_string(parent.id()));
      metadata.putParameter(parentagePrefix + text, std::move(branches));
    }
    metadata.putParameter(parentageVersionKey, 1);
    metadata.putParameter(parentageIDsKey, std::move(ids));
  }

  std::optional<ParentageDictionary> ParentageDictionary::restore(podio::Frame const& metadata) {
    auto version = metadata.getParameter<std::vector<int>>(parentageVersionKey);
    if (!version)
      return std::nullopt;
    if (*version != std::vector<int>{1})
      invalid("unsupported dictionary version");
    auto ids = metadata.getParameter<std::vector<std::string>>(parentageIDsKey);
    if (!ids)
      invalid(std::string("missing key ") + parentageIDsKey);
    ParentageDictionary dictionary;
    for (auto const& text : *ids) {
      auto id = parentageID(text);
      if (dictionary.find(id))
        invalid("duplicate dictionary ID");
      auto value = metadata.getParameter<std::vector<std::string>>(parentagePrefix + text);
      if (!value)
        invalid("missing parent vector");
      std::vector<edm::BranchID> parents;
      parents.reserve(value->size());
      for (auto const& branch : *value)
        parents.push_back(branchID(branch));
      dictionary.insert(id, edm::Parentage(std::move(parents)));
    }
    return dictionary;
  }

  void ParentageDictionary::registerParentages() const {
    // Each entry's ID was checked against its parents' hash, so an existing
    // registry entry for the same ID cannot have different parents.
    auto registry = edm::ParentageRegistry::instance();
    for (auto const& entry : entries_)
      registry->insertMapped(entry.second);
  }

  void collectParentage(edm::BranchID original,
                       edm::ProductProvenanceLookup const* retriever,
                       EventParentage& event, ParentageDictionary& dictionary) {
    if (!retriever)
      return;
    std::vector<edm::BranchID> pending{original};
    while (!pending.empty()) {
      auto branch = pending.back();
      pending.pop_back();
      // Records collected earlier this event, for this or another stored product,
      // already include their available ancestors.
      if (event.records.count(edm::ProductProvenance(branch)))
        continue;
      auto record = retriever->branchIDToProvenance(branch);
      if (!record)
        continue;
      if (record->branchID() != branch)
        invalid("retriever returned a different BranchID");
      event.records.insert(*record);
      // Preserve the record, but there is no distinguishable parent vector for a default ID.
      if (!record->parentageID().isValid())
        continue;
      // parentage() hides missing registry entries by substituting an empty vector.
      auto parents = edm::ParentageRegistry::instance()->getMapped(record->parentageID());
      if (!parents)
        invalid("referenced ParentageID is missing from the framework registry");
      dictionary.insert(record->parentageID(), *parents);
      pending.insert(pending.end(), parents->parents().begin(), parents->parents().end());
    }
  }

  void saveEventParentage(podio::Frame& frame, EventParentage const& event,
                         ParentageDictionary const& dictionary) {
    std::vector<int> branches, indexes;
    branches.reserve(event.records.size());
    indexes.reserve(event.records.size());
    for (auto const& record : event.records) {
      if (!record.branchID().isValid())
        invalid("invalid record BranchID");
      branches.push_back(std::bit_cast<int>(record.branchID().id()));
      indexes.push_back(dictionary.index(record.parentageID()));
    }
    frame.putParameter(eventParentageVersionKey, 1);
    frame.putParameter(eventParentageBranchesKey, std::move(branches));
    frame.putParameter(eventParentageIndexesKey, std::move(indexes));
  }

  EventParentage restoreEventParentage(podio::Frame const& frame, ParentageDictionary const& dictionary) {
    auto version = frame.getParameter<std::vector<int>>(eventParentageVersionKey);
    if (!version || *version != std::vector<int>{1})
      invalid("missing or unsupported event schema");
    auto const branches = ints(frame, eventParentageBranchesKey);
    auto const indexes = ints(frame, eventParentageIndexesKey);
    if (branches.size() != indexes.size())
      invalid("unequal parallel vector lengths");
    EventParentage event;
    for (std::size_t i = 0; i != branches.size(); ++i) {
      edm::BranchID const branch(std::bit_cast<edm::BranchID::value_type>(branches[i]));
      if (!branch.isValid())
        invalid("invalid record BranchID");
      if (!event.records.emplace(branch, dictionary.id(indexes[i])).second)
        invalid("duplicate record BranchID");
    }
    return event;
  }
}  // namespace c4h
