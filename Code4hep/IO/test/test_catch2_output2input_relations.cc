#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DataFormats/Provenance/interface/BranchIDListHelper.h"
#include "DataFormats/Provenance/interface/Provenance.h"
#include "DataFormats/Provenance/interface/processingOrderMerge.h"
#include "FWCore/Framework/interface/EventPrincipal.h"
#include "FWCore/Framework/interface/EventProcessor.h"
#include "FWCore/Framework/interface/HistoryAppender.h"
#include "FWCore/Framework/interface/LuminosityBlockPrincipal.h"
#include "FWCore/Framework/interface/ProductResolversFactory.h"
#include "FWCore/Framework/interface/RunPrincipal.h"
#include "FWCore/ParameterSetReader/interface/ParameterSetReader.h"
#include "FWCore/TestProcessor/interface/TestProcessor.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/FriendlyName.h"
#include "catch2/catch_all.hpp"
#include "edm4hep/edm4hep.h"
#include "podio/CollectionBufferFactory.h"
#include "podio/CollectionIDTable.h"
#include "podio/FrameCategories.h"
#include "podio/ICollectionProvider.h"
#include "podio/LinkCollection.h"
#include "podio/Reader.h"
#include "podio/Writer.h"

#include "../bin/ProvDump.h"
#include "../plugins/ParameterSetMetadata.h"
#include "../plugins/ParentageMetadata.h"
#include "../plugins/PodioFile.h"
#include "../plugins/ProcessHistoryMetadata.h"
#include "../plugins/ProductDescriptionMetadata.h"
#include "Output2InputTestHelpers.h"

using namespace c4h::test;

TEST_CASE("Tests of PodioOutput -> PodioSource relations, subsets and parentage", s_tag) {
  SECTION("SameProducerRelationsSurviveOutputSourceAndRewrite") {
    std::string const firstFile = "same_producer_relations.root";
    std::string const secondFile = "same_producer_relations_rewrite.root";
    auto const withSubset = GENERATE(false, true);
    auto config = setOutputFile(baseOutConfig, firstFile) + R"_(
process.testRelationsAndLinksProducer21 = cms.EDProducer('c4h::TestRelationsAndLinksProducer',
    tracks = cms.untracked.InputTag('NotUsed'),
    mcParticles = cms.untracked.InputTag('NotUsed'),
    linkToCollectionsMadeInModule = cms.untracked.bool(True),
    testValue = cms.untracked.int32(21))
process.moduleToTest(process.out, cms.Task(process.testRelationsAndLinksProducer21))
)_";
    {
      // Run the actual plugin without output: no Frame or converter can assign its IDs.
      auto producerConfig = config + "\nprocess.moduleToTest(process.testRelationsAndLinksProducer21)\n";
      edm::test::TestProcessor producer(edm::test::TestProcessor::Config{producerConfig});
      auto const event = producer.test();
      auto const tracks = event.get<edm4hep::TrackCollection>();
      auto const dqdxs = event.get<edm4hep::RecDqdxCollection>("recDqdxs");
      auto const particles = event.get<edm4hep::ReconstructedParticleCollection>("reconstructedParticles");
      auto const mcParticles = event.get<edm4hep::MCParticleCollection>("mcParticles");
      auto const links = event.get<podio::LinkCollection<edm4hep::Track, edm4hep::MCParticle>>("links");
      podio::CollectionIDTable ids;
      auto checkProductID = [&ids](auto const& collection, std::string const& name) {
        INFO(name);
        auto const expected = ids.add(name);
        REQUIRE(collection.size() == 1);
        REQUIRE(collection.getID() == expected);
        REQUIRE(collection.at(0).getObjectID().collectionID == expected);
        REQUIRE(collection.at(0).getObjectID().index == 0);
      };
      checkProductID(*tracks, "testRelationsAndLinksProducer21");
      checkProductID(*dqdxs, "testRelationsAndLinksProducer21recDqdxs");
      checkProductID(*particles, "testRelationsAndLinksProducer21reconstructedParticles");
      checkProductID(*mcParticles, "testRelationsAndLinksProducer21mcParticles");
      checkProductID(*links, "testRelationsAndLinksProducer21links");
      auto const track = tracks->at(0);
      auto const mcParticle = mcParticles->at(0);
      REQUIRE(track.getTracks().size() == 1);
      REQUIRE(track.getTracks().front() == track);
      REQUIRE(track.getTracks().front().getObjectID().collectionID == tracks->getID());
      REQUIRE(dqdxs->at(0).getTrack() == track);
      REQUIRE(particles->at(0).getTracks().size() == 1);
      REQUIRE(particles->at(0).getTracks().front() == track);
      REQUIRE(links->at(0).get<edm4hep::Track>() == track);
      REQUIRE(links->at(0).get<edm4hep::MCParticle>() == mcParticle);
      auto checkRawIDs = [](podio::CollectionBase const& collection, std::vector<podio::ObjectID> const& expected) {
        collection.prepareForWrite();
        auto buffers = const_cast<podio::CollectionBase&>(collection).getBuffers();
        REQUIRE(buffers.references != nullptr);
        std::vector<podio::ObjectID> actual;
        for (auto const& references : *buffers.references) {
          REQUIRE(references != nullptr);
          actual.insert(actual.end(), references->begin(), references->end());
        }
        REQUIRE(actual == expected);
      };
      auto const trackID = track.getObjectID();
      checkRawIDs(*tracks, {trackID});
      checkRawIDs(*dqdxs, {trackID});
      // The producer leaves the one-to-one decay-vertex relation unset.
      checkRawIDs(*particles, {trackID, {podio::ObjectID::invalid, 0}});
      checkRawIDs(*links, {trackID, mcParticle.getObjectID()});
    }
    if (withSubset) {
      config += R"_(
process.asubset = cms.EDProducer('c4h::TestParentageProducer',
    input = cms.InputTag('testRelationsAndLinksProducer21', 'mcParticles'), subset = cms.untracked.bool(True))
process.moduleToTest(process.out, cms.Task(process.testRelationsAndLinksProducer21, process.asubset))
)_";
    }
    {
      edm::test::TestProcessor tester(edm::test::TestProcessor::Config{config});
      REQUIRE_NOTHROW(tester.test());
    }
    completeTestProcessorHistory(firstFile, config);
    for (auto const& fileName : {firstFile, secondFile}) {
      if (fileName == secondFile) {
        std::string rewrite = R"_(import FWCore.ParameterSet.Config as cms
process = cms.Process('TEST2')
process.source = cms.Source('PodioSource', fileNames = cms.untracked.vstring('same_producer_relations.root'))
process.maxEvents = cms.untracked.PSet(input = cms.untracked.int32(1))
process.InitRootHandlers = cms.Service('InitRootHandlers')
process.JobReportService = cms.Service('JobReportService')
process.read21 = cms.EDAnalyzer('c4h::TestRelationsAndLinksAnalyzer',
    trackCollection = cms.untracked.InputTag('testRelationsAndLinksProducer21', '', 'TEST'),
    recDqdxCollection = cms.untracked.InputTag('testRelationsAndLinksProducer21', 'recDqdxs', 'TEST'),
    reconstructedParticleCollection = cms.untracked.InputTag('testRelationsAndLinksProducer21', 'reconstructedParticles', 'TEST'),
    trackToMCParticleLinkCollection = cms.untracked.InputTag('testRelationsAndLinksProducer21', 'links', 'TEST'),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(21),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(21),
    expectedTestValueInMCParticle = cms.untracked.int32(21))
process.out = cms.OutputModule('PodioOutputModule', fileName = cms.untracked.string('same_producer_relations_rewrite.root'))
process.end = cms.EndPath(process.read21 + process.out)
)_";
        edm::EventProcessor processor(edm::getPSetFromConfig(rewrite));
        processor.beginJob();
        REQUIRE(processor.run() == edm::EventProcessor::epSuccess);
        processor.endJob();
      }
      {
        auto reader = podio::makeReader(fileName);
        auto frame = reader.readEvent(0);
        auto const& tracks = frame.get<edm4hep::TrackCollection>("testRelationsAndLinksProducer21");
        REQUIRE(tracks.size() == 1);
        auto const track = tracks[0];
        REQUIRE(track.isAvailable());
        REQUIRE(track.getType() == 21);
        podio::CollectionIDTable ids;
        REQUIRE(tracks.getID() == ids.add("testRelationsAndLinksProducer21"));
        REQUIRE(track.getObjectID().collectionID == tracks.getID());
        REQUIRE(track.getObjectID().index == 0);
        auto buffers = const_cast<edm4hep::TrackCollection&>(tracks).getBuffers();
        bool foundSelfID = false;
        for (auto const& references : *buffers.references) {
          for (auto const& id : *references) {
            REQUIRE(id.collectionID == tracks.getID());
            REQUIRE(id.index == 0);
            foundSelfID = true;
          }
        }
        REQUIRE(foundSelfID);
        REQUIRE(track.getTracks().size() == 1);
        REQUIRE(track.getTracks().front().isAvailable());
        REQUIRE(track.getTracks().front() == track);
        auto const& dqdxs = frame.get<edm4hep::RecDqdxCollection>("testRelationsAndLinksProducer21recDqdxs");
        REQUIRE(dqdxs.at(0).getTrack() == track);
        auto const& particles = frame.get<edm4hep::ReconstructedParticleCollection>("testRelationsAndLinksProducer21reconstructedParticles");
        REQUIRE(particles.at(0).getTracks().front() == track);
        auto const& links = frame.get<podio::LinkCollection<edm4hep::Track, edm4hep::MCParticle>>("testRelationsAndLinksProducer21links");
        REQUIRE(links.at(0).get<edm4hep::Track>() == track);
        REQUIRE(links.at(0).get<edm4hep::MCParticle>().isAvailable());
        REQUIRE(links.at(0).get<edm4hep::MCParticle>().getPDG() == 21);
      }
      {
        SourceTester source(setInputFile(baseSourceConfig, fileName));
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
        source.openFile();
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
        source.readRun();
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
        source.readLuminosityBlock();
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
        auto event = source.readEvent();
        auto tracks = event.get<edm4hep::TrackCollection>("testRelationsAndLinksProducer21", "", "TEST");
        REQUIRE(tracks->at(0).getTracks().front().isAvailable());
        REQUIRE(tracks->at(0).getTracks().front() == tracks->at(0));
        auto dqdxs = event.get<edm4hep::RecDqdxCollection>("testRelationsAndLinksProducer21", "recDqdxs", "TEST");
        REQUIRE(dqdxs->at(0).getTrack() == tracks->at(0));
        auto latestTracks = event.get<edm4hep::TrackCollection>("testRelationsAndLinksProducer21", "", "");
        REQUIRE(latestTracks.isValid());
        REQUIRE(latestTracks->at(0).getType() == 21);
        REQUIRE(latestTracks->at(0) == tracks->at(0));
        auto latestDqdxs = event.get<edm4hep::RecDqdxCollection>("testRelationsAndLinksProducer21", "recDqdxs", "");
        REQUIRE(latestDqdxs.isValid());
        REQUIRE(latestDqdxs->at(0).getTrack() == latestTracks->at(0));
      }
    }
    std::filesystem::remove(firstFile);
    std::filesystem::remove(secondFile);
  }

  SECTION("NonemptySubsetOutputRoundTripAndUnscheduledOwners") {
    std::string const fileName = "subset_roundtrip.root";
    auto const omitOwner = GENERATE(false, true);
    auto config = setOutputFile(baseOutConfig, fileName) + R"_(
process.zowner = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag(''))
process.asubset = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag('zowner'), subset = cms.untracked.bool(True))
process.moduleToTest(process.out, cms.Task(process.asubset, process.zowner))
)_";
    if (omitOwner)
      config += "\nprocess.out.outputCommands = cms.untracked.vstring('drop *', 'keep *_asubset_*_*')\n";
    {
      edm::test::TestProcessor tester(edm::test::TestProcessor::Config{config});
      if (omitOwner) {
        try {
          tester.test();
          FAIL("Subset without owning target was written");
        } catch (cms::Exception const& ex) {
          REQUIRE(ex.category() == "InvalidSubset");
          std::string message = ex.what();
          REQUIRE(message.find("asubset") != std::string::npos);
          REQUIRE(message.find("element index 0") != std::string::npos);
          REQUIRE(message.find("target collection ID") != std::string::npos);
          REQUIRE(message.find("drop the subset") != std::string::npos);
        }
      } else {
        tester.test();
        tester.test();
      }
    }
    if (!omitOwner)
      completeTestProcessorHistory(fileName, config);
    auto reader = podio::makeReader(fileName);
    REQUIRE(reader.getEntries("events") == (omitOwner ? 0 : 2));
    if (!omitOwner) {
      auto metadata = reader.readFrame(podio::Category::Metadata, 0);
      std::map<edm::BranchID, std::string> names;
      auto products = c4h::restoreProductDescriptions(metadata, names);
      REQUIRE(products);
      REQUIRE(names.size() == 2);
      auto dictionary = c4h::ParentageDictionary::restore(metadata);
      REQUIRE(dictionary);
      for (unsigned index : {0U, 1U}) {
        auto frame = reader.readEvent(index);
        auto const& subset = frame.get<edm4hep::MCParticleCollection>("asubset");
        auto const& owner = frame.get<edm4hep::MCParticleCollection>("zowner");
        REQUIRE(subset.isSubsetCollection());
        REQUIRE(subset.size() == 1);
        REQUIRE(subset[0].isAvailable());
        REQUIRE(subset[0] == owner[0]);
        REQUIRE(subset[0].getPDG() == 11);
        auto const records = c4h::restoreEventParentage(frame, *dictionary).records;
        for (auto const& [id, name] : names)
          REQUIRE(records.count(edm::ProductProvenance(id)) == 1);
      }
      SourceTester source(setInputFile(baseSourceConfig, fileName));
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
      source.openFile();
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
      source.readRun();
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
      source.readLuminosityBlock();
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
      auto event = source.readEvent();
      auto subset = event.get<edm4hep::MCParticleCollection>("asubset", "", "TEST");
      auto owner = event.get<edm4hep::MCParticleCollection>("zowner", "", "TEST");
      REQUIRE(subset->at(0) == owner->at(0));
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
      REQUIRE(subset->at(0).getPDG() == 11);
      auto secondEvent = source.readEvent();
      auto secondSubset = secondEvent.get<edm4hep::MCParticleCollection>("asubset", "", "TEST");
      auto secondOwner = secondEvent.get<edm4hep::MCParticleCollection>("zowner", "", "TEST");
      REQUIRE(secondSubset->at(0) == secondOwner->at(0));
      REQUIRE(secondSubset->at(0).getPDG() == 11);
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsStop);
    }
    std::filesystem::remove(fileName);
  }

  SECTION("InputRejectsUnavailableSubsetAndSubsetTargetsWithFullMetadata") {
    auto const subsetTarget = GENERATE(false, true);
    std::string const fileName = "invalid_input_subset.root";
    edm::ParameterSet parameters;
    parameters.registerIt();
    edm::ProcessHistory history;
    history.push_back(edm::ProcessConfiguration("ORIGINAL", parameters.id(), "test", {}));
    history.setProcessHistoryID();
    std::map<edm::BranchID, edm::ProductDescription> descriptions;
    std::map<edm::BranchID, std::string> names;
    c4h::EventParentage parentage;
    podio::Frame frame;
    auto owner = std::make_unique<edm4hep::MCParticleCollection>();
    owner->create().setPDG(11);
    frame.put(std::move(owner), "owner");
    edm4hep::MCParticleCollection target;
    target.setSubsetCollection();
    target.push_back(frame.get<edm4hep::MCParticleCollection>("owner")[0]);
    frame.put(std::move(target), "targetsubset");
    podio::CollectionIDTable ids;
    auto const targetID = ids.add(subsetTarget ? "targetsubset" : "absentowner");
    auto buffers = podio::CollectionBufferFactory::instance().createBuffers(
        "edm4hep::MCParticleCollection", frame.get("owner")->getSchemaVersion(), true);
    REQUIRE(buffers);
    buffers->references->front()->push_back({0, targetID});
    auto broken = buffers->createCollection(std::move(*buffers), true);
    // Preserve prepared raw IDs so podio can write a deliberately malformed fixture without dereferencing it.
    broken->prepareAfterRead();
    frame.put(std::move(broken), "broken");
    edm4hep::EventHeaderCollection headers;
    headers.create().setRunNumber(1);
    headers[0].setEventNumber(1);
    frame.put(std::move(headers), "EventHeader");
    for (auto const& name : {"owner", "targetsubset", "broken"}) {
      edm::ProductDescription description(edm::InEvent, name, "ORIGINAL", "",
                                         edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
      descriptions.emplace(description.branchID(), description);
      names.emplace(description.branchID(), name);
    }
    frame.putParameter(c4h::eventProcessHistoryKey, c4h::historyIDString(history.id()));
    frame.putParameter(c4h::runProcessHistoryKey, c4h::historyIDString(history.id()));
    frame.putParameter(c4h::lumiProcessHistoryKey, c4h::historyIDString(history.id()));
    frame.putParameter(c4h::luminosityBlockKey, "1");
    c4h::saveEventParentage(frame, parentage, {});
    {
      auto writer = podio::makeWriter(fileName);
      writer.writeEvent(frame);
      auto metadata = c4h::saveParameterSets();
      c4h::saveProductDescriptions(metadata, descriptions, names);
      c4h::saveProcessHistories(metadata, {{history.id(), history}});
      c4h::ParentageDictionary{}.save(metadata);
      writer.writeFrame(metadata, podio::Category::Metadata);
      writer.finish();
    }
    try {
      edm::ProcessHistoryRegistry histories;
      c4h::PodioFile file(fileName, histories, false, 0, 0);
      FAIL("Invalid input subset was accepted");
    } catch (edm::Exception const& ex) {
      REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
      std::string const message = ex.what();
      REQUIRE(message.find("broken") != std::string::npos);
      REQUIRE(message.find("element index 0") != std::string::npos);
      REQUIRE(message.find(std::to_string(targetID)) != std::string::npos);
      REQUIRE(message.find(fileName) != std::string::npos);
      REQUIRE(message.find(subsetTarget ? "targetsubset" : "unavailable") != std::string::npos);
    }
    std::filesystem::remove(fileName);
  }

  SECTION("OutputSubsetValidationUsesPresentOwningPayloads") {
    auto const scenario = GENERATE(0, 1, 2, 3);
    // Missing placeholder, another subset with the target ID, unavailable entry, empty subset.
    std::string const fileName = "subset_payload_validation.root";
    auto outputConfig = setOutputFile(baseOutConfig, fileName);
    bool selectedTarget = true;
    if (scenario == 1) {
      SECTION("Selected subset target has a known name") {}
      SECTION("Unselected target has an unknown name") {
        selectedTarget = false;
        outputConfig += "\nprocess.out.outputCommands = cms.untracked.vstring('drop *', 'keep *_subset_*_*')\n";
      }
    }
    edm::test::TestProcessor::Config config{outputConfig};
    auto subsetToken = config.produces<edm4hep::MCParticleCollection>("subset");
    auto targetToken = config.produces<edm4hep::MCParticleCollection>("target");
    podio::CollectionIDTable ids;
    auto const targetID = ids.add("target");
    edm4hep::MCParticleCollection hiddenOwner;
    hiddenOwner.setID(targetID);
    auto element = hiddenOwner.create();
    auto subset = std::make_unique<edm4hep::MCParticleCollection>();
    subset->setSubsetCollection();
    if (scenario < 2)
      subset->push_back(element);
    if (scenario == 2) {
      auto buffers = podio::CollectionBufferFactory::instance().createBuffers(
          "edm4hep::MCParticleCollection", subset->getSchemaVersion(), true);
      REQUIRE(buffers);
      buffers->references->front()->push_back({0, targetID});
      auto base = buffers->createCollection(std::move(*buffers), true);
      base->prepareAfterRead();
      class MissingProvider final : public podio::ICollectionProvider {
        bool get(uint32_t, podio::CollectionBase*&) const final { return false; }
      } provider;
      base->setReferences(&provider);
      subset = std::make_unique<edm4hep::MCParticleCollection>(
          std::move(dynamic_cast<edm4hep::MCParticleCollection&>(*base)));
      REQUIRE_FALSE(std::as_const(*subset)[0].isAvailable());
    }
    {
      edm::test::TestProcessor tester(config);
      auto write = [&] {
        if (scenario == 1) {
          auto target = std::make_unique<edm4hep::MCParticleCollection>();
          target->setSubsetCollection();
          target->setID(targetID);
          tester.test(std::make_pair(subsetToken, std::move(subset)),
                      std::make_pair(targetToken, std::move(target)));
        } else {
          tester.test(std::make_pair(subsetToken, std::move(subset)));
        }
      };
      if (scenario == 3) {
        REQUIRE_NOTHROW(write());
      } else {
        try {
          write();
          FAIL("Unsafe subset was written");
        } catch (cms::Exception const& ex) {
          REQUIRE(ex.category() == "InvalidSubset");
          std::string message = ex.what();
          REQUIRE(message.find("Subset collection 'subset'") != std::string::npos);
          REQUIRE(message.find("element index 0") != std::string::npos);
          REQUIRE(message.find(std::to_string(targetID)) != std::string::npos);
          if (scenario == 1)
            REQUIRE(message.find(selectedTarget ? "('target')" : "(name unknown)") != std::string::npos);
          if (scenario == 2)
            REQUIRE(message.find("unavailable") != std::string::npos);
        }
      }
    }
    auto reader = podio::makeReader(fileName);
    REQUIRE(reader.getEntries("events") == (scenario == 3 ? 1 : 0));
    if (scenario == 3) {
      auto frame = reader.readEvent(0);
      REQUIRE(frame.get("subset")->isSubsetCollection());
      REQUIRE(frame.get("subset")->empty());
      REQUIRE(frame.get("target")->empty());
      REQUIRE_FALSE(frame.get("target")->isSubsetCollection());
    }
    std::filesystem::remove(fileName);
  }

  SECTION("OutputRejectsConflictingCollectionNames") {
    std::string const fileName = "conflicting_collections.root";
    edm::test::TestProcessor::Config config{setOutputFile(baseOutConfig, fileName)};
    std::vector<edm::ProductDescription> descriptions;
    std::string name;
    auto addParticles = [&](std::string const& module, std::string const& instance,
                            std::string const& process = "TEST") {
      auto token = process == "TEST" ? edm::test::ProcessToken{} : config.addExtraProcess(process);
      config.produces<edm4hep::MCParticleCollection>(module, instance, token);
      descriptions.emplace_back(edm::InEvent, module, process, instance,
                                edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    };
    SECTION("Concatenated labels") {
      name = "producerparticles";
      addParticles("producer", "particles");
      addParticles("producerparticles", "");
    }
    SECTION("Different processes") {
      name = "producerparticles";
      addParticles("producer", "particles");
      addParticles("producer", "particles", "PREVIOUS");
    }
    SECTION("Different types") {
      name = "producerparticles";
      addParticles("producer", "particles");
      config.produces<edm4hep::EventHeaderCollection>("producer", "particles");
      descriptions.emplace_back(edm::InEvent, "producer", "TEST", "particles",
                                edm::TypeID(typeid(edm4hep::EventHeaderCollection)), false);
    }
    SECTION("Reserved EventHeader") {
      name = "EventHeader";
      addParticles("EventHeader", "");
    }
    SECTION("Reserved EventHeader from concatenation") {
      name = "EventHeader";
      addParticles("Event", "Header");
    }
    SECTION("Current-process EventHeader collection") {
      name = "EventHeader";
      config.produces<edm4hep::EventHeaderCollection>("EventHeader");
      descriptions.emplace_back(edm::InEvent, "EventHeader", "TEST", "",
                                 edm::TypeID(typeid(edm4hep::EventHeaderCollection)), false);
    }
    try {
      edm::test::TestProcessor tester(config);
      tester.test();
      FAIL("Conflicting collection names were accepted");
    } catch (cms::Exception const& ex) {
      REQUIRE(ex.category() == "Configuration");
      std::string const message = ex.what();
      REQUIRE(message.find("Duplicate podio collection name '" + name + "'") != std::string::npos);
      REQUIRE(message.find("Rename the module/product instance or change the output selection") != std::string::npos);
      for (auto const& description : descriptions) {
        REQUIRE(message.find("module='" + description.moduleLabel() + "'") != std::string::npos);
        REQUIRE(message.find("process='" + description.processName() + "'") != std::string::npos);
        REQUIRE(message.find("instance='" + description.productInstanceName() + "'") != std::string::npos);
        REQUIRE(message.find("BranchID=" + std::to_string(description.branchID().id())) != std::string::npos);
      }
      if (name == "EventHeader")
        REQUIRE(message.find("automatically generated by PodioOutputModule") != std::string::npos);
    }
    std::filesystem::remove(fileName);
  }

  SECTION("OutputRejectsMurmur32CollectionIDCollisions") {
    // Use podio's actual hash API, without duplicating its implementation or injecting IDs.
    std::unordered_map<std::uint32_t, std::string> seen;
    std::string first, second;
    std::uint32_t collisionID = 0;
    for (unsigned int i = 0; i != 1000000; ++i) {
      auto name = "collision" + std::to_string(i);
      podio::CollectionIDTable table;
      auto const id = table.add(name);
      auto const [found, added] = seen.emplace(id, name);
      if (!added) {
        first = found->second;
        second = name;
        collisionID = id;
        break;
      }
    }
    REQUIRE_FALSE(first.empty());
    REQUIRE(first != second);
    std::string const fileName = "hash_collision.root";
    edm::test::TestProcessor::Config config{setOutputFile(baseOutConfig, fileName)};
    config.produces<edm4hep::MCParticleCollection>(first);
    config.produces<edm4hep::MCParticleCollection>(second);
    try {
      edm::test::TestProcessor tester(config);
      tester.test();
      FAIL("Murmur32 collection ID collision was accepted");
    } catch (cms::Exception const& ex) {
      REQUIRE(ex.category() == "Configuration");
      std::string const message = ex.what();
      REQUIRE(message.find("Murmur32 collection ID collision") != std::string::npos);
      REQUIRE(message.find(first) != std::string::npos);
      REQUIRE(message.find(second) != std::string::npos);
      REQUIRE(message.find("collection ID " + std::to_string(collisionID)) != std::string::npos);
      for (auto const& module : {first, second}) {
        edm::ProductDescription description(edm::InEvent, module, "TEST", "",
                                            edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
        REQUIRE(message.find("module='" + module + "'") != std::string::npos);
        REQUIRE(message.find("BranchID=" + std::to_string(description.branchID().id())) != std::string::npos);
      }
      REQUIRE(message.find("Rename the module/product instance or change the output selection") != std::string::npos);
    }
    std::filesystem::remove(fileName);
  }

  SECTION("SelectedEDAliasKeepsOriginalCollectionNamesAndIDs") {
    std::string const firstFile = "selected_alias.root";
    std::string const secondFile = "selected_alias_rewrite.root";
    std::string const original = "testRelationsAndLinksProducer21";
    auto const config = setOutputFile(baseOutConfig, firstFile) + R"_(
process.testRelationsAndLinksProducer21 = cms.EDProducer('c4h::TestRelationsAndLinksProducer',
    tracks = cms.untracked.InputTag('NotUsed'),
    mcParticles = cms.untracked.InputTag('NotUsed'),
    linkToCollectionsMadeInModule = cms.untracked.bool(True),
    testValue = cms.untracked.int32(21))
process.selected = cms.EDAlias(testRelationsAndLinksProducer21 = cms.EDAlias.allProducts())
process.out.outputCommands = cms.untracked.vstring('drop *', 'keep *_selected_*_*')
process.moduleToTest(process.out, cms.Task(process.testRelationsAndLinksProducer21))
)_";
    {
      edm::test::TestProcessor tester(edm::test::TestProcessor::Config{config});
      REQUIRE_NOTHROW(tester.test());
    }
    completeTestProcessorHistory(firstFile, config);
    std::string const rewrite = R"_(import FWCore.ParameterSet.Config as cms
process = cms.Process('TEST2')
process.source = cms.Source('PodioSource', fileNames = cms.untracked.vstring('selected_alias.root'))
process.maxEvents = cms.untracked.PSet(input = cms.untracked.int32(1))
process.InitRootHandlers = cms.Service('InitRootHandlers')
process.JobReportService = cms.Service('JobReportService')
process.readSelected = cms.EDAnalyzer('c4h::TestRelationsAndLinksAnalyzer',
    trackCollection = cms.untracked.InputTag('selected', '', 'TEST'),
    recDqdxCollection = cms.untracked.InputTag('selected', 'recDqdxs', 'TEST'),
    reconstructedParticleCollection = cms.untracked.InputTag('selected', 'reconstructedParticles', 'TEST'),
    trackToMCParticleLinkCollection = cms.untracked.InputTag('selected', 'links', 'TEST'),
    expectedTrackTypeInTrackCollection = cms.untracked.int32(21),
    expectedTrackTypeTrackInRelationOrLink = cms.untracked.int32(21),
    expectedTestValueInMCParticle = cms.untracked.int32(21))
process.out = cms.OutputModule('PodioOutputModule', fileName = cms.untracked.string('selected_alias_rewrite.root'))
process.end = cms.EndPath(process.readSelected + process.out)
)_";
    for (auto const& fileName : {firstFile, secondFile}) {
      INFO(fileName);
      if (fileName == secondFile) {
        // Read only through the alias labels and write again: storage names must not change.
        edm::EventProcessor processor(edm::getPSetFromConfig(rewrite));
        processor.beginJob();
        REQUIRE(processor.run() == edm::EventProcessor::epSuccess);
        processor.endJob();
      }
      {
        auto reader = podio::makeReader(fileName);
        auto frame = reader.readEvent(0);
        podio::CollectionIDTable ids;
        auto const& tracks = frame.get<edm4hep::TrackCollection>(original);
        REQUIRE(tracks.size() == 1);
        REQUIRE(tracks.getID() == ids.add(original));
        REQUIRE(tracks[0].getTracks().front() == tracks[0]);
        auto const& dqdxs = frame.get<edm4hep::RecDqdxCollection>(original + "recDqdxs");
        REQUIRE(dqdxs.at(0).getTrack() == tracks[0]);
        auto const& links =
            frame.get<podio::LinkCollection<edm4hep::Track, edm4hep::MCParticle>>(original + "links");
        REQUIRE(links.at(0).get<edm4hep::Track>() == tracks[0]);
        REQUIRE(links.at(0).get<edm4hep::MCParticle>().getPDG() == 21);
        for (auto const& name : frame.getAvailableCollections()) {
          INFO(name);
          REQUIRE(name.find("selected") == std::string::npos);
        }

        auto metadata = reader.readFrame(podio::Category::Metadata, 0);
        std::map<edm::BranchID, std::string> names;
        auto products = c4h::restoreProductDescriptions(metadata, names);
        REQUIRE(products);
        REQUIRE(names.size() == 5);
        std::map<edm::BranchID, edm::ProductDescription> descriptions;
        for (auto const& entry : products->productList())
          descriptions.emplace(entry.second.branchID(), entry.second);
        REQUIRE(descriptions.size() == 10);
        for (auto const& [id, name] : names) {
          auto const& alias = descriptions.at(id);
          REQUIRE(alias.moduleLabel() == "selected");
          REQUIRE(alias.processName() == "TEST");
          auto const& target = descriptions.at(alias.aliasForBranchID());
          REQUIRE(target.dropped());
          REQUIRE(target.moduleLabel() == original);
          REQUIRE(name == target.moduleLabel() + target.productInstanceName());
        }
        auto dictionary = c4h::ParentageDictionary::restore(metadata);
        REQUIRE(dictionary);
        // Records are keyed by the originals' BranchIDs, never by the aliases'.
        auto const parentage = c4h::restoreEventParentage(frame, *dictionary);
        for (auto const& [id, name] : names) {
          REQUIRE(parentage.records.count(edm::ProductProvenance(descriptions.at(id).aliasForBranchID())) == 1);
          REQUIRE(parentage.records.count(edm::ProductProvenance(id)) == 0);
        }
      }
      {
        SourceTester source(setInputFile(baseSourceConfig, fileName));
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
        source.openFile();
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
        source.readRun();
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
        source.readLuminosityBlock();
        REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
        auto event = source.readEvent();
        auto tracks = event.get<edm4hep::TrackCollection>("selected", "", "TEST");
        REQUIRE(tracks.isValid());
        REQUIRE(tracks->at(0).getType() == 21);
        REQUIRE(tracks->at(0).getTracks().front() == tracks->at(0));
        auto dqdxs = event.get<edm4hep::RecDqdxCollection>("selected", "recDqdxs", "TEST");
        REQUIRE(dqdxs->at(0).getTrack() == tracks->at(0));
        auto links = event.get<podio::LinkCollection<edm4hep::Track, edm4hep::MCParticle>>("selected", "links", "TEST");
        REQUIRE(links->at(0).get<edm4hep::Track>() == tracks->at(0));
        REQUIRE(links->at(0).get<edm4hep::MCParticle>().getPDG() == 21);
        // The original labels are not accessible, although they name the stored collections.
        REQUIRE_FALSE(event.get<edm4hep::TrackCollection>(original, "", "TEST").isValid());
        REQUIRE_FALSE(event.get<edm4hep::RecDqdxCollection>(original, "recDqdxs", "TEST").isValid());
      }
    }
    std::filesystem::remove(firstFile);
    std::filesystem::remove(secondFile);
  }

  SECTION("SelectingEDAliasAndOriginalIsRejectedByTheFramework") {
    std::string const fileName = "selected_alias_and_original.root";
    auto const config = setOutputFile(baseOutConfig, fileName) + R"_(
process.original = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag(''))
process.selected = cms.EDAlias(original = cms.EDAlias.allProducts())
process.moduleToTest(process.out, cms.Task(process.original))
)_";
    try {
      edm::test::TestProcessor tester(edm::test::TestProcessor::Config{config});
      tester.test();
      FAIL("Selecting both an EDAlias and its original was accepted");
    } catch (cms::Exception const& ex) {
      std::string const message = ex.what();
      REQUIRE(message.find("Two (or more) equivalent branches have been selected for output") != std::string::npos);
    }
    std::filesystem::remove(fileName);
  }

  SECTION("EDAliasParentageUsesTheOriginalBranchID") {
    std::string const fileName = "alias_parentage.root";
    auto const config = setOutputFile(baseOutConfig, fileName) + R"_(
process.seed = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag(''))
process.leaf = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag('seed'))
process.selectedLeaf = cms.EDAlias(leaf = cms.EDAlias.allProducts())
process.out.outputCommands = cms.untracked.vstring('drop *', 'keep *_selectedLeaf_*_*')
process.moduleToTest(process.out, cms.Task(process.seed, process.leaf))
)_";
    {
      edm::test::TestProcessor tester(edm::test::TestProcessor::Config{config});
      tester.test();
    }
    completeTestProcessorHistory(fileName, config);
    auto const friendly = edm::friendlyname::friendlyName("edm4hep::MCParticleCollection");
    edm::BranchID const seed(friendly + "_seed__TEST.");
    edm::BranchID const leaf(friendly + "_leaf__TEST.");
    edm::BranchID const selectedLeaf(friendly + "_selectedLeaf__TEST.");
    {
      auto reader = podio::makeReader(fileName);
      auto frame = reader.readEvent(0);
      REQUIRE(frame.get<edm4hep::MCParticleCollection>("leaf").size() == 1);
      auto dictionary = c4h::ParentageDictionary::restore(reader.readFrame(podio::Category::Metadata, 0));
      REQUIRE(dictionary);
      auto const snapshot = c4h::restoreEventParentage(frame, *dictionary);
      REQUIRE(snapshot.records.size() == 2);
      auto leafRecord = snapshot.records.find(edm::ProductProvenance(leaf));
      REQUIRE(leafRecord != snapshot.records.end());
      REQUIRE(dictionary->find(leafRecord->parentageID())->parents() == std::vector<edm::BranchID>{seed});
      REQUIRE(snapshot.records.count(edm::ProductProvenance(selectedLeaf)) == 0);
    }
    SourceTester source(setInputFile(baseSourceConfig, fileName));
    REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
    source.openFile();
    REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
    source.readRun();
    REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
    source.readLuminosityBlock();
    REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
    auto event = source.readEvent();
    REQUIRE(event.get<edm4hep::MCParticleCollection>("selectedLeaf", "", "TEST")->at(0).getPDG() == 11);
    REQUIRE_FALSE(event.get<edm4hep::MCParticleCollection>("leaf", "", "TEST").isValid());
    std::filesystem::remove(fileName);
  }

  SECTION("ActualProducerParentageSurvivesDroppedAncestorsAndInput") {
    const std::string fileName = "parentage_chain.root";
    auto config = setOutputFile(baseOutConfig, fileName) + R"_(
process.seed = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag(''))
process.middle = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag('seed'))
process.leaf = cms.EDProducer('c4h::TestParentageProducer', input = cms.InputTag('middle'))
process.out.outputCommands = cms.untracked.vstring('drop *', 'keep *_leaf_*_*')
process.moduleToTest(process.out, cms.Task(process.seed, process.middle, process.leaf))
)_";
    {
      edm::test::TestProcessor tester(edm::test::TestProcessor::Config{config});
      tester.test();
      tester.test();
    }
    completeTestProcessorHistory(fileName, config);
    auto const friendly = edm::friendlyname::friendlyName("edm4hep::MCParticleCollection");
    edm::BranchID const seed(friendly + "_seed__TEST.");
    edm::BranchID const middle(friendly + "_middle__TEST.");
    edm::BranchID const leaf(friendly + "_leaf__TEST.");
    c4h::ParentageDictionary dictionary;
    std::vector<c4h::EventParentage> snapshots;
    {
      auto reader = podio::makeReader(fileName);
      auto const metadata = reader.readFrame(podio::Category::Metadata, 0);
      auto restoredDictionary = c4h::ParentageDictionary::restore(metadata);
      REQUIRE(restoredDictionary);
      dictionary = std::move(*restoredDictionary);
      // Dropped ancestors are saved as metadata-only (dropped) descriptions.
      std::map<edm::BranchID, std::string> names;
      auto products = c4h::restoreProductDescriptions(metadata, names);
      REQUIRE(products);
      REQUIRE((names == std::map<edm::BranchID, std::string>{{leaf, "leaf"}}));
      std::map<edm::BranchID, edm::ProductDescription> descriptions;
      for (auto const& entry : products->productList())
        descriptions.emplace(entry.second.branchID(), entry.second);
      REQUIRE(descriptions.size() == 3);
      for (auto const& [id, label] : {std::pair{seed, "seed"}, std::pair{middle, "middle"}}) {
        auto const& description = descriptions.at(id);
        REQUIRE(description.dropped());
        REQUIRE(description.moduleLabel() == label);
        REQUIRE(description.processName() == "TEST");
      }
      for (unsigned int i : {0U, 1U}) {
        snapshots.push_back(c4h::restoreEventParentage(reader.readEvent(i), dictionary));
        auto const& snapshot = snapshots.back();
        REQUIRE(snapshot.records.size() == 3);
        auto leafRecord = snapshot.records.find(edm::ProductProvenance(leaf));
        auto middleRecord = snapshot.records.find(edm::ProductProvenance(middle));
        auto seedRecord = snapshot.records.find(edm::ProductProvenance(seed));
        REQUIRE(leafRecord != snapshot.records.end());
        REQUIRE(middleRecord != snapshot.records.end());
        REQUIRE(seedRecord != snapshot.records.end());
        auto leafParents = dictionary.find(leafRecord->parentageID());
        auto middleParents = dictionary.find(middleRecord->parentageID());
        REQUIRE(leafParents);
        REQUIRE(middleParents);
        REQUIRE(leafParents->parents() == std::vector<edm::BranchID>{middle});
        REQUIRE(middleParents->parents() == std::vector<edm::BranchID>{seed});
        REQUIRE(seedRecord->parentageID() == edm::ParentageID{});
        REQUIRE(dictionary.find(seedRecord->parentageID()) == nullptr);
      }
    }
    {
      // The provenance dump labels the dropped ancestors from their saved descriptions.
      std::vector<char const*> argv{"c4hProvDump", fileName.c_str(), "-x"};
      std::ostringstream out, err;
      // The status is not checked: the synthetic TestProcessor history configuration has
      // no module PSets, so the module listing that follows the dependencies reports them.
      c4h::provDump(static_cast<int>(argv.size()), argv.data(), out, err);
      auto const text = out.str();
      REQUIRE(text.find("Ancestors:") != std::string::npos);
      REQUIRE(text.find("<label unavailable>") == std::string::npos);
      REQUIRE(text.find("edm4hep::MCParticleCollection middle::TEST") != std::string::npos);
      REQUIRE(text.find("edm4hep::MCParticleCollection seed::TEST") != std::string::npos);
    }
    {
      edm::ProcessHistoryRegistry histories;
      c4h::PodioFile file(fileName, histories, false, 0, 0);
      auto registry = std::make_shared<edm::ProductRegistry>(*file.productRegistry());
      std::vector<std::string> processOrder;
      edm::processingOrderMerge(histories, processOrder);
      REQUIRE(processOrder == std::vector<std::string>{"TEST"});
      for (auto const& entry : registry->productList()) {
        INFO(entry.second.branchName());
        REQUIRE(std::find(processOrder.begin(), processOrder.end(), entry.second.processName()) != processOrder.end());
      }
      registry->setProcessOrder(processOrder);
      registry->setFrozen();
      auto helper = std::make_shared<edm::BranchIDListHelper>();
      helper->updateFromRegistry(*registry);
      edm::ParameterSet parameters;
      parameters.registerIt();
      edm::ProcessConfiguration process("READ", parameters.id(), "test", {});
      edm::HistoryAppender appender;
      auto run = std::make_shared<edm::RunPrincipal>(registry, edm::productResolversFactory::makePrimary,
                                                    process, &appender, 0);
      REQUIRE(file.getNextItemType() == edm::InputSource::ItemType::IsRun);
      run->setAux(*file.readRunAuxiliary_());
      file.readRun_(*run, histories);
      auto lumi = std::make_shared<edm::LuminosityBlockPrincipal>(registry, edm::productResolversFactory::makePrimary,
                                                                process, &appender, 0);
      REQUIRE(file.getNextItemType() == edm::InputSource::ItemType::IsLumi);
      lumi->setAux(*file.readLuminosityBlockAuxiliary_());
      lumi->setRunPrincipal(run);
      file.readLuminosityBlock_(*lumi, histories);
      edm::EventPrincipal principal(registry, edm::productResolversFactory::makePrimary, helper, process, &appender);
      principal.setLuminosityBlockPrincipal(lumi.get());
      for (auto const& snapshot : snapshots) {
        REQUIRE(file.getNextItemType() == edm::InputSource::ItemType::IsEvent);
        file.readEvent_(principal, histories);
        auto handle = principal.getByLabel(edm::PRODUCT_TYPE, edm::TypeID(typeid(edm4hep::MCParticleCollection)),
                                          "leaf", "", "TEST", nullptr, nullptr, nullptr);
        REQUIRE(handle.isValid());
        auto provenance = handle.provenance()->productProvenance();
        REQUIRE(provenance);
        REQUIRE(provenance->branchID() == leaf);
        REQUIRE(provenance->parentageID() == snapshot.records.find(edm::ProductProvenance(leaf))->parentageID());
        REQUIRE(provenance->parentage().parents() == std::vector<edm::BranchID>{middle});
        for (auto const& record : snapshot.records) {
          auto restored = principal.productProvenanceRetrieverPtr()->branchIDToProvenance(record.branchID());
          REQUIRE(restored);
          REQUIRE(*restored == record);
        }
        principal.clearEventPrincipal();
      }
      REQUIRE(file.getNextItemType() == edm::InputSource::ItemType::IsFile);
    }
    std::filesystem::remove(fileName);
  }
}
