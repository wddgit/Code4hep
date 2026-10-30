#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Output2InputTestHelpers.h"

#include "DataFormats/Provenance/interface/HardwareResourcesDescription.h"
#include "DataFormats/Provenance/interface/processingOrderMerge.h"
#include "FWCore/Common/interface/ProcessBlockHelper.h"
#include "FWCore/Framework/interface/InputSourceDescription.h"
#include "FWCore/Framework/interface/ProductResolversFactory.h"
#include "FWCore/Framework/interface/ScheduleItems.h"
#include "FWCore/Framework/interface/maker/InputSourceFactory.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescriptionFillerPluginFactory.h"
#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/ProcessDesc.h"
#include "FWCore/ParameterSet/interface/validateTopLevelParameterSets.h"
#include "FWCore/ParameterSetReader/interface/ProcessDescImpl.h"
#include "FWCore/PluginManager/interface/PluginManager.h"
#include "FWCore/PluginManager/interface/standard.h"
#include "FWCore/ServiceRegistry/interface/ServiceRegistry.h"
#include "FWCore/ServiceRegistry/interface/StreamContext.h"
#include "catch2/catch_all.hpp"
#include "podio/FrameCategories.h"
#include "podio/Reader.h"
#include "podio/Writer.h"

#include "../plugins/ParameterSetMetadata.h"
#include "../plugins/ProcessHistoryMetadata.h"
#include "../plugins/ProductDescriptionMetadata.h"

namespace c4h::test {
  std::string const baseOutConfig{
      R"_(from FWCore.TestProcessor.TestProcess import *
process = TestProcess()
process.out = cms.OutputModule('PodioOutputModule',
                      fileName = cms.untracked.string('')
)
process.add_(cms.Service("InitRootHandlers"))
process.add_(cms.Service("JobReportService"))

process.moduleToTest(process.out)
)_"};

  std::string const baseSourceConfig{
      R"_(from FWCore.TestProcessor.TestSourceProcess import *
process = TestSourceProcess("READ")
process.source = cms.Source("PodioSource", fileNames = cms.untracked.vstring(''))
process.add_(cms.Service("InitRootHandlers"))
process.add_(cms.Service("JobReportService"))
    )_"};

  SourceTester::SourceTester(std::string const& config) {
    if (!edmplugin::PluginManager::isAvailable())
      edmplugin::PluginManager::configure(edmplugin::standard::config());
    ProcessDescImpl desc(config, false);
    auto parameters = desc.parameterSet();
    edm::validateTopLevelParameterSets(parameters.get());
    edm::ScheduleItems items;
    auto token = items.initServices(desc.processDesc()->getServicesPSets(), *parameters, {},
                                   edm::serviceregistry::kOverlapIsError);
    token_ = items.addTNS(*parameters, token);
    edm::ServiceRegistry::Operate operate(token_);
    items.initMisc(*parameters);
    process_ = items.processConfiguration();
    context_.setProcessConfiguration(process_.get());
    helper_ = items.branchIDListHelper();
    auto sourceParameters = parameters->getPSetForUpdate("@main_input");
    REQUIRE(sourceParameters != nullptr);
    auto const type = sourceParameters->getParameter<std::string>("@module_type");
    std::unique_ptr<edm::ParameterSetDescriptionFillerBase> filler(
        edm::ParameterSetDescriptionFillerPluginFactory::get()->create(type));
    edm::ConfigurationDescriptions descriptions(filler->baseType(), type);
    filler->fill(descriptions);
    descriptions.validate(*sourceParameters, "source");
    sourceParameters->registerIt();
    edm::ModuleDescription module(sourceParameters->id(), type, "source", process_.get(),
                                  edm::ModuleDescription::getUniqueID());
    edm::InputSourceDescription input(module, helper_, std::make_shared<edm::ProcessBlockHelper>(),
                                     items.actReg_, -1, -1, 0, allocations_);
    source_ = edm::InputSourceFactory::get()->makeInputSource(*sourceParameters, input);
    registry_ = std::make_shared<edm::ProductRegistry>();
    std::vector<std::string> order;
    edm::processingOrderMerge(source_->processHistoryRegistry(), order);
    REQUIRE(source_->productRegistry().processOrder() == order);
    REQUIRE(std::find(order.begin(), order.end(), process_->processName()) == order.end());
    order.insert(order.begin(), process_->processName());
    registry_->updateFromInput(source_->productRegistry().productList(), order);
    for (auto const& entry : registry_->productList()) {
      INFO(entry.second.branchName());
      REQUIRE(std::find(order.begin(), order.end(), entry.second.processName()) != order.end());
    }
    registry_->setFrozen();
    helper_->updateFromRegistry(*registry_);
    event_ = std::make_unique<edm::EventPrincipal>(registry_, edm::productResolversFactory::makePrimary,
                                                 helper_, *process_, &appender_);
    source_->doBeginJob(*registry_);
  }

  SourceTester::~SourceTester() {
    edm::ServiceRegistry::Operate operate(token_);
    source_.reset();
  }

  edm::InputSource::ItemTypeInfo SourceTester::findNextTransition() {
    edm::ServiceRegistry::Operate operate(token_);
    return source_->nextItemType();
  }

  std::shared_ptr<edm::FileBlock> SourceTester::openFile() {
    edm::ServiceRegistry::Operate operate(token_);
    auto file = source_->readFile();
    REQUIRE(registry_->merge(source_->productRegistry(), file ? file->fileName() : std::string(),
                             edm::ProductDescription::FromInputToCurrent).empty());
    source_->fillProcessBlockHelper();
    return file;
  }

  void SourceTester::closeFile(std::shared_ptr<edm::FileBlock> const& file) {
    edm::ServiceRegistry::Operate operate(token_);
    source_->closeFile(file.get(), false);
  }

  edm::test::RunFromSource SourceTester::readRun() {
    edm::ServiceRegistry::Operate operate(token_);
    run_ = std::make_shared<edm::RunPrincipal>(registry_, edm::productResolversFactory::makePrimary,
                                              *process_, &appender_, 0);
    run_->setAux(*source_->runAuxiliary());
    source_->readRun(*run_, appender_);
    return {run_, token_};
  }

  edm::test::LuminosityBlockFromSource SourceTester::readLuminosityBlock() {
    edm::ServiceRegistry::Operate operate(token_);
    lumi_ = std::make_shared<edm::LuminosityBlockPrincipal>(registry_, edm::productResolversFactory::makePrimary,
                                                         *process_, &appender_, 0);
    lumi_->setAux(*source_->luminosityBlockAuxiliary());
    lumi_->setRunPrincipal(run_);
    source_->readLuminosityBlock(*lumi_, appender_);
    return {lumi_, token_};
  }

  edm::test::EventFromSource SourceTester::readEvent() {
    edm::ServiceRegistry::Operate operate(token_);
    event_->clearEventPrincipal();
    event_->setLuminosityBlockPrincipal(lumi_.get());
    edm::StreamContext stream(event_->streamID(), &context_);
    source_->readEvent(*event_, stream);
    return {*event_, token_};
  }

  std::string setOutputFile(std::string const& iConfig, std::string const& iFileName) {
    using namespace std::string_literals;
    return iConfig + "\nprocess.out.fileName = '"s + iFileName + "'\n";
  }

  std::string setInputFile(std::string const& iConfig, std::string const& iFileName) {
    using namespace std::string_literals;
    return iConfig + "\nprocess.source.fileNames = ['file:"s + iFileName + "']\n";
  }

  void completeTestProcessorHistory(std::string const& fileName, std::string const& config) {
    // TestProcessor fills events with a null input-history pointer; its synthetic
    // input history contains only extra processes. The affected test framework
    // leaves empty occurrence histories despite TEST products (normally Principal
    // appends the primary configuration when anyProductProduced() is true). Repair
    // that fixture, not the writer: retain payloads/parentage and replace all three
    // references together with a registered, explicitly synthetic configuration.
    edm::ParameterSet parameters;
    parameters.addParameter<std::string>("testProcessorConfiguration", config);
    parameters.addParameter<std::string>("historyFixture", "synthetic TestProcessor primary process");
    parameters.registerIt();
    edm::ProcessHistory synthetic;
    synthetic.emplace_back("TEST", parameters.id(), "TestProcessor-fixture", edm::HardwareResourcesDescription{});
    synthetic.setProcessHistoryID();
    std::vector<podio::Frame> events;
    podio::Frame metadata;
    {
      auto reader = podio::makeReader(fileName);
      REQUIRE(reader.getEntries(podio::Category::Metadata) == 1);
      metadata = reader.readFrame(podio::Category::Metadata, 0);
      edm::ProcessHistoryRegistry histories;
      REQUIRE(c4h::restoreProcessHistories(metadata, histories));
      c4h::restoreParameterSets(metadata);
      std::map<edm::BranchID, std::string> names;
      auto products = c4h::restoreProductDescriptions(metadata, names);
      REQUIRE(products);
      for (auto const& entry : products->productList())
        REQUIRE(entry.second.processName() == "TEST");
      auto completedHistory = synthetic;
      bool hasPrimaryHistory = false;
      for (auto const& entry : histories) {
        auto const& history = entry.second;
        if (!history.empty()) {
          REQUIRE(history.size() == 1);
          REQUIRE(history[0].processName() == "TEST");
          if (hasPrimaryHistory)
            REQUIRE(completedHistory == history);
          completedHistory = history;
          hasPrimaryHistory = true;
        }
      }
      for (unsigned int index = 0; index < reader.getEntries("events"); ++index) {
        auto event = reader.readEvent(index);
        for (auto const* key : {c4h::eventProcessHistoryKey, c4h::runProcessHistoryKey, c4h::lumiProcessHistoryKey}) {
          auto const id = c4h::readHistoryID(event, key, histories);
          if (histories.getMapped(id)->empty())
            event.putParameter(key, c4h::historyIDString(completedHistory.id()));
        }
        events.push_back(std::move(event));
      }
      c4h::saveProcessHistories(metadata, {{completedHistory.id(), completedHistory}});
      // The synthetic configuration must be resolvable just like a real one.
      auto parameterSets = c4h::saveParameterSets();
      for (auto const& [key, values] : parameterSets.getParameters().getMap<int>())
        metadata.putParameter(key, values);
      edm::ProcessHistoryRegistry restored;
      REQUIRE(c4h::restoreProcessHistories(metadata, restored));
      std::vector<std::string> order;
      edm::processingOrderMerge(restored, order);
      REQUIRE(order == std::vector<std::string>{"TEST"});
      auto const parameterSetIDs = c4h::readParameterSetIDs(metadata);
      for (auto const& event : events) {
        for (auto const* key : {c4h::eventProcessHistoryKey, c4h::runProcessHistoryKey, c4h::lumiProcessHistoryKey}) {
          auto const* history = restored.getMapped(c4h::readHistoryID(event, key, restored));
          REQUIRE(history->id() == completedHistory.id());
          REQUIRE(history->size() == 1);
          REQUIRE((*history)[0].processName() == "TEST");
          REQUIRE(std::find(parameterSetIDs.begin(), parameterSetIDs.end(), (*history)[0].parameterSetID()) !=
                  parameterSetIDs.end());
        }
      }
    }
    auto const completedFile = fileName + ".completed.root";
    {
      auto writer = podio::makeWriter(completedFile);
      for (auto const& event : events)
        writer.writeEvent(event);
      writer.writeFrame(metadata, podio::Category::Metadata);
      writer.finish();
    }
    std::filesystem::rename(completedFile, fileName);
  }
}  // namespace c4h::test
