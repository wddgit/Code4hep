#ifndef Code4hep_IO_ParentageMetadata_h
#define Code4hep_IO_ParentageMetadata_h

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DataFormats/Provenance/interface/Parentage.h"
#include "DataFormats/Provenance/interface/ProductProvenance.h"
#include "podio/Frame.h"

namespace edm { class ProductProvenanceLookup; }

namespace c4h {
  // File-level dictionary: ordered ParentageIDs and, per ID, its parent BranchIDs.
  inline constexpr char parentageVersionKey[] = "Code4hep.edm.Parentage.version";
  inline constexpr char parentageIDsKey[] = "Code4hep.edm.Parentage.ids";
  inline constexpr char parentagePrefix[] = "Code4hep.edm.Parentage.parents.";

  // Per event: two parallel vector<int> parameters, one entry per provenance record.
  // branches: BranchID bit patterns (uint32 stored as int, never 0).
  // indexes: position of the record's ParentageID in the file-level ID list, or
  // noRecordedParentsIndex for the default ID.
  inline constexpr char eventParentageVersionKey[] = "Code4hep.edm.EventParentage.version";
  inline constexpr char eventParentageBranchesKey[] = "Code4hep.edm.EventParentage.branches";
  inline constexpr char eventParentageIndexesKey[] = "Code4hep.edm.EventParentage.indexes";
  inline constexpr int noRecordedParentsIndex = -1;

  inline constexpr char noRecordedParents[] = "no recorded parents (empty or recording disabled)";

  // The empty string encodes the default ID shared by empty parents and disabled recording.
  std::string parentageIDString(edm::ParentageID const& id);

  // Only nondefault IDs with hash-validated nonempty parent vectors are stored.
  // Entries keep their insertion order, because each event written refers to an
  // entry by its index before the file-level list is complete.
  class ParentageDictionary {
  public:
    // Returns the entry's index.
    std::size_t insert(edm::ParentageID const& id, edm::Parentage const& parents);
    edm::Parentage const* find(edm::ParentageID const& id) const;
    // Index for a record's ParentageID: noRecordedParentsIndex for the default ID.
    // Throws if a nondefault ID is not in the dictionary.
    int index(edm::ParentageID const& id) const;
    // ParentageID for an index from an event: the default ID for noRecordedParentsIndex.
    // Throws if the index is out of range.
    edm::ParentageID id(int index) const;
    std::size_t size() const { return entries_.size(); }
    // Every parent BranchID referenced by a dictionary entry. Every collected
    // ancestor record other than a stored product's own record is one of these.
    std::set<edm::BranchID> parentBranchIDs() const;
    void save(podio::Frame& metadata) const;
    void registerParentages() const;
    static std::optional<ParentageDictionary> restore(podio::Frame const& metadata);
  private:
    std::vector<std::pair<edm::ParentageID, edm::Parentage>> entries_;
    std::map<edm::ParentageID, std::size_t> indexes_;
  };

  struct EventParentage {
    // Absence differs from a present default-ID record; that record cannot tell
    // empty parents from disabled parentage recording.
    std::set<edm::ProductProvenance> records;
  };

  // Collect the provenance of a stored product and its available ancestors. Provenance
  // is keyed by the original BranchID, which differs from the stored product's own
  // BranchID only for an EDAlias. Ancestors already collected this event are not revisited.
  void collectParentage(edm::BranchID original,
                       edm::ProductProvenanceLookup const* retriever,
                       EventParentage& event, ParentageDictionary& dictionary);
  void saveEventParentage(podio::Frame& frame, EventParentage const& event,
                         ParentageDictionary const& dictionary);
  EventParentage restoreEventParentage(podio::Frame const& frame, ParentageDictionary const& dictionary);
}

#endif
