#ifndef ASPELL__FILTER__SGML
#define ASPELL__FILTER__SGML

#include "indiv_filter.hpp"
#include "string_map.hpp"
#include "asc_ctype.hpp"

namespace aspell_filters {

using namespace acommon;

class SgmlFilter : public IndividualFilter {
public:
  virtual void process_inplace(acommon::FilterChar *, FilterChar *) = 0;
};

SgmlFilter * new_html_filter();

class ToLowerMap : public StringMap
{
public:
  PosibErr<bool> add(ParmStr to_add) {
    String new_key;
    for (const char * i = to_add; *i; ++i) new_key += asc_tolower(*i);
    return StringMap::add(new_key);
  }
  
  PosibErr<bool> remove(ParmStr to_rem) {
    String new_key;
    for (const char * i = to_rem; *i; ++i) new_key += asc_tolower(*i);
    return StringMap::remove(new_key);
  }
};

} // namespace aspell_filters

#endif
