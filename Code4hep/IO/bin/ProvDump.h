#ifndef Code4hep_IO_ProvDump_h
#define Code4hep_IO_ProvDump_h

#include <iosfwd>

namespace c4h {
  int provDump(int argc, char const* const* argv, std::ostream& out, std::ostream& err);
}

#endif
