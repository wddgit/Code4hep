#include <atomic>
#include <memory>
#include <set>
#include <vector>
#include "catch2/catch_all.hpp"
#include "DataFormats/Provenance/interface/ParentageRegistry.h"
#include "FWCore/Concurrency/interface/FinalWaitingTask.h"
#include "DataFormats/Provenance/interface/BranchIDListHelper.h"
#include "FWCore/Framework/interface/EventPrincipal.h"
#include "FWCore/Framework/interface/HistoryAppender.h"
#include "FWCore/Framework/interface/ProductResolversFactory.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "../plugins/ParentageMetadata.h"
#include "../plugins/ParentageProvenanceReader.h"

namespace {
  constexpr auto tag = "[Parentage]";
  edm::BranchID const root(11), parent(22), unknown(33), alias(44);
}

TEST_CASE("Parentage codec preserves nonempty vectors and absent versus present default-ID records", tag) {
  c4h::ParentageDictionary dictionary;
  edm::Parentage nonempty(std::vector<edm::BranchID>{parent, unknown});
  edm::Parentage empty;
  REQUIRE(empty.id() == edm::ParentageID{});
  REQUIRE_FALSE(empty.id().isValid());
  REQUIRE(c4h::parentageIDString(empty.id()).empty());
  REQUIRE(dictionary.insert(nonempty.id(), nonempty) == 0);
  REQUIRE(dictionary.insert(nonempty.id(), nonempty) == 0);
  c4h::EventParentage event;
  event.records = {edm::ProductProvenance(root, nonempty.id()), edm::ProductProvenance(parent, empty.id()),
                   edm::ProductProvenance(alias)};
  podio::Frame metadata, frame;
  dictionary.save(metadata);
  c4h::saveEventParentage(frame, event, dictionary);
  // Events store integer BranchIDs and dictionary indexes only.
  REQUIRE(frame.getParameters().getMap<std::string>().empty());
  REQUIRE(frame.getParameter<std::vector<int>>(c4h::eventParentageBranchesKey) ==
          std::vector<int>{static_cast<int>(root.id()), static_cast<int>(parent.id()), static_cast<int>(alias.id())});
  REQUIRE(frame.getParameter<std::vector<int>>(c4h::eventParentageIndexesKey) ==
          std::vector<int>{0, c4h::noRecordedParentsIndex, c4h::noRecordedParentsIndex});
  auto restored = c4h::ParentageDictionary::restore(metadata);
  REQUIRE(restored);
  auto restoredParents = restored->find(nonempty.id());
  REQUIRE(restoredParents);
  REQUIRE(restoredParents->parents() == nonempty.parents());
  auto snapshot = c4h::restoreEventParentage(frame, *restored);
  REQUIRE(snapshot.records == event.records);
  REQUIRE(snapshot.records.count(edm::ProductProvenance(unknown)) == 0);
  REQUIRE_FALSE(snapshot.records.find(edm::ProductProvenance(alias))->parentageID().isValid());
  REQUIRE(snapshot.records.find(edm::ProductProvenance(parent))->parentageID() == edm::ParentageID{});
  REQUIRE(restored->find(empty.id()) == nullptr);
  REQUIRE(metadata.getParameter<std::vector<std::string>>(c4h::parentageIDsKey) ==
          std::vector<std::string>{c4h::parentageIDString(nonempty.id())});
  restored->registerParentages();
  auto registeredParents = edm::ParentageRegistry::instance()->getMapped(nonempty.id());
  REQUIRE(registeredParents);
  REQUIRE(registeredParents->parents() == nonempty.parents());
  // The global registry cannot satisfy a reference missing from this file.
  REQUIRE_THROWS(c4h::restoreEventParentage(frame, {}));
  REQUIRE_FALSE(c4h::ParentageDictionary::restore(podio::Frame{}));
  REQUIRE_THROWS(c4h::restoreEventParentage(podio::Frame{}, dictionary));
  SECTION("Duplicate dictionary ID") {
    metadata.putParameter(c4h::parentageIDsKey,
                          std::vector<std::string>{c4h::parentageIDString(nonempty.id()), c4h::parentageIDString(nonempty.id())});
    REQUIRE_THROWS(c4h::ParentageDictionary::restore(metadata));
  }
  SECTION("Hash mismatch") {
    metadata.putParameter(c4h::parentagePrefix + c4h::parentageIDString(nonempty.id()), std::vector<std::string>{});
    REQUIRE_THROWS(c4h::ParentageDictionary::restore(metadata));
  }
  SECTION("Malformed ID") {
    metadata.putParameter(c4h::parentageIDsKey, std::vector<std::string>{std::string(31, 'a')});
    REQUIRE_THROWS(c4h::ParentageDictionary::restore(metadata));
  }
  SECTION("Default ID is not a dictionary entry") {
    metadata.putParameter(c4h::parentageIDsKey, std::vector<std::string>{""});
    REQUIRE_THROWS(c4h::ParentageDictionary::restore(metadata));
  }
  SECTION("Empty digest cannot be stored as a nondefault hexadecimal ID") {
    std::string text;
    empty.id().toString(text);
    REQUIRE(text == "d41d8cd98f00b204e9800998ecf8427e");
    metadata.putParameter(c4h::parentageIDsKey, std::vector<std::string>{text});
    REQUIRE_THROWS(c4h::ParentageDictionary::restore(metadata));
  }
  SECTION("Unequal event lengths") {
    frame.putParameter(c4h::eventParentageIndexesKey, std::vector<int>{});
    REQUIRE_THROWS(c4h::restoreEventParentage(frame, dictionary));
  }
  SECTION("Duplicate event record") {
    frame.putParameter(c4h::eventParentageBranchesKey, std::vector<int>{11, 11});
    frame.putParameter(c4h::eventParentageIndexesKey, std::vector<int>{-1, -1});
    REQUIRE_THROWS(c4h::restoreEventParentage(frame, dictionary));
  }
  SECTION("Zero BranchID") {
    frame.putParameter(c4h::eventParentageBranchesKey, std::vector<int>{0, 22, 44});
    REQUIRE_THROWS(c4h::restoreEventParentage(frame, dictionary));
  }
  SECTION("Dictionary index out of range") {
    auto const index = GENERATE(1, -2);
    frame.putParameter(c4h::eventParentageIndexesKey, std::vector<int>{index, -1, -1});
    REQUIRE_THROWS(c4h::restoreEventParentage(frame, dictionary));
  }
  SECTION("String-typed event vectors are rejected") {
    // podio keeps a separate map per parameter type, so start from an empty frame.
    podio::Frame strings;
    strings.putParameter(c4h::eventParentageVersionKey, 1);
    strings.putParameter(c4h::eventParentageBranchesKey, std::vector<std::string>{"11"});
    strings.putParameter(c4h::eventParentageIndexesKey, std::vector<std::string>{"-1"});
    REQUIRE_THROWS(c4h::restoreEventParentage(strings, dictionary));
  }
  REQUIRE_THROWS(dictionary.insert(nonempty.id(), empty));
  REQUIRE_THROWS(dictionary.insert(empty.id(), empty));
  edm::Parentage duplicate(std::vector<edm::BranchID>{parent, parent});
  REQUIRE_THROWS(dictionary.insert(duplicate.id(), duplicate));
}

TEST_CASE("Default-ID records round trip with an empty file dictionary and do not manufacture absent records", tag) {
  c4h::ParentageDictionary dictionary;
  podio::Frame metadata;
  dictionary.save(metadata);
  auto restored = c4h::ParentageDictionary::restore(metadata);
  REQUIRE(restored);
  auto ids = metadata.getParameter<std::vector<std::string>>(c4h::parentageIDsKey);
  REQUIRE(ids);
  REQUIRE(ids->empty());
  for (bool present : {true, false, true}) {
    c4h::EventParentage event;
    if (present)
      event.records.emplace(root, edm::Parentage{}.id());
    podio::Frame frame;
    c4h::saveEventParentage(frame, event, dictionary);
    auto snapshot = c4h::restoreEventParentage(frame, *restored);
    REQUIRE(snapshot.records == event.records);
    REQUIRE(snapshot.records.size() == (present ? 1 : 0));
    if (present) {
      REQUIRE(snapshot.records.begin()->parentageID() == edm::ParentageID{});
      REQUIRE(restored->find(snapshot.records.begin()->parentageID()) == nullptr);
    }
  }
}

TEST_CASE("Collecting a present default-ID record does not require or insert a dictionary entry", tag) {
  edm::ProductProvenance record(root, edm::Parentage{}.id());
  edm::ProductProvenanceRetriever retriever(
      0, std::make_unique<c4h::ParentageProvenanceReader>(std::set<edm::ProductProvenance>{record}));
  c4h::EventParentage event;
  c4h::ParentageDictionary dictionary;
  c4h::collectParentage(root, &retriever, event, dictionary);
  REQUIRE(event.records == std::set<edm::ProductProvenance>{record});
  REQUIRE(dictionary.find(record.parentageID()) == nullptr);
  podio::Frame frame, metadata;
  c4h::saveEventParentage(frame, event, dictionary);
  dictionary.save(metadata);
  REQUIRE(c4h::restoreEventParentage(frame, dictionary).records == event.records);
  REQUIRE(metadata.getParameter<std::vector<std::string>>(c4h::parentageIDsKey) == std::vector<std::string>{});
}

TEST_CASE("Snapshot retriever owns data across deep copy and event transitions", tag) {
  auto registry = std::make_shared<edm::ProductRegistry>();
  registry->setProcessOrder({"READ"});
  registry->setFrozen();
  auto helper = std::make_shared<edm::BranchIDListHelper>();
  helper->updateFromRegistry(*registry);
  edm::ParameterSet pset;
  pset.registerIt();
  edm::ProcessConfiguration config("READ", pset.id(), "test", {});
  edm::HistoryAppender appender;
  edm::EventPrincipal principal(registry, edm::productResolversFactory::makePrimary, helper, config, &appender);
  edm::Parentage parents(std::vector<edm::BranchID>{parent});
  for (int number : {1, 2, 3}) {
    std::set<edm::ProductProvenance> records;
    if (number == 1)
      records.emplace(root, parents.id());
    else if (number == 2)
      records.emplace(root);
    {
      edm::ProductProvenanceRetriever retriever(0, std::make_unique<c4h::ParentageProvenanceReader>(records));
      edm::EventAuxiliary aux(edm::EventID(1, 1, number), "", {}, false);
      principal.fillEventPrincipal(aux, nullptr, {}, {}, {}, retriever, nullptr, true);
    }
    auto record = principal.productProvenanceRetrieverPtr()->branchIDToProvenance(root);
    if (number == 3) {
      REQUIRE(record == nullptr);
    } else {
      REQUIRE(record);
      REQUIRE(record->parentageID() == (number == 1 ? parents.id() : edm::ParentageID{}));
    }
    principal.clearEventPrincipal();
  }
}

TEST_CASE("Snapshot reader completes asynchronous reads and transfers ownership once", tag) {
  c4h::ParentageProvenanceReader reader({edm::ProductProvenance(root)});
  std::atomic<std::set<edm::ProductProvenance> const*> target{nullptr};
  for (int i = 0; i != 2; ++i) {
    oneapi::tbb::task_group group;
    edm::FinalWaitingTask task(group);
    reader.readProvenanceAsync(edm::WaitingTaskHolder(group, &task), nullptr, 0, target);
    task.wait();
    REQUIRE(task.done());
    REQUIRE(target.load());
    REQUIRE(target.load()->count(edm::ProductProvenance(root)) == 1);
  }
  delete target.load();
}

TEST_CASE("Collection retains available ancestors and stops at unknown boundaries", tag) {
  edm::ProductProvenance rootRecord(root, std::vector<edm::BranchID>{parent, unknown});
  edm::ProductProvenance parentRecord(parent, std::vector<edm::BranchID>{});
  edm::ProductProvenanceRetriever retriever(
      0, std::make_unique<c4h::ParentageProvenanceReader>(std::set<edm::ProductProvenance>{rootRecord, parentRecord}));
  c4h::ParentageDictionary dictionary;
  c4h::EventParentage event;
  c4h::collectParentage(root, &retriever, event, dictionary);
  REQUIRE(event.records.size() == 2);
  auto rootParents = dictionary.find(rootRecord.parentageID());
  REQUIRE(rootParents);
  REQUIRE((rootParents->parents() == std::vector<edm::BranchID>{parent, unknown}));
  REQUIRE(parentRecord.parentageID() == edm::ParentageID{});
  REQUIRE(event.records.find(edm::ProductProvenance(parent))->parentageID() == edm::ParentageID{});
  REQUIRE(dictionary.find(parentRecord.parentageID()) == nullptr);
  edm::ProductProvenanceRetriever missing(
      0, std::make_unique<c4h::ParentageProvenanceReader>(
             std::set<edm::ProductProvenance>{edm::ProductProvenance(root, edm::ParentageID(std::string(32, '9')))}));
  c4h::EventParentage fresh;
  REQUIRE_THROWS(c4h::collectParentage(root, &missing, fresh, dictionary));
}

TEST_CASE("Shared ancestors are collected once per event and dictionary indexes are stable", tag) {
  // leafA <- shared <- seed and leafB <- shared: the shared chain is walked once.
  edm::BranchID const leafA(101), leafB(102), shared(103), seed(104);
  edm::ProductProvenance sharedRecord(shared, std::vector<edm::BranchID>{seed});
  edm::ProductProvenance leafARecord(leafA, std::vector<edm::BranchID>{shared});
  edm::ProductProvenance leafBRecord(leafB, std::vector<edm::BranchID>{shared, seed});
  edm::ProductProvenance seedRecord(seed);
  std::set<edm::ProductProvenance> const all{sharedRecord, leafARecord, leafBRecord, seedRecord};
  edm::ProductProvenanceRetriever retriever(0, std::make_unique<c4h::ParentageProvenanceReader>(all));
  c4h::ParentageDictionary dictionary;
  c4h::EventParentage event;
  c4h::collectParentage(leafA, &retriever, event, dictionary);
  REQUIRE(event.records.size() == 3);
  REQUIRE(dictionary.size() == 2);
  c4h::collectParentage(leafB, &retriever, event, dictionary);
  REQUIRE(event.records == all);
  REQUIRE(dictionary.size() == 3);
  // Insertion order defines indexes; indexes written by earlier events stay valid.
  REQUIRE(dictionary.index(leafARecord.parentageID()) == 0);
  REQUIRE(dictionary.index(sharedRecord.parentageID()) == 1);
  REQUIRE(dictionary.index(leafBRecord.parentageID()) == 2);
  REQUIRE(dictionary.index(seedRecord.parentageID()) == c4h::noRecordedParentsIndex);
  REQUIRE_THROWS(dictionary.index(edm::ParentageID(std::string(32, '9'))));
  podio::Frame metadata, frame;
  c4h::saveEventParentage(frame, event, dictionary);
  dictionary.save(metadata);
  auto restored = c4h::ParentageDictionary::restore(metadata);
  REQUIRE(restored);
  for (auto const& record : all)
    REQUIRE(restored->index(record.parentageID()) == dictionary.index(record.parentageID()));
  REQUIRE(c4h::restoreEventParentage(frame, *restored).records == all);
}
