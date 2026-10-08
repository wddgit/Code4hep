#include "FWCore/TestProcessor/interface/TestProcessor.h"
#include "FWCore/TestProcessor/interface/TestSourceProcessor.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/Digest.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "../plugins/ParameterSetMetadata.h"
#include "../plugins/ProcessHistoryMetadata.h"
#include "../plugins/ProductDescriptionMetadata.h"
#include "podio/Reader.h"
#include "podio/Writer.h"
#include "podio/FrameCategories.h"

#include "edm4hep/edm4hep.h"
#include <algorithm>
#include <iterator>
#include <vector>
#include <filesystem>
#include "catch2/catch_all.hpp"

static constexpr auto s_tag = "[PodioOutputSource]";

namespace {
  std::string setOutputFile(std::string const& iConfig, std::string const& iFileName) {
    using namespace std::string_literals;
    return iConfig + "\nprocess.out.fileName = '"s + iFileName + "'\n";
  }

  std::string setInputFile(std::string const& iConfig, std::string const& iFileName) {
    using namespace std::string_literals;
    return iConfig + "\nprocess.source.fileNames = ['file:"s + iFileName + "']\n";
  }
}  // namespace
TEST_CASE("Tests of PodioOuput -> PodioSource", s_tag) {
  const std::string baseOutConfig{
      R"_(from FWCore.TestProcessor.TestProcess import *
process = TestProcess()
process.out = cms.OutputModule('PodioOutputModule',
                      fileName = cms.untracked.string('')
)
process.add_(cms.Service("InitRootHandlers"))
process.add_(cms.Service("JobReportService"))

process.moduleToTest(process.out)
)_"};

  const std::string baseSourceConfig{
      R"_(from FWCore.TestProcessor.TestSourceProcess import *
process = TestSourceProcess("READ")
process.source = cms.Source("PodioSource", fileNames = cms.untracked.vstring(''))
process.add_(cms.Service("InitRootHandlers"))
process.add_(cms.Service("SiteLocalConfigService"))
process.add_(cms.Service("JobReportService"))
    )_"};

  SECTION("OneEmptyEvent") {
    const std::string fileName = "one_event.root";
    bool emptyHistory = false;
    edm::ParameterSet saved;
    saved.addParameter<std::string>("podioPersistenceTest", "output registry snapshot");
    saved.registerIt();
    {
      auto configString = setOutputFile(baseOutConfig, fileName);

      edm::test::TestProcessor::Config config{configString};

      edm::test::TestProcessor tester(config);
      tester.test();
    }
    {
      auto reader = podio::makeReader(fileName);
      REQUIRE(reader.getEntries(podio::Category::Metadata) == 1);
      auto metadata = reader.readFrame(podio::Category::Metadata, 0);
      edm::ProcessHistoryRegistry histories;
      REQUIRE(c4h::restoreProcessHistories(metadata, histories));
      auto event = reader.readEvent(0);
      auto historyID = c4h::readHistoryID(event, c4h::eventProcessHistoryKey, histories);
      auto history = histories.getMapped(historyID);
      REQUIRE(history != nullptr);
      emptyHistory = history->empty() ||
                     histories.getMapped(c4h::readHistoryID(event, c4h::runProcessHistoryKey, histories))->empty() ||
                     histories.getMapped(c4h::readHistoryID(event, c4h::lumiProcessHistoryKey, histories))->empty();
      for (auto const& configuration : *history) {
        REQUIRE(edm::pset::Registry::instance()->getMapped(configuration.parameterSetID()) != nullptr);
      }
      auto ids = metadata.getParameter<std::vector<std::string>>(c4h::parameterSetIDsKey);
      auto blobs = metadata.getParameter<std::vector<std::string>>(c4h::parameterSetBlobsKey);
      REQUIRE(ids);
      REQUIRE(blobs);
      REQUIRE(ids->size() == blobs->size());
      std::string id;
      saved.id().toString(id);
      auto found = std::find(ids->begin(), ids->end(), id);
      REQUIRE(found != ids->end());
      REQUIRE((*blobs)[std::distance(ids->begin(), found)] == saved.toString());
    }
    {
      auto config = setInputFile(baseSourceConfig, fileName);
      edm::test::TestSourceProcessor tester(config);

      {
        auto n = tester.findNextTransition();
        REQUIRE(n == edm::InputSource::ItemType::IsFile);
        auto f = tester.openFile();
        REQUIRE(bool(f));
      }
      {
        if (emptyHistory) {
          REQUIRE_THROWS_AS(tester.findNextTransition(), edm::Exception);
        } else {
          auto n = tester.findNextTransition();
          REQUIRE(n.itemType() == edm::InputSource::ItemType::IsRun);
          auto r = tester.readRun();
          REQUIRE(r.run() == 1);
        }
      }
      if (!emptyHistory) {
        auto n = tester.findNextTransition();
        REQUIRE(n.itemType() == edm::InputSource::ItemType::IsLumi);
        auto r = tester.readLuminosityBlock();
        REQUIRE(r.run() == 1);
        REQUIRE(r.luminosityBlock() == 1);
      }
      if (!emptyHistory) {
        auto n = tester.findNextTransition();
        REQUIRE(n.itemType() == edm::InputSource::ItemType::IsEvent);
        auto r = tester.readEvent();
        REQUIRE(r.run() == 1);
        REQUIRE(r.luminosityBlock() == 1);
        REQUIRE(r.event() == 1);
      }
      if (!emptyHistory) {
        auto n = tester.findNextTransition();
        REQUIRE(n.itemType() == edm::InputSource::ItemType::IsStop);
      }
    }
    std::filesystem::remove(fileName);
  }

  SECTION("SourceReadsFilesWithAndWithoutParameterSetMetadata") {
    auto const withMetadata = GENERATE(false, true);
    const std::string fileName = "source_parameter_sets.root";
    edm::ParameterSet saved;
    saved.addParameter<std::string>("podioPersistenceTest", "source registry restoration");
    auto blob = saved.toString();
    cms::Digest digest(blob);
    auto idString = digest.digest().toString();
    edm::ParameterSetID id(idString);
    REQUIRE(edm::pset::Registry::instance()->getMapped(id) == nullptr);
    {
      auto writer = podio::makeWriter(fileName);
      podio::Frame event;
      auto headers = std::make_unique<edm4hep::EventHeaderCollection>();
      auto header = headers->create();
      header.setRunNumber(1);
      header.setEventNumber(1);
      event.put(std::move(headers), "EventHeader");
      writer.writeEvent(event);
      if (withMetadata) {
        podio::Frame metadata;
        metadata.putParameter(c4h::parameterSetVersionKey, 1);
        metadata.putParameter(c4h::parameterSetIDsKey, std::vector<std::string>{idString});
        metadata.putParameter(c4h::parameterSetBlobsKey, std::vector<std::string>{blob});
        writer.writeFrame(metadata, podio::Category::Metadata);
      }
      writer.finish();
    }
    {
      edm::test::TestSourceProcessor tester(setInputFile(baseSourceConfig, fileName));
      auto restored = edm::pset::Registry::instance()->getMapped(id);
      if (withMetadata) {
        REQUIRE(restored != nullptr);
        REQUIRE(restored->getParameter<std::string>("podioPersistenceTest") == "source registry restoration");
      } else {
        REQUIRE(restored == nullptr);
      }
    }
    std::filesystem::remove(fileName);
  }

  SECTION("EventWithMCParticleCollection") {
    const std::string fileName = "mcparticle.root";
    constexpr int kPDG = 11;
    {
      auto configString = setOutputFile(baseOutConfig, fileName);

      edm::test::TestProcessor::Config config{configString};
      auto mcparticleToken = config.produces<edm4hep::MCParticleCollection>("MCParticleCollection", "particles");

      edm::test::TestProcessor tester(config);
      auto mcparticles = std::make_unique<edm4hep::MCParticleCollection>();
      auto particle = mcparticles->create();
      particle.setPDG(kPDG);
      tester.test(std::make_pair(mcparticleToken, std::move(mcparticles)));
    }
    {
      auto config = setInputFile(baseSourceConfig, fileName);
      edm::test::TestSourceProcessor tester(config);

      {
        auto n = tester.findNextTransition();
        REQUIRE(n == edm::InputSource::ItemType::IsFile);
        auto f = tester.openFile();
        REQUIRE(bool(f));
      }
      {
        auto n = tester.findNextTransition();
        REQUIRE(n.itemType() == edm::InputSource::ItemType::IsRun);
        auto r = tester.readRun();
        REQUIRE(r.run() == 1);
      }
      {
        auto n = tester.findNextTransition();
        REQUIRE(n.itemType() == edm::InputSource::ItemType::IsLumi);
        auto r = tester.readLuminosityBlock();
        REQUIRE(r.run() == 1);
        REQUIRE(r.luminosityBlock() == 1);
      }
      {
        auto n = tester.findNextTransition();
        REQUIRE(n.itemType() == edm::InputSource::ItemType::IsEvent);
        auto r = tester.readEvent();
        REQUIRE(r.run() == 1);
        REQUIRE(r.luminosityBlock() == 1);
        REQUIRE(r.event() == 1);
        auto v = r.get<edm4hep::MCParticleCollection>("MCParticleCollection", "particles", "TEST");
        REQUIRE(v->size() == 1);
        REQUIRE((*v)[0].getPDG() == kPDG);
      }
      {
        auto n = tester.findNextTransition();
        REQUIRE(n.itemType() == edm::InputSource::ItemType::IsStop);
      }
    }
    std::filesystem::remove(fileName);
  }

  SECTION("ProductIdentityAndLuminosityBlockTransitions") {
    const std::string fileName = "product_identity.root";
    edm::ParameterSet processParameters;
    processParameters.addParameter<std::string>("provenanceTest", "original process configuration");
    processParameters.registerIt();
    edm::ProcessHistory history;
    history.push_back(edm::ProcessConfiguration("ORIGINAL", processParameters.id(), "test-release", {}));
    history.setProcessHistoryID();
    auto laterHistory = history;
    laterHistory.push_back(edm::ProcessConfiguration("LATER", processParameters.id(), "test-release", {}));
    laterHistory.initializeTransients();
    laterHistory.setProcessHistoryID();
    edm::ProductDescription first(edm::InEvent, "producer", "ORIGINAL", "particles",
                                  edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    edm::ProductDescription second(edm::InEvent, "producerparticles", "ORIGINAL", "",
                                   edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    edm::ProductDescription alias(first, "alias", "particles");
    std::map<edm::BranchID, edm::ProductDescription> descriptions{
        {first.branchID(), first}, {second.branchID(), second}, {alias.branchID(), alias}};
    // The alias target is metadata-only, so it must remain available as a
    // dropped description when the input is written to another podio file.
    std::map<edm::BranchID, std::string> names{{alias.branchID(), "particles_a"}, {second.branchID(), "particles_b"}};
    {
      auto writer = podio::makeWriter(fileName);
      for (int lumi : {3, 7}) {
        podio::Frame event;
        auto headers = std::make_unique<edm4hep::EventHeaderCollection>();
        auto header = headers->create();
        header.setRunNumber(1);
        header.setEventNumber(lumi);
        event.put(std::move(headers), "EventHeader");
        for (auto const& entry : names) {
          auto particles = std::make_unique<edm4hep::MCParticleCollection>();
          particles->create().setPDG(entry.first == alias.branchID() ? 11 : 13);
          event.put(std::move(particles), entry.second);
        }
        auto const& eventHistory = lumi == 3 ? history : laterHistory;
        event.putParameter(c4h::eventProcessHistoryKey, c4h::historyIDString(eventHistory.id()));
        event.putParameter(c4h::runProcessHistoryKey, c4h::historyIDString(history.id()));
        event.putParameter(c4h::lumiProcessHistoryKey, c4h::historyIDString(history.id()));
        event.putParameter(c4h::luminosityBlockKey, std::to_string(lumi));
        writer.writeEvent(event);
      }
      auto metadata = c4h::parameterSetMetadata();
      c4h::putProductDescriptions(metadata, descriptions, names);
      c4h::putProcessHistories(metadata, {{history.id(), history}, {laterHistory.id(), laterHistory}});
      writer.writeFrame(metadata, podio::Category::Metadata);
      writer.finish();
    }
    {
      auto reader = podio::makeReader(fileName);
      auto metadata = reader.readFrame(podio::Category::Metadata, 0);
      std::map<edm::BranchID, std::string> restoredNames;
      auto registry = c4h::restoreProductDescriptions(metadata, restoredNames);
      REQUIRE(registry);
      REQUIRE(restoredNames == names);
      std::map<edm::BranchID, edm::ProductDescription> restoredDescriptions;
      for (auto const& entry : registry->productList()) {
        restoredDescriptions.emplace(entry.second.branchID(), entry.second);
      }
      REQUIRE(restoredDescriptions.at(first.branchID()).dropped());
      REQUIRE(restoredDescriptions.at(alias.branchID()).aliasForBranchID() == first.branchID());
      podio::Frame rewritten;
      REQUIRE_NOTHROW(c4h::putProductDescriptions(rewritten, restoredDescriptions, restoredNames));
    }
    {
      edm::test::TestSourceProcessor tester(setInputFile(baseSourceConfig, fileName));
      REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
      tester.openFile();
      REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
      tester.readRun();
      for (int lumi : {3, 7}) {
        REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
        REQUIRE(tester.readLuminosityBlock().luminosityBlock() == lumi);
        REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
        auto event = tester.readEvent();
        REQUIRE(event.luminosityBlock() == lumi);
        REQUIRE(event.aux().processHistoryID() == (lumi == 3 ? history.id() : laterHistory.id()));
        REQUIRE(event.get<edm4hep::MCParticleCollection>("alias", "particles", "ORIGINAL")->at(0).getPDG() == 11);
        REQUIRE(event.get<edm4hep::MCParticleCollection>("producerparticles", "", "ORIGINAL")->at(0).getPDG() == 13);
      }
      REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsStop);
    }
    std::filesystem::remove(fileName);
  }
}
