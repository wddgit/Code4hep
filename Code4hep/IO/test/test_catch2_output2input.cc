#include <algorithm>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "DataFormats/Common/interface/Wrapper.h"
#include "DataFormats/Provenance/interface/BranchIDListHelper.h"
#include "DataFormats/Provenance/interface/HardwareResourcesDescription.h"
#include "DataFormats/Provenance/interface/Provenance.h"
#include "DataFormats/Provenance/interface/ProductResolverIndexHelper.h"
#include "DataFormats/Provenance/interface/processingOrderMerge.h"
#include "FWCore/Framework/interface/EventPrincipal.h"
#include "FWCore/Framework/interface/EventProcessor.h"
#include "FWCore/Framework/interface/ProductResolversFactory.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/ParameterSetReader/interface/ParameterSetReader.h"
#include "FWCore/TestProcessor/interface/TestProcessor.h"
#include "FWCore/Utilities/interface/Digest.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "catch2/catch_all.hpp"
#include "edm4hep/edm4hep.h"
#include "podio/FrameCategories.h"
#include "podio/Reader.h"
#include "podio/UserDataCollection.h"
#include "podio/Writer.h"

#include "../plugins/ParameterSetMetadata.h"
#include "../plugins/ParentageMetadata.h"
#include "../plugins/ProcessHistoryMetadata.h"
#include "../plugins/ProductDescriptionMetadata.h"
#include "Output2InputTestHelpers.h"
#include "ParameterSetMetadataTestHelpers.h"

using namespace c4h::test;

TEST_CASE("Tests of PodioOuput -> PodioSource", s_tag) {
  SECTION("OneEmptyEvent") {
    const std::string fileName = "one_event.root";
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
      auto const& headers = event.get<edm4hep::EventHeaderCollection>("EventHeader");
      REQUIRE(headers.size() == 1);
      REQUIRE(headers.at(0).getRunNumber() == 1);
      REQUIRE(headers.at(0).getEventNumber() == 1);
      auto historyID = c4h::readHistoryID(event, c4h::eventProcessHistoryKey, histories);
      auto history = histories.getMapped(historyID);
      REQUIRE(history != nullptr);
      REQUIRE(histories.getMapped(c4h::readHistoryID(event, c4h::runProcessHistoryKey, histories)) != nullptr);
      REQUIRE(histories.getMapped(c4h::readHistoryID(event, c4h::lumiProcessHistoryKey, histories)) != nullptr);
      for (auto const& configuration : *history) {
        REQUIRE(edm::pset::Registry::instance()->getMapped(configuration.parameterSetID()) != nullptr);
      }
      auto const ids = c4h::readParameterSetIDs(metadata);
      auto const found = std::find(ids.begin(), ids.end(), saved.id());
      REQUIRE(found != ids.end());
      auto const index = static_cast<std::size_t>(std::distance(ids.begin(), found));
      auto const& ints = metadata.getParameters().getMap<int>();
      auto const& lengths = ints.at(c4h::parameterSetLengthsKey);
      auto const& words = ints.at(c4h::parameterSetBlobsKey);
      REQUIRE(lengths[index] == static_cast<int>(saved.toString().size()));
      std::size_t offset = 0;
      for (std::size_t i = 0; i != index; ++i)
        offset += static_cast<std::size_t>(lengths[i]) / 4 + (lengths[i] % 4 != 0);
      auto const expected = c4h::test::packedWords({saved.toString()});
      REQUIRE(std::equal(expected.begin(), expected.end(), words.begin() + offset));
      REQUIRE_NOTHROW(c4h::restoreParameterSets(metadata));
      REQUIRE(edm::getParameterSet(saved.id()).toString() == saved.toString());
    }
    {
      auto config = setInputFile(baseSourceConfig, fileName);
      SourceTester tester(config);

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
      }
      {
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
        auto metadata = c4h::test::metadataFrame({id.compactForm()}, {blob});
        writer.writeFrame(metadata, podio::Category::Metadata);
      }
      writer.finish();
    }
    {
      SourceTester tester(setInputFile(baseSourceConfig, fileName));
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

  SECTION("SourceRequiresUniformInputModeAcrossFiles") {
    auto const firstHasProvenance = GENERATE(false, true);
    auto const secondHasProvenance = GENERATE(false, true);
    std::vector<std::string> const fileNames{"uniform_input_first.root", "uniform_input_second.root"};
    edm::ParameterSet parameters;
    parameters.registerIt();
    edm::ProcessHistory history;
    history.push_back(edm::ProcessConfiguration("PODIO", parameters.id(), "", {}));
    history.setProcessHistoryID();
    edm::ProductDescription particles(edm::InEvent, "particles", "PODIO", "",
                                     edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    edm::ProductDescription headers(edm::InEvent, "EventHeader", "PODIO", "",
                                   edm::TypeID(typeid(edm4hep::EventHeaderCollection)), false);
    std::map<edm::BranchID, edm::ProductDescription> const descriptions{
        {particles.branchID(), particles}, {headers.branchID(), headers}};
    std::map<edm::BranchID, std::string> const names{
        {particles.branchID(), "particles"}, {headers.branchID(), "EventHeader"}};
    for (unsigned int fileIndex : {0U, 1U}) {
      bool const hasProvenance = fileIndex == 0 ? firstHasProvenance : secondHasProvenance;
      auto writer = podio::makeWriter(fileNames[fileIndex]);
      {
        podio::Frame event;
        edm4hep::EventHeaderCollection eventHeaders;
        auto header = eventHeaders.create();
        header.setRunNumber(fileIndex + 1);
        header.setEventNumber(fileIndex + 1);
        event.put(std::move(eventHeaders), "EventHeader");
        edm4hep::MCParticleCollection eventParticles;
        eventParticles.create().setPDG(11 + fileIndex);
        event.put(std::move(eventParticles), "particles");
        if (hasProvenance) {
          event.putParameter(c4h::eventProcessHistoryKey, c4h::historyIDString(history.id()));
          event.putParameter(c4h::runProcessHistoryKey, c4h::historyIDString(history.id()));
          event.putParameter(c4h::lumiProcessHistoryKey, c4h::historyIDString(history.id()));
          event.putParameter(c4h::luminosityBlockKey, "1");
          c4h::saveEventParentage(event, c4h::EventParentage{}, {});
        }
        writer.writeEvent(event);
      }
      if (hasProvenance) {
        auto metadata = c4h::saveParameterSets();
        c4h::saveProductDescriptions(metadata, descriptions, names);
        c4h::saveProcessHistories(metadata, {{history.id(), history}});
        c4h::ParentageDictionary{}.save(metadata);
        writer.writeFrame(metadata, podio::Category::Metadata);
      } else if (fileIndex == 1) {
        // Unrelated metadata must not classify an ordinary file as Code4hep provenance input.
        podio::Frame metadata;
        metadata.putParameter("unrelated", "ordinary podio metadata");
        writer.writeFrame(metadata, podio::Category::Metadata);
      }
      writer.finish();
    }
    {
      auto config = baseSourceConfig + "\nprocess.source.fileNames = ['file:" + fileNames[0] +
                    "', 'file:" + fileNames[1] + "']\n";
      SourceTester tester(config);
      REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
      for (unsigned int fileIndex : {0U, 1U}) {
        if (fileIndex == 1 && firstHasProvenance != secondHasProvenance) {
          try {
            tester.openFile();
            FAIL("Mixed provenance and ordinary podio input was accepted");
          } catch (edm::Exception const& ex) {
            REQUIRE(ex.categoryCode() == edm::errors::MismatchedInputFiles);
            std::string const message = ex.what();
            REQUIRE(message.find("First input file 'file:" + fileNames[0] + "' uses " +
                                 (firstHasProvenance ? "Code4hep with provenance metadata" : "ordinary podio")) !=
                    std::string::npos);
            REQUIRE(message.find("current input file 'file:" + fileNames[1] + "' uses " +
                                 (secondHasProvenance ? "Code4hep with provenance metadata" : "ordinary podio")) !=
                    std::string::npos);
            REQUIRE(message.find("Do not mix Code4hep files with provenance metadata and ordinary podio files "
                                 "in the same job") !=
                    std::string::npos);
          }
          break;
        }
        auto file = tester.openFile();
        REQUIRE(bool(file));
        REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
        REQUIRE(tester.readRun().run() == fileIndex + 1);
        REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
        REQUIRE(tester.readLuminosityBlock().luminosityBlock() == 1);
        REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
        if (fileIndex == 1) {
          auto event = tester.readEvent();
          REQUIRE(event.run() == fileIndex + 1);
          REQUIRE(event.event() == fileIndex + 1);
          REQUIRE(event.get<edm4hep::MCParticleCollection>("particles", "", "PODIO")->at(0).getPDG() == 11 + fileIndex);
          REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsStop);
          tester.closeFile(file);
          break;
        }
        auto event = tester.readEvent();
        REQUIRE(event.run() == fileIndex + 1);
        REQUIRE(event.event() == fileIndex + 1);
        REQUIRE(event.get<edm4hep::MCParticleCollection>("particles", "", "PODIO")->at(0).getPDG() == 11 + fileIndex);
        REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
        tester.closeFile(file);
      }
    }
    for (auto const& fileName : fileNames)
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
    completeTestProcessorHistory(fileName, setOutputFile(baseOutConfig, fileName));
    {
      auto config = setInputFile(baseSourceConfig, fileName);
      SourceTester tester(config);

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

  SECTION("OutputReusesInputEventHeaderIndependentlyOfSelection") {
    auto const selectedHeader = GENERATE(false, true);
    // Ordinary input, imported metadata, metadata-exempt physical header,
    // or a dropped correct-type input description without an available product.
    auto const inputMode = GENERATE(0, 1, 2, 3);
    bool const inputHasMetadata = inputMode != 0;
    bool const reusableHeader = inputMode < 2;
    std::string const inputName = "input_header.root";
    std::string const outputName = "reused_header.root";
    edm::ParameterSet parameters;
    parameters.registerIt();
    edm::ProcessHistory history;
    history.push_back(edm::ProcessConfiguration("PODIO", parameters.id(), "", {}));
    history.setProcessHistoryID();
    edm::ProductDescription description(edm::InEvent, "EventHeader", "PODIO", "",
                                        edm::TypeID(typeid(edm4hep::EventHeaderCollection)), false);
    {
      auto writer = podio::makeWriter(inputName);
      podio::Frame frame;
      edm4hep::EventHeaderCollection headers;
      for (int index : {0, 1}) {
        auto header = headers.create();
        header.setRunNumber(43 + index);
        header.setEventNumber(42 + index);
        header.setTimeStamp(123456 + index);
        header.setWeight(2.5f + index);
      }
      frame.put(std::move(headers), "EventHeader");
      if (inputHasMetadata) {
        frame.putParameter(c4h::eventProcessHistoryKey, c4h::historyIDString(history.id()));
        frame.putParameter(c4h::runProcessHistoryKey, c4h::historyIDString(history.id()));
        frame.putParameter(c4h::lumiProcessHistoryKey, c4h::historyIDString(history.id()));
        frame.putParameter(c4h::luminosityBlockKey, "1");
        c4h::saveEventParentage(frame, c4h::EventParentage{}, {});
      }
      writer.writeEvent(frame);
      if (inputHasMetadata) {
        auto metadata = c4h::saveParameterSets();
        std::map<edm::BranchID, edm::ProductDescription> descriptions;
        std::map<edm::BranchID, std::string> names;
        if (reusableHeader) {
          descriptions.emplace(description.branchID(), description);
          names.emplace(description.branchID(), "EventHeader");
        } else if (inputMode == 3) {
          descriptions.emplace(description.branchID(), description);
        }
        c4h::saveProductDescriptions(metadata, descriptions, names);
        c4h::saveProcessHistories(metadata, {{history.id(), history}});
        c4h::ParentageDictionary{}.save(metadata);
        writer.writeFrame(metadata, podio::Category::Metadata);
      }
      writer.finish();
    }
    {
      // Initialize the framework through the test harness and verify that the
      // source applies offsets only to EDM identifiers, not to the input payload.
      auto sourceConfig = setInputFile(baseSourceConfig, inputName) +
                          "\nprocess.source.runOffset = cms.untracked.uint32(100)\n"
                          "process.source.eventOffset = cms.untracked.uint32(200)\n";
      SourceTester source(sourceConfig);
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
      source.openFile();
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
      REQUIRE(source.readRun().run() == 143);
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
      source.readLuminosityBlock();
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
      REQUIRE(source.readEvent().event() == 242);
    }
    auto config = std::string{R"_(import FWCore.ParameterSet.Config as cms
process = cms.Process('REWRITE')
process.source = cms.Source('PodioSource', fileNames = cms.untracked.vstring('file:input_header.root'),
                            runOffset = cms.untracked.uint32(100), eventOffset = cms.untracked.uint32(200))
process.maxEvents = cms.untracked.PSet(input = cms.untracked.int32(1))
process.InitRootHandlers = cms.Service('InitRootHandlers')
process.JobReportService = cms.Service('JobReportService')
process.out = cms.OutputModule('PodioOutputModule', fileName = cms.untracked.string('reused_header.root'))
process.end = cms.EndPath(process.out)
)_"};
    if (!selectedHeader)
      config += "\nprocess.out.outputCommands = cms.untracked.vstring('drop *')\n";
    {
      edm::EventProcessor processor(edm::getPSetFromConfig(config));
      processor.beginJob();
      REQUIRE(processor.run() == edm::EventProcessor::epSuccess);
      processor.endJob();
    }
    {
      auto reader = podio::makeReader(outputName);
      REQUIRE(reader.getEntries("events") == 1);
      auto frame = reader.readEvent(0);
      auto const names = frame.getAvailableCollections();
      REQUIRE(std::count(names.begin(), names.end(), "EventHeader") == 1);
      auto const& headers = frame.get<edm4hep::EventHeaderCollection>("EventHeader");
      REQUIRE(headers.size() == (reusableHeader ? 2 : 1));
      for (unsigned index = 0; index < headers.size(); ++index) {
        REQUIRE(headers[index].getRunNumber() == 43 + index + (reusableHeader ? 0 : 100));
        REQUIRE(headers[index].getEventNumber() == 42 + index + (reusableHeader ? 0 : 200));
        REQUIRE(headers[index].getTimeStamp() == 123456 + index);
        if (reusableHeader)
          REQUIRE(headers[index].getWeight() == 2.5f + index);
      }
      std::map<edm::BranchID, std::string> storedNames;
      auto metadata = reader.readFrame(podio::Category::Metadata, 0);
      REQUIRE(c4h::restoreProductDescriptions(metadata, storedNames));
      REQUIRE(storedNames.size() == (selectedHeader && reusableHeader ? 1 : 0));
      if (selectedHeader && reusableHeader)
        REQUIRE(storedNames.at(description.branchID()) == "EventHeader");
      auto dictionary = c4h::ParentageDictionary::restore(metadata);
      REQUIRE(dictionary);
      // The input file had no provenance records, so only the stored header can have one.
      auto parentage = c4h::restoreEventParentage(frame, *dictionary);
      for (auto const& record : parentage.records)
        REQUIRE(record.branchID() == description.branchID());
    }
    {
      SourceTester source(setInputFile(baseSourceConfig, outputName));
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
      source.openFile();
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
      REQUIRE(source.readRun().run() == (reusableHeader ? 43 : 143));
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsLumi);
      source.readLuminosityBlock();
      REQUIRE(source.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
      REQUIRE(source.readEvent().event() == (reusableHeader ? 42 : 242));
    }
    std::filesystem::remove(inputName);
    std::filesystem::remove(outputName);
  }

  SECTION("OrdinaryInputRejectsUnregisteredCollections") {
    auto const ignoreMissing = GENERATE(false, true);
    std::string const fileName = "unregistered_input_collection.root";
    {
      auto writer = podio::makeWriter(fileName);
      podio::Frame event;
      edm4hep::EventHeaderCollection headers;
      auto header = headers.create();
      header.setRunNumber(1);
      header.setEventNumber(1);
      event.put(std::move(headers), "EventHeader");
      // podio supports double user data, but Code4hep registers only float and int32_t.
      podio::UserDataCollection<double> extra(std::vector<double>{42.});
      event.put(std::move(extra), "unregistered");
      writer.writeEvent(event);
      writer.finish();
    }
    auto config = setInputFile(baseSourceConfig, fileName);
    // This option applies to null first-event collections, not missing converters.
    config += std::string("\nprocess.source.ignoreMissingOnFirstEvent = cms.untracked.bool(") +
              (ignoreMissing ? "True" : "False") + ")\n";
    try {
      SourceTester tester(config);
      FAIL("Unregistered input collection was accepted without an event owner");
    } catch (edm::Exception const& ex) {
      REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
      std::string const message = ex.what();
      REQUIRE(message.find("unregistered") != std::string::npos);
      REQUIRE(message.find(fileName) != std::string::npos);
      REQUIRE(message.find("no ProductDescription") != std::string::npos);
      REQUIRE(message.find("frame-owned") != std::string::npos);
      REQUIRE(message.find("remove it from the file") != std::string::npos);
    }
    std::filesystem::remove(fileName);
  }

  SECTION("ProductIdentityAndLuminosityBlockTransitions") {
    // Normal input, omitted target, metadata-only target with data, dropped metadata without data,
    // and uniquely named products from two processes. Same-label process lookup
    // is tested in memory below; it cannot be represented by two podio payloads.
    auto const inventory = GENERATE(0, 1, 2, 3, 4);
    bool const hiddenTarget = inventory == 1 || inventory == 2;
    bool const hasLaterProduct = inventory == 4;
    std::vector<int> const lumis = hasLaterProduct ? std::vector<int>{7} : std::vector<int>{3, 7};
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
    edm::ProductDescription second(edm::InEvent, "otherproducerparticles", "ORIGINAL", "",
                                    edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    edm::ProductDescription dropped(edm::InEvent, "hiddentargets", "ORIGINAL", "",
                                     edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    edm::ProductDescription later(edm::InEvent, "laterproducer", "LATER", "particles",
                                   edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    std::map<edm::BranchID, edm::ProductDescription> descriptions{
        {first.branchID(), first}, {second.branchID(), second}};
    std::map<edm::BranchID, std::string> names{
        {first.branchID(), "producerparticles"}, {second.branchID(), "otherproducerparticles"}};
    if (inventory == 2 || inventory == 3)
      descriptions.emplace(dropped.branchID(), dropped);
    if (hasLaterProduct) {
      descriptions.emplace(later.branchID(), later);
      names.emplace(later.branchID(), "laterproducerparticles");
    }
    {
      auto writer = podio::makeWriter(fileName);
      for (int lumi : lumis) {
        podio::Frame event;
        auto headers = std::make_unique<edm4hep::EventHeaderCollection>();
        auto header = headers->create();
        header.setRunNumber(1);
        header.setEventNumber(lumi);
        event.put(std::move(headers), "EventHeader");
        edm4hep::MCParticleCollection firstParticles, secondParticles;
        auto parent = firstParticles.create();
        parent.setPDG(11);
        auto child = secondParticles.create();
        child.setPDG(13);
        child.addToParents(parent);
        if (hiddenTarget) {
          edm4hep::MCParticleCollection targets;
          auto target = targets.create();
          target.setPDG(22);
          child.addToParents(target);
          event.put(std::move(targets), "hiddentargets");
        }
        event.put(std::move(firstParticles), names.at(first.branchID()));
        event.put(std::move(secondParticles), names.at(second.branchID()));
        if (hasLaterProduct) {
          edm4hep::MCParticleCollection laterParticles;
          laterParticles.create().setPDG(22);
          event.put(std::move(laterParticles), names.at(later.branchID()));
        }
        auto const& eventHistory = lumi == 3 ? history : laterHistory;
        event.putParameter(c4h::eventProcessHistoryKey, c4h::historyIDString(eventHistory.id()));
        event.putParameter(c4h::runProcessHistoryKey, c4h::historyIDString(history.id()));
        event.putParameter(c4h::lumiProcessHistoryKey, c4h::historyIDString(history.id()));
        event.putParameter(c4h::luminosityBlockKey, std::to_string(lumi));
        c4h::saveEventParentage(event, c4h::EventParentage{}, {});
        writer.writeEvent(event);
      }
      auto metadata = c4h::saveParameterSets();
      c4h::saveProductDescriptions(metadata, descriptions, names);
      c4h::saveProcessHistories(metadata, {{history.id(), history}, {laterHistory.id(), laterHistory}});
      c4h::ParentageDictionary{}.save(metadata);
      writer.writeFrame(metadata, podio::Category::Metadata);
      writer.finish();
    }
    if (hiddenTarget) {
      try {
        SourceTester tester(setInputFile(baseSourceConfig, fileName));
        FAIL("Hidden relation target was accepted without an event owner");
      } catch (edm::Exception const& ex) {
        REQUIRE(ex.categoryCode() == edm::errors::FormatIncompatibility);
        std::string const message = ex.what();
        REQUIRE(message.find("hiddentargets") != std::string::npos);
        REQUIRE(message.find(fileName) != std::string::npos);
        REQUIRE(message.find("no non-dropped event ProductDescription") != std::string::npos);
        REQUIRE(message.find("hidden relation targets") != std::string::npos);
        REQUIRE(message.find("Include a ProductDescription") != std::string::npos);
        REQUIRE(message.find("remove it from the file") != std::string::npos);
      }
      std::filesystem::remove(fileName);
      return;
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
      REQUIRE_FALSE(restoredDescriptions.at(first.branchID()).dropped());
      REQUIRE_FALSE(restoredDescriptions.at(first.branchID()).aliasForBranchID().isValid());
      if (hasLaterProduct) {
        auto const& description = restoredDescriptions.at(later.branchID());
        REQUIRE(description.moduleLabel() == "laterproducer");
        REQUIRE(description.moduleLabel() != first.moduleLabel());
        REQUIRE(description.productInstanceName() == first.productInstanceName());
        REQUIRE(description.fullClassName() == first.fullClassName());
        REQUIRE(description.processName() == "LATER");
        REQUIRE_FALSE(description.dropped());
      }
      if (inventory == 3)
        REQUIRE(restoredDescriptions.at(dropped.branchID()).dropped());
      edm::ProcessHistoryRegistry histories;
      REQUIRE(c4h::restoreProcessHistories(metadata, histories));
      auto restoredHistory = histories.getMapped(laterHistory.id());
      REQUIRE(restoredHistory != nullptr);
      REQUIRE(restoredHistory->size() == 2);
      REQUIRE((*restoredHistory)[0].processName() == "ORIGINAL");
      REQUIRE((*restoredHistory)[1].processName() == "LATER");
      auto dictionary = c4h::ParentageDictionary::restore(metadata);
      REQUIRE(dictionary);
      for (unsigned int index = 0; index < lumis.size(); ++index) {
        auto frame = reader.readEvent(index);
        REQUIRE(c4h::readHistoryID(frame, c4h::eventProcessHistoryKey, histories) ==
                (lumis[index] == 3 ? history.id() : laterHistory.id()));
        auto parentage = c4h::restoreEventParentage(frame, *dictionary);
        REQUIRE(parentage.records.empty());
      }
      podio::Frame rewritten;
      REQUIRE_NOTHROW(c4h::saveProductDescriptions(rewritten, restoredDescriptions, restoredNames));
    }
    {
      SourceTester tester(setInputFile(baseSourceConfig, fileName));
      REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsFile);
      tester.openFile();
      REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsRun);
      tester.readRun();
      auto next = tester.findNextTransition();
      for (int lumi : lumis) {
        REQUIRE(next.itemType() == edm::InputSource::ItemType::IsLumi);
        REQUIRE(tester.readLuminosityBlock().luminosityBlock() == lumi);
        REQUIRE(tester.findNextTransition().itemType() == edm::InputSource::ItemType::IsEvent);
        auto event = tester.readEvent();
        REQUIRE(event.luminosityBlock() == lumi);
        REQUIRE(event.aux().processHistoryID() == (lumi == 3 ? history.id() : laterHistory.id()));
        REQUIRE(event.get<edm4hep::MCParticleCollection>("producer", "particles", "ORIGINAL")->at(0).getPDG() == 11);
        REQUIRE(event.get<edm4hep::MCParticleCollection>("otherproducerparticles", "", "ORIGINAL")->at(0).getPDG() == 13);
        auto latest = event.get<edm4hep::MCParticleCollection>("producer", "particles", "");
        REQUIRE(latest.isValid());
        REQUIRE(latest->size() == 1);
        REQUIRE(latest->at(0).getPDG() == 11);
        if (hasLaterProduct) {
          auto explicitLater = event.get<edm4hep::MCParticleCollection>("laterproducer", "particles", "LATER");
          REQUIRE(explicitLater.isValid());
          auto implicitLater = event.get<edm4hep::MCParticleCollection>("laterproducer", "particles", "");
          REQUIRE(implicitLater.isValid());
          REQUIRE(implicitLater->at(0).getPDG() == 22);
          REQUIRE(implicitLater->at(0) == explicitLater->at(0));
        }
        // Even with LATER in the history, this label must fall back to ORIGINAL.
        auto fallback = event.get<edm4hep::MCParticleCollection>("otherproducerparticles", "", "");
        REQUIRE(fallback.isValid());
        REQUIRE(fallback->at(0).getPDG() == 13);
        auto const& child = event.get<edm4hep::MCParticleCollection>("otherproducerparticles", "", "ORIGINAL")->at(0);
        REQUIRE(fallback->at(0) == child);
        REQUIRE(child.parents_size() == 1);
        REQUIRE(child.getParents(0).getPDG() == 11);
        // Advance to the next frame while this event still owns its collections.
        next = tester.findNextTransition();
        REQUIRE(child.getParents(0).getPDG() == 11);
      }
      REQUIRE(next.itemType() == edm::InputSource::ItemType::IsStop);
    }
    std::filesystem::remove(fileName);
  }

  SECTION("SameLabelImplicitLookupSelectsNewestProcessInMemory") {
    // Framework products may share a module/instance across processes, but the
    // fixed podio name/ID policy forbids persisting both in the same Frame.
    edm::ParameterSet parameters;
    parameters.registerIt();
    edm::ProcessHistory original;
    original.emplace_back("ORIGINAL", parameters.id(), "test-release", edm::HardwareResourcesDescription{});
    original.setProcessHistoryID();
    auto later = original;
    later.emplace_back("LATER", parameters.id(), "test-release", edm::HardwareResourcesDescription{});
    later.initializeTransients();
    later.setProcessHistoryID();
    edm::ProcessHistoryRegistry histories;
    histories.registerProcessHistory(original);
    histories.registerProcessHistory(later);
    std::vector<std::string> order;
    edm::processingOrderMerge(histories, order);
    REQUIRE(order == std::vector<std::string>{"LATER", "ORIGINAL"});
    edm::ProductDescription older(edm::InEvent, "producer", "ORIGINAL", "particles",
                                 edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    edm::ProductDescription newer(edm::InEvent, "producer", "LATER", "particles",
                                 edm::TypeID(typeid(edm4hep::MCParticleCollection)), false);
    edm::ProcessConfiguration process("READ", parameters.id(), "test-release", {});
    for (auto const* history : {&original, &later}) {
      INFO("Input history ID: " << history->id() << "; newest process: " << history->rbegin()->processName());
      // Freeze the inventory of this input history, not future-process branches:
      // implicit lookup is selected from the registry's process order at freeze time.
      auto registry = std::make_shared<edm::ProductRegistry>();
      std::vector<edm::ProductDescription> descriptions{older};
      if (history == &later)
        descriptions.push_back(newer);
      auto const inputOrder = history == &later ? order : std::vector<std::string>{"ORIGINAL"};
      CAPTURE(inputOrder);
      registry->updateFromInput(descriptions, inputOrder);
      registry->setFrozen();
      auto helper = std::make_shared<edm::BranchIDListHelper>();
      helper->updateFromRegistry(*registry);
      edm::EventPrincipal principal(registry, edm::productResolversFactory::makePrimary, helper, process, nullptr);
      principal.clearEventPrincipal();
      edm::EventAuxiliary aux(edm::EventID(1, 1, 1), "", edm::Timestamp(), false);
      aux.setProcessHistoryID(history->id());
      principal.fillEventPrincipal(aux, history, nullptr);
      REQUIRE(principal.processHistoryID() == history->id());
      for (auto const& description : descriptions) {
        auto particles = std::make_unique<edm4hep::MCParticleCollection>();
        particles->create().setPDG(description.branchID() == older.branchID() ? 11 : 22);
        // This fixture tests process selection, not per-event parentage insertion.
        principal.putOnRead(description, std::make_unique<edm::Wrapper<edm4hep::MCParticleCollection>>(
                                              std::move(particles)), {});
      }
      auto check = [&](std::string const& processName, edm::BranchID expected, int pdg) {
        CAPTURE(processName, expected, pdg);
        INFO("Older branch: " << older.branchName() << "; ID: " << older.branchID()
             << "; newer branch: " << newer.branchName() << "; ID: " << newer.branchID());
        auto const index = registry->productLookup(edm::InEvent)->index(
            edm::PRODUCT_TYPE, edm::TypeID(typeid(edm4hep::MCParticleCollection)),
            "producer", "particles", processName.c_str());
        CAPTURE(index);
        REQUIRE(index != edm::ProductResolverIndexInvalid);
        REQUIRE(index == registry->indexFrom(expected));
        auto handle = principal.getByLabel(edm::PRODUCT_TYPE, edm::TypeID(typeid(edm4hep::MCParticleCollection)),
                                           "producer", "particles", processName, nullptr, nullptr, nullptr);
        if (handle.failedToGet()) {
          INFO(handle.whyFailed()->what());
          REQUIRE(handle.isValid());
        }
        REQUIRE(handle.isValid());
        REQUIRE(handle.provenance()->branchID() == expected);
        auto const* wrapper = dynamic_cast<edm::Wrapper<edm4hep::MCParticleCollection> const*>(handle.wrapper());
        REQUIRE(wrapper != nullptr);
        REQUIRE(wrapper->product()->size() == 1);
        REQUIRE(wrapper->product()->at(0).getPDG() == pdg);
      };
      check("ORIGINAL", older.branchID(), 11);
      if (history == &later)
        check("LATER", newer.branchID(), 22);
      check("", history == &later ? newer.branchID() : older.branchID(), history == &later ? 22 : 11);
    }
  }
}
