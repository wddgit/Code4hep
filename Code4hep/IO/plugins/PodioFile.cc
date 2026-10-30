#include <cassert>
#include <algorithm>
#include <exception>
#include <iomanip>

#include "edm4hep/EventHeaderCollection.h"
#include "podio/CollectionBase.h"
#include "podio/FrameCategories.h"

#include "fillProductRegistry.h"
#include "putOnReadForAllProducts.h"
#include "PodioFile.h"
#include "ParameterSetMetadata.h"
#include "ProductDescriptionMetadata.h"
#include "ProcessHistoryMetadata.h"
#include "ParentageProvenanceReader.h"
#include <charconv>

#include "DataFormats/Provenance/interface/EventAuxiliary.h"
#include "DataFormats/Provenance/interface/EventID.h"
#include "DataFormats/Provenance/interface/HardwareResourcesDescription.h"
#include "DataFormats/Provenance/interface/LuminosityBlockAuxiliary.h"
#include "DataFormats/Provenance/interface/ProcessConfiguration.h"
#include "DataFormats/Provenance/interface/ProcessHistory.h"
#include "DataFormats/Provenance/interface/RunAuxiliary.h"

#include "FWCore/Framework/interface/EventPrincipal.h"
#include "FWCore/Framework/interface/LuminosityBlockPrincipal.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/Framework/interface/RunPrincipal.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/TimeOfDay.h"
#include "DataFormats/Provenance/interface/Timestamp.h"

namespace c4h {

  using namespace edm;

  PodioFile::PodioFile(std::string const& fileName,
                       ProcessHistoryRegistry& processHistoryRegistry,
                       bool ignoreMissingOnFirstEvent,
		       RunNumber_t runOffset,
		       EventNumber_t eventOffset) {
    fileName_ = fileName;
    runOffset_ = runOffset;
    eventOffset_ = eventOffset;
    try {
      podioReader_ = std::make_unique<podio::Reader>(podio::makeReader(fileName));
      logFileAction("  Successfully opened file ", fileName);
    } catch (std::exception const& ex) {
      // Don't print out ex.what() as its info is currently misleading
      throw Exception(errors::FileOpenError, "PodioSource")
          << "Failed to open input file \"" << fileName << "\"\nor failed to create podio::Reader\n";
    }
    numberOfEvents_ = podioReader_->getEntries("events");

    try {
      auto categories = podioReader_->getAvailableCategories();
      if (std::find(categories.begin(), categories.end(), podio::Category::Metadata) != categories.end()) {
        if (podioReader_->getEntries(podio::Category::Metadata) != 1) {
          throw Exception(errors::FormatIncompatibility) << "Expected one file-level podio metadata frame";
        }
        auto metadata = podioReader_->readFrame(podio::Category::Metadata, 0);
        restoreParameterSets(metadata);
        auto products = restoreProductDescriptions(metadata, collectionNames_);
        auto hasHistories = restoreProcessHistories(metadata, fileHistories_);
        auto parentages = ParentageDictionary::restore(metadata);
        if (bool(products) != hasHistories) {
          throw Exception(errors::FormatIncompatibility) << "Incomplete EDM product/process provenance metadata";
        }
        hasProvenanceMetadata_ = hasHistories;
        if (hasProvenanceMetadata_ && !parentages) {
          throw Exception(errors::FormatIncompatibility) << "Missing Code4hep parentage dictionary";
        }
        if (parentages) {
          parentages_ = std::move(*parentages);
          parentages_.registerParentages();
        }
        if (products) {
          std::vector<std::string> processOrder;
          processingOrderMerge(fileHistories_, processOrder);
          products->setProcessOrder(processOrder);
          productRegistry_.reset(products.release());
          for (auto const& entry : fileHistories_) {
            processHistoryRegistry.registerProcessHistory(entry.second);
          }
        }
      }
    } catch (cms::Exception& ex) {
      ex.addContext("Restoring EDM metadata from podio file '" + fileName + "'");
      throw;
    } catch (std::exception const& ex) {
      throw Exception(errors::FormatIncompatibility)
          << "Failed to read EDM metadata from podio file '" << fileName << "': " << ex.what();
    }

    std::string const processNameForInputProducts = "PODIO";

    if (!hasProvenanceMetadata_) {
      ParameterSet processParameterSet;
      processParameterSet.registerIt();
      ProcessConfiguration processConfiguration(
          processNameForInputProducts, processParameterSet.id(), "", HardwareResourcesDescription());
      ProcessHistory processHistory;
      processHistory.push_back(processConfiguration);
      processHistory.setProcessHistoryID();
      processHistoryRegistry.registerProcessHistory(processHistory);
      processHistoryID_ = processHistory.id();
      runHistoryID_ = processHistoryID_;
      lumiHistoryID_ = processHistoryID_;
    }

    // Fill the ProductRegistry with ProductDescriptions
    // from the collections available in the first event
    if (numberOfEvents_ != 0) {
      frame_ = podioReader_->readFrame("events", 0);
      eventIndexOfOpenFrame_ = 0;
    }

    if (!hasProvenanceMetadata_) {
      try {
        auto productRegistry = c4h::fillProductRegistry(frame_, processNameForInputProducts, ignoreMissingOnFirstEvent);
        productRegistry_.reset(productRegistry.release());
      } catch (cms::Exception& ex) {
        ex.addContext("Inferring EDM products from podio file '" + fileName + "'");
        throw;
      }
    }
    for (auto const& entry : productRegistry_->productList()) {
      auto const& description = entry.second;
      if (!description.dropped() && description.branchType() == edm::InEvent) {
        representedCollections_.insert(productCollectionName(description, collectionNames_));
      }
    }
    validateCollectionOwnership();
  }

  void PodioFile::validateCollectionOwnership() const {
    // Reader has no file-wide collection inventory contract. Check raw and
    // unpacked names on every frame, before getting products or moving them.
    for (auto const& name : frame_.getAvailableCollections()) {
      if (name != "EventHeader" && !representedCollections_.count(name)) {
        throw Exception(errors::FormatIncompatibility)
            << "Stored input collection '" << name << "' in podio file '" << fileName_
            << "' (event entry " << eventIndexOfOpenFrame_
            << ") has no non-dropped event ProductDescription representing its actual collection name. "
            << "PodioSource cannot support hidden relation targets that remain frame-owned: "
            << "every input data collection must be transferred to an event-owned wrapper. "
            << "Include a ProductDescription and registered collection converter for this collection, "
            << "or remove it from the file.";
      }
    }
    try {
      validateInputSubsets(frame_);
    } catch (cms::Exception& ex) {
      ex.addContext("Validating subsets in podio file '" + fileName_ + "' (event entry " +
                    std::to_string(eventIndexOfOpenFrame_) + ")");
      throw;
    }
  }

  InputSource::ItemType PodioFile::getNextItemType() {
    assert(!getNextItemTypeCalled_);
    getNextItemTypeCalled_ = true;

    if (numberOfEvents_ == 0) {
      return InputSource::ItemType::IsFile;
    }
    if (nextItemType_ == InputSource::ItemType::IsEvent || nextItemType_ == InputSource::ItemType::IsInvalid) {
      if (nextItemType_ != InputSource::ItemType::IsInvalid) {
        ++nextEventIndex_;
      }

      // Already processed the last event in the file
      if (nextEventIndex_ >= numberOfEvents_) {
        return InputSource::ItemType::IsFile;
      }

      if (nextEventIndex_ != eventIndexOfOpenFrame_) {
        frame_ = podioReader_->readFrame("events", nextEventIndex_);
        eventIndexOfOpenFrame_ = nextEventIndex_;
        validateCollectionOwnership();
      }
      auto const& eventHeaderCollection = frame_.get<edm4hep::EventHeaderCollection>("EventHeader");
      auto const& eventHeader = eventHeaderCollection.at(0);
      RunNumber_t run = eventHeader.getRunNumber() + runOffset_;
      LuminosityBlockNumber_t lumi = 1;
      auto runHistory = runHistoryID_;
      auto lumiHistory = lumiHistoryID_;
      auto reducedRunHistory = reducedRunHistoryID_;
      if (hasProvenanceMetadata_) {
        processHistoryID_ = readHistoryID(frame_, eventProcessHistoryKey, fileHistories_);
        runHistory = readHistoryID(frame_, runProcessHistoryKey, fileHistories_);
        lumiHistory = readHistoryID(frame_, lumiProcessHistoryKey, fileHistories_);
        reducedRunHistory = fileHistories_.reducedProcessHistoryID(runHistory);
        auto savedLumi = frame_.getParameter<std::vector<std::string>>(luminosityBlockKey);
        if (!savedLumi || savedLumi->size() != 1) {
          throw Exception(errors::FormatIncompatibility) << "Missing luminosity block in podio event metadata";
        }
        auto const& value = savedLumi->front();
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), lumi);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || lumi == invalidLuminosityBlockNumber) {
          throw Exception(errors::FormatIncompatibility) << "Invalid luminosity block in podio event metadata";
        }
      }
      nextEvent_ = eventHeader.getEventNumber() + eventOffset_;
      if (nextEvent_ == invalidEventNumber) {
        throw Exception(errors::LogicError, "PodioSource") << "Invalid event number in file: " << nextEvent_ << "\n";
      }
      if (run == invalidRunNumber) {
        throw Exception(errors::LogicError, "PodioSource") << "Invalid run number in file: " << run << "\n";
      }
      if (hasProvenanceMetadata_ && nextItemType_ != InputSource::ItemType::IsInvalid && run == nextRun_ &&
          reducedRunHistory == reducedRunHistoryID_) {
        if (runHistory != runHistoryID_ || (lumi == nextLumi_ && lumiHistory != lumiHistoryID_)) {
          throw Exception(errors::FormatIncompatibility)
              << "Conflicting full process history references within a podio run or luminosity block";
        }
      }
      if (nextItemType_ == InputSource::ItemType::IsInvalid || run != nextRun_ ||
          reducedRunHistory != reducedRunHistoryID_) {
        nextItemType_ = InputSource::ItemType::IsRun;
        nextRun_ = run;
        nextLumi_ = lumi;
        runHistoryID_ = runHistory;
        lumiHistoryID_ = lumiHistory;
        reducedRunHistoryID_ = reducedRunHistory;
      } else if (lumi != nextLumi_) {
        nextItemType_ = InputSource::ItemType::IsLumi;
        nextLumi_ = lumi;
        lumiHistoryID_ = lumiHistory;
      } else {
        nextItemType_ = InputSource::ItemType::IsEvent;
      }
    } else {
      if (nextItemType_ == InputSource::ItemType::IsLumi) {
        nextItemType_ = InputSource::ItemType::IsEvent;
      } else if (nextItemType_ == InputSource::ItemType::IsRun) {
        nextItemType_ = InputSource::ItemType::IsLumi;
      } else {
        // It should be impossible to get here
        // There is a bug we need to fix if we do
        assert(false);
      }
    }
    return nextItemType_;
  }

  std::shared_ptr<RunAuxiliary> PodioFile::readRunAuxiliary_() {
    auto runAuxiliary =
        std::make_shared<RunAuxiliary>(nextRun_, Timestamp::invalidTimestamp(), Timestamp::invalidTimestamp());
    runAuxiliary->setProcessHistoryID(runHistoryID_);
    return runAuxiliary;
  }

  std::shared_ptr<LuminosityBlockAuxiliary> PodioFile::readLuminosityBlockAuxiliary_() {
    auto lumiAuxiliary = std::make_shared<LuminosityBlockAuxiliary>(
        nextRun_, nextLumi_, Timestamp::invalidTimestamp(), Timestamp::invalidTimestamp());
    lumiAuxiliary->setProcessHistoryID(lumiHistoryID_);
    return lumiAuxiliary;
  }

  void PodioFile::readRun_(RunPrincipal& runPrincipal, ProcessHistoryRegistry const& processHistoryRegistry) {
    assert(getNextItemTypeCalled_);
    getNextItemTypeCalled_ = false;
    runPrincipal.fillRunPrincipal(processHistoryRegistry);
  }

  void PodioFile::readLuminosityBlock_(LuminosityBlockPrincipal& lumiPrincipal,
                                       ProcessHistoryRegistry const& processHistoryRegistry) {
    assert(getNextItemTypeCalled_);
    getNextItemTypeCalled_ = false;

    auto history = processHistoryRegistry.getMapped(lumiPrincipal.aux().processHistoryID());
    lumiPrincipal.fillLuminosityBlockPrincipal(history);
  }

  void PodioFile::readEvent_(EventPrincipal& eventPrincipal, ProcessHistoryRegistry const& processHistoryRegistry) {
    assert(getNextItemTypeCalled_);
    getNextItemTypeCalled_ = false;

    auto const& header = frame_.get<edm4hep::EventHeaderCollection>("EventHeader").at(0);
    EventAuxiliary eventAuxiliary(EventID(nextRun_, nextLumi_, nextEvent_), "", Timestamp(header.getTimeStamp()), false);
    auto history = processHistoryRegistry.getMapped(processHistoryID_);
    eventAuxiliary.setProcessHistoryID(processHistoryID_);
    if (hasProvenanceMetadata_) {
      // The stored product -> original BranchID mapping comes from the restored
      // ProductDescriptions (aliasForBranchID), so events store only provenance records.
      auto snapshot = restoreEventParentage(frame_, parentages_);
      edm::ProductProvenanceRetriever retriever(
          eventPrincipal.streamID().value(), std::make_unique<ParentageProvenanceReader>(std::move(snapshot.records)));
      eventPrincipal.fillEventPrincipal(eventAuxiliary, history, {}, {}, {}, retriever, nullptr, true);
    } else {
      eventPrincipal.fillEventPrincipal(eventAuxiliary, history);
    }

    putOnReadForAllProducts(frame_, *productRegistry_, eventPrincipal, collectionNames_);
  }

  void PodioFile::logFileAction(char const* msg, std::string const& fileName) const {
    LogAbsolute("fileAction") << std::setprecision(0) << TimeOfDay() << msg << fileName;
    FlushMessageLog();
  }

}  // namespace c4h
