#include <algorithm>
#include <exception>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/program_options.hpp>
#include "podio/FrameCategories.h"
#include "podio/Reader.h"

#include "DataFormats/Provenance/interface/ProcessHistoryRegistry.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/Registry.h"

#include "../plugins/ParameterSetMetadata.h"
#include "../plugins/ProcessHistoryMetadata.h"
#include "../plugins/ProductDescriptionMetadata.h"
#include "../plugins/ParentageMetadata.h"
#include "ProvDump.h"

namespace c4h {
  namespace {
    bool matches(std::string const& text, std::vector<std::string> const& filters) {
      return std::all_of(filters.begin(), filters.end(), [&](auto const& filter) {
        return text.find(filter) != std::string::npos;
      });
    }

    // Resolve nested PSets through the framework, but bound recursion and report
    // missing references locally so other stored parameters remain inspectable.
    bool printPSet(edm::ParameterSet const& pset, std::ostream& out, unsigned int indent = 4) {
      if (indent > 200)
        throw std::runtime_error("PSet nesting exceeds 100 levels (possibly cyclic metadata)");
      bool valid = true;
      std::string const padding(indent, ' ');
      out << "{\n";
      for (auto const& [name, entry] : pset.tbl())
        out << padding << "  " << name << ": " << entry << '\n';
      for (auto const& [name, entry] : pset.psetTable()) {
        out << padding << "  " << name << ": PSet " << (entry.isTracked() ? "tracked" : "untracked") << " = ";
        try {
          valid = printPSet(entry.pset(), out, indent + 2) && valid;
        } catch (std::exception const& ex) {
          out << "<unavailable: " << ex.what() << ">";
          valid = false;
        }
        out << '\n';
      }
      for (auto const& [name, entry] : pset.vpsetTable()) {
        out << padding << "  " << name << ": VPSet " << (entry.isTracked() ? "tracked" : "untracked") << " = [\n";
        try {
          for (auto const& child : entry.vpset()) {
            out << padding << "    ";
            valid = printPSet(child, out, indent + 4) && valid;
            out << '\n';
          }
        } catch (std::exception const& ex) {
          out << padding << "    <unavailable: " << ex.what() << ">\n";
          valid = false;
        }
        out << padding << "  ]\n";
      }
      out << padding << "}";
      return valid;
    }

    std::vector<std::string> labels(edm::ParameterSet const& process, std::string const& key) {
      if (!process.exists(key))
        return {};
      return process.getParameter<std::vector<std::string>>(key);
    }
  }  // namespace

  int provDump(int argc, char const* const* argv, std::ostream& out, std::ostream& err) {
    namespace po = boost::program_options;
    po::options_description options("Options");
    options.add_options()
        ("help,h", "print help")
        ("file", po::value<std::string>(), "input podio file (also positional)")
        ("showHistoryID", "show history, process configuration and ParameterSet IDs")
        ("findMatch,f", po::value<std::vector<std::string>>()->composing(),
         "print product/PSet blocks containing every specified substring (AND matching)")
        ("dontPrintProducts,p", "suppress product descriptions, not module configurations")
        ("dumpPSetID,i", po::value<std::string>(), "dump only this ParameterSet (32 hexadecimal characters)")
        ("showAllModules,a", "include modules without stored products")
        ("excludeESModules,e", "exclude EventSetup modules and sources (included by default)")
        ("showTopLevelPSets,t", "include user top-level PSets and VPSets, excluding framework components/internals")
        ("dependencies,d", "print immediate parent dependencies for stored products")
        ("extendedAncestors,x", "print all available ancestors, including unknown boundaries")
        ("extendedDescendants,c", "print all known descendants within the persisted event graph")
        ("event", po::value<unsigned int>()->default_value(0), "zero-based event index for dependencies (default 0; no aggregation)")
        ("sort,s", "sort product identities and module labels; preserve configuration order in histories");
    auto usage = [&](std::ostream& stream) {
      stream << "Usage: c4hProvDump [options] <input_file>\n" << options
             << "\nReads Code4hep file-level podio metadata, not native EDM ROOT trees.\n"
             << "No CMS catalog/site services or ProductID mapping.\n"
             << "Dependency traversal is event-local; only requested event frames are read.\n"
             << "A present default-ID provenance record means " << noRecordedParents << ".\n"
             << "Only persisted parameters are available (normally tracked parameters).\n";
    };
    po::variables_map vm;
    try {
      po::positional_options_description positional;
      positional.add("file", 1);
      po::store(po::command_line_parser(argc, argv).options(options).positional(positional).run(), vm);
      po::notify(vm);
      if (vm.count("help")) {
        usage(out);
        return 0;
      }
      if (!vm.count("file") || vm["file"].as<std::string>().empty())
        throw std::runtime_error("No input file specified");
      if (vm.count("dumpPSetID")) {
        auto const& text = vm["dumpPSetID"].as<std::string>();
        if (text.size() != 32 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
          throw std::runtime_error("dumpPSetID requires exactly 32 hexadecimal characters");
      }
    } catch (std::exception const& ex) {
      err << "Command line error: " << ex.what() << '\n';
      usage(err);
      return 1;
    }

    try {
      auto const& file = vm["file"].as<std::string>();
      auto reader = podio::makeReader(file);
      out << "Input file: " << file << "\npodio version: " << reader.currentFileVersion() << '\n';
      auto const categories = reader.getAvailableCategories();
      bool const hasMetadata = std::find(categories.begin(), categories.end(), podio::Category::Metadata) != categories.end();
      bool const requested = vm.count("dumpPSetID");
      if (!hasMetadata) {
        out << "No file-level metadata; no persisted Code4hep provenance/configurations available.\n";
        if (requested)
          err << "Requested ParameterSet is not available.\n";
        return requested ? 1 : 0;
      }
      if (reader.getEntries(podio::Category::Metadata) != 1)
        throw std::runtime_error("Expected one file-level podio metadata frame");
      auto metadata = reader.readFrame(podio::Category::Metadata, 0);
      restoreParameterSets(metadata);
      auto products = restoreStoredProductDescriptions(metadata);
      edm::ProcessHistoryRegistry histories;
      bool const hasHistories = restoreProcessHistories(metadata, histories);
      auto dictionary = ParentageDictionary::restore(metadata);
      if ((products || hasHistories) && !dictionary)
        throw std::runtime_error("Missing Code4hep parentage dictionary");
      bool const hasPSets = metadata.getParameter<std::vector<int>>(parameterSetVersionKey).has_value();
      auto const filePSetIDs = readParameterSetIDs(metadata);
      auto inFile = [&](edm::ParameterSetID const& id) {
        return std::find(filePSetIDs.begin(), filePSetIDs.end(), id) != filePSetIDs.end();
      };
      if (!products && !hasHistories && !hasPSets)
        out << "Ordinary podio metadata: no persisted Code4hep provenance/configurations available.\n";
      if (bool(products) != hasHistories)
        err << "Warning: incomplete EDM product/process provenance metadata; printing available sections.\n";

      if (requested) {
        edm::ParameterSetID const id(vm["dumpPSetID"].as<std::string>());
        // Require membership in this file, not just a pre-existing global registry entry.
        if (!inFile(id)) {
          err << "Requested ParameterSet " << id << " is not available in this file.\n";
          return 1;
        }
        auto const* pset = edm::pset::Registry::instance()->getMapped(id);
        if (!pset)
          throw std::runtime_error("Requested ParameterSet was not restored");
        out << "ParameterSet " << id << " = ";
        bool const valid = printPSet(*pset, out);
        out << '\n';
        if (!valid)
          err << "Requested ParameterSet contains unavailable nested configurations.\n";
        return valid ? 0 : 1;
      }

      std::vector<std::string> const filters = vm.count("findMatch")
          ? vm["findMatch"].as<std::vector<std::string>>() : std::vector<std::string>{};
      bool const showIDs = vm.count("showHistoryID");
      bool valid = true;
      if (vm.count("dependencies") || vm.count("extendedAncestors") || vm.count("extendedDescendants")) {
        if (!dictionary || !products) {
          out << "No persisted event parentage available.\n";
        } else {
          auto const index = vm["event"].as<unsigned int>();
          if (index >= reader.getEntries(podio::Category::Event))
            throw std::runtime_error("Requested event index is out of range");
          auto event = restoreEventParentage(reader.readFrame(podio::Category::Event, index), *dictionary);
          out << "\nDependencies for event index " << index << " (event-local, no aggregation):\n";
          auto label = [&](edm::BranchID id) {
            auto found = products->find(id);
            if (found == products->end())
              return std::string("BranchID ") + std::to_string(id.id()) + " <label unavailable>";
            auto const& f = found->second.fields;
            return std::string("BranchID ") + std::to_string(id.id()) + " " + f[4] + " " + f[1] + ':' + f[3] + ':' + f[2];
          };
          auto parents = [&](edm::BranchID id) -> edm::Parentage const* {
            auto record = event.records.find(edm::ProductProvenance(id));
            if (record == event.records.end() || !record->parentageID().isValid())
              return nullptr;
            return dictionary->find(record->parentageID());
          };
          auto printNode = [&](edm::BranchID id) {
            out << "    " << label(id);
            auto record = event.records.find(edm::ProductProvenance(id));
            if (record == event.records.end())
              out << " <unknown boundary: no provenance record>";
            else if (!record->parentageID().isValid())
              out << " <" << noRecordedParents << ">";
            else
              out << " ParentageID " << parentageIDString(record->parentageID());
            out << '\n';
          };
          std::map<edm::BranchID, std::vector<edm::BranchID>> children;
          if (vm.count("extendedDescendants"))
            for (auto const& record : event.records)
              if (auto p = parents(record.branchID()))
                for (auto parent : p->parents())
                  children[parent].push_back(record.branchID());
          // Each stored product's provenance is keyed by its original BranchID, which
          // differs from its own BranchID only for an EDAlias.
          for (auto const& [selected, product] : *products) {
            if (product.fields[7].empty())
              continue;
            auto const aliasFor = static_cast<edm::BranchID::value_type>(std::stoul(product.fields[6]));
            edm::BranchID const original = aliasFor != 0 ? edm::BranchID(aliasFor) : selected;
            std::string const rootLabel = label(selected) + " collection: " + product.fields[7];
            if (!matches(rootLabel, filters))
              continue;
            out << "  " << rootLabel << " (original BranchID " << original.id() << ")\n";
            printNode(original);
            if (vm.count("dependencies") || vm.count("extendedAncestors")) {
              out << (vm.count("extendedAncestors") ? "    Ancestors:\n" : "    Immediate parents:\n");
              std::vector<edm::BranchID> pending;
              if (auto p = parents(original))
                pending = p->parents();
              std::set<edm::BranchID> visited{original};
              for (std::size_t i = 0; i != pending.size(); ++i) {
                auto id = pending[i];
                if (!visited.insert(id).second)
                  continue;
                printNode(id);
                if (vm.count("extendedAncestors"))
                  if (auto p = parents(id))
                    pending.insert(pending.end(), p->parents().begin(), p->parents().end());
              }
            }
            if (vm.count("extendedDescendants")) {
              out << "    Descendants (known persisted graph only; unknown boundaries may hide others):\n";
              std::vector<edm::BranchID> pending = children[original];
              std::set<edm::BranchID> visited{original};
              for (std::size_t i = 0; i != pending.size(); ++i) {
                auto id = pending[i];
                if (!visited.insert(id).second)
                  continue;
                printNode(id);
                auto const& next = children[id];
                pending.insert(pending.end(), next.begin(), next.end());
              }
            }
          }
        }
      }
      if (products && !vm.count("dontPrintProducts")) {
        out << "\nProducts (stored identities, independent of ParameterSets):\n";
        std::vector<std::pair<edm::BranchID, StoredProductDescription const*>> ordered;
        for (auto const& [id, entry] : *products)
          ordered.emplace_back(id, &entry);
        if (vm.count("sort"))
          std::sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) {
            return a.second->fields < b.second->fields;
          });
        for (auto const& [id, entry] : ordered) {
          auto const& f = entry->fields;
          std::ostringstream block;
          block << "  " << f[4] << " " << f[1] << ':' << f[3] << ':' << f[2]
                << "\n    BranchID: " << id.id() << "  branch type: " << f[0]
                << "  friendly class: " << f[5] << "\n    collection: "
                << (f[7].empty() ? "<not stored>" : f[7]) << "  aliasForBranchID: " << f[6]
                << "\n    branch aliases:";
          for (auto const& alias : entry->aliases)
            block << " " << alias;
          block << '\n';
          if (matches(block.str(), filters))
            out << block.str();
        }
      } else if (!products && !vm.count("dontPrintProducts")) {
        out << "No persisted product identities available.\n";
      }

      out << "\nProcess histories (configurations oldest to newest):\n";
      if (!hasHistories)
        out << "  No persisted process histories available.\n";
      for (auto const& [id, history] : histories) {
        out << "History";
        if (showIDs)
          out << " " << id;
        out << '\n';
        if (history.empty())
          out << "  <empty>\n";
        for (auto const& config : history) {
          out << "  Process: " << config.processName() << "  release: " << config.releaseVersion()
              << "\n    Hardware: " << config.hardwareResourcesDescription() << '\n';
          if (showIDs)
            out << "    ProcessConfigurationID: " << config.id() << "  ParameterSetID: " << config.parameterSetID() << '\n';
          auto const* process = inFile(config.parameterSetID())
              ? edm::pset::Registry::instance()->getMapped(config.parameterSetID()) : nullptr;
          if (!process) {
            err << "Missing process ParameterSet " << config.parameterSetID() << " for " << config.processName()
                << "; configuration unavailable.\n";
            valid = false;
            continue;
          }
          std::vector<std::string> modules;
          if (vm.count("showAllModules")) {
            modules = labels(*process, "@all_modules");
          } else if (products) {
            for (auto const& [branchID, entry] : *products)
              if (entry.fields[2] == config.processName())
                modules.push_back(entry.fields[1]);
          }
          auto const esModules = labels(*process, "@all_esmodules");
          auto const esSources = labels(*process, "@all_essources");
          if (!vm.count("excludeESModules")) {
            modules.insert(modules.end(), esModules.begin(), esModules.end());
            modules.insert(modules.end(), esSources.begin(), esSources.end());
          }
          std::set<std::string> moduleNames;
          std::vector<std::string> selected;
          for (auto const& label : modules)
            if (moduleNames.insert(label).second)
              selected.push_back(label);
          if (vm.count("sort"))
            std::sort(selected.begin(), selected.end());
          for (auto const& label : selected) {
            std::ostringstream block;
            block << "    Module " << label << " (process " << config.processName() << ")";
            try {
              auto configLabel = label;
              if (!process->existsAs<edm::ParameterSet>(label)) {
                if (label == "source")
                  configLabel = "@main_input";
                else if (label == "TriggerResults")
                  configLabel = "@trigger_paths";
              }
              auto const& module = process->getParameterSet(configLabel);
              if (showIDs)
                block << " ParameterSetID: " << module.id();
              block << " = ";
              valid = printPSet(module, block) && valid;
              block << '\n';
            } catch (std::exception const& ex) {
              block << " <unavailable: " << ex.what() << ">\n";
              valid = false;
            }
            if (matches(block.str(), filters))
              out << block.str();
          }
          if (vm.count("showTopLevelPSets")) {
            for (auto const* key : {"@all_modules", "@all_esmodules", "@all_essources", "@all_sources",
                                    "@all_loopers", "@all_esprefers", "@all_aliases"}) {
              auto const components = labels(*process, key);
              moduleNames.insert(components.begin(), components.end());
            }
            auto isComponent = [&](std::string const& name) {
              return (!name.empty() && name.front() == '@') || moduleNames.count(name);
            };
            for (auto const& [name, entry] : process->psetTable()) {
              if (isComponent(name))
                continue;
              std::ostringstream block;
              block << "    Top-level PSet " << name << " (process " << config.processName() << ") = ";
              try {
                valid = printPSet(entry.pset(), block) && valid;
              } catch (std::exception const& ex) {
                block << "<unavailable: " << ex.what() << ">";
                valid = false;
              }
              block << '\n';
              if (matches(block.str(), filters))
                out << block.str();
            }
            for (auto const& [name, entry] : process->vpsetTable()) {
              if (isComponent(name))
                continue;
              std::ostringstream block;
              block << "    Top-level VPSet " << name << " (process " << config.processName() << ") = [\n";
              try {
                for (auto const& child : entry.vpset()) {
                  valid = printPSet(child, block) && valid;
                  block << '\n';
                }
              } catch (std::exception const& ex) {
                block << "<unavailable: " << ex.what() << ">\n";
                valid = false;
              }
              block << "    ]\n";
              if (matches(block.str(), filters))
                out << block.str();
            }
          }
        }
      }
      if (!valid)
        err << "Some configurations could not be printed (missing or unreadable PSets); output may be incomplete.\n";
      return valid ? 0 : 1;
    } catch (std::exception const& ex) {
      err << "c4hProvDump: " << ex.what() << '\n';
      return 1;
    }
  }
}  // namespace c4h
